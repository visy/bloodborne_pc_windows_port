### Fixed
- **a guest in another game region than the host could not join Chalice Dungeons: its game played offline** (NW-cec05db,
  NW-be0f493, NW-cab726f). The guest's game got its server-status document (ss.info) from the host at the title, and that
  document was written for the host's region; a guest of another region (for example the host in Europe and the guest in
  America, or a guest with the "Unlock Game Region" patch) could not read it, quietly never signed in and loaded its world
  in offline mode. The guest then played with the host as usual, but every Chalice Dungeon summon stopped with "This game is
  in offline mode" (before 0.7.8: it was sent home and the summon failed with "This game is offline"). The guest's game now
  writes its own ss.info for its own region. The log says `http: ss.info answered in this process ...`.
  `BB_PARTY_SSINFO_LOCAL=0` asks the host as before.
