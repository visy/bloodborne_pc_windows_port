# Network robustness: internet play, latency, loss, outages, NAT

Party co-op runs serverless peer-to-peer. Before 2026-10-11 every live test ran over loopback at
0 ms. This page covers the network simulator, the runs under realistic profiles and what a host
must do about NAT.

## 1. Transports

| Channel | Protocol | What it carries | Reliability |
|---|---|---|---|
| Party link (`party_link.h`) | TCP to the host's party port (default 9307) | Roster, RPC (the guest game's FROM API / matchmaking), our EVENTs: travel, `flags`, `flag_snapshot`, `world_groups`, items, story, lamp requests, `lamp_lit`... | TCP (in order, retransmitted), plus a per-stream event cursor: unacked events are replayed after a reconnect and de-duplicated. Lost = no frame for 10 s; the slot is kept 60 s; the guest reconnects with backoff 1-8 s. |
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

Not covered here: a real NAT and a real ISP (no 2-PC test). The simulator also does not model
bandwidth limits (the party's bandwidth is small: ~180 KB/min UDP per guest in the runs above,
plus the 10 KB mirror image at join).

## 4. NAT: what a host must do

- **Guests**: nothing. They connect out (TCP to the host, UDP to the host's port). Their own NAT
  keeps the mapping open, and the host answers to the observed address (the link's WELCOME and
  the STUN Binding Response at the host's port tell each guest its public address).
  Guest-to-guest game traffic hole-punches. Where that fails (symmetric NAT), it goes through the
  host relay automatically (`PeerPaths`; `BB_PARTY_FORCE_RELAY=1` forces it).
- **Host**: one port must be reachable from the internet, **UDP and TCP, default 9307**
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
  5. **Carrier-grade NAT / no forwarding possible** (mobile, some ISPs): the host cannot be
     reached. Use a VPN that puts everyone on one network (Tailscale, ZeroTier, Radmin VPN) and
     the host's VPN address, or let a player with a reachable port host.
- There is no relay server: the host itself is the rendezvous and the relay. That is why only the
  host needs a port.

## 5. Running the profiles

```
python tools/mp/instances.py run --party --count 2 --env BB_NET_SIM=lat=150,jit=30,loss=3 ...
--ienv "0:BB_PARTY_DIRECTOR_TEST=...,host_outage=5000@20%m24_01"
--ienv "1:BB_PARTY_DIRECTOR_TEST=...,guest_outage=5000@29%m24_01"
```
(the outage steps run S s after the last load / previous step, in the named map).
