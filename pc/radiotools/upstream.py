# SPDX-License-Identifier: GPL-3.0-or-later
"""The pinned upstream RadioFoundry files the radio tools load, and where they are.

RadioFoundry (MIT, https://github.com/RadioFoundry/RadioFoundry) at commit PIN provides the RAM monitor host
(stub/stubtool.py, which carries the RAM monitor image), the debug-link protocol (tools/rfoundry_debug.py) and the
codec slot writer with the stock codec profiles (tools/hrc7000_codec.py, tools/rfoundry/rfoundry/*). Every file is
checked against its SHA-256 below before it is loaded; a different file is never imported.

Where the tree is looked for, first hit wins:
  1. DDM25_UPSTREAM (a RadioFoundry checkout at PIN; hashes still apply)
  2. <root>/research/upstream/radiofoundry, root = the folder above radiotools/ (pc/ from source, the program folder
     of the frozen app, where pc/bundle.py copies exactly the files below)
  3. <repository>/research/upstream/radiofoundry (the DDM25 Plus firmware build clones it there:
     tools/build_radiofoundry.py, or python pc/fetch_upstream.py)
No stock firmware and no codec byte is part of these files.
"""
import hashlib
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PIN = 'dfbaf20c400c73e2ccf6edb80af7d1d255bee418'
URL = 'https://github.com/RadioFoundry/RadioFoundry'

STUBTOOL = 'stub/stubtool.py'
DEBUG = 'tools/rfoundry_debug.py'
CODEC = 'tools/hrc7000_codec.py'
RFOUNDRY = 'tools/rfoundry/rfoundry'

# {path inside the RadioFoundry tree: SHA-256}
PINNED = {
    STUBTOOL: '4e3ad1fa0c85d89e9f4bc15c5d6f97212bbab341ac72d958bdf7d0e250124b9e',
    DEBUG: 'b2e572cf6119d1bf1d287a126b05df9233a5dfb4e3ab7e2d8685e3732b92075c',
    CODEC: '409ec22790e9eae7e8053076417786478472b24d182410d41f3ce3a6f7782f39',
    f'{RFOUNDRY}/__init__.py': 'a3f3ea7c58e17439ed0f45c1ed95d734b204e15ee17e7f0ac128363b8ce8d8fc',
    f'{RFOUNDRY}/boards.py': '2f67ed171ff37225736417895106a864c94eaeb6e93fcbbde2debfacd2508c89',
    f'{RFOUNDRY}/codecmap.py': '0d90296f58b7f733ae1ed617fca54f5e3357ec7541c53874c15a092b6dc1df54',
    f'{RFOUNDRY}/names.py': '4e71e6531f70731ae5cd70ed3e3731108f85d1b08c9549d2998d81d3a51068d8',
    f'{RFOUNDRY}/protect.py': '213a828290f19695c24391178b1bb23e4a36d790d208cc47fe71eba6573f148e',
    f'{RFOUNDRY}/stockfw.py': 'd797e1713505f9dc7623941ff24443333f6ab55d08e51b8f2eb2626911e2995a',
    f'{RFOUNDRY}/registry.json': 'b1c649086ae9c67809ef9581757ecd11c7ee90ad3c49351753e10db5f2a6fb6b',
    f'{RFOUNDRY}/codec_maps/dm32/dm32-032.json': '207f4a2ff3b439200645d0b0138c7dc2d74018bcef1122844a02e9d7cc113299',
    f'{RFOUNDRY}/codec_maps/dm32/dm32-037.json': 'e0b302ab53e4393507ef7c4f722b43262d6e43cc7cec877b0e577f3e3e720e3d',
    f'{RFOUNDRY}/codec_maps/dm32/dm32-046.json': 'aee52c05fa17f3b4d121c28d569fce056403228fa7801363659510d85b2b821a',
    f'{RFOUNDRY}/codec_maps/dm32/dm32-046hr.json': 'bdf36db39582a03a36a0ecfe34e0c93d1c5431fee98c9647d78e7625e5300e63',
    f'{RFOUNDRY}/codec_maps/dm32/dm32-049.json': '77a6593cb8d04bcb5c7bb5eec45e5fe6b2427bc1519795e3ef81ab5e1a1995e5',
    f'{RFOUNDRY}/codec_maps/dm32/dm32-l01-048.json': 'b1e0d411e97cb34faa60309b6af47ba1de6ac1a09cb2cfd23e8398774353cf02',
    f'{RFOUNDRY}/codec_maps/dm32/dm32-nrf-049.json': '4364a34e3799495de6acb7e36ffb4cf4c06c5be10bfe8dc12f7dcc49b8668c2e',
    'LICENSE': '9988da23b503eebd9b627eca5bea80742257eec657c2a6916a705a301de42f67',
}
# What the codec slot writer reads (checked together before tools/hrc7000_codec.py is loaded)
CODEC_INPUTS = tuple(k for k in PINNED if k == CODEC or k.startswith(RFOUNDRY))


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def candidates():
    out = []
    env = os.environ.get('DDM25_UPSTREAM')
    if env:
        out.append(Path(env))
    out.append(ROOT / 'research' / 'upstream' / 'radiofoundry')
    out.append(ROOT.parent / 'research' / 'upstream' / 'radiofoundry')
    return out


def root():
    """The first candidate that holds the RAM monitor host; else the first candidate (errors then name it)"""
    found = candidates()
    for c in found:
        if (c / STUBTOOL).is_file():
            return c
    return found[0]


def available():
    return (root() / STUBTOOL).is_file()


def path(rel):
    """The pinned file rel, after its SHA-256 check"""
    if rel not in PINNED:
        raise ValueError(f'{rel} is not a pinned upstream file')
    p = root() / rel
    try:
        data = p.read_bytes()
    except OSError:
        raise FileNotFoundError(f'Pinned upstream file missing: {p} (RadioFoundry {PIN[:8]}, see pc/README.en.md)')
    if sha256(data) != PINNED[rel]:
        raise ValueError(f'Pinned upstream file differs from its SHA-256: {rel}; preserving it and refusing import')
    return p


def verify(rels=None):
    """Checks every pinned file (or the given ones); returns the tree's root"""
    for rel in (PINNED if rels is None else rels):
        path(rel)
    return root()
