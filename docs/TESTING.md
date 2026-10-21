# Testing on your PC

Most bugs can only be found with the real game, and the game files stay on your PC. This is how a test round works.

## Once: set up

1. Install [Git](https://git-scm.com/download/win) and [Python 3.14](https://www.python.org/downloads/) (tick
   "Add python.exe to PATH").
2. Get the code, with its submodules:
   ```
   git clone --recursive https://github.com/androidfail/wintears
   cd wintears
   ```
3. Keep your game file (`.xci` or `.nsp`, version 1.0.0) and your `prod.keys` somewhere outside the repository, for
   example `D:\Games\TOTK\`. They must never be copied into the repository, a pull request, an issue or a chat.

### Finding your files if you use Ryujinx

- **The game file:** in Ryujinx, open *Options → Settings → UI → Game Directories*. Those are the folders Ryujinx
  scans; your `.xci` or `.nsp` is in one of them. Ryujinx's list view can also show a *Path* column. Use the base game
  file (version 1.0.0), not an update file: Ryujinx keeps updates as separate files (*Manage Title Updates*).
- **prod.keys:** *File → Open Ryujinx Folder*, then `system\prod.keys` (usually `%APPDATA%\Ryujinx\system\prod.keys`).
- **Shared fonts for an NSP:** Ryujinx's installed firmware works with
  `--firmware "%APPDATA%\Ryujinx\bis\system\Contents\registered"`.

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

## If the build stops at "1/3 code"

NSPs whose sections were decrypted by a conversion tool (encryption type 1) are supported, and so is a RomFS a
converter re-packed in another file order: the build rebuilds it in the original layout and uses it only if it is
then byte for byte the verified 1.0.0 image (this needs about 17 GB more free space for a moment). For anything else, run the
diagnostic and send its output (it prints no key values):

```
python tools\nsp_info.py "<path to your game file>" -k "%APPDATA%\Ryujinx\system\prod.keys"
```

## What not to send

- Your `prod.keys`, `title.keys` or any key value.
- Game files or anything from `TOTK_build\extracted\` or the output folder (`TOTK_win\`).
- Save files.

## Checking an output folder

```
python tools\build_win.py --check D:\Games\TOTK\TOTK_win          # sizes and small-file hashes (seconds)
python tools\build_win.py --check D:\Games\TOTK\TOTK_win --full   # also the 17 GB RomFS image (minutes)
```
