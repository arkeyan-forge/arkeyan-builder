// XenosRecomp writes each compressed shader cache of shader_cache.cpp as a list of numbers ("{40,181,47,...}"): 200
// MB of text for Giants, which clang reads in about 30 s, twice per install (before and after the movie pass). The
// same bytes written as string literals compile in about 2 s, so Arkeyan-Builder compiles a copy written that way and leaves
// ppc/shader_cache.cpp as XenosRecomp wrote it (tools/shaders/compare_cache.py reads that one).
#pragma once

#include <string>

// The text of shader_cache.cpp with every "...Cache[] = {n,n,...}" array as a string literal of the same bytes (each
// array then holds one more byte, the literal's final zero, after the size the cache records). Anything else is kept.
std::string ShaderCacheLiterals(const std::string& text);
