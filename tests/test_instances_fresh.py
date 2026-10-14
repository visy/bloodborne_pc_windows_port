"""tools/mp/instances.py --fresh-saves: a run never inherits the last run's character state."""
from paths import ROOT
import importlib.util
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("mp_instances", ROOT / "tools" / "mp" / "instances.py")
instances = importlib.util.module_from_spec(spec)
spec.loader.exec_module(instances)


class FreshSavesTests(unittest.TestCase):
    def test_fresh_saves_drop_rot_and_prologue_state(self):
        with tempfile.TemporaryDirectory() as tmp:
            user = Path(tmp)
            (user / "savedata").mkdir()
            (user / "savedata" / "s").write_bytes(b"new")
            (user / "savedata_party").mkdir()
            (user / "savedata_party" / "s").write_bytes(b"old")
            for name in ("party_rot.txt", "party_prologue.txt", "party_state.json", "party_story.json"):
                (user / name).write_text("x")
            (user / "party.json").write_text("{}")
            instances.party_saves_from(user, fresh=True)
            self.assertEqual((user / "savedata_party" / "s").read_bytes(), b"new")
            for name in ("party_rot.txt", "party_prologue.txt", "party_state.json", "party_story.json"):
                self.assertFalse((user / name).exists(), name)
            self.assertTrue((user / "party.json").exists())  # the party settings stay

    def test_not_fresh_keeps_rot(self):
        with tempfile.TemporaryDirectory() as tmp:
            user = Path(tmp)
            (user / "savedata").mkdir()
            (user / "party_rot.txt").write_text("stacks 2\n")
            instances.party_saves_from(user, fresh=False)
            self.assertTrue((user / "party_rot.txt").exists())


if __name__ == "__main__":
    unittest.main()
