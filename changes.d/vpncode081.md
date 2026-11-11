### Fixed

- **Nightwatch VPN host codes** (NW-03d11e5) a party hosted over the Nightwatch VPN now gives a code with the host's full tunnel address, so players whose VPN address is in a different range than the host's can join with it (before, only players with a nearby address could). Old short codes keep working, and the game now also tries the other address ranges for them. If a code still fails, join from the VPN lobby list or type the host's full VPN address. Set `BB_PARTY_VPN_FULL_CODE=0` to get the old short code.
