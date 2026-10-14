#!/usr/bin/env python3
"""Main application controller and fast-boot entry point for Bloodborne PC Launcher."""

import os
import sys
from pathlib import Path

if len(sys.argv) > 1 and any(arg.lower().endswith(".py") for arg in sys.argv[1:]):
    import runpy

    idx = next(i for i, arg in enumerate(sys.argv[1:], 1) if arg.lower().endswith(".py"))
    target_script = str(Path(sys.argv[idx]).resolve())
    sys.argv = [target_script] + sys.argv[idx + 1:]
    sys.path.insert(0, str(Path(target_script).parent))
    runpy.run_path(target_script, run_name="__main__")
    sys.exit(0)
import json
from datetime import datetime
from PySide6.QtCore import QEvent, QTimer
from PySide6.QtGui import QTextCursor
from PySide6.QtWidgets import QApplication, QFileDialog, QMainWindow, QMessageBox

from bblauncher_UI import STYLE_SHEET, LauncherUI, apply_dwm_styling, install_dialog_styling
from bblauncher_Utils import (
    ANISO_CHOICES,
    APP_DIR,
    DEFAULT_GAME,
    DEFAULT_SETTINGS,
    LOG_FILE,
    RUN_BAT,
    SETTINGS_FILE,
    VK_NOISE,
    ProcessWorker,
    acquire_single_instance,
    get_bb_guard,
    load_xml_patches,
    resolve_game_dir,
    rotate_log,
    running_instances,
    shader_cache_dir,
    verify_prelaunch,
)


class BloodborneLauncher(LauncherUI, QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Bloodborne PC Launcher")
        self.setFixedSize(1000, 720)
        self.setStyleSheet(STYLE_SHEET)

        self.worker = None
        self.log_fh = None
        self.hidden_count = 0

        self._patches_built = True
        self._settings_built = True
        self.all_xml_patches = load_xml_patches()
        self.patch_checkboxes = {}

        self.settings = self.load_settings()
        self.enabled_patches = set(self.settings.get("enabled_patches", DEFAULT_SETTINGS["enabled_patches"]))

        self.init_ui()
        self.log("Ready. Select your eboot.bin and press Launch Bloodborne.")

        self._last_ini_mtime = 0
        try:
            ini_file = APP_DIR / "bbport.ini"
            if ini_file.is_file():
                self._last_ini_mtime = ini_file.stat().st_mtime
        except OSError:
            pass
        self.ini_sync_timer = QTimer(self)
        self.ini_sync_timer.setInterval(1000)
        self.ini_sync_timer.timeout.connect(self.sync_bbport_ini_to_ui)
        self.ini_sync_timer.start()

    def showEvent(self, event):
        super().showEvent(event)
        if not getattr(self, "_dwm_applied", False):
            self._dwm_applied = True
            apply_dwm_styling(int(self.winId()))

    def changeEvent(self, event):
        if event.type() == QEvent.ActivationChange and self.isActiveWindow():
            self.sync_bbport_ini_to_ui()
        super().changeEvent(event)

    def _build_patches_tab_now(self):
        if self._patches_built:
            return
        self.all_xml_patches = load_xml_patches()
        patches_widget = self.create_patches_tab()
        self.stack.removeWidget(self.patches_placeholder)
        self.patches_placeholder.deleteLater()
        self.stack.insertWidget(1, patches_widget)
        self._patches_built = True

    def _build_settings_tab_now(self):
        if self._settings_built:
            return
        settings_widget = self.create_settings_tab()
        self.stack.removeWidget(self.settings_placeholder)
        self.settings_placeholder.deleteLater()
        self.stack.insertWidget(2, settings_widget)
        self._settings_built = True

    def load_settings(self) -> dict:
        merged = dict(DEFAULT_SETTINGS)
        try:
            if SETTINGS_FILE.is_file():
                loaded = json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
                if isinstance(loaded, dict):
                    merged.update(loaded)
        except Exception:
            pass
        return merged

    def current_config(self) -> dict:
        if self._settings_built:
            win_mode = self.cmb_win_mode.currentText()
            return {
                "eboot": self.txt_eboot.text().strip(),
                "fps": self.cmb_fps.currentText(),
                "res": self.cmb_res.currentText(),
                "aniso": self.cmb_aniso.currentText(),
                "mods": self.chk_mods.isChecked(),
                "hide_vk": self.chk_vk_noise.isChecked(),
                "feat_upscaler": self.chk_fsr.isChecked(),
                "feat_overlay": self.chk_overlay.isChecked(),
                "feat_hud": self.chk_hud.isChecked(),
                "feat_fps_patch": self.chk_fps_patch.isChecked(),
                "feat_res_scaling": self.chk_res_scale.isChecked(),
                "window_mode": win_mode,
                "fullscreen": win_mode == "Fullscreen",
                "enabled_patches": sorted(list(self.enabled_patches)),
            }
        self.settings["eboot"] = self.txt_eboot.text().strip()
        self.settings["hide_vk"] = self.chk_vk_noise.isChecked()
        self.settings["enabled_patches"] = sorted(list(self.enabled_patches))
        return self.settings

    def save_settings(self):
        data = self.current_config()
        self.settings = data
        try:
            SETTINGS_FILE.write_text(json.dumps(data, indent=2), encoding="utf-8")
        except OSError:
            pass

    def load_bbport_ini(self) -> dict:
        ini_path = APP_DIR / "bbport.ini"
        values = {}
        if ini_path.is_file():
            try:
                for line in ini_path.read_text(encoding="utf-8").splitlines():
                    trimmed = line.strip()
                    if trimmed and not trimmed.startswith("#") and "=" in trimmed:
                        k, v = trimmed.split("=", 1)
                        values[k.strip()] = v.strip()
            except OSError:
                pass
        return values

    def save_bbport_ini(self):
        if not getattr(self, "_settings_built", False):
            return
        ini_path = APP_DIR / "bbport.ini"
        keys_to_update = {
            "mk_enabled": "1" if self.chk_mk_enabled.isChecked() else "0",
            "mk_sens_x": f"{self.sl_sens_x.value() / 100:.2f}",
            "mk_sens_y": f"{self.sl_sens_y.value() / 100:.2f}",
            "mk_invert_x": "1" if self.chk_inv_x.isChecked() else "0",
            "mk_invert_y": "1" if self.chk_inv_y.isChecked() else "0",
            "mk_deadzone": f"{self.sl_dz.value() / 100:.2f}",
            "mk_smoothing": f"{self.sl_smoothing.value() / 100:.2f}",
        }
        if hasattr(self, "keybind_buttons"):
            for k, btn in self.keybind_buttons.items():
                keys_to_update[k] = btn.current_binding

        lines = []
        written = set()
        if ini_path.is_file():
            try:
                for line in ini_path.read_text(encoding="utf-8").splitlines():
                    trimmed = line.strip()
                    if trimmed and not trimmed.startswith("#") and "=" in trimmed:
                        k, _ = trimmed.split("=", 1)
                        k = k.strip()
                        if k in keys_to_update:
                            lines.append(f"{k}={keys_to_update[k]}")
                            written.add(k)
                            continue
                    lines.append(line)
            except OSError:
                pass

        for k, v in keys_to_update.items():
            if k not in written:
                lines.append(f"{k}={v}")

        try:
            ini_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            self._last_ini_mtime = ini_path.stat().st_mtime
            self.log("[M&K] Settings updated successfully in bbport.ini")
        except OSError as ex:
            self.log(f"[ERROR] Could not write to {ini_path}: {ex}")

    def on_keybind_button_assigned(self, action_key: str, new_val: str):
        if not hasattr(self, "keybind_buttons"):
            return
        # Conflict detection: if another action already has this binding, swap with previous binding
        old_val = self.keybind_buttons[action_key].current_binding
        if new_val != "None":
            for other_key, btn in self.keybind_buttons.items():
                if other_key != action_key and btn.current_binding.lower() == new_val.lower():
                    btn.set_binding(old_val)
                    self.log(f"[Keybinds] Swapped '{new_val}' between {action_key} and {other_key}")
                    break
        self.save_bbport_ini()

    def reset_keybinds_to_defaults(self):
        from bblauncher_Utils import DEFAULT_KEYBINDS
        if not hasattr(self, "keybind_buttons"):
            return
        for ini_key, _, def_val in DEFAULT_KEYBINDS:
            if ini_key in self.keybind_buttons:
                self.keybind_buttons[ini_key].set_binding(def_val)
        self.save_bbport_ini()
        self.log("[Keybinds] Reset all keybinds to defaults.")

    def sync_bbport_ini_to_ui(self):
        if not getattr(self, "_settings_built", False):
            return
        ini_path = APP_DIR / "bbport.ini"
        if not ini_path.is_file():
            return
        try:
            mtime = ini_path.stat().st_mtime
            if getattr(self, "_last_ini_mtime", None) == mtime:
                return
            self._last_ini_mtime = mtime
            ini_opts = self.load_bbport_ini()
            widgets = [self.sl_sens_x, self.sl_sens_y, self.sl_dz, self.sl_smoothing,
                       self.chk_mk_enabled, self.chk_inv_x, self.chk_inv_y]
            for w in widgets:
                w.blockSignals(True)

            self.chk_mk_enabled.setChecked(ini_opts.get("mk_enabled", "1") != "0")

            sx = int(float(ini_opts.get("mk_sens_x", "1.50")) * 100)
            self.sl_sens_x.setValue(max(10, min(1000, sx)))
            self.lbl_sens_x_val.setText(f"{self.sl_sens_x.value() / 100:.2f}")

            sy = int(float(ini_opts.get("mk_sens_y", "1.50")) * 100)
            self.sl_sens_y.setValue(max(10, min(1000, sy)))
            self.lbl_sens_y_val.setText(f"{self.sl_sens_y.value() / 100:.2f}")

            dz = int(float(ini_opts.get("mk_deadzone", "0.13")) * 100)
            self.sl_dz.setValue(max(0, min(35, dz)))
            self.lbl_dz_val.setText(f"{self.sl_dz.value() / 100:.2f}")

            sm = int(float(ini_opts.get("mk_smoothing", "0.00")) * 100)
            self.sl_smoothing.setValue(max(0, min(80, sm)))
            self.lbl_smoothing_val.setText(f"{self.sl_smoothing.value() / 100:.2f}")

            self.chk_inv_x.setChecked(ini_opts.get("mk_invert_x", "0") != "0")
            self.chk_inv_y.setChecked(ini_opts.get("mk_invert_y", "0") != "0")

            for w in widgets:
                w.blockSignals(False)

            if hasattr(self, "keybind_buttons"):
                for k, btn in self.keybind_buttons.items():
                    if k in ini_opts:
                        btn.set_binding(ini_opts[k])
        except Exception:
            pass


    def on_patch_toggled(self, name: str, state: int):
        if state:
            self.enabled_patches.add(name)
        else:
            self.enabled_patches.discard(name)
        self.update_patch_count_label()
        self.save_settings()

    def update_patch_count_label(self):
        if self._patches_built:
            self.lbl_patch_count.setText(f"{len(self.enabled_patches)} / {len(self.all_xml_patches)} enabled")

    def sync_patches_to_ui(self):
        if not self._patches_built:
            return
        for name, cb in self.patch_checkboxes.items():
            cb.blockSignals(True)
            cb.setChecked(name in self.enabled_patches)
            cb.blockSignals(False)
        self.update_patch_count_label()

    def filter_patches(self):
        if not self._patches_built:
            return
        q = self.txt_patch_search.text().strip().lower()
        selected_cat = self.cmb_patch_cat.currentText()

        self.patch_list_widget.setUpdatesEnabled(False)
        for i in range(self.patch_list_layout.count() - 1):
            widget = self.patch_list_layout.itemAt(i).widget()
            if not widget:
                continue
            w_cat = widget.property("patch_category")
            cat_match = (selected_cat == "All Categories") or (w_cat == selected_cat)
            query_match = not q or (q in widget.property("patch_search_blob"))
            widget.setVisible(cat_match and query_match)
        self.patch_list_widget.setUpdatesEnabled(True)

    def apply_recommended_patches(self):
        self.enabled_patches = {
            "Skip Intro",
            "Performance Patch (perf increase)",
            "Disable Motion Blur (perf increase)",
        }
        self.sync_patches_to_ui()
        self.save_settings()
        self.log("[PATCHES] Recommended defaults applied.")

    def log(self, text: str):
        self.log_batch([text])

    def log_batch(self, lines: list[str]):
        if not lines:
            return
        if self.log_fh:
            try:
                self.log_fh.write("\n".join(lines) + "\n")
                self.log_fh.flush()
            except OSError:
                pass

        if self.chk_vk_noise.isChecked():
            filtered = [ln for ln in lines if VK_NOISE not in ln]
            self.hidden_count += len(lines) - len(filtered)
            lines = filtered

        if lines:
            self.log_view.appendPlainText("\n".join(lines))
            self.log_view.moveCursor(QTextCursor.End)

    def copy_log(self):
        QApplication.clipboard().setText(self.log_view.toPlainText())

    def clear_log(self):
        self.log_view.clear()

    def browse_eboot(self):
        start = self.txt_eboot.text().strip() or str(DEFAULT_GAME if DEFAULT_GAME.exists() else APP_DIR)
        path, _ = QFileDialog.getOpenFileName(
            self, "Select Bloodborne eboot.bin", start, "PS4 Executable (*.bin);;All Files (*)"
        )
        if path:
            self.txt_eboot.setText(os.path.normpath(path))
            self.log(f"Selected executable: {path}")

    def manual_verify_setup(self):
        target = self.txt_eboot.text().strip()
        if not target:
            QMessageBox.critical(self, "Error", "Specify the path to eboot.bin first.")
            return

        game_dir = resolve_game_dir(target)
        if not game_dir:
            QMessageBox.critical(self, "Invalid Path", f"Could not find eboot.bin in:\n{target}")
            return

        self.log("--- Running Pre-Launch Verification Check ---")
        ready, err, details = verify_prelaunch(game_dir)
        self.log_batch([f"  {line}" for line in details])

        if ready:
            self.log("[PRE-LAUNCH] All checks passed.")
            QMessageBox.information(self, "Verification Passed", "System ready.\n\n" + "\n".join(details))
        else:
            self.log(f"[PRE-LAUNCH ERROR] {err}")
            QMessageBox.critical(self, "Verification Failed", err)

    def reset_shader_cache(self):
        import shutil

        if self.worker and self.worker.isRunning():
            QMessageBox.warning(self, "Busy", "Terminate game execution before resetting the shader cache.")
            return

        cache_dir = shader_cache_dir()
        files = [p for p in cache_dir.rglob("*") if p.is_file()] if cache_dir.is_dir() else []
        if not files:
            QMessageBox.information(self, "Cache Clear", "The shader cache is already empty.")
            return

        confirm = QMessageBox.question(
            self,
            "Reset Shader Cache",
            f"Delete {len(files)} compiled shader cache files in {cache_dir}?",
            QMessageBox.Yes | QMessageBox.No,
        )
        if confirm == QMessageBox.Yes:
            try:
                shutil.rmtree(cache_dir)
                self.log(f"[CACHE] Shader cache purged ({len(files)} files removed).")
            except OSError as ex:
                QMessageBox.critical(self, "Error", f"Could not purge cache directory: {ex}")

    def blocked_by_running_instance(self) -> bool:
        found = running_instances()
        if not found:
            return False
        guard = get_bb_guard()
        lines = "\n".join(guard.describe(i) for i in found)
        self.log(f"[PRE-LAUNCH] bbport.exe is already running ({lines.replace(chr(10), '; ')})")
        reply = QMessageBox.warning(
            self,
            "Bloodborne is already running",
            f"bbport.exe is still running:\n\n{lines}\n\nTerminate it now and continue?",
            QMessageBox.Yes | QMessageBox.No,
        )
        if reply != QMessageBox.Yes:
            self.log("[PRE-LAUNCH] Launch cancelled: close the running bbport.exe first.")
            return True
        survivors = [i.pid for i in found if not guard.terminate(i.pid)]
        if survivors:
            self.log(f"[PRE-LAUNCH ERROR] Could not terminate PID {', '.join(map(str, survivors))}")
            QMessageBox.critical(
                self,
                "Cannot terminate bbport.exe",
                f"PID {', '.join(map(str, survivors))} did not exit.\nEnd it in Task Manager, then launch again.",
            )
            return True
        self.log("[PRE-LAUNCH] Old bbport.exe terminated.")
        return False

    def launch_game(self):
        if self.worker and self.worker.isRunning():
            return

        if self.blocked_by_running_instance():
            return

        cfg = self.current_config()
        target = cfg["eboot"]
        if not target:
            QMessageBox.critical(self, "Error", "Select eboot.bin or the game directory first.")
            return

        game_dir = resolve_game_dir(target)
        if not game_dir:
            QMessageBox.critical(self, "Invalid Path", f"eboot.bin not found in:\n{target}")
            return

        ready, err, _ = verify_prelaunch(game_dir)
        if not ready:
            QMessageBox.critical(self, "Verification Failed", err)
            return

        env = os.environ.copy()
        if not getattr(sys, "frozen", False):
            env["PYTHON"] = sys.executable
        env["BB_GAME_DIR"] = str(game_dir)
        env["BB_FPS"] = cfg["fps"]
        env["BB_MODS_ENABLED"] = "1" if cfg["mods"] else "0"

        if not cfg["feat_upscaler"]:
            env["BB_UPSCALER"] = "off"
        else:
            env.pop("BB_UPSCALER", None)

        if not cfg["feat_overlay"]:
            env["BB_OVERLAY"] = "0"
        else:
            env.pop("BB_OVERLAY", None)

        if cfg["feat_hud"]:
            env["BB_HUD"] = "1"
        else:
            env.pop("BB_HUD", None)

        if not cfg["feat_fps_patch"]:
            env["BB_FPS_PATCH"] = "0"
        else:
            env.pop("BB_FPS_PATCH", None)

        env.pop("BB_RENDER_RES", None)
        res_choice = cfg["res"]
        if cfg["feat_res_scaling"]:
            if res_choice not in ("1920x1080 (Native)", "Default (1080p)", "1920x1080"):
                env["BB_RENDER_RES"] = res_choice

        win_mode_val = cfg["window_mode"].lower()
        env["BB_WINDOW_MODE"] = win_mode_val
        env["BB_FULLSCREEN"] = "1" if win_mode_val != "windowed" else "0"
        env.setdefault("BB_FRAMES_AHEAD", "2")

        aniso_mapping = {c[0]: c[1] for c in ANISO_CHOICES}
        env["BB_ANISO"] = aniso_mapping.get(cfg["aniso"], "16")

        active_patches = cfg["enabled_patches"]
        if active_patches:
            env["BB_PATCHES"] = ";".join(active_patches)
        else:
            env.pop("BB_PATCHES", None)

        rotate_log()
        try:
            self.log_fh = open(LOG_FILE, "w", encoding="utf-8")
        except OSError:
            self.log_fh = None

        self.hidden_count = 0
        self.log_batch([
            "=" * 60,
            f"Starting Bloodborne from: {game_dir}  [{datetime.now():%Y-%m-%d %H:%M:%S}]",
            f"Target FPS: {cfg['fps']} | Res: {res_choice} | Window: {win_mode_val}",
            f"Active XML Patches ({len(active_patches)}): {', '.join(active_patches) if active_patches else 'None'}",
            "=" * 60,
        ])

        cmd = ["cmd.exe", "/d", "/c", RUN_BAT.name] if os.name == "nt" else [str(RUN_BAT)]

        self.save_settings()
        if self._settings_built:
            self.save_bbport_ini()

        self.worker = ProcessWorker(cmd, env, APP_DIR)
        self.worker.batch_emitted.connect(self.log_batch)
        self.worker.process_finished.connect(self.on_process_finished)
        self.worker.start()

        self.btn_launch.setEnabled(False)
        self.btn_stop.setEnabled(True)
        self.lbl_status.setText("\u25cf Game Running")
        self.lbl_status.setStyleSheet("color: #4ade80; font-size: 11px; padding: 0 20px; background: transparent;")

    def stop_game(self):
        if self.worker and self.worker.isRunning():
            self.log("Terminating process tree...")
            self.worker.terminate_proc()

    def on_process_finished(self, rc: int):
        if self.hidden_count:
            self.log(f"({self.hidden_count} Vulkan warning lines hidden)")
        self.log(f"Process exited with code: {rc}")

        guard = get_bb_guard()
        if guard:
            for instance in running_instances():
                self.log(
                    f"[WARNING] bbport.exe is still running after the game ended "
                    f"({guard.describe(instance)}); the next launch will ask to terminate it."
                )

        if self.log_fh:
            try:
                self.log_fh.close()
            except OSError:
                pass
            self.log_fh = None

        self.btn_launch.setEnabled(True)
        self.btn_stop.setEnabled(False)
        self.lbl_status.setText("\u25cf Engine Idle")
        self.lbl_status.setStyleSheet("color: #6a6a78; font-size: 11px; padding: 0 20px; background: transparent;")

    def closeEvent(self, event):
        if self.worker and self.worker.isRunning():
            reply = QMessageBox.question(
                self,
                "Quit Confirmation",
                "Bloodborne is actively executing. Terminate process and exit?",
                QMessageBox.Yes | QMessageBox.No,
            )
            if reply != QMessageBox.Yes:
                event.ignore()
                return
            self.worker.terminate_proc()
        self.save_settings()
        event.accept()


if __name__ == "__main__":
    if not acquire_single_instance():
        app = QApplication(sys.argv)
        install_dialog_styling(app)
        QMessageBox.information(None, "Bloodborne PC Launcher", "The launcher is already open.")
        sys.exit(0)

    app = QApplication(sys.argv)
    install_dialog_styling(app)
    window = BloodborneLauncher()
    window.show()
    sys.exit(app.exec())