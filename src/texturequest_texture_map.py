"""Owner-published texture map: stock content key -> upscaled VPK member."""
from dataclasses import dataclass
import hashlib
import os
import re
import struct
from urllib.request import urlopen
import uuid

MAGIC = b'TQMAP001'
HEADER = struct.Struct('<8s3I32s4I')
ARCHIVE = struct.Struct('<QI')
STOCK_ARCHIVE = struct.Struct('<I')
ROW = struct.Struct('<QQ8IIIQIIQII')
MAX_ROWS = 200_000
MAX_BYTES = 64 * 1024 * 1024
DESCRIPTOR_SCHEMA = 'texturequest-map-v1'
REPOSITORY = 'https://github.com/Maergoth/TextureQuest'


@dataclass(frozen=True)
class Archive:
    name: str
    bytes: int


@dataclass(frozen=True)
class Row:
    key_low: int
    key_high: int
    width: int
    height: int
    mips: int
    family: int
    target_width: int
    target_height: int
    target_mips: int
    target_family: int
    archive: int
    compressed_bytes: int
    block_offset: int
    inflated_bytes: int
    pixel_offset: int
    pixel_bytes: int
    stock_archive: int
    stock_member: int


@dataclass(frozen=True)
class TextureMap:
    texture_version: tuple
    release_digest: str
    archives: tuple
    stock_archives: tuple
    rows: tuple
    strings: bytes

    def strings_at(self, offset):
        end = self.strings.index(b'\0', offset)
        return self.strings[offset:end].decode('utf-8')


def need(ok, why):
    if not ok:
        raise ValueError(why)


def _string(strings, offset):
    need(0 <= offset < len(strings) and b'\0' in strings[offset:], 'Map string offset out of range')
    end = strings.index(b'\0', offset)
    need(end > offset, 'Empty map string')
    return strings[offset:end].decode('utf-8')


def write_map(texture_version, release_digest, archives, stock_archives, rows):
    """archives: [(name, bytes)], stock_archives: [name], rows: [dict with Row fields + 'stock_member_name']."""
    strings = bytearray()
    offsets = {}

    def intern(text):
        if text not in offsets:
            offsets[text] = len(strings)
            strings.extend(text.encode('utf-8') + b'\0')
        return offsets[text]

    archive_entries = [ARCHIVE.pack(size, intern(name)) for name, size in archives]
    stock_entries = [STOCK_ARCHIVE.pack(intern(name)) for name in stock_archives]
    ordered = sorted(rows, key=lambda r: (r['key_low'], r['key_high']))
    packed_rows = [ROW.pack(r['key_low'], r['key_high'], r['width'], r['height'], r['mips'], r['family'],
                            r['target_width'], r['target_height'], r['target_mips'], r['target_family'],
                            r['archive'], r['compressed_bytes'], r['block_offset'], r['inflated_bytes'],
                            r['pixel_offset'], r['pixel_bytes'], r['stock_archive'], intern(r['stock_member_name']))
                   for r in ordered]
    header = HEADER.pack(MAGIC, *texture_version, bytes.fromhex(release_digest), len(archives),
                         len(stock_archives), len(ordered), len(strings))
    return header + b''.join(archive_entries) + b''.join(stock_entries) + b''.join(packed_rows) + bytes(strings)


def read_map(raw):
    need(HEADER.size <= len(raw) <= MAX_BYTES, 'Map size out of range')
    magic, major, minor, patch, digest, archive_count, stock_count, row_count, string_bytes = HEADER.unpack_from(raw)
    need(magic == MAGIC, 'Not a TextureQuest map')
    need(0 < archive_count <= 4096 and 0 < stock_count <= 4096 and row_count <= MAX_ROWS, 'Map counts out of range')
    expected = HEADER.size + archive_count * ARCHIVE.size + stock_count * STOCK_ARCHIVE.size + row_count * ROW.size + string_bytes
    need(len(raw) == expected, 'Map length does not match its header')
    at = HEADER.size
    strings = raw[expected - string_bytes:]
    need(not strings or strings[-1:] == b'\0', 'Map string table not terminated')
    archives = []
    for _ in range(archive_count):
        size, name = ARCHIVE.unpack_from(raw, at); at += ARCHIVE.size
        archives.append(Archive(_string(strings, name), size))
    stock = []
    for _ in range(stock_count):
        (name,) = STOCK_ARCHIVE.unpack_from(raw, at); at += STOCK_ARCHIVE.size
        stock.append(_string(strings, name))
    rows, previous = [], None
    for _ in range(row_count):
        row = Row(*ROW.unpack_from(raw, at)); at += ROW.size
        need(row.archive < archive_count and row.stock_archive < stock_count, 'Map row archive out of range')
        need(1 <= row.family <= 7 and row.family == row.target_family, 'Map row family invalid')
        need(row.mips >= 1 and row.target_mips >= row.mips and row.target_mips <= 15, 'Map row mips invalid')
        need(row.mips <= max(row.width, row.height).bit_length() and
             row.target_mips <= max(row.target_width, row.target_height).bit_length(), 'Map row mips exceed its size')
        need(row.inflated_bytes <= 256 << 20, 'Map row inflated size out of range')
        need(row.pixel_offset >= 128 and row.pixel_offset + row.pixel_bytes <= row.inflated_bytes, 'Map row pixel range invalid')
        need(row.block_offset + 4 + row.compressed_bytes <= archives[row.archive].bytes, 'Map row block outside its archive')
        _string(strings, row.stock_member)
        key = (row.key_low, row.key_high)
        need(previous is None or key > previous, 'Map rows not strictly sorted')
        previous = key
        rows.append(row)
    return TextureMap((major, minor, patch), digest.hex(), tuple(archives), tuple(stock), tuple(rows), bytes(strings))


def validate_descriptor(value):
    need(isinstance(value, dict) and set(value) == {'schema', 'texture_version', 'release_digest', 'map'},
         'Unsupported texture map descriptor')
    need(value['schema'] == DESCRIPTOR_SCHEMA, 'Unsupported texture map descriptor')
    need(re.fullmatch(r'\d{1,4}\.\d{1,4}\.\d{1,4}', str(value['texture_version'])) is not None, 'Invalid map texture version')
    need(re.fullmatch(r'[0-9a-f]{64}', str(value['release_digest'])) is not None, 'Invalid map release digest')
    item = value['map']
    need(isinstance(item, dict) and set(item) == {'name', 'bytes', 'sha256', 'url'}, 'Invalid map identity')
    need(item['name'] == 'texturequest-map-' + value['texture_version'] + '.bin', 'Unexpected map file name')
    need(type(item['bytes']) is int and 0 < item['bytes'] <= MAX_BYTES, 'Invalid map size')
    need(re.fullmatch(r'[0-9a-f]{64}', str(item['sha256'])) is not None, 'Invalid map checksum')
    need(re.fullmatch(re.escape(REPOSITORY) + r'/releases/download/v\d{1,4}\.\d{1,4}\.\d{1,4}/' + re.escape(item['name']),
                      str(item['url'])) is not None, 'Unexpected map URL')
    return value


DESCRIPTOR_NAME = 'texturequest-map.json'
LOCAL_MAP_NAME = 'texturequest-map.bin'


def _descriptor(opener, pause):
    from texturequest_version_check import _metadata, _json_asset
    value, _ = _json_asset(_metadata(opener, pause), DESCRIPTOR_NAME, 64 * 1024, opener, pause)
    return validate_descriptor(value)


def _download(item, opener, pause):
    import texturequest_latest as latest
    return latest._read(item['url'], item['bytes'], opener, pause)


def stage_map(state_dir, release_digest, opener=urlopen, pause=None):
    """Download and verify the published map for release_digest without touching the active map.

    Returns the staged file for activate_map, or None when the active map already matches."""
    from texture_swapper import core
    descriptor = _descriptor(opener, pause)
    need(descriptor['release_digest'] == release_digest,
         'The published texture map is for a different texture release. Download / Resume the latest textures first.')
    item = descriptor['map']
    folder = core._plain(state_dir) / 'graphics'
    path = core._plain(folder / LOCAL_MAP_NAME, state_dir)
    if path.is_file() and path.stat().st_size == item['bytes'] and hashlib.sha256(path.read_bytes()).hexdigest() == item['sha256']:
        return None
    raw = _download(item, opener, pause)
    need(len(raw) == item['bytes'] and hashlib.sha256(raw).hexdigest() == item['sha256'], 'Texture map failed checksum verification')
    parsed = read_map(raw)
    need(parsed.release_digest == descriptor['release_digest'], 'Texture map does not match its descriptor')
    folder.mkdir(parents=True, exist_ok=True)
    staged = core._plain(folder / (LOCAL_MAP_NAME + '.' + uuid.uuid4().hex + '.tmp'), state_dir)
    try:
        with staged.open('xb') as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
    except BaseException:
        if staged.exists():
            staged.unlink()
        raise
    return staged


def activate_map(state_dir, staged):
    """Make a map from stage_map the active graphics/texturequest-map.bin, so TextureQuest.ini never has to change."""
    from texture_swapper import core
    path = core._plain(core._plain(state_dir) / 'graphics' / LOCAL_MAP_NAME, state_dir)
    if staged is not None:
        os.replace(staged, path)
    return path


def discard_map(staged):
    """Remove a staged map that will not be activated."""
    if staged is not None and staged.exists():
        staged.unlink()


def fetch_map(state_dir, prepared, opener=urlopen, pause=None):
    """Return the verified local map for the downloaded pack, downloading it when the published one changed."""
    return activate_map(state_dir, stage_map(state_dir, prepared['release_sha256'], opener, pause))
