// bbport: force-included into every C++ file of the MinGW build (gpu/CMakeLists.txt).
// Newer libc++ headers (MSYS2 clang 23) no longer include these transitively; the vendored
// code (FidelityFX SDK, sirit, shadPS4) relied on that for std::abort, _countof, swprintf_s,
// placement new and the math functions.
#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cmath>
#include <new>
