#!/usr/bin/env bash
# Builds the GPU library's dependencies into $PREFIX as static PIC archives (see Dockerfile).
set -euo pipefail
jobs=$(nproc)
fetch() { mkdir -p "$2" && curl -fsSL "$1" | tar xz -C "$2" --strip-components=1; }
cmake_install() {
    local dir=$1; shift
    cmake -S "$dir" -B "$dir/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_LIBDIR=lib "$@"
    cmake --build "$dir/build" -j"$jobs" && cmake --install "$dir/build"
}
# Vulkan-Hpp in the SDK (1.4.309) predates driver ids and features the renderer names (KosmicKrisp, VALVE mixed float dot).
fetch https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.350.1.tar.gz vulkan-headers
cmake_install vulkan-headers
# Abseil (flat_hash_map in the texture cache).
fetch https://github.com/abseil/abseil-cpp/archive/refs/tags/20250512.1.tar.gz abseil
cmake_install abseil -DABSL_BUILD_TESTING=OFF -DABSL_PROPAGATE_CXX_STD=ON -DCMAKE_CXX_STANDARD=23
fetch https://github.com/fmtlib/fmt/archive/refs/tags/11.2.0.tar.gz fmt
cmake_install fmt -DFMT_TEST=OFF -DFMT_DOC=OFF
fetch https://github.com/zyantific/zycore-c/archive/refs/tags/v1.5.2.tar.gz zycore
cmake_install zycore
# zydis builds its own copy of zycore; its installed CMake package looks for the one above.
fetch https://github.com/zyantific/zydis/archive/refs/tags/v4.1.1.tar.gz zydis
fetch https://github.com/zyantific/zycore-c/archive/refs/tags/v1.5.2.tar.gz zydis/dependencies/zycore
cmake_install zydis -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_EXAMPLES=OFF -DZYDIS_BUILD_DOXYGEN=OFF
fetch https://github.com/Tessil/robin-map/archive/refs/tags/v1.4.0.tar.gz robin-map
cmake_install robin-map
fetch https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz json
cmake_install json -DJSON_BuildTests=OFF
fetch https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.3.0.tar.gz vma
cmake_install vma
fetch https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.3.tar.gz xxhash
cmake_install xxhash/cmake_unofficial -DXXHASH_BUILD_XXHSUM=OFF
# UPnP port mapping for online play (bbnet).
fetch https://github.com/miniupnp/miniupnp/archive/refs/tags/miniupnpc_2_3_3.tar.gz miniupnp
cmake_install miniupnp/miniupnpc -DUPNPC_BUILD_SHARED=OFF -DUPNPC_BUILD_STATIC=ON -DUPNPC_BUILD_TESTS=OFF \
    -DUPNPC_BUILD_SAMPLE=OFF
# Header-only use (Boost::headers).
fetch https://archives.boost.io/release/1.88.0/source/boost_1_88_0.tar.gz boost
mkdir -p "$PREFIX/include" && cp -r boost/boost "$PREFIX/include/"
# protobuf 21.x: the last series without Abseil; installs protobuf-config.cmake and protoc.
fetch https://github.com/protocolbuffers/protobuf/archive/refs/tags/v21.12.tar.gz protobuf
cmake_install protobuf -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_WITH_ZLIB=OFF
# OpenSSL 3, static: SteamOS and current desktops no longer have the SDK's OpenSSL 1.1.
fetch https://github.com/openssl/openssl/releases/download/openssl-3.5.1/openssl-3.5.1.tar.gz openssl
(cd openssl && ./Configure linux-x86_64 no-shared no-tests no-docs -fPIC --prefix="$PREFIX" --libdir=lib \
    --openssldir=/etc/ssl >/dev/null && make -j"$jobs" >/dev/null && make install_sw >/dev/null)
# FFmpeg (movie playback, avplayer): decoders and demuxers only, nothing autodetected.
fetch https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.gz ffmpeg
(cd ffmpeg && ./configure --prefix="$PREFIX" --cc="$CC" --enable-pic --disable-shared --enable-static \
    --disable-programs --disable-doc --disable-encoders --disable-muxers --disable-devices \
    --disable-network --disable-autodetect --disable-debug >/dev/null && make -j"$jobs" >/dev/null && make install >/dev/null)
# SDL3, shared: the SDK's copy links PulseAudio, ALSA, Wayland, libdecor and fribidi directly.
# Built here (same 3.4.16 release) it loads all of those at run time when present (SDL's
# default *_SHARED options), so libSDL3.so.0 itself needs only glibc. It is installed in $PREFIX,
# ahead of the SDK's copy on PKG_CONFIG_PATH.
fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz sdl
cmake -S sdl -B sdl/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_LIBDIR=lib -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_DEPS_SHARED=ON
cmake --build sdl/build -j"$jobs" && cmake --install sdl/build
