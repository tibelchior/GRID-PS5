#!/bin/bash
# Builds the game's third-party libraries for PS5 as static libraries, from the
# sources the Dirt3 repo already fetched (Dirt3/build/deps/src):
#   SDL3 (no window system yet: dummy video and audio), libogg, libvorbis,
#   OpenAL Soft (through SDL3's audio) and FFmpeg cut down to Bink.
# Output: ps5-deps/{include,lib}; built in ps5-deps-work. Each library is built once;
# delete ps5-deps-work/<name>.done to rebuild it.
# Usage: build_ps5_deps.sh [sdl3 ogg vorbis openal ffmpeg]   (default: all)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/Dirt3/build/deps/src
# PS5_OUT: where build output goes (default: beside the sources); PS5_SDK_DIR: the payload SDK.
# The Linux container sets both, to build against PS5_Vulkan's pinned SDK without touching the Mac's output.
OUTROOT=${PS5_OUT:-$ROOT}
export PS5_PAYLOAD_SDK=${PS5_SDK_DIR:-$ROOT/ps5-sdk}
PREFIX=$OUTROOT/ps5-deps
BLD=$OUTROOT/ps5-deps-work
export LLVM_CONFIG=${LLVM_CONFIG:-/opt/homebrew/opt/llvm@18/bin/llvm-config}
JOBS=$(getconf _NPROCESSORS_ONLN)
WANT=${*:-sdl3 ogg vorbis openal ffmpeg}
mkdir -p "$PREFIX" "$BLD"
# Only the PS5 libraries are visible to configure steps: nothing from the host.
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig PKG_CONFIG_PATH=

SDL_VER=3.4.18
OPENAL_VER=1.25.2
OGG_VER=1.3.5
VORBIS_VER=1.3.7
FFMPEG_VER=7.1.1

wanted() { case " $WANT " in *" $1 "*) return 0 ;; esac; return 1; }

cmake_lib() {  # name srcdir [cmake args...]
    local name=$1 src=$2
    shift 2
    wanted "$name" || return 0
    [ -f "$BLD/$name.done" ] && { echo "== $name: already built"; return 0; }
    echo "== $name (ps5)"
    cmake -S "$src" -B "$BLD/$name" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DCMAKE_FIND_ROOT_PATH="$PREFIX;$PS5_PAYLOAD_SDK/target" \
        -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local" \
        -DCMAKE_VERBOSE_MAKEFILE=OFF -DBUILD_SHARED_LIBS=OFF "$@" > "$BLD/$name.log" 2>&1
    cmake --build "$BLD/$name" --target install >> "$BLD/$name.log" 2>&1
    touch "$BLD/$name.done"
}

# SDL3: threads, timers, events and the API surface the shims use. The console has no
# X11/Wayland/udev/dbus; video and audio are SDL's dummy drivers until PS5 ones exist.
# A copy of the pinned source with one change: the dummy display is 1920x1080 at 60 Hz, not
# 1024x768. It stands in for the console's display: the game's and DXVK's mode lists come from
# it, and DXVK scales the frame to the real 3840x2160 plane.
SDL_SRC=$OUTROOT/ps5-src/SDL3-$SDL_VER
if [ ! -d "$SDL_SRC" ]; then
    mkdir -p "$OUTROOT/ps5-src" && cp -R "$SRC/SDL3-$SDL_VER" "$SDL_SRC"
    sed -i.orig -e 's/mode\.w = 1024;/mode.w = 1920;/' -e 's/mode\.h = 768;/mode.h = 1080; mode.refresh_rate = 60.0f;/' \
        "$SDL_SRC/src/video/dummy/SDL_nullvideo.c"
    grep -q "mode.w = 1920;" "$SDL_SRC/src/video/dummy/SDL_nullvideo.c" || { echo "SDL dummy display patch did not apply" >&2; exit 1; }
    rm -rf "$BLD/sdl3" "$BLD/sdl3.done"
fi
cmake_lib sdl3 "$SDL_SRC" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF \
    -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF \
    -DSDL_VULKAN=ON -DSDL_DUMMYVIDEO=ON -DSDL_OFFSCREEN=OFF -DSDL_DUMMYAUDIO=ON -DSDL_DISKAUDIO=OFF \
    -DSDL_OSS=OFF -DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF \
    -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_LIBUDEV=OFF -DSDL_HIDAPI=OFF -DSDL_LIBUSB=OFF -DSDL_CAMERA=OFF \
    -DSDL_GPU=OFF -DSDL_RENDER=OFF -DSDL_DIALOG=OFF -DSDL_TRAY=OFF -DSDL_ASSEMBLY=ON
# CMake follows FreeBSD's layout for SDL's package file; the other tools read lib/pkgconfig.
[ -f "$PREFIX/libdata/pkgconfig/sdl3.pc" ] && mkdir -p "$PREFIX/lib/pkgconfig" && cp "$PREFIX/libdata/pkgconfig/sdl3.pc" "$PREFIX/lib/pkgconfig/"
cmake_lib ogg "$SRC/libogg-$OGG_VER" -DINSTALL_DOCS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake_lib vorbis "$SRC/libvorbis-$VORBIS_VER" -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake_lib openal "$SRC/openal-soft-$OPENAL_VER" -DLIBTYPE=STATIC -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF \
    -DALSOFT_TESTS=OFF -DALSOFT_INSTALL_EXAMPLES=OFF -DALSOFT_INSTALL_UTILS=OFF \
    -DALSOFT_BACKEND_SDL3=ON -DALSOFT_REQUIRE_SDL3=ON -DALSOFT_RTKIT=OFF -DALSOFT_BACKEND_WAVE=OFF \
    -DALSOFT_BACKEND_OSS=OFF -DALSOFT_BACKEND_SOLARIS=OFF -DALSOFT_BACKEND_SNDIO=OFF -DALSOFT_ENABLE_MODULES=OFF \
    -DCMAKE_CXX_SCAN_FOR_MODULES=OFF  `# the PS5 toolchain has no clang-scan-deps`

if wanted ffmpeg && [ ! -f "$BLD/ffmpeg.done" ]; then
    echo "== ffmpeg (ps5)"
    rm -rf "$BLD/ffmpeg" && mkdir -p "$BLD/ffmpeg"
    (cd "$BLD/ffmpeg" && "$SRC/ffmpeg-$FFMPEG_VER/configure" --prefix="$PREFIX" \
        --enable-cross-compile --arch=x86_64 --target-os=freebsd \
        --cc="$PS5_PAYLOAD_SDK/bin/prospero-clang" --cxx="$PS5_PAYLOAD_SDK/bin/prospero-clang++" \
        --ar="$PS5_PAYLOAD_SDK/bin/prospero-ar" --ranlib="$PS5_PAYLOAD_SDK/bin/prospero-ranlib" \
        --nm="$PS5_PAYLOAD_SDK/bin/prospero-nm" --strip="$PS5_PAYLOAD_SDK/bin/prospero-strip" \
        --enable-static --disable-shared --enable-pic --disable-debug --disable-autodetect --disable-x86asm \
        --disable-programs --disable-doc --disable-everything --disable-network \
        --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
        --enable-decoder=bink,binkaudio_rdft,binkaudio_dct --enable-demuxer=bink --enable-protocol=file \
        > "$BLD/ffmpeg.log" 2>&1 && make -j"$JOBS" install >> "$BLD/ffmpeg.log" 2>&1)
    touch "$BLD/ffmpeg.done"
fi
echo "ps5: $(ls "$PREFIX/lib"/*.a 2>/dev/null | wc -l | tr -d ' ') static libraries in $PREFIX/lib"
