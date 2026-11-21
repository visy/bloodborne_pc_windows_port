### Fixed
- **Guest never joins after the direct summon** (vladtepes22, NW-dc104e5, NW-ffb869b) a guest whose own world had
  the area boss of the region it stands in killed (a played save ringing from a cleared area) took the host's room and
  then dropped it: no join, the bell rang in a loop. The game's summon pickup kept the area-boss rule that "Party:
  Bells after boss defeated" lifts elsewhere; it is lifted there too. The guest's game is also put in its bell wait
  mode for the handover (a game that never set it took the room without joining).
