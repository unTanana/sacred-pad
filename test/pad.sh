#!/bin/sh
# Hold a pad state for N seconds, then return to an idle state.
# usage: pad.sh "CX=512 CY=415 A" [seconds=0.15] [idle-state]
C=$(dirname "$0")/../logs/ctl.txt
echo "$1" > "$C"; sleep "${2:-0.15}"; echo "${3:-}" > "$C"
