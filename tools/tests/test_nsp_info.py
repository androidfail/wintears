"""Tests for tools/nsp_info.py with synthetic encrypted NSPs from upstream's container tests (no game data or keys)."""
import hashlib
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'upstream/tests')]
import nsp_info  # noqa: E402
import test_game_containers as fx  # noqa: E402
from cryptography.hazmat.primitives.ciphers import modes  # noqa: E402


def plain_pfs0(files):
    """(FS header, section data) of a hashed PFS0 section with encryption type 1 (none)."""
    pfs = fx.partition(files)
    fs = bytearray(512)
    fs[2:5] = bytes([1, 2, 1])
    hashes = hashlib.sha256(pfs).digest()
    fs[8:40] = hashlib.sha256(hashes).digest()
    struct.pack_into('<IIQQQQ', fs, 0x28, 4096, 2, 0, len(hashes), 512, len(pfs))
    return fs, hashes.ljust(512, b'\0') + pfs.ljust(512, b'\0')


def plain_nca(romfs=False, logo=False, orphan_rights=False):
    """A Program NCA whose sections are not encrypted (encryption type 1), as some NSP converters write: the code
    section, optionally a RomFS and a logo PFS0 (as in the real layout), and optionally a rights ID whose ticket is
    not in the file (the key must then never be needed)."""
    sections = [plain_pfs0([('main', b'synthetic main'), ('main.npdm', b'synthetic metadata')])]
    if romfs:
        fs, image = fx.ivfc(fx.romfs())
        fs[4] = 1
        sections.append((fs, image))
    if logo:
        sections.append(plain_pfs0([('NintendoLogo.png', b'synthetic logo')]))
    hdr = bytearray(0xc00)
    hdr[0x200:0x204] = b'NCA3'
    hdr[0x206] = 1
    struct.pack_into('<Q', hdr, 0x210, fx.TITLE)
    if orphan_rights:
        hdr[0x230:0x240] = fx.RIGHTS
    else:
        hdr[0x300:0x340] = fx.encrypt(fx.AREA, modes.ECB(), bytes(32) + fx.CONTENT + bytes(16))
    body = b''
    for i, (fs, data) in enumerate(sections):
        start = 0xc00 + len(body)
        struct.pack_into('<II', hdr, 0x240 + i * 16, start // 512, (start + len(data)) // 512)
        hdr[0x400 + i * 512:0x600 + i * 512] = fs
        hdr[0x280 + i * 32:0x2a0 + i * 32] = hashlib.sha256(fs).digest()
        body += data
    struct.pack_into('<Q', hdr, 0x208, len(hdr) + len(body))
    return b''.join(fx.encrypt(fx.HEADER, modes.XTS(i.to_bytes(16, 'big')), hdr[i * 512:(i + 1) * 512])
                    for i in range(6)) + body


class NspInfo(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)
        self.keys = self.path / 'prod.keys'
        self.keys.write_text('\n'.join(f'{n} = {k.hex()}' for n, k in [
            ('header_key', fx.HEADER), ('key_area_key_application_00', fx.AREA), ('titlekek_00', fx.TITLEKEK)]))

    def run_on(self, data):
        game = self.path / 'game.nsp'
        game.write_bytes(data)
        lines = []
        verdicts = nsp_info.describe(game, self.keys, out=lines.append)
        text = '\n'.join(lines)
        for secret in (fx.CONTENT, fx.TITLEKEK, fx.HEADER, fx.AREA):
            self.assertNotIn(secret.hex(), text.lower())
        return text, verdicts

    def test_standard_nsp(self):
        text, verdicts = self.run_on(fx.game_image('nsp'))
        self.assertIn('encryption 3 = AES-CTR', text)
        self.assertIn('plain PFS0 no, PFS0 after AES-CTR yes', text)
        self.assertIn('standard AES-CTR', verdicts[0])

    def test_ticket_nsp(self):
        text, verdicts = self.run_on(fx.game_image('nsp', managed=True))
        self.assertIn('needs a ticket', text)
        self.assertIn('common tickets: 1', text)
        self.assertIn('standard AES-CTR', verdicts[0])

    def test_unencrypted_code_section(self):
        text, verdicts = self.run_on(fx.partition([('program.nca', plain_nca())]))
        self.assertIn('encryption 1 = none', text)
        self.assertIn('plain PFS0 yes', text)
        self.assertIn('NOT encrypted', verdicts[0])

    def test_logo_section_gives_no_second_verdict(self):
        text, verdicts = self.run_on(fx.partition([('program.nca', plain_nca(romfs=True, logo=True))]))
        self.assertEqual(text.count('code data:'), 1)
        self.assertEqual(len(verdicts), 1)

    def test_other_title(self):
        _, verdicts = self.run_on(fx.game_image('nsp', title=0x0100000000010000))
        self.assertIn('not the TOTK base game', verdicts[0])


if __name__ == '__main__':
    unittest.main()
