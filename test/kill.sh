#!/bin/sh
WINEPREFIX=$(cd "$(dirname "$0")/.." && pwd)/prefix wineserver -k
