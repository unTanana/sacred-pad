#!/bin/sh
# Fresh launch into the world with the imported level-29 Daemon (sandbox copy of the user's export).
T=$(dirname "$0")
$T/kill.sh 2>/dev/null; sleep 1; : > "$T/../logs/ctl.txt"
$T/run.sh; sleep 14
click() { $T/pad.sh "CX=$1 CY=$2 A" 0.2 "CX=$1 CY=$2"; sleep "$3"; }
click 512 288 3   # Ancaria Campaign
click 488 663 2   # Import
click 920 600 2   # Daemon
click 607 713 12  # Start Game
click 609 511 2   # Skip Tutorial
: > "$T/../logs/ctl.txt"
