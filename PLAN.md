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
- Windowed, no intro movies. Game audio is never muted or rerouted (that broke system audio once). Launched with `hyprctl dispatch exec "[workspace 5 silent]"`.
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
- [x] 1.5 Panels: stick-driven virtual cursor, A/X = left/right click

### Phase 2 — engine-aware gameplay (RE)
- [x] 2.1 Hero world position + world→screen projection
- [x] 2.2 Entity enumeration: type, hostility, alive, position
- [x] 2.3 Auto-target: cone score, sticky, R3 lock, right-stick bias; aim + click
- [x] 2.4 Combat arts on buttons; LT page = weapon slots; hold art + right stick = ground marker, cast on release
- [x] 2.5 Loot: LB-held item targeting, A picks up, LB+A with nothing in reach = collect all
- [x] 2.6 Stand still (L3); mount = A on a horse, LT+A dismounts (not live-tested)

### Phase 3 — controller UI
- [x] 3.1 Open-panel detection
- [x] 3.2 Grid snapping: inventory, equipment, chest, trader (32px cells + button jumps)
- [x] 3.3 Dialog navigation (cursor warps to OK; D-pad between buttons)
- [x] 3.4 Xbox button icons: slots, potions, target prompt, lock ring, menu legend, aim ring

### Phase 4 — ship
- [x] 4.1 install.sh / uninstall.sh for the Steam install
- [x] 4.2 Verified on the real install via Proton 11 (DRM exe, fullscreen): menus + pad input. Needed two Proton fixes
- [x] 4.3 `sacredpad.ini`: deadzone, cursor speed, target range, hints, d2r_loot, manual-aim timeout
- [x] 4.4 README (incl. Steam Input: use the "Gamepad" template)

## Findings
- SteamStub Variant 2.0; Steamless unpacks it. `.text` is decrypted by the time `CreateWindowExA` runs, so code
  patches go there (not DllMain).
- The main thread only pumps messages and the world swaps in its own WndProc (via a delay-loaded SetWindowLongA),
  so the DLL ticks on its own thread; engine-side work runs from the frame loop's cursor pass (`0x611e70` call sites).
- Engine (Steam 2.0.2.28): object manager `*0xad5c40` (+4/+8 ref table), game state `*0x182ebe8` (+0x14 hero ref),
  type table `*0xaab5e4` (0x80/record, +0x2e family), picker `0x626c50` (thiscall view), view screen->ground =
  vtable +0x18, can-attack `0x548f60` (thiscall hero, creature), has-dialog `0x549920`, item-type `0x427920`.
- Object: +0xC ref, +0x10 type, +0x18 sector (0 = carried), +0x1C/+0x20 world x/y; creature HP +0x4D8.
- Camera is a fixed 2:1 isometric projection; measured each frame from three view picks.
- Click semantics come from the game: forcing the picker's answer makes the stock click handler attack / talk /
  open / pick up and path there. Forcing the hover answer gives the game's own target highlight + enemy info bar.
- Proton 11: the game picks a D3D device record whose GUID pointer is stack garbage (0x480); Proton's ddraw
  dereferences it (EnumZBufferFormats, CreateDevice). Fixed at 0x645260's call site. Movies use amstream,
  which crashes under Proton: the opener 0x6a0d80 is stubbed on Wine. Both happen without the mod too.
- UI: cUI_Manager `*0x17eebbc`, windows at +0x80 (23 slots), dialogs [+0x124, +0x128); control: +0x10 flags
  (bit 0 visible), +0x24/+0x28 pos (parent-relative), +0x2c/+0x2e size, +0x30 name, +0x50 parent,
  +0x78/+0x7c children. NPC text dialog = cUI_Tooltip (vtable 0x896b3c) with OK drawn bottom centre.
- Crash on Start (2026-10-04): UI +0x78 is a child list only on some window classes; walking it blindly on
  the Esc menu followed garbage. All UI pointers are now validated.
- Present: 0x645e40 (thiscall driver) from 0x60ea8a (world), 0x757250 (menu thread). Overlay glyphs live in
  video memory: system-memory surfaces cost ~2 ms/frame of colour-keyed blits on wined3d (now ~0.2 ms).
  Targeting + UI snapshot: ~0.09 ms/frame.
  Fullscreen flips the front (+0xb8); mouse singleton `*0xcddb5c`, +4/+8 = cursor sprite position.
- Hero +0x3fe level (HUD slots: 1 + at levels 2, 8, 16, 30), +0x1ec ridden horse, creature +0x1f0 == 4: horse.
  Dismount = Shift + click whose picked ref is the ridden horse (click handler 0x6172c0).
- Focus (2026-10-04): clickable controls are recorded as they draw (vtable slot 5 = render(dev), ret 4, hooked per
  class: Button2, RadioButton2, StaticText64 [+0x98 command], StaticText64FX [+0x88 command], EditControl,
  DigitEdit, Combobox, Listbox, Listbox2; flags & 4 = enabled). Hidden pages/tabs never draw, so the list is
  exactly what's on screen. Text draws 2-9x (shadow/glow offsets): de-duplicated. Parentless buttons = HUD potions.
- Grids: windows store their cInventoryEntry index at +0x158, write the grid's screen origin to entry
  +0x1164/+0x1166, then draw it with 0x5dc430 (thiscall entry, dev, flag; 8 call sites, hooked). Cells 32 px.
  Equipment slots: cUI_Equipment +0x15c..+0x1a0 = UI_EQ_* cUI_Static2 controls.
- Cursor draw: 0x655450 (thiscall mouse, dev, glow), from 0x612e6a/0x612e91 (world) and 0x757227 (menus).
- Main menu runs on its own thread (no world frame loop): UI snapshot is taken at every present instead.
- Character select: heroes are models on 8 pedestals (screen points in snapshot_ui), same on the Import page.
- Keys (manual): LMB move/attack/open/talk, RMB use selected art, 1-5 weapon slots, 6-0 art slots,
  Space/Q/W/E/R potions, I inventory, M map, L log, A collect all, Alt names, Ctrl+click attack in place.
