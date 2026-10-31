#!/bin/sh
# Menu test with a custom input script: boots with the save data in dbg/menu/ps3_data (profile exists) and runs
# F1CE_INPUT=$IN for TIMEOUT seconds (default 90).
# usage: IN="2:cross,12:start,16:cross,..." tools/menu_test.sh <run dir name> [VAR=value ...]
# e.g. Quick Race -> DRIVER: IN="2:cross,12:start,16:cross,20:cross,24:cross,30:down,33:cross"
cd "$(dirname "$0")/.." || exit 1
GAME="C:/Users/user/Desktop/Repos/F1_CE_PS3/F1_CE_PS3_og/PS3_GAME"
name="$1"; shift
d="dbg/$name"; rm -rf "$d"; mkdir -p "$d"; cp -r dbg/menu/ps3_data "$d/"
cd "$d" && env "$@" F1CE_INPUT="$IN" timeout ${TIMEOUT:-90} ../../build-win/f1ce.exe "$GAME" > log.txt 2>&1
rc=$?; cp f1ce_recomp.log log.txt 2>/dev/null   # (the console only gets warnings/errors)
echo "exit $rc"
python -I ../../tools/frames_to_png.py . > /dev/null
grep -E "FATAL" log.txt | cut -c16-
