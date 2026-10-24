"""The launcher's party settings tabs (0.7.3, launchtabs): the Party tab holds hosting / joining /
connection; Party Rules holds what the host decides for the party (party_balance Rules, sent to every
guest); My Party what each player chooses for itself. Every party setting has exactly one row.
python -m pytest tests/test_launcher_party_tabs.py"""
from paths import ROOT
import importlib.util
import json
import os
import re
import tempfile
import unittest
from pathlib import Path
from unittest import mock


def load_launcher():
    spec = importlib.util.spec_from_file_location("bb_launcher_party_tabs", ROOT / "launcher.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def host_rule_keys() -> set:
    """The host's on/off rules: the bool members of party_balance Rules, as bbport.ini keys."""
    src = (ROOT / "gpu" / "shim" / "party" / "party_balance_rules.h").read_text(encoding="utf-8")
    start = src.index("struct Rules {")
    body = src[start:src.index("};", start)]
    return {"party_" + name for name in re.findall(r"^\s*bool (\w+) = ", body, re.M)}


def rows(table):
    for _, _, title, section_rows in table:
        for row in section_rows:
            yield title, row


class Tables(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    @staticmethod
    def keys(table):
        return [row[1] for _, row in rows(table) if row[0] == "check"]

    def test_every_party_setting_once(self):
        L = self.L
        every = self.keys(L.PARTY_RULES_TAB) + self.keys(L.MY_PARTY_TAB)
        self.assertEqual(len(every), len(set(every)), "a key in two rows")
        expected = ({k for k, _, _ in L.PARTY_SWITCHES + L.PARTY_INI_SWITCHES}
                    | {k for k, _ in L.PARTY_INI_FLAGS})
        self.assertEqual(set(every), expected)
        combos = [row[1] for _, row in rows(L.MY_PARTY_TAB) if row[0] == "combo"]
        self.assertEqual(sorted(combos), sorted(["party_start_var", "party_backup_minutes_var",
                                                 "party_backup_keep_var"]))
        # 0.7.5: Insight for bosses is the host's rule (join check), so on the Party Rules tab
        self.assertEqual([row[1] for _, row in rows(L.PARTY_RULES_TAB) if row[0] == "combo"],
                         ["party_insight_var"])

    def test_host_rules_tab_is_exactly_the_hosts_rules(self):
        """A new bool in party_balance Rules needs its row in PARTY_RULES_TAB (and never in My Party)."""
        L = self.L
        self.assertEqual(set(self.keys(L.PARTY_RULES_TAB)), host_rule_keys())
        self.assertFalse(set(self.keys(L.MY_PARTY_TAB)) & host_rule_keys())

    def test_needs_point_at_a_box_on_the_same_tab(self):
        L = self.L
        for table in (L.PARTY_RULES_TAB, L.MY_PARTY_TAB):
            keys = self.keys(table)
            for _, row in rows(table):
                if row[0] == "check" and len(row) > 4:
                    self.assertIn(row[4], keys, row[1])

    def test_in_game_paths_name_the_pages(self):
        """Each section is a page of the game's System menu (Party rules: / My party:), named in its tips."""
        L = self.L
        src = (ROOT / "gpu" / "shim" / "bbport_game_menu_pages.cpp").read_text(encoding="utf-8")
        pages = set(re.findall(r'AddPage\("([^"]+)"', src))
        for table, prefix in ((L.PARTY_RULES_TAB, "Party rules: "), (L.MY_PARTY_TAB, "My party: ")):
            for title, row in rows(table):
                if title == "Start & saves":
                    continue  # launch options, not in the game's menus
                tip = row[4] if row[0] == "combo" else row[3]
                self.assertIn(prefix + title, pages, (title, row[1]))
                self.assertIn("System > " + prefix + title, tip, row[1])

    def test_no_dialog_any_more(self):
        src = (ROOT / "launcher.py").read_text(encoding="utf-8")
        self.assertNotIn("open_party_rules_dialog", src)
        self.assertNotIn("Rules & Sync", src)


class Window(unittest.TestCase):
    """The real window, invisible (alpha 0), on a test bbport.ini and launcher_settings.json."""

    @classmethod
    def setUpClass(cls):
        try:
            cls.L = load_launcher()
            import tkinter  # noqa: F401
        except Exception as ex:
            raise unittest.SkipTest(f"no Tk here: {ex}")
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        cls.ini = root / "bbport.ini"
        cls.ini.write_text("party_rot=1\nsettings_rev=1\nparty_toasts=0\n", encoding="utf-8")
        cls.settings = root / "launcher_settings.json"
        cls.settings.write_text(json.dumps({"ini_sync_rev": 2, "check_updates": False}), encoding="utf-8")
        L = cls.L
        cls.patches = [mock.patch.dict(os.environ, {"BB_CONFIG": str(cls.ini), "BB_DATA_DIR": str(root)}),
                       mock.patch.object(L, "SETTINGS_FILE", cls.settings),
                       mock.patch.object(L, "LOG_FILE", root / "launcher.log"),
                       mock.patch.object(L.BloodborneLauncher, "start_update_check", lambda self: None),
                       mock.patch.object(L.BloodborneLauncher, "check_last_party_session", lambda self: None)]
        build_style = L.BloodborneLauncher.build_style

        def hidden_style(win):
            win.attributes("-alpha", 0.0)
            build_style(win)
        cls.patches.append(mock.patch.object(L.BloodborneLauncher, "build_style", hidden_style))
        for p in cls.patches:
            p.start()
        try:
            cls.win = L.BloodborneLauncher()
        except Exception as ex:
            for p in reversed(cls.patches):
                p.stop()
            raise unittest.SkipTest(f"no Tk window here: {ex}")

    @classmethod
    def tearDownClass(cls):
        try:
            cls.win.destroy()
        finally:
            for p in reversed(cls.patches):
                p.stop()
            cls.tmp.cleanup()

    def texts(self, widget):
        out = []
        for c in widget.winfo_children():
            try:
                out.append(str(c.cget("text")))
            except Exception:
                pass
            out += self.texts(c)
        return out

    def tab(self, title):
        nb = self.win.notebook
        return nb.nametowidget([t for t in nb.tabs() if nb.tab(t, "text").strip() == title][0])

    def test_tabs_and_their_rows(self):
        L, w = self.L, self.win
        titles = [w.notebook.tab(t, "text").strip() for t in w.notebook.tabs()]
        at = titles.index("Party")
        self.assertEqual(titles[at:at + 3], ["Party", "Party Rules", "My Party"])
        party = self.texts(self.tab("Party"))
        for table in (L.PARTY_RULES_TAB, L.MY_PARTY_TAB):
            for _, row in rows(table):
                self.assertNotIn(row[2], party, "a party setting on the Party tab")
        self.assertNotIn("Rules & sync...", party)
        for title, table in (("Party Rules", L.PARTY_RULES_TAB), ("My Party", L.MY_PARTY_TAB)):
            shown = self.texts(self.tab(title))
            for _, row in rows(table):
                self.assertIn(row[2], shown)
        self.assertIn(L.PARTY_RULES_TAB_INTRO, self.texts(self.tab("Party Rules")))

    def test_rows_read_and_write_bbport_ini(self):
        L, w = self.L, self.win
        rules = self.tab("Party Rules").party_checks
        mine = self.tab("My Party").party_checks
        # read: the test ini's values
        self.assertTrue(rules["party_rot"][1].get())
        self.assertFalse(mine["party_toasts"][1].get())
        # a one-key flag (PARTY_INI_FLAGS) and a party rules switch: written at once, same keys
        mine["party_toasts"][0].invoke()
        self.assertEqual(L.read_ini(self.ini)["party_toasts"], "1")
        cb = rules["party_friendly_fire"][0]
        cb.invoke()
        self.assertEqual(L.read_ini(self.ini)["party_friendly_fire"], "1")
        cb.invoke()
        self.assertEqual(L.read_ini(self.ini)["party_friendly_fire"], "0")
        # a needs box: the rot cleanses grey out while rot is off
        cb = rules["party_rot"][0]
        cb.invoke()
        self.assertEqual(L.read_ini(self.ini)["party_rot"], "0")
        self.assertIn("disabled", rules["party_rot_cleanse_boss"][0].state())
        cb.invoke()
        self.assertNotIn("disabled", rules["party_rot_cleanse_boss"][0].state())
        # the game changes bbport.ini: the tabs follow
        L.update_ini(self.ini, {"party_glow_off": "0", "party_story": "0"})
        w.refresh_game_settings(force=True)
        self.assertFalse(mine["party_glow_off"][1].get())

    def test_tab_bar_fits(self):
        """The tab bar fits the window's width: no tab title is cut."""
        w = self.win
        w.update_idletasks()
        self.assertLessEqual(w.notebook.winfo_reqwidth(), 780 - 2 * 16)


if __name__ == "__main__":
    unittest.main()
