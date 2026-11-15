### Fixed

- **Nightwatch VPN under Proton / Wine:** the game now recognises the Nightwatch tunnel when Linux shows it as a plain `tun0` (no VPN word in its name), by its tunnel address, so `party_vpn=auto` picks it; a real home network on 10.8.x is still treated as the LAN (Nightwatch, ported from the NW Proton VPN work).
