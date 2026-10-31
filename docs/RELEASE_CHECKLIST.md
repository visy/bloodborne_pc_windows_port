# Release checklist

The steps for a coop/port release (`X.Y.Z`), revised after 0.7.7. Push only with `git push fork master`.

## 1. Before the smoke

- [ ] Every item of the version in Iosefka is `done`, `dropped` or moved on: `cl list --track coop --version X.Y.Z`.
- [ ] All agent branches for the release are merged into master and `python -m pytest tests -q` passes.
- [ ] Upstreams checked (Mrsuss60 `origin`, deadinside28 `upstream`, Supermedo `supermedo`): crash and VRAM fixes ported or
      queued, the rest moved to the next version (`agents/upstream_<date>.md`).

## 2. Full smoke on master

- [ ] Run the release gate: `BB_MP_FPS=60 python tools/mp/smoke_party.py --release --lanes auto --out <dir>`. `--release` is
      the **full** list (all 155 phases, nothing hidden; the old default was a 40-phase quick list, now `--tier quick`).
      The run prints `tier full: N of 155 phases`, then `progress done/total` with an ETA after every phase, and writes
      the usual `verdict.txt`, per-phase `rows.json` and `retries.txt`.
      - Time (measured 2026-10-31, 60 FPS, one lane): a phase is a real-time game session, about 107 s on average
        (boot to world is only 13 s of it), so game speed cannot be raised and the 1 h goal is not reachable on this PC.
        `--tier quick` (40 phases) took 2 h 50 min with 16 failed rows (retries of failing flaky phases are the biggest
        single cost: lfw and lfwc 24 min each, s3 17 min). `--tier release` (136 phases, variants of `RELEASE_SKIP`
        left out) is about 5 h, full (155) about 5.5 h when the phases pass. A second lane needs ~20 GB of commit per
        3-game phase on top of the first; this 32 GB PC has no room for it while anything else runs, so `--lanes auto`
        picks 1 lane (2 only with 40 GB free). The game queue's memory guard (`game_queue.py`, only for guarded jobs) is
        what allows 2; it never goes over 6 clients, keeps 3 GB free and waits for other agents' runs first.
        Never start more lanes by hand: 2 lanes x 3 games ran out of memory in 0.7.4.
      - A smaller run: `--tier release` (every feature, no variants), `--changed-since <tag>` (only the phases that
        exercise the files changed since the tag; docs/tests run nothing, a game file with no mapping runs the quick tier),
        `--tier quick` (the 40-phase core, the default), `--phase a,b,c` or `--only <group>`.
      - Phase times of the last run are kept in `<agent>\smoke\phase_times.json` (seed: `tools/mp/phase_times_seed.json`)
        and feed the ETA and the lane order (longest first).
- [ ] Compare with the previous release's full smoke (steps and failed rows).
- [ ] Sort every failed row, and aim for 0:
  - **Test fault**: fix the test (`tools/mp`), rerun that phase with `--only`.
  - **Flaky**: rerun at 30 and 60 FPS; make the test wait for state instead of sleeping. A pass that needs a retry
    (`retries.txt`) counts as flaky, not as a pass; read its first failure.
  - **Real bug**: fix it before release, or hold the release.
- [ ] A crash in any phase is real until proven otherwise: symbolize the return address
      (`addr2line -f -C -e out/bbport.exe 0x14xxxxxxx`). A GPU report written next to a crash comes from the crash
      handler, not the cause.
- [ ] For a failure that could be old, run the phase on the previous release's exe: same failure there = not a regression.
- [ ] After a test-only fix, one clean run is enough (user decision, 0.7.7). After a game-code fix, rerun on the release exe.

**Blocks the release**: a crash, softlock or stuck player, save harm, a regression from the previous release in a core
flow (join, travel, death, bosses, lifts), or a changelog item shown broken (fix it or take the line out).
**May ship as a known issue**: a proven test fault, a flaky test with a known test cause, a small real bug that is not
a regression.

## 3. Build and package

- [ ] Version bump commit: `VERSION`, `launcher.py`, README, `CHANGES.md` (`## X.Y.Z (date)`); `tests/test_version.py` passes.
- [ ] Release build: PGO use + LTO, **no `-march`** (older CPUs; game code via Legacy CPU). `bbport.exe --version` says X.Y.Z.
- [ ] Symbols to `rel\symbols\X.Y.Z`.
- [ ] Stage `relXYZ` from the previous stage plus the new exe, gpu-caps, launcher, `run.bat`, `net_doctor.bat`, docs
      (without docs/superpowers), scripts, patches, `tools/legacy-cpu-plan.json`. No saves, shader caches or `__pycache__`.
- [ ] Join check on the staged build (`relcheck`), plus the phases touched by any late fix, on the final exe.
- [ ] Zip `bloodborne_pc_windows_port_vX.Y.Z.zip` from the final stage; `zipfile.testzip()` is clean.
- [ ] Release notes: plain **Added / Changed / Fixed** sections; no player names, IPs or log excerpts; credit upstream
      authors by their public handles. `CHANGES.md` and the notes say the same.
- [ ] The exe's game code matches master: `git diff --stat <exe commit> HEAD -- . ':!tools' ':!tests' ':!docs' ':!*.md'` is empty.

## 4. Publish

- [ ] Merge the release branch into master, run pytest, `git push fork master`.
- [ ] Install over `E:\games\bloodborne` and leave `user\`, `mods\`, `bbport.ini` and `launcher_settings.json` alone:
      `robocopy relXYZ E:\games\bloodborne /E /XD relXYZ\user relXYZ\mods /XF bbport.ini launcher_settings.json`.
      Check `VERSION` and the exe hash.
- [ ] GitHub release `vX.Y.Z` at the full master SHA with the zip (scratchpad `make_release.py repo tag SHA title notes zip`;
      run it **without** `python -I`, or the SSL check fails).
- [ ] Iosefka: `cl release coop X.Y.Z --next <next>` (preview with `--dry-run`); unfinished items move to the next version.
- [ ] No #releases post for a coop release: the Nightwatch update announcement covers it (and its retest asks).
- [ ] Memory note `release-XYZ` with the tag, the smoke result and the gotchas.
