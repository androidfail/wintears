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
target_link_libraries(pal PUBLIC synchronization shell32 ole32 uuid)

# ---- CPU backend: dynarmic ----
if(WINTEARS_JIT)
  set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
  set(DYNARMIC_FRONTENDS "A64" CACHE STRING "" FORCE)
  set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
  set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
  # Older MinGW-w64 headers lack the newer CPU topology structs xbyak's cache query uses; dynarmic does not need it.
  add_compile_definitions($<$<COMPILE_LANGUAGE:CXX>:XBYAK_CPU_CACHE=0>)
  # Our changes to dynarmic, applied to the submodule's working tree once (patches/): see each patch's comments.
  foreach(p dynarmic-map-load-factor)
    set(pf ${CMAKE_CURRENT_SOURCE_DIR}/patches/${p}.patch)
    execute_process(COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR}/externals/dynarmic apply --reverse --check ${pf}
                    RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
    if(NOT applied EQUAL 0)
      execute_process(COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR}/externals/dynarmic apply ${pf} RESULT_VARIABLE r)
      if(NOT r EQUAL 0)
        message(FATAL_ERROR "patches/${p}.patch does not apply to externals/dynarmic")
      endif()
    endif()
  endforeach()
  add_subdirectory(externals/dynarmic EXCLUDE_FROM_ALL)
  # libopus (externals/opus, v1.5.2): the hardware Opus decoder upstream implements with it (game voices)
  set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
  set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
  set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
  add_subdirectory(externals/opus EXCLUDE_FROM_ALL)
  add_library(cpu_jit STATIC src/cpu/jit/jit.cpp)
  target_include_directories(cpu_jit PUBLIC src/cpu)
  target_compile_options(cpu_jit PRIVATE -mcx16)
  target_link_libraries(cpu_jit PUBLIC pal PRIVATE dynarmic)
  # Direct Native recompiler (build-time tool; its output is derived from the game and stays on the player's machine)
  add_executable(wt_recomp tools/recomp/wt_recomp.cpp)
  target_link_libraries(wt_recomp PRIVATE dynarmic fmt::fmt merry::mcl tsl::robin_map)
  target_include_directories(wt_recomp PRIVATE externals/dynarmic/src src/cpu/native)
  target_link_options(wt_recomp PRIVATE -static)

  # ---- guest-to-host bridge ----
  set(NVN_DIR upstream/games/totk/third_party/d2r_gfx/gfx/nvn)
  add_library(bridge STATIC src/bridge/bridge.c src/bridge/nvn_bridge_gen.c)
  target_include_directories(bridge PUBLIC src PRIVATE ${NVN_DIR} ${NVN_DIR}/include)
  target_link_libraries(bridge PUBLIC cpu_jit pal)
endif()

# ---- module loader (original NSO modules) ----
add_library(loader STATIC src/loader/loader.c)
target_include_directories(loader PUBLIC src)
target_link_libraries(loader PUBLIC pal)

# ---- the runtime (phase 3A: boot probe) ----
if(WINTEARS_JIT)
  # Horizon host: kernel svcs and system services (ported from upstream games/totk/runtime)
  set(MNG_RUNTIME upstream/games/totk/runtime)
  set(MNG_HOST_SOURCES ${MNG_RUNTIME}/ipc.c ${MNG_RUNTIME}/audio_effects.c ${MNG_RUNTIME}/audio_reverb.c
      ${MNG_RUNTIME}/opus_codec.c)   # compiled unchanged
  set(MNG_HOST_COPIES src/host/audio_renderer.c src/host/audio_mixer.c)   # Windows copies, compiled the same way
  add_library(host STATIC src/host/svc.c src/host/memory.c src/host/threads.c src/host/overrides.c
              src/host/services.c src/host/sha256.c src/host/romfs.c src/host/fs_host.c src/host/account.c
              src/host/gfx_bridge.c src/host/vi_stub.c src/host/input.c src/host/time.c src/host/fonts.c
              src/host/applet.c src/host/audio_out.c src/host/codec.c src/host/movie.c
              ${MNG_HOST_SOURCES} ${MNG_HOST_COPIES})
  set_source_files_properties(${MNG_HOST_SOURCES} ${MNG_HOST_COPIES} PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_SOURCE_DIR}/src/host/upstream_compat.h;-Wno-unused-parameter")
  set_source_files_properties(${MNG_HOST_COPIES} PROPERTIES INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/${MNG_RUNTIME})
  target_include_directories(host PUBLIC src)
  target_link_libraries(host PUBLIC loader bridge cpu_jit pal pthread bcrypt ole32 opus)
  # libvpx (MSYS2 mingw-w64-ucrt-x86_64-libvpx, static): the cutscene decoder, src/host/movie.c. Without it the game
  # still runs, and aborts at the first cutscene.
  find_path(VPX_INCLUDE_DIR vpx/vpx_decoder.h)
  find_library(VPX_LIBRARY NAMES libvpx.a vpx)
  if(VPX_INCLUDE_DIR AND VPX_LIBRARY)
    target_compile_definitions(host PRIVATE HAVE_LIBVPX)
    target_include_directories(host PRIVATE ${VPX_INCLUDE_DIR})
    target_link_libraries(host PUBLIC ${VPX_LIBRARY})
  else()
    message(WARNING "libvpx not found: cutscenes will abort the game (pacman -S mingw-w64-ucrt-x86_64-libvpx)")
  endif()
  # Upstream services register themselves from constructors (audio_renderer.c: audren:u); nothing else refers to
  # their objects, so the archive would drop them.
  target_link_options(host INTERFACE "LINKER:-u,runtime_audio_register")
  # NVN layer (ported from upstream third_party/d2r_gfx/gfx/nvn) with the null GPU backend. An object library, linked
  # directly into each program: its functions register themselves from constructors, which an archive would drop.
  file(STRINGS ${NVN_DIR}/names.txt NVN_NAMES)
  set(NVN_NAMES_C "/* Generated by CMake from upstream names.txt: main's NVN name table. */\nconst char *const nvn_names[] = {\n")
  foreach(n IN LISTS NVN_NAMES)
    string(STRIP "${n}" n)
    if(NOT n STREQUAL "")
      string(APPEND NVN_NAMES_C "    \"${n}\",\n")
    endif()
  endforeach()
  string(APPEND NVN_NAMES_C "};\nconst unsigned nvn_name_count = sizeof nvn_names / sizeof *nvn_names;\n")
  file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/nvn_names.c CONTENT "${NVN_NAMES_C}")
  # SDL3 (externals/SDL, static): the window, keyboard and controllers
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(externals/SDL EXCLUDE_FROM_ALL)
  # ASTC decoder (externals/astc-encoder): desktop GPUs cannot sample ASTC, the game's main texture format
  set(ASTCENC_ISA_SSE41 ON CACHE BOOL "" FORCE)
  set(ASTCENC_DECOMPRESSOR ON CACHE BOOL "" FORCE)
  set(ASTCENC_CLI OFF CACHE BOOL "" FORCE)
  set(ASTCENC_WERROR OFF CACHE BOOL "" FORCE)
  set(ASTCENC_UNIVERSAL_BUILD OFF CACHE BOOL "" FORCE)
  add_subdirectory(externals/astc-encoder EXCLUDE_FROM_ALL)
  # Vulkan: headers, volk (loads vulkan-1.dll at run time), Vulkan Memory Allocator
  set(TEX_DIR upstream/games/totk/third_party/d2r_gfx/gfx/tex)
  add_library(gfx OBJECT src/gfx/nvn.c src/gfx/nvn_ext.c src/gfx/nvn_vpool.c src/gfx/nvn_null.c src/gfx/nvn_gpu.c src/gfx/nvn_dirty.c
              src/gfx/vk/vk_dev.c src/gfx/vk/vk_fmt.c src/gfx/vk/vk_tex.c src/gfx/vk/vk_exec.c src/gfx/vk/vk_clear.c src/gfx/vk/vk_winmem.c src/gfx/vk/vk_draw.c src/gfx/vk/vk_shader.c src/gfx/vk/vk_shot.c src/gfx/vk/vk_astc.cpp
              src/gfx/vk/vk_present.c src/gfx/vk/vk_mem.cpp externals/volk/volk.c src/frontend/frontend.c
              ${NVN_DIR}/nvn_tex.c ${TEX_DIR}/nvn_deswizzle.c ${CMAKE_BINARY_DIR}/nvn_names.c)
  target_include_directories(gfx PUBLIC src ${NVN_DIR} ${NVN_DIR}/include ${TEX_DIR}
                             externals/Vulkan-Headers/include externals/volk externals/VulkanMemoryAllocator/include)
  target_compile_definitions(gfx PUBLIC VK_NO_PROTOTYPES)
  target_compile_options(gfx PRIVATE -Wno-missing-field-initializers)   # Vulkan structs: {sType, .field = ...}
  set_source_files_properties(src/gfx/vk/vk_mem.cpp PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-unused-variable;-Wno-missing-field-initializers")
  target_link_libraries(gfx PUBLIC host SDL3::SDL3-static astcdec-sse4.1-static)
  # alloc.cpp: operator new / delete and malloc / free with per-thread and large-block caches (Wine's heap lock and
  # its mmap per large block cost the Steam Deck dearly). Static, so libstdc++'s own allocations use it too.
  add_executable(wintears src/app/main.c src/app/alloc.cpp)
  target_link_libraries(wintears PRIVATE gfx)
  target_link_options(wintears PRIVATE -static
    "LINKER:--wrap=malloc" "LINKER:--wrap=free" "LINKER:--wrap=calloc" "LINKER:--wrap=realloc")   # alloc.cpp: also C code
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
    add_executable(loader_test tests/loader_test.c)
    target_link_libraries(loader_test PRIVATE loader cpu_jit pal)
    add_test(NAME loader COMMAND loader_test)
    add_executable(nvn_test tests/nvn_test.c)
    target_link_libraries(nvn_test PRIVATE gfx)
    add_test(NAME nvn COMMAND nvn_test)
    add_executable(audio_test tests/audio_test.c)
    target_link_libraries(audio_test PRIVATE gfx)
    add_test(NAME audio COMMAND audio_test)
  endif()
  if(CMAKE_CROSSCOMPILING)
    # MinGW runtime DLLs next to the tests so Wine (and a copied folder on Windows) finds them.
    target_link_options(pal_test PRIVATE -static)
    if(WINTEARS_JIT)
      target_link_options(jit_test PRIVATE -static)
      target_link_options(bridge_test PRIVATE -static)
      target_link_options(loader_test PRIVATE -static)
      target_link_options(nvn_test PRIVATE -static)
      target_link_options(audio_test PRIVATE -static)
    endif()
  endif()
endif()
