#!/bin/sh
# Run the installed game through Proton directly (Steam must be running for the DRM), with PROTON_LOG,
# on workspace 5 without focus, muted.
L=$(cd "$(dirname "$0")/../logs" && pwd)
G="${GAME:-/mnt/storage/steam/SteamLibrary/steamapps/common/Sacred Gold}"
S="$HOME/.local/share/Steam"
hyprctl eval 'hl.window_rule({ match = { class = "^steam_app_12320$" }, workspace = "5 silent", float = true, size = { "window_w", "window_h" },
  no_initial_focus = true, focus_on_activate = false,
  suppress_event = "fullscreen maximize activate activatefocus" })' >/dev/null
cd "$G" && STEAM_COMPAT_DATA_PATH=/mnt/storage/steam/SteamLibrary/steamapps/compatdata/12320 \
  STEAM_COMPAT_CLIENT_INSTALL_PATH="$S" SteamAppId=12320 SteamGameId=12320 PROTON_LOG=1 \
  PROTON_LOG_DIR="$L" PULSE_SINK=nonexistent_sink_mute \
  setsid "$S/steamapps/common/Proton 11.0/proton" run "$G/Sacred.exe" > "$L/proton-run.log" 2>&1 < /dev/null &
