#!/usr/bin/env python3
"""Rebuild a RomFS image in Nintendo's layout, for game files whose RomFS a conversion tool re-packed.

    python tools/romfs_rebuild.py <romfs.bin> <output romfs.bin>

Some NSP converters extract the RomFS and pack it again with their own builder: same files, same contents, but
another file order (e.g. paths sorted with '\\' as the separator), so the image no longer matches the verified
1.0.0 SHA-256 even though every IVFC level checks out. This writes the same files in the layout of Nintendo's
builder, which reproduces the original image byte for byte when the contents are genuine:

  - directories and files ordered by their full path, '/'-separated, compared as bytes; sibling lists by name
  - file data 16-byte aligned from the data partition at 0x200; the tables start at the next 8-byte boundary
  - tables in the usual order (directory hash, directory entries, file hash, file entries); hash buckets
    sized with the standard prime-ish count, entries inserted in table order, each new one becomes the bucket head

Whether the result is the original is decided only by the caller comparing its SHA-256 with the expected one:
a rebuilt image is accepted only on an exact match. Standard library only; prints no file contents.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

NONE = 0xffffffff
DATA = 0x200
CHUNK = 1 << 24


def pad4(n):
    return (n + 3) & ~3


def entry_count(n):
    """Hash bucket count used by Nintendo's builder."""
    if n < 3:
        return 3
    if n < 19:
        return n | 1
    while any(n % p == 0 for p in (2, 3, 5, 7, 11, 13, 17)):
        n += 1
    return n


def name_hash(parent, name):
    h = parent ^ 123456789
    for c in name:
        h = ((h >> 5) | (h << 27)) & 0xffffffff
        h ^= c
    return h


def read_tree(f):
    """Directories {path tuple: [file names]} and files {path tuple: (data offset, size)} of a RomFS image."""
    h = struct.unpack('<10Q', f.read(80))
    if h[0] != 80 or h[9] != DATA:
        raise ValueError('not a RomFS image')
    f.seek(h[3]); dmeta = f.read(h[4])
    f.seek(h[7]); fmeta = f.read(h[8])
    if len(dmeta) != h[4] or len(fmeta) != h[8]:
        raise ValueError('truncated RomFS tables')
    dirs, files = {}, {}
    todo, seen = [(0, None)], set()
    while todo:
        off, parent = todo.pop()
        if off in seen or off + 24 > len(dmeta):
            raise ValueError('invalid RomFS directory table')
        seen.add(off)
        _, sib, child, file, _, n = struct.unpack_from('<6I', dmeta, off)
        path = () if parent is None else parent + (bytes(dmeta[off + 24:off + 24 + n]),)
        dirs[path] = []
        if sib != NONE:
            todo.append((sib, parent))
        if child != NONE:
            todo.append((child, path))
        while file != NONE:
            if file + 32 > len(fmeta) or (path, file) in files:
                raise ValueError('invalid RomFS file table')
            _, nxt, data, size, _, n = struct.unpack_from('<IIQQII', fmeta, file)
            name = bytes(fmeta[file + 32:file + 32 + n])
            dirs[path].append(name)
            files[path + (name,)] = (data, size)
            file = nxt
    return dirs, files


def layout(dirs, files, separator=b'/'):
    """(header, tables, [(path, source data offset, size, new data offset)]) in Nintendo's layout. Another
    `separator` gives the order of builders that sort paths differently (b'\\\\': the converter seen so far)."""
    key = separator.join
    dorder = sorted(dirs, key=key)
    forder = sorted(files, key=key)
    doff, pos = {}, 0
    for d in dorder:
        doff[d] = pos
        pos += 24 + pad4(len(d[-1]) if d else 0)
    foff, pos = {}, 0
    for p in forder:
        foff[p] = pos
        pos += 32 + pad4(len(p[-1]))
    placed, end = [], 0
    for p in forder:
        end = (end + 15) & ~15
        placed.append((p, files[p][0], files[p][1], end))
        end += files[p][1]
    new = {p: o for p, _, _, o in placed}

    def chains(count, items):
        buckets, nxt = [NONE] * count, {}
        for h, me in items:
            nxt[me] = buckets[h % count]
            buckets[h % count] = me
        return struct.pack(f'<{count}I', *buckets), nxt

    dhash, dnext = chains(entry_count(len(dorder)),
                          [(name_hash(doff[d[:-1]] if d else 0, d[-1] if d else b''), doff[d]) for d in dorder])
    fhash, fnext = chains(entry_count(len(forder)), [(name_hash(doff[p[:-1]], p[-1]), foff[p]) for p in forder])
    children = {d: [] for d in dorder}
    for d in dorder[1:]:
        children[d[:-1]].append(d)
    dmeta = bytearray()
    for d in dorder:
        kids = sorted(children[d])
        sib = NONE
        if d:
            peers = sorted(children[d[:-1]])
            i = peers.index(d)
            sib = doff[peers[i + 1]] if i + 1 < len(peers) else NONE
        names = sorted(dirs[d])
        name = d[-1] if d else b''
        dmeta += struct.pack('<6I', doff[d[:-1]] if d else 0, sib, doff[kids[0]] if kids else NONE,
                             foff[d + (names[0],)] if names else NONE, dnext[doff[d]], len(name))
        dmeta += name.ljust(pad4(len(name)), b'\0')
    fsib = {}
    for d in dorder:
        names = sorted(dirs[d])
        for a, b in zip(names, names[1:] + [None]):
            fsib[d + (a,)] = foff[d + (b,)] if b is not None else NONE
    fmeta = bytearray()
    for p in forder:
        fmeta += struct.pack('<IIQQII', doff[p[:-1]], fsib[p], new[p], files[p][1], fnext[foff[p]], len(p[-1]))
        fmeta += p[-1].ljust(pad4(len(p[-1])), b'\0')
    tables = [dhash, bytes(dmeta), fhash, bytes(fmeta)]
    ofs = (DATA + end + 7) & ~7
    fields = [80]
    for t in tables:
        fields += [ofs, len(t)]
        ofs += len(t)
    fields.append(DATA)
    return struct.pack('<10Q', *fields), tables, placed


def write_image(emit, head, tables, placed, read, gap=bytes(DATA - 80)):
    """Emit a whole image: header, the bytes up to the data partition, file data (from read(source offset, n), in
    chunks), padding, tables."""
    emit(head)
    emit(gap)
    pos = 0
    for _, source, size, new in placed:
        if new > pos:
            emit(bytes(new - pos))
        done = 0
        while done < size:
            data = read(source + done, min(size - done, CHUNK))
            if not data:
                raise ValueError('RomFS file data out of bounds')
            emit(data)
            done += len(data)
        pos = new + size
    emit(bytes(struct.unpack_from('<Q', head, 8)[0] - DATA - pos))
    for t in tables:
        emit(t)


def rebuild(src, dst):
    """Write `src` in Nintendo's layout to `dst`. Returns (sha256 hex, size, index entries as extract_romfs.py
    writes them: path, absolute offset, size, sorted by path)."""
    digest = hashlib.sha256()
    with open(src, 'rb') as f, open(dst, 'wb') as out:
        dirs, files = read_tree(f)
        head, tables, placed = layout(dirs, files)
        f.seek(80)
        gap = f.read(DATA - 80)

        def emit(data):
            out.write(data)
            digest.update(data)

        def read(offset, n):
            f.seek(DATA + offset)
            return f.read(n)
        write_image(emit, head, tables, placed, read, gap)
        size = out.tell()
    index = sorted(({'path': b'/'.join(p).decode('utf-8'), 'offset': DATA + new, 'size': n}
                    for p, _, n, new in placed), key=lambda e: e['path'])
    return digest.hexdigest(), size, index


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    p.add_argument('source', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args(argv)
    if a.output.exists():
        sys.exit('error: output already exists')
    sha, size, index = rebuild(a.source, a.output)
    print(json.dumps({'size': size, 'sha256': sha, 'files': len(index)}))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
