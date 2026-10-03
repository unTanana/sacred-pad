#!/bin/sh
# Fresh launch, then load the first save slot (the Daemon in Ancaria's graveyard).
T=$(dirname "$0")
$T/kill.sh 2>/dev/null; sleep 1; : > "$T/../logs/ctl.txt"
$T/run.sh; sleep 14
click() { $T/pad.sh "CX=$1 CY=$2 A" 0.2 "CX=$1 CY=$2"; sleep "$3"; }
click 512 415 2   # Load Game
click 453 322 1   # first save slot
click 867 303 1   # Load
click 420 438 12  # confirm
: > "$T/../logs/ctl.txt"
