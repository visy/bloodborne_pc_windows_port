# Troubleshooting

Back to the [README](../README.md).

## Game

| Problem | What to do |
|---|---|
| White or grey picture with FSR 4 | Seen on some PCs; suspected cause is FSR 4 with HDR turned on in Windows (still being confirmed). Switch to FSR 3.1, DLSS or no upscaler |
| Start refused, "60 fps eboot" | The `eboot.bin` has a 60 fps patch baked in; use a clean v1.09 dump ([INSTALL.md](INSTALL.md)) |
| Old CPU (no MOVBE / BMI1 / LZCNT) | The game's code is translated at start: works, but slower ([INSTALL.md](INSTALL.md)) |
| Lost or damaged save | Backups and restore steps: [SAVES.md](SAVES.md) |
| Shader stutter, shared caches | [SHADERS.md](SHADERS.md) |
| Settings and environment variables | [SETTINGS.md](SETTINGS.md) |

## Co-op

| Problem | What to do |
|---|---|
| Guests stay at *Connecting* | Host port 9307 (UDP and TCP) must be reachable; CGNAT needs a VPN; Windows Firewall must allow `bbport.exe` (launcher: *Fix firewall issues*). Steps: [COOP.md](COOP.md#first-time-step-by-step) |
| More connection options (guest codes, DHT, IPv6) | [COOP.md](COOP.md#connecting-over-the-internet), [party/network_robustness.md](party/network_robustness.md) |
| Hosting, ports, VPN, crashes, disconnects | [PARTY.md](PARTY.md) (Troubleshooting and Known limitations sections) |
| Known issues and what is coming | [COOP.md](COOP.md#known-issues) |

## Reporting a bug

| Step | How |
|---|---|
| Before playing | Launcher, *Advanced / Debug*, *Party logging*: **Verbose** on every PC |
| When it happens | Press **F9** (or *Party tab > Mark an issue*): every player's log gets `=== USER MARK n ===` |
| After playing | *Advanced / Debug*, **Save support bundle** on every PC; send the zips (passwords, codes and addresses are removed). A crash writes one by itself to `logs\` |

Details: [party/logging.md](party/logging.md).
