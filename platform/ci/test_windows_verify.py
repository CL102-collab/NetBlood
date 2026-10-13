#!/usr/bin/env python3
"""Ensure the XP CPU audit's scalar dispatch exception stays narrow."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location("windows_verify", Path(__file__).with_name("windows-verify.py"))
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)

# A minimal disassembler fixture, not a Windows executable or runtime test.
# Its addresses make the selector's references to the three helpers explicit.
DISPATCH = """
00001000 <__ZNSt12_GLOBAL__N_112__x86_rdrandEPv>:
 1000: rdrand %eax
 1003: ret
00002000 <__ZNSt12_GLOBAL__N_112__x86_rdseedEPv>:
 2000: rdseed %eax
 2003: ret
00003000 <__ZNSt12_GLOBAL__N_119__x86_rdseed_rdrandEPv>:
 3000: rdseed %eax
 3003: ret
00004000 <__ZNSt13random_device7_M_initERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE>:
 4000: mov $0x7,%eax
 4005: cpuid
 4007: and $0x40000,%ebx
 400d: je 4060
 4010: mov $0x2000,%eax
 4015: mov $0x1,%eax
 401a: cpuid
 401c: and $0x40000000,%ecx
 4022: je 4060
 4024: mov $0x1000,%eax
 4029: mov $0x3000,%eax
 4060: ret
"""


class WindowsCpuAuditTests(unittest.TestCase):
    def audit(self, listing, allow=True):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "nblood.exe"
            path.write_bytes(b"audit fixture; not an executable")
            with patch.object(VERIFY.subprocess, "check_output", return_value=listing):
                return VERIFY.verify_no_sse2(path, "objdump", allow_cpp_entropy=allow)

    def test_verified_scalar_dispatch_is_reported(self):
        report = self.audit(DISPATCH)
        self.assertTrue(report["no_sse2"])
        self.assertEqual(len(report["optional_scalar_entropy"]["instructions"]), 3)

    def test_exception_is_opt_in(self):
        with self.assertRaisesRegex(ValueError, "rdrand"):
            self.audit(DISPATCH, allow=False)

    def test_names_without_both_cpu_probes_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "rdrand"):
            self.audit(DISPATCH.replace("and $0x40000000,%ecx", "and $0x1,%ecx"))

    def test_unguarded_scalar_outside_helpers_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "5000: rdrand"):
            self.audit(DISPATCH + "00005000 <_main>:\n 5000: rdrand %eax\n")

    def test_sse2_inside_entropy_helper_is_still_rejected(self):
        with self.assertRaisesRegex(ValueError, "pxor"):
            self.audit(DISPATCH.replace("1003: ret", "1003: pxor %xmm0,%xmm0"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
