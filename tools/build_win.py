#!/usr/bin/env python3
"""WinTears: prepare your own Tears of the Kingdom game file for the Windows runtime.

    python tools/build_win.py <game.xci|game.nsp> -k <prod.keys> [-o <output folder>] [--work <folder>]
                              [--firmware <unpacked firmware NCA folder> | --shared-fonts <folder>]

Supported now: TOTK base 1.0.0 (0100F2C0115B6000) as XCI or NSP. Updates come later.

What it does, each step checked against MacNativeGames' verified hashes (upstream/games/totk/assets/expected-v1.0.0.json),
so a damaged dump or another version stops the build at the first wrong file:
  1. code: the ExeFS out of your game file, its code modules checked byte for byte. The Windows runtime runs them
     through an ARM64-to-x86 JIT, so they are NOT rewritten (that is only for native ARM64).
  2. data: the RomFS (17 GB, all six IVFC hash levels verified), the system shared fonts, and the small RomFS overlays
     (made from your own files).
  3. output: a folder with the data and a manifest of every file and its SHA-256. Large files are hard-linked from the
     work folder when both are on the same drive, otherwise copied (allow ~17 GB more).

Nothing from the game is in this repository and nothing leaves your PC. Key values are never printed. The output folder
contains the game: it is for your own use only.

Needs: Python 3.14 or later (the build reads zstd with the standard library), about 25 GB free for the work folder.
The build tools' Python packages are installed into a private environment in the work folder.

This reuses the upstream build steps (upstream/build_app.py, upstream/games/totk/scripts); only the macOS-specific
orchestration (rsync, symlinks, xcrun, codesign, APFS clones, app bundles) is replaced.
"""
import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
UPSTREAM = REPO / 'upstream'
GAME = UPSTREAM / 'games/totk'
sys.path.insert(0, str(UPSTREAM))
import build_app as mng   # noqa: E402  (upstream helpers: hash checks, container detection, genuine-dump checks)
import romfs_rebuild      # noqa: E402
import upstream_patches   # noqa: E402

BuildError = mng.BuildError
say, run, sha256, check = mng.say, mng.run, mng.sha256, mng.check
EXPECTED = mng.EXPECTED
TITLE_ID = EXPECTED['title_id']
OUTPUT_FORMAT = 1
# Generated or Mac-only parts of the upstream game folder that the work copy does not need.
SKIP = {'extracted', 'rewritten', 'build', 'analysis', '.venv', 'tooling', 'user-data', 'ios', 'prebuilt', 'tests', 'app'}


def prerequisites():
    if sys.version_info < (3, 14):
        raise BuildError(f'needs Python 3.14 or later (the build reads zstd with the standard library); this is '
                         f'{platform.python_version()}. Get it from https://www.python.org/downloads/')
    if not (GAME / 'scripts/extract_exefs.py').is_file():
        raise BuildError('upstream/ is empty: run "git submodule update --init upstream" first')


def venv_python(venv):
    return venv / ('Scripts/python.exe' if os.name == 'nt' else 'bin/python')


def prepare_work(work, logs):
    """work/totk: a copy of upstream's game folder (its scripts write extracted/ next to themselves), with
    tools/upstream_patches.py applied (e.g. NSPs whose sections are not encrypted)."""
    tree = work / 'totk'
    tree.mkdir(parents=True, exist_ok=True)
    for item in GAME.iterdir():
        if item.name in SKIP:
            continue
        dst = tree / item.name
        if item.is_dir():
            shutil.copytree(item, dst, dirs_exist_ok=True)
        else:
            shutil.copy2(item, dst)
    try:
        upstream_patches.apply(tree)
    except upstream_patches.PatchError as e:
        raise BuildError(str(e)) from None
    for d in ('analysis', 'build'):
        (tree / d).mkdir(exist_ok=True)
    venv = work / 'venv'
    py = venv_python(venv)
    if not py.exists():
        say('Python environment for the build tools')
        run([sys.executable, '-m', 'venv', venv], logs / 'venv.log')
    run([py, '-m', 'pip', 'install', '-q', '-r', tree / 'requirements.txt'], logs / 'pip.log')
    return tree, py


def step_code(tree, py, game, keys, logs):
    """ExeFS extracted and checked like upstream step_code, without the ARM64 rewrite."""
    exefs = tree / 'extracted/v0/exefs'
    want = EXPECTED['exefs_sha256']
    manifest = exefs.parent / 'extraction-manifest.json'
    if not mng.source_matches(manifest, game) or not mng.exefs_matches(exefs, want, TITLE_ID):
        say('1/3 code: extracting the ExeFS from the game file')
        if exefs.exists():
            shutil.rmtree(exefs)
        run([py, 'scripts/extract_exefs.py', game, '--keys', keys, '--output', exefs], logs / 'extract_exefs.log', cwd=tree)
    for n, h in want.items():
        if not (exefs / n).is_file():
            raise BuildError(f'ExeFS {n} is missing: this is not the supported version (1.0.0)')
        got = sha256(exefs / n)
        if got == h:
            continue
        if n == 'main.npdm' and mng.npdm_kind(exefs / n, TITLE_ID) in ('signed', 'resigned'):
            say('1/3 code: main.npdm comes from another genuine dump of 1.0.0; the code modules must still match, using it')
            continue
        check(f'ExeFS {n}', got, h)
    if not (tree / 'extracted/v0/elf/main-inventory.json').exists():
        say('1/3 code: checking the module segments and listing their symbols')
        run([py, 'scripts/prepare_modules.py', exefs, '--output', tree / 'extracted/v0/elf'], logs / 'prepare_modules.log', cwd=tree)
    return exefs


def step_data(tree, py, game, keys, logs, *, firmware=None, shared_fonts=None):
    """RomFS, shared fonts and overlays, as upstream step_data (same scripts, same checks)."""
    romfs = tree / 'extracted/v0/romfs'
    index = romfs / 'index.json'
    ok = (index.exists() and (romfs / 'romfs.bin').is_file()
          and json.loads(index.read_text()).get('romfs_sha256') == EXPECTED['romfs']['sha256']
          and (romfs / 'romfs.bin').stat().st_size == EXPECTED['romfs']['size'])
    if not ok:
        say('2/3 data: extracting and verifying the RomFS (17 GB, a few minutes)')
        if romfs.exists():
            shutil.rmtree(romfs)
        run([py, 'scripts/extract_romfs.py', '--keys', keys], logs / 'extract_romfs.log', cwd=tree)
        if json.loads(index.read_text())['romfs_sha256'] != EXPECTED['romfs']['sha256']:
            canonical_romfs(romfs)
        check('RomFS', json.loads(index.read_text())['romfs_sha256'], EXPECTED['romfs']['sha256'])
    fonts = tree / 'extracted/firmware/shared-fonts'
    if not mng.fonts_valid(fonts):
        if shared_fonts:
            report = mng.font_report(shared_fonts)
            if shared_fonts.resolve() != fonts.resolve():
                if fonts.exists():
                    shutil.rmtree(fonts)
                fonts.mkdir(parents=True)
                for f in report['fonts']:
                    name = f"{f['type']}-{f['name']}.bfttf"
                    shutil.copy2(shared_fonts / name, fonts / name)
            (fonts / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
        else:
            if not firmware and mng.input_kind(game) == 'nsp':
                raise BuildError('an NSP has no system fonts: add --firmware <unpacked firmware NCA folder> or '
                                 '--shared-fonts <folder with the six .bfttf files>')
            say('2/3 data: extracting the system shared fonts')
            if fonts.exists():
                shutil.rmtree(fonts)
            source = ['--firmware', firmware] if firmware else ['--xci', game]
            run([py, 'scripts/extract_shared_fonts.py', '--keys', keys, *source, '--output', fonts],
                logs / 'shared_fonts.log', cwd=tree)
        mng.validate_fonts(fonts)
    if not (tree / 'extracted/v0/romfs-patches/manifest.tsv').exists():
        say('2/3 data: RomFS overlays (quick-menu capture timing, game-over retry)')
        run([py, 'scripts/make_romfs_patches.py'], logs / 'romfs_patches.log', cwd=tree)
    if not (tree / 'extracted/v0/romfs-patches-out/2560x1440/manifest.tsv').exists():
        say('2/3 data: RomFS overlay for the 2560x1440 output')
        run([py, 'scripts/make_output_res_overlay.py', '2560x1440'], logs / 'output_res_overlay.log', cwd=tree)
    return romfs, fonts


def canonical_romfs(romfs):
    """The extracted RomFS passed all IVFC levels but is not the 1.0.0 image: some converters re-pack the RomFS
    (same files, another order). Rebuild it in Nintendo's layout (tools/romfs_rebuild.py) and keep the result only
    if it is byte for byte the verified image; otherwise stop as before."""
    say('2/3 data: the RomFS image is not the 1.0.0 one although its hash levels verify (re-packed by a converter?); '
        "rebuilding it in Nintendo's layout")
    image, tmp, index = romfs / 'romfs.bin', romfs / 'romfs.rebuilt.bin', romfs / 'index.json'
    if tmp.exists():
        tmp.unlink()
    try:
        sha, size, files = romfs_rebuild.rebuild(image, tmp)
    except ValueError as e:
        tmp.unlink(missing_ok=True)
        raise BuildError(f'RomFS: rebuilding it failed: {e}') from None
    if sha != EXPECTED['romfs']['sha256'] or size != EXPECTED['romfs']['size']:
        tmp.unlink()
        raise BuildError(f'RomFS: SHA-256 {sha[:16]}... is not the expected {EXPECTED["romfs"]["sha256"][:16]}... even '
                         f'rebuilt in the original layout: the files differ from 1.0.0 (another version, or modified)')
    os.replace(tmp, image)
    report = json.loads(index.read_text())
    report['rebuilt_from'] = {'sha256': report['romfs_sha256'], 'size': report['romfs_size']}
    report.update(romfs_sha256=sha, romfs_size=size, files=files,
                  integrity=report['integrity'] + "; re-packed by a converter: rebuilt in Nintendo's layout, which "
                                                  'matched the verified 1.0.0 SHA-256')
    index.write_text(json.dumps(report, indent=2) + '\n')
    say('2/3 data: the rebuilt RomFS is byte for byte the verified 1.0.0 image')


def place(src, dst):
    """Hard link when possible (same drive, no extra space), otherwise copy. Returns 'link' or 'copy'."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        if os.path.samefile(src, dst):
            return 'link'
        dst.unlink()
    try:
        os.link(src, dst)
        return 'link'
    except OSError:
        shutil.copy2(src, dst)
        return 'copy'


def output_files(tree, exefs, romfs, fonts):
    """(published path, source) for everything the runtime reads."""
    files = [(f'data/v0/exefs/{n}', exefs / n) for n in EXPECTED['exefs_sha256']]
    files += [('data/v0/romfs/romfs.bin', romfs / 'romfs.bin'), ('data/v0/romfs/index.json', romfs / 'index.json')]
    for sub in ('romfs-patches', 'romfs-patches-out'):
        base = tree / 'extracted/v0' / sub
        files += [(f'data/v0/{sub}/{p.relative_to(base).as_posix()}', p) for p in sorted(base.rglob('*')) if p.is_file()]
    files += [(f'data/firmware/shared-fonts/{p.name}', p) for p in sorted(fonts.iterdir()) if p.is_file()]
    return files


def step_output(tree, out, exefs, romfs, fonts):
    say(f'3/3 output: {out}')
    tmp = out.with_name(out.name + '.partial')
    if tmp.exists():
        shutil.rmtree(tmp)
    records, how = [], {'link': 0, 'copy': 0}
    for rel, src in output_files(tree, exefs, romfs, fonts):
        how[place(src, tmp / rel)] += 1
        big = rel.endswith('romfs.bin')
        # The 17 GB image is identified by the verified RomFS hash; hashing it again would take minutes.
        digest = EXPECTED['romfs']['sha256'] if big else sha256(src)
        records.append({'path': rel, 'size': src.stat().st_size, 'sha256': digest})
    manifest = {'format': OUTPUT_FORMAT, 'game': 'totk', 'title_id': TITLE_ID, 'version': 'v0',
                'display_version': EXPECTED.get('display_version', '1.0.0'),
                'upstream': subprocess.run(['git', '-C', str(UPSTREAM), 'rev-parse', 'HEAD'], capture_output=True,
                                           text=True).stdout.strip(),
                'built': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'files': records}
    (tmp / 'wintears-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    if out.exists():
        old = out.with_name(out.name + '.old')
        if old.exists():
            shutil.rmtree(old)
        out.rename(old)
        tmp.rename(out)
        shutil.rmtree(old)
    else:
        tmp.rename(out)
    say(f'3/3 output: {len(records)} files ({how["link"]} hard-linked, {how["copy"]} copied)')
    return manifest


def verify_output(out, *, full=False):
    """Check an output folder against its manifest: sizes always, SHA-256 of every file except the RomFS image
    unless full=True. Returns a list of problems (empty: ok)."""
    m = json.loads((out / 'wintears-manifest.json').read_text())
    problems = []
    for r in m['files']:
        p = out / r['path']
        if not p.is_file():
            problems.append(f'missing: {r["path"]}')
        elif p.stat().st_size != r['size']:
            problems.append(f'wrong size: {r["path"]}')
        elif (full or not r['path'].endswith('romfs.bin')) and sha256(p) != r['sha256']:
            problems.append(f'wrong contents: {r["path"]}')
    return problems


def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('game', type=Path, nargs='?', help='your Tears of the Kingdom 1.0.0 XCI or NSP')
    p.add_argument('-k', '--keys', type=Path, help='prod.keys dumped from your own Switch')
    p.add_argument('-o', '--output', type=Path, help='output folder (default: TOTK_win next to the game file)')
    p.add_argument('--work', type=Path, help='work folder (default: TOTK_build next to the game file)')
    p.add_argument('--firmware', type=Path, help='unpacked firmware NCA folder (shared fonts, for an NSP)')
    p.add_argument('--shared-fonts', type=Path, help='folder with the six extracted .bfttf shared fonts')
    p.add_argument('--check', type=Path, metavar='OUTPUT', help='only verify an existing output folder')
    p.add_argument('--full', action='store_true', help='with --check: also hash the 17 GB RomFS image')
    a = p.parse_args(argv)
    if not a.check and (not a.game or not a.keys):
        p.error('the game file and -k/--keys are required')
    return a


def main(argv):
    args = parse_args(argv)
    try:
        if args.check:
            problems = verify_output(args.check, full=args.full)
            for line in problems:
                print(line)
            print('ok' if not problems else f'{len(problems)} problem(s)')
            return 1 if problems else 0
        prerequisites()
        game, keys = args.game.resolve(), args.keys.resolve()
        if not game.is_file():
            raise BuildError(f'game file not found: {game}')
        if not keys.is_file():
            raise BuildError(f'keys file not found: {keys}')
        kind = mng.input_kind(game)
        if kind not in ('xci', 'nsp'):
            raise BuildError('the base game must be an XCI or NSP (updates are not supported yet)')
        work = (args.work or game.parent / 'TOTK_build').resolve()
        out = (args.output or game.parent / 'TOTK_win').resolve()
        logs = work / 'logs'
        say(f'game file: {game.name} ({kind.upper()}), work folder: {work}')
        tree, py = prepare_work(work, logs)
        exefs = step_code(tree, py, game, keys, logs)
        romfs, fonts = step_data(tree, py, game, keys, logs, firmware=args.firmware, shared_fonts=args.shared_fonts)
        step_output(tree, out, exefs, romfs, fonts)
        problems = verify_output(out)
        if problems:
            raise BuildError('the output folder does not match its manifest: ' + '; '.join(problems[:5]))
        say('done. The game runtime itself is not built yet: see docs/PORTING_PLAN.md for the current phase.')
        return 0
    except BuildError as e:
        print(f'\nerror: {e}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
