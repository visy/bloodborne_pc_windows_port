### Added
- **Native loading-screen hold (experimental)** `BB_PARTY_NATIVE_HOLD=1` makes the held loading screen of *Load with the host* hold the game's own screen gate, so the world does not start under it. Off by default: it adds about 0.7 s.
- **Rejoin re-sync** *Rejoin in place* asks a member that has not reappeared for its record with the game's own request. `BB_PARTY_RESYNC_0XA=0` turns it off.

### Changed
- **Arrive beside the host** the beside-host position is written into the load itself instead of a move after it, so the loading screen no longer waits for that move. `BB_PARTY_LOAD_PLACE=0` restores the held screen and move.

### Fixed
- Smoke harness: the boss-death row no longer fails when the game re-adds the host to the enemies' target list before the check runs.
