"""Reject incomplete captures and resolve native PCs with an ASLR load base."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('switch_profile', Path(__file__).resolve().parents[1] / 'tools/profile_switch_cpu.py')
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


class ProfileChecks(unittest.TestCase):
    text = ('[cpu-native] reason=time_limit samples=2 anchor=0x10100\n'
            '[cpu-native] csv_hex=pc,lr,sp\n10208,10210,ff000\n10300,10204,ff100\n[cpu-native] end\n')

    def test_relocated_addresses_and_unknown_code(self):
        symbols = tool.parse_symbols('00000100 00000004 T night100SwitchCpuSamplerAnchor\n'
                                     '00000200 00000040 t physics(float)\n')
        metadata, rows = tool.parse_profile(self.text)
        report = tool.summarize(metadata, rows, symbols)
        self.assertEqual(report['load_base'], '0x10000')
        self.assertEqual(report['samples'], 2)
        self.assertEqual(report['program_counters'][0], {'function': 'physics(float)', 'samples': 1, 'percent': 50.0})
        self.assertEqual(report['return_addresses'][0]['samples'], 2)

    def test_truncated_and_corrupted_logs_are_rejected(self):
        for text in (self.text.replace('[cpu-native] end', ''),
                     self.text.replace('samples=2', 'samples=3'),
                     self.text.replace('10208,10210,ff000', '[log] omitted bytes')):
            with self.subTest(text=text), self.assertRaises(ValueError):
                tool.parse_profile(text)

    def test_empty_or_wrong_elf_cannot_produce_a_profile(self):
        empty = '[cpu-native] reason=context_failed samples=0 anchor=0x10100\n[cpu-native] csv_hex=pc,lr,sp\n[cpu-native] end\n'
        with self.assertRaises(ValueError):
            tool.parse_profile(empty)
        metadata, rows = tool.parse_profile(self.text)
        with self.assertRaises(ValueError):
            tool.summarize(metadata, rows, [])

    def test_assembly_svc_symbols_without_size_are_resolved(self):
        symbols = tool.parse_symbols('00000100 00000004 T night100SwitchCpuSamplerAnchor\n'
                                     '00000200 T svcSendSyncRequest\n00000208 T svcBreak\n')
        metadata, rows = tool.parse_profile(self.text)
        report = tool.summarize(metadata, [(0x10204, 0x10204, 0)], symbols)
        self.assertEqual(report['program_counters'][0]['function'], 'svcSendSyncRequest')


if __name__ == '__main__':
    unittest.main()
