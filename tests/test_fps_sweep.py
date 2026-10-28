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


if __name__ == "__main__":
    unittest.main()
