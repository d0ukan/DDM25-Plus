#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline helpers on the user's own verified backup: the saved images, and the radio's own codec prepared for the
reserved codec slot. Never opens a port and never writes a file.

- saved_images(folder): both passes of both flash chips from a verified two-pass backup (dm32_readonly_backup.py),
  checked against the backup's manifest: complete capture set, lengths, SHA-256 per file and per 4 KiB sector,
  pass 1 == pass 2, 1 MiB program flash and 16 MiB data flash.
- backup_jedec(folder): the flash chip IDs the backup tool recorded for this radio.
- load_codec(): the pinned RadioFoundry codec slot writer (tools/hrc7000_codec.py) and the files it reads (board
  registry, codec maps), loaded only after each matched its SHA-256 (radiotools/upstream.py), and only when the
  board's codec windows equal the Classic / DDM25 Plus linker layout.
- stock_profile(codec, program): the stock firmware a program flash image holds (found by the codec's content, as
  RadioFoundry identifies a program flash dump), or None.
- compose_from_stock(program, application): the full 1 MiB target image for a first install from stock: the
  untouched boot area, the release application and the radio's OWN codec in the slot at 0xE8000, the same merge the
  lab's tools/prepare_unit_image.py does for its unit images (codec.extract + codec.merge), checked the same way: the
  slot parses back with the stock tag, code and table hashes.

The composed image contains the radio's own proprietary codec bytes. It is only ever handed to the installer in
memory; it is never saved, shipped or uploaded. No stock firmware file is used or needed.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from radiotools import upstream  # noqa: E402

START, SIZE, SECTOR = 0xC000, 0x100000, 4096
SLOT = 0xE8000
PROGMEM_BASE = 0x03000000
# The Classic / DDM25 Plus codec windows (install_plus.CODEC_SYMBOLS): slot, the codec's run window, the modem table
# window the firmware accepts
SLOT_WINDOW = (0x030E8000, 0x03100000)
RUN_WINDOW = (0x43A00, 0x58000)
TABLES_LO, TABLES_HI = 0x18000E48, 0x18003E38
APP_BASE = 0x0300C000
# Stock versions DDM25 Plus has been installed over (the author's radio). Every other known profile goes through the
# same code path but is untried.
TESTED_STOCK = {'DM32.01.01.047.bin'}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def saved_images(folder):
    folder = Path(folder)
    manifest = json.loads((folder / 'backup-manifest.json').read_text())
    names = {f'{m}-pass{n}.bin' for m in ('progmem', 'q128') for n in (1, 2)}
    if not manifest.get('backup_verified') or {r['file'] for r in manifest['captures']} != names:
        raise ValueError('A complete verified two-pass backup is required')
    images = {}
    for record in manifest['captures']:
        data = (folder / record['file']).read_bytes()
        if len(data) != record['bytes'] or sha(data) != record['sha256']:
            raise ValueError('Saved image length/hash differs from its manifest')
        sectors = [sha(data[i:i + 4096]) for i in range(0, len(data), 4096)]
        if sectors != record['sector_sha256']:
            raise ValueError('Saved sector hashes differ')
        images[record['file']] = data
    for m in ('progmem', 'q128'):
        if images[m + '-pass1.bin'] != images[m + '-pass2.bin']:
            raise ValueError('Saved passes differ')
    if len(images['progmem-pass1.bin']) != 0x100000 or len(images['q128-pass1.bin']) != 0x1000000:
        raise ValueError('Candidate board preset requires 1 MiB program and 16 MiB data flash')
    return images


def backup_jedec(folder):
    """{'progmem': '<6 hex>', 'q128': '<6 hex>'}: the JEDEC IDs the backup recorded for this radio. The installers
    compare the attached chips with these before any read or write"""
    manifest = json.loads((Path(folder) / 'backup-manifest.json').read_text())
    chips = manifest.get('chips') or {}
    out = {}
    for memory in ('progmem', 'q128'):
        jedec = str((chips.get(memory) or {}).get('jedec', '')).lower()
        if len(jedec) != 6 or any(c not in '0123456789abcdef' for c in jedec):
            raise ValueError('The backup does not record its flash chip IDs; take a new backup with this program')
        out[memory] = jedec
    return out


class CodecError(ValueError):
    pass


def load_codec():
    """The pinned codec slot writer, after the hash check of everything it reads and a check of its DM32 windows"""
    root = upstream.verify(upstream.CODEC_INPUTS)
    codec = load('ddm25_unit_codec', root / upstream.CODEC)
    lay = codec.BOARDS.get('dm32')
    if (not lay or tuple(lay['slot']) != SLOT_WINDOW or tuple(lay['run']) != RUN_WINDOW or
            lay['app_base'] != APP_BASE or lay['tables'][0] != TABLES_LO or lay['tables'][1] > TABLES_HI):
        raise CodecError('The codec tool\'s DM32 windows differ from the DDM25 Plus layout')
    return codec


def has_slot(codec, program):
    """True when a valid version 2 codec slot is at 0xE8000 (the Classic / DDM25 Plus layout)"""
    return codec.parse_slot(program[SLOT:SIZE]) is not None


def stock_profile(codec, program):
    """The stock profile of a program flash image (stock layout: no slot, the codec where that stock build keeps it),
    or None"""
    if len(program) != SIZE or has_slot(codec, program):
        return None
    try:
        prof = codec.identify(program, board='dm32')
    except Exception:          # the tool's own errors: not a known stock image
        return None
    if not prof or prof.get('board') != 'dm32':
        return None
    code_at = prof['code'][0] - PROGMEM_BASE
    tables_at = prof['tables'][0] - PROGMEM_BASE
    if not (START <= code_at < SLOT and START <= tables_at < SLOT):
        return None                    # a codec copy outside the stock application area is not a stock layout
    return prof


def stock_version(prof):
    """'047' from 'DM32.01.01.047.bin'; the profile name otherwise"""
    name = prof.get('name', '')
    parts = name.replace('.bin', '').split('.')
    return parts[-1] if len(parts) >= 4 and parts[-1].isdigit() else name


def compose_from_stock(program, application, codec=None):
    """(target image, stock profile): boot area unchanged, application from 0xC000, 0xFF up to the slot, the radio's
    own codec slot, 0xFF to the end. Raises CodecError (a ValueError) for anything that is not a known stock layout"""
    codec = codec or load_codec()
    if len(program) != SIZE:
        raise CodecError('Expected a 1 MiB program flash image')
    if has_slot(codec, program):
        raise CodecError('A prepared codec slot is already present: this radio has the Classic layout, '
                         'install over it instead')
    prof = stock_profile(codec, program)
    if prof is None:
        raise CodecError('Unknown stock firmware: its codec was not found where a supported stock version keeps it')
    if not application or len(application) > SLOT - START:
        raise CodecError('The application must end before the codec slot')
    try:
        prof, code, tables = codec.extract(program, board='dm32')
        raw = codec.merge(application, prof, code, tables)
    except Exception as exc:        # codec.ExtractError and friends, as one refusal
        raise CodecError(f'Codec preparation failed: {exc}') from None
    if len(raw) > SIZE - START or raw[:len(application)] != application:
        raise CodecError('Merged image does not fit the application area')
    slot = codec.parse_slot(raw[SLOT - START:])
    if (not slot or slot['tag'] != prof['tag'] or slot['code_sha256'] != sha(code) or
            slot['tables_sha256'] != sha(tables)):
        raise CodecError('Codec slot did not validate')
    target = program[:START] + raw + b'\xff' * (SIZE - START - len(raw))
    if len(target) != SIZE or target[:START] != program[:START]:
        raise CodecError('Composed image does not keep the boot area')
    return target, prof
