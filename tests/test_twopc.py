"""tools/twopc (the two-PC session kit): the plan, the verdict rows, the kit zips, the beacon files."""
import json
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

from paths import ROOT

TW = ROOT / "tools" / "twopc"
sys.path.insert(0, str(TW))
sys.path.insert(0, str(ROOT / "scripts"))
import blocks  # noqa: E402
import make_kits  # noqa: E402
import runner  # noqa: E402
import verdict  # noqa: E402


def log(*lines):
    return "".join("[+%.3f] %s\n" % (t, s) for t, s in lines)


class Plan(unittest.TestCase):
    def test_ids_and_select(self):
        ids = [s["id"] for s in blocks.sessions()]
        self.assertEqual(ids, ["1a.travel", "1a.rejoin", "1a.join", "1c", "1d", "1e", "2a", "2c", "3"])

        def sel(only="", skip=""):
            return [s["id"] for s in blocks.select(blocks.sessions(), only.split(",") if only else [], skip.split(",") if skip else [])]
        self.assertEqual(sel("1a"), ["1a.travel", "1a.rejoin", "1a.join"])
        self.assertEqual(sel(skip="1d,2,3"), ["1a.travel", "1a.rejoin", "1a.join", "1c", "1e"])
        self.assertEqual(sel("1c,3"), ["1c", "3"])

    def test_sessions_are_event_synced(self):
        for s in blocks.sessions():
            self.assertTrue(s["host_until"] and s["seconds"] >= 300, s["id"])
            if s["start"].startswith("event:"):
                self.assertIn(s["start"][6:], s["signals"])
        sim = {s["id"]: s["net_sim"] for s in blocks.sessions()}
        self.assertEqual((sim["1a.travel"], sim["2a"], sim["2c"]), (None, "dsl", "dsl"))

    def test_no_game_data_in_the_plan(self):
        self.assertNotIn("dvdroot", (TW / "blocks.py").read_text(encoding="utf-8"))


class Rows(unittest.TestCase):
    def test_boss_rows_use_each_pcs_own_clock(self):
        pc1 = log((10, "cooperators 0 -> 1"), (30, "member revived: task mode 1 (step 2) rewound to STEP_Create"),
                  (31, "rebuilt member chr 1a (HP 5, rebuilt): target-bank state 0"), (50, "boss health bar shown"),
                  (80, "host death in a boss fight"), (81, "boss target dump x: players [chr 1 hp 5] [chr 2a hp 9]"),
                  (82, "boss target dump x: boss 5: kind 0 t h 2a]"))
        pc2 = log((20, "local death: HP 0"), (21, "died alone: revived at its own last lamp"), (900, "test: god_bar: mortal now"),
                  (920, "Party balance: hit: HP 100 -> 80"))
        R = verdict.Rows({"pc1": verdict.Log(pc1), "pc2": verdict.Log(pc2)})
        verdict.s_boss(R, "1c")
        failed = [n for n, s, d in R.out if s == "FAIL"]
        self.assertEqual(failed, [], R.out)

    def test_boss_hit_missing_fails(self):
        pc1 = log((10, "cooperators 0 -> 1"), (50, "boss health bar shown"), (80, "host death in a boss fight"))
        pc2 = log((20, "local death: HP 0"), (900, "test: god_bar: mortal now"))
        R = verdict.Rows({"pc1": verdict.Log(pc1), "pc2": verdict.Log(pc2)})
        verdict.s_boss(R, "1c")
        self.assertTrue(any(s == "FAIL" and "boss hits the guest" in n for n, s, d in R.out))

    def test_screens(self):
        host = verdict.Log(log((5, "test: travel step 1/3: host_warp to lamp 1"), (9.5, "Party seam: loading screen dropped (settled)"),
                               (20, "test: travel step 2/3: host_warp to lamp 2"), (26, "Party seam: loading screen dropped (settled)")))
        guest = verdict.Log(log((5.5, "guest: loading screen raised at the host's travel"), (10, "Party seam: loading screen dropped (cap)")))
        self.assertEqual([round(x, 1) for x in verdict.screens(host, True)], [4.5, 6.0])
        self.assertEqual([round(x, 1) for x in verdict.screens(guest, False)], [4.5])

    def test_unmasked_ip_scan(self):
        ipm = verdict._ipmask()
        self.assertIsNotNone(ipm)
        self.assertEqual(ipm.unmasked_ips("host 10.8.x.x ok 127.0.0.1"), [])
        self.assertTrue(ipm.unmasked_ips("host 203.0.113.7 leaked"))

    def test_verdict_end_to_end(self):
        with tempfile.TemporaryDirectory() as d:
            paths = []
            for role, text in (("pc1", log((29, "cooperators 0 -> 1"))),
                               ("pc2", log((40, "Party > Rejoin pressed now")) + "203.0.113.7 leaked\n")):
                p = Path(d) / ("twopc_logs_%s_x.zip" % role)
                with zipfile.ZipFile(p, "w") as z:
                    z.writestr("twopc_logs_%s/meta.json" % role, json.dumps({"role": role, "plan": ["1a.rejoin"], "results": []}))
                    z.writestr("twopc_logs_%s/1a.rejoin/%s.log" % (role, role), text)
                paths.append(str(p))
            r = subprocess.run([sys.executable, str(TW / "verdict.py")] + paths, capture_output=True, text=True)
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
            self.assertIn("FAIL  no unmasked IP address in the pc2 zip", r.stdout)
            self.assertIn("PASS  no unmasked IP address in the pc1 zip", r.stdout)
            self.assertIn("PASS  1a.rejoin join: 2 players", r.stdout)


class Kits(unittest.TestCase):
    def test_zip_layout_and_no_stage_player_data(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            stage, game, saves = d / "stage", d / "game", d / "saves"
            for p, body in (("run.bat", "x"), ("out/bbport.exe", "e"), ("user/savedata/1/userdata0000", "PLAYER"),
                            ("user/readme.txt", "r"), ("launcher_settings.json", "{}"), ("logs/x.log", "l"), ("scripts/a.py", "p")):
                (stage / p).parent.mkdir(parents=True, exist_ok=True)
                (stage / p).write_text(body)
            for p in ("eboot.bin", "sce_discmap.plt", "dvdroot_ps4/map/a.bin", "docs/not_game.txt", "python/python.exe"):
                (game / p).parent.mkdir(parents=True, exist_ok=True)
                (game / p).write_text("g")
            for fx in ("savedata", "chalice_pthumeru_ritual/savedata"):
                base = saves / fx
                (base / "1").mkdir(parents=True)
                (base / "1" / "userdata0000").write_text("fixture")
            groups = make_kits.kit_members("pc2", stage, game, saves, "readme")
            out = d / "pc2.zip"
            make_kits.write_zip(out, "twopc081_pc2", groups)
            z = zipfile.ZipFile(out)
            names = z.namelist()
            self.assertIn("twopc081_pc2/CUSA03173/eboot.bin", names)
            self.assertIn("twopc081_pc2/CUSA03173/dvdroot_ps4/map/a.bin", names)
            self.assertNotIn("twopc081_pc2/CUSA03173/python/python.exe", names)      # only the game parts travel
            self.assertIn("twopc081_pc2/bbport/run.bat", names)
            self.assertNotIn("twopc081_pc2/bbport/user/savedata/1/userdata0000", names)
            self.assertNotIn("twopc081_pc2/bbport/launcher_settings.json", names)
            self.assertIn("twopc081_pc2/twopc/fixtures/chalice_pthumeru_ritual/savedata/1/userdata0000", names)
            self.assertIn("twopc081_pc2/START_PC2.bat", names)
            self.assertEqual(json.loads(z.read("twopc081_pc2/twopc/config.json")), {"role": "pc2", "game_dir": "CUSA03173"})
            info = {i.filename: i.compress_type for i in z.infolist()}
            z.close()
            self.assertEqual(info["twopc081_pc2/CUSA03173/dvdroot_ps4/map/a.bin"], zipfile.ZIP_STORED)
            pc1 = make_kits.kit_members("pc1", stage, game, saves, "r")
            self.assertEqual([a for _, a in pc1[2] if "chalice" in a], [])           # pc1 needs only the default save

    def test_space_check_refuses(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(SystemExit) as e:
                make_kits.check_space(Path(d), 1 << 60)
            self.assertIn("not enough free space", str(e.exception))

    def test_gitignore_guard(self):
        text = (TW / ".gitignore").read_text()
        for pat in ("*.zip", "CUSA03173/"):
            self.assertIn(pat, text)


class Sync(unittest.TestCase):
    def test_file_sync_roundtrip(self):
        with tempfile.TemporaryDirectory() as d:
            a, b = runner.FileSync("pc1", d), runner.FileSync("pc2", d)
            self.assertIsNone(b.peer())
            a.publish(idx=2, session="1c", code="X")
            self.assertEqual(b.peer()["code"], "X")
            b.publish(state="done")
            self.assertEqual(a.peer()["state"], "done")

    def test_same_24(self):
        self.assertTrue(runner.same24("10.8.0.5", "10.8.0.77"))
        self.assertFalse(runner.same24("10.8.0.5", "10.8.1.5"))

    def test_mask_in_runner_output(self):
        self.assertEqual(runner.mask("peer 203.0.113.7:9307"), "peer 203.0.x.x:9307")


if __name__ == "__main__":
    unittest.main()
