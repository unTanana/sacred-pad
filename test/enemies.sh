#!/bin/sh
# Nearest enemies right now: distance from the hero (px), ref, screen position.
L=$(dirname "$0")/../sandbox/sacredpad.log
n=$(wc -l < "$L"); $(dirname "$0")/pad.sh DUMP 0.02; sleep 0.3
tail -n +$((n+1)) "$L" | grep "dump ref=.*kind=1" | sort -u | awk '{split($6,a,"[=,]"); dx=a[2]-512; dy=a[3]-384; print int(sqrt(dx*dx+dy*dy)), $3, $6, $7}' | sort -n | head -${1:-5}
