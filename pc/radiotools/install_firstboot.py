#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DM32 program-flash writer guard, bounded writes and full readback; restore of a saved stock backup.

Uses the pinned RAM monitor already used for backups (dm32_readonly_backup.py). The InstallPort guard lets only the
changed application sectors (0xC000 and up) be erased/programmed, with their exact planned page bytes, in order and
once. Boot area and data flash are read-only. A restart is permitted only after independent verification.
install_plus.py reuses this guard and these helpers for every DDM25 Plus install.

The command line of this public copy does one thing: --restore-stock --use-saved-backup puts every application sector
back from the user's own verified backup of the stock firmware (the restore proven on hardware in the lab: 244
sectors, byte-exact readback, normal stock boot). The lab's first-boot install, warm-update and restart entries are not
part of the public program.

Every run is one user-approved step. No RF command is ever sent.
"""
import argparse
import json
import os
from pathlib import Path
import sys
import time
import zlib
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from radiotools import dm32_readonly_backup as backup
from radiotools.prepare_unit_image import saved_images, backup_jedec

START=0xc000
SIZE=0x100000
SECTOR=4096

def packet(op,payload=b''):
    return bytes([2,op])+len(payload).to_bytes(2,'big')+bytes([sum(payload)&255,3])+payload
def spi_packet(tx,rx=0):
    return packet(5,bytes.fromhex('040000000100')+len(tx).to_bytes(2,'big')+rx.to_bytes(2,'big')+tx)

class InstallPort(backup.ReadOnlyPort):
    def __init__(self,raw,module,original,target,skip_original=False,bulk=False):
        super().__init__(raw,module)
        if len(original)!=SIZE or len(target)!=SIZE or original[:START]!=target[:START]:
            raise ValueError('Exact 1 MiB images with identical protected boot area required')
        self.original,self.target=bytes(original),bytes(target)
        self.todo=[a for a in range(START,SIZE,SECTOR) if original[a:a+SECTOR]!=target[a:a+SECTOR]]
        self.plan=[];self.index=0;self.enabled=False;self.closed_writes=False
        self.reset_plan=[];self.reset_index=0
        self.skip_original=skip_original;self.sector_ends={};self.bulk=bulk
        for a in self.todo:
            if bulk:
                sector=target[a:a+SECTOR]
                body=bytes.fromhex('040000000100')+a.to_bytes(4,'big')
                self.plan.append(packet(14,body+(sector if sector!=b'\xff'*SECTOR else b'')))
                self.sector_ends[a]=len(self.plan);continue
            self.plan += [spi_packet(b'\x06'),spi_packet(b'\x20'+a.to_bytes(3,'big'))]
            for q in range(a,a+SECTOR,256):
                page=target[q:q+256]
                if page!=b'\xff'*256:
                    self.plan += [spi_packet(b'\x06'),spi_packet(b'\x02'+q.to_bytes(3,'big')+page)]
            self.sector_ends[a]=len(self.plan)
    def authorize(self,program,data,expected_data):
        if self.enabled or self.closed_writes or program!=self.original or data!=expected_data:
            raise ValueError('Live pre-write comparison failed; writes remain disabled')
        self.enabled=True
    def authorize_saved_backup(self):
        if not self.skip_original or self.enabled or self.closed_writes:
            raise ValueError('Saved-backup-only installation was not selected')
        self.enabled=True
    def finish_writes(self):
        if not self.enabled or self.index!=len(self.plan):raise ValueError('Write plan incomplete')
        self.enabled=False;self.closed_writes=True
    def authorize_reset(self,program,data,expected_data):
        if not self.closed_writes or program!=self.target or data!=expected_data:
            raise ValueError('Post-write verification failed; reset remains disabled')
        self._allow_reset()
    def authorize_reset_application(self,application):
        if not self.skip_original or not self.closed_writes or application!=self.target[START:]:
            raise ValueError('Application readback failed; reset remains disabled')
        self._allow_reset()
    def _allow_reset(self):
        if self.reset_plan:raise ValueError('Restart was already authorized')
        m=self.m
        values=((m.CRASH_SOFT_MARK,m.CRASH_BOOT_SOFT),(m.WDT_LOCK,m.WDT_KEY),(m.WDT_EN,0),
                (m.WDT_LOCK,0),(m.QUAD_ENABLE,0),(m.LCSFC_BAUDR,4),(m.SYS_SOFT_RSTN,0))
        self.reset_plan=[packet(3,a.to_bytes(4,'big')+v.to_bytes(4,'little')) for a,v in values]
    def validate(self,data):
        if self.monitor:
            if self.skip_original:
                op=data[1] if len(data)>1 else -1
                payload=data[6:]
                if op in (7,9):
                    if (not self.closed_writes or len(payload)!=12 or self.memory(payload)!='progmem' or
                        int.from_bytes(payload[6:10],'big')<START):
                        raise ValueError('Original firmware/data reads are disabled in saved-backup mode')
                if op==5 and len(payload)>=14 and payload[10]==3:
                    address=int.from_bytes(payload[11:14],'big');n=int.from_bytes(payload[8:10],'big')
                    sectors=range(address//SECTOR*SECTOR,(address+n+SECTOR-1)//SECTOR*SECTOR,SECTOR)
                    if (self.memory(payload)!='progmem' or address<START or
                        not self.closed_writes and not (self.enabled and all(
                            a in self.sector_ends and self.index>=self.sector_ends[a] for a in sectors))):
                        raise ValueError('Only already-written application bytes may be read back')
            if data==spi_packet(b'\x05',1):return 'program_status_read'
            if self.enabled and self.index<len(self.plan) and data==self.plan[self.index]:
                self.index+=1
                if self.bulk:return 'program_sector_write'
                tx=data[16:]
                return {6:'program_write_enable',0x20:'program_sector_erase',2:'program_page'}[tx[0]]
            if self.reset_index<len(self.reset_plan) and data==self.reset_plan[self.reset_index]:
                self.reset_index+=1;return 'verified_restart_register'
        return super().validate(data)

def crc_list(data):return [zlib.crc32(data[i:i+SECTOR])&0xffffffff for i in range(0,len(data),SECTOR)]
def device_crcs(radio,memory,size,label):
    out=[];last=-1
    for a in range(0,size,16*SECTOR):
        n=min(16,(size-a)//SECTOR)
        out.extend(crc for crc,blank in radio.sums(memory,a,n))
        percent=(a+n*SECTOR)*100//size
        if percent//10!=last:
            print(f'{label}: {percent}%',flush=True);last=percent//10
    return out
def read_program(radio,path,label,start=0):
    if path.exists():raise FileExistsError(path)
    partial=path.with_suffix(path.suffix+'.partial')
    last=[-1]
    def progress(done,total):
        p=done*100//total
        if p//10!=last[0]:print(f'{label}: {p}%',flush=True);last[0]=p//10
    with partial.open('xb+') as out:
        radio.dump(out,'progmem',start,SIZE,progress=progress)
        out.flush();os.fsync(out.fileno())
    data=partial.read_bytes()
    if len(data)!=SIZE-start:raise ValueError('Incomplete program readback')
    os.link(partial,path);partial.unlink()
    return data
def write_plan(radio,guard,progress=None):
    # Force known generic SPI commands. No opaque sector-write operation or
    # automatic re-erase/reprogram retry is allowed after a serial error.
    radio.retries=0
    if guard.bulk and not radio.sector_op:raise ValueError('Pinned helper does not report sector-write support')
    for i,a in enumerate(guard.todo):
        if guard.bulk:
            sector=guard.target[a:a+SECTOR]
            crc=radio.write_sector('progmem',a,None if sector==b'\xff'*SECTOR else sector)
            if crc!=zlib.crc32(sector)&0xffffffff:raise ValueError(f'Sector CRC readback mismatch at {a:#x}; stopped without retry')
            if progress:progress(i+1,len(guard.todo))
            continue
        tx=b'\x20'+a.to_bytes(3,'big')
        radio._write_cmd('progmem',tx,f'erase {a:#x}',3)
        for q in range(a,a+SECTOR,256):
            page=guard.target[q:q+256]
            if page!=b'\xff'*256:
                radio._write_cmd('progmem',b'\x02'+q.to_bytes(3,'big')+page,f'program {q:#x}',1)
        got=b''.join(radio.read_flash('progmem',q,1024) for q in range(a,a+SECTOR,1024))
        if got!=guard.target[a:a+SECTOR]:raise ValueError(f'Sector readback mismatch at {a:#x}; stopped')
        if progress:progress(i+1,len(guard.todo))
    guard.finish_writes();radio.retries=3

def restore_inputs(folder):
    """Stock-restore plan from the verified backup, independent of current flash.

    The basis is synthetic (every application byte inverted) so every
    application sector is erased and rewritten from the backup, whatever a
    previous or interrupted install left behind. Boot area stays identical.
    """
    images=saved_images(folder)
    target=images['progmem-pass1.bin']
    basis=target[:START]+bytes(b^0xff for b in target[START:])
    return basis,target,images['q128-pass1.bin']

def check_chips(radio,guard,expected):
    """The attached flash chips must be the ones this radio's backup recorded; sets the guard's read limits"""
    for memory in ('progmem','q128'):
        got=radio.jedec(memory)
        if got.hex()!=expected[memory]:raise ValueError('Attached flash IDs differ from the saved unit')
        guard.limits[memory]=backup.capacity(got,memory)

def main():
    ap=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port',required=True);ap.add_argument('--backup',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--restore-stock',action='store_true',help='rewrite every application sector from the verified backup; needs --use-saved-backup')
    ap.add_argument('--use-saved-backup',action='store_true',help='skip all pre-write firmware/data reads (the restore works from the saved backup only)')
    ap.add_argument('--wait',type=int,default=120)
    ap.add_argument('--plan-only',action='store_true',help='print the restore plan; no serial port is opened')
    args=ap.parse_args()
    if not 1<=args.wait<=600:ap.error('Wait must be 1..600 seconds')
    if not args.restore_stock or not args.use_saved_backup:
        ap.error('This program only restores a stock backup: --restore-stock --use-saved-backup')
    original,target,data=restore_inputs(args.backup)
    expected_chips=backup_jedec(args.backup)
    todo=[a for a in range(START,SIZE,SECTOR) if original[a:a+SECTOR]!=target[a:a+SECTOR]]
    plan={'mode':'restore_stock','changed_sectors':len(todo),'first':hex(todo[0]) if todo else None,
          'last':hex(todo[-1]) if todo else None,'target_application_sha256':backup.sha(target[START:]),
          'boot_area_unchanged':original[:START]==target[:START],'codec_slot_unchanged':False,
          'codec_slot_rewritten_from_backup':True,'data_chip_written':False}
    print(json.dumps(plan,indent=2),flush=True)
    if args.plan_only:return
    module=backup.load_upstream()
    args.out.mkdir(parents=True,exist_ok=False,mode=0o700)
    report={'started_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'mode':'restore_stock',
            'target_application_sha256':backup.sha(target[START:]),
            'writes_started':False,'installation_verified':False,'bootloader_unchanged':False,
            'data_sector_crcs_unchanged':False,'restart_sent':False,
            'preinstall_reads_requested':False,'write_basis':'saved original stock backup'}
    def receipt():
        report['outgoing_operations']=guard.counts
        report['planned_write_frames']=len(guard.plan);report['sent_write_frames']=guard.index
        (args.out/'install-receipt.json').write_text(json.dumps(report,indent=2)+'\n')
    import serial
    raw=serial.Serial(port=None,baudrate=115200,timeout=0.02,write_timeout=3)
    raw.rts=raw.dtr=False;raw.port=args.port
    guard=InstallPort(raw,module,original,target,skip_original=True)
    report['changed_application_sectors']=guard.todo
    try:
        raw.open()
        radio=module.Radio(guard,log=lambda s:print(s,flush=True),retries=3)
        print(f'READY on {args.port}: turn ON normally, no buttons held. Waiting {args.wait}s.',flush=True)
        radio.start(wait=args.wait)
        radio.configure('dm32')
        if not radio.v6 or radio.v6['board']!=1 or radio.v6['flags']!=0 or not radio.configured:
            raise ValueError('Monitor configuration/identity did not validate')
        radio.check_writable()
        check_chips(radio,guard,expected_chips)
        print('Using the verified saved backup. No pre-write firmware or settings-chip reads.',flush=True)
        guard.authorize_saved_backup()
        report['writes_started']=True;receipt()
        print(f'INSTALLING: {len(guard.todo)} changed application sectors. Boot/data writes blocked.',flush=True)
        last=[-1]
        def progress(n,total):
            p=n*100//total
            if p//5!=last[0]:print(f'Firmware written and sector-readback checked: {p}% ({n}/{total})',flush=True);last[0]=p//5;receipt()
        write_started=time.monotonic();write_plan(radio,guard,progress)
        report['write_seconds']=time.monotonic()-write_started
        after=read_program(radio,args.out/'application-after.bin','Verify newly installed application',start=START)
        guard.authorize_reset_application(after)
        report['installed_application_sha256']=backup.sha(after)
        report['installation_verified']=True
        receipt();print('VERIFIED: installed application readback matches. Restarting.',flush=True)
        radio.retries=0;radio.reset_chip();report['restart_sent']=True;receipt()
        print('PASS: stock application restored and verified byte for byte. Confirm the normal stock screen on the radio.',flush=True)
    except BaseException as exc:
        report['error']=f'{type(exc).__name__}: {exc}';raise
    finally:
        report['finished_utc']=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())
        raw.close();receipt()
if __name__=='__main__':main()
