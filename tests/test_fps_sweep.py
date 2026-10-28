"""Frame-rate sweep of the party harness (tools/mp/instances.py --fps, tools/mp/smoke_party.py --fps): no game needed."""
from paths import ROOT
import importlib.util
import sys
import unittest

MP = ROOT / "tools" / "mp"
sys.path.insert(0, str(MP))


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, MP / file)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


instances = load("mp_instances_fps", "instances.py")
smoke = load("mp_smoke_fps", "smoke_party.py")


class ParseFpsTests(unittest.TestCase):
    def test_single_value_for_every_instance(self):
        self.assertEqual(instances.parse_fps("60", 3), ["60", "60", "60"])

    def test_host_guest_mix(self):
        self.assertEqual(instances.parse_fps("60/30", 3), ["60", "30", "30"])

    def test_per_instance_last_repeats(self):
        self.assertEqual(instances.parse_fps("60/30/90", 4), ["60", "30", "90", "90"])
        self.assertEqual(instances.parse_fps("60/30/90", 2), ["60", "30"])

    def test_uncap_and_bad_values(self):
        self.assertEqual(instances.parse_fps("30/UNCAP", 2), ["30", "uncap"])
        for bad in ("", "abc", "60/x", "0", "-5"):
            with self.assertRaises(ValueError):
                instances.parse_fps(bad, 2)

    def test_sweep_entries(self):
        self.assertEqual(smoke.parse_fps_sweep("30, 60,90,60/30"), ["30", "60", "90", "60/30"])
        with self.assertRaises(ValueError):
            smoke.parse_fps_sweep("30,abc")
        with self.assertRaises(ValueError):
            smoke.parse_fps_sweep(" , ")


class SweepNamingTests(unittest.TestCase):
    def test_dir_names(self):
        self.assertEqual(smoke.fps_dir_name("60"), "fps60")
        self.assertEqual(smoke.fps_dir_name("60/30"), "fps60x30")
        self.assertEqual(smoke.fps_dir_name("60/30/90"), "fps60x30x90")

    def test_child_args_drop_fps_and_out(self):
        argv = ["--only", "elevator", "--fps", "30,60", "--out", "X", "--every", "5", "--fps=1", "--out=Y"]
        self.assertEqual(smoke.sweep_args(argv), ["--only", "elevator", "--every", "5"])

    def test_physics_groups_exist(self):
        for g in smoke.PHYSICS_GROUPS:
            self.assertIn(g, smoke.GROUPS)
        for g in ("lifts", "elevator", "ladder", "fall", "frontier_fall"):
            self.assertIn(g, smoke.PHYSICS_GROUPS)


class PhaseFpsTests(unittest.TestCase):
    def test_no_sweep_keeps_phase_key(self):
        self.assertEqual(smoke.phase_fps({"fps": 60}), "60")
        self.assertIsNone(smoke.phase_fps({}))

    def test_sweep_overrides_unless_fixed(self):
        self.assertEqual(smoke.phase_fps({"fps": 60}, "30"), "30")
        self.assertEqual(smoke.phase_fps({}, "60/30"), "60/30")
        self.assertEqual(smoke.phase_fps({"fps": 60, "fps_fixed": True}, "30"), "60")


class SummaryTests(unittest.TestCase):
    def test_sensitive_detection(self):
        results = {"30": {"el": [5, 0], "lf": [4, 0], "fl": [3, 1]},
                   "60": {"el": [5, 0], "lf": [2, 2], "fl": [3, 1]},
                   "60/30": {"el": [4, 1], "lf": [4, 0], "fl": [3, 1]}}
        text, sens = smoke.fps_summary(results, ["30", "60", "60/30"])
        self.assertEqual(sens, ["el", "lf"])  # fl fails everywhere: broken, not frame-rate sensitive
        self.assertIn("FPS-sensitive (pass at some rates, fail at others): el, lf", text)
        self.assertIn("PASS 5/5", text)
        self.assertIn("FAIL 2/4", text)

    def test_all_pass_none_sensitive(self):
        text, sens = smoke.fps_summary({"30": {"el": [5, 0]}, "60": {"el": [5, 0]}}, ["30", "60"])
        self.assertEqual(sens, [])
        self.assertIn("FPS-sensitive (pass at some rates, fail at others): none", text)

    def test_missing_phase_in_a_rate_is_a_dash(self):
        text, sens = smoke.fps_summary({"30": {"el": [5, 0]}, "60": {}}, ["30", "60"])
        self.assertEqual(sens, [])
        self.assertIn("-", text.splitlines()[1])


class FrameGenerationTests(unittest.TestCase):
    def test_suffix_split(self):
        self.assertEqual(instances.split_fg("60fg"), ("60", "fsr"))
        self.assertEqual(instances.split_fg("120FG"), ("120", "fsr"))
        self.assertEqual(instances.split_fg("60fgdlss"), ("60", "dlss"))
        self.assertEqual(instances.split_fg("uncapfg"), ("uncap", "fsr"))
        self.assertEqual(instances.split_fg("90"), ("90", None))

    def test_parse_fps_strips_fg(self):
        self.assertEqual(instances.parse_fps("60fg", 2), ["60", "60"])
        self.assertEqual(instances.parse_fps("60fg/30", 3), ["60", "30", "30"])
        self.assertEqual(instances.parse_fps("uncapfg", 1), ["uncap"])
        for bad in ("fg", "xfg", "60fx", "0fg"):
            with self.assertRaises(ValueError):
                instances.parse_fps(bad, 2)

    def test_parse_fg_per_instance(self):
        self.assertEqual(instances.parse_fg("60fg", 2), ["fsr", "fsr"])
        self.assertEqual(instances.parse_fg("60fg/30", 3), ["fsr", None, None])
        self.assertEqual(instances.parse_fg("30/60fgdlss", 3), [None, "dlss", "dlss"])
        self.assertEqual(instances.parse_fg("120", 2), [None, None])

    def test_env(self):
        e = instances.fps_env("60", "fsr")
        self.assertEqual((e["BB_FPS"], e["BB_FRAME_GENERATION"], e["BB_FG_BACKEND"], e["BB_FG_TIMING"]),
                         ("60", "2x", "fsr", "1"))
        e = instances.fps_env("120", None)
        self.assertEqual((e["BB_FPS"], e["BB_FRAME_GENERATION"]), ("120", "off"))  # an old ini cannot turn it on
        self.assertNotIn("BB_FG_BACKEND", e)
        self.assertEqual(instances.fps_env("60", "fsr", headless=True)["BB_FRAME_GENERATION"], "off")

    def test_sweep_entries_and_dirs(self):
        self.assertEqual(smoke.parse_fps_sweep("30,60,90,120,60fg,60/30"), ["30", "60", "90", "120", "60fg", "60/30"])
        self.assertEqual(smoke.fps_dir_name("60fg"), "fps60fg")
        self.assertEqual(smoke.fps_dir_name("60fg/30"), "fps60fgx30")

    def test_measured_rates(self):
        lines = ["Frame generation: FSR 2x on (presented frames per rendered frame; presentation queue independent)"]
        for k in range(8):
            lines.append("[+ %d.0] Frame rate: %d.0 s real, 600 steps (%.1f per s), game time 1.000 s per real s, "
                         "last dt 0.01667 s; effects time 1.000 s per real s" % (k, 10 * k, 59.5 + (k % 2)))
            lines.append("FG pacing: source %.2f presented %.2f generated 60 skipped 0" % (60.0, 119.0 + k % 2))
        r = smoke.measured_rates(lines)
        self.assertEqual(r["fg"], "FSR 2x on")
        self.assertAlmostEqual(r["steps"], 60.5)
        self.assertEqual(r["source"], 60.0)
        self.assertGreater(r["presented"], 118)
        r = smoke.measured_rates(["Frame generation: FSR unavailable (no derivatives); presenting normally"])
        self.assertEqual(r["fg"], "FSR unavailable")
        self.assertIsNone(r["steps"])

    def test_measured_summary_flags_fg_not_running(self):
        import json, tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as d:
            ph = Path(d) / "el"
            ph.mkdir()
            (ph / "instances.json").write_text(json.dumps({"count": 2, "headless": 0}))
            (ph / "inst0.log").write_text("Frame generation: FSR 2x on (x)\n"
                                          "FG pacing: source 60.00 presented 119.50 generated 1 skipped 0\n")
            (ph / "inst1.log").write_text("Frame generation: FSR unavailable (no); presenting normally\n")
            lines, problems = smoke.measured_summary(d, "60fg")
            self.assertIn("frame generation FSR 2x on (rendered 60.0, presented 119.5 per s)", lines[0])
            self.assertEqual(len(problems), 1)
            self.assertIn("inst1", problems[0])
            # the same logs at a plain rate ask for nothing
            self.assertEqual(smoke.measured_summary(d, "60")[1], [])
            # a headless instance runs no frame generation: not a problem
            (ph / "instances.json").write_text(json.dumps({"count": 2, "headless": 1}))
            self.assertEqual(smoke.measured_summary(d, "60fg")[1], [])


if __name__ == "__main__":
    unittest.main()
