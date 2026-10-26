"""Direct Native phase 0: a survey of the game's code modules (docs/DIRECT_NATIVE_PLAN.md).

Reads the NSOs in a built TOTK_win folder and prints totals only: code size, instruction classes, function starts
found ahead of time (symbols, eh_frame, relocations, BL targets), how much of the code a recursive descent from those
starts reaches, and the indirect branches an ahead-of-time translator has to handle. It prints no code, addresses or
names from the game. Optional --json writes the same totals to a file (keep it out of the repo: derived data rules).

    python tools/recomp/survey.py <TOTK_win> [--json out.json]
"""
import argparse, json, os, struct, sys, time
import numpy as np

MODULES = ['main', 'subsdk0', 'sdk']


def lz4_block(src, size):
    out = bytearray(size); o = i = 0; n = len(src)
    while i < n:
        t = src[i]; i += 1
        lit = t >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255: break
        out[o:o + lit] = src[i:i + lit]; i += lit; o += lit
        if i >= n: break
        off = src[i] | src[i + 1] << 8; i += 2
        ml = (t & 15) + 4
        if t & 15 == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255: break
        s = o - off
        if off >= ml: out[o:o + ml] = out[s:s + ml]
        else:
            for k in range(ml): out[o + k] = out[s + k]
        o += ml
    assert o == size, 'LZ4 size mismatch'
    return out


def load_nso(path):
    f = open(path, 'rb').read()
    assert f[:4] == b'NSO0', path
    flags = struct.unpack_from('<I', f, 0xC)[0]
    segs = []
    for k in range(3):
        fo, mo, sz = struct.unpack_from('<III', f, 0x10 + 16 * k)
        st = struct.unpack_from('<I', f, 0x60 + 4 * k)[0] if flags >> k & 1 else sz
        segs.append((fo, mo, sz, st, flags >> k & 1))
    bss = struct.unpack_from('<I', f, 0x3C)[0]
    img = bytearray(segs[2][1] + segs[2][2] + bss)
    for fo, mo, sz, st, comp in segs:
        img[mo:mo + sz] = lz4_block(f[fo:fo + st], sz) if comp else f[fo:fo + sz]
    sym_off, sym_size = struct.unpack_from('<II', f, 0x98)
    return img, segs, sym_off, sym_size


def dynamic(img):
    mod0 = struct.unpack_from('<I', img, 4)[0]
    assert img[mod0:mod0 + 4] == b'MOD0'
    dyn = mod0 + struct.unpack_from('<i', img, mod0 + 4)[0]
    eh_lo = mod0 + struct.unpack_from('<i', img, mod0 + 16)[0]
    eh_hi = mod0 + struct.unpack_from('<i', img, mod0 + 20)[0]
    t = {}
    for k in range(4096):
        tag, val = struct.unpack_from('<qQ', img, dyn + 16 * k)
        if tag == 0: break
        t.setdefault(tag, val)
    return t, eh_lo, eh_hi


def eh_frame_starts(img, lo, hi):
    """Function starts from .eh_frame_hdr's binary-search table (encoding datarel sdata4 expected)."""
    if hi - lo < 12: return []
    ver, ptr_enc, cnt_enc, tab_enc = img[lo:lo + 4]
    if ver != 1 or tab_enc != 0x3B: return []
    p = lo + 4 + 4   # eh_frame_ptr (sdata4)
    cnt = struct.unpack_from('<I', img, p)[0] if cnt_enc in (0x03, 0x0B) else 0
    p += 4
    return [lo + struct.unpack_from('<i', img, p + 8 * k)[0] for k in range(cnt)]


def survey(img, segs, sym_off, sym_size):
    t_lo, t_hi = segs[0][1], segs[0][1] + segs[0][2]
    in_text = lambda a: t_lo <= a < t_hi and not (a - t_lo) & 3
    w = np.frombuffer(bytes(img[t_lo:t_hi]), dtype='<u4')
    r = {'text_bytes': int(t_hi - t_lo), 'words': int(w.size)}
    m = lambda mask, val: int(np.count_nonzero((w & mask) == val))
    cls = {
        'bl': m(0xFC000000, 0x94000000), 'b': m(0xFC000000, 0x14000000),
        'b_cond_cbz_tbz': m(0xFF000010, 0x54000000) + m(0x7E000000, 0x34000000) + m(0x7E000000, 0x36000000),
        'br': m(0xFFFFFC1F, 0xD61F0000), 'blr': m(0xFFFFFC1F, 0xD63F0000), 'ret': m(0xFFFFFC1F, 0xD65F0000),
        'svc': m(0xFFE0001F, 0xD4000001), 'mrs': m(0xFFF00000, 0xD5300000), 'msr': m(0xFFF00000, 0xD5100000),
        'barriers': m(0xFFFFF09F, 0xD503309F),
        'exclusive_or_ordered': m(0x3F000000, 0x08000000), 'lse_atomics': m(0x3B200C00, 0x38200000),
        'simd_fp_data': int(np.count_nonzero(((w >> 25) & 7) == 7)),
        'simd_fp_loadstore': int(np.count_nonzero((((w >> 25) & 5) == 4) & ((w >> 26) & 1 == 1))),
        'zero_words': m(0xFFFFFFFF, 0),
    }
    r['classes'] = cls

    # function starts
    tags, eh_lo, eh_hi = dynamic(img)
    seeds = {}
    def seed(a, src):
        if in_text(a): seeds.setdefault(a, src)
    nsym = 0
    if 6 in tags:
        symtab = tags[6]
        if 4 in tags: nsym = struct.unpack_from('<I', img, tags[4] + 4)[0]
        else: nsym = sym_size // 24
        for k in range(nsym):
            name, info, other, shn, val, size = struct.unpack_from('<IBBHQQ', img, symtab + 24 * k)
            if shn and (info & 15) == 2: seed(val, 'symbol')
    for a in eh_frame_starts(img, eh_lo, eh_hi): seed(a, 'eh_frame')
    if 7 in tags:
        for k in range(tags.get(8, 0) // 24):
            off, info, add = struct.unpack_from('<QQq', img, tags[7] + 24 * k)
            if info & 0xFFFFFFFF == 1027: seed(add, 'relocation')
    if 36 in tags:
        where = 0
        for k in range(tags.get(35, 0) // 8):
            e = struct.unpack_from('<Q', img, tags[36] + 8 * k)[0]
            if not e & 1:
                where = e; seed(struct.unpack_from('<Q', img, where)[0], 'relocation'); where += 8
            else:
                for b in range(63):
                    if e >> (b + 1) & 1: seed(struct.unpack_from('<Q', img, where + 8 * b)[0], 'relocation')
                where += 63 * 8
    bl_idx = np.nonzero((w & 0xFC000000) == 0x94000000)[0]
    for i in bl_idx.tolist():
        imm = int(w[i]) & 0x3FFFFFF
        seed(t_lo + 4 * i + ((imm - (1 << 26) if imm >> 25 else imm) << 2), 'bl_target')
    by = {}
    for s in seeds.values(): by[s] = by.get(s, 0) + 1
    r['function_starts'] = {'total': len(seeds), **by}
    r['dynamic_symbols'] = nsym

    # recursive descent from every start: which code is reachable without running it
    W = w.tolist(); n = len(W)
    seen = bytearray(n); work = [(a - t_lo) >> 2 for a in seeds]
    br_jt = br_stub = br_mem = br_other = 0
    while work:
        i = work.pop()
        while 0 <= i < n and not seen[i]:
            seen[i] = 1; x = W[i]
            if x & 0xFC000000 == 0x14000000:      # b
                imm = x & 0x3FFFFFF; work.append(i + (imm - (1 << 26) if imm >> 25 else imm)); break
            if x & 0xFF000010 == 0x54000000 or x & 0x7E000000 == 0x34000000:   # b.cond, cbz/cbnz
                imm = (x >> 5) & 0x7FFFF; work.append(i + (imm - (1 << 19) if imm >> 18 else imm))
            elif x & 0x7E000000 == 0x36000000:     # tbz/tbnz
                imm = (x >> 5) & 0x3FFF; work.append(i + (imm - (1 << 14) if imm >> 13 else imm))
            elif x & 0xFFFFFC1F == 0xD61F0000:     # br: a switch table if a table load comes just before it
                back = W[max(0, i - 8):i]
                rn = (x >> 5) & 31
                if any((y & 0xFFE00C00) in (0x38600800, 0x78600800, 0xB8A00800, 0xB8600800) for y in back): br_jt += 1   # ldrb/ldrh/ldrsw/ldr w [xn, xm{, lsl}]
                elif rn in (16, 17): br_stub += 1   # import stubs and veneers (adrp/ldr/br x16-x17): known targets
                elif any(y & 0xFFC0001F == 0xF9400000 | rn for y in W[max(0, i - 3):i]): br_mem += 1   # ldr xN, [..]; br xN: pointer tail call
                else: br_other += 1
                break
            elif x & 0xFFFFFC1F == 0xD65F0000 or x == 0 or x & 0xFFFF0000 == 0:   # ret, udf
                break
            i += 1
    reached = int(sum(seen))
    left = np.frombuffer(bytes(seen), dtype=np.uint8) == 0
    r['descent'] = {'words_reached': reached, 'share_of_text': round(reached / n, 4),
                    'unreached_nonzero': int(np.count_nonzero(left & (w != 0))),
                    'br_switch_tables': br_jt, 'br_stubs': br_stub, 'br_pointer_tail_calls': br_mem, 'br_other_indirect': br_other}
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('totk_win'); ap.add_argument('--json')
    a = ap.parse_args()
    exefs = os.path.join(a.totk_win, 'data', 'v0', 'exefs')
    out = {}
    for mod in MODULES:
        t0 = time.time()
        img, segs, so, ss = load_nso(os.path.join(exefs, mod))
        r = survey(img, segs, so, ss)
        out[mod] = r
        c, f, d = r['classes'], r['function_starts'], r['descent']
        print(f"{mod}: code {r['text_bytes'] / 2**20:.1f} MiB, {r['words']:,} words; "
              f"function starts {f['total']:,} ({', '.join(f'{k} {v:,}' for k, v in f.items() if k != 'total')}); "
              f"reached by descent {d['share_of_text']:.1%} ({d['unreached_nonzero']:,} non-zero words not reached); "
              f"indirect jumps: {d['br_switch_tables']:,} switch tables, {d['br_stubs']:,} stubs, "
              f"{d['br_pointer_tail_calls']:,} pointer tail calls, {d['br_other_indirect']:,} other; "
              f"{c['blr']:,} indirect calls, {c['svc']:,} svc, "
              f"{c['exclusive_or_ordered'] + c['lse_atomics']:,} atomic/ordered, "
              f"SIMD/FP {(c['simd_fp_data'] + c['simd_fp_loadstore']) / r['words']:.1%} ({time.time() - t0:.0f} s)")
    tot = sum(r['words'] for r in out.values())
    print(f"all: {tot:,} instructions ({tot * 4 / 2**20:.1f} MiB of code), "
          f"{sum(r['function_starts']['total'] for r in out.values()):,} function starts")
    if a.json: json.dump(out, open(a.json, 'w'), indent=1)


if __name__ == '__main__':
    main()
