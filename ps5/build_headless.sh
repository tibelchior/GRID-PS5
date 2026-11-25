#!/bin/bash
# Builds the whole game for PS5 (runtime, lifted code, shims, launcher, SDL3, OpenAL, FFmpeg,
# DXVK), in one of two forms. Needs build_ps5_deps.sh and "build_ps5_dxvk.sh all" first.
#
#   default   dirt3-headless.elf: a payload for an ELF loader, with no Vulkan driver. It runs
#             until the game asks for Direct3D.
#   --title   an installed title, linked with RADV (mihawk-99/PS5_Vulkan): the folder
#             title/<TITLE_ID> with eboot.bin, sce_sys and sce_module. Linux container only
#             (PS5_VULKAN_DIR, default /build/PS5_Vulkan, with RADV and its tools built).
#
# Usage: build_headless.sh [--title] [--gen] [--small]
#   --gen    also recompiles the generated sources
#   --small  lifted code built for size (-Oz, slower); the payload ELF is also stripped, to
#            stay under the 128 MiB that some ELF senders accept
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SPIKE=$ROOT/ps5-spike
REPO=$ROOT/Dirt3
# PS5_OUT: where build output goes (default: beside the sources); PS5_SDK_DIR: the payload SDK.
# The Linux container sets both, to build against PS5_Vulkan's pinned SDK without touching the Mac's output.
OUTROOT=${PS5_OUT:-$ROOT}
export PS5_PAYLOAD_SDK=${PS5_SDK_DIR:-$ROOT/ps5-sdk}
DEPS=$OUTROOT/ps5-deps
DXVK_SRC=$OUTROOT/ps5-src/dxvk
DXVK_LIB=$OUTROOT/ps5-dxvk/all/libdxvk_ps5.a
export LLVM_CONFIG=${LLVM_CONFIG:-/opt/homebrew/opt/llvm@18/bin/llvm-config}
CXX=$PS5_PAYLOAD_SDK/bin/prospero-clang++
# Objects, logs and the ELF stay in ps5-spike on the Mac; with PS5_OUT they go there.
OBJ=$SPIKE/obj LOG=$SPIKE/headless-logs ELFDIR=$SPIKE
if [ -n "${PS5_OUT:-}" ]; then OBJ=$PS5_OUT/ps5-obj LOG=$PS5_OUT/ps5-logs ELFDIR=$PS5_OUT; fi
REGEN=0 SMALL=0 TITLE=0
for a in "$@"; do
    case $a in --gen) REGEN=1 ;; --small) SMALL=1 ;; --title) TITLE=1 ;; *) echo "unknown option $a" >&2; exit 2 ;; esac
done
GEN=$OBJ/gen OUT=$ELFDIR/dirt3-headless.elf
export GEN_OPT=-O2
if [ $SMALL = 1 ]; then GEN=$OBJ/gen-small OUT=$ELFDIR/dirt3-headless-small.elf GEN_OPT=-Oz; fi
VK=${PS5_VULKAN_DIR:-/build/PS5_Vulkan}
RADV=$VK/.deps/native/radv-release
if [ $TITLE = 1 ]; then
    for f in "$RADV/lib/libvulkan_radeon.ps5.a" "$VK/build/host/ps5-native-tool" "$VK/runtime/libc.prx" "$VK/tools/radv-link.sh"; do
        [ -e "$f" ] || { echo "missing $f: --title needs PS5_Vulkan with RADV and its tools built" >&2; exit 2; }
    done
fi
cd "$REPO"
mkdir -p "$OBJ/runtime" "$GEN" "$OBJ/shims" "$LOG"

if [ $REGEN = 1 ] || [ "$(ls "$GEN" | grep -c '\.o$')" != "$(ls generated/src/*.c | wc -l)" ]; then
    echo "== generated code ($GEN_OPT)"
    export SPIKE_OBJ=$GEN
    ls generated/src/*.c | xargs -P 12 -n 1 "$SPIKE/compile_gen.sh" > "$LOG/gen_fail.txt" 2>&1
    ! grep -q FAIL "$LOG/gen_fail.txt" || { echo "generated sources failed, see $LOG/gen_fail.txt" >&2; exit 1; }
fi

COMMON=(-std=gnu++20 -O2 -g -DNDEBUG -Wall -Wno-unused-parameter -Wno-unused-function)
DX=(-isystem "$DXVK_SRC/include/native/directx" -isystem "$DXVK_SRC/include/native/windows")
rm -f "$LOG"/*.failed
compile() {  # object-name source flags...   (may run in the background: failure is a marker file)
    local name=$1 src=$2
    shift 2
    if ! "$CXX" "${COMMON[@]}" "$@" -c "$src" -o "$OBJ/$name.o" > "$LOG/$(basename "$name").log" 2>&1; then
        echo "FAIL $src ($(grep -c 'error' "$LOG/$(basename "$name").log") error lines)"
        : > "$LOG/$(basename "$name").failed"
    fi
}

echo "== runtime"
for f in runtime/src/*.cpp; do compile "runtime/$(basename "$f" .cpp)" "$f" -Iruntime/include & done
wait

echo "== shims"
# shims/CMakeLists.txt's list. No touch controller: the console has a pad.
SHIMS="core vfs kernel32/sync kernel32/memory kernel32/file kernel32/process kernel32/locale user32/user32
    misc/misc misc/envmap_list misc/dirt3_stubs misc/grid_stubs steam/steam_offline xlive/xlive ws2_32/ws2_32 d3d9/bridge d3d9/conformance frontend/menu_patch frontend/glyph_patch frontend/grid_stats_row
    d3d11/reflect d3d11/compiler audio/dsound input/dinput8 input/xinput d3dx9/d3dx9 video/bink video/bink_decoder
    audio/openal audio/vorbisfile"
for s in $SHIMS; do
    compile "shims/$(echo "$s" | tr / _)" "shims/$s.cpp" -DAL_LIBTYPE_STATIC -I"$DEPS/include" -Iruntime/include \
        "${DX[@]}" -isystem "$DEPS/include/AL" &
done
wait
# The D3D11/DXGI bridge, on DXVK's D3D11 (as on Switch and Android).
compile shims/d3d11_bridge shims/d3d11/bridge.cpp -DNOMINMAX -D_WIN32_WINNT=0xa00 -DDIRT3_D3D11_DXVK=1 \
    -Ishims -Iruntime/include "${DX[@]}" -I"$DEPS/include"

echo "== launcher and platform"
# DiRT 3 runs its DX9 renderer (the path tested on macOS); DIRT3_RENDERER=dx11 is not supported yet.
compile sdl_main platform/sdl/sdl_main.cpp -DAL_LIBTYPE_STATIC -I"$DEPS/include" -Iruntime/include \
    "${DX[@]}" -isystem "$DEPS/include/AL"
compile ps5_platform platform/ps5/ps5_platform.cpp -Iruntime/include
if [ $TITLE = 1 ]; then
    compile ps5_vulkan platform/ps5/ps5_vulkan.cpp -Iruntime/include -I"$RADV/include"
    compile ps5_console platform/ps5/ps5_console.cpp -Iruntime/include
    VULKAN_OBJ="$OBJ/ps5_vulkan.o $OBJ/ps5_console.o"
else
    compile headless_vulkan "$SPIKE/headless_vulkan.cpp"
    VULKAN_OBJ=$OBJ/headless_vulkan.o
fi
"$PS5_PAYLOAD_SDK/bin/prospero-clang" -O2 -c "$SPIKE/ps5_link_compat.c" -o "$OBJ/ps5_link_compat.o" || : > "$LOG/ps5_link_compat.failed"
[ -z "$(ls "$LOG"/*.failed 2>/dev/null)" ] || { echo "compile errors: see $LOG" >&2; exit 1; }

ls "$GEN"/*.o "$OBJ"/runtime/*.o "$OBJ"/shims/*.o "$OBJ/sdl_main.o" "$OBJ/ps5_platform.o" $VULKAN_OBJ \
    "$OBJ/ps5_link_compat.o" | sed 's/.*/"&"/' > "$OBJ/objects.rsp"
LIBS=("$DXVK_LIB" "$DEPS/lib/libopenal.a" "$DEPS/lib/libSDL3.a"
    "$DEPS/lib/libavformat.a" "$DEPS/lib/libavcodec.a" "$DEPS/lib/libavutil.a"
    "$DEPS/lib/libvorbisfile.a" "$DEPS/lib/libvorbis.a" "$DEPS/lib/libogg.a")
READELF=$(dirname "$LLVM_CONFIG")/llvm-readelf
# The ELF must hold only the relocation types the first probe ran with: the payload loader
# rejected R_X86_64_64 against an import ("not a valid ELF payload").
check_relocations() {
    local odd
    odd=$("$READELF" -rW "$1" | awk '$3 ~ /^R_X86_64/ && $3 != "R_X86_64_RELATIVE" && $3 != "R_X86_64_GLOB_DAT" && $3 != "R_X86_64_JUMP_SLOT" {print $3, $5}' | sort | uniq -c)
    [ -z "$odd" ] || { echo "relocations the console's loader may reject:" >&2; echo "$odd" >&2; exit 1; }
}

if [ $TITLE = 0 ]; then
    echo "== link (payload)"
    WRAP=
    for s in malloc calloc realloc free posix_memalign aligned_alloc memalign malloc_usable_size reallocf reallocarray getline getdelim; do
        WRAP="$WRAP -Wl,--wrap=$s"
    done
    # open_memstream (DXVK's libdisplay-info): the console's libc has none; the platform layer's
    # needs fclose and fflush wrapped (ps5platform/libc.h).
    WRAP="$WRAP -Wl,--defsym=open_memstream=ps5_open_memstream -Wl,--wrap=fclose -Wl,--wrap=fflush"
    # No data pointers to imported functions (ps5_link_compat.c): the console's loader rejects them.
    WRAP="$WRAP -Wl,--wrap=fread -Wl,--wrap=ftell"
    "$CXX" -o "$OUT" @"$OBJ/objects.rsp" $WRAP "${LIBS[@]}" -lps5platform -lpthread > "$LOG/link.log" 2>&1 ||
        { echo "link failed: see $LOG/link.log" >&2; exit 1; }
    if [ $SMALL = 1 ]; then "$PS5_PAYLOAD_SDK/bin/llvm-strip" "$OUT"; fi
    check_relocations "$OUT"
    ls -la "$OUT"
    exit 0
fi

# --- The installed title: PS5_Vulkan's link recipe (its tools/build-radv-title.sh and the
# template's ps5/tools/link-title.sh), with the game's objects and libraries.
echo "== link (title, with RADV)"
SDK=$PS5_PAYLOAD_SDK
native=$VK/tooling/native
tool=$VK/build/host/ps5-native-tool
work=$OBJ/title
param=$SPIKE/title/param.json
mkdir -p "$work/stubs"
cc() { PS5_PAYLOAD_SDK="$SDK" sh "$VK/tooling/prospero-clang18" "$@"; }
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$native/app_crt.cpp" -o "$work/app_crt.o"
stub() {
    cc -std=c11 -O2 -fPIC -c "$VK/$2" -o "$work/${1}_stub.o"
    "$SDK/bin/prospero-lld" --shared -soname "${1}.prx" -o "$work/stubs/${1}.so" "$work/${1}_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c
# _setjmp and _longjmp (the lifted code's exception frames, the runtime's unwinding): the
# console's libc modules export only setjmp and longjmp. The payload SDK's static libc has
# them as one self-contained assembly object, taken alone: the rest of that libc is a payload's.
(cd "$work" && "$SDK/bin/llvm-ar" x "$SDK/target/lib/libc.a" _setjmp.o)
# POSIX functions no module a title loads exports (ps5_title_compat.c).
"$SDK/bin/prospero-clang" -O2 -Wall -c "$SPIKE/ps5_title_compat.c" -o "$work/ps5_title_compat.o"
# Its names stay local: the SDK's stub libraries define them too, a title that defined them
# visibly would export them to override the stubs, and the title converter refuses exports.
printf '{\n    local:\n' > "$work/compat-local.map"
for name in fnmatch isatty mkstemp readlink link symlink pathconf setsid vfork getresuid getresgid strdup strndup vasprintf asprintf __wrap_pthread_mutex_lock; do
    printf '        %s;\n' "$name" >> "$work/compat-local.map"
done
printf '};\n' >> "$work/compat-local.map"
source "$VK/tools/radv-link.sh"
radv_link_recipe "$VK" "$SDK" "$RADV/lib/libvulkan_radeon.ps5.a" || exit 2
"$SDK/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
    --wrap=fread --wrap=ftell --wrap=pthread_mutex_lock \
    --version-script "$native/app-symbols.map" --version-script "$work/compat-local.map" --exclude-libs=ALL \
    -e _start -o "$work/llvm-pie.elf" \
    "$work/app_crt.o" "$work/_setjmp.o" "$work/ps5_title_compat.o" @"$OBJ/objects.rsp" "${LIBS[@]}" \
    "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" \
    "${radv_link_inputs[@]}" \
    --as-needed "$SDK"/target/lib/*.so > "$LOG/link-title.log" 2>&1 ||
    { echo "title link failed: see $LOG/link-title.log" >&2; exit 1; }
check_relocations "$work/llvm-pie.elf"
# An import that no module a title loads exports is null at run time.
null_imports=$(comm -23 \
    <("$SDK/bin/llvm-nm" -D --undefined-only "$work/llvm-pie.elf" | awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) \
    <(for library in "$SDK"/target/lib/*.so "$work"/stubs/*.so; do
        case ${library##*/} in libkernel_sys.so | libScePosixForWebKit.so) continue ;; esac
        "$SDK/bin/llvm-nm" -D --defined-only "$library" 2>/dev/null | awk '{ print $NF }'
    done | sort -u))
if [ -n "$null_imports" ]; then
    echo "imports that no module a title loads exports (null at run time): $(echo $null_imports)" >&2
    exit 1
fi
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
    --stub-dir "$SDK/target/lib" --stub "$work/stubs/libSceAgc.so" --stub "$work/stubs/libSceAgcDriver.so" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf > "$LOG/tool-link.log" 2>&1 ||
    { echo "title conversion failed: see $LOG/tool-link.log" >&2; exit 1; }

title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$param")
app=$ELFDIR/title/$title_id
rm -rf "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
"$tool" self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F > "$LOG/tool-sign.log" 2>&1
"$tool" self --inspect --file "$app/eboot.bin" > /dev/null
cp "$param" "$app/sce_sys/param.json"
# The launcher tile and backgrounds: ps5-spike/title/sce_sys, made from the project's own
# artwork by PS5_Vulkan's tools/prepare-assets.sh. No selection music.
for asset in icon0.png pic0.dds pic1.dds; do
    [ -f "$SPIKE/title/sce_sys/$asset" ] || { echo "missing $SPIKE/title/sce_sys/$asset" >&2; exit 1; }
    cp "$SPIKE/title/sce_sys/$asset" "$app/sce_sys/$asset"
done
(cd "$VK/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$VK/runtime/libc.prx" "$app/sce_module/libc.prx"
# The shipped shader cache and pipeline list (ps5-spike/make_shipped_cache.sh): the game
# compiles the listed pipelines while it loads, instead of at their first draw.
for f in grid.dxvk.bin grid.dxvk.lut grid.pipelines; do
    [ -f "$SPIKE/shipped/$f" ] && cp "$SPIKE/shipped/$f" "$app/$f"
done
echo "title: $app ($(stat -c %s "$app/eboot.bin") bytes)"
