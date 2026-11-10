### Added
- **Optional direct connection over the Nightwatch VPN (lower ping)** a new switch, *Allow direct connection (lower ping, party members see your IP)*
  (*System > My party: Loading*, a launcher checkbox, `bbport.ini` `party_nw_direct`, off by default). Over the Nightwatch VPN your
  game normally reaches every party member through the tunnel. When you and another member both turn this on, the game tests a
  direct connection to them for about 3 seconds (20 probes while play stays on the tunnel) and switches to it, once and together at a
  loading screen, only if every probe was answered and it is at least 20 ms or 30 % faster than the tunnel. A direct connection that
  then drops for 2 seconds goes back to the tunnel for the rest of the session. The other member sees your public IP. With the switch
  off (or the other member's off) nothing changes and no public address is looked up.
