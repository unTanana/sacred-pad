#!/bin/sh
# Fresh launch straight into the world: Ancaria campaign, Gladiator, tutorial skipped.
T=$(dirname "$0")
$T/kill.sh 2>/dev/null; sleep 1; : > "$T/../logs/ctl.txt"
$T/run.sh; sleep 14
click() { $T/pad.sh "CX=$1 CY=$2 A" 0.2 "CX=$1 CY=$2"; sleep "$3"; }
click 512 288 3   # Ancaria Campaign
click 100 440 2   # Gladiator
click 607 713 8   # Start Game
click 609 612 2   # Skip Tutorial
: > "$T/../logs/ctl.txt"
