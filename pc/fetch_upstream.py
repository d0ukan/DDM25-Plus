#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch the pinned RadioFoundry tree the radio tools load (research/upstream/radiofoundry of the repository, the
same place tools/build_radiofoundry.py clones it) and check every pinned file's SHA-256 (radiotools/upstream.py).

  python pc/fetch_upstream.py          clone at the pin when missing, then verify
"""
from pathlib import Path
import subprocess
import sys

PC = Path(__file__).resolve().parent
sys.path.insert(0, str(PC))
from radiotools import upstream  # noqa: E402

DEST = PC.parent / 'research' / 'upstream' / 'radiofoundry'


def main():
    if not DEST.exists():
        DEST.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['git', 'clone', '--no-checkout', upstream.URL, str(DEST)], check=True)
        subprocess.run(['git', '-C', str(DEST), 'checkout', '--detach', upstream.PIN], check=True)
    head = subprocess.check_output(['git', '-C', str(DEST), 'rev-parse', 'HEAD'], text=True).strip()
    if head != upstream.PIN:
        raise SystemExit(f'{DEST} is at {head[:8]}, not the pin {upstream.PIN[:8]}; refusing to change it')
    root = upstream.verify()
    print(f'RadioFoundry {upstream.PIN[:8]}: {len(upstream.PINNED)} pinned files verified in {root}')


if __name__ == '__main__':
    main()
