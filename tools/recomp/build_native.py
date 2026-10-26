"""Direct Native: build the recompiled-code DLLs for a TOTK_win folder (vault: Wintears Direct Native Plan).

    python tools/recomp/build_native.py <TOTK_win> [main subsdk0 sdk] [--jobs N] [--keep]

For each module: build/wt_recomp emits C into <TOTK_win>/native/<module>/, gcc -O1 compiles it (-ffp-contract=off: no
fused multiply-add the ARM code did not ask for; x86-64-v3: AVX2 and FMA, which the Deck, laptop and PC have) in parallel (a file
gcc crashes on is retried: MSYS2 GCC sometimes crashes at random), and the objects are linked into
<TOTK_win>/native/totk_<module>.dll. The C and objects are derived from the game: they stay in that folder and are
deleted after linking unless --keep. Run the game with TOTK_NATIVE=1 to use the DLLs.
"""
import argparse, concurrent.futures as cf, os, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def compile_one(src, obj, inc):
    for attempt in range(8):
        r = subprocess.run(['gcc', '-O1', '-w', '--param=min-pagesize=0', '-ffp-contract=off', '-march=x86-64-v3', '-c', '-I', inc, src, '-o', obj], capture_output=True, text=True)
        if r.returncode == 0: return None
        if 'internal compiler error' not in r.stderr and 'Segmentation fault' not in r.stderr: break
        time.sleep(1 + attempt)
    return f'{os.path.basename(src)}: {r.stderr.strip()[:600]}'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('totk_win'); ap.add_argument('modules', nargs='*', default=['main', 'subsdk0', 'sdk'])
    ap.add_argument('--jobs', type=int, default=2)   # MSYS2 GCC crashes at random under more parallel load
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    tool = os.path.join(ROOT, 'build', 'wt_recomp.exe' if os.name == 'nt' else 'wt_recomp')
    inc = os.path.join(ROOT, 'src', 'cpu', 'native')
    out = os.path.join(a.totk_win, 'native')
    for mod in a.modules:
        t0 = time.time()
        d = os.path.join(out, mod)
        shutil.rmtree(d, ignore_errors=True); os.makedirs(os.path.join(d, 'obj'))
        r = subprocess.run([tool, a.totk_win, 'emit', d, mod, '2000'], capture_output=True, text=True)
        if r.returncode: sys.exit(f'wt_recomp failed for {mod}: {r.stderr or r.stdout}')
        print(r.stdout.splitlines()[3] if len(r.stdout.splitlines()) > 3 else r.stdout.strip())
        srcs = sorted(f for f in os.listdir(d) if f.endswith('.c'))
        t1 = time.time()
        with cf.ThreadPoolExecutor(a.jobs) as ex:
            errs = [e for e in ex.map(lambda f: compile_one(os.path.join(d, f), os.path.join(d, 'obj', f[:-2] + '.o'), inc), srcs) if e]
        if errs: sys.exit('\n'.join(errs[:5]))
        t2 = time.time()
        dll = os.path.join(out, f'totk_{mod}.dll')
        objs = [os.path.join(d, 'obj', f[:-2] + '.o') for f in srcs]
        rsp = os.path.join(d, 'objs.rsp')
        open(rsp, 'w').write('\n'.join(o.replace('\\', '/') for o in objs))
        r = subprocess.run(['gcc', '-shared', '-o', dll, '@' + rsp], capture_output=True, text=True)
        if r.returncode: sys.exit(f'link failed for {mod}: {r.stderr[:2000]}')
        if not a.keep: shutil.rmtree(d)
        print(f'{mod}: emit {t1 - t0:.0f} s, compile {t2 - t1:.0f} s ({len(srcs)} files), link {time.time() - t2:.0f} s, '
              f'{os.path.getsize(dll) / 2**20:.0f} MiB -> {dll}')


if __name__ == '__main__':
    main()
