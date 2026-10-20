# Testing on your PC

Most bugs can only be found with the real game, and the game files stay on your PC. This is how a test round works.

## Once: set up

1. Install [Git](https://git-scm.com/download/win) and [Python 3.14](https://www.python.org/downloads/) (tick
   "Add python.exe to PATH").
2. Get the code, with its submodules:
   ```
   git clone --recursive https://github.com/androidfail/wintears
   cd wintears
   git checkout claude/porting-plan
   ```
3. Keep your game file (`.xci` or `.nsp`, version 1.0.0) and your `prod.keys` somewhere outside the repository, for
   example `D:\Games\TOTK\`. They must never be copied into the repository, a pull request, an issue or a chat.

## Each round

1. Update: `git pull --recurse-submodules`
2. Run what the round asks for. Right now that is the data build:
   ```
   python tools\build_win.py D:\Games\TOTK\totk.xci -k D:\Games\TOTK\prod.keys
   ```
   For an NSP, add `--firmware <folder>` or `--shared-fonts <folder>` (NSPs carry no system fonts).
   The first run takes a while (the 17 GB RomFS is decrypted and every hash level checked); later runs reuse what is
   already done and correct.
3. Send back:
   - the last 30 lines it printed (or the whole thing if it failed),
   - the files in `TOTK_build\logs\` that the error message names.

   Logs contain file names, sizes and hashes, never key values or game content.

## What not to send

- Your `prod.keys`, `title.keys` or any key value.
- Game files or anything from `TOTK_build\extracted\` or the output folder (`TOTK_win\`).
- Save files.

## Checking an output folder

```
python tools\build_win.py --check D:\Games\TOTK\TOTK_win          # sizes and small-file hashes (seconds)
python tools\build_win.py --check D:\Games\TOTK\TOTK_win --full   # also the 17 GB RomFS image (minutes)
```
