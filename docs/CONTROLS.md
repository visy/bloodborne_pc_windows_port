# Controls and key bindings

Back to the [README](../README.md). All settings: [SETTINGS.md](SETTINGS.md).

Gamepads work out of the box. The keyboard always works too, also next to a connected gamepad.
There are two keyboard layouts: the **classic layout** (keyboard only, camera on IJKL) and
**mouse & keyboard mode** (mouse camera and attacks; turn it on in the launcher's *Input & Audio*
tab or *System > Mouse & keyboard* in game).

| Action | Gamepad | Classic keyboard | Mouse & keyboard mode |
|---|---|---|---|
| Move | Left stick | `W` `A` `S` `D` | `W` `A` `S` `D` |
| Camera | Right stick | `I` `J` `K` `L` | Mouse |
| Walk (hold while moving) | Push the stick part way | `Left Alt` | `Left Alt` |
| Interact / confirm (Cross) | Bottom button | `Space`, `Enter` | `E`, `Enter` |
| Dodge / run / back (Circle) | Right button | `Left Shift`, `Esc` | `Space` |
| Quick item (Square) | Left button | `E` | `R` |
| Blood vial (Triangle) | Top button | `Q` | `X`, `F`, `V` |
| Attack (R1) / strong attack (R2) | `R1` / `R2` | `3` / `F` | Left click / `Shift` + left click |
| Firearm (L2) / transform (L1) | `L2` / `L1` | `R` / `1` | Right click / `Shift` + right click |
| Lock on (R3) | Right stick click | `C` | Middle click, `Q` |
| Jump while running (L3) | Left stick click | `Z` | `C`, `Z` |
| Items and weapons (d-pad) | D-pad | Arrow keys | Arrow keys, `1`-`4`, mouse wheel (quick items) |
| Menu (Options) | `Start` | `F1`, `O` | `Esc`, `F1` |
| Gestures / key items (touchpad) | Touchpad left / right half (Xbox pads: `Back` + d-pad left / right, `Back` alone the left half) | `Tab` or `G` / `Backspace` | `Tab` or `G` / `Backspace` |
| Party: ping / gather the party (host) | Hold the touchpad, press `R3` / `L3` (Xbox pads: hold `Back`) | `V` / `H` (or `Tab` + `C` / `Tab` + `Z`) | `V` / `H` (or `Tab` + `Q` / `Tab` + `Z`) |
| Party: text chat (off by default) | Hold the touchpad, press `L1` (Xbox pads: hold `Back`): a list of short lines | `T` (`party_chat_key` in `bbport.ini` changes it) | `T` |
| In-game map (off by default, [SETTINGS.md](SETTINGS.md#in-game-map)) | Hold `L1` + `R1` + `Start` for 0.65 s | `M` | `M` |
| Port settings | *System* in the game's menu | *System* in the game's menu | *System* in the game's menu |
| Overlay menu (fallback) | `L3 + R3` (enable in the launcher) | `Insert` | `Insert` |
| Fullscreen | — | `Alt + Enter` | `Alt + Enter` |

In mouse & keyboard mode the side buttons are L1 (`X1`) and R2 (`X2`).

**Button prompts** follow the device you used last: your keys and mouse buttons, Xbox letters or
Nintendo letters (PlayStation pads keep the game's icons). Launcher *Input & Audio > Button
prompts* (or *System > Mouse & keyboard* in game) picks a set, e.g. *Nintendo* for an 8BitDo pad
in X-input mode; what each button does never changes.

Mouse sensitivity, free camera, debug menu and more: [docs/SETTINGS.md](SETTINGS.md).

## Custom key bindings

Every input can be moved to other keys, separately for the classic layout and for mouse & keyboard
mode. Gamepad buttons can be remapped too.

**In the game (easiest):**

1. Press **`Insert`** and open the **Controls** tab. It lists every input of the layout you are using,
   with its keys.
2. Select a row (click it, or `Enter` / Cross), then press the new key. In mouse & keyboard mode a
   mouse button or the wheel works too.
3. The new key replaces that input's keys and works at once. If another input had that key, the two
   swap (the screen says so, e.g. "Q taken from R3 (lock-on); it gets X").

`Esc` cancels, `Delete` unbinds the row, and **Default keys** puts the whole layout back.

**In `bbport.ini` (several keys per input, gamepad buttons):** the in-game menu writes these lines
for you, and you can edit them by hand while the game is closed. Each line replaces that input's
default keys:

```ini
# Classic layout: key.<input>=<keys>
key.touchpad=Tab, rshift
key.touchpad_right=enter

# Mouse & keyboard mode: mkkey.<input>=<keys or mouse buttons>
mkkey.square=R, Mouse X1
mkkey.walk=lctrl

# Gamepad: pad.<input>=<buttons>
pad.touchpad=back, misc1
```

| Part | What you can write |
|---|---|
| `<input>` | `cross` `circle` `square` `triangle` `l1` `r1` `l2` `r2` `l3` `r3` `options` `touchpad` (left half, gestures) `touchpad_right` (right half, key items) `up` `down` `left` `right` (d-pad); keyboard only: `move_up` `move_down` `move_left` `move_right` `look_up` `look_down` `look_left` `look_right` `walk` |
| Keys | Key names as on the keycap, any case: `Q`, `F1`, `Tab`, `Space`, `Backspace`, `Return`, `Escape`, `Left Shift`, `Right Ctrl`, `Up`, `Keypad 5`, ... Short names work too: `rshift` `lshift` `shift` `enter` `return` `esc` `escape` `ctrl` `lctrl` `rctrl` `alt` `lalt` `ralt` `altgr` `bksp` `del` `ins` `pgup` `pgdn` |
| Mouse (mouse & keyboard mode) | `Mouse Left` `Mouse Right` `Mouse Middle` `Mouse X1` `Mouse X2` `Wheel Up` `Wheel Down`. A mouse button bound here leaves its built-in role (and its `Shift` + click). |
| Gamepad buttons | `a` `b` `x` `y` (by position: bottom, right, left, top) `back` `start` `leftstick` `rightstick` `leftshoulder` `rightshoulder` `lefttrigger` `righttrigger` `dpup` `dpdown` `dpleft` `dpright` `touchpad` `misc1` `paddle1`-`paddle4` |

- Separate several keys with commas (up to 4 per input). An empty value (`key.l3=`) unbinds the input.
- `key.` lines only change the classic layout and `mkkey.` lines only mouse & keyboard mode.
- A name the game does not know is skipped and logged (`controls: unknown key ...` in the game log).
- To go back to the defaults, delete the lines (or press **Default keys** in the Controls tab).

All the details: [docs/SETTINGS.md](SETTINGS.md#controls).
