# bbport Linux build environment: Valve's Steam Runtime 3 "sniper" SDK (Debian 11, glibc 2.31),
# the same base Steam Deck native games are built on. Binaries built here need glibc 2.31 or
# newer, so they run on SteamOS 3 and on desktop distributions from 2020 on.
# The SDK has gcc-14, SDL3, the Vulkan loader/headers, glslang, X11, OpenSSL and zlib. The other
# libraries are built here from source as static, position-independent archives, so the
# produced bb-probe and libbbgpu.so need nothing beyond the system's glibc, Vulkan loader,
# X11 and zlib, plus libSDL3.so.0 (installed next to them by packaging/linux/build.sh).
FROM registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest
RUN apt-get update && apt-get install -y --no-install-recommends nasm ninja-build ca-certificates \
    curl xz-utils && rm -rf /var/lib/apt/lists/*
ENV CC=gcc-14 CXX=g++-14 PREFIX=/opt/deps
ENV CMAKE_PREFIX_PATH=/opt/deps PKG_CONFIG_PATH=/opt/deps/lib/pkgconfig:/opt/deps/lib64/pkgconfig
# CMake >= 3.24 (gpu/CMakeLists.txt); the SDK has 3.25 but newer FSR-Vulkan wants more.
RUN curl -fsSL https://github.com/Kitware/CMake/releases/download/v3.31.8/cmake-3.31.8-linux-x86_64.tar.gz \
    | (mkdir -p /opt && tar xz -C /opt) && ln -sf /opt/cmake-3.31.8-linux-x86_64/bin/* /usr/local/bin/
WORKDIR /src
COPY deps.sh /src/deps.sh
RUN bash /src/deps.sh && rm -rf /src/*
