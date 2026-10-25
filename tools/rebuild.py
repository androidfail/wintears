#!/usr/bin/env python3
"""Everything the launcher's Rebuild should do: compile the runtime (build/wintears.exe, with MSYS2 UCRT64), then the
data build (tools/build_win.py with the same arguments).

    python tools/rebuild.py <game file> -k <prod.keys> [build_win.py options]

build_win.py alone only prepares the game data; without the compile, Play keeps running whatever exe was built last.
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS = Path(os.environ.get('MSYS2_ROOT', 'C:/msys64'))


def compile_runtime():
    bash = MSYS / 'usr/bin/bash.exe'
    if not bash.exists():
        sys.exit(f'MSYS2 not found at {MSYS} (set MSYS2_ROOT): cannot compile build/wintears.exe')
    env = dict(os.environ, MSYSTEM='UCRT64', CHERE_INVOKING='1')
    root = ROOT.as_posix()
    configure = '' if (ROOT / 'build/build.ninja').exists() else f'cmake -S "{root}" -B "{root}/build" -G Ninja && '
    cmd = f'cd "{root}" && {configure}cmake --build build -j4'
    print(f'[rebuild] compiling the runtime: {cmd}', flush=True)
    r = subprocess.run([str(bash), '-lc', cmd], env=env, cwd=ROOT)
    if r.returncode:
        sys.exit(f'[rebuild] the runtime did not compile (exit {r.returncode}); build/wintears.exe was not updated')
    exe = ROOT / 'build/wintears.exe'
    print(f'[rebuild] {exe} is up to date ({exe.stat().st_size} bytes)', flush=True)


def main():
    compile_runtime()
    print('[rebuild] game data: tools/build_win.py ' + ' '.join(sys.argv[1:]), flush=True)
    sys.exit(subprocess.run([sys.executable, str(ROOT / 'tools/build_win.py')] + sys.argv[1:], cwd=ROOT).returncode)


if __name__ == '__main__':
    main()
