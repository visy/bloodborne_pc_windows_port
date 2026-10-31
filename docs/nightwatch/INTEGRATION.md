# Nightwatch Co-op: changes to integrate from bbport

For the Nightwatch Co-op maintainer (and a Claude session working in the NightwatchCoop repo). Pull this repo
(`visy/bloodborne_pc_windows_port`, branch `master`), then work through the items below in order. Each item says what
changes on the engine side (our code, which NW carries under `engine/`) and on the launcher side (NW's own C# code),
how to check it, and what must not happen. Newest items are added at the end; an item marked **done** is already in a
NW release.

Engine side, in general: NW's `engine/` tracks our `master` (the log header's `co-op visy vX+sha` names the commit).
Sync it the usual way; the files listed per item are the ones that matter for that item.

---

## 1. VPN tunnel range 10.8.0.0/22 (room for 1021 players)

**Why.** The Nightwatch VPN server hands out 10.8.0.0/24: 253 players at most. To grow, the server widens its pool to
/22 (10.8.0.2 - 10.8.3.254). The game and the launcher only accepted 10.8.0.2 - 10.8.0.254, so a player given
10.8.1.x would connect but could not announce or see games. This item makes both accept the whole /22. A /24 server
only hands out 10.8.0.x, which stays inside the new range, so **this build works before and after the server change**.

**Rule (identical everywhere):** a player address is inside 10.8.0.0/22 and is not 10.8.0.0 (network), 10.8.0.1 (the
server) or 10.8.3.255 (broadcast). As code: `(ip & 0xfffffc00) == 0x0a080000 && ip != 0x0a080000 && ip != 0x0a080001
&& ip != 0x0a0803ff` (host byte order).

**Engine side** (from our commit 32987d04, "VPN: accept the 10.8.0.0/22 tunnel range"):
- `gpu/shim/net/party_vpnlobby.cpp` `is_tunnel_player_address()` and its comment in `party_vpnlobby.h`.
- `tests/test_party_net.cpp`: the range checks (party-net-test).
- (Our `launcher.py` and `tools/vpn/lobby/nwlobby.py` changed too; NW does not ship them.)

**Launcher side** (NW code, not in our repo): `launcher/windows/BloodborneLauncher/Core/VpnLobby.cs`
`IsTunnelAddress()` and the range check in `launcher/windows/SeamlessTests/VpnTests.cs`. A ready patch against
NightwatchCoop `main` at 8cc2efc is in [vpn22-launcher.patch](vpn22-launcher.patch):

```
git am /path/to/bloodborne_pc_windows_port/docs/nightwatch/vpn22-launcher.patch
```

If it does not apply (the files moved on), make the same change by hand: replace the body of
`VpnLobby.IsTunnelAddress(uint ip)` with the rule above and extend the test: 10.8.0.255, 10.8.1.2 and 10.8.3.254 are
tunnel addresses; 10.8.0.0, 10.8.0.1, 10.8.3.255, 10.8.4.2 and 10.9.0.2 are not. `PartyDiscovery.cs` (labels a
tunnel address "Nightwatch Online") calls `IsTunnelAddress` and needs no change. Nothing else in the launcher
hard-codes the range (`VpnLobby.Address` stays 10.8.0.1).

**Check:** `SeamlessTests` passes (the "tunnel range (10.8.0.0/22)" check); party-net-test passes on the engine; on a
VPN PC the Join page still lists games and hosting still announces.

**Roll-out order (do not change it):**
1. Ship a NW beta with this item (engine + launcher).
2. Wait until most players run it (Iosefka's `/vpnstatus` "Versions" line shows the builds in the game list).
3. Only then the VPN server cutover (visy runs it: `cutover.sh` in our `tools/vpn/server/`, one VPN restart, ~30 s
   reconnect for everyone, announced on Discord first). Players on an older build keep working while they get a
   10.8.0.x address; after the cutover, new connections can get 10.8.1.x - 10.8.3.x, which old builds cannot use.

**Do not:** change the server address, the lobby port (9310) or the profile; do not widen beyond /22 (the server and
its firewall are prepared for /22 only).

---

## 2. The launcher drops an idle VPN connection and reconnects on demand

**Status:** written as a patch, **not built or run on our side** (we do not build NW): build it and run `SeamlessTests`
before release. Apply after item 1:

```
git am /path/to/bloodborne_pc_windows_port/docs/nightwatch/vpn-idle-launcher.patch
```

Files: new `Core/VpnIdle.cs` (the pure rule), `Views/SeamlessPage.cs` (`VpnIdleCheck`, the setting row, a
`byPlayer` flag on `ToggleVpn` so a manual Disconnect is remembered), `MainWindow.xaml.cs` (window activation, a
30 s timer, the Play wait), `Core/LauncherSettings.cs` (defaults), `SeamlessTests/VpnTests.cs` (`IdleRules`). The new
texts are English only: add them to the translations.

NW's launcher owns the OpenVPN connection (`Core/NightwatchVpn.cs`, `Views/SeamlessPage.cs` `ToggleVpn`/`WatchVpn`),
so this is launcher-only work.

**Why.** A launcher left open keeps its tunnel up forever: on 2026-10-31, 18 of 30 connected players were in no game.
Each one holds a pool address and server capacity.

**Behaviour:**
- Only for a connection the player turned on in this launcher (`NightwatchVpn.OwnedHere`), and only when the player has
  not turned the VPN off by hand (then it stays off until they press Connect again).
- **Disconnect when idle:** the game is not running, and the launcher window has not been focused for N minutes
  (default 10). Show in the VPN bar: "Disconnected while idle; it reconnects when you need it."
- **Reconnect on demand:** when the window is activated, when the Co-op page (Host or Join) is shown, and before
  Play starts the game as host or guest (Play waits for the tunnel, bounded ~15 s, then starts anyway with the
  existing "connection did not come up" note).
- Never disconnect while the game runs (`GameStateChanged`), during a connect/disconnect in progress (`_vpn.Busy`), or
  while another launcher owns the connection.
- Setting: Co-op tab "Disconnect the VPN when idle" (on by default) and the minutes; stored with the other launcher
  settings.

**Check:** `SeamlessTests` passes (`IdleRules`); by hand: Connect on Co-op, put the launcher in the background with no
game for 10 min (or set `vpn_idle_minutes` to 1): the VPN bar shows "Disconnected while idle"; bring the window back:
it reconnects; press Disconnect, background it, bring it back: it stays off; Play as host after an idle drop: the game
starts once the tunnel is up (15 s at most).

---

## Reference

- Our VPN docs: [docs/party/nightwatch_vpn.md](../party/nightwatch_vpn.md) (lobby wire format, counts socket,
  status-file fix).
- Server scripts: `tools/vpn/server/` (cutover.sh, rollback.sh, no-status-file.sh, ssh-no-password.sh), lobby:
  `tools/vpn/lobby/`. The server's address is never in this repo.
