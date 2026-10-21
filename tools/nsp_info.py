#!/usr/bin/env python3
"""Describe what is inside a game file (XCI or NSP), for diagnosing a failed build. Prints no key values.

    python tools/nsp_info.py <game.xci|game.nsp> -k <prod.keys>

For each NCA: content type, title ID, key generation, whether it needs a ticket, and each section's type, hash type
and encryption type. For the code (ExeFS) section it also checks whether the data reads as plain PFS0 or only after
AES-CTR decryption, which tells an NSP with removed section encryption apart from a damaged one.
"""
import argparse
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SCRIPTS = REPO / 'upstream/games/totk/scripts'
if not (SCRIPTS / 'extract_exefs.py').is_file():
    sys.exit('error: the upstream folder is empty: run "git submodule update --init upstream" in the repository folder')
sys.path.insert(0, str(SCRIPTS))
from cryptography.hazmat.primitives.ciphers import modes  # noqa: E402
from extract_exefs import (container_entries, common_tickets, decrypt, load_keys, nca_ctr_key,  # noqa: E402
                           nca_inventory, read_at)

CONTENT = {0: 'Program', 1: 'Meta', 2: 'Control', 3: 'Manual', 4: 'Data', 5: 'PublicData'}
FS_TYPE = {0: 'RomFS', 1: 'PFS0'}
HASH_TYPE = {0: 'auto', 1: 'none', 2: 'HierarchicalSha256', 3: 'HierarchicalIntegrity (IVFC)'}
CRYPT = {0: 'auto', 1: 'none (not encrypted)', 2: 'AES-XTS', 3: 'AES-CTR', 4: 'AES-CTR-EX (update patch, BKTR)',
         5: 'AES-CTR (skipped layer hash)', 6: 'AES-CTR-EX (skipped layer hash)'}
BASE_TITLE = '0100F2C0115B6000'


def describe(game, keys_path, out=print):
    keys = load_keys(keys_path)
    verdicts = []
    with game.open('rb') as f:
        kind, entries = container_entries(f, game.stat().st_size)
        out(f'container: {kind.upper()}, {game.stat().st_size:,} bytes, {len(entries)} entries')
        for name, _, size in entries:
            out(f'  {name}  {size:,} bytes')
        tickets = common_tickets(f, entries)
        out(f'common tickets: {len(tickets)}')
        for record, hdr in nca_inventory(f, entries, keys):
            content = CONTENT.get(record['content_type'], f'type {record["content_type"]}')
            rights = hdr[0x230:0x240] != bytes(16)
            generation = max(hdr[0x206], hdr[0x220])
            out(f'NCA {record["name"]}: {content}, title {record["title_id"]}, key generation {generation}, '
                f'{"needs a ticket (rights ID set)" if rights else "key area (no ticket needed)"}')
            for i in range(4):
                start, end = struct.unpack_from('<II', hdr, 0x240 + 16 * i)
                if not start:
                    continue
                fs = hdr[0x400 + 512 * i:0x600 + 512 * i]
                crypt = CRYPT.get(fs[4], f'unknown ({fs[4]})')
                out(f'  section {i}: {FS_TYPE.get(fs[2], fs[2])}, hash {HASH_TYPE.get(fs[3], fs[3])}, '
                    f'encryption {fs[4]} = {crypt}, {(end - start) * 512:,} bytes')
                if record['content_type'] != 0 or fs[2:4] != b'\x01\x02':
                    continue
                # the code section: does its PFS0 read as plain data, or after AES-CTR decryption?
                pfsoff = struct.unpack_from('<Q', fs, 0x40)[0]
                absolute = start * 512 + pfsoff
                aligned = absolute & ~15
                raw = read_at(f, record['nca_offset'] + aligned, 32)[absolute - aligned:absolute - aligned + 4]
                plain = raw == b'PFS0'
                try:
                    key = nca_ctr_key(hdr, keys, tickets)
                    iv = fs[0x140:0x148][::-1] + (aligned // 16).to_bytes(8, 'big')
                    dec = decrypt(key, modes.CTR(iv), read_at(f, record['nca_offset'] + aligned, 32))
                    ctr = dec[absolute - aligned:absolute - aligned + 4] == b'PFS0'
                    key_note = ''
                except ValueError as e:
                    ctr, key_note = False, f' (no AES-CTR check: {e})'
                out(f'    code data: plain PFS0 {"yes" if plain else "no"}, PFS0 after AES-CTR '
                    f'{"yes" if ctr else "no"}{key_note}')
                if record['title_id'] != BASE_TITLE:
                    verdicts.append(f'the Program NCA belongs to title {record["title_id"]}, not the TOTK base game '
                                    f'{BASE_TITLE}')
                elif fs[4] == 3 and ctr:
                    verdicts.append('the code section is standard AES-CTR: extraction should work')
                elif fs[4] == 1 and plain:
                    verdicts.append('the code section is NOT encrypted (type 1): this NSP was converted by a tool that '
                                    'removed section encryption. The extraction scripts need support for unencrypted '
                                    'sections (see docs/HANDOFF.md)')
                elif fs[4] == 4:
                    verdicts.append('the code section is an update patch (BKTR): this is an update file, not the '
                                    'base game')
                else:
                    verdicts.append(f'the code section has encryption type {fs[4]} ({crypt}) and plain PFS0 '
                                    f'{"yes" if plain else "no"}, AES-CTR PFS0 {"yes" if ctr else "no"}: unexpected')
    if not verdicts:
        verdicts.append('no Program NCA with a code section was found: this is not a game file')
    for v in verdicts:
        out(f'verdict: {v}')
    return verdicts


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    p.add_argument('game', type=Path)
    p.add_argument('-k', '--keys', type=Path, required=True)
    a = p.parse_args(argv)
    try:
        describe(a.game, a.keys)
    except (ValueError, OSError) as e:
        print(f'error: {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
