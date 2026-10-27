"""Direct Native: build the recompiled-code DLLs for a TOTK_win folder (vault: Wintears Direct Native Plan).

    python tools/recomp/build_native.py <TOTK_win> [main subsdk0 sdk] [--jobs N] [--clean] [--quick]

For each module, build/wt_recomp emits C into <TOTK_win>/native/<module>/, one file per 16 KiB of guest code, named by
address. Each file is compiled with gcc unless the object cache already has it: <TOTK_win>/native/cache/<module>/ keeps
objects by a hash of the file, the headers and the flags, so a rebuild after a change to one opcode recompiles only the
files that use it (a header change still recompiles everything). The objects are linked into
<TOTK_win>/native/totk_<module>.dll; when every object is unchanged and the DLL is there, the link is skipped too.
Flags: -O1 (--quick: -Og, a third faster to compile but its code is 16% slower in play), -ffp-contract=off (no fused multiply-add the ARM code did not ask for), -march=x86-64-v3 (AVX2 and FMA:
the Deck, laptop and PC have them), no unwind tables or debug info (smaller and faster). A module whose inputs
(recompiler, game code, seeds, headers, flags) are unchanged is skipped without emitting. A file gcc crashes on is
retried (MSYS2 GCC sometimes crashes at random).

Code reached only through pointers can be missed by discovery: run the game with
TOTK_NATIVE_MISSES=<TOTK_win>/native/misses.txt and the next build adds the block starts recorded there.

Everything under native/ is derived from the game: it stays on this machine. Run the game with TOTK_NATIVE=1.
"""
import argparse, concurrent.futures as cf, glob, hashlib, os, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FLAGS = ['-O1', '-w', '--param=min-pagesize=0', '-ffp-contract=off', '-march=x86-64-v3', '-pipe', '-g0',
         '-fno-asynchronous-unwind-tables', '-fno-ident']


def compile_one(src, obj, inc, flags=None):
    for attempt in range(8):
        r = subprocess.run(['gcc', *(flags or FLAGS), '-c', '-I', inc, src, '-o', obj], capture_output=True, text=True)
        if r.returncode == 0: return None
        if not any(s in r.stderr for s in ('internal compiler error', 'Segmentation fault', 'confused by earlier errors')): break
        time.sleep(1 + attempt)
    return f'{os.path.basename(src)}: {r.stderr.strip()[:600]}'


def pick_hot(samples, dll, mod, share, hot_path):
    """The C files of `mod` that hold `share` of the samples (dll offsets, TOTK_SAMPLE_OUT), via the last link's map."""
    import bisect, re, struct
    if not (os.path.exists(dll + '.map') and os.path.exists(dll + '.files')):
        print(f'{mod}: no map from an earlier link yet: build once, run with TOTK_SAMPLE_OUT, then --hot'); return
    pe = open(dll, 'rb').read(4096); o = struct.unpack_from('<I', pe, 0x3c)[0]
    base = struct.unpack_from('<Q', pe, o + 24 + 24)[0]   # PE32+ ImageBase
    names = dict(l.split(' ', 1) for l in open(dll + '.files').read().splitlines() if ' ' in l)   # object -> C file
    ranges, pend = [], None   # (start, end, C file) of each object's .text
    for line in open(dll + '.map', errors='replace'):
        t = line.split()
        if pend and len(t) >= 3 and t[0].startswith('0x'): t = [pend] + t
        pend = t[0] if len(t) == 1 and t[0].startswith('.text') else None
        if len(t) >= 4 and t[0].startswith('.text') and t[1].startswith('0x') and t[3].endswith('.o'):
            f = names.get(os.path.basename(t[3].replace('\\', '/')))
            size = int(t[2], 16)
            if f and size: ranges.append((int(t[1], 16), int(t[1], 16) + size, f))
    ranges.sort(); starts = [r[0] for r in ranges]
    pat = re.compile(r'totk_' + re.escape(mod) + r'\.dll\+0x([0-9a-f]+)')
    per, n = {}, 0
    for line in open(samples, errors='replace'):
        m = pat.match(line.strip())
        if not m: continue
        va = base + int(m.group(1), 16); i = bisect.bisect_right(starts, va) - 1
        if i >= 0 and va < ranges[i][1]: per[ranges[i][2]] = per.get(ranges[i][2], 0) + 1; n += 1
    if not n: print(f'{mod}: no samples in {samples}'); return
    hot, got = [], 0
    for f, k in sorted(per.items(), key=lambda x: -x[1]):
        if got >= share * n: break
        hot.append(f); got += k
    open(hot_path, 'w').write('\n'.join(hot))
    print(f'{mod}: {n} samples, {len(hot)} of {len(ranges)} files hold {100 * got / n:.0f}% of them: those get -O2 ({hot_path})')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('totk_win'); ap.add_argument('modules', nargs='*', default=['main', 'subsdk0', 'sdk'])
    ap.add_argument('--jobs', type=int, default=3)   # MSYS2 GCC crashes at random under more parallel load
    ap.add_argument('--clean', action='store_true', help='empty the object cache first')
    ap.add_argument('--quick', action='store_true', help='-Og: faster to build, slower code (for trying changes)')
    ap.add_argument('--hot', metavar='SAMPLES', help='profile from a run with TOTK_SAMPLE=1 TOTK_SAMPLE_OUT=<file>: the files '
                    'holding --hot-share of the samples get -O2 (kept in native/hot_<module>.txt for later builds)')
    ap.add_argument('--hot-share', type=float, default=0.8)
    a = ap.parse_args()
    if a.quick: FLAGS[0] = '-Og'
    tool = os.path.join(ROOT, 'build', 'wt_recomp.exe' if os.name == 'nt' else 'wt_recomp')
    inc = os.path.join(ROOT, 'src', 'cpu', 'native')
    salt = hashlib.sha256(' '.join(FLAGS).encode())
    for h in sorted(glob.glob(os.path.join(inc, '*.h'))): salt.update(open(h, 'rb').read())
    out = os.path.join(a.totk_win, 'native')
    seeds = os.path.join(out, 'misses.txt')   # block starts seen at run time (TOTK_NATIVE_MISSES), when recorded
    # everything the C depends on: recompiler, game code, seeds, headers and flags; unchanged -> the module is skipped
    inputs0 = salt.copy()
    for f in [tool, seeds] + [os.path.join(a.totk_win, 'data', 'v0', 'exefs', m) for m in ('main', 'subsdk0', 'sdk')]:
        if os.path.exists(f): inputs0.update(open(f, 'rb').read())
    hot_flags = ['-O2'] + FLAGS[1:]
    for mod in a.modules:
        hot_path = os.path.join(out, f'hot_{mod}.txt')
        if a.hot: pick_hot(a.hot, os.path.join(out, f'totk_{mod}.dll'), mod, a.hot_share, hot_path)
        hot = set(open(hot_path).read().split()) if os.path.exists(hot_path) else set()
        inputs = inputs0.copy(); inputs.update(' '.join(sorted(hot)).encode()); inputs = inputs.hexdigest()
        t0 = time.time()
        dll = os.path.join(out, f'totk_{mod}.dll')
        if not a.clean and os.path.exists(dll) and os.path.exists(dll + '.inputs') and open(dll + '.inputs').read() == inputs:
            print(f'{mod}: up to date ({os.path.getsize(dll) / 2**20:.0f} MiB)')
            continue
        # one cache folder per flag set: switching between -O1 and --quick keeps both
        d, cache = os.path.join(out, mod), os.path.join(out, 'cache', mod, hashlib.sha256(' '.join(FLAGS).encode()).hexdigest()[:12])
        hcache = os.path.join(out, 'cache', mod, hashlib.sha256(' '.join(hot_flags).encode()).hexdigest()[:12])   # the hot files' -O2 objects
        os.makedirs(hcache, exist_ok=True)
        if a.clean: shutil.rmtree(os.path.dirname(cache), ignore_errors=True)
        shutil.rmtree(d, ignore_errors=True); os.makedirs(d); os.makedirs(cache, exist_ok=True)
        r = subprocess.run([tool, a.totk_win, 'emit', d, mod, '14'] + ([seeds] if os.path.exists(seeds) else []),
                           capture_output=True, text=True)
        if r.returncode: sys.exit(f'wt_recomp failed for {mod} ({r.returncode}): {r.stderr or r.stdout}')
        print(next((l for l in r.stdout.splitlines() if 'emitted' in l), ''))
        srcs = sorted(f for f in os.listdir(d) if f.endswith('.c'))
        keys = {}
        for f in srcs:
            h = salt.copy(); h.update(open(os.path.join(d, f), 'rb').read()); keys[f] = h.hexdigest()[:32]
        obj = {f: os.path.join(hcache if f in hot else cache, keys[f] + '.o') for f in srcs}
        todo = [f for f in srcs if not os.path.exists(obj[f])]
        t1 = time.time()

        def build(f):
            tmp = obj[f][:-2] + '.tmp.o'
            err = compile_one(os.path.join(d, f), tmp, inc, hot_flags if f in hot else FLAGS)
            if not err: os.replace(tmp, obj[f])
            return err
        with cf.ThreadPoolExecutor(a.jobs) as ex:
            errs = [e for e in ex.map(build, todo) if e]
        if errs: sys.exit('\n'.join(errs[:5]))
        t2 = time.time()
        dll = os.path.join(out, f'totk_{mod}.dll')
        stamp = hashlib.sha256('\n'.join(obj[f] for f in srcs).encode()).hexdigest()
        linked = 'skipped (unchanged)'
        if not (os.path.exists(dll) and os.path.exists(dll + '.stamp') and open(dll + '.stamp').read() == stamp and os.path.exists(dll + '.map')):
            rsp = os.path.join(d, 'objs.rsp')
            open(rsp, 'w').write('\n'.join(obj[f].replace('\\', '/') for f in srcs))
            # the map says which object (and so which C file, dll.files) each address came from: for --hot
            r = subprocess.run(['gcc', '-shared', '-s', '-Wl,-Map=' + dll + '.map', '-o', dll, '@' + rsp], capture_output=True, text=True)
            if r.returncode: sys.exit(f'link failed for {mod}: {r.stderr[:2000]}')
            open(dll + '.files', 'w').write('\n'.join(f'{os.path.basename(obj[f])} {f}' for f in srcs))
            open(dll + '.stamp', 'w').write(stamp)
            linked = f'{time.time() - t2:.0f} s'
        used = {os.path.basename(obj[f]) for f in srcs}   # each flag set's cache keeps only what this build uses
        for c in (cache, hcache):
            for o in os.listdir(c):
                if o not in used: os.remove(os.path.join(c, o))
        shutil.rmtree(d)
        open(dll + '.inputs', 'w').write(inputs)
        print(f'{mod}: emit {t1 - t0:.0f} s, compiled {len(todo)} of {len(srcs)} files ({len(srcs) - len(todo)} from the cache, {len(hot & set(srcs))} at -O2) '
              f'in {t2 - t1:.0f} s, link {linked}, {os.path.getsize(dll) / 2**20:.0f} MiB -> {dll}')


if __name__ == '__main__':
    main()
