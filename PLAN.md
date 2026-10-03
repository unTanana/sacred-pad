# Sacred Gold controller support — plan

Goal: D2R-style gamepad play for Sacred Gold (Steam, v2.0.2.28, exe timestamp 0x452F85C7).
Delivered as a drop-in DLL; the game files themselves are never patched.

## Approach (ponytail)

- **Load path**: proxy `ijl15.dll` (the game loads it from its own folder; the real one is renamed `ijl15_real.dll`).
  No launch options, no Wine overrides, works under Steam/Proton as-is.
- **Input virtualization, not RE-heavy hooks**: IAT-hook `GetCursorPos`/`SetCursorPos`/`GetAsyncKeyState`/
  `GetKeyboardState`/`GetForegroundWindow`, and post synthetic `WM_*` messages to the game window.
  The game then sees a virtual mouse/keyboard driven by the pad. The real cursor is never touched.
- **"Fake the hover, reuse the click"** (DevilutionX pattern): auto-targeting picks an entity, points the
  virtual cursor at its screen position, then clicks. The game's own hit-test, network commands and rules do the rest.
- RE only what that needs: hero position, world→screen, entity list, open-panel state, grid geometry.
  Addresses seeded from bssth/sacred-sdk (same build, verified) and ancaria-dev mappings (re-matched).
- One C source file, built with llvm-mingw (i686).

## Control scheme (Xbox names; split-loot variant)

| Input | World | Panels |
|---|---|---|
| Left stick | Move | Snap cursor between slots |
| Right stick | Bias/cycle target; aim ground art while held | Free cursor |
| A | Attack / talk / open / mount (context) | Click (pick/place) |
| X Y B RB RT | Combat-art slots 1–5 | X use/equip, B close |
| Hold LT + face/RB/RT | Slots 6–10 | Shift-click |
| LB (hold) | Target loot only; A picks up | Prev tab |
| D-pad | Potions | Step one cell |
| L3 | Stand still | — |
| R3 | Lock/unlock target | — |
| View | Map | — |
| Start | Esc menu | Close |

## Test rig

- Steamless-unpacked exe in `sandbox/` (symlinks to the real install's data), own `WINEPREFIX=prefix/`.
- Windowed, muted, no intro movies. Launched with `hyprctl dispatch exec "[workspace 5 silent]"`.
- Pad input is simulated via a control file the DLL polls (`SACREDPAD_CTL`); no uinput devices.
- Verification is done through DLL-side screenshots and a state dump.

## Tasks

### Phase 0 — setup
- [x] 0.1 Project dir, git, plan
- [x] 0.2 Portable tools: llvm-mingw, Ghidra + JDK, wine-mono, Steamless
- [x] 0.3 Unpack Sacred.exe with Steamless; start Ghidra auto-analysis (background)
- [x] 0.4 Sandbox + prefix + launch/kill scripts; game reaches the menu on ws5 and keeps running unfocused
- [x] 0.5 Proxy DLL skeleton: forwards ijl15, logs, per-frame tick, control-file pad, screenshot on demand

### Phase 1 — virtual input layer (playable without RE)
- [x] 1.1 IAT hooks + synthetic messages; the game runs and accepts input while unfocused
- [x] 1.2 Pad source: XInput (real) or control file (test) → one pad state
- [x] 1.3 Left stick → move (held LMB at hero + stick offset; picker patched so it never talks/attacks)
- [x] 1.4 Buttons → game hotkeys (potions, slots, map, Esc, Alt labels)
- [ ] 1.5 Panels: stick-driven virtual cursor, A/X = left/right click

### Phase 2 — engine-aware gameplay (RE)
- [ ] 2.1 Hero world position + world→screen projection
- [ ] 2.2 Entity enumeration: type, hostility, alive, position
- [ ] 2.3 Auto-target: cone score, sticky, R3 lock, right-stick bias; aim + click
- [ ] 2.4 Combat arts on buttons; LT page; ground arts tap / hold-to-aim
- [ ] 2.5 Loot: LB-held item targeting, A picks up; gold auto
- [ ] 2.6 Stand still, horse mount/dismount

### Phase 3 — controller UI
- [ ] 3.1 Open-panel detection
- [ ] 3.2 Grid snapping: inventory, equipment, chest, trader
- [ ] 3.3 Dialog option navigation
- [ ] 3.4 Xbox button-icon hints (minimal)

### Phase 4 — ship
- [ ] 4.1 install.sh / uninstall.sh for the Steam install
- [ ] 4.2 One verification run via Steam/Proton (ws5)
- [ ] 4.3 `sacredpad.ini`: deadzone, D2R-exact vs split preset
- [ ] 4.4 README (incl. Steam Input: use the "Gamepad" template)

## Findings
(filled in as work progresses)
