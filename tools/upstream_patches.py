"""Small, exact text patches for the work copy of upstream's game scripts (upstream/ itself is never edited).

build_win.prepare_work copies upstream/games/totk into <work>/totk on every run, then calls apply(). Each patch
names a file, the exact text it expects and its replacement; if the text is not there exactly once (upstream
changed), the build stops instead of running half-patched scripts. Remove a patch once upstream has the change.

Patches:
  unencrypted sections: some NSP converters remove NCA section encryption (encryption type 1, "none") and write the
    sections in plain text. extract_exefs.py and extract_romfs.py accept only AES-CTR (type 3). With the patch they
    also read type 1 sections as they are, and derive the AES key only for an AES-CTR section (a converted NCA can
    keep a rights ID whose ticket was dropped along with the encryption). Every hash check is unchanged: they check
    the plain content, so a converted dump must still match the verified 1.0.0 hashes exactly.
"""
from pathlib import Path


class PatchError(Exception):
    pass


PATCHES = [
    ('unencrypted sections', 'scripts/extract_exefs.py', [
        ("        ctr_key = nca_ctr_key(hdr, keys, tickets)\n"
         "        for i in range(4):\n",
         "        for i in range(4):\n"),
        ("            require(fs[4] == 3, 'Only AES-CTR ExeFS is supported')\n",
         "            require(fs[4] in (1, 3), 'Only unencrypted or AES-CTR ExeFS is supported')\n"
         "            ctr_key = nca_ctr_key(hdr, keys, tickets) if fs[4] == 3 else None\n"),
        ("                return decrypt(ctr_key, modes.CTR(iv), data)[absolute - aligned:]\n",
         "                if ctr_key is None:\n"
         "                    return data[absolute - aligned:]\n"
         "                return decrypt(ctr_key, modes.CTR(iv), data)[absolute - aligned:]\n"),
    ]),
    ('unencrypted sections', 'scripts/extract_romfs.py', [
        ("        ctr_key=nca_ctr_key(hdr,keys,common_tickets(f,entries))\n",
         ""),
        ("            if fs[2:5]!=bytes([0,3,3]):continue\n",
         "            if fs[2:5] not in (bytes([0,3,3]),bytes([0,3,1])):continue\n"
         "            ctr_key=nca_ctr_key(hdr,keys,common_tickets(f,entries)) if fs[4]==3 else None\n"),
        ("                return decrypt(ctr_key,modes.CTR(iv),read_at(f,record_offset(record)+aligned,size+skip))[skip:]\n",
         "                data=read_at(f,record_offset(record)+aligned,size+skip)\n"
         "                return (data if ctr_key is None else decrypt(ctr_key,modes.CTR(iv),data))[skip:]\n"),
    ]),
]


def apply(tree):
    """Patch the work copy at `tree` (<work>/totk). Returns the names of the applied patches."""
    tree = Path(tree)
    applied = []
    for name, rel, edits in PATCHES:
        path = tree / rel
        text = path.read_text(encoding='utf-8')
        for old, new in edits:
            count = text.count(old)
            if count != 1:
                raise PatchError(f'patch "{name}" does not fit {rel}: expected text found {count} times '
                                 f'(upstream changed?): {old.strip().splitlines()[0]!r}')
            text = text.replace(old, new)
        path.write_text(text, encoding='utf-8', newline='\n')
        if name not in applied:
            applied.append(name)
    return applied
