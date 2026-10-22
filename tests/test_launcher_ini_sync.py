"""launcher.py game settings kept in bbport.ini only (launchsync): the upscaler's checkbox + choice
<-> bbport.ini upscaler, the 0/1 game settings, motion blur resolution and the effect patches, the
one-time move of old launcher_settings.json values, no BB_* override for them at launch, and an
off-screen window that shows a test bbport.ini (plus the DLSS row's buttons staying on the card)."""
from paths import ROOT
import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock


def load_launcher():
    spec = importlib.util.spec_from_file_location("bb_launcher_ini_sync", ROOT / "launcher.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class IniSync(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.ini = Path(self.tmp.name) / "bbport.ini"

    def tearDown(self):
        self.tmp.cleanup()

    # ---- upscaler
    def test_upscaler_both_ways(self):
        L = self.L
        self.assertEqual([c[0] for c in L.UPSCALER_CHOICES], ["FSR 3.1", "FSR 4", "FSR 4.1.1", "TAA", "DLSS"])
        for label, name in L.UPSCALER_CHOICES:
            ini = L.upscaler_ini(True, label)
            self.assertEqual(ini, {"upscaler": name})
            self.assertEqual(L.upscaler_state(ini), (True, label))
            self.assertEqual(L.upscaler_ini(False, label), {"upscaler": "off"})
        # off: the remembered choice comes back for re-enabling; nothing remembered: FSR 3.1
        self.assertEqual(L.upscaler_state({"upscaler": "off"}, "DLSS"), (False, "DLSS"))
        self.assertEqual(L.upscaler_state({}, ""), (False, "FSR 3.1"))
        self.assertEqual(L.upscaler_state({"upscaler": "bogus"}, "nonsense"), (False, "FSR 3.1"))

    # ---- converted settings, read and write
    def test_flags_read_write(self):
        L = self.L
        for _, key, default, _ in L.INI_FLAG_SETTINGS:
            self.assertEqual(L.ini_flag_value({}, key, default), default, key)
            for value in (True, False):
                L.update_ini(self.ini, {key: "1" if value else "0"})
                self.assertEqual(L.ini_flag_value(L.read_ini(self.ini), key, default), value, key)

    def test_volumetric_read_write(self):
        L = self.L
        self.assertEqual(L.volumetric_label({}), "Game default (half)")
        self.assertEqual(L.volumetric_label({"volumetric_res": "full"}), "Full resolution")
        for label, value in L.VOLUMETRIC_CHOICES:
            self.assertEqual(L.volumetric_ini(label), {"volumetric_res": value})
            self.assertEqual(L.volumetric_label(L.volumetric_ini(label)), label)

    def test_effect_patches_read_write(self):
        L = self.L
        self.assertEqual(L.effect_patches_from_ini({}), set())
        names = {"Skip Intro", "Disable Motion Blur (perf increase)", "Model LOD 1 (Lower)"}
        ini = L.effect_patches_ini(names)
        self.assertEqual((ini["skip_intro"], ini["effect_motion_blur"], ini["model_lod"], ini["effect_dof"]),
                         ("1", "0", "1", "1"))
        self.assertEqual(L.effect_patches_from_ini(ini), names)
        self.assertEqual(L.effect_patches_from_ini(L.effect_patches_ini(set())), set())

    def test_effect_keys_match_patches_py(self):
        import importlib.util as iu
        spec = iu.spec_from_file_location("bb_patches_sync", ROOT / "scripts" / "patches.py")
        patches = iu.module_from_spec(spec)
        spec.loader.exec_module(patches)
        for name, (key, on) in self.L.EFFECT_PATCHES.items():
            off_patch, on_patch = patches.EFFECTS[key]
            self.assertEqual(on_patch if on == "1" else off_patch, name)
        # Each ini value applies the patch the launcher names for it; the withheld "Lowest" patch
        # (it drops map floors) is written as Lower and patches.py applies Lower for an old 2; the
        # withheld "Highest" patch (far objects not drawn) is written as the game's own detail and
        # patches.py applies nothing for an old -2.
        shown = {v: k for k, v in self.L.MODEL_LOD_PATCHES.items() if k not in self.L.WITHHELD_PATCHES}
        self.assertEqual(shown, {v: k for v, k in patches.MODEL_LOD.items() if v != "2"})
        self.assertEqual(self.L.MODEL_LOD_PATCHES["Model LOD 2 (Lowest)"], "1")
        self.assertEqual(patches.MODEL_LOD["2"], "Model LOD 1 (Lower)")
        self.assertEqual(self.L.MODEL_LOD_PATCHES["Model LOD -2 (Highest)"], "0")
        self.assertNotIn("-2", patches.MODEL_LOD)
        self.assertEqual(patches.effect_patches({"model_lod": "-2"}), [])
        listed = {p["name"] for p in self.L.load_xml_patches()}
        self.assertNotIn("Model LOD 2 (Lowest)", listed)
        self.assertNotIn("Model LOD -2 (Highest)", listed)
        self.assertEqual(self.L.effect_patches_ini({"Model LOD -2 (Highest)"})["model_lod"], "0")

    # ---- launch environment
    def test_no_game_setting_env(self):
        L = self.L
        old = {"feat_upscaler": False, "feat_object_motion": False, "feat_hud": True, "feat_gpl": True,
               "feat_mouse_keyboard": True, "feat_async_pipelines": False, "feat_compile_indicator": False,
               "volumetric_res": "Full resolution",
               "enabled_patches": ["Skip Intro", "Disable Motion Blur (perf increase)",
                                   "Performance Patch (perf increase)", "Model LOD -2 (Highest)"]}
        for s in (old, {}):
            env = L.settings_env(s)
            for name in L.GAME_SETTING_ENVS:
                self.assertNotIn(name, env)
        # launch-only patches still go through BB_PATCHES, effect patches come from bbport.ini
        self.assertEqual(L.settings_env(old)["BB_PATCHES"], "Performance Patch (perf increase)")
        self.assertIsNone(L.settings_env({})["BB_PATCHES"])

    # ---- migration
    def test_migration_missing_keys(self):
        L = self.L
        s = {}
        written = L.migrate_settings_to_ini(s, self.ini)
        ini = L.read_ini(self.ini)
        self.assertEqual(ini["upscaler"], "fsr3")  # the old launcher default (checked)
        self.assertEqual(ini["skip_intro"], "1")   # the old default patch list
        self.assertEqual(ini["async_pipelines"], "1")
        self.assertEqual(s["ini_sync_rev"], L.INI_SYNC_REV)
        self.assertTrue(written)
        self.assertIsNone(L.migrate_settings_to_ini(s, self.ini))  # once only

    def test_migration_ini_wins_unless_forced(self):
        L = self.L
        self.ini.write_text("upscaler=dlss\nshow_hud=1\nobject_motion=1\nvolumetric_res=2\nskip_intro=0\n"
                            "key.cross=X\n", encoding="utf-8")
        # defaults that never overrode the ini: the ini's values stay
        L.migrate_settings_to_ini({"feat_upscaler": True, "feat_hud": False, "feat_object_motion": True,
                                   "volumetric_res": "In-game setting", "enabled_patches": []}, self.ini)
        ini = L.read_ini(self.ini)
        self.assertEqual((ini["upscaler"], ini["show_hud"], ini["object_motion"], ini["volumetric_res"],
                          ini["skip_intro"], ini["key.cross"]), ("dlss", "1", "1", "2", "0", "X"))
        # values that overrode the ini at every launch (BB_UPSCALER=off, BB_PATCHES=Skip Intro ...)
        L.migrate_settings_to_ini({"feat_upscaler": False, "feat_object_motion": False,
                                   "volumetric_res": "2x pixels", "enabled_patches": ["Skip Intro"],
                                   "feat_gpl": True}, self.ini)
        ini = L.read_ini(self.ini)
        self.assertEqual((ini["upscaler"], ini["object_motion"], ini["volumetric_res"], ini["skip_intro"],
                          ini["gpl"]), ("off", "0", "1", "1", "1"))

    # ---- party gameplay switches (0.7.0: bbport.ini, the game's party rules pages)
    def test_party_switches_in_ini(self):
        L = self.L
        keys = [k for k, _, _ in L.PARTY_INI_SWITCHES]
        for key in ("party_host_revive", "party_host_limbo_revive", "party_guest_revive_inplace", "party_items",
                    "party_kill_record", "party_gather", "party_npc_mirror", "party_key_view",
                    "party_key_items_summoned", "party_covenant_per_player", "party_npc_fights"):
            self.assertIn(key, keys)
        env = L.party_env({"party_mode": "host", "party_host_limbo_revive": False})
        for _, name, _ in L.PARTY_INI_SWITCHES:
            self.assertIn(name, env)
            self.assertIsNone(env[name], name)  # unset: bbport.ini decides
        self.assertIsNone(env["BB_PARTY_GUEST_INSIGHT"])
        self.assertEqual(env["BB_PARTY_RESTART"], "1")
        values = L.party_rules_ini({"party_items": False}, "Full bonus")
        self.assertEqual((values["party_items"], values["party_host_revive"], values["party_guest_insight"]),
                         ("0", "1", "full"))
        for label, v in L.PARTY_INSIGHT_CHOICES:
            self.assertEqual(L.party_insight_label({"party_guest_insight": v}), label)
        self.assertEqual(L.party_insight_label({}), L.PARTY_INSIGHT_CHOICES[0][0])

    def test_rot_off_migration_once(self):
        """0.7.2 settings_rev 1: party_rot=1 saved by a 0.7.1 game goes to 0 once; a later 1 stays."""
        L = self.L
        self.assertIsNone(L.migrate_ini_rev(self.ini))  # no bbport.ini: nothing to do
        self.ini.write_text("party_rot=1\nupscaler=dlss\n", encoding="utf-8")
        self.assertEqual(L.migrate_ini_rev(self.ini), {"settings_rev": "1", "party_rot": "0"})
        ini = L.read_ini(self.ini)
        self.assertEqual((ini["party_rot"], ini["settings_rev"], ini["upscaler"]), ("0", "1", "dlss"))
        L.update_ini(self.ini, {"party_rot": "1"})  # turned on again by the player
        self.assertIsNone(L.migrate_ini_rev(self.ini))
        self.assertEqual(L.read_ini(self.ini)["party_rot"], "1")
        # the game did it first (settings_rev=1 written): the launcher leaves it
        self.ini.write_text("party_rot=1\nsettings_rev=1\n", encoding="utf-8")
        self.assertIsNone(L.migrate_ini_rev(self.ini))
        # a bbport.ini the launcher creates starts at the current revision
        self.ini.unlink()
        L.update_ini(self.ini, {"party_rot": "1"})
        self.assertIsNone(L.migrate_ini_rev(self.ini))
        self.assertEqual(L.read_ini(self.ini)["party_rot"], "1")

    def test_party_switches_migrate_once(self):
        L = self.L
        self.ini.write_text("party_items=1\nupscaler=dlss\n", encoding="utf-8")
        s = {"ini_sync_rev": 1, "party_items": False, "party_host_limbo_revive": True,
             "party_guest_insight": "0", "party_restart": False}
        written = L.migrate_settings_to_ini(s, self.ini)
        ini = L.read_ini(self.ini)
        self.assertEqual((ini["party_items"], ini["party_host_limbo_revive"], ini["party_guest_insight"],
                          ini["upscaler"]), ("0", "1", "0", "dlss"))
        self.assertNotIn("party_restart", ini)  # a launcher option: stays in launcher_settings.json
        self.assertNotIn("party_items", s)
        self.assertEqual(s["ini_sync_rev"], L.INI_SYNC_REV)
        self.assertTrue(written)
        self.assertIsNone(L.migrate_settings_to_ini(s, self.ini))
        # 0.7.1's keys were never in launcher_settings.json: nothing written for them
        self.assertNotIn("party_kill_record", ini)
        self.assertNotIn("party_key_view", ini)

    def test_write_env_migrates(self):
        L = self.L
        settings = Path(self.tmp.name) / "launcher_settings.json"
        settings.write_text(json.dumps({"feat_upscaler": False}), encoding="utf-8")
        with mock.patch.dict(os.environ, {"BB_CONFIG": str(self.ini)}):
            L.write_env_bat(Path(self.tmp.name) / "env.bat", settings)
        self.assertEqual(L.read_ini(self.ini)["upscaler"], "off")
        self.assertEqual(json.loads(settings.read_text(encoding="utf-8"))["ini_sync_rev"], L.INI_SYNC_REV)
        self.assertNotIn("BB_UPSCALER", (Path(self.tmp.name) / "env.bat").read_text())


class OffscreenWindow(unittest.TestCase):
    """The real window, invisible (alpha 0), with a test bbport.ini and launcher_settings.json."""

    # One window for the class: a second Tk interpreter in one process fails on some Pythons.
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
            import tkinter  # noqa: F401
        except Exception as ex:
            raise unittest.SkipTest(f"no Tk here: {ex}")
        self = cls
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.ini = root / "bbport.ini"
        self.ini.write_text("upscaler=fsr4\nshow_hud=1\ngpl=1\nvolumetric_res=2\nskip_intro=0\n"
                            "effect_motion_blur=0\nkey.cross=X\n", encoding="utf-8")
        self.settings = root / "launcher_settings.json"
        self.settings.write_text(json.dumps({"ini_sync_rev": 1, "check_updates": False}), encoding="utf-8")
        L = self.L
        self.patches = [mock.patch.dict(os.environ, {"BB_CONFIG": str(self.ini), "BB_DATA_DIR": str(root)}),
                        mock.patch.object(L, "SETTINGS_FILE", self.settings),
                        mock.patch.object(L, "LOG_FILE", root / "launcher.log"),
                        mock.patch.object(L.BloodborneLauncher, "start_update_check", lambda self: None),
                        mock.patch.object(L.BloodborneLauncher, "check_last_party_session", lambda self: None)]
        for p in self.patches:
            p.start()
        build_style = L.BloodborneLauncher.build_style

        def hidden_style(win):
            win.attributes("-alpha", 0.0)
            build_style(win)
        self.patches.append(mock.patch.object(L.BloodborneLauncher, "build_style", hidden_style))
        self.patches[-1].start()
        try:
            self.win = L.BloodborneLauncher()
        except Exception as ex:
            cls.tearDownClass(destroy=False)
            raise unittest.SkipTest(f"no Tk window here: {ex}")

    @classmethod
    def tearDownClass(cls, destroy=True):
        try:
            if destroy:
                cls.win.destroy()
        finally:
            for p in reversed(cls.patches):
                p.stop()
            cls.tmp.cleanup()

    def test_shows_and_writes_bbport_ini(self):
        w, L = self.win, self.L
        self.assertTrue(w.feat_upscaler.get())
        self.assertEqual(w.upscaler_var.get(), "FSR 4")
        self.assertEqual(str(w.upscaler_combo.cget("state")), "readonly")
        self.assertTrue(w.feat_hud.get())
        self.assertTrue(w.feat_gpl.get())
        self.assertFalse(w.feat_mouse_keyboard.get())
        self.assertEqual(w.volumetric_var.get(), "Full resolution")
        self.assertFalse(w.feat_skip_intro.get())
        self.assertTrue(w.feat_no_blur.get())
        # unchecking writes off and remembers the choice; re-checking brings it back
        w.feat_upscaler.set(False)
        w.save_upscaler()
        self.assertEqual(L.read_ini(self.ini)["upscaler"], "off")
        self.assertEqual(str(w.upscaler_combo.cget("state")), "disabled")
        self.assertEqual(json.loads(self.settings.read_text(encoding="utf-8"))["upscaler_choice"], "FSR 4")
        w.feat_upscaler.set(True)
        w.upscaler_var.set("DLSS")
        w.save_upscaler()
        self.assertEqual(L.read_ini(self.ini)["upscaler"], "dlss")
        w.feat_skip_intro.set(True)
        w.on_quick_patch_toggle()
        ini = L.read_ini(self.ini)
        self.assertEqual((ini["skip_intro"], ini["effect_motion_blur"], ini["key.cross"]), ("1", "0", "X"))
        saved = json.loads(self.settings.read_text(encoding="utf-8"))
        self.assertNotIn("Skip Intro", saved["enabled_patches"])
        self.assertNotIn("feat_upscaler", saved)
        # the game changes bbport.ini, then exits: the launcher shows its choices
        L.update_ini(self.ini, {"upscaler": "taa", "show_hud": "0", "mk_enabled": "1", "volumetric_res": "0"})
        w.refresh_game_settings(force=True)
        self.assertEqual((w.feat_upscaler.get(), w.upscaler_var.get()), (True, "TAA"))
        self.assertFalse(w.feat_hud.get())
        self.assertTrue(w.feat_mouse_keyboard.get())
        self.assertEqual(w.volumetric_var.get(), "Game default (half)")
        self.assertNotIn("BB_UPSCALER", L.settings_env(w.current_settings()))

    def test_dlss_row_fits_at_min_width(self):
        w = self.win
        w.geometry("720x680")
        w.notebook.select([t for t in w.notebook.tabs() if w.notebook.tab(t, "text").strip() == "Graphics"][0])
        statuses = ["bundled", "bundled + FG", "your own", "your own + FG", "none (FSR used)",
                    "your own; frame generation runtime found and a very long text " * 3]
        spots = set()
        for status in statuses:
            w.dlss_var.set(status)
            w.update()
            spots.add((w.dlss_choose_btn.winfo_rootx(), w.dlss_remove_btn.winfo_rootx()))
            card = w.dlss_choose_btn.master.master  # dlss_row -> its section card
            card_right = card.winfo_rootx() + card.winfo_width()
            for btn in (w.dlss_choose_btn, w.dlss_remove_btn):
                self.assertTrue(btn.winfo_ismapped(), status)
                self.assertLessEqual(btn.winfo_rootx() + btn.winfo_width(), card_right, status)
                self.assertLessEqual(btn.winfo_rootx() + btn.winfo_width(), w.winfo_rootx() + 720, status)
        self.assertEqual(len(spots), 1, spots)  # the buttons never move
        # the real status is short: label, buttons and status fit the card
        w.update_dlss_status()
        w.update()
        self.assertLessEqual(len(w.dlss_var.get()), len("none (FSR used)"))
        lbl = w.dlss_status_lbl
        self.assertLessEqual(lbl.winfo_rootx() + lbl.winfo_width(), card.winfo_rootx() + card.winfo_width())


if __name__ == "__main__":
    unittest.main()
