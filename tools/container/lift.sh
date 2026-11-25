#!/bin/bash
# Recompiles the user's grid_game.exe (mounted at /game) into C, with the tree's recompiler.
source "$(dirname "$0")/common.sh"
cd "$TREE/Dirt3"
exe=/game/grid_game.exe
log "recompiler"
cmake -S . -B build/host-tools -G Ninja -DCMAKE_BUILD_TYPE=Release -DDIRT3_TOOLS_ONLY=ON > "$OUT.lift-cmake.log" 2>&1 ||
    { tail -20 "$OUT.lift-cmake.log"; exit 1; }
cmake --build build/host-tools --target recomp -j"$(nproc)" > "$OUT.lift-build.log" 2>&1 || { tail -30 "$OUT.lift-build.log"; exit 1; }

# GRID has its own entry and hook lists (cfgGRID); the DiRT ones do not apply.
want="$(sha256sum "$exe" | cut -c1-64) $(cat cfgGRID/*.txt | sha256sum | cut -c1-16) $(sha256sum build/host-tools/tools/recomp/recomp | cut -c1-16) $(cat .ps5-port-stamp)"
if [ -f generated/.lift-stamp ] && [ "$(cat generated/.lift-stamp)" = "$want" ] && [ -f generated/sources.txt ]; then
    echo "lift: already done"; exit 0
fi
log "recompiling the game (a few minutes)"
rm -rf generated
build/host-tools/tools/recomp/recomp lift "$exe" --out generated \
    --entries cfgGRID/entries.txt --hooks cfgGRID/hooks.txt --include-dead > "$OUT.lift.log" 2>&1 ||
    { tail -30 "$OUT.lift.log"; exit 1; }
echo "$want" > generated/.lift-stamp
rm -rf "$OUT/ps5-obj/gen"      # lifted code changed: its objects are stale
echo "lift: $(ls generated/src/*.c | wc -l) source files"
