// Forced into XenosRecomp when tools/make_kit.sh builds it with llvm-mingw: its sources rely on MSVC's implicit
// includes, and mingw's __uuidof macro wants a __CRT_UUID_DECL per interface where dxcapi.h only gives
// __declspec(uuid), which clang's own __uuidof (-fms-extensions) reads.
#pragma once
#include <atomic>
#include <cstdlib>
#include <windows.h>
#undef __uuidof
