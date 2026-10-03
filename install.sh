#!/bin/sh
# Install sacredpad into the Steam copy of Sacred Gold: the game's ijl15.dll becomes ijl15_real.dll and the
# controller DLL takes its name. Game files are otherwise untouched. Undo with ./uninstall.sh.
set -e
G=${1:-"$HOME/.local/share/Steam/steamapps/common/Sacred Gold"}
[ -f "$G/Sacred.exe" ] || G="/mnt/storage/steam/SteamLibrary/steamapps/common/Sacred Gold"
[ -f "$G/Sacred.exe" ] || { echo "Sacred Gold not found; pass its folder as the first argument"; exit 1; }
cd "$(dirname "$0")" && ./build.sh
[ -f "$G/ijl15_real.dll" ] || cp "$G/ijl15.dll" "$G/ijl15_real.dll"
cp build/ijl15.dll "$G/ijl15.dll"
echo "Installed into $G"
