# Test walker over the game's navmesh (walk078, 0.7.8)

The smoke harness walks the players with the real left stick (`BB_PAD_FILE`). The old walker nudged: pulses of
0.12-0.7 s, a pause, a re-measure, a camera angle corrected from the miss, side steps and chr warps ("HOP") against
walls. The nav walker (`gpu/shim/party/nav_walk.h`, hooked into `party_director.cpp`) plans on the game's own navmesh
and steers like a player.

Switches: `BB_TEST_WALKER=nav|old` (default nav), `BB_NAV_CACHE` (set by `tools/mp/instances.py`),
`BB_TEST_MOVE_WALK=1` (see below). `BB_TEST_WALK_SPRINT=1` still holds Circle.

## 1. Where the AI's path data is (RE, with confidence)

* The game's AI pathing is Havok AI (hkai): the exe carries the reflection names of `hkaiNavMesh`,
  `hkaiNavMeshInstance`, `hkaiPathfindingUtilFindPathInput/Output`, `hkaiNavMeshPathSearchParameters`,
  `hkaiLocalSteering`, `hkaiPathFollowingProperties` (`strings.tsv` 0x4764871-0x4767714). The functions are stripped
  and have no name or string xref in `deps\ghidra-export`; the classes are only registered through reflection tables,
  so the path search entry is not findable by a string; it would have to be traced from an AI goal (Approach /
  MoveToTarget in the Lua AI -> the NpcIns ComManipulator +0x3B8 -> hkaiBehavior) with a live debugger. (Confidence
  that the AI uses hkaiNavMesh A* + local steering: high. Confidence of any address for a callable path query: none.)
* Calling it safely from the test director is a poor trade: the query needs the live `hkaiWorld`, the
  `hkaiNavMeshInstance` of the block (instances are cut by closed gates and moving cars), an input struct with
  agent properties and a mediator, on the game thread, and its output lives in Havok pools. A wrong struct would
  crash the game, which is exactly the fragile test aid this work must not add.
* The same data is in the map files: `map/mAA_BB_CC_DD/mAA_BB_CC_DD.nvmhktbnd.dcx` is a BND4 of Havok 2014
  packfiles with one `hkaiNavMesh` per piece (faces, edges with the opposite face, vertices in map coordinates);
  `tools/party/nav_route.py` (0.7.2) already read them. That is the enemies' navmesh, so the walker's corridors are
  the AI's corridors (closed gates are in it, as the AI's static mesh has them; they are not the dynamic cutter's
  state). NVA / MCG graphs are not needed: the navmesh pieces link by their boundary edges.

**Chosen design: offline extraction + our own A* and funnel.** `tools/party/nav_cache.py GAME OUT [AREA...]` writes one
compact `nav_mAA.bin` per area (all maps `mAA_*`, so the m24_00 / m24_01 seam is linked): convex faces, vertices and
face links with their portal (the shared edge; across files the overlap point). m24 is 58 801 faces, 8 MB, loaded in
20 ms, a 250 m route plans in 4 ms. The director loads the file of the player's area (`map id >> 24`) when a walk
starts. Why not the runtime query: no safe call exists (above); the data is identical; the cache also lets the
harness answer "is this point on the mesh" (`NavMesh::FaceAt` / `Snap`). Failure modes it reports:
`start is off the navmesh` (on a lift car, in a ladder: the old walker takes over, logged), `goal is off the
navmesh` (nothing within 4 m), `no route (goal on another navmesh island)` (e.g. Cathedral Ward's bottom end): these
two end the walk at once with `approach: NAV UNREACHABLE` / `GAVE UP`.

## 2. How it walks

* Planner: A* over faces (portal midpoints), then string pulling (funnel) with the portals pulled in by 0.45 m so the
  path keeps off wall corners. X:Z-only goals take the floor nearest the player's height.
* Camera: the stick is camera-relative. The walker reads the view camera's world position (RendMan config
  `+0x50 +0x80`, the host_gfx `log` value) and takes the camera yaw as `atan2(player - camera)`; the stick angle is
  `wanted heading - camera yaw`. The observed motion (velocity direction under the commanded stick) is compared with the
  model; eight disagreeing observations in a row (>26 deg) switch to the observed offset (`cam MISMATCH` in the end line),
  and with no readable camera the offset is learned from the first 2 m/s of motion. No pulses, no trial and error.
* Pure pursuit: aims at the point 0.9-1.8 m ahead on the corridor (shorter when slow), the commanded heading turns at
  most 4.5 rad/s, full tilt except inside 4 m of the goal (down to 0.45) and at sharp corners (0.6). The stick is
  rewritten at about 25 Hz and only when it changed.
* Replan when 2.5 m off the corridor. Stall (no 0.4 m of progress in 1.5 s): 1 replan, 2 back up and strafe, 3 roll
  (Circle), 4 back up and strafe the other way, 5 a counted warp onto the path 4 m ahead (y + 1 m, because navmesh y
  can sit under the floor; logged as `route: HOP`), after 3 warps `blocked: no way past` fails the walk.
* Route steps: the waypoints are goals planned in order (one that is off the mesh is dropped as a hint; the last one
  must be reachable). Stick steps: one goal. `obj:` and `stain` goals are X:Z.
* The end lines keep their old shape (`test: approach: arrived|GAVE UP at N m`, `test: route: DONE|STOPPED ...`) plus
  `(nav: seconds, replans, back-ups, strafes, rolls, warps, stalls, cam model)`.
* `BB_TEST_MOVE_WALK=1`: a `host_move` / `guest_move` that is only a placement (3-80 m, same map, within 6 m of height,
  no `:pin`, `:native`, or yaw) is walked when the planner reaches the spot within 120 m; the step chain waits for the
  walk. Warps stay where the warp is the test (lamp travel, `host_warp`, falls, pinned fixtures).

## 3. Unit tests

`tests/test_nav_walk.cpp` (`ninja -C out/gpu nav-walk-test`): planner on synthetic meshes (straight, detour, island,
two floors, snap), the funnel keeps off a wall end, stick math and camera model (including the mismatch switch),
path following, a simulated player walks open floor at four camera yaws with a continuous stick and no warps, a
detour without a camera, X:Z hints, an unreachable goal fails at the start, a blocked player goes through the
recovery ladder (replan, back up, roll, 3 counted warps, a clear failure), replan when knocked off, turn-rate limit
and ease-off.
