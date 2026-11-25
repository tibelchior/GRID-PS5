#!/bin/bash
# Builds DXVK native for PS5 as static libraries, from a copy of the repo's patched
# DXVK source (Dirt3/build/src/dxvk; the Mac build's copy is never edited), with the
# Switch port's static-build patch and ps5-spike/patches/dxvk on top.
# Output: ps5-dxvk/libdxvk_ps5.a, one archive with DXVK's internal libraries merged in.
# Needs build_ps5_deps.sh first: DXVK compiles against SDL3's headers.
# Usage: build_ps5_dxvk.sh [d3d9|all]    d3d9: D3D9 only (default); all: D3D9, DXGI and D3D11
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
# PS5_OUT: where build output goes (default: beside the sources); PS5_SDK_DIR: the payload SDK.
# The Linux container sets both, to build against PS5_Vulkan's pinned SDK without touching the Mac's output.
OUTROOT=${PS5_OUT:-$ROOT}
export PS5_PAYLOAD_SDK=${PS5_SDK_DIR:-$ROOT/ps5-sdk}
export LLVM_CONFIG=${LLVM_CONFIG:-/opt/homebrew/opt/llvm@18/bin/llvm-config}
MODE=${1:-d3d9}
SRC=$OUTROOT/ps5-src/dxvk
BLD=$OUTROOT/ps5-dxvk/$MODE
BASE=$ROOT/Dirt3/build/src/dxvk
[ -d "$BASE" ] || { echo "run Dirt3/tools/scripts/prepare_sources.sh first" >&2; exit 1; }
if [ ! -d "$SRC" ]; then
    mkdir -p "$OUTROOT/ps5-src"
    cp -R "$BASE" "$SRC"
    # The Switch port's static, loader-less DXVK, then its guards extended to the PS5.
    patch -s -p1 -d "$SRC" < "$ROOT/Dirt3/third_party/patches/dxvk-switch/0001-static-horizon-native.patch"
    for p in "$ROOT"/ps5-spike/patches/dxvk/*.patch; do
        [ -f "$p" ] && patch -s -p1 -d "$SRC" < "$p"
    done
fi

CROSS=$OUTROOT/ps5-dxvk/ps5.cross
mkdir -p "$OUTROOT/ps5-dxvk"
cat > "$CROSS.new" <<EOF
[binaries]
c = '$PS5_PAYLOAD_SDK/bin/prospero-clang'
cpp = '$PS5_PAYLOAD_SDK/bin/prospero-clang++'
ar = '$PS5_PAYLOAD_SDK/bin/prospero-ar'
strip = '$PS5_PAYLOAD_SDK/bin/prospero-strip'
pkg-config = 'pkg-config'

[built-in options]
c_args = ['-I$OUTROOT/ps5-deps/include']
cpp_args = ['-I$OUTROOT/ps5-deps/include']

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$OUTROOT/ps5-deps/lib/pkgconfig'

[host_machine]
system = 'freebsd'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
cmp -s "$CROSS.new" "$CROSS" && rm "$CROSS.new" || mv "$CROSS.new" "$CROSS"

case $MODE in
    d3d9) FEATURES=(-Denable_dxgi=false -Denable_d3d11=false) ;;
    all) FEATURES=(-Denable_dxgi=true -Denable_d3d11=true) ;;
    *) echo "usage: $0 [d3d9|all]" >&2; exit 2 ;;
esac
[ -d "$BLD" ] || (cd "$SRC" && meson setup "$BLD" --cross-file "$CROSS" --buildtype=release \
    -Denable_d3d9=true -Denable_d3d8=false -Denable_d3d10=false "${FEATURES[@]}" \
    -Dnative_sdl3=enabled -Dnative_sdl2=disabled -Dnative_glfw=disabled)
ninja -C "$BLD"
# One archive for the title: every DXVK static library, objects with equal names kept apart.
OUT=$BLD/libdxvk_ps5.a
rm -f "$OUT"
{
    echo "CREATE $OUT"
    find "$BLD" -name '*.a' ! -path "$OUT" | sort | sed 's/^/ADDLIB /'
    echo SAVE
    echo END
} | "$PS5_PAYLOAD_SDK/bin/llvm-ar" -M
echo "DXVK (ps5, $MODE): $OUT"
