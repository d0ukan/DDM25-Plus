#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read why the last P25 PTT call on DM32 Plus ended (read-only debug link).

Sends only these read-only queries, refused locally otherwise:
  0x20 [13]  live TX summary (mic frames, encoder, stop reason)
  0x21 [0]   native modem status
  0x21 [9]   last request seen by the native lab
  0x21 [12]  P25 zone scan counters (--scan)
  0x09 [20|21|23]  views tasks, crash, reset (--health)
No TX, flash, storage or register command is ever sent.
"""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from radiotools import native_modem as native  # noqa: E402
from radiotools import upstream  # noqa: E402

ALLOWED = {(0x20, b'\x0d'), (0x21, b'\x00'), (0x21, b'\x09'), (0x21, b'\x0c'),
           (0x09, b'\x14'), (0x09, b'\x15'), (0x09, b'\x17')}   # GET_VIEW tasks / crash / reset (read only)
VIEWS = (('tasks', 0x14), ('crash', 0x15), ('reset', 0x17))   # DL_VIEW_TASKS, _CRASH, _RESET (view version 5)
SCAN_FIELDS = 'version stops carriers cooled takes synced voiced ended tails quiet foreign sync_ms voice_ms'.split()
REQ_FIELDS = 'version last_req_type last_req_arg last_req_ms ptt_consumed cancellations cancel_type cancel_arg'.split()


def open_link(port, allowed=ALLOWED):
    """The debug link on port, refusing every request outside allowed (pairs of command and exact payload)"""
    if not port or '://' in port or port.startswith('udp:'):
        raise ValueError('Explicit local serial port required')
    debug = upstream.path(upstream.DEBUG)        # refuses a file that differs from its SHA-256 pin
    if hashlib.sha256(debug.read_bytes()).hexdigest() != upstream.PINNED[upstream.DEBUG]:
        raise ValueError('Pinned protocol module changed')
    spec = importlib.util.spec_from_file_location('txdiag_protocol', debug)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    import serial

    class DiagLink(module.DebugLink):
        def __init__(self):
            self.port = port; self.timeout = 1; self.seq = 0; self.packed_shots = None; self.buf = bytearray()
            self.s = serial.Serial(port=None, baudrate=115200, timeout=0.02, write_timeout=3)
            self.s.rts = self.s.dtr = False; self.s.port = port; self.s.open()

        def request(self, cmd, payload=b'', **kw):
            if (cmd, bytes(payload)) not in allowed:
                raise ValueError('plus_txdiag refused a command before transmission')
            return super().request(cmd, payload, **kw)
    return DiagLink()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port', required=True)
    ap.add_argument('--health', action='store_true',
                    help='read the task stacks, heap low-water mark and the last reset cause instead')
    ap.add_argument('--scan', action='store_true',
                    help='read the P25 zone scan counters instead (firmware/docs/plus-radio-test.md R3a-R3f)')
    args = ap.parse_args()
    link = open_link(args.port)
    if args.scan:
        try:
            code, data = link.request(0x21, b'\x0c', busy_wait=0)
            print('--- P25 zone scan counters (0x21/12) status', code)
            if not code and len(data) == 4 * len(SCAN_FIELDS):
                for k, v in zip(SCAN_FIELDS, struct.unpack('<%dI' % len(SCAN_FIELDS), data)):
                    print(f'{k}={v}')
            elif not code:
                print('raw', bytes(data).hex())
        finally:
            link.s.close()
        return
    if args.health:
        try:
            for name, section in VIEWS:
                code, data = link.request(0x09, bytes([section]), busy_wait=0)
                print(f'--- view {name} ({section}) status {code}')
                if not code and len(data) >= 5:
                    print(f'uptime_ms={struct.unpack("<I", bytes(data[:4]))[0]}')
                    print(bytes(data[5:]).decode('ascii', 'replace').rstrip())
        finally:
            link.s.close()
        return
    try:
        code, data = link.request(0x20, b'\x0d', busy_wait=0)
        print('--- live TX summary (0x20/13) status', code)
        if not code:
            text = bytes(data).decode('ascii', 'replace')
            for line in text.splitlines():
                key, _, val = line.partition('=')
                extra = ''
                if key == 'live.reason' and val.isdigit() and int(val) < len(native.REASONS):
                    extra = '  (' + native.REASONS[int(val)] + ')'
                print(line + extra)
        code, data = link.request(0x21, b'\x00', busy_wait=0)
        print('--- native status (0x21/0) status', code)
        if not code and len(data) == 4 * len(native.FIELDS):
            r = dict(zip(native.FIELDS, struct.unpack('<' + 'I' * len(native.FIELDS), data)))
            for k, v in r.items():
                print(f'{k}={v}')
            if r['state'] < len(native.STATES):
                print('state_name=' + native.STATES[r['state']])
            if r['reason'] < len(native.REASONS):
                print('reason_name=' + native.REASONS[r['reason']])
        elif not code:
            print('raw', bytes(data).hex())
        code, data = link.request(0x21, b'\x09', busy_wait=0)
        print('--- native request log (0x21/9) status', code)
        if not code and len(data) == 32:
            for k, v in zip(REQ_FIELDS, struct.unpack('<8I', data)):
                print(f'{k}={v}')
    finally:
        link.s.close()


if __name__ == '__main__':
    main()
