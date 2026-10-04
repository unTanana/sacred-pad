#!/bin/sh
WINEPREFIX=$(cd "$(dirname "$0")/.." && pwd)/prefix wineserver -k
# Proton-launched copies (sandbox or the real install): match the exe path, never this script's own command line.
ps -eo pid,args | awk '$2 ~ /^[A-Za-z]:\\/ && /Sacred[.]exe/ {print $1}' | xargs -r kill 2>/dev/null
