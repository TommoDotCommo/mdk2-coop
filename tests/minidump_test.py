"""The minidump reader: exception, registers, modules, stack, rejection of damaged files."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import minidump
import minidump_fixture

MODULES = [(0x400000, 0x208000, 'C:\\games\\copy7\\mdk2hd.exe'),
           (0x52560000, 0x614000, 'C:\\games\\copy7\\dinput8.dll')]


class MinidumpTest(unittest.TestCase):
    def dump(self, **kw):
        return minidump.Minidump(minidump_fixture.build(MODULES, **kw))

    def test_exception_and_registers(self):
        d = self.dump(address=0x44995a, access=(1, 0x18), regs={'ebx': 0x184, 'edi': 0x186})
        ex = d.exception
        self.assertEqual(ex['code'], 0xC0000005)
        self.assertEqual(ex['address'], 0x44995a)
        self.assertEqual(ex['thread'], 7)
        self.assertEqual(ex['info'], (1, 0x18))
        self.assertEqual((ex['regs']['ebx'], ex['regs']['edi'], ex['regs']['eip']), (0x184, 0x186, 0x44995a))

    def test_location_names_the_module_and_offset(self):
        d = self.dump(address=0x4672ed)
        self.assertEqual(d.locate(0x4672ed), 'mdk2hd.exe+0x672ed')
        self.assertEqual(d.locate(0x52560000 + 0x408c5), 'dinput8.dll+0x408c5')
        self.assertEqual(d.locate(0x1234), '0x00001234')

    def test_stack_words_come_from_the_memory_stream(self):
        d = self.dump(stack=[0x40de07, 0, 0x1af938], esp=0x1af910)
        self.assertEqual(d.stack_words(8), [0x40de07, 0, 0x1af938])
        self.assertEqual(d.u32(0x1af910), 0x40de07)
        self.assertIsNone(d.u32(0x1af000))

    def test_describe_reads_like_the_game_log(self):
        text = '\n'.join(minidump.describe(self.dump(address=0x44995a, access=(1, 0x18), stack=[0x40de07])))
        self.assertIn('ACCESS_VIOLATION at mdk2hd.exe+0x4995a', text)
        self.assertIn('writing address 0x18', text)
        self.assertIn('stack +00 0040de07 mdk2hd.exe+0xde07', text)

    def test_damaged_files_are_rejected(self):
        raw = minidump_fixture.build(MODULES)
        with self.assertRaises(minidump.MinidumpError):
            minidump.Minidump(b'PK\x03\x04' + raw[4:])
        with self.assertRaises(minidump.MinidumpError):
            minidump.Minidump(raw[:20])
        with self.assertRaises(Exception):
            minidump.Minidump(raw[:80])

    def test_real_dumps_when_present(self):
        root = Path(__file__).resolve().parents[1] / 'local' / 'crash'
        for path in sorted(root.glob('dump*.dmp')):
            d = minidump.Minidump(path.read_bytes())
            self.assertEqual(d.exception['code'] & 0xC0000000, 0xC0000000, path.name)
            self.assertTrue(d.module_for(d.exception['address']), path.name)


if __name__ == '__main__':
    unittest.main()
