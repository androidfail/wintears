#!/usr/bin/env python3
"""Translate the game's shaders before play, so they are not translated while playing (each one stalls the game for
13-45 ms; a new area used to freeze for up to 3 s).

    python tools/prepare_shaders.py <TOTK_win> [--profile DIR] [--translator EXE] [--jobs N]

Upstream's games/totk/scripts/prepare_shader_seed.py finds the shaders (every shader in the RomFS for the default bind
state, then each bind state its shared list, assets/shader_list.tsv, names for them); build/shader-service/wt_xlate.exe
translates them into the shader cache the runtime reads (<profile>/shader-cache/vk-1), one translator per core. Shaders
already in the cache are skipped, so it can run again after an update of the list. The cache holds game-derived data:
it stays on this PC (or the player's own drive).
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'upstream/games/totk/scripts'))
import prepare_shader_seed as seed   # noqa: E402
from shader_list import ShaderList   # noqa: E402


def runtime_answers(a):
    """A bind state in the runtime's text (src/gfx/vk/vk_shader.c answers_json): its SHA-256 names the variant. The runtime
    picks a variant by its .wtr deps, not by name, so a prepared one serves every bind state with the same deps."""
    def entries(d, value):
        return ', '.join(f'"{k}": {value(v)}' for k, v in sorted(d.items(), key=lambda kv: int(kv[0])))
    def img(v):
        if not isinstance(v, dict):
            return json.dumps(v)
        return '{"type": "%s"%s}' % (v['type'], ', "fmt": "%s"' % v['fmt'] if v.get('fmt') else '')
    text = lambda v: f'"{v}"'
    return '"tex": {%s}, "img": {%s}, "attr": {%s}, "fout": {%s}' % (
        entries(a.get('tex', {}), text), entries(a.get('img', {}), img), entries(a.get('attr', {}), text),
        entries(a.get('fout', {}), text))


def variant_name(key, answers):
    return key+'-'+hashlib.sha256(runtime_answers(answers).encode()).hexdigest()[:12]


def deps_of(wtr):
    """A translated variant's dependencies on the bind state (its .wtr 'dep' lines): equal deps = the same shader."""
    return tuple(sorted(line for line in wtr.read_text(encoding='utf-8', errors='replace').splitlines() if line.startswith('dep ')))


def profile_dir():
    if os.environ.get('TOTK_PROFILE_DIR'):
        return Path(os.environ['TOTK_PROFILE_DIR'])
    return Path(os.environ['LOCALAPPDATA'])/'WinTears'/'totk'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('game', type=Path, help='the TOTK_win folder from tools/build_win.py')
    ap.add_argument('--profile', type=Path, help='profile folder (default: TOTK_PROFILE_DIR or %%LOCALAPPDATA%%\\WinTears\\totk)')
    ap.add_argument('--translator', type=Path, default=ROOT/'build/shader-service/wt_xlate.exe')
    ap.add_argument('--jobs', type=int, default=max(1, min(8, (os.cpu_count() or 4))))
    args = ap.parse_args()
    romfs = args.game/'data/v0/romfs'
    if not (romfs/'index.json').is_file():
        sys.exit(f'{romfs}/index.json not found: give the TOTK_win folder')
    if not args.translator.is_file():
        sys.exit(f'{args.translator} not found: run python tools/build_xlate.py first')
    cache = (args.profile or profile_dir())/'shader-cache'/'vk-1'
    cache.mkdir(parents=True, exist_ok=True)
    for stale in cache.glob('*.code'):   # inputs of a run that was stopped
        stale.unlink()

    started = time.monotonic()
    _, shaders, startup, _ = seed.inventory(romfs)
    listed = ShaderList.load(seed.LIST)
    extra = seed.located({w for w in listed.locators if w[0] not in shaders}, seed.source_reader(romfs, None))
    shaders.update(extra)
    jobs = {}
    for key, answers, _, _ in seed.plan(shaders, startup, listed.variants):
        jobs.setdefault(key, []).append(answers)
    total = sum(len(v) for v in jobs.values())
    print(f'{len(shaders)} shaders, {total} bind states to translate ({time.monotonic()-started:.0f} s to find them); '
          f'{args.jobs} translators, cache {cache}', flush=True)

    lock = threading.Lock()
    pending = iter(sorted(jobs.items()))
    count = dict(done=0, new=0, same=0, failed=0)
    stop = threading.Event()

    def worker():
        service = subprocess.Popen([str(args.translator), '--service', '--out', str(cache)], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, text=True, encoding='utf-8', errors='replace', creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        try:
            while not stop.is_set():
                with lock:
                    item = next(pending, None)
                if item is None:
                    return
                key, states = item
                stage, control, code, _ = shaders[key]
                index = cache/(key+'.variants')
                names = index.read_text(encoding='utf-8').split() if index.is_file() else []
                have = {deps_of(cache/(n+'.wtr')) for n in names if (cache/(n+'.wtr')).is_file()}
                c1, length = struct.unpack_from('<II', control, 0x700)
                for answers in states:
                    variant = variant_name(key, answers)
                    result = 'same'
                    if variant not in names:
                        code_path = cache/(variant+'.code')
                        code_path.write_bytes(code[0 if stage == 'cs' else 0x30:length])
                        request = dict(variant=variant, stage=stage, code=str(code_path), c1=code[c1:length].hex(),
                                       depth_minus_w=True, **answers)
                        if stage == 'cs':
                            request['local'] = list(struct.unpack_from('<III', control, 0x748))
                        service.stdin.write(json.dumps(request)+'\n'); service.stdin.flush()
                        reply = service.stdout.readline()
                        code_path.unlink(missing_ok=True)
                        if not reply:
                            raise RuntimeError('the translator stopped')
                        result = 'failed'
                        if json.loads(reply).get('ok'):
                            deps = deps_of(cache/(variant+'.wtr'))
                            if deps in have:   # the bind state changes nothing this shader reads: one variant is enough
                                (cache/(variant+'.spv')).unlink(missing_ok=True); (cache/(variant+'.wtr')).unlink(missing_ok=True)
                                result = 'same'
                            else:
                                have.add(deps); names.append(variant)
                                with index.open('a', encoding='utf-8', newline='\n') as f:   # the runtime reads \n lines
                                    f.write(variant+'\n')
                                result = 'new'
                    with lock:
                        count['done'] += 1; count[result] += 1
                        if count['done'] % 1000 == 0:
                            print(f"{count['done']}/{total}: {count['new']} translated, {count['failed']} failed; "
                                  f'{time.monotonic()-started:.0f} s', flush=True)
        except BaseException:
            stop.set()
            raise
        finally:
            service.stdin.close(); service.wait()

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for f in [pool.submit(worker) for _ in range(args.jobs)]:
            f.result()
    print(f"done in {time.monotonic()-started:.0f} s: {count['new']} variants translated, {count['same']} already there "
          f"or the same as another, {count['failed']} failed (translated in play if the game needs them)", flush=True)


if __name__ == '__main__':
    main()
