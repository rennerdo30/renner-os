# Turns a SPIR-V binary into a C++ header with a uint32_t array.
# Usage: cmake -DIN=x.spv -DOUT=x.h -DVAR=name -P embed_spirv.cmake
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" len)
math(EXPR rem "${len} % 8")
if(len EQUAL 0 OR NOT rem EQUAL 0)
  message(FATAL_ERROR "${IN} is not a valid SPIR-V binary (size ${len} hex digits)")
endif()
# SPIR-V words are little-endian: bytes b0 b1 b2 b3 -> 0xb3b2b1b0.
string(REGEX REPLACE "([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])"
       "0x\\4\\3\\2\\1u,\n" words "${hex}")
file(WRITE "${OUT}" "// Generated from ${IN}; do not edit.\n#pragma once\n#include <cstdint>\nstatic const uint32_t ${VAR}[] = {\n${words}};\n")
