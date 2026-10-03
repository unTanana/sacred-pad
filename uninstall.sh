#!/bin/sh
# Restore the game's own ijl15.dll.
set -e
G=${1:-"$HOME/.local/share/Steam/steamapps/common/Sacred Gold"}
[ -f "$G/Sacred.exe" ] || G="/mnt/storage/steam/SteamLibrary/steamapps/common/Sacred Gold"
[ -f "$G/ijl15_real.dll" ] || { echo "Not installed in $G"; exit 0; }
mv -f "$G/ijl15_real.dll" "$G/ijl15.dll"
rm -f "$G/sacredpad.log"
echo "Uninstalled from $G"
