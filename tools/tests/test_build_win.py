"""Tests for tools/build_win.py with fake work folders: no game data or keys.

Run: python -m unittest discover -s tools/tests -v
"""
import hashlib
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import build_win  # noqa: E402


def sha(data):
    return hashlib.sha256(data).hexdigest()


class FakeTree:
    """A work tree laid out like the real one after steps 1 and 2."""

    def __init__(self, root):
        self.tree = root / 'work/totk'
        self.exefs = self.tree / 'extracted/v0/exefs'
        self.romfs = self.tree / 'extracted/v0/romfs'
        self.fonts = self.tree / 'extracted/firmware/shared-fonts'
        self.modules = {}
        for name in ('main', 'main.npdm', 'rtld', 'sdk', 'subsdk0'):
            data = f'module {name}'.encode()
            self.modules[name] = sha(data)
            self.write(self.exefs / name, data)
        self.romfs_bytes = b'R' * 4096
        self.write(self.romfs / 'romfs.bin', self.romfs_bytes)
        self.write(self.romfs / 'index.json', b'{}')
        self.write(self.romfs / 'runtime-index.bin', b'TOTKRFS1')
        self.write(self.romfs / 'romfs-config.txt', b'image_size 4096\n')
        self.write(self.tree / 'extracted/v0/romfs-patches/manifest.tsv', b'# none\n')
        self.write(self.tree / 'extracted/v0/romfs-patches/UI/a.bin', b'overlay')
        self.write(self.tree / 'extracted/v0/romfs-patches-out/2560x1440/manifest.tsv', b'# none\n')
        self.write(self.fonts / 'standard-a.bfttf', b'font')

    @staticmethod
    def write(path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def expected(self):
        return {'title_id': '0100F2C0115B6000', 'display_version': '1.0.0', 'exefs_sha256': dict(self.modules),
                'romfs': {'size': len(self.romfs_bytes), 'sha256': sha(self.romfs_bytes)}}


class Output(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.fake = FakeTree(self.root)
        self.patch = mock.patch.object(build_win, 'EXPECTED', self.fake.expected())
        self.patch.start()
        self.out = self.root / 'TOTK_win'

    def tearDown(self):
        self.patch.stop()
        self.tmp.cleanup()

    def build(self):
        f = self.fake
        return build_win.step_output(f.tree, self.out, f.exefs, f.romfs, f.fonts)

    def test_output_has_every_file_and_verifies(self):
        m = self.build()
        paths = {r['path'] for r in m['files']}
        self.assertIn('data/v0/exefs/main', paths)
        self.assertIn('data/v0/romfs/romfs.bin', paths)
        self.assertIn('data/v0/romfs-patches/UI/a.bin', paths)
        self.assertIn('data/firmware/shared-fonts/standard-a.bfttf', paths)
        self.assertEqual(m['version'], 'v0')
        self.assertEqual(build_win.verify_output(self.out), [])
        self.assertEqual(build_win.verify_output(self.out, full=True), [])
        self.assertFalse(self.out.with_name('TOTK_win.partial').exists())

    def test_large_files_are_hard_linked_on_the_same_drive(self):
        self.build()
        import os
        self.assertTrue(os.path.samefile(self.out / 'data/v0/romfs/romfs.bin', self.fake.romfs / 'romfs.bin'))

    def test_damaged_output_is_reported(self):
        self.build()
        (self.out / 'data/v0/exefs/sdk').unlink()
        (self.out / 'data/v0/exefs/sdk').write_bytes(b'module sdX')        # same size, other bytes
        (self.out / 'data/firmware/shared-fonts/standard-a.bfttf').unlink()
        problems = build_win.verify_output(self.out)
        self.assertIn('wrong contents: data/v0/exefs/sdk', problems)
        self.assertIn('missing: data/firmware/shared-fonts/standard-a.bfttf', problems)

    def test_romfs_contents_only_hashed_on_full_check(self):
        self.build()
        img = self.out / 'data/v0/romfs/romfs.bin'
        img.unlink()                                  # break the hard link, then write other bytes of the same size
        img.write_bytes(b'X' * len(self.fake.romfs_bytes))
        self.assertEqual(build_win.verify_output(self.out), [])
        self.assertEqual(build_win.verify_output(self.out, full=True), ['wrong contents: data/v0/romfs/romfs.bin'])

    def test_rebuild_replaces_previous_output(self):
        self.build()
        (self.out / 'stale.txt').write_text('old')
        self.build()
        self.assertFalse((self.out / 'stale.txt').exists())
        self.assertEqual(build_win.verify_output(self.out), [])


class Code(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.fake = FakeTree(self.root)
        self.game = self.root / 'game.xci'
        self.game.write_bytes(b'x')

    def tearDown(self):
        self.tmp.cleanup()

    def step(self, expected):
        f = self.fake
        with mock.patch.object(build_win, 'EXPECTED', expected), \
             mock.patch.object(build_win.mng, 'source_matches', return_value=True), \
             mock.patch.object(build_win, 'run') as run:
            (f.tree / 'extracted/v0/elf').mkdir(parents=True, exist_ok=True)
            (f.tree / 'extracted/v0/elf/main-inventory.json').write_text('{}')
            build_win.step_code(f.tree, Path('python'), self.game, Path('prod.keys'), self.root / 'logs')
            return run

    def test_matching_exefs_is_reused_without_extraction(self):
        run = self.step(self.fake.expected())
        run.assert_not_called()

    def test_other_version_stops_the_build(self):
        expected = self.fake.expected()
        expected['exefs_sha256'] = dict(expected['exefs_sha256'], main='0' * 64)
        with self.assertRaises(build_win.BuildError):
            self.step(expected)

    def test_modules_are_not_rewritten(self):
        """The JIT runs the original modules: no patch_nso.py run, no rewritten/ folder."""
        run = self.step(self.fake.expected())
        self.assertFalse((self.fake.tree / 'rewritten').exists())
        for call in run.call_args_list:
            self.assertNotIn('patch_nso.py', ' '.join(map(str, call.args[0])))


class RuntimeIndex(unittest.TestCase):
    def test_format_is_upstreams(self):
        with tempfile.TemporaryDirectory() as t:
            romfs = Path(t)
            files = [{'path': 'A/b.txt', 'offset': 0x200, 'size': 5}, {'path': 'Z.bin', 'offset': 0x210, 'size': 0}]
            (romfs / 'index.json').write_text(json.dumps({'files': files, 'romfs_size': 4096, 'romfs_sha256': 'ab' * 32}))
            build_win.runtime_index(romfs)
            data = (romfs / 'runtime-index.bin').read_bytes()
            self.assertEqual(data[:8], b'TOTKRFS1')
            import struct
            count, strings = struct.unpack_from('<QQ', data, 8)
            self.assertEqual((count, strings), (2, 24 + 2 * 24))
            self.assertEqual(struct.unpack_from('<QQII', data, 24), (0x200, 5, 0, 8))
            self.assertEqual(struct.unpack_from('<QQII', data, 48), (0x210, 0, 8, 6))
            self.assertEqual(data[strings:], b'A/b.txt\0Z.bin\0')
            config = dict(line.split() for line in (romfs / 'romfs-config.txt').read_text().splitlines())
            self.assertEqual(config, {'image_size': '4096', 'image_sha256': 'ab' * 32, 'index_sha256': sha(data)})


class Work(unittest.TestCase):
    def test_work_copy_skips_mac_only_and_generated_folders(self):
        with tempfile.TemporaryDirectory() as t, mock.patch.object(build_win, 'run'):
            tree, py = build_win.prepare_work(Path(t), Path(t) / 'logs')
            self.assertTrue((tree / 'scripts/extract_exefs.py').is_file())
            self.assertTrue((tree / 'third_party/d2r/patch_nso.py').is_file())
            for skipped in ('ios', 'prebuilt', 'app', 'extracted'):
                self.assertFalse((tree / skipped).exists(), skipped)
            self.assertTrue(str(py).endswith(('python.exe', 'python')))


class Args(unittest.TestCase):
    def test_game_and_keys_required(self):
        with self.assertRaises(SystemExit), mock.patch('sys.stderr'):
            build_win.parse_args(['game.xci'])

    def test_check_needs_no_game(self):
        a = build_win.parse_args(['--check', 'TOTK_win'])
        self.assertEqual(a.check, Path('TOTK_win'))


if __name__ == '__main__':
    unittest.main()
