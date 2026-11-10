### Added
- **Shorter, smoother loading screens in a party (My party: Loading, all off by default)** four new switches,
  each a System menu row (*System > My party: Loading*), a launcher checkbox (My Party tab) and a `bbport.ini` key:
  *Load with the host* (`party_parallel_follow`): on the host's party travel to a lamp, the Hunter's Dream, a
  headstone or a Hunter's Mark your game starts loading at once, in parallel with the host, behind one loading
  screen, instead of waiting for the host's load first; *Hold the screen until smooth* (`party_settled_drop`): the
  loading screen stays up until the world runs smoothly (at most 2 s more), so the first heavy frames after a load
  are not seen as a frozen world; *Arrive beside the host* (`party_load_beside`): a party travel's load puts you
  beside the host, with no jump after the screen; *Rejoin in place* (`party_quick_rejoin`): *Party > Rejoin* in the
  host's world reloads you beside the host with one loading screen, without *Leave* and the trip to your own
  world. Anything unusual (a death, a boss fight, a Chalice Dungeon, another map) keeps the old way. The frame-end
  quiet point is ported by intent from droogie/bbhost (GPL-3.0, live_resolution.cpp at bdc5cdf).
