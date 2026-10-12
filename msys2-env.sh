# Sourced by the Windows build/run scripts: puts the MSYS2 CLANG64 toolchain first on PATH.
# MSYS2_ROOT overrides the default C:/msys64.
msys2_root=${MSYS2_ROOT:-/c/msys64}
if [[ -z ${MSYS2_ROOT:-} && ! -d $msys2_root/clang64/bin ]]; then
    msys2_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/out/msys64
fi
if [[ -d $msys2_root/clang64/bin ]]; then
    export PATH="$msys2_root/clang64/bin:$msys2_root/usr/bin:$PATH"
    export MSYSTEM=CLANG64
fi
