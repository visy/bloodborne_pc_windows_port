# Release runbook

The steps of a coop/port release `X.Y.Z`, in order, with the tools in `tools/release/`. The long gate lists
(full smoke, blockers, known issues) stay in `docs/RELEASE_CHECKLIST.md`; this file is the order of the commands.
Paths below are the usual ones (`E:\games\bbport`); `<clone>` is `agents\NAME` (repo in `agents\NAME\repo`).

## 1. Version bump

- [ ] `VERSION`, `launcher.py` (`PORT_VERSION`), `README.md` (`**Version X.Y.Z**`), `CHANGES.md` (`## X.Y.Z (date)`).
      `python -m pytest tests/test_version.py -q` passes.
- [ ] `CHANGES.md` for the version is grouped `### Added` / `### Changed` / `### Fixed`, plain player-facing
      wording, no player names, IPs or log excerpts, upstream authors by public handle.
- [ ] Changelog fragments: agents add `changes.d/<slug>.md` instead of editing CHANGES.md (see `changes.d/README.md`).
      `python tools/release/changelog.py check`, then `python tools/release/changelog.py assemble X.Y.Z --date YYYY-MM-DD`
      (merges the fragments into the `## X.Y.Z` section, deletes them), review `git diff CHANGES.md`, commit. `stage.py` and
      `build_release.py` refuse while `changes.d/` still holds fragments in the commit.
- [ ] Release notes file (for GitHub) = the same three sections, same text.
- [ ] Commit the bump on the release branch, merge master into it, `python -m pytest tests -q` is green.

## 2. Build, stage and zip: one command

- [ ] Commit the version bump with the release date in the `CHANGES.md` heading (`## X.Y.Z (2026-..)`, not
      `(unreleased)`) on `master` of `E:\gamesbport\src`, tracked files clean.
- [ ] `python tools/release/build_release.py X.Y.Z --base E:\gamesbport
el<previous>`
      (`--commit SHA` for another commit than `master`, `--out-root` / `--work` for a scratch run, `--dry-run` to
      see each step's state). It runs, in order, skipping a step whose output already exists and matches the
      commit (so a stopped run resumes):
      1. clone of `src` at the commit into `agents
elcloneXYZ` with `git clone --no-tags`, then tags from the
         `fork` remote only (an upstream tag such as Mrsuss60's `v0.8.0` at another commit must not reach the
         clone; `publish.py` refuses a local tag on another commit). Refuses a `src` with modified tracked files;
      2. checks `VERSION` and that `CHANGES.md` at the commit does not say `(unreleased)` for X.Y.Z and `changes.d/` holds no fragments;
      3. `configure_pgo.py` (submodules, `libatrac9`, FSR patches, committed `pgo-profile\`);
      4. `cmake --build` of `out\gpu-pgo-gen` with `C:\msys64\mingw64in` on PATH, below-normal priority,
         through the machine-wide build queue (`tools/mp/game_queue.py`, pool `build`, same as `tools/agent/build.py`);
         a full LTO build takes about 20 minutes;
      5. `stage.py` into `relXYZ` and `make_zip.py` into `bloodborne_pc_windows_port_vX.Y.Z.zip` (+ stamps);
      6. the release notes (the `CHANGES.md` section, `notes_vX.Y.Z.md`) and a `publish.py` **dry run**.
      It never publishes: it ends by printing the exact `publish.py ... --yes-publish` command.
- [ ] The exe's build id ignores submodule changes (the FSR patches are applied inside the submodule), so a clean
      commit gives a build id without `-dirty`; changes to tracked files of the main tree still mark it `-dirty`
      and staging refuses that. Do not commit after the build (or rebuild).
- [ ] An LTO build matters: asm-only symbols are only discarded by LTO (see `__attribute__((used))` notes).
- [ ] The profile is committed in `pgo-profile\` (flat `.gcda` files). After re-recording, copy
      `src\pgo\E~\gamesbport\src\out\gpu-pgo-gen\*.gcda` over it and commit. To re-record instead of reusing it, see
      the `release-066` memory note (train in the same build dir).
- [ ] Python bundle changed (rare): `python tools/release/make_python_bundle.py --out relXYZ\python` before the zip step
      (delete the zip and stamp, rerun). Map generator changed: see `docs/RELEASE_CHECKLIST.md`.

## 3. Short smoke on the release exe

- [ ] `python tools/mp/smoke_party.py --build <repo> --only <groups of the late fixes>` on the **release** exe
      (a handful of phases is enough here; the full gate ran on master before, `docs/RELEASE_CHECKLIST.md` section 2).
      Look at the screenshots. `bbport.exe --version` says `X.Y.Z`.
- [ ] One two-PC session with the seamless loading switches on (the 0.8.2 defaults: `party_parallel_follow`, `party_settled_drop`,
      `party_load_beside`, `party_quick_rejoin`): lamp travel, a death follow, Party > Rejoin; look at both screens.

## 4. Merge and push

- [ ] Merge the release branch into master in `E:\games\bbport\src`, `python -m pytest tests -q`.
- [ ] `git push fork master`. **Only** that: never `origin` (Mrsuss60), `upstream` or `supermedo`.
- [ ] Install over `E:\games\bloodborne`: `robocopy relXYZ E:\games\bloodborne /E /XD relXYZ\user relXYZ\mods /XF bbport.ini launcher_settings.json`;
      check `VERSION` and the exe hash.

## 5. Publish (dry run first)

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
| `stage.py` | `VERSION` or `bbport.exe --version` is not X.Y.Z (it runs the exe with mingw64 on PATH and prints the exit code and stderr when that fails); `CHANGES.md` still says `## X.Y.Z (unreleased)` or `changes.d/` still holds fragments; exe build id is `-dirty` or not the repo's HEAD; the stage fails the zip rules; the stage dir exists (without `--replace`) |
| `make_zip.py` | the stage rules above; the output exists; the exe changed after staging |
| `publish.py` | short sha; commit is not the exe's; tag is not `vX.Y.Z`; local tag on another commit (the message says to delete it or use a `build_release.py` clone, which has no upstream tags); zip or exe differs from the stamp; empty notes; release or tag exists; no `--yes-publish` means dry run |
## Appendix: the manual steps (what build_release.py runs)

Only for a step that has to be run by hand (a failed step, a special build).

### A. PGO configure and build (in a clone, from the exact commit to release)

- [ ] `python tools/release/configure_pgo.py <clone>` first does build.bat's source prep from git (submodules,
      `out\libatrac9.a` from `third_party\LibAtrac9`, the FSR-Vulkan patches), then copies the profile from `src\pgo` into
      `<repo>\pgo_map\<mangled build dir>` (gcc keys `.gcda` files by absolute object path) and configures
      `<repo>\out\gpu-pgo-gen`: Ninja, RelWithDebInfo, `BB_LTO=ON`, `BB_STRIP=ON`, `BB_PGO=use`, `VMA_INC`,
      `MINIZ_INC` and the compilers from `src\out\gpu-pgo-gen\CMakeCache.txt`; no `-march` (older CPUs, game
      code goes through Legacy CPU). `--dry-run` only prints. `tools/agent/build.py` never configures.
- [ ] zlib-ng (the fast inflate, `BB_FAST_INFLATE`): a pinned submodule, `gpu/third_party/zlib-ng` (2.2.x tag), built
      static by `gpu/CMakeLists.txt` (required: configure fails if it is not initialised; `configure_pgo.py` inits it;
      `git submodule update --init gpu/third_party/zlib-ng` otherwise). Runtime CPU dispatch, no `-march`.
- [ ] zstd (converted game files, `bb-zstdpack`, `BB_ZSTD_DIR`): vendored as the git submodule `third_party/zstd` (tag v1.5.7),
      built static from source by `gpu/CMakeLists.txt` (required: configure fails if the submodule is not initialised;
      `configure_pgo.py` runs `git submodule update --init`). `bb-zstdpack.exe` lands in `out\` and is staged with the other exes.
- [ ] Build: `set PATH=C:\msys64\mingw64\bin;%PATH%` then `cmake --build <repo>\out\gpu-pgo-gen`
      (machine-wide build limits: only when no other build runs). The exe lands in `<repo>\out\release`.
- [ ] The tree is clean when the exe is built: the exe embeds `git describe --dirty`; staging refuses a `-dirty`
      id or one that is not the repo's HEAD. Do not commit after the build (or rebuild).
- [ ] An LTO build matters: asm-only symbols are only discarded by LTO (see `__attribute__((used))` notes).
- [ ] The profile is committed in `pgo-profile\` (flat `.gcda` files). After re-recording, copy
      `src\pgo\E~\games\bbport\src\out\gpu-pgo-gen\*.gcda` over it and commit.
- [ ] To re-record the profile instead of reusing it, see the `release-066` memory note (train in the same build dir).

### B. Stage

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

### C. Zip

- [ ] `python tools/release/make_zip.py E:\games\bbport\relXYZ E:\games\bbport\bloodborne_pc_windows_port_vX.Y.Z.zip`
      refuses (writes nothing) when the stage holds logs, `*.ini`, saves (`userdata` / `savedata`), `player_logs`,
      `.env` files, keys, a top-level entry outside the allowlist, game files (`eboot.bin`, `*.elf`, `dvdroot`,
      `.dcx` / param / msg / map dumps, a `*.bin` over 5 MB unless `--allow`ed), debug symbols, or any file under
      `user\` except `readme.txt` and `custom_trophy\Notice.txt`.
- [ ] Note its output: size, file count, zip sha256, exe sha256 (`testzip` ran). It also writes
      `<zip>.stamp.json`, which `publish.py` checks.

