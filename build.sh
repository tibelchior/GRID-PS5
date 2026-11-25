#!/bin/bash
# Builds the PS5 title for Race Driver GRID from your own copy of the game and the DiRT 3
# recompilation source tree (this repository's GRID changes and PS5 port are applied to a copy of it).
#
#   ./build.sh            everything: toolchain (first time only), recompile, build, package
#   ./build.sh toolchain  only the PS5 toolchain (first time: 1 to 2 hours)
#   ./build.sh lift       only recompile the game executable
#   ./build.sh title      only build the title (needs the steps above)
#
# Options (environment):
#   GAME_DIR=<folder>     the folder that holds GRID.exe or the unpacked grid_game.exe (default: gamedata/GRID)
#   DIRT3_SRC=<folder>    the DiRT 3 recompilation source tree (default: ../dirt3-recomp-src)
#   GRID_SRC=<folder>     a tree that already has the GRID changes (cfgGRID/); used instead of DIRT3_SRC when set
#   COPY_GAME=1           also copy the game files (10 GB) into dist/; by default dist/ holds only the PS5 files
#                         and the game folder is copied to the console once, by hand
#   ALLOW_OTHER_EXE=1     do not stop when the unpacked exe is not the known GOG file
#   BUILD_VOLUME=<name>   Docker volume holding the toolchain and build trees (default dirt2ps5-build-data:
#                         the DiRT 2 PS5 port's volume, so its toolchain is reused; GRID builds in its own
#                         folders there, tree-grid and game-grid)
#
# Needs: Docker (Docker Desktop on macOS or Windows+WSL, or Docker on Linux), about 60 GB free,
# and the game: Race Driver GRID, PC (GOG). Nothing from the game is uploaded.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
source "$HERE/tools/pins.env"
STEP=${1:-all}
IMAGE=dirt3ps5-build VOLUME=${BUILD_VOLUME:-dirt2ps5-build-data} NAME=gridps5
die() { echo "error: $*" >&2; exit 1; }

command -v docker > /dev/null || die "Docker is not installed (https://www.docker.com/products/docker-desktop/)."
docker info > /dev/null 2>&1 || die "Docker is installed but not running. Start it and try again."

# --- the game ---------------------------------------------------------------------------
GAME=${GAME_DIR:-}
if [ -z "$GAME" ]; then
    for d in "$HERE/gamedata/GRID" "$HERE/gamedata/Race Driver GRID" "$HERE/gamedata"; do
        { [ -f "$d/grid_game.exe" ] || [ -f "$d/GRID.exe" ]; } && { GAME=$d; break; }
    done
fi
if [ "$STEP" = toolchain ]; then GAME=$HERE/gamedata; fi
[ -n "$GAME" ] || die "no game found. Copy your GRID folder (the one with GRID.exe) into gamedata/, or set GAME_DIR."
if [ "$STEP" != toolchain ]; then
    # GRID.exe is UPX-packed; the recompiler needs the unpacked file, kept as grid_game.exe in the game folder.
    if [ ! -f "$GAME/grid_game.exe" ]; then
        [ -f "$GAME/GRID.exe" ] || die "$GAME/GRID.exe not found."
        command -v upx > /dev/null || die "GRID.exe is UPX-packed: install upx (brew install upx) or unpack it yourself (upx -d GRID.exe -o grid_game.exe) and put grid_game.exe in the game folder."
        mkdir -p "$HERE/work"
        upx -d -q -o "$HERE/work/grid_game.exe" "$GAME/GRID.exe" > /dev/null || die "upx could not unpack GRID.exe"
        UNPACKED=$HERE/work/grid_game.exe
        EXE=$UNPACKED
    else
        EXE=$GAME/grid_game.exe
    fi
    sha=$( (shasum -a 256 "$EXE" 2> /dev/null || sha256sum "$EXE") | cut -d' ' -f1)
    if [ "$sha" != "$GAME_EXE_SHA256" ] && [ "${ALLOW_OTHER_EXE:-0}" != 1 ]; then
        die "the unpacked GRID exe is not the expected GOG file (sha256 $sha). The recompiler only fits that version; set ALLOW_OTHER_EXE=1 to try anyway."
    fi
    UNPACKED=$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$EXE")   # also resolves a symlinked game folder
    echo "game: $GAME (exe $UNPACKED)"
fi

# --- the source tree -----------------------------------------------------------------
# GRID_SRC: a tree that already has the GRID changes. Otherwise the DiRT 3 tree, to which the container
# applies ps5/patches/grid first.
SRC=${GRID_SRC:-}
if [ -z "$SRC" ]; then SRC=${DIRT3_SRC:-$HERE/../dirt3-recomp-src}; fi
[ -f "$SRC/CMakeLists.txt" ] && [ -d "$SRC/tools/recomp" ] || die "recompilation source tree not found at $SRC (set DIRT3_SRC)."
SRC=$(cd "$SRC" && pwd)
echo "source: $SRC"

# --- the container ----------------------------------------------------------------------
echo "== build image"
docker build -q -t "$IMAGE" "$HERE/docker" > /dev/null
docker volume create "$VOLUME" > /dev/null
docker rm -f "$NAME" > /dev/null 2>&1 || true
mkdir -p "$HERE/dist" "$HERE/art" "$HERE/cache"
# The container sees the game folder at /game and, when GRID.exe was unpacked here, the unpacked exe on top of it.
EXTRA=()
[ -n "${UNPACKED:-}" ] && EXTRA=(-v "$UNPACKED":/game/grid_game.exe:ro)   # mounted over any (possibly symlinked) copy
docker run -d --name "$NAME" -v "$VOLUME":/build -v "$HERE":/work -v "$GAME":/game:ro ${EXTRA[@]+"${EXTRA[@]}"} -v "$SRC":/src:ro "$IMAGE" sleep infinity > /dev/null
trap 'docker rm -f "$NAME" > /dev/null 2>&1 || true' EXIT
run() { docker exec -e VOL=/build "$NAME" bash "/work/tools/container/$1.sh"; }

case $STEP in
    toolchain) run toolchain ;;
    lift) run sources; run lift ;;
    title) run toolchain; run sources; run lift; run title ;;
    all) run toolchain; run sources; run lift; run title ;;
    *) die "unknown step '$STEP' (all, toolchain, lift, title)" ;;
esac
[ "$STEP" = toolchain ] || [ "$STEP" = lift ] || {
    OUTDIR=$HERE/dist/$TITLE_ID
    [ -f "$OUTDIR/eboot.bin" ] || die "the build did not produce $OUTDIR/eboot.bin"
    if [ "${COPY_GAME:-0}" = 1 ]; then
        echo "== copying the game files into the title (about 10 GB)"
        mkdir -p "$OUTDIR/GRID"
        rsync -aL --exclude '*.lnk' --exclude '_dxmt.log' "$GAME/" "$OUTDIR/GRID/"
        cp "$UNPACKED" "$OUTDIR/GRID/grid_game.exe"
    fi
    echo
    echo "Done: $OUTDIR"
    echo "Copy it to the console as /data/homebrew/$TITLE_ID (see Install on the console in README.md)."
}
