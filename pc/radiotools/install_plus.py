#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Install a DDM25 Plus release on a Baofeng DM-32UV, or put the radio back from its backup.

DDM25 Plus uses the Classic flash layout: the application from 0xC000 up to the reserved codec slot at 0xE8000, and
the radio's own DMR (AMBE) codec in the slot 0xE8000..0x100000. The codec is never shipped: it comes from the radio.

  over Classic / DDM25 Plus:  --backup <backup of the radio now> --plus <release folder> --out <new dir>
      only application sectors below the slot are written; boot area (< 0xC000), codec slot and the whole data chip
      must read back byte for byte as before, or the restart stays disabled.
  first install from stock:   ... --plus <release folder> --from-stock
      the radio's own codec is taken from THIS backup and prepared in the slot (prepare_unit_image.compose_from_stock,
      the pinned RadioFoundry codec writer, as the lab's prepare_unit_image does for its unit images); application
      and slot sectors are written, the boot area and the data chip must read back unchanged, and the slot must
      read back exactly as prepared.
  restore:                    --backup <backup> --restore --out <new dir>
      every application and slot sector back from a Classic-layout backup (a stock backup: install_firstboot.py
      --restore-stock --use-saved-backup).

Gate: the release folder must hold the release's signed manifest (ddm25-manifest.json + .sig) and the application's
SHA-256 must be listed in it (radiotools/signed_manifest.py). The lab's PLUS_REVIEWED list is not used here.

Same pinned RAM monitor, bounded write plan (install_firstboot.InstallPort) and full readback as the lab tools. Needs
a verified two-pass backup of the radio as it is now (dm32_readonly_backup.py); before any write the live program
flash must equal the backup and every data chip sector CRC must equal the backup's. A failed write is never retried.
Every run is one user-approved step. No RF command is ever sent.
"""
import argparse
import json
from pathlib import Path
import re
import sys
import time
import zlib
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from radiotools import dm32_readonly_backup as backup  # noqa: E402
from radiotools import install_firstboot as fb  # noqa: E402
from radiotools import prepare_unit_image as unit  # noqa: E402
from radiotools import signed_manifest  # noqa: E402
from radiotools.prepare_unit_image import saved_images, backup_jedec  # noqa: E402

START, SIZE, SECTOR = fb.START, fb.SIZE, fb.SECTOR
SLOT = 0xE8000
SLOT_MAGIC, SLOT_V2 = 0x43524444, 2
# The Classic/Plus linker layout of the codec (Classic tools/prepare_web_firmware.py): a different one would run the
# slot's code at the wrong place
CODEC_SYMBOLS = {'__codec_slot': 0x030e8000, '__codec_slot_end': 0x03100000, '__codec_run_lo': 0x43a00,
                 '__codec_run_hi': 0x58000, '__codec_tab_lo': 0x18000e48, '__codec_tab_hi': 0x18003e38}
MODES = ('install_plus', 'install_plus_from_stock', 'restore')


def word(b, off):
    return int.from_bytes(b[off:off + 4], 'little')


def slot_check(program):
    """The prepared codec slot in a program image: header, version and its own CRC. Returns its stock tag"""
    s = program[SLOT:SIZE]
    if word(s, 0) != SLOT_MAGIC or word(s, 4) != SLOT_V2:
        raise ValueError('No prepared codec slot (v2) at 0xE8000: not the Classic layout (stock firmware: first install with --from-stock)')
    # codec_slot_t (platform/hrc7000/codec_slot.h): magic, version, code_off, code_len, code_ram, tab_off, tab_len,
    # tab_ram, 7 entry offsets, stock_tag @0x3C, crc @0x40, rel_off @0x44, rel_count @0x48, reserved[13]
    code_off, code_len, tab_off, tab_len = word(s, 8), word(s, 12), word(s, 20), word(s, 24)
    rel_off, rel_count, crc = word(s, 0x44), word(s, 0x48), word(s, 0x40)
    for off, n in ((code_off, code_len), (tab_off, tab_len), (rel_off, 4 * rel_count)):
        if off + n > len(s):
            raise ValueError('Codec slot parts run past the slot')
    got = zlib.crc32(s[rel_off:rel_off + 4 * rel_count],
                     zlib.crc32(s[tab_off:tab_off + tab_len], zlib.crc32(s[code_off:code_off + code_len])))
    if got != crc:
        raise ValueError('Codec slot CRC mismatch')
    return word(s, 0x3C)


def plus_application(folder):
    """The release's application after checking its build manifest, map and the signed release manifest"""
    folder = Path(folder)
    image = folder / 'rfoundry_dm32.bin'
    app = image.read_bytes()
    manifest = json.loads(image.with_suffix('.json').read_text())
    if not manifest.get('plus_enabled') or not manifest.get('forced_rebuild_identical'):
        raise ValueError('Expected a reproducible DDM25 Plus build manifest')
    outputs = {Path(k).name: v for k, v in manifest.get('outputs', {}).items()}
    if outputs.get('rfoundry_dm32.bin') != backup.sha(app):
        raise ValueError('Application differs from its build manifest')
    mapping = image.with_suffix('.map').read_bytes()
    if outputs.get('rfoundry_dm32.map') != backup.sha(mapping):
        raise ValueError('Linker map differs from its build manifest')
    symbols = {n: int(v, 16) for v, n in re.findall(r'(0x[0-9a-fA-F]+)\s+(__codec_\w+)\s*=', mapping.decode())}
    if any(symbols.get(n) != v for n, v in CODEC_SYMBOLS.items()):
        raise ValueError('Linker layout differs from the reserved codec location')
    if not app or len(app) > SLOT - START:
        raise ValueError('The application must end before the codec slot')
    # The public gate (the lab's PLUS_REVIEWED): listed in a manifest signed with the embedded release key
    signed_manifest.release_gate(app, folder)
    return app


def install_inputs(backup_folder, plus_folder):
    images = saved_images(backup_folder)
    original = images['progmem-pass1.bin']
    tag = slot_check(original)
    app = plus_application(plus_folder)
    target = original[:START] + app + b'\xff' * (SLOT - START - len(app)) + original[SLOT:]
    return original, target, images['q128-pass1.bin'], tag


def stock_inputs(backup_folder, plus_folder):
    """First install over stock: the radio's own codec from this backup, prepared in the slot"""
    images = saved_images(backup_folder)
    original = images['progmem-pass1.bin']
    app = plus_application(plus_folder)
    target, prof = unit.compose_from_stock(original, app)
    if slot_check(target) != prof['tag'] or target[:START] != original[:START]:
        raise ValueError('Prepared codec slot did not validate')
    return original, target, images['q128-pass1.bin'], prof


def restore_inputs(backup_folder):
    """Every application and slot sector rewritten from the backup (a synthetic inverted basis), boot untouched"""
    images = saved_images(backup_folder)
    target = images['progmem-pass1.bin']
    slot_check(target)
    basis = target[:START] + bytes(b ^ 0xff for b in target[START:])
    return basis, target, images['q128-pass1.bin']


def make_plan(mode, original, target, tag, prof=None):
    """The write plan printed before anything else (and alone with --plan-only)"""
    todo = [a for a in range(START, SIZE, SECTOR) if original[a:a + SECTOR] != target[a:a + SECTOR]]
    if mode == 'install_plus' and any(a >= SLOT for a in todo):
        raise SystemExit('Refusing: the plan would touch the codec slot')
    if any(a < START for a in todo) or original[:START] != target[:START]:
        raise SystemExit('Refusing: the plan would touch the boot area')
    plan = {'mode': mode, 'changed_sectors': len(todo),
            'first': hex(todo[0]) if todo else None, 'last': hex(todo[-1]) if todo else None,
            'codec_slot_stock_tag': tag, 'target_application_sha256': backup.sha(target[START:]),
            'boot_area_unchanged': original[:START] == target[:START],
            'codec_slot_unchanged': original[SLOT:] == target[SLOT:],
            'codec_slot_rewritten_from_backup': mode == 'restore',
            'codec_slot_prepared_from_backup': mode == 'install_plus_from_stock',
            'data_chip_written': False}
    if prof is not None:
        plan.update(stock_profile=prof['name'], stock_version=unit.stock_version(prof),
                    stock_version_tested=prof['name'] in unit.TESTED_STOCK,
                    slot_sectors=len([a for a in todo if a >= SLOT]))
    return plan


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True)
    ap.add_argument('--backup', type=Path, required=True, help='verified two-pass backup of the radio as it is now')
    ap.add_argument('--plus', type=Path, help='release folder: rfoundry_dm32.bin/.json/.map and the signed manifest')
    ap.add_argument('--from-stock', action='store_true',
                    help='first install over stock firmware: prepare the radio\'s own codec in the slot')
    ap.add_argument('--restore', action='store_true', help='put application and codec slot back from the backup')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--wait', type=int, default=600)
    ap.add_argument('--plan-only', action='store_true', help='print the write plan; no serial port is opened')
    args = ap.parse_args()
    if args.restore == bool(args.plus):
        ap.error('Choose exactly one of --plus or --restore')
    if args.from_stock and not args.plus:
        ap.error('--from-stock needs --plus')
    if not 1 <= args.wait <= 600:
        ap.error('Wait must be 1..600 seconds')
    prof = None
    if args.restore:
        mode = 'restore'
        original, target, data = restore_inputs(args.backup)
        tag = None
    elif args.from_stock:
        mode = 'install_plus_from_stock'
        original, target, data, prof = stock_inputs(args.backup, args.plus)
        tag = prof['tag']
    else:
        mode = 'install_plus'
        original, target, data, tag = install_inputs(args.backup, args.plus)
    expected_chips = backup_jedec(args.backup)
    plan = make_plan(mode, original, target, tag, prof)
    print(json.dumps(plan, indent=2), flush=True)
    if args.plan_only:
        return
    module = backup.load_upstream()
    expected_data = fb.crc_list(data)
    args.out.mkdir(parents=True, exist_ok=False, mode=0o700)
    report = dict(plan, started_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), writes_started=False,
                  installation_verified=False, bootloader_unchanged=False, codec_slot_verified=False,
                  data_sector_crcs_unchanged=False, restart_sent=False)

    def receipt():
        report['outgoing_operations'] = guard.counts
        report['planned_write_frames'] = len(guard.plan)
        report['sent_write_frames'] = guard.index
        (args.out / 'install-receipt.json').write_text(json.dumps(report, indent=2) + '\n')
    import serial
    raw = serial.Serial(port=None, baudrate=115200, timeout=0.02, write_timeout=3)
    raw.rts = raw.dtr = False
    raw.port = args.port
    guard = fb.InstallPort(raw, module, original, target, skip_original=args.restore)
    try:
        raw.open()
        radio = module.Radio(guard, log=lambda s: print(s, flush=True), retries=3)
        print(f'READY on {args.port}: turn the radio OFF, then ON normally, no buttons held. Waiting {args.wait}s.',
              flush=True)
        radio.start(wait=args.wait)
        radio.configure('dm32')
        if not radio.v6 or radio.v6['board'] != 1 or radio.v6['flags'] != 0 or not radio.configured:
            raise ValueError('Monitor configuration/identity did not validate')
        radio.check_writable()
        fb.check_chips(radio, guard, expected_chips)
        if args.restore:
            # As the proven stock restore: no pre-reads, every application and slot sector from the backup
            guard.authorize_saved_backup()
            report['writes_started'] = True
            receipt()
            print(f'RESTORING: {len(guard.todo)} sectors from the backup. Boot and data writes blocked.', flush=True)
            fb.write_plan(radio, guard, None)
            after = fb.read_program(radio, args.out / 'application-after.bin', 'Verify restored application',
                                    start=START)
            guard.authorize_reset_application(after)
            report.update(installation_verified=True, codec_slot_verified=after[SLOT - START:] == target[SLOT:],
                          installed_application_sha256=backup.sha(after))
            receipt()
            print('VERIFIED: application and codec slot read back as in the backup. Restarting.', flush=True)
            radio.retries = 0
            radio.reset_chip()
            report['restart_sent'] = True
            receipt()
            print('PASS: restored. Check the boot screen on the radio.', flush=True)
            return
        # The radio must be exactly as backed up
        before = fb.read_program(radio, args.out / 'program-before.bin', 'Read current firmware')
        before_data = fb.device_crcs(radio, 'q128', len(data), 'Check settings chip')
        report['precheck_program_matches_backup'] = before == original
        report['precheck_changed_data_sectors'] = [i * SECTOR for i, (a, b) in
                                                    enumerate(zip(before_data, expected_data)) if a != b]
        receipt()
        guard.authorize(before, before_data, expected_data)
        report['writes_started'] = True
        receipt()
        where = 'application and codec slot sectors' if prof else 'application sectors below the codec slot'
        print(f'INSTALLING: {len(guard.todo)} {where}. Boot and data writes blocked.', flush=True)
        last = [-1]

        def progress(n, total):
            p = n * 100 // total
            if p // 5 != last[0]:
                print(f'Written and sector-readback checked: {p}% ({n}/{total})', flush=True)
                last[0] = p // 5
                receipt()
        started = time.monotonic()
        fb.write_plan(radio, guard, progress)
        report['write_seconds'] = time.monotonic() - started
        after = fb.read_program(radio, args.out / 'program-after.bin', 'Read complete installed program chip')
        after_data = fb.device_crcs(radio, 'q128', len(data), 'Recheck untouched settings chip')
        if prof is None:
            if after[SLOT:] != original[SLOT:]:
                raise ValueError('Codec slot changed; restart disabled')
        elif after[SLOT:] != target[SLOT:] or slot_check(after) != tag:
            raise ValueError('Prepared codec slot did not read back; restart disabled')
        guard.authorize_reset(after, after_data, expected_data)
        report.update(installation_verified=True, bootloader_unchanged=True, codec_slot_verified=True,
                      data_sector_crcs_unchanged=True, installed_program_sha256=backup.sha(after))
        receipt()
        print('VERIFIED: application, untouched boot area, codec slot and settings chip all read back. Restarting.',
              flush=True)
        radio.retries = 0
        radio.reset_chip()
        report['restart_sent'] = True
        receipt()
        print('PASS: DDM25 Plus installed and verified. Check the boot screen on the radio. No RF command was sent.',
              flush=True)
    except BaseException as exc:
        report['error'] = f'{type(exc).__name__}: {exc}'
        raise
    finally:
        report['finished_utc'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
        raw.close()
        receipt()


if __name__ == '__main__':
    main()
