cmake_minimum_required(VERSION 3.24)
project(wintears C CXX ASM)

if(NOT WIN32)
  message(FATAL_ERROR "wintears builds Windows programs. Cross-build from Linux with "
                      "-DCMAKE_TOOLCHAIN_FILE=cmake/mingw-x64.cmake")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(WINTEARS_TESTS "Build the game-free tests" ON)
option(WINTEARS_JIT "Build the ARM64-to-x86 JIT backend (dynarmic)" ON)

add_compile_definitions(_WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000005 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra $<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>)
endif()

# ---- platform layer ----
set(PAL_SOURCES src/pal/pal_win.c)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
  list(APPEND PAL_SOURCES src/pal/pal_win_x64.S)
endif()
add_library(pal STATIC ${PAL_SOURCES})
target_include_directories(pal PUBLIC src/pal)
target_link_libraries(pal PUBLIC synchronization)

# ---- CPU backend: dynarmic ----
if(WINTEARS_JIT)
  set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
  set(DYNARMIC_FRONTENDS "A64" CACHE STRING "" FORCE)
  set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
  set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
  # Older MinGW-w64 headers lack the newer CPU topology structs xbyak's cache query uses; dynarmic does not need it.
  add_compile_definitions($<$<COMPILE_LANGUAGE:CXX>:XBYAK_CPU_CACHE=0>)
  add_subdirectory(externals/dynarmic EXCLUDE_FROM_ALL)
  add_library(cpu_jit STATIC src/cpu/jit/jit.cpp)
  target_include_directories(cpu_jit PUBLIC src/cpu)
  target_compile_options(cpu_jit PRIVATE -mcx16)
  target_link_libraries(cpu_jit PUBLIC pal PRIVATE dynarmic)

  # ---- guest-to-host bridge ----
  set(NVN_DIR upstream/games/totk/third_party/d2r_gfx/gfx/nvn)
  add_library(bridge STATIC src/bridge/bridge.c src/bridge/nvn_bridge_gen.c)
  target_include_directories(bridge PUBLIC src PRIVATE ${NVN_DIR} ${NVN_DIR}/include)
  target_link_libraries(bridge PUBLIC cpu_jit pal)
endif()

# ---- tests ----
if(WINTEARS_TESTS)
  enable_testing()
  add_executable(pal_test tests/pal_test.c)
  target_link_libraries(pal_test PRIVATE pal)
  add_test(NAME pal COMMAND pal_test)
  if(WINTEARS_JIT)
    add_executable(jit_test tests/jit_test.cpp)
    target_link_libraries(jit_test PRIVATE cpu_jit pal)
    target_include_directories(jit_test PRIVATE tests)
    add_test(NAME jit COMMAND jit_test)
    add_executable(bridge_test tests/bridge_test.c tests/bridge_fixture.c tests/bridge_fixture_gen.c)
    target_include_directories(bridge_test PRIVATE tests)
    target_link_libraries(bridge_test PRIVATE bridge)
    add_test(NAME bridge COMMAND bridge_test)
  endif()
  if(CMAKE_CROSSCOMPILING)
    # MinGW runtime DLLs next to the tests so Wine (and a copied folder on Windows) finds them.
    target_link_options(pal_test PRIVATE -static)
    if(WINTEARS_JIT)
      target_link_options(jit_test PRIVATE -static)
      target_link_options(bridge_test PRIVATE -static)
    endif()
  endif()
endif()
