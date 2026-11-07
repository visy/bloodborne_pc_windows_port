# Release runbook

The steps of a coop/port release `X.Y.Z`, in order, with the tools in `tools/release/`. The long gate lists
(full smoke, blockers, known issues) stay in `docs/RELEASE_CHECKLIST.md`; this file is the order of the commands.
Paths below are the usual ones (`E:\games\bbport`); `<clone>` is `agents\NAME` (repo in `agents\NAME\repo`).

## 1. Version bump

- [ ] `VERSION`, `launcher.py` (`PORT_VERSION`), `README.md` (`**Version X.Y.Z**`), `CHANGES.md` (`## X.Y.Z (date)`).
      `python -m pytest tests/test_version.py -q` passes.
- [ ] `CHANGES.md` for the version is grouped `### Added` / `### Changed` / `### Fixed`, plain player-facing
      wording, no player names, IPs or log excerpts, upstream authors by public handle.
- [ ] Release notes file (for GitHub) = the same three sections, same text.
- [ ] Commit the bump on the release branch, merge master into it, `python -m pytest tests -q` is green.

## 2. PGO configure and build (in a clone, from the exact commit to release)

- [ ] `python tools/release/configure_pgo.py <clone>` copies the profile from `src\pgo` into
      `<repo>\pgo_map\<mangled build dir>` (gcc keys `.gcda` files by absolute object path) and configures
      `<repo>\out\gpu-pgo-gen`: Ninja, RelWithDebInfo, `BB_LTO=ON`, `BB_STRIP=ON`, `BB_PGO=use`, `VMA_INC`,
      `MINIZ_INC` and the compilers from `src\out\gpu-pgo-gen\CMakeCache.txt`; no `-march` (older CPUs, game
      code goes through Legacy CPU). `--dry-run` only prints. `tools/agent/build.py` never configures.
- [ ] Build: `set PATH=C:\msys64\mingw64\bin;%PATH%` then `cmake --build <repo>\out\gpu-pgo-gen`
      (machine-wide build limits: only when no other build runs). The exe lands in `<repo>\out\release`.
- [ ] The tree is clean when the exe is built: the exe embeds `git describe --dirty`; staging refuses a `-dirty`
      id or one that is not the repo's HEAD. Do not commit after the build (or rebuild).
- [ ] An LTO build matters: asm-only symbols are only discarded by LTO (see `__attribute__((used))` notes).
- [ ] To re-record the profile instead of reusing it, see the `release-066` memory note (train in the same build dir).

## 3. Short smoke on the release exe

- [ ] `python tools/mp/smoke_party.py --build <repo> --only <groups of the late fixes>` on the **release** exe
      (a handful of phases is enough here; the full gate ran on master before, `docs/RELEASE_CHECKLIST.md` section 2).
      Look at the screenshots. `bbport.exe --version` says `X.Y.Z`.

## 4. Stage

- [ ] `python tools/release/stage.py X.Y.Z <repo>\out\release E:\games\bbport\relXYZ --base E:\games\bbport\rel<previous>`
      - tracked release files come from the repo's HEAD (docs without `docs\superpowers`, scripts, patches,
        `tools\legacy-cpu-plan.json`, launcher, `run.bat`, `net_doctor.bat`, README, CHANGES, LICENSE);
      - files git does not hold (DLLs and licenses in `out\`, `fsr4_shaders\`, `python\`, `map_generator\`, `mods\`,
        the `user\` skeleton) come from `--base`; the new exes and DLLs overwrite them;
      - symbols (`bbport.exe`, `bbport.exe.debug`, `bb-gpu-capabilities.exe`) go to `E:\games\bbport\rel\symbols\X.Y.Z`;
      - it writes `relXYZ.stamp.json` (version, full commit the exe was built from, exe sha256).
- [ ] Python bundle changed (rare): `python tools/release/make_python_bundle.py --out relXYZ\python` first.
- [ ] Map generator changed: `python tools/map/build_generator.py ...` (see `docs/RELEASE_CHECKLIST.md`), copy to `relXYZ\map_generator`.
- [ ] Join check on the staged build (`relcheck`) and the phases of any late fix on the final exe.

## 5. Zip

- [ ] `python tools/release/make_zip.py E:\games\bbport\relXYZ E:\games\bbport\bloodborne_pc_windows_port_vX.Y.Z.zip`
      refuses (writes nothing) when the stage holds logs, `*.ini`, saves (`userdata` / `savedata`), `player_logs`,
      `.env` files, keys, a top-level entry outside the allowlist, game files (`eboot.bin`, `*.elf`, `dvdroot`,
      `.dcx` / param / msg / map dumps, a `*.bin` over 5 MB unless `--allow`ed), debug symbols, or any file under
      `user\` except `readme.txt` and `custom_trophy\Notice.txt`.
- [ ] Note its output: size, file count, zip sha256, exe sha256 (`testzip` ran). It also writes
      `<zip>.stamp.json`, which `publish.py` checks.

## 6. Merge and push

- [ ] Merge the release branch into master in `E:\games\bbport\src`, `python -m pytest tests -q`.
- [ ] `git push fork master`. **Only** that: never `origin` (Mrsuss60), `upstream` or `supermedo`.
- [ ] Install over `E:\games\bloodborne`: `robocopy relXYZ E:\games\bloodborne /E /XD relXYZ\user relXYZ\mods /XF bbport.ini launcher_settings.json`;
      check `VERSION` and the exe hash.

## 7. Publish (dry run first)

- [ ] `git rev-parse HEAD` of the pushed master: the **full** 40-character sha, equal to the stamp's commit
      (the exe's commit). If master moved on after the build, the exe is not from it: rebuild, or tag the stamped commit.
- [ ] Dry run (default; prints the checks and requests, no network, no token):
      `python tools/release/publish.py --repo visy/bloodborne_pc_windows_port --tag vX.Y.Z --commit <FULL SHA> --title "..." --notes notes.md --zip <zip>`
- [ ] Real run: the same command plus `--yes-publish`. It refuses an existing release or tag, a commit that is
      not the exe's, a changed zip. SSL uses certifi when installed: **do not** run it with `python -I`.
      The token comes from `git credential fill` and is never printed.
- [ ] Iosefka: `cl release coop X.Y.Z --next <next>` (preview with `--dry-run`).
- [ ] **A coop release gets no #releases post**: the Nightwatch update announcement covers it (and its retest asks).
- [ ] Memory note `release-XYZ`: tag, smoke result, hashes, gotchas.

## What the tools check, in short

| Tool | Refuses when |
| --- | --- |
| `stage.py` | `VERSION` or `bbport.exe --version` is not X.Y.Z; exe build id is `-dirty` or not the repo's HEAD; the stage fails the zip rules; the stage dir exists (without `--replace`) |
| `make_zip.py` | the stage rules above; the output exists; the exe changed after staging |
| `publish.py` | short sha; commit is not the exe's; tag is not `vX.Y.Z`; local tag on another commit; zip or exe differs from the stamp; empty notes; release or tag exists; no `--yes-publish` means dry run |
