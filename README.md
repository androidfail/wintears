# wintears

A Windows port of [MacNativeGames](https://github.com/ly05010419/macnativegames) for *The Legend of Zelda:
Tears of the Kingdom*: your own copy of the game, with the Switch system services answered by a small host and its
NVN graphics on Vulkan.

**Status:** playable from your own 1.0.0 dump at about 30 fps on a desktop PC, also on the Steam Deck (Proton) and,
slowly, on 8 GB laptops. The game's ARM64 code runs from DLLs recompiled on your machine (`tools/recomp/build_native.py
<TOTK_win> --verify`), with the dynarmic JIT as a fallback made only when a block is missing (`-DWINTEARS_NATIVE_ONLY=ON`
builds without it). See [the porting plan](docs/PORTING_PLAN.md) and [how to test on your PC](docs/TESTING.md).

## Building (developers)

Cross-build from Linux (what CI and the dev sandbox use):

```sh
sudo apt-get install mingw-w64 wine64 libboost-dev ninja-build
git submodule update --init --recursive
mkdir -p /tmp/boostinc && ln -sfn /usr/include/boost /tmp/boostinc/boost
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-x64.cmake -DBoost_INCLUDE_DIR=/tmp/boostinc
cmake --build build && ctest --test-dir build --output-on-failure   # tests run under Wine
```

On Windows, with [MSYS2](https://www.msys2.org/) UCRT64 (`pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,boost}`):
`cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build`.

You must own the game. This repository contains no game code, assets, shaders or keys, and is not affiliated with
Nintendo.
