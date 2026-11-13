cmake_minimum_required(VERSION 3.20)
project(renner_bench VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Vulkan REQUIRED)

# SDL2: the CMake package first (SDL2 or sdl2-compat), pkg-config as a fallback.
find_package(SDL2 CONFIG QUIET)
if(TARGET SDL2::SDL2)
  set(BENCH_SDL_TARGET SDL2::SDL2)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  set(BENCH_SDL_TARGET PkgConfig::SDL2)
endif()

# Shader compiler: glslc (shaderc), glslangValidator as a fallback.
find_program(GLSLC_EXECUTABLE glslc HINTS "$ENV{VULKAN_SDK}/bin")
find_program(GLSLANG_EXECUTABLE glslangValidator HINTS "$ENV{VULKAN_SDK}/bin")
if(NOT GLSLC_EXECUTABLE AND NOT GLSLANG_EXECUTABLE)
  message(FATAL_ERROR "Neither glslc nor glslangValidator was found (install shaderc or glslang)")
endif()

set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders_gen)
file(MAKE_DIRECTORY ${SHADER_GEN_DIR})

set(SHADERS
  fullscreen.vert
  raymarch.frag
  instanced.vert
  instanced.frag
  particles.comp
  particles.vert
  particles.frag
  composite.frag
  bloom_down.frag
  bloom_up.frag
  post_tonemap.frag
  fxaa.frag
)

set(SHADER_HEADERS)
foreach(shader ${SHADERS})
  string(REPLACE "." "_" var ${shader})
  set(spv ${SHADER_GEN_DIR}/${shader}.spv)
  set(hdr ${SHADER_GEN_DIR}/${var}.h)
  if(GLSLC_EXECUTABLE)
    set(compile_cmd ${GLSLC_EXECUTABLE} --target-env=vulkan1.2 -O -I ${SHADER_DIR} -o ${spv} ${SHADER_DIR}/${shader})
  else()
    set(compile_cmd ${GLSLANG_EXECUTABLE} -V --target-env vulkan1.2 -I${SHADER_DIR} -o ${spv} ${SHADER_DIR}/${shader})
  endif()
  add_custom_command(
    OUTPUT ${hdr}
    COMMAND ${compile_cmd}
    COMMAND ${CMAKE_COMMAND} -DIN=${spv} -DOUT=${hdr} -DVAR=${var} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake
    DEPENDS ${SHADER_DIR}/${shader} ${SHADER_DIR}/common.glsl ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake
    COMMENT "Compiling shader ${shader}"
    VERBATIM)
  list(APPEND SHADER_HEADERS ${hdr})
endforeach()

add_executable(renner-bench
  src/main.cpp
  src/vk_context.cpp
  src/vk_context.h
  src/scenes/scenes.h
  src/scenes/raymarch.cpp
  src/scenes/instanced.cpp
  src/scenes/particles.cpp
  src/scenes/postfx.cpp
  ${SHADER_HEADERS})

target_include_directories(renner-bench PRIVATE src ${SHADER_GEN_DIR})
target_compile_definitions(renner-bench PRIVATE SDL_MAIN_HANDLED RENNER_BENCH_VERSION="${PROJECT_VERSION}")
target_link_libraries(renner-bench PRIVATE Vulkan::Vulkan ${BENCH_SDL_TARGET})

if(MSVC)
  target_compile_options(renner-bench PRIVATE /W3)
else()
  target_compile_options(renner-bench PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()

if(MINGW)
  # Single binary: no libstdc++/libgcc/winpthread DLLs next to it, only SDL2.dll.
  target_link_options(renner-bench PRIVATE -static-libgcc -static-libstdc++ $<$<CONFIG:Release>:-s>
    -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive,-Bdynamic)
endif()

if(WIN32 AND TARGET SDL2::SDL2)
  add_custom_command(TARGET renner-bench POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:renner-bench>
    COMMENT "Copying SDL2 runtime next to renner-bench")
endif()

install(TARGETS renner-bench RUNTIME DESTINATION bin)
