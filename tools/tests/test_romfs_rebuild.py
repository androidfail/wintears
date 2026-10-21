"""Tests for tools/romfs_rebuild.py and build_win's use of it, with small synthetic RomFS images (no game data)."""
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'upstream/games/totk/scripts')]
import build_win  # noqa: E402
import romfs_rebuild as rr  # noqa: E402
from extract_romfs import index_romfs  # noqa: E402

PINNED = '29572cf4e6b6d4b3'

# Names chosen so that '/' and '\\' path orders differ ('-' and '.' sort below '/', letters above '\\' do not):
# 'AI/...' vs 'AISchedule/...', 'A-B/...' vs 'A/...'.
FILES = {
    'AI/Sub/deep.bin': b'deep' * 9,
    'AI/a.ainb': b'a' * 17,
    'AI/b.ainb': b'',
    'AISchedule/EachFrame/x.bgyml': b'x' * 31,
    'A-B/c.txt': b'c' * 16,
    'A/d.txt': b'd' * 3,
    'top.bin': b't' * 4097,
    'Zz/last.dat': b'z' * 5,
}


def image(files, separator=b'/'):
    """A RomFS image of `files`, laid out with paths ordered by `separator`."""
    dirs, entries, blob = {(): []}, {}, bytearray()
    for path, data in files.items():
        parts = tuple(p.encode() for p in path.split('/'))
        for i in range(len(parts)):
            dirs.setdefault(parts[:i], [])
        dirs[parts[:-1]].append(parts[-1])
        entries[parts] = (len(blob), len(data))
        blob += data
    head, tables, placed = rr.layout(dirs, entries, separator)
    out = bytearray()
    rr.write_image(out.extend, head, tables, placed, lambda off, n: bytes(blob[off:off + n]))
    return bytes(out)


class Rebuild(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)

    def rebuild(self, data):
        src, dst = self.path / 'src.bin', self.path / 'dst.bin'
        dst.unlink(missing_ok=True)
        src.write_bytes(data)
        return rr.rebuild(src, dst), dst

    def test_converter_order_is_rebuilt_to_nintendo_order(self):
        converted, original = image(FILES, b'\\'), image(FILES)
        self.assertNotEqual(converted, original)
        (sha, size, index), dst = self.rebuild(converted)
        self.assertEqual(dst.read_bytes(), original)
        self.assertEqual((sha, size), (hashlib.sha256(original).hexdigest(), len(original)))
        # the index is what upstream's extract_romfs.py would write for the rebuilt image
        self.assertEqual(index, index_romfs(dst))
        for entry in index:
            with dst.open('rb') as f:
                f.seek(entry['offset'])
                self.assertEqual(f.read(entry['size']), FILES[entry['path']])

    def test_original_layout_is_unchanged(self):
        original = image(FILES)
        _, dst = self.rebuild(original)
        self.assertEqual(dst.read_bytes(), original)

    def test_layout_is_pinned(self):
        """Nintendo's layout was confirmed on the real 1.0.0 RomFS (17 GB, exact SHA-256 match); this pins it."""
        original = image(FILES)
        self.assertEqual(len(original) % 4, 0)
        self.assertEqual(hashlib.sha256(original).hexdigest()[:16], PINNED)

    def test_damaged_tables_are_rejected(self):
        data = bytearray(image(FILES))
        data[0] = 0x51
        with self.assertRaises(ValueError):
            self.rebuild(bytes(data))


class BuildStep(unittest.TestCase):
    """build_win.canonical_romfs keeps a rebuilt image only on an exact match."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.romfs = Path(self.tmp.name) / 'romfs'
        self.romfs.mkdir()
        self.converted = image(FILES, b'\\')
        (self.romfs / 'romfs.bin').write_bytes(self.converted)
        (self.romfs / 'index.json').write_text(json.dumps({
            'status': 'pass', 'romfs_size': len(self.converted), 'integrity': 'IVFC verified',
            'romfs_sha256': hashlib.sha256(self.converted).hexdigest(), 'files': index_romfs(self.romfs / 'romfs.bin')}))

    def run_step(self, expected):
        with mock.patch.object(build_win, 'EXPECTED', {'romfs': expected}), mock.patch.object(build_win, 'say'):
            build_win.canonical_romfs(self.romfs)

    def test_match_replaces_image_and_index(self):
        original = image(FILES)
        want = {'sha256': hashlib.sha256(original).hexdigest(), 'size': len(original)}
        self.run_step(want)
        self.assertEqual((self.romfs / 'romfs.bin').read_bytes(), original)
        report = json.loads((self.romfs / 'index.json').read_text())
        self.assertEqual(report['romfs_sha256'], want['sha256'])
        self.assertEqual(report['files'], index_romfs(self.romfs / 'romfs.bin'))
        self.assertEqual(report['rebuilt_from']['sha256'], hashlib.sha256(self.converted).hexdigest())
        self.assertFalse((self.romfs / 'romfs.rebuilt.bin').exists())

    def test_no_match_stops_and_keeps_the_extracted_image(self):
        with self.assertRaisesRegex(build_win.BuildError, 'even rebuilt'):
            self.run_step({'sha256': '0' * 64, 'size': len(self.converted)})
        self.assertEqual((self.romfs / 'romfs.bin').read_bytes(), self.converted)
        self.assertFalse((self.romfs / 'romfs.rebuilt.bin').exists())



if __name__ == '__main__':
    unittest.main()
