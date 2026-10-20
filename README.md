# wintears

A planned Windows port of [MacNativeGames](https://github.com/ly05010419/macnativegames) for *The Legend of Zelda:
Tears of the Kingdom*: your own copy of the game, with the Switch system services answered by a small host and its
NVN graphics on Vulkan.

**Status:** early bring-up. The Windows platform layer and the ARM64-to-x86 JIT run their tests; the data build for
version 1.0.0 is ready for its first real run. The game does not start yet. See [the porting plan](docs/PORTING_PLAN.md)
and [how to test on your PC](docs/TESTING.md).

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
