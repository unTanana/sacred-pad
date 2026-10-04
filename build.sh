#!/bin/sh
# Build sacredpad as a proxy ijl15.dll (32-bit).
cd "$(dirname "$0")" && mkdir -p build &&
tools/llvm-mingw/bin/i686-w64-mingw32-clang -O2 -Wall -shared -s -static -o build/ijl15.dll src/sacredpad.c src/ijl15.def -lgdi32
