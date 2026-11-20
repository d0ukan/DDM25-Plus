"""COM port list and cable auto-detect. Listing reads the Windows device registry only; no port is opened here.

Nothing is hard-coded to a COM number: the radio's programming cable is recognised by its USB chip. The Baofeng
DM-32UV cable is a WCH CH340 (VID 1A86, PID 7523); other common USB-serial chips are offered too, after it.
Virtual ports (com0com, Bluetooth serial links) are never preselected.
"""

# (VID, PID): name. Rank 0 is the DM-32UV's own cable chip
USB_SERIAL = {
    (0x1A86, 0x7523): ('CH340', 0),
    (0x1A86, 0x7522): ('CH340K', 1),
    (0x1A86, 0x5523): ('CH341', 1),
    (0x1A86, 0x55D4): ('CH9102', 1),
    (0x1A86, 0x55D3): ('CH343', 1),
    (0x10C4, 0xEA60): ('CP210x', 1),
    (0x0403, 0x6001): ('FTDI FT232R', 1),
    (0x0403, 0x6015): ('FTDI FT231X', 1),
    (0x0403, 0x6010): ('FTDI FT2232', 1),
    (0x067B, 0x2303): ('PL2303', 1),
    (0x067B, 0x23A3): ('PL2303GT', 1),
}
CH340 = (0x1A86, 0x7523)
VIRTUAL_HINTS = ('com0com', 'virtual', 'cnca', 'cncb', 'bthenum', 'bluetooth', 'emulator')


class PortInfo:
    def __init__(self, device, description='', vid=None, pid=None, hwid=''):
        self.device = device
        self.description = description or ''
        self.vid, self.pid = vid, pid
        self.hwid = hwid or ''

    @property
    def chip(self):
        """The USB-serial chip's name, or ''"""
        return USB_SERIAL.get((self.vid, self.pid), ('', 9))[0]

    @property
    def rank(self):
        """0: the DM-32UV cable chip, 1: another known USB-serial chip, 2: an unknown real port, 3: virtual"""
        if self.is_virtual:
            return 3
        return USB_SERIAL.get((self.vid, self.pid), ('', 2))[1]

    @property
    def is_ch340(self):
        return (self.vid, self.pid) == CH340

    @property
    def is_usb_serial(self):
        return (self.vid, self.pid) in USB_SERIAL

    @property
    def is_virtual(self):
        text = (self.description + ' ' + self.hwid).lower()
        return any(h in text for h in VIRTUAL_HINTS)

    def label(self):
        text = self.device
        if self.description and self.description != self.device:
            text += ' - ' + self.description
        if self.vid is not None and self.pid is not None:
            text += f' ({self.vid:04X}:{self.pid:04X})'
        if self.is_ch340:
            text += '  [telsiz kablosu]'
        elif self.is_usb_serial:
            text += f'  [USB-seri: {self.chip}]'
        elif self.is_virtual:
            text += '  [sanal port, telsiz değil]'
        return text


def list_ports():
    try:
        from serial.tools import list_ports as lp
    except ImportError:
        return []
    out = [PortInfo(p.device, p.description, p.vid, p.pid, p.hwid) for p in lp.comports()]
    return sorted(out, key=lambda p: _port_number(p.device))


def _port_number(device):
    digits = ''.join(c for c in device if c.isdigit())
    return int(digits) if digits else 0


def candidates(ports):
    """The ports that may be the radio cable, best first: the CH340 ones, then other known USB-serial chips; without
    any of those, every real (non-virtual) port"""
    usb = sorted((p for p in ports if p.rank <= 1), key=lambda p: (p.rank, _port_number(p.device)))
    if usb:
        return usb
    return [p for p in ports if not p.is_virtual]


def preferred(ports, last=None):
    """The port to preselect: among the best-ranked candidates the last used one, else the first; a lone unknown
    real port only when it was used last. Never a virtual port"""
    cands = candidates(ports)
    if not cands:
        return None
    best = min(p.rank for p in cands)
    top = [p for p in cands if p.rank == best]
    if best <= 1:
        return next((p for p in top if p.device == last), top[0])
    return next((p for p in top if p.device == last), None)


def status_text(ports, port):
    chosen = next((p for p in ports if p.device == port), None)
    if chosen is not None and chosen.is_ch340:
        return f'Telsiz kablosu bulundu ({port}).'
    if chosen is not None and chosen.is_usb_serial:
        return f'USB-seri kablo bulundu ({port}, {chosen.chip}). DM-32UV kablosu genelde CH340 olur.'
    if port:
        return f'Seçili port: {port} (telsiz kablosu olduğundan emin olun).'
    return 'Kablo bulunamadı. Kabloyu takın ve "Yenile"ye basın (gerekirse CH340 sürücüsünü kurun).'
