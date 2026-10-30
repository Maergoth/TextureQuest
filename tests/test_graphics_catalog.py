from pathlib import Path
import hashlib
import io
import json
import struct
import sys
import tempfile
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
import texturequest_graphics_catalog as g


def dds(width=8, height=8, mips=2, fourcc=b'DXT3', family=2, tail=b'qualified tail'):
    w = [0] * 31
    w[0], w[1], w[2], w[3], w[6], w[18], w[19], w[20], w[26] = 124, 0x21007, height, width, mips, 32, 4, int.from_bytes(fourcc, 'little'), 0x401008
    data = bytes((i * 7 + 11) % 256 for i in range(sum(x[4] for x in g.mip_levels((width, height, mips, family, 1)))))
    return b'DDS ' + struct.pack('<31I', *w) + data + tail


class CatalogTests(unittest.TestCase):
    def test_all_compressed_and_signed_mips_and_tail(self):
        for fourcc, family in [(b'DXT1', 1), (b'DXT3', 2), (b'DXT5', 3), (b'QWVU', 7)]:
            data = dds(fourcc=fourcc, family=family)
            layout, pixels, tail = g.decode_dds(data)
            self.assertEqual(layout, (8, 8, 2, family, 1))
            self.assertEqual(tail, b'qualified tail')
            self.assertEqual(len(g.fingerprint(layout, pixels)), 64)
            altered = bytearray(pixels); altered[-1] ^= 1
            self.assertNotEqual(g.fingerprint(layout, pixels), g.fingerprint(layout, altered))

    def test_raw_profiles_and_refusals(self):
        for flags, bits, masks, family in [(0x41, 32, (0xff0000, 0xff00, 0xff, 0xff000000), 4),
                                           (0x40, 32, (0xff0000, 0xff00, 0xff, 0), 5),
                                           (0x20000, 8, (255, 0, 0, 0), 6)]:
            words = [0] * 31
            words[0], words[2], words[3], words[6], words[18] = 124, 8, 8, 1, 32
            words[19:26] = (flags, 0, bits, *masks)
            pixels = bytes(64 * (1 if family == 6 else 4))
            self.assertEqual(g.decode_dds(b'DDS ' + struct.pack('<31I', *words) + pixels)[0][3], family)
        for field, value in [(18, 31), (27, 0x200), (5, 1), (6, 30)]:
            bad = bytearray(dds()); struct.pack_into('<I', bad, 4 + field * 4, value)
            with self.assertRaises(ValueError): g.decode_dds(bad)
        with self.assertRaises(ValueError): g.decode_dds(dds()[:129])

    def test_complete_preparation_deduplicates_payload_and_keeps_full_identity(self):
        root = Path(__file__).resolve().parents[1] / 'build'
        root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root) as td:
            root = Path(td); prep = root / 'prepared'; (prep / 'receipts').mkdir(parents=True)
            old, new = dds(), dds(width=16, height=16, mips=3)
            source = prep / 'source.vpk'; target = prep / 'target.vpk'
            source.write_bytes(b'stock stand-in'); target.write_bytes(b'released stand-in')
            pin = lambda p: dict(path=str(p), bytes=p.stat().st_size, sha256=hashlib.sha256(p.read_bytes()).hexdigest())
            decisions = [dict(action='RELEASED_SOURCE_COMPATIBLE', member=name,
                             observed=dict(bytes=len(old), sha256=hashlib.sha256(old).hexdigest(),
                                           target=dict(bytes=len(new), sha256=hashlib.sha256(new).hexdigest())))
                         for name in ['one.dds', 'two.dds']]
            receipt = dict(name='source.vpk', source=pin(source), output=pin(target), decisions=decisions)
            (prep / 'receipts/source.vpk.json').write_bytes(g.canonical(receipt))
            provider = dict(name='source.vpk', source=pin(source), output=pin(target), applied=2, receipt='receipts/source.vpk.json')
            ready = dict(schema='tq-fullpack-ready-v1', game=str(root/'game'), unavailable=[], provider_count=1, providers=[provider], applied=2)
            (prep/'READY.json').write_bytes(g.canonical(ready))
            entries = SimpleNamespace(entries=[SimpleNamespace(member=n) for n in ['one.dds', 'two.dds']])
            def member_data(f,e):
                f.seek(0)
                return old if f.read(5)==b'stock' else new
            api = SimpleNamespace(index=lambda f, size: entries, member_data=member_data)
            doc = g.prepare(prep/'READY.json', root/'graphics', api)
            self.assertTrue(doc['complete']); self.assertEqual(doc['rows'], 2)
            self.assertEqual(doc['payload']['bytes'], len(g.decode_dds(new)[1]))
            wire = (root/'graphics/catalog.bin').read_bytes()
            self.assertEqual(wire[:8], b'TQGX0001')
            self.assertEqual(struct.unpack_from('<IIQ', wire, 8), (1, 2, doc['payload']['bytes']))
            self.assertIn(b'one.dds', wire); self.assertIn(b'two.dds', wire)
            with self.assertRaises(ValueError): g.prepare(prep/'READY.json', root/'graphics', api)
            reversed_doc=g.prepare(prep/'READY.json',root/'original-comparison',api,direction='upscaled_canonical')
            self.assertEqual(reversed_doc['direction'],'upscaled_canonical')
            self.assertEqual(reversed_doc['payload']['bytes'],len(g.decode_dds(old)[1]))
            reverse_wire=(root/'original-comparison/catalog.bin').read_bytes()
            self.assertEqual(struct.unpack_from('<5I',reverse_wire,32),g.decode_dds(new)[0])


if __name__ == '__main__':
    unittest.main()
