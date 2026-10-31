"""Map tool and reader tests: invented VPKs only."""
from pathlib import Path
import hashlib
import json
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

import xxhash

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import texturequest_texture_map as tmap
import build_texture_map as tool
from texture_swapper import core


def dds(size, mips, value, fourcc=b'DXT1'):
    words = [0] * 31
    words[0], words[1], words[2], words[3], words[6], words[18] = 124, 0x21007, size, size, mips, 32
    words[19], words[20] = 4, struct.unpack('<I', fourcc)[0]
    pixels = b''
    for mip in range(mips):
        side = max(1, size >> mip)
        pixels += bytes([value]) * (((side + 3) // 4) ** 2 * (8 if fourcc == b'DXT1' else 16))
    return b'DDS ' + struct.pack('<31I', *words) + pixels


def vpk(members):
    body, rows = b'', b''
    for name, data in members.items():
        encoded = name.encode('latin-1')
        packed = zlib.compress(struct.pack('<II', len(encoded) + 1, len(data)) + encoded + b'\0' + data)
        rows += struct.pack('<III', len(body), len(packed), len(encoded)) + encoded
        body += struct.pack('<I', len(packed)) + packed
    raw = struct.pack('<II', len(rows) + 8, len(members)) + rows
    index = zlib.compress(raw)
    return body + struct.pack('<I', len(index) + 12) + index + struct.pack('<III', 0x33744, len(body), 2)


def stock_key(data):
    pixels = data[128:]
    value = xxhash.xxh3_128_intdigest(pixels)
    return value & (2**64 - 1), value >> 64


class MapToolTests(unittest.TestCase):
    def build(self, stock, upscaled):
        root = Path(tempfile.mkdtemp())
        (root / 'paks').mkdir(); (root / 'release').mkdir(); (root / 'out').mkdir()
        rows = []
        for name in stock:
            (root / 'paks' / name).write_bytes(vpk(stock[name]))
            data = vpk(upscaled[name]); (root / 'release' / name).write_bytes(data)
            rows.append(dict(name=name, bytes=len(data), sha256='0' * 64, entries=len(upscaled[name]),
                             url='https://github.com/Maergoth/TextureQuest/releases/download/v1.3.0/' + name))
        manifest = dict(schema='texturequest-release-v3', version='1.7.1', texture_version='1.2.3',
                        texture_count=1, archives=rows)
        (root / 'release-manifest.json').write_text(json.dumps(manifest))
        summary = tool.build(root / 'paks', root / 'release', root / 'release-manifest.json', root / 'out')
        return root, manifest, summary

    def test_changed_member_becomes_row(self):
        old, new = dds(4, 1, 1), dds(8, 2, 9)
        root, manifest, summary = self.build({'A.vpk': {'t/a.dds': old}}, {'A.vpk': {'t/a.dds': new}})
        parsed = tmap.read_map((root / 'out' / 'texturequest-map-1.2.3.bin').read_bytes())
        self.assertEqual(summary['rows'], 1)
        row = parsed.rows[0]
        self.assertEqual((row.key_low, row.key_high), stock_key(old))
        self.assertEqual((row.width, row.height, row.mips, row.family), (4, 4, 1, 1))
        self.assertEqual((row.target_width, row.target_height, row.target_mips), (8, 8, 2))
        self.assertEqual(parsed.archives[row.archive].name, 'A.vpk')
        self.assertEqual(parsed.stock_archives[row.stock_archive], 'A.vpk')
        self.assertEqual(parsed.strings_at(row.stock_member), 't/a.dds')

    def test_unchanged_and_non_dds_members_skipped(self):
        same = dds(4, 1, 3)
        root, _, summary = self.build({'A.vpk': {'a.dds': same, 'b.txt': b'x'}}, {'A.vpk': {'a.dds': same, 'b.txt': b'y'}})
        self.assertEqual(summary['rows'], 0)

    def test_family_change_and_fewer_mips_reported(self):
        stock = {'A.vpk': {'a.dds': dds(4, 3, 1), 'b.dds': dds(4, 1, 1)}}
        upscaled = {'A.vpk': {'a.dds': dds(8, 2, 2), 'b.dds': dds(8, 1, 2, b'DXT5')}}
        root, _, summary = self.build(stock, upscaled)
        self.assertEqual(summary['rows'], 0)
        self.assertEqual(sorted(r['reason'] for r in summary['skipped']), ['fewer upscaled mips', 'format differs'])

    def test_ambiguous_stock_key_excluded(self):
        same = dds(4, 1, 5)
        stock = {'A.vpk': {'a.dds': same}, 'B.vpk': {'b.dds': same}}
        upscaled = {'A.vpk': {'a.dds': dds(8, 1, 6)}, 'B.vpk': {'b.dds': dds(8, 1, 7)}}
        _, _, summary = self.build(stock, upscaled)
        self.assertEqual((summary['rows'], summary['ambiguous_keys'], summary['ambiguous_members']), (0, 1, 2))

    def test_rows_sorted_and_descriptor_matches(self):
        stock = {'A.vpk': {f'{i}.dds': dds(4, 1, i) for i in range(1, 20)}}
        upscaled = {'A.vpk': {f'{i}.dds': dds(8, 1, i + 100) for i in range(1, 20)}}
        root, manifest, summary = self.build(stock, upscaled)
        raw = (root / 'out' / 'texturequest-map-1.2.3.bin').read_bytes()
        parsed = tmap.read_map(raw)
        keys = [(r.key_low, r.key_high) for r in parsed.rows]
        self.assertEqual(keys, sorted(keys))
        descriptor = json.loads((root / 'out' / 'texturequest-map.json').read_text())
        self.assertEqual(descriptor['release_digest'], core._digest(manifest))
        self.assertEqual(parsed.release_digest, core._digest(manifest))
        self.assertEqual(descriptor['map']['bytes'], len(raw))
        tmap.validate_descriptor(descriptor)

    def test_truncated_map_refused(self):
        root, _, _ = self.build({'A.vpk': {'a.dds': dds(4, 1, 1)}}, {'A.vpk': {'a.dds': dds(8, 1, 2)}})
        raw = (root / 'out' / 'texturequest-map-1.2.3.bin').read_bytes()
        with self.assertRaises(ValueError):
            tmap.read_map(raw[:-5])
        with self.assertRaises(ValueError):
            tmap.read_map(b'XXXX' + raw[4:])

    def test_too_many_target_mips_refused(self):
        root, _, _ = self.build({'A.vpk': {'a.dds': dds(4, 1, 1)}}, {'A.vpk': {'a.dds': dds(8, 1, 2)}})
        raw = bytearray((root / 'out' / 'texturequest-map-1.2.3.bin').read_bytes())
        at = len(raw) - len(tmap.read_map(bytes(raw)).strings) - tmap.ROW.size
        fields = list(tmap.ROW.unpack_from(raw, at))
        fields[8] = 16
        tmap.ROW.pack_into(raw, at, *fields)
        with self.assertRaises(ValueError):
            tmap.read_map(bytes(raw))


class FetchTests(unittest.TestCase):
    def setUp(self):
        root, manifest, _ = MapToolTests.build(self, {'A.vpk': {'a.dds': dds(4, 1, 1)}}, {'A.vpk': {'a.dds': dds(8, 1, 2)}})
        self.raw = (root / 'out' / 'texturequest-map-1.2.3.bin').read_bytes()
        self.descriptor = json.loads((root / 'out' / 'texturequest-map.json').read_text())
        self.prepared = {'release_sha256': core._digest(manifest)}
        self.state = Path(tempfile.mkdtemp())

    def fetch(self, descriptor=None, raw=None):
        descriptor = descriptor or self.descriptor
        raw = self.raw if raw is None else raw
        with patch.object(tmap, '_descriptor', return_value=descriptor), patch.object(tmap, '_download', return_value=raw) as download:
            return tmap.fetch_map(self.state, self.prepared), download

    def test_matching_map_saved(self):
        path, _ = self.fetch()
        self.assertEqual(path, self.state / 'graphics' / 'texturequest-map.bin')
        self.assertEqual(path.read_bytes(), self.raw)

    def test_other_release_refused(self):
        changed = dict(self.descriptor, release_digest='f' * 64)
        with self.assertRaisesRegex(ValueError, 'Download / Resume'):
            self.fetch(changed)
        self.assertFalse((self.state / 'graphics').exists())

    def test_wrong_bytes_refused(self):
        with self.assertRaises(ValueError):
            self.fetch(raw=self.raw[:-1] + b'\x01')
        self.assertFalse((self.state / 'graphics' / 'texturequest-map.bin').exists())

    def test_saved_map_reused(self):
        self.fetch()
        _, download = self.fetch()
        download.assert_not_called()


if __name__ == '__main__':
    unittest.main()
