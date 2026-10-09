from paths import ROOT
import struct
import unittest
from aspect_ratio import segment_fingerprint

class AspectFingerprintTests(unittest.TestCase):
    def test_container_padding_does_not_change_loaded_identity(self):
        elf=bytearray(144)
        struct.pack_into('<Q',elf,32,64)
        struct.pack_into('<H',elf,56,1)
        struct.pack_into('<IIQQQQQQ',elf,64,1,5,128,0,0,16,16,16)
        original=segment_fingerprint(elf)
        self.assertEqual(original,segment_fingerprint(elf+b'container-only'))
        elf[128]=1
        self.assertNotEqual(original,segment_fingerprint(elf))


if __name__ == '__main__':
    unittest.main()
