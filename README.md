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
| L3 | Toggle stand-still: attacks happen in place instead of walking up (the stick still walks) |
| Right stick | Manual cursor (aims arts at a spot); auto-targeting returns 2 s after you let go |
| LT + right stick ↑↓ | Zoom |
| Start | Panels: inventory first, LB / RB page through the rest (see below) |
| Back | Map |
| LT + Start | Quick save (F9; load it from Load Game) |
| LT + Back (hold) | Minimap |

Holding LT shows a reminder of the LT combinations; stand-still shows an indicator while it's on.

Targeting picks the best enemy in front of where you're steering (enemies first, then NPCs, containers and
horses), sticks to it while you hold the button, and uses the game's own highlight and info bar.

Button icons show which button fires which slot (they move to the weapon slots while LT is held), which D-pad
direction drinks which potion, an A above the current target, a red ring on a locked target, and a legend of
what the buttons do on the current screen.

There is no cursor in the world: targets get the game's highlight and an A prompt.

In menus, dialogs, the inventory and trade screens, a gold frame marks the focused entry: menu items, buttons,
dialog answers, inventory items, equipment slots, heroes on the character screen. The game's own hover
effects and item tooltips show for whatever is focused.

| Input | Action |
|---|---|
| D-pad / left stick | Move focus to the next entry that way (hold to repeat); on a slider: change it |
| A | Select (pick up / place an item) |
| X | Use / equip (right click) |
| Y | On a weapon in the backpack: put it on a weapon slot |
| B | Back / close; in a "Quit?"-style pop-up: Cancel; with an item picked up: put it back |
| Start | Close the panels |
| Back | Map |
| LB / RB | Page: backpack → combat arts → combo → quest log (its four tabs) → game menu → backpack; in a shop: its tabs |
| Right stick | Free cursor, for anything focus can't reach (the left stick too where nothing is focusable, e.g. the map) |

Screens start where you'd want them to: the first item of a page, the game menu's Continue, the open quest
log tab, Cancel in pop-ups, and where you left off when you come back to a menu. Picking a hero moves the focus
to Start Game, picking a save to Load (or Save, in the game); past the last save, down scrolls the list. Quit
asks first. NPC conversations keep the focus on their answers. After using the mouse, the first pad press only
shows the focus again.

In grids, focus jumps item to item; while an item is picked up it moves cell by cell so you can place it, and
B, Start or Back put it back first (a panel never closes with an item on the cursor). On the Combat Arts and
Combo pages, A on an art and then the button that should cast it (X / Y / B / RB / RT; only the slots your
level has unlocked) puts it on the hotbar; Y on a weapon and then a button does the same for the weapon slots
(LT + that button). A weapon the slot held before goes into free space in the backpack. Merchants, smiths and
combat masters are targets in the world: walk toward one and press A.

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
- Not verified in a live game: mounting/dismounting (no horse near the test saves), chests, multiplayer
  screens.
- The quest log's quest entries aren't focus stops yet (the right-stick cursor reaches them).
- Y only offers weapons (bows and staves included); shields aren't offered for weapon slots.
- In shops the legend is hidden: the merchant's tabs sit where it would go.
- The game's own tooltips still say "right mouse button" (X on the pad).
- No button remapping yet.
- `sacredpad.log` next to the game says what the mod did.

## Development

`PLAN.md` has the task list and the reverse-engineering findings (addresses are for the Steam
2.0.2.28 exe). `./build.sh` builds with llvm-mingw from `tools/`. The `test/` scripts run a sandboxed,
DRM-free copy on Hyprland workspace 5 (`test/start.sh new|load|daemon`; `PROTON=1` for Proton, `REAL=1` for
the real install) and drive it through a control file (`SACREDPAD_CTL`, or
`sacredpad_ctl.txt` in the game folder) with tokens like `LX=1 A`, `CX=512 CY=288 A`, `LOCK=<ref>`, `FAR`, `DUMP`, `FOCUS` (focus list),
`UISCAN` (controls found in open windows' memory; `test/uiscan.sh` names their classes).
