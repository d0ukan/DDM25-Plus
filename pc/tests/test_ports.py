"""Cable auto-detect on fake port lists (nothing is hard-coded to a COM number; no port is opened)."""
import unittest

import fixtures  # noqa: F401  (test data folder)
from ddm32_app import ports
from ddm32_app.ports import PortInfo

CH340 = lambda dev: PortInfo(dev, 'USB-SERIAL CH340', 0x1A86, 0x7523, 'USB VID:PID=1A86:7523')  # noqa: E731
CP210 = PortInfo('COM5', 'Silicon Labs CP210x USB to UART Bridge', 0x10C4, 0xEA60, 'USB VID:PID=10C4:EA60')
FTDI = PortInfo('COM6', 'USB Serial Port', 0x0403, 0x6001, 'USB VID:PID=0403:6001')
PL2303 = PortInfo('COM8', 'Prolific USB-to-Serial Comm Port', 0x067B, 0x2303, 'USB VID:PID=067B:2303')
COM1 = PortInfo('COM1', 'Communications Port (COM1)', None, None, 'ACPI\\PNP0501\\1')
VIRT = PortInfo('COM3', 'com0com - serial port emulator', None, None, 'COM0COM\\PORT')
BT = PortInfo('COM4', 'Standard Serial over Bluetooth link', None, None, 'BTHENUM\\{00001101}')


class PortsTest(unittest.TestCase):
    def test_ch340_is_chosen_anywhere(self):
        for number in ('COM3', 'COM21', 'COM27'):
            found = [COM1, VIRT, BT, CP210, CH340(number)]
            self.assertEqual(ports.preferred(found).device, number)
            self.assertEqual(ports.candidates(found)[0].device, number)

    def test_last_used_among_several_cables(self):
        found = [CH340('COM9'), CH340('COM11')]
        self.assertEqual(ports.preferred(found).device, 'COM9')
        self.assertEqual(ports.preferred(found, last='COM11').device, 'COM11')
        self.assertEqual(len(ports.candidates(found)), 2)

    def test_other_usb_serial_chips(self):
        found = [COM1, VIRT, FTDI, CP210, PL2303]
        self.assertEqual([p.device for p in ports.candidates(found)], ['COM5', 'COM6', 'COM8'])
        self.assertEqual(ports.preferred(found).device, 'COM5')
        self.assertEqual(ports.preferred(found, last='COM8').device, 'COM8')
        self.assertIn('USB-seri: CP210x', CP210.label())
        self.assertIn('CH340', ports.status_text(found, 'COM5'))

    def test_never_a_virtual_port(self):
        self.assertIsNone(ports.preferred([VIRT, BT]))
        self.assertEqual(ports.candidates([VIRT, BT]), [])
        self.assertIsNone(ports.preferred([VIRT, BT], last='COM3'))
        self.assertTrue(VIRT.is_virtual and BT.is_virtual)
        self.assertIn('sanal port', VIRT.label())

    def test_unknown_real_port_only_when_used_last(self):
        self.assertIsNone(ports.preferred([COM1, VIRT]))
        self.assertEqual(ports.preferred([COM1, VIRT], last='COM1').device, 'COM1')
        self.assertEqual([p.device for p in ports.candidates([COM1, VIRT])], ['COM1'])

    def test_status_texts(self):
        self.assertIn('Telsiz kablosu bulundu (COM9)', ports.status_text([CH340('COM9')], 'COM9'))
        self.assertIn('Kablo bulunamadı', ports.status_text([], None))
        self.assertIn('emin olun', ports.status_text([COM1], 'COM1'))
        self.assertIn('[telsiz kablosu]', CH340('COM2').label())


if __name__ == '__main__':
    unittest.main()
