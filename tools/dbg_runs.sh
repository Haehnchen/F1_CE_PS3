#!/bin/sh
# Parallel debug runs up to the profile-creation 3D scene.
# usage: tools/dbg_runs.sh "<surfaces>" "NAME1:VAR=val VAR2=val" "NAME2:VAR=val" ...
# Each run gets its own directory dbg/<NAME>; the latest dump of every surface is converted to PNG.
cd "$(dirname "$0")/.." || exit 1
GAME="C:/Users/user/Desktop/Repos/F1_CE_PS3/Formula One - Championship Edition (Europe) (En,Fr,De,Es,It,Pt,Fi)/PS3_GAME"
SURF="$1"; shift
for spec in "$@"; do
    name="${spec%%:*}"; vars="${spec#*:}"
    d="dbg/$name"; rm -rf "$d"; mkdir -p "$d"; cp -r run/ps3_data "$d/"
    (cd "$d" && env $vars F1CE_DUMP_SURFACES="$SURF" F1CE_INPUT="2:cross,12:start,15:start,18:start,21:cross,25:down,27:cross" \
        timeout "${DBG_TIME:-75}" ../../build-win/f1ce.exe "$GAME" > log.txt 2>&1
     for s in $(ls frame_*.argb 2>/dev/null | sed 's/_[0-9]*_[0-9x]*\.argb$//' | sort -u); do
         ls ${s}_[0-9]*.argb | head -n -1 | xargs rm -f
     done
     python -I ../../tools/frames_to_png.py . > /dev/null) &
done
wait
ls dbg/*/*.png
