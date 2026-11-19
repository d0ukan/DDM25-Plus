#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""What a DM-32UV runs, read from its verified backup only (no serial port, nothing written).

  --backup <folder>   a verified two-pass backup (dm32_readonly_backup.py)

Prints one JSON object:
  layout          'classic' (a valid codec slot v2 at 0xE8000: Classic, DDM25 Plus or another Classic-layout build),
                  'stock' (no slot; the radio's codec found where a known stock build keeps it), or 'unknown'
  firmware        'ddm25' | 'classic' | 'stock' | 'unknown' (from the layout and the application's own strings)
  codec_tag       the stock tag in the slot (classic layout) or of the stock profile
  stock_profile, stock_version, stock_tested   for a stock radio (profiles of the pinned RadioFoundry codec tool:
                  DM32 .032, .037, .039, .040, .046 (three builds), .047, L01.048, .049 (three builds); .047 is the
                  version DDM25 Plus has been installed over)
  install_mode    'over_classic' | 'from_stock' | null: how install_plus.py would install DDM25 Plus on this radio
  supported       whether this program can install DDM25 Plus on it
  reason          why not, when not supported
The app matches the installed DDM25 Plus version itself (application hash against the signed manifests).
"""
import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from radiotools import prepare_unit_image as unit  # noqa: E402

START, SIZE, SLOT = unit.START, unit.SIZE, unit.SLOT
MARKERS = {'ddm25': (b'DDM25 Plus', b'DDM25'), 'classic': (b'open25-classic', b'Open25', b'open25-')}


def application_end(program):
    """End of the application area's used part (trailing 0xFF up to the slot dropped)"""
    app = program[START:SLOT]
    return START + len(app.rstrip(b'\xff'))


def detect(program, codec=None):
    codec = codec or unit.load_codec()
    if len(program) != SIZE:
        raise ValueError('Expected a 1 MiB program flash image')
    app = program[START:SLOT]
    out = {'layout': 'unknown', 'firmware': 'unknown', 'codec_tag': None, 'stock_profile': None,
           'stock_version': None, 'stock_tested': False, 'install_mode': None, 'supported': False, 'reason': '',
           'application_end': hex(application_end(program))}
    slot = codec.parse_slot(program[SLOT:SIZE])
    if slot is not None:
        out.update(layout='classic', codec_tag=slot['tag'], install_mode='over_classic', supported=True)
        if any(m in app for m in MARKERS['ddm25']):
            out['firmware'] = 'ddm25'
        elif any(m in app for m in MARKERS['classic']):
            out['firmware'] = 'classic'
        else:
            out['firmware'] = 'unknown'
        return out
    prof = unit.stock_profile(codec, program)
    if prof is not None:
        out.update(layout='stock', firmware='stock', codec_tag=prof['tag'], stock_profile=prof['name'],
                   stock_version=unit.stock_version(prof), stock_tested=prof['name'] in unit.TESTED_STOCK,
                   install_mode='from_stock', supported=True)
        return out
    if any(m in app for m in MARKERS['ddm25'] + MARKERS['classic']):
        out.update(firmware='classic', reason='Classic-type firmware without a valid codec slot (damaged slot?)')
    else:
        out['reason'] = ('Unknown firmware: no codec slot and no known stock codec layout (for example stock .050 '
                         'or newer). Install Classic with the upstream Open25-DM32 Flasher first, then DDM25 Plus '
                         'over it.')
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--backup', type=Path, required=True)
    args = ap.parse_args()
    images = unit.saved_images(args.backup)
    print(json.dumps(detect(images['progmem-pass1.bin']), indent=2), flush=True)


if __name__ == '__main__':
    main()
