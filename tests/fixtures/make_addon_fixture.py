"""Writes a fixture map, release VPK and expected pixels for graphics-addon/selfcheck.exe."""
from pathlib import Path
import json
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'src'))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import texturequest_texture_map as tmap  # noqa: E402
from test_texture_map import dds, vpk, stock_key  # noqa: E402


def main(out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    stock, upscaled = dds(8, 2, 11), dds(16, 3, 22)
    member = 'fixture/a.dds'
    archive = vpk({member: upscaled, 'fixture/b.txt': b'unrelated'})
    (out / 'Fixture.vpk').write_bytes(archive)
    (out / 'paks').mkdir(exist_ok=True)
    (out / 'paks' / 'Fixture.vpk').write_bytes(vpk({member: stock}))
    header = 8 + len(member) + 1
    low, high = stock_key(stock)
    row = dict(key_low=low, key_high=high, width=8, height=8, mips=2, family=1, target_width=16, target_height=16,
               target_mips=3, target_family=1, archive=0, compressed_bytes=struct.unpack_from('<I', archive, 0)[0],
               block_offset=0, inflated_bytes=header + len(upscaled), pixel_offset=header + 128,
               pixel_bytes=len(upscaled) - 128, stock_archive=0, stock_member_name=member)
    raw = tmap.write_map((1, 2, 3), '0' * 64, [('Fixture.vpk', len(archive))], ['Fixture.vpk'], [row])
    (out / 'map.bin').write_bytes(raw)
    (out / 'stock_pixels.bin').write_bytes(stock[128:])
    (out / 'upscaled_pixels.bin').write_bytes(upscaled[128:])
    (out / 'expected.json').write_text(json.dumps(dict(key_low=str(low), key_high=str(high))))


if __name__ == '__main__':
    main(sys.argv[1])
