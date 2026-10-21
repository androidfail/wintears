#!/usr/bin/env python3
"""WinTears: build the shader translation service the Vulkan backend runs (wt_xlate.exe --service).

    python tools/build_xlate.py [-o <output folder>]   (default: build/shader-service)

The game's shaders are Switch GPU (Maxwell) code; at run time each one is translated to SPIR-V by the Ryujinx shader
translator that MacNativeGames vendors (upstream/vendor/ryujinx), with upstream's patches
(upstream/games/totk/tools/totk_xlate/patches) and upstream's translator front end (Program.cs, unchanged). Only the
service part is WinTears' own (tools/wt_xlate/VkService.cs: Vulkan output instead of Metal).

Compiled ahead of time (NativeAOT) into one program that needs no .NET runtime: needs the .NET 10 SDK and the MSVC
build tools (Visual Studio "Desktop development with C++" or Build Tools), and network access the first time (NuGet).
--jit builds a framework-dependent program instead (needs only the SDK; the .NET 10 runtime must be installed to run it).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UP = ROOT / "upstream"
XLATE = UP / "games/totk/tools/totk_xlate"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--out", type=Path, default=ROOT / "build/shader-service")
    ap.add_argument("--dotnet", default="dotnet")
    ap.add_argument("--jit", action="store_true", help="framework-dependent build (no NativeAOT)")
    a = ap.parse_args()
    if not (UP / "vendor/ryujinx/src").is_dir():
        sys.exit("upstream/vendor/ryujinx is missing: git submodule update --init upstream")
    with tempfile.TemporaryDirectory(prefix="wt_xlate_") as tmp:
        work = Path(tmp)
        ryu = work / "ryujinx-src"
        shutil.copytree(UP / "vendor/ryujinx", ryu, ignore=shutil.ignore_patterns("obj", "bin"))
        for p in sorted(XLATE.glob("patches/*.patch")):
            subprocess.run(["git", "apply", "-p1", "--whitespace=nowarn", str(p)], cwd=ryu, check=True)
        src = work / "xlate"
        src.mkdir()
        for f in ("Program.cs", "VertexCounterLowering.cs"):
            shutil.copy2(XLATE / f, src / f)
        for f in ("VkService.cs", "wt_xlate.csproj"):
            shutil.copy2(ROOT / "tools/wt_xlate" / f, src / f)
        cmd = [a.dotnet, "publish", str(src / "wt_xlate.csproj"), "-c", "Release", "-r", "win-x64", f"-p:RyuSrc={ryu}",
               "-p:InvariantGlobalization=true", "-o", str(work / "out")]
        cmd += ["--self-contained", "false"] if a.jit else ["-p:PublishAot=true"]
        env = dict(os.environ)
        vs_installer = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer"
        env["PATH"] = env.get("PATH", "") + os.pathsep + str(vs_installer)   # the NativeAOT linker step runs vswhere.exe
        subprocess.run(cmd, check=True, env=env)
        a.out.mkdir(parents=True, exist_ok=True)
        for f in (work / "out").iterdir():
            if f.suffix.lower() in (".exe", ".dll", ".json") and f.is_file():
                shutil.copy2(f, a.out / f.name)
    print(f"built {a.out / 'wt_xlate.exe'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
