"""Tests for tools/upstream_patches.py: the patched work copies of upstream's extraction scripts, run on synthetic
NSPs (no game data or keys)."""
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'tools/tests'), str(ROOT / 'upstream/tests')]
import upstream_patches  # noqa: E402
import test_game_containers as fx  # noqa: E402
from test_nsp_info import plain_nca  # noqa: E402

SCRIPTS = ROOT / 'upstream/games/totk/scripts'


class Patched(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)
        self.tree = self.path / 'totk'
        shutil.copytree(SCRIPTS, self.tree / 'scripts', ignore=shutil.ignore_patterns('__pycache__'))
        self.keys = self.path / 'prod.keys'
        self.keys.write_text('\n'.join(f'{n} = {k.hex()}' for n, k in [
            ('header_key', fx.HEADER), ('key_area_key_application_00', fx.AREA), ('titlekek_00', fx.TITLEKEK)]))

    def extract(self, image, scripts=None):
        """Run extract_exefs.py then extract_romfs.py; returns (out folder, failing process or None)."""
        scripts = scripts or self.tree / 'scripts'
        game = self.path / 'game.nsp'
        game.write_bytes(image)
        out = self.path / 'extracted'
        shutil.rmtree(out, ignore_errors=True)
        steps = [[scripts / 'extract_exefs.py', game, '--keys', self.keys, '--output', out / 'exefs'],
                 [scripts / 'extract_romfs.py', '--keys', self.keys, '--manifest', out / 'extraction-manifest.json',
                  '--output', out / 'romfs']]
        for step in steps:
            p = subprocess.run([sys.executable, *step], capture_output=True, text=True)
            for secret in (fx.CONTENT, fx.TITLEKEK, fx.HEADER, fx.AREA):
                self.assertNotIn(secret.hex(), (p.stdout + p.stderr).lower())
            if p.returncode:
                return out, p
        return out, None

    def assert_extracted(self, image):
        out, failed = self.extract(image)
        self.assertIsNone(failed, failed and failed.stdout + failed.stderr)
        self.assertEqual((out / 'exefs/main').read_bytes(), b'synthetic main')
        self.assertEqual((out / 'romfs/romfs.bin').read_bytes(), fx.romfs())
        self.assertEqual(json.loads((out / 'romfs/index.json').read_text())['ivfc_blocks'], [1] * 6)

    def test_unpatched_upstream_rejects_unencrypted_sections(self):
        _, failed = self.extract(fx.partition([('program.nca', plain_nca(romfs=True))]), scripts=SCRIPTS)
        self.assertIn('Only AES-CTR ExeFS is supported', failed.stderr)

    def test_unencrypted_sections_extract(self):
        self.assertEqual(upstream_patches.apply(self.tree), ['unencrypted sections'])
        self.assert_extracted(fx.partition([('program.nca', plain_nca(romfs=True, logo=True))]))

    def test_rights_id_without_ticket_needs_no_key_when_unencrypted(self):
        upstream_patches.apply(self.tree)
        self.assert_extracted(fx.partition([('program.nca', plain_nca(romfs=True, orphan_rights=True))]))

    def test_encrypted_files_still_extract(self):
        upstream_patches.apply(self.tree)
        for managed in (False, True):
            with self.subTest(managed=managed):
                self.assert_extracted(fx.game_image('nsp', managed))

    def test_hash_checks_still_apply_to_unencrypted_sections(self):
        upstream_patches.apply(self.tree)
        image = bytearray(fx.partition([('program.nca', plain_nca(romfs=True))]))
        at = image.rfind(b'synthetic icon payload')
        image[at] ^= 1
        _, failed = self.extract(bytes(image))
        self.assertIn('IVFC level 5', failed.stderr)
        image = bytearray(fx.partition([('program.nca', plain_nca(romfs=True))]))
        at = image.find(b'synthetic main')
        image[at] ^= 1
        _, failed = self.extract(bytes(image))
        self.assertIn('ExeFS block hash mismatch', failed.stderr)

    def test_changed_upstream_text_stops_loudly(self):
        upstream_patches.apply(self.tree)
        with self.assertRaisesRegex(upstream_patches.PatchError, 'upstream changed'):
            upstream_patches.apply(self.tree)    # already patched: the expected text is gone


if __name__ == '__main__':
    unittest.main()
