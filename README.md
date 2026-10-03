# sacredpad — controller support for Sacred Gold

D2R-style gamepad play for **Sacred Gold (Steam, v2.0.2.28)**: walk with the stick, auto-targeted attacks and
combat arts, potions on the D-pad, and a pad-driven cursor in menus. One DLL; game files are not patched.

It also makes the Steam version start under **Proton 11** (stock, the game crashes on start there).

## Install

```sh
./install.sh            # finds the Steam folder; or: ./install.sh "/path/to/Sacred Gold"
./uninstall.sh          # restores the original ijl15.dll
```

The game's `ijl15.dll` is renamed `ijl15_real.dll` and replaced by this one (which forwards to it).
A Steam "Verify integrity of game files" undoes the install; just run `./install.sh` again.

**Steam Input:** in the game's controller settings pick the **Gamepad** layout (not a keyboard/mouse
template), so the game only sees the virtual Xbox pad.

**First Steam launch:** Steam runs the DirectX redistributable installer once and asks you to accept
Microsoft's license. It isn't needed under Proton; Cancel is fine.

## Controls (Xbox names)

In the world:

| Input | Action |
|---|---|
| Left stick | Walk / run |
| A | Attack / talk / open / use what's targeted (the hero walks there if needed) |
| X Y B RB RT | Combat-art slots 6, 7, 8, 9, 0: select it and use it on the target (hold to keep casting) |
| LT + X Y B RB RT | Weapon slots 1–5 |
| LB (hold) | Show names, target loot only; LB + A picks it up (nothing in reach: collect all) |
| D-pad ↑ → ← ↓ | Healing potion (Space), Q, E, R potions; LT + ↑: W potion |
| R3 | Lock / unlock the current target |
| L3 | Toggle stand-still (Ctrl: attack in place) |
| Right stick | Manual cursor (aims arts at a spot); auto-targeting returns 2 s after you let go |
| LT + right stick ↑↓ | Zoom |
| Back / LT + Back | Inventory / map |
| Start / LT + Start | Game menu / quest log |

Targeting picks the best enemy in front of where you're steering (enemies first, then NPCs and
containers), sticks to it while you hold the button, and uses the game's own highlight and info bar.

In menus, inventory, trade and dialogs:

| Input | Action |
|---|---|
| Either stick | Cursor (slow near the centre for precision) |
| D-pad | Jump to the next button that way / one inventory cell |
| A / X | Left / right click (pick up & place / equip & use) |
| B, Start | Close |
| LB / RB | Inventory tabs |

The mouse keeps working: moving it hands the cursor back until you touch the pad again.

## Notes

- Under Wine/Proton the movies (intro, chapter transitions) are skipped: DirectShow crashes there.
- The game keeps running when it loses focus (it used to freeze).
- Not done yet: on-screen button hints, rebinding/config file, dismount button (Shift+click the horse
  with a mouse), hold-to-aim reticle for ground arts (use the right stick).
- `sacredpad.log` next to the game says what the mod did.

## Development

`PLAN.md` has the task list and the reverse-engineering findings (addresses are for the Steam
2.0.2.28 exe). `./build.sh` builds with llvm-mingw from `tools/`. The `test/` scripts run a sandboxed,
DRM-free copy on Hyprland workspace 5 and drive it through a control file (`SACREDPAD_CTL`, or
`sacredpad_ctl.txt` in the game folder) with tokens like `LX=1 A`, `CX=512 CY=288 A`, `DUMP`, `UIDUMP`.
