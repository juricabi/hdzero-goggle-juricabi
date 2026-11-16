#!/bin/bash
# Emulator (SDL) build of this worktree with the host compiler.
#   utilities/dev/emu_build.sh <goggle|goggle2|boxpro>      -> $EMU_OUT/build_<target>/HDZGOGGLE
# EMU_OUT defaults to /root/hdz/emu.  Run the result only through emu_run.sh (the emulator runs the goggle's
# shell commands on the host).
set -u
T=${1:?usage: $0 goggle|goggle2|boxpro}
case $T in
  goggle) opt="-DHDZ_GOGGLE=ON -DHDZ_GOGGLE2=OFF -DHDZ_BOXPRO=OFF" ;;
  goggle2) opt="-DHDZ_GOGGLE=OFF -DHDZ_GOGGLE2=ON -DHDZ_BOXPRO=OFF" ;;
  boxpro) opt="-DHDZ_GOGGLE=OFF -DHDZ_GOGGLE2=OFF -DHDZ_BOXPRO=ON" ;;
  *) echo "usage: $0 goggle|goggle2|boxpro"; exit 2 ;;
esac
SRC=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${EMU_OUT:-/root/hdz/emu}
B=$OUT/build_$T
mkdir -p "$B"
if [ ! -f "$B/Makefile" ]; then
  cmake -S "$SRC" -B "$B" -DEMULATOR_BUILD=ON -DCMAKE_BUILD_TYPE=Debug $opt > "$B.cmake.log" 2>&1 ||
    { echo "cmake failed"; tail -20 "$B.cmake.log"; exit 1; }
fi
nice -n 5 make -C "$B" -j"$(nproc)" HDZGOGGLE > "$B.make.log" 2>&1
r=$?
echo "build $T: exit $r, warnings $(grep -c 'warning:' "$B.make.log")"
[ $r -eq 0 ] || grep -B2 -A6 'error' "$B.make.log" | head -60
ls -la "$B/HDZGOGGLE" 2> /dev/null
exit $r
