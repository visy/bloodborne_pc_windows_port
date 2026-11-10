### Fixed
- **a rare crash when a boss dies in co-op (an animation event of an enemy that was already gone)** (CL-078659).
  A guest's game crashed once on a Havok worker thread. The crash happened 75 ms after Mergo's Wet Nurse died,
  while the game played the animation events of a character whose animation data had already been released.
  That event is now skipped, as if the animation had none. The log says
  `Havok event guard: animation event of a freed animation skipped (n)`. The fix works only when the game
  hits that fault, so it costs nothing while you play. `BB_HAVOK_EVENT_GUARD=0` turns it off.
  `BB_HAVOK_EVENT_TEST=N` is a test aid: it makes every N-th such event fault on purpose.
- **a possible GPU crash ("device lost") while a level loads** (CL-ab12f5; ported from Nightwatch 1c99d56,
  Lantern Engine by Awaken, https://github.com/Awakenchan/LanternEngine; modified: kill switch). When one texture
  of a draw was looked up, the lookup could free a texture an earlier binding of the same draw had just found.
  The remembered texture set then kept pointing at the freed texture's view until the next texture change.
  A set is now stamped with the cache state from before its lookups, so such a set is looked up again the next
  time it is used. `BB_TEXSET_GEN_FIX=0` restores the old stamping.
