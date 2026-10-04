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
| Hold an art button + right stick | Aim: a ring marks the spot; let go of the art button to cast there |
| A on a horse / LT + A while riding | Mount / dismount |
| R3 | Lock / unlock the current target |
| L3 | Toggle stand-still (Ctrl: attack in place) |
| Right stick | Manual cursor (aims arts at a spot); auto-targeting returns 2 s after you let go |
| LT + right stick ↑↓ | Zoom |
| Back / LT + Back | Inventory / map |
| Start / LT + Start | Game menu / quest log |

Targeting picks the best enemy in front of where you're steering (enemies first, then NPCs, containers and
horses), sticks to it while you hold the button, and uses the game's own highlight and info bar.

Button icons show which button fires which slot (they move to the weapon slots while LT is held), which D-pad
direction drinks which potion, an A above the current target, a red ring on a locked target, and a short
legend in menus.

There is no cursor in the world: targets get the game's highlight and an A prompt.

In menus, dialogs, the inventory and trade screens, a gold frame marks the focused entry: menu items, buttons,
dialog answers, inventory items, equipment slots, heroes on the character screen. The game's own hover
effects and item tooltips show for whatever is focused.

| Input | Action |
|---|---|
| D-pad / left stick | Move focus to the next entry that way (hold to repeat) |
| A / X | Select / use or equip (left / right click) |
| B, Start | Back / close |
| LB / RB | Previous / next tab |
| Right stick | Free cursor, for anything focus can't reach |

In grids, focus jumps item to item; while an item is picked up it moves cell by cell so you can place it.
Tabs (backpack / combat arts / combo, a merchant's weapons / armour / misc) are focus stops above their
grid; LB / RB switch the tabs of the grid you're in. Pop-ups ("Load game?", "Quit?") take the focus until
they close. Merchants, smiths and combat masters are targets in the world: walk toward one and press A.

The mouse keeps working: moving it hands the cursor back until you touch the pad again.

## Settings

`sacredpad.ini` is created next to the game on first start (delete it to get the defaults back):

| Key | Default | Meaning |
|---|---|---|
| `deadzone` | 24 | Stick dead zone, percent |
| `cursor_speed` | 12 | Cursor speed in menus and for manual aiming |
| `target_range` | 480 | How far auto-targeting looks (screen pixels) |
| `hints` | 1 | Button icons (0 = off) |
| `d2r_loot` | 0 | 1 = A also picks up loot, like D2R (otherwise hold LB) |
| `manual_aim_ms` | 2000 | How long the right-stick cursor stays manual before auto-targeting returns |

## Notes

- Under Wine/Proton the movies (intro, chapter transitions) are skipped: DirectShow crashes there.
- The game keeps running when it loses focus (it used to freeze).
- Not verified in a live game: mounting/dismounting (no horse near the test saves), and merchant/chest/trade
  grids (no merchant near the test saves; they use the same generic grid code as the verified backpack).
- No button remapping yet.
- `sacredpad.log` next to the game says what the mod did.

## Development

`PLAN.md` has the task list and the reverse-engineering findings (addresses are for the Steam
2.0.2.28 exe). `./build.sh` builds with llvm-mingw from `tools/`. The `test/` scripts run a sandboxed,
DRM-free copy on Hyprland workspace 5 (`test/start.sh new|load|daemon`; `PROTON=1` for Proton, `REAL=1` for
the real install) and drive it through a control file (`SACREDPAD_CTL`, or
`sacredpad_ctl.txt` in the game folder) with tokens like `LX=1 A`, `CX=512 CY=288 A`, `LOCK=<ref>`, `FAR`, `DUMP`, `FOCUS` (focus list),
`UISCAN` (controls found in open windows' memory; `test/uiscan.sh` names their classes).
