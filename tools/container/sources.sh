#!/bin/bash
# The recompilation tree (mounted read-only at /src) with this repository's changes applied, DXVK
# prepared, and the third-party libraries fetched. /src is either the DiRT 3 recompilation tree
# (the GRID changes in ps5/patches/grid are applied first) or a tree that already has them (it has
# cfgGRID/). Then the PS5 port follows: ps5/patches/dirt3 and ps5/overlay.
source "$(dirname "$0")/common.sh"
mkdir -p "$TREE" && cd "$TREE"

log "recompilation sources"
[ -f /src/CMakeLists.txt ] || { echo "the recompilation source tree is not mounted at /src (set DIRT3_SRC or GRID_SRC)" >&2; exit 1; }
# (cfgGRID is missing in the DiRT 3 tree: find then exits 1, which must not stop the script)
src_hash=$(cd /src && { find CMakeLists.txt CMakePresets.json cfgGRID platform runtime shims tools third_party/patches \
    -type f ! -name '.DS_Store' 2>/dev/null || true; } | sort | xargs cat | sha256sum | cut -c1-16)
port_hash=$( { find /work/ps5/overlay -type f ! -name '.DS_Store' | sort | xargs cat; cat /work/ps5/patches/grid/*.patch /work/ps5/patches/dirt3/*.patch; } | sha256sum | cut -c1-16)
want="$src_hash $port_hash"
mkdir -p Dirt3
if [ ! -f Dirt3/.ps5-port-stamp ] || [ "$(cat Dirt3/.ps5-port-stamp)" != "$want" ]; then
    # Fresh copy of the source (keeps build/ and generated/, which are outputs).
    rsync -a --delete --exclude /build --exclude /generated --exclude '/generated_*' --exclude /.claude --exclude .DS_Store \
        --exclude /third_party/dxvk --exclude /third_party/MoltenVK --exclude /third_party/dxmt --exclude /.git --exclude .git --exclude .ps5-port-stamp /src/ Dirt3/
    cd Dirt3
    if [ ! -d cfgGRID ]; then
        log "applying the GRID changes to the DiRT 3 tree"
        for p in /work/ps5/patches/grid/*.patch; do patch -s -p1 --no-backup-if-mismatch < "$p"; done
    fi
    log "applying the PS5 port"
    for p in /work/ps5/patches/dirt3/*.patch; do patch -s -p1 --no-backup-if-mismatch < "$p"; done
    cp -R /work/ps5/overlay/Dirt3/. .
    echo "$want" > .ps5-port-stamp
    cd ..
fi
cd Dirt3

log "DXVK"
# DXVK 3.1.1 with its submodules, at the commit the source tree's submodule uses; prepare_sources.sh
# then exports a patched copy into build/src/dxvk.
if [ "$(git -C third_party/dxvk rev-parse HEAD 2>/dev/null)" != "$DXVK_COMMIT" ]; then
    rm -rf third_party/dxvk
    git clone -q "$DXVK_REPO" third_party/dxvk
    git -C third_party/dxvk -c advice.detachedHead=false checkout -q "$DXVK_COMMIT"
    git -C third_party/dxvk submodule update --init --recursive -q
fi
bash tools/scripts/prepare_sources.sh --dxvk-only

log "libraries (SDL3, OpenAL Soft, Ogg, Vorbis, FFmpeg)"
# The script was written for macOS: GNU tar cannot detect compression on a pipe, bsdtar can.
mkdir -p "$TREE/.shim" && ln -sf "$(command -v bsdtar)" "$TREE/.shim/tar"
rm -rf build/deps/src.tmp build/deps/src.lock     # left behind if an earlier run was interrupted
PATH="$TREE/.shim:$PATH" bash tools/scripts/build_deps.sh --fetch-only

log "build scripts"
mkdir -p "$TREE/ps5-spike"
rsync -a --exclude title/sce_sys --exclude shipped /work/ps5/ "$TREE/ps5-spike/"
mkdir -p "$TREE/ps5-spike/shipped"
rm -f "$TREE"/ps5-spike/shipped/*
for f in grid.dxvk.bin grid.dxvk.lut grid.pipelines; do
    [ -f "/work/cache/$f" ] && cp "/work/cache/$f" "$TREE/ps5-spike/shipped/$f"
done
echo "shader cache: $(ls "$TREE/ps5-spike/shipped" | wc -l) of 3 files found in cache/"
