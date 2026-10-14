#!/usr/bin/env python3
"""User Interface layout, lazy tab builders, and stylesheet for Bloodborne PC Launcher."""

import ctypes
import os
from pathlib import Path
from PySide6.QtCore import QEvent, QObject, Qt, Signal
from PySide6.QtGui import QKeyEvent, QMouseEvent
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPlainTextEdit,
    QPushButton,
    QScrollArea,
    QSlider,
    QStackedWidget,
    QVBoxLayout,
    QWidget,
)

from bblauncher_Utils import (
    ANISO_CHOICES,
    DEFAULT_GAME,
    DEFAULT_KEYBINDS,
    FPS_CHOICES,
    KEYBINDINGS,
    RES_CHOICES,
    WINDOW_MODE_CHOICES,
)


class KeybindButton(QPushButton):
    key_assigned = Signal(str, str)  # (action_key, new_binding)

    def __init__(self, action_key: str, binding_text: str, parent=None):
        super().__init__(binding_text, parent)
        self.action_key = action_key
        self.current_binding = binding_text
        self.listening = False
        self.setObjectName("KeybindBtn")
        self.setFixedHeight(30)
        self.setFocusPolicy(Qt.StrongFocus)
        self.clicked.connect(self.start_listening)
        self.update_style()

    def update_style(self):
        if self.listening:
            self.setText("< Press Key / Click >")
            self.setStyleSheet("""
                QPushButton#KeybindBtn {
                    background-color: #2b2510;
                    border: 2px solid #d1b87a;
                    border-radius: 4px;
                    color: #ffd700;
                    font-weight: 700;
                    font-size: 12px;
                }
            """)
        else:
            self.setText(self.current_binding if self.current_binding else "None")
            self.setStyleSheet("""
                QPushButton#KeybindBtn {
                    background-color: #1a1a24;
                    border: 1px solid #363645;
                    border-radius: 4px;
                    color: #d1b87a;
                    font-weight: 600;
                    font-size: 12px;
                    padding: 0 10px;
                }
                QPushButton#KeybindBtn:hover {
                    background-color: #242433;
                    border-color: #d1b87a;
                }
            """)

    def set_binding(self, text: str):
        self.current_binding = text
        self.listening = False
        self.update_style()

    def start_listening(self):
        self.listening = True
        self.update_style()
        self.grabKeyboard()
        self.grabMouse()

    def stop_listening(self):
        if self.listening:
            self.listening = False
            self.releaseKeyboard()
            self.releaseMouse()
            self.update_style()

    def keyPressEvent(self, event: QKeyEvent):
        if not self.listening:
            super().keyPressEvent(event)
            return

        key = event.key()
        modifiers = event.modifiers()

        if key in (Qt.Key_Shift, Qt.Key_Control, Qt.Key_Alt, Qt.Key_Meta):
            return

        if key == Qt.Key_Escape:
            self.stop_listening()
            return
        if key == Qt.Key_Delete:
            self.stop_listening()
            self.current_binding = "None"
            self.update_style()
            self.key_assigned.emit(self.action_key, "None")
            return

        name = self._qt_key_to_name(key)
        self.stop_listening()
        self.current_binding = name
        self.update_style()
        self.key_assigned.emit(self.action_key, name)

    def mousePressEvent(self, event: QMouseEvent):
        if not self.listening:
            super().mousePressEvent(event)
            return

        btn = event.button()
        modifiers = event.modifiers()
        is_shift = bool(modifiers & Qt.ShiftModifier)

        btn_map = {
            Qt.LeftButton: "Shift + Mouse Left" if is_shift else "Mouse Left",
            Qt.RightButton: "Shift + Mouse Right" if is_shift else "Mouse Right",
            Qt.MiddleButton: "Mouse Middle",
            Qt.BackButton: "Mouse X1",
            Qt.ForwardButton: "Mouse X2",
        }

        name = btn_map.get(btn, None)
        if name:
            self.stop_listening()
            self.current_binding = name
            self.update_style()
            self.key_assigned.emit(self.action_key, name)
        else:
            self.stop_listening()

    def _qt_key_to_name(self, key: int) -> str:
        key_map = {
            Qt.Key_Space: "Space",
            Qt.Key_Return: "Return",
            Qt.Key_Enter: "Return",
            Qt.Key_Tab: "Tab",
            Qt.Key_Backspace: "Backspace",
            Qt.Key_Left: "Left",
            Qt.Key_Right: "Right",
            Qt.Key_Up: "Up",
            Qt.Key_Down: "Down",
            Qt.Key_PageUp: "PageUp",
            Qt.Key_PageDown: "PageDown",
            Qt.Key_Home: "Home",
            Qt.Key_End: "End",
            Qt.Key_Insert: "Insert",
            Qt.Key_CapsLock: "CapsLock",
            Qt.Key_NumLock: "Numlock",
            Qt.Key_Print: "PrintScreen",
            Qt.Key_ScrollLock: "ScrollLock",
            Qt.Key_Pause: "Pause",
        }
        if key in key_map:
            return key_map[key]
        if Qt.Key_F1 <= key <= Qt.Key_F24:
            return f"F{key - Qt.Key_F1 + 1}"
        if Qt.Key_A <= key <= Qt.Key_Z:
            return chr(key)
        if Qt.Key_0 <= key <= Qt.Key_9:
            return chr(key)
        return "Unknown"




def apply_dwm_styling(hwnd: int):
    if os.name != "nt" or not hwnd:
        return
    try:
        dwmapi = ctypes.windll.dwmapi
        dark = ctypes.c_int(1)
        dwmapi.DwmSetWindowAttribute(hwnd, 20, ctypes.byref(dark), ctypes.sizeof(dark))
        corner = ctypes.c_int(2)
        dwmapi.DwmSetWindowAttribute(hwnd, 33, ctypes.byref(corner), ctypes.sizeof(corner))
        caption_color = ctypes.c_int(0x00131111)
        dwmapi.DwmSetWindowAttribute(hwnd, 35, ctypes.byref(caption_color), ctypes.sizeof(caption_color))
    except Exception:
        pass


def hide_title_icon(hwnd: int):
    """Remove the (blank) icon from a native title bar by setting WS_EX_DLGMODALFRAME."""
    if os.name != "nt" or not hwnd:
        return
    try:
        from ctypes import wintypes

        user32 = ctypes.WinDLL("user32", use_last_error=True)
        get_long = getattr(user32, "GetWindowLongPtrW", None) or user32.GetWindowLongW
        set_long = getattr(user32, "SetWindowLongPtrW", None) or user32.SetWindowLongW
        get_long.restype = ctypes.c_ssize_t
        get_long.argtypes = [wintypes.HWND, ctypes.c_int]
        set_long.restype = ctypes.c_ssize_t
        set_long.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_ssize_t]
        user32.SetWindowPos.argtypes = [
            wintypes.HWND, wintypes.HWND,
            ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint,
        ]

        GWL_EXSTYLE = -20
        WS_EX_DLGMODALFRAME = 0x00000001
        # SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED
        flags = 0x0001 | 0x0002 | 0x0004 | 0x0010 | 0x0020

        hwnd = wintypes.HWND(hwnd)
        ex = get_long(hwnd, GWL_EXSTYLE)
        set_long(hwnd, GWL_EXSTYLE, ex | WS_EX_DLGMODALFRAME)
        user32.SetWindowPos(hwnd, None, 0, 0, 0, 0, flags)
    except Exception:
        pass


class DialogStyler(QObject):
    """Applies the dark, icon-free title bar to every dialog (QMessageBox etc.) when it is first shown."""

    def eventFilter(self, obj, event):
        if event.type() == QEvent.Show and isinstance(obj, QDialog) and not obj.property("_dwm_styled"):
            obj.setProperty("_dwm_styled", True)
            hwnd = int(obj.winId())
            hide_title_icon(hwnd)
            apply_dwm_styling(hwnd)
        return False


def install_dialog_styling(app):
    app.setStyleSheet(STYLE_SHEET)
    styler = DialogStyler(app)
    app.installEventFilter(styler)
    app._dialog_styler = styler


STYLE_SHEET = """
QMainWindow, QStackedWidget, QDialog {
    background-color: #111113;
}
QFrame#MainContainer {
    background-color: #111113;
    border: 1px solid #23232a;
}
QFrame#TitleBar {
    background-color: #111113;
    border-bottom: 1px solid #1a1a20;
}
QLabel#TitleBarText {
    color: #6a6a78;
    font-size: 11px;
    font-weight: 600;
    background: transparent;
}
QPushButton#WinBtn, QPushButton#WinCloseBtn {
    background-color: transparent;
    color: #92929e;
    font-family: "Segoe UI Symbol", "Segoe UI", sans-serif;
    font-size: 12px;
    border: none;
    border-radius: 0px;
}
QPushButton#WinBtn:hover {
    background-color: #22222a;
    color: #ffffff;
}
QPushButton#WinCloseBtn:hover {
    background-color: #cc161e;
    color: #ffffff;
}
QWidget {
    color: #e0e0e4;
    font-family: "Segoe UI", sans-serif;
    font-size: 13px;
}
QScrollArea, QScrollArea > QWidget > QWidget, QWidget#ScrollContent {
    background-color: #111113;
    border: none;
}
QFrame#Sidebar {
    background-color: #16161a;
    border-right: 1px solid #23232a;
}
QPushButton#NavBtn {
    background-color: transparent;
    color: #92929e;
    font-size: 13px;
    font-weight: 600;
    text-align: left;
    padding: 12px 18px;
    border: none;
    border-radius: 6px;
    margin: 3px 10px;
}
QPushButton#NavBtn:hover {
    background-color: #1f1f26;
    color: #ffffff;
}
QPushButton#NavBtn:checked {
    background-color: #271214;
    color: #e63946;
    border-left: 3px solid #e63946;
}
QFrame#Card {
    background-color: #18181d;
    border: 1px solid #24242d;
    border-radius: 8px;
}
QFrame#Card QLabel, QFrame#Card QCheckBox {
    background-color: transparent;
}
QLabel#HeaderTitle {
    color: #f2f2f5;
    font-size: 19px;
    font-weight: 700;
    background-color: transparent;
}
QLabel#CardTitle {
    color: #d1b87a;
    font-size: 13px;
    font-weight: 600;
    background-color: transparent;
}
QLineEdit, QComboBox {
    background-color: #101014;
    color: #ffffff;
    border: 1px solid #2c2c36;
    border-radius: 5px;
    padding: 6px 10px;
}
QLineEdit:focus, QComboBox:focus {
    border: 1px solid #a31d24;
}
QComboBox::drop-down {
    border: none;
    width: 24px;
}
QComboBox QAbstractItemView {
    background-color: #16161b;
    border: 1px solid #2c2c36;
    color: #ffffff;
    selection-background-color: #8b1116;
}
QPushButton#PrimaryBtn {
    background-color: #99141a;
    color: #ffffff;
    font-size: 13px;
    font-weight: 700;
    padding: 10px 18px;
    border: none;
    border-radius: 6px;
}
QPushButton#PrimaryBtn:hover {
    background-color: #b51920;
}
QPushButton#PrimaryBtn:disabled {
    background-color: #3d1416;
    color: #7a5052;
}
QPushButton#SecondaryBtn {
    background-color: #22222a;
    color: #dcdce0;
    font-weight: 600;
    padding: 7px 14px;
    border: 1px solid #2e2e38;
    border-radius: 5px;
}
QPushButton#SecondaryBtn:hover {
    background-color: #2d2d38;
    color: #ffffff;
}
QPushButton#StopBtn {
    background-color: #361718;
    color: #e65555;
    font-weight: 700;
    border: 1px solid #542224;
    border-radius: 6px;
    padding: 10px 18px;
}
QPushButton#StopBtn:hover {
    background-color: #4a1d1f;
}
QPushButton#StopBtn:disabled {
    background-color: #1a1a1e;
    color: #44444c;
    border-color: #222228;
}
QMessageBox, QDialog {
    background-color: #111113;
}
QMessageBox QLabel {
    background-color: transparent;
    color: #e0e0e4;
}
QMessageBox QPushButton, QDialogButtonBox QPushButton {
    background-color: #22222a;
    color: #dcdce0;
    font-weight: 600;
    padding: 7px 14px;
    min-width: 76px;
    border: 1px solid #2e2e38;
    border-radius: 5px;
}
QMessageBox QPushButton:hover, QDialogButtonBox QPushButton:hover {
    background-color: #2d2d38;
    color: #ffffff;
}
QMessageBox QPushButton:pressed, QDialogButtonBox QPushButton:pressed {
    background-color: #1a1a21;
}
QMessageBox QPushButton:default, QMessageBox QPushButton:focus,
QDialogButtonBox QPushButton:default, QDialogButtonBox QPushButton:focus {
    border: 1px solid #a31d24;
}
QCheckBox {
    spacing: 8px;
    background-color: transparent;
}
QCheckBox::indicator {
    width: 16px;
    height: 16px;
    border: 2px solid #8c8c9a;
    border-radius: 4px;
    background-color: #000000;
}
QCheckBox::indicator:hover {
    border: 2px solid #d1b87a;
}
QCheckBox::indicator:checked {
    background-color: #cc161e;
    border: 2px solid #8c8c9a;
}
QCheckBox::indicator:checked:hover {
    background-color: #e01b24;
    border: 2px solid #d1b87a;
}
QSlider {
    background-color: transparent;
}
QSlider::groove:horizontal {
    height: 4px;
    background: #25252e;
    border-radius: 2px;
}
QSlider::sub-page:horizontal {
    background: #99141a;
    border-radius: 2px;
}
QSlider::handle:horizontal {
    background: #ffffff;
    width: 14px;
    height: 14px;
    margin-top: -5px;
    margin-bottom: -5px;
    border-radius: 7px;
}
QPlainTextEdit {
    background-color: #0b0b0d;
    color: #b0b0b8;
    font-family: "Consolas", monospace;
    font-size: 11px;
    border: 1px solid #1f1f26;
    border-radius: 6px;
}
QScrollBar:vertical {
    border: none;
    background: #111113;
    width: 8px;
    margin: 0px;
}
QScrollBar::handle:vertical {
    background: #2c2c36;
    min-height: 24px;
    border-radius: 4px;
}
QScrollBar::handle:vertical:hover {
    background: #424250;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0px;
    background: none;
}
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
    background: none;
}
"""


class LauncherUI:
    def init_ui(self):
        self.setWindowFlags(
            Qt.FramelessWindowHint
            | Qt.Window
            | Qt.WindowMinimizeButtonHint
        )

        central = QFrame()
        central.setObjectName("MainContainer")
        self.setCentralWidget(central)

        root_layout = QHBoxLayout(central)
        root_layout.setContentsMargins(0, 0, 0, 0)
        root_layout.setSpacing(0)

        # ----------------- SIDEBAR -----------------
        self.sidebar = QFrame()
        self.sidebar.setObjectName("Sidebar")
        self.sidebar.setFixedWidth(220)
        sb_layout = QVBoxLayout(self.sidebar)
        sb_layout.setContentsMargins(0, 18, 0, 16)
        sb_layout.setSpacing(4)

        logo_title = QLabel("BB LAUNCHER")
        logo_title.setStyleSheet(
            "color: #d1b87a; font-size: 15px; font-weight: 800; letter-spacing: 2px; padding: 0 20px; background: transparent;"
        )
        logo_sub = QLabel("WIN32 / VULKAN PORT")
        logo_sub.setStyleSheet(
            "color: #656573; font-size: 10px; font-weight: 600; padding: 0 20px 18px 20px; background: transparent;"
        )
        sb_layout.addWidget(logo_title)
        sb_layout.addWidget(logo_sub)

        self.btn_home = QPushButton("  HOME")
        self.btn_patches = QPushButton("  PATCHES")
        self.btn_settings = QPushButton("  SETTINGS")

        for b in [self.btn_home, self.btn_patches, self.btn_settings]:
            b.setObjectName("NavBtn")
            b.setCheckable(True)
            sb_layout.addWidget(b)

        self.btn_home.setChecked(True)
        self.btn_home.clicked.connect(lambda: self.switch_tab(0))
        self.btn_patches.clicked.connect(lambda: self.switch_tab(1))
        self.btn_settings.clicked.connect(lambda: self.switch_tab(2))

        sb_layout.addStretch()

        self.lbl_status = QLabel("\u25cf Engine Idle")
        self.lbl_status.setStyleSheet("color: #6a6a78; font-size: 11px; padding: 0 20px; background: transparent;")
        sb_layout.addWidget(self.lbl_status)

        root_layout.addWidget(self.sidebar)

        # ----------------- RIGHT PANEL (TITLE BAR + STACK) -----------------
        right_panel = QWidget()
        right_layout = QVBoxLayout(right_panel)
        right_layout.setContentsMargins(0, 0, 0, 0)
        right_layout.setSpacing(0)

        self.title_bar = QFrame()
        self.title_bar.setObjectName("TitleBar")
        self.title_bar.setFixedHeight(36)
        tb_layout = QHBoxLayout(self.title_bar)
        tb_layout.setContentsMargins(24, 0, 0, 0)
        tb_layout.setSpacing(0)

        lbl_win_title = QLabel("Bloodborne PC Windows Launcher")
        lbl_win_title.setObjectName("TitleBarText")
        tb_layout.addWidget(lbl_win_title)
        tb_layout.addStretch()

        btn_min = QPushButton("\u2014")
        btn_min.setObjectName("WinBtn")
        btn_min.setFixedSize(46, 36)
        btn_min.clicked.connect(self.showMinimized)
        tb_layout.addWidget(btn_min)

        btn_close = QPushButton("\u2715")
        btn_close.setObjectName("WinCloseBtn")
        btn_close.setFixedSize(46, 36)
        btn_close.clicked.connect(self.close)
        tb_layout.addWidget(btn_close)

        right_layout.addWidget(self.title_bar)

        self.stack = QStackedWidget()
        self.stack.addWidget(self.create_home_tab())
        self.stack.addWidget(self.create_patches_tab())
        self.stack.addWidget(self.create_settings_tab())
        right_layout.addWidget(self.stack, 1)

        root_layout.addWidget(right_panel, 1)

    def _in_drag_zone(self, pos) -> bool:
        if pos.x() < self.sidebar.width():
            return pos.y() <= 72  # sidebar logo area, above the nav buttons
        return pos.y() <= self.title_bar.height()

    def mousePressEvent(self, event):
        if event.button() == Qt.LeftButton and self.windowHandle():
            pos = event.position().toPoint()
            if self._in_drag_zone(pos):
                self.windowHandle().startSystemMove()
                event.accept()
                return
        super().mousePressEvent(event)

    def switch_tab(self, idx: int):
        self.btn_home.setChecked(idx == 0)
        self.btn_patches.setChecked(idx == 1)
        self.btn_settings.setChecked(idx == 2)
        self.stack.setCurrentIndex(idx)

    def create_home_tab(self) -> QWidget:
        w = QWidget()
        w.setObjectName("ScrollContent")
        layout = QVBoxLayout(w)
        layout.setContentsMargins(24, 16, 24, 20)
        layout.setSpacing(14)

        card_exec = QFrame()
        card_exec.setObjectName("Card")
        l_exec = QVBoxLayout(card_exec)
        l_exec.setContentsMargins(16, 14, 16, 14)
        l_exec.setSpacing(8)

        lbl = QLabel("Bloodborne eboot.bin Path")
        lbl.setObjectName("CardTitle")
        l_exec.addWidget(lbl)

        row_exec = QHBoxLayout()
        init_eboot = self.settings.get("eboot", "")
        if not init_eboot or not Path(init_eboot).exists():
            init_eboot = str(DEFAULT_GAME / "eboot.bin") if (DEFAULT_GAME / "eboot.bin").exists() else ""
        self.txt_eboot = QLineEdit(init_eboot)
        self.txt_eboot.setPlaceholderText("Path to decrypted eboot.bin or game folder...")
        row_exec.addWidget(self.txt_eboot, 1)

        btn_browse = QPushButton("Browse...")
        btn_browse.setObjectName("SecondaryBtn")
        btn_browse.clicked.connect(self.browse_eboot)
        row_exec.addWidget(btn_browse)
        l_exec.addLayout(row_exec)

        layout.addWidget(card_exec)

        row_actions = QHBoxLayout()
        row_actions.setSpacing(10)

        self.btn_launch = QPushButton("\u25b6  LAUNCH BLOODBORNE")
        self.btn_launch.setObjectName("PrimaryBtn")
        self.btn_launch.setFixedHeight(44)
        self.btn_launch.clicked.connect(self.launch_game)
        row_actions.addWidget(self.btn_launch, 2)

        self.btn_stop = QPushButton("\u25a0  Stop")
        self.btn_stop.setObjectName("StopBtn")
        self.btn_stop.setFixedHeight(44)
        self.btn_stop.setEnabled(False)
        self.btn_stop.clicked.connect(self.stop_game)
        row_actions.addWidget(self.btn_stop, 1)

        layout.addLayout(row_actions)

        row_tools = QHBoxLayout()
        row_tools.setSpacing(8)

        btn_verify = QPushButton("Verify Setup")
        btn_verify.setObjectName("SecondaryBtn")
        btn_verify.clicked.connect(self.manual_verify_setup)
        row_tools.addWidget(btn_verify)

        btn_cache = QPushButton("Reset Shader Cache")
        btn_cache.setObjectName("SecondaryBtn")
        btn_cache.clicked.connect(self.reset_shader_cache)
        row_tools.addWidget(btn_cache)

        row_tools.addStretch()
        layout.addLayout(row_tools)

        card_log = QFrame()
        card_log.setObjectName("Card")
        l_log = QVBoxLayout(card_log)
        l_log.setContentsMargins(14, 12, 14, 12)
        l_log.setSpacing(8)

        row_log_head = QHBoxLayout()
        lbl_log = QLabel("Engine Console Output")
        lbl_log.setObjectName("CardTitle")
        row_log_head.addWidget(lbl_log)
        row_log_head.addStretch()

        self.chk_vk_noise = QCheckBox("Filter Vulkan Warnings")
        self.chk_vk_noise.setChecked(self.settings.get("hide_vk", True))
        row_log_head.addWidget(self.chk_vk_noise)

        btn_copy = QPushButton("Copy")
        btn_copy.setObjectName("SecondaryBtn")
        btn_copy.clicked.connect(self.copy_log)
        row_log_head.addWidget(btn_copy)

        btn_clear = QPushButton("Clear")
        btn_clear.setObjectName("SecondaryBtn")
        btn_clear.clicked.connect(self.clear_log)
        row_log_head.addWidget(btn_clear)

        l_log.addLayout(row_log_head)

        self.log_view = QPlainTextEdit()
        self.log_view.setReadOnly(True)
        self.log_view.setMaximumBlockCount(5000)
        l_log.addWidget(self.log_view, 1)

        layout.addWidget(card_log, 1)
        return w

    def create_patches_tab(self) -> QWidget:
        w = QWidget()
        w.setObjectName("ScrollContent")
        w.setUpdatesEnabled(False)
        layout = QVBoxLayout(w)
        layout.setContentsMargins(24, 16, 24, 20)
        layout.setSpacing(12)

        header = QLabel("Game Engine & Community Patches")
        header.setObjectName("HeaderTitle")
        layout.addWidget(header)

        card_filter = QFrame()
        card_filter.setObjectName("Card")
        l_filter = QHBoxLayout(card_filter)
        l_filter.setContentsMargins(12, 10, 12, 10)
        l_filter.setSpacing(10)

        self.txt_patch_search = QLineEdit()
        self.txt_patch_search.setPlaceholderText("Filter patches by keyword or author...")
        self.txt_patch_search.textChanged.connect(self.filter_patches)
        l_filter.addWidget(self.txt_patch_search, 1)

        self.cmb_patch_cat = QComboBox()
        self.cmb_patch_cat.addItems([
            "All Categories",
            "Performance & Fixes",
            "Visuals & Camera",
            "Gameplay & Cheats",
            "Framerate & Engine",
            "Resolutions & Grids",
            "Debug & Tools",
        ])
        self.cmb_patch_cat.currentTextChanged.connect(self.filter_patches)
        l_filter.addWidget(self.cmb_patch_cat)

        btn_rec = QPushButton("Recommended Defaults")
        btn_rec.setObjectName("SecondaryBtn")
        btn_rec.clicked.connect(self.apply_recommended_patches)
        l_filter.addWidget(btn_rec)

        self.lbl_patch_count = QLabel(f"{len(self.enabled_patches)} / {len(self.all_xml_patches)} enabled")
        self.lbl_patch_count.setStyleSheet("color: #d1b87a; font-weight: 600; padding: 0 4px;")
        l_filter.addWidget(self.lbl_patch_count)

        layout.addWidget(card_filter)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        self.patch_list_widget = QWidget()
        self.patch_list_widget.setObjectName("ScrollContent")
        self.patch_list_layout = QVBoxLayout(self.patch_list_widget)
        self.patch_list_layout.setContentsMargins(0, 0, 6, 0)
        self.patch_list_layout.setSpacing(6)

        for patch in self.all_xml_patches:
            name = patch["name"]
            author = patch["author"]
            cat = patch["category"]
            note = patch["note"]

            card = QFrame()
            card.setObjectName("Card")
            c_layout = QVBoxLayout(card)
            c_layout.setContentsMargins(12, 10, 12, 10)
            c_layout.setSpacing(4)

            top_row = QHBoxLayout()
            cb = QCheckBox(name.replace("&", "&&"))
            cb.setStyleSheet("font-weight: 700; color: #ffffff;")
            cb.setChecked(name in self.enabled_patches)
            cb.stateChanged.connect(lambda state, p=name: self.on_patch_toggled(p, state))
            top_row.addWidget(cb, 1)

            meta_lbl = QLabel(f'<span style="color:#7b7b88;">by {author}</span> &nbsp;<span style="color:#d1b87a;font-weight:600;">[{cat}]</span>')
            meta_lbl.setTextFormat(Qt.RichText)
            meta_lbl.setStyleSheet("font-size: 11px;")
            top_row.addWidget(meta_lbl)
            c_layout.addLayout(top_row)

            if note:
                lbl_n = QLabel(note)
                lbl_n.setStyleSheet("color: #92929e; font-size: 11px; padding-left: 26px;")
                lbl_n.setWordWrap(True)
                c_layout.addWidget(lbl_n)

            card.setProperty("patch_search_blob", f"{name} {author} {note}".lower())
            card.setProperty("patch_category", cat)

            self.patch_checkboxes[name] = cb
            self.patch_list_layout.addWidget(card)

        self.patch_list_layout.addStretch()
        scroll.setWidget(self.patch_list_widget)
        layout.addWidget(scroll, 1)

        w.setUpdatesEnabled(True)
        return w

    def create_settings_tab(self) -> QWidget:
        w = QWidget()
        w.setObjectName("ScrollContent")
        layout = QVBoxLayout(w)
        layout.setContentsMargins(24, 16, 24, 20)
        layout.setSpacing(14)

        header = QLabel("Render, Runtime & Input Settings")
        header.setObjectName("HeaderTitle")
        layout.addWidget(header)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        container = QWidget()
        container.setObjectName("ScrollContent")
        c_layout = QVBoxLayout(container)
        c_layout.setContentsMargins(0, 0, 6, 0)
        c_layout.setSpacing(14)

        card_disp = QFrame()
        card_disp.setObjectName("Card")
        l_disp = QVBoxLayout(card_disp)
        l_disp.setContentsMargins(16, 14, 16, 14)
        l_disp.setSpacing(12)

        lbl = QLabel("Display & Render Core")
        lbl.setObjectName("CardTitle")
        l_disp.addWidget(lbl)

        grid_disp = QGridLayout()
        grid_disp.setHorizontalSpacing(16)
        grid_disp.setVerticalSpacing(10)

        grid_disp.addWidget(QLabel("Target Framerate:"), 0, 0)
        self.cmb_fps = QComboBox()
        self.cmb_fps.addItems(FPS_CHOICES)
        self.cmb_fps.setCurrentText(self.settings.get("fps", "60"))
        grid_disp.addWidget(self.cmb_fps, 0, 1)

        grid_disp.addWidget(QLabel("Render Resolution:"), 0, 2)
        self.cmb_res = QComboBox()
        self.cmb_res.addItems(RES_CHOICES)
        self.cmb_res.setCurrentText(self.settings.get("res", RES_CHOICES[0]))
        grid_disp.addWidget(self.cmb_res, 0, 3)

        grid_disp.addWidget(QLabel("Window Mode:"), 1, 0)
        self.cmb_win_mode = QComboBox()
        self.cmb_win_mode.addItems(WINDOW_MODE_CHOICES)
        self.cmb_win_mode.setCurrentText(self.settings.get("window_mode", "Borderless"))
        grid_disp.addWidget(self.cmb_win_mode, 1, 1)

        grid_disp.addWidget(QLabel("Anisotropic Filtering:"), 1, 2)
        self.cmb_aniso = QComboBox()
        for label, _, _ in ANISO_CHOICES:
            self.cmb_aniso.addItem(label)
        self.cmb_aniso.setCurrentText(self.settings.get("aniso", "16x"))
        grid_disp.addWidget(self.cmb_aniso, 1, 3)

        l_disp.addLayout(grid_disp)

        row_flags = QGridLayout()
        row_flags.setHorizontalSpacing(16)
        row_flags.setVerticalSpacing(8)

        self.chk_fsr = QCheckBox("Enable FSR 3.1 Temporal Upscaler")
        self.chk_fsr.setChecked(self.settings.get("feat_upscaler", False))
        row_flags.addWidget(self.chk_fsr, 0, 0)

        self.chk_fps_patch = QCheckBox("Engine Framerate Patch (60/90/Uncap)")
        self.chk_fps_patch.setChecked(self.settings.get("feat_fps_patch", True))
        row_flags.addWidget(self.chk_fps_patch, 0, 1)

        self.chk_res_scale = QCheckBox("Resolution Scaling Engine")
        self.chk_res_scale.setChecked(self.settings.get("feat_res_scaling", True))
        self.chk_res_scale.toggled.connect(self.cmb_res.setEnabled)
        self.cmb_res.setEnabled(self.chk_res_scale.isChecked())
        row_flags.addWidget(self.chk_res_scale, 1, 0)

        self.chk_mods = QCheckBox("Load mods/ Directory Assets")
        self.chk_mods.setChecked(self.settings.get("mods", False))
        row_flags.addWidget(self.chk_mods, 1, 1)

        self.chk_overlay = QCheckBox("In-Game Overlay Configuration Menu")
        self.chk_overlay.setChecked(self.settings.get("feat_overlay", True))
        row_flags.addWidget(self.chk_overlay, 2, 0)

        self.chk_hud = QCheckBox("Display Internal HUD Frame Diagnostics")
        self.chk_hud.setChecked(self.settings.get("feat_hud", False))
        row_flags.addWidget(self.chk_hud, 2, 1)

        l_disp.addLayout(row_flags)
        c_layout.addWidget(card_disp)

        card_mk = QFrame()
        card_mk.setObjectName("Card")
        l_mk = QVBoxLayout(card_mk)
        l_mk.setContentsMargins(16, 14, 16, 14)
        l_mk.setSpacing(12)

        lbl_mk = QLabel("Mouse & Keyboard Input Configuration")
        lbl_mk.setObjectName("CardTitle")
        l_mk.addWidget(lbl_mk)

        ini_opts = self.load_bbport_ini()
        self.chk_mk_enabled = QCheckBox("Enable Mouse && Keyboard Mode")
        self.chk_mk_enabled.setChecked(ini_opts.get("mk_enabled", "1") != "0")
        self.chk_mk_enabled.stateChanged.connect(lambda: self.save_bbport_ini())
        l_mk.addWidget(self.chk_mk_enabled)

        grid_mk = QGridLayout()
        grid_mk.setHorizontalSpacing(16)
        grid_mk.setVerticalSpacing(10)

        grid_mk.addWidget(QLabel("Horizontal Sensitivity:"), 0, 0)
        self.sl_sens_x = QSlider(Qt.Horizontal)
        self.sl_sens_x.setRange(10, 1000)
        self.sl_sens_x.setValue(int(float(ini_opts.get("mk_sens_x", "1.50")) * 100))
        self.lbl_sens_x_val = QLabel(f"{self.sl_sens_x.value() / 100:.2f}")
        self.sl_sens_x.valueChanged.connect(lambda v: self.lbl_sens_x_val.setText(f"{v / 100:.2f}"))
        self.sl_sens_x.sliderReleased.connect(self.save_bbport_ini)
        grid_mk.addWidget(self.sl_sens_x, 0, 1)
        grid_mk.addWidget(self.lbl_sens_x_val, 0, 2)

        grid_mk.addWidget(QLabel("Vertical Sensitivity:"), 1, 0)
        self.sl_sens_y = QSlider(Qt.Horizontal)
        self.sl_sens_y.setRange(10, 1000)
        self.sl_sens_y.setValue(int(float(ini_opts.get("mk_sens_y", "1.50")) * 100))
        self.lbl_sens_y_val = QLabel(f"{self.sl_sens_y.value() / 100:.2f}")
        self.sl_sens_y.valueChanged.connect(lambda v: self.lbl_sens_y_val.setText(f"{v / 100:.2f}"))
        self.sl_sens_y.sliderReleased.connect(self.save_bbport_ini)
        grid_mk.addWidget(self.sl_sens_y, 1, 1)
        grid_mk.addWidget(self.lbl_sens_y_val, 1, 2)

        grid_mk.addWidget(QLabel("Deadzone:"), 2, 0)
        self.sl_dz = QSlider(Qt.Horizontal)
        self.sl_dz.setRange(0, 35)
        self.sl_dz.setValue(int(float(ini_opts.get("mk_deadzone", "0.13")) * 100))
        self.lbl_dz_val = QLabel(f"{self.sl_dz.value() / 100:.2f}")
        self.sl_dz.valueChanged.connect(lambda v: self.lbl_dz_val.setText(f"{v / 100:.2f}"))
        self.sl_dz.sliderReleased.connect(self.save_bbport_ini)
        grid_mk.addWidget(self.sl_dz, 2, 1)
        grid_mk.addWidget(self.lbl_dz_val, 2, 2)

        grid_mk.addWidget(QLabel("Mouse Smoothing:"), 3, 0)
        self.sl_smoothing = QSlider(Qt.Horizontal)
        self.sl_smoothing.setRange(0, 80)
        self.sl_smoothing.setValue(int(float(ini_opts.get("mk_smoothing", "0.00")) * 100))
        self.lbl_smoothing_val = QLabel(f"{self.sl_smoothing.value() / 100:.2f}")
        self.sl_smoothing.valueChanged.connect(lambda v: self.lbl_smoothing_val.setText(f"{v / 100:.2f}"))
        self.sl_smoothing.sliderReleased.connect(self.save_bbport_ini)
        grid_mk.addWidget(self.sl_smoothing, 3, 1)
        grid_mk.addWidget(self.lbl_smoothing_val, 3, 2)

        self.chk_inv_x = QCheckBox("Invert Horizontal Look (X)")
        self.chk_inv_x.setChecked(ini_opts.get("mk_invert_x", "0") != "0")
        self.chk_inv_x.stateChanged.connect(lambda: self.save_bbport_ini())
        grid_mk.addWidget(self.chk_inv_x, 4, 0)

        self.chk_inv_y = QCheckBox("Invert Vertical Look (Y)")
        self.chk_inv_y.setChecked(ini_opts.get("mk_invert_y", "0") != "0")
        self.chk_inv_y.stateChanged.connect(lambda: self.save_bbport_ini())
        grid_mk.addWidget(self.chk_inv_y, 4, 1)

        l_mk.addLayout(grid_mk)

        sep = QFrame()
        sep.setFixedHeight(1)
        sep.setStyleSheet("background-color: #24242d; border: none;")
        l_mk.addWidget(sep)

        row_kb_header = QHBoxLayout()
        lbl_kb = QLabel("Customize Keybinds")
        lbl_kb.setObjectName("CardTitle")
        row_kb_header.addWidget(lbl_kb)
        row_kb_header.addStretch()

        btn_reset_kb = QPushButton("Reset Keybinds to Defaults")
        btn_reset_kb.setObjectName("SecondaryBtn")
        btn_reset_kb.clicked.connect(self.reset_keybinds_to_defaults)
        row_kb_header.addWidget(btn_reset_kb)
        l_mk.addLayout(row_kb_header)

        grid_kb = QGridLayout()
        grid_kb.setHorizontalSpacing(16)
        grid_kb.setVerticalSpacing(8)
        grid_kb.setColumnMinimumWidth(0, 240)
        grid_kb.setColumnMinimumWidth(1, 160)
        grid_kb.setColumnStretch(1, 0)
        grid_kb.setColumnStretch(2, 1)

        self.keybind_buttons = {}
        for row, (ini_key, desc, def_val) in enumerate(DEFAULT_KEYBINDS):
            lbl_desc = QLabel(desc)
            lbl_desc.setStyleSheet("color: #dcdce0; font-weight: 500; background: transparent;")
            grid_kb.addWidget(lbl_desc, row, 0)

            cur_val = ini_opts.get(ini_key, def_val)
            btn_kb = KeybindButton(ini_key, cur_val)
            btn_kb.key_assigned.connect(self.on_keybind_button_assigned)
            self.keybind_buttons[ini_key] = btn_kb
            grid_kb.addWidget(btn_kb, row, 1)

        l_mk.addLayout(grid_kb)

        btn_save_ini = QPushButton("Save Input Parameters to bbport.ini")
        btn_save_ini.setObjectName("SecondaryBtn")
        btn_save_ini.clicked.connect(self.save_bbport_ini)
        l_mk.addWidget(btn_save_ini)

        c_layout.addWidget(card_mk)
        c_layout.addStretch()

        scroll.setWidget(container)
        layout.addWidget(scroll, 1)
        return w
