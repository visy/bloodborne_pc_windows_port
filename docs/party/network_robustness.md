# Network robustness: internet play, latency, loss, outages, NAT

Party co-op runs serverless peer-to-peer. Before 2026-10-11 every live test ran over loopback at
0 ms. This page covers the network simulator, the runs under realistic profiles and what a host
must do about NAT.

## 1. Transports

| Channel | Protocol | What it carries | Reliability |
|---|---|---|---|
| Party link (`party_link.h`) | TCP to the host's party port (default 9307); or a reliable UDP stream on the party UDP port (`party_rudp.h`) when TCP does not get through | Roster, RPC (the guest game's FROM API / matchmaking), our EVENTs: travel, `flags`, `flag_snapshot`, `world_groups`, items, story, lamp requests, `lamp_lit`... | TCP (in order, retransmitted), plus a per-stream event cursor: unacked events are replayed after a reconnect and de-duplicated. Lost = no frame for 10 s; the slot is kept 60 s; the guest reconnects with backoff 1-8 s. |
| Game P2P (`net_socket.cpp`, `party_udp.h`) | UDP on the same port number | The game's own session traffic (players, enemies, damage, the join data) | The game's own protocol over UDP. Guest-to-guest pairs hole-punch, falling back to the host relay (PeerPaths). |

So our own events are reliable and ordered by construction. The open question was whether the
game's UDP session and our loopback-tuned timers survive real latency, loss and outages.

## 2. Simulator (`BB_NET_SIM`, `gpu/shim/net/netsim.h`)

`BB_NET_SIM="lat=80,jit=20,loss=2,dup=0.5,reorder=1,seed=7"` (or a preset `lan`, `wifi`, `dsl`,
`mobile`, `bad`, optionally followed by overrides). `lat` and `jit`/`jitter` are one-way ms;
`loss`, `dup` and `reorder` are percentages.

- **UDP**: a delay line in front of every datagram the game sends (`Queue`). It delays, drops,
  duplicates and reorders. A destination's datagrams stay in order unless reordered.
- **Party link (new)**: on the receiving side, every TCP chunk waits until its due time
  (`StreamDelay`): lat ± jitter, never ahead of the previous chunk. A "lost" segment costs a
  retransmission delay of max(200 ms, 3 × lat) instead of a drop. An outage holds everything
  until it ends. `last_rx` follows delivery, so a held link is silent to the timeout logic, as a
  real one would be.
- **Per direction**: each instance applies the simulator to what it sends (UDP) and receives
  (TCP). `--ienv I:BB_NET_SIM=...` gives an asymmetric link.
- **Outages**:
  - `outage=MS,every=MS` repeats an outage periodically.
  - `outage=MS,at=MS` runs one outage that long after process start.
  - The director test steps `host_outage=MS@S` / `guest_outage=MS@S` (`netsim::outage_now`) start a
    total outage on that instance at a scripted point. They need the simulator on (`lat=0` will do).
- **Loopback only**: only 127.0.0.0/8 destinations or peers are simulated, unless `any=1`. A
  stray test setting never slows a real internet session.
- **Tests**:
  - party-net-test covers parsing, the one-shot outage, loopback-only, StreamDelay (order,
    retransmits, outage hold) and outage_now.
  - party-link-test runs a full link at 60 ms ± 20 with 5 % loss and a 1.5 s outage. The guest
    joined in 0.33 s; 300 events arrived in order, none duplicated or missing; ping was 123 ms;
    the link never dropped.

## 3. Live runs (2 instances, loopback + simulator, 2026-10-11)

Scenario (`agents/overlay/netrun.sh`):
1. The guest rings the bell in the Dream.
2. The host lamp-warps to Great Bridge (B2 party travel).
3. Both move to the Cleric Beast; the guest dies (respawns at the host's lamp in the host's world).
4. The host kills the boss.
5. The flag mirror, C2 progress and C3 items run throughout.

`BB_PARTY_FLAG_DUMP=1` gives the parity check: host pool vs guest overlay.

| Profile | Bell -> client | B2 travel (host warp -> guest arrived) | Guest death -> respawned in the host's world | Host death -> party follows | Boss clear | Link | Mirror parity at the end |
|---|---|---|---|---|---|---|---|
| loopback 0 ms (earlier runs) | ~5 s | ~15 s | - | - | ok | - | 0 host-only bits |
| `lat=60,jit=10,loss=1` | 4.9 s | 14.7 s | (death step had no effect after the boss) | - | ok, guest Insight 6 -> 9 | ping ~125-145 ms, 0 drops | - |
| `lat=150,jit=30,loss=3` | 5.8 s | 18.1 s | 23.6 s from the death (8 s death hold, then 15.6 s load) | 22 s (the host died to the boss) | ok, guest Insight +3 | 0 drops | 0 host-only bits (39 = 39 groups) |
| `lat=60,jit=10,loss=1` + **5 s total outage** on both sides mid-session (in m24_01, ~30 s after B2) | 11.2 s | 8.8 s from the snapshot (load) | 33.7 s after the 8 s death hold | - | ok, after the outage | **0 drops / reconnects**, still client, cooperators 1 | 0 host-only bits (only session flag 2010, masked) |
| `...,outage=5000,at=95000` (the outage hit during the bell join) | joined (host cooperators after 27 s instead of ~8 s) | 19.1 s from the snapshot | ok | - | - | 0 drops | - |

Findings:
- Nothing broke at 150 ms / 3 % loss. Join, B2 travel and gather, deaths, boss clear, events and
  the mirror all behaved as on loopback, only slower by the round trips.
- A 5 s total outage mid-session (UDP dropped, link held) left the game session (client role,
  cooperator count) and the link intact. Afterwards the boss clear and the guest's death respawn
  ran normally; no desync (mirror parity), no crash.
- An outage during the join slows the summon (host cooperators 0 -> 1 after 27 s instead of
  ~8 s) but completes.
- **Timeouts**: the link's 10 s lost timeout and 60 s slot keep, B2's 45 s snapshot wait and
  8 s death grace, and the host watchdog (stall 60 s, gone 25 s, wait 90 s, hard 240 s) all have
  room over 150 ms RTTs and 5 s outages. Nothing needed retuning. The fast-join patches shorten
  the game's own message intervals, not its timeouts; joins at 150 ms took 5.8 s.
- Fixed on the way: the guest-lamp diagnostics printed a line every second while a guest stood
  near a world lamp. They are now silent outside tests.

### 3b. Simulated NAT, no port forward (2026-10-11, run `NAT sim: link over UDP`)

`BB_NET_SIM=lat=30,jit=5,loss=1,nat=1` on both, host has no TCP listener, the guest's code goes
to the host through `BB_PARTY_GUEST_CODE_FILE` (standing in for chat):

| Time (instance clock) | Host | Guest |
|---|---|---|
| +27.4 | | `guest code BBG1-0ZR0-000V-PPCD (UDP 127.0.0.1:47961)` |
| +28.3 | `punching towards a guest at 127.0.0.1:47961` | |
| +28.9 | `a guest connects over UDP` | `no TCP answer from the host; connecting over UDP`, `connected over UDP` (+0.07 s) |
| +29.1 | flag snapshot due | `connected: slot 1` |
| +44.5 | game P2P `punch Hunter1 ... peer heard after 500 ms` | same towards Hunter0 |
| +47.1 / +52.3 | `cooperators 0 -> 1` | `client (6)` (bell join) |
| +74.4 | | B2 travel: `arrived in the host world` |

So the link (events, RPC, mirror) and the game's own session both crossed the simulated NAT;
without the guest code the guest never connects (`test_udp_nat_link`, Bob).

Not covered here: a real NAT and a real ISP (no 2-PC test). The simulator also does not model
bandwidth limits (the party's bandwidth is small: ~180 KB/min UDP per guest in the runs above,
plus the 10 KB mirror image at join).

### 3c. Direct summon and title join under DSL + NAT (2026-10-13, 0.6.1)

The default join paths since 0.6.0 (title join and the bell's direct summon, `party_dsummon`)
had only run on loopback at 0 ms. Two runs, `BB_NET_SIM=dsl,nat=1` on both (80 ms ± 20 one-way,
2 % loss, 0.5 % dup, 1 % reorder; no TCP listener, guest code through `BB_PARTY_GUEST_CODE_FILE`),
existing characters, automated Continue:

| Run | Link (no TCP -> UDP) | Join | Host `cooperators 0 -> 1` | After |
|---|---|---|---|---|
| Title join (default) | guest `connecting over UDP` 13.0 -> `connected over UDP` 13.2 s, slot 1 at 13.5 s | Continue -> `client (6)` 20.7 s (game P2P punch heard after 500 ms) | 30.6 s | roster ping 172 ms; the run was ended by the central idle reaper at ~46 s |
| Bell, direct summon (`BB_PARTY_TITLE_JOIN=0`) | `connected over UDP` 10.1 s (0.15 s after the switch) | rang 19.54 -> host room created and sent 19.84 -> guest has it 19.95 (0.4 s) -> item delivered 21.58 (waits for the bell's own entry) -> `client (6)` 22.68: **bell -> client 3.1 s**, `dsum_done` ok at the host 22.78 | 27.41 s (**7.9 s after the bell**, guest loaded in) | **173 s stable** in the host's world (to the 200 s end): cooperators 1 throughout, roster ping 172-184 ms, 0 link losses / reconnects, 0 fallbacks to signs, clean shutdown on both |

Same as the loopback direct summon (bell -> client 3.0 s): the dsum messages ride the party link,
which is reliable (TCP or the reliable UDP stream), so a lost datagram costs a retransmit, not the
request; the 4 s answer wait (`kAnswerWait`) had 3.6 s to spare at 0.4 s, the 8 s entry/join
waits 6 s. No retry or timeout change was needed.

### 3d. No guest code: the DHT rendezvous under the NAT sim (2026-10-14, 0.6.1)

`BB_NET_SIM=dsl,nat=1` on both (no TCP listener on the host, unsolicited UDP dropped), **no
`BB_PARTY_GUEST_CODE_FILE`**, `BB_PARTY_DHT_BOOTSTRAP=127.0.0.1:46881` pointing at
`tools/mp/fake_dht.py` (one DHT node on loopback standing in for the public DHT). Title join with
existing characters, one clock for both logs:

| Time | Host | Guest |
|---|---|---|
| +17.4 | `NAT: cone (no open port)`, `rendezvous: looking for guests in the DHT every 5 s` | |
| +21.1 | | `NAT: cone; ... hole punching works (endpoint exchanged automatically)` |
| +22.6 | | `no TCP answer from the host; connecting over UDP` |
| +24.2 | | `rendezvous: announce done: ... stored at 1` |
| +27.5 | `a guest's endpoint 127.0.0.1:47601 found in the DHT`, `punching towards a guest` | |
| +27.7 | | the first UDP attempt (started before the punch) times out, retried |
| +30.2 / +30.5 | `Hunter1 rejoined (slot 1)` | `connected over UDP`, `network path: party link over UDP` |
| +32.1 | | title join: `client (6)` |
| +39.1 / +40.8 | `cooperators 0 -> 1` | `arrived 0.5 m from the host` |

Screenshots every 15 s: both hunters side by side in the host's world from 45 s to the end
(130 s), no glitch; the guest has the shared key items box open (nobody pressed OK). A first run
with the host polling every 10 s connected too (link over UDP 2.3 s after the announce), but the
guest's title screen showed *Failed to acquire Bloodborne server information* once at 20 s, while
its game waited for the link. The run then stopped at 28 s on its end markers, so the outcome of
that box was not seen. The host now polls every 5 s; in the second run the link was up 6 s after
the announce and no such box showed.
Not covered: the real DHT (public bootstrap nodes) and a real router. Both test instances stay
on 127.0.0.1.

## 4. NAT: what a host must do

- **Guests**: nothing. They connect out (TCP to the host, UDP to the host's port). Their own NAT
  keeps the mapping open, and the host answers to the observed address (the link's WELCOME and
  the STUN Binding Response at the host's port tell each guest its public address).
  Guest-to-guest game traffic hole-punches. Where that fails (symmetric NAT), it goes through the
  host relay automatically (`PeerPaths`; `BB_PARTY_FORCE_RELAY=1` forces it).
- **Host**, best case: one port reachable from the internet, **UDP and TCP, default 9307**
  (`BB_PARTY_PORT` / Party tab).
  1. **UPnP** (already implemented: `party/upnp_win.cpp`, Windows' own IUPnPNAT, on by default,
     `BB_PARTY_UPNP=0` off) asks the router to map both protocols while the game runs and removes
     the mappings on exit. UPnP itself causes no firewall prompt. When the router refuses, the log
     and the launcher say *"forward UDP+TCP 9307 to this PC"*.
  2. **Manual port forward**: if UPnP is off on the router, forward UDP and TCP 9307 to the host
     PC's LAN address.
  3. **Windows Firewall**: the first time the host listens, Windows asks once whether
     `bbport.exe` may accept connections. Allow it, on private and public networks, or the guests
     time out at *Connecting*. This prompt cannot be avoided without admin rights (a firewall
     rule needs elevation); it appears once per executable path.
  4. **The party code** carries the public address found by STUN (`BB_PARTY_STUN`, default
     Google's) or reported by the UPnP router, plus a LAN code for same-network guests. A fixed
     IP, DNS name or VPN address can be set as *Public address* (`BB_PARTY_PUBLIC_ADDR`).
- **Host without UPnP or a port forward: UDP hole punching with a guest code** (2026-10-11).
  1. The party code already carries the host's STUN-mapped public UDP endpoint.
  2. Each guest, on joining, finds its own public UDP endpoint of the party port by STUN and shows
     it as a **guest code** `BBG1-XXXX-XXXX-XXXX` (Crockford base32 of IPv4 + port + CRC-8;
     overlay Party page *Your guest code: copy*; also `user/party_guest_code.txt`).
  3. The guest sends that code to the host out of band (chat), the host presses *Add a guest
     code (paste)* (or `BB_PARTY_GUEST_CODES=...`, or a file named by `BB_PARTY_GUEST_CODE_FILE`).
  4. The host then sends small punch datagrams from its party port to the guest's endpoint every
     200 ms for 30 s. That opens the host's NAT mapping for that guest; the guest's own outbound
     packets opened its side. Both sides' NAT then pass the traffic (simultaneous open).
  5. The guest tries TCP first; after 1.5 s without an answer, or at once on *refused*, the party
     link switches to a **reliable UDP stream** on the same UDP port (`party_rudp.h`: SYN / DATA /
     cumulative ACK / FIN, 1180-byte segments, window 256, RTO from the smoothed RTT, 150 ms to
     2 s, keep-alive 1 s, 30 s idle limit). The link's frames, encryption, events and replay run
     over it unchanged. TCP simultaneous open was not used: Windows and most NATs handle it badly.
  6. The game's own UDP session uses the same port and thus the same NAT mapping, so it passes too.
  - Tests: `party-link-test` (`test_rudp`: clean, 10 % loss with 60 ms jitter, 2 x 300 KB in order;
    NAT filter without a punch fails, with a punch connects; `test_udp_nat_link`: a guest behind
    the simulated NAT connects over UDP in ~0.6 s, 200 events in order; a guest without a code
    does not). The simulator has a NAT mode (`BB_NET_SIM=...,nat=1`): inbound UDP is dropped
    unless we sent to that address:port first, and the host's TCP listener is off. Live run: see
    §3b (link over UDP 0.6 s after the punch started, bell join and B2 travel as usual).
- **No guest code needed: the automatic rendezvous (DHT)** (2026-10-14, 0.6.1, `party_rdv.h`,
  `party_traversal.h`). Without any server of ours, the guest's public UDP endpoint now reaches
  the host through the public BitTorrent DHT (BEP 5 "mainline", as any torrent client uses):
  1. The DHT key is `BLAKE2b("bbport rendezvous v1" || party key)` cut to 20 bytes. Only someone
     with the code (and the password) can compute it; DHT nodes see an opaque hash.
  2. Guest: when its party link has not come up 3 s after the start (TCP failed), it does an
     iterative `get_peers` lookup for the hash and `announce_peer`s the party port's STUN mapping
     to the 8 closest nodes (the node records the guest's public IP). Repeated every 45 s while
     the link is down; read-only node (BEP 43), so nobody routes through us.
  3. Host, only without an open port (no UPnP mapping, no override, not on a public IP): a
     `get_peers` lookup every 5 s (the first from the bootstrap nodes, later ones seeded with the
     closest nodes found: one hop). Every endpoint found is punched exactly like a pasted guest
     code (at most once per 50 s per endpoint). The guest's UDP SYNs then get in (§ above).
  4. Bootstrap: `router.bittorrent.com`, `dht.transmissionbt.com`, `router.utorrent.com`,
     `dht.libtorrent.org` (`BB_PARTY_DHT_BOOTSTRAP=host:port,...` replaces them).
     `BB_PARTY_RENDEZVOUS=0` turns it off; the guest code still works as before.
  - Exposure: the guest's public IP and the host's lookups appear on public DHT nodes under that
    hash, only when needed. A forged entry can only make the host send a few punch datagrams; the
    link still needs the party key.
  - Tests (`party-nat-test`): bencode / KRPC; a simulated 300-node Kademlia DHT with 5 % loss (the
    announce lands on the 8 closest nodes, the host's lookup finds the endpoint, a seeded re-poll
    takes 8 queries); the socket driver against fake nodes on loopback; and the whole path: a
    guest behind the simulated NAT, **no guest code**, connects over UDP ~0.8 s after the
    rendezvous starts (the host found it and punched once). Live: see §3d.
  - Not tested against the real DHT or a real router (test machines stay on 127.0.0.1).
- **NAT type and advice** (2026-10-14): host and guest ask a second STUN server
  (`BB_PARTY_STUN2`, default `stun.cloudflare.com:3478`) from the same party port. The same
  external port from both: a cone (punching works); another port per server: **symmetric**
  (punching cannot work). The log line `NAT: <type>; <advice>` and the overlay *Party* page
  (*Network:*) say what that means and what to do (forward UDP+TCP 9307, let a friend host, or a
  VPN such as Tailscale / ZeroTier). Once connected the guest logs `network path: party link over
  TCP|UDP` and the *Network* line shows it.
- **The fallback chain, verified for every pair of NAT types** (`party-nat-test`, a simulated NAT
  per RFC 4787 on each side carrying the real reliable-UDP streams, compared with the model in
  `party_nat.h`, 25 / 25 pairs agree):

  | Host NAT \ guest NAT | none | full cone | restricted | port-restricted | symmetric |
  |---|---|---|---|---|---|
  | none / open port (UPnP, forward) | TCP | TCP | TCP | TCP | TCP |
  | full cone | UDP direct | UDP direct | UDP direct | UDP direct | UDP direct |
  | restricted cone | punch | punch | punch | punch | punch |
  | port-restricted cone (most routers) | punch | punch | punch | punch | **no** |
  | symmetric | **no** | **no** | **no** | **no** | **no** |

  "punch" needs the guest's endpoint at the host: now automatic (rendezvous) or the guest code.
  "no": port forward on the host, another host, or a VPN. Guest-to-guest game traffic punches the
  same way and falls back to the host relay (`PeerPaths`).
  The network simulator has the same kinds: `BB_NET_SIM=...,nat=full|addr|port|sym` (`nat=1` =
  port); `sym` also makes the guest code / NAT check report another port per STUN server, so a
  live run detects a simulated symmetric NAT.
- **What CGNAT / hard NAT still cannot do** (no server anywhere, by design):
  - **Symmetric (endpoint-dependent) NAT** on either side - many mobile carriers and some
    carrier-grade NATs - gives the punch a different external port than STUN saw, so the guest
    code points at the wrong port and the punch fails. Two symmetric NATs can never meet without
    a relay.
  - **Port-restricted CGNAT with short mapping timeouts** may drop the mapping between STUN and
    the punch; the guest code then goes stale (the guest rejoins to get a new one).
  - **No inbound at all** (some campus / corporate firewalls block unsolicited UDP even after an
    outbound packet) cannot be punched.
  - There is no third-party relay or TURN server. **Relay through a member with an open port**
    (considered, not built): when the host cannot be reached at all but member A can, both the host
    and guest B could connect *out* to A and A would forward the link and the game's UDP between
    them (the host already relays guest-to-guest game traffic the same way, `PeerPaths`). It would
    keep the host's world, at the cost of A's upload (~3 KB/s per relayed guest) and one more hop
    of latency. Not built because the simpler remedies cover it: let A host (the world is then A's),
    or use a VPN that puts everyone on one network (Tailscale, ZeroTier, Radmin VPN) and the
    host's VPN address.
- There is no relay server: the host itself is the rendezvous and the relay. That is why only the
  host needs a port (or a guest code per guest).

## 5. Running the profiles

```
python tools/mp/instances.py run --party --count 2 --env BB_NET_SIM=lat=150,jit=30,loss=3 ...
--ienv "0:BB_PARTY_DIRECTOR_TEST=...,host_outage=5000@20%m24_01"
--ienv "1:BB_PARTY_DIRECTOR_TEST=...,guest_outage=5000@29%m24_01"
```
(the outage steps run S s after the last load / previous step, in the named map).

## 6. Security: what a stranger on the party port meets (F3, 2026-10-14)

Every byte from a peer is untrusted until the AUTH proof is checked, and still capped after it.

| Threat | Defence | Test |
|---|---|---|
| Garbage / oversized frames before the handshake | 64 KiB handshake frame cap, connection dropped; 16 pending handshakes in all, 4 per address; a handshake that never finishes times out | party-security-test `regress_link`, fuzz `linkhost` |
| Online password guessing | `AuthThrottle` (party_link.h): 5 wrong proofs per address per minute are free (typos, an old code), the 6th locks the address out for 30 s, every later wrong proof doubles it (to 15 min); a locked-out address is refused at HELLO with "too many wrong passwords from your address: try again in N s", the right password included. 4096 addresses tracked at most. Offline guessing of a captured handshake pays Argon2i (8 MiB, 3 passes) per guess | `regress_auth_throttle` (pure rule + a real host over loopback) |
| Replay / forged frames after WELCOME | Every frame is sealed with a per-direction session key and a counter nonce (party_crypto): a replayed, reordered or altered frame fails to open and drops the connection | fuzz `linkhost` (forged frame at the end) |
| A member flooding the host | Per-member token buckets (200 frames/s, 4 MiB/s, bursts 2000 / 48 MiB); RPCs over the limit get "rate limited"; 20000 drops in a row close the connection | `regress_link` flood |
| Malformed events | Every event body has a decoder with caps and a sanitizer (travel, story, items, phantom, lamp requests, world actions, flags, snapshots, world groups, balance, chalice goods, drop seed, direct summon bodies, gift_up, sync_digest / sync_resync); each is fuzzed with random and structure-aware inputs under UBSan (party-fuzz-ubsan) | party-fuzz / party-security-test (20 s run in the test) |
| A malicious host | The guest's decoders get the same checks (fuzz `linkguest`: mutated CHALLENGE / WELCOME / ROSTER / EVENT frames) | fuzz `linkguest` |

## 7. Desync detection and long outages (F5, 2026-10-14)

**Digests** (party_desync.h). The host sends every `BB_PARTY_DESYNC_S` (15 s) a `sync_digest`: 32
bucket hashes of its host-world mirror image and 32 of its candidate flags, with the flag stream's
seq. Each guest hashes what it holds: its mirror image (GuestWorldMirror) and a model of the host's
flags (the last snapshot plus every change after it). The link is reliable and ordered per member,
so the digest arrives after every change the host sent before it: any difference is a real desync
(a lost or misapplied event, a seq gap). The guest then asks `sync_resync` with the differing
buckets; the host answers with the world groups of those buckets (a partial `world_groups`, zero
groups included) and, for any flag bucket or a gap, a fresh `flag_snapshot` (the join catch-up
path: set-only, idempotent). Limits: a guest asks at most every 5 s and stops after 3 resyncs that
did not help ("persistent", logged once; retried after 5 min); the host serves one resync per member
per 10 s. Logs: `Party progress: desync: ...` on both sides.

**Long outages** (party-link-test `test_long_outages`, timings scaled 1:10 so the test runs in
~25 s: lost timeout 1 s for the game's 10 s, slot keep 6 s for 60 s):
- "30 s" outage (3 s): both sides call the link lost, the host keeps the slot, the guest reconnects
  by itself and resumes 3.1 s after the outage began; all 60 events posted before and during the
  outage arrive once, in order.
- "120 s" outage (12 s): past the keep the host releases the member; the guest still comes back by
  itself as a new member (not resumed). Events posted into the released slot are gone (0 of 10);
  the director's new-member path re-sends the full flag snapshot and world image, which covers C2.
- A host whose event callback stalls for 4 lost timeouts (a paused / alt-tabbed / loading host)
  keeps its member: the link thread answers pings; the 30 events sent meanwhile arrive in order.

**Live (2026-10-14, 2 clients, `BB_NET_SIM=lat=30`, `BB_PARTY_DESYNC_S=5`, guest `BB_PARTY_DESYNC_TEST=3`,
host `host_outage=30000@50`):** the first digest matched 15 s after the join; the injected
desync (mirror group 0 flipped, one host flag forgotten) was found by the next digest ("world
buckets 0, flag buckets 19"), the host sent 36 groups and a snapshot, and the following digest
said "resolved". The 30 s total outage: the host's link called the guest lost after 10 s (slot
kept); the guest's game left the session ~12 s in (its own timeout) and went home to its last lamp
(a 3.4 s loading screen); the link resumed as the network came back (20.2 s after it was lost);
the first direct summon was refused (the host's game still held the dead connection) and the bell
summon brought the guest back 2.4 m from the host, 41.3 s after the outage began (two loading
screens: correct, not seamless). 21 digests matched afterwards. Screenshots showed both players
together before and after and the HUD's "Connection lost" during.
