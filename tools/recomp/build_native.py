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


def compile_one(src, obj, inc):
    for attempt in range(8):
        r = subprocess.run(['gcc', *FLAGS, '-c', '-I', inc, src, '-o', obj], capture_output=True, text=True)
        if r.returncode == 0: return None
        if not any(s in r.stderr for s in ('internal compiler error', 'Segmentation fault', 'confused by earlier errors')): break
        time.sleep(1 + attempt)
    return f'{os.path.basename(src)}: {r.stderr.strip()[:600]}'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('totk_win'); ap.add_argument('modules', nargs='*', default=['main', 'subsdk0', 'sdk'])
    ap.add_argument('--jobs', type=int, default=3)   # MSYS2 GCC crashes at random under more parallel load
    ap.add_argument('--clean', action='store_true', help='empty the object cache first')
    ap.add_argument('--quick', action='store_true', help='-Og: faster to build, slower code (for trying changes)')
    a = ap.parse_args()
    if a.quick: FLAGS[0] = '-Og'
    tool = os.path.join(ROOT, 'build', 'wt_recomp.exe' if os.name == 'nt' else 'wt_recomp')
    inc = os.path.join(ROOT, 'src', 'cpu', 'native')
    salt = hashlib.sha256(' '.join(FLAGS).encode())
    for h in sorted(glob.glob(os.path.join(inc, '*.h'))): salt.update(open(h, 'rb').read())
    out = os.path.join(a.totk_win, 'native')
    seeds = os.path.join(out, 'misses.txt')   # block starts seen at run time (TOTK_NATIVE_MISSES), when recorded
    # everything the C depends on: recompiler, game code, seeds, headers and flags; unchanged -> the module is skipped
    inputs = salt.copy()
    for f in [tool, seeds] + [os.path.join(a.totk_win, 'data', 'v0', 'exefs', m) for m in ('main', 'subsdk0', 'sdk')]:
        if os.path.exists(f): inputs.update(open(f, 'rb').read())
    inputs = inputs.hexdigest()
    for mod in a.modules:
        t0 = time.time()
        dll = os.path.join(out, f'totk_{mod}.dll')
        if not a.clean and os.path.exists(dll) and os.path.exists(dll + '.inputs') and open(dll + '.inputs').read() == inputs:
            print(f'{mod}: up to date ({os.path.getsize(dll) / 2**20:.0f} MiB)')
            continue
        # one cache folder per flag set: switching between -O1 and --quick keeps both
        d, cache = os.path.join(out, mod), os.path.join(out, 'cache', mod, hashlib.sha256(' '.join(FLAGS).encode()).hexdigest()[:12])
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
        todo = [f for f in srcs if not os.path.exists(os.path.join(cache, keys[f] + '.o'))]
        t1 = time.time()

        def build(f):
            tmp = os.path.join(cache, keys[f] + '.tmp.o')
            err = compile_one(os.path.join(d, f), tmp, inc)
            if not err: os.replace(tmp, os.path.join(cache, keys[f] + '.o'))
            return err
        with cf.ThreadPoolExecutor(a.jobs) as ex:
            errs = [e for e in ex.map(build, todo) if e]
        if errs: sys.exit('\n'.join(errs[:5]))
        t2 = time.time()
        dll = os.path.join(out, f'totk_{mod}.dll')
        stamp = hashlib.sha256('\n'.join(keys[f] for f in srcs).encode()).hexdigest()
        linked = 'skipped (unchanged)'
        if not (os.path.exists(dll) and os.path.exists(dll + '.stamp') and open(dll + '.stamp').read() == stamp):
            rsp = os.path.join(d, 'objs.rsp')
            open(rsp, 'w').write('\n'.join(os.path.join(cache, keys[f] + '.o').replace('\\', '/') for f in srcs))
            r = subprocess.run(['gcc', '-shared', '-s', '-o', dll, '@' + rsp], capture_output=True, text=True)
            if r.returncode: sys.exit(f'link failed for {mod}: {r.stderr[:2000]}')
            open(dll + '.stamp', 'w').write(stamp)
            linked = f'{time.time() - t2:.0f} s'
        used = {keys[f] + '.o' for f in srcs}   # this flag set's cache keeps only what this build uses
        for o in os.listdir(cache):
            if o not in used: os.remove(os.path.join(cache, o))
        shutil.rmtree(d)
        open(dll + '.inputs', 'w').write(inputs)
        print(f'{mod}: emit {t1 - t0:.0f} s, compiled {len(todo)} of {len(srcs)} files ({len(srcs) - len(todo)} from the cache) '
              f'in {t2 - t1:.0f} s, link {linked}, {os.path.getsize(dll) / 2**20:.0f} MiB -> {dll}')


if __name__ == '__main__':
    main()
