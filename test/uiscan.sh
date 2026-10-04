#!/bin/sh
# Log the UI controls found on screen right now (vtables named via re/vtables.txt).
L=$(dirname "$0")/../sandbox/sacredpad.log
n=$(wc -l < "$L"); $(dirname "$0")/pad.sh UISCAN 0.05; sleep 0.3
tail -n +$((n+1)) "$L" | grep " scan" | python3 -c "
import sys,re
names={int(l.split()[0],16):l.split()[1] for l in open('$(dirname "$0")/../re/vtables.txt')}
for l in sys.stdin:
    l=l.rstrip().split(' scan',1)[1]
    print(re.sub(r'vt=([0-9A-F]{8})', lambda m: names.get(int(m.group(1),16), m.group(0)), l))
"
