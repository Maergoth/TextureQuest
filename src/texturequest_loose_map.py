"""Build the existing XXH3 graphics lookup against verified loose DDS objects.

Only reads the client's stock archives. No replacement PAK is read or written.
"""
from collections import defaultdict
import hashlib
from pathlib import Path
import struct
import xxhash
from texture_swapper import core
import texturequest_graphics_catalog as catalog
import texturequest_graphics_vpk as vpk
import texturequest_texture_map as wire
from texturequest_shared_textures import group_key
from texturequest_release import check_pause

FILE = struct.Struct('<QI32s')


def write_map(version, digest, objects, stocks, rows):
    strings, offsets = bytearray(), {}
    def intern(text):
        if text not in offsets:
            offsets[text] = len(strings)
            strings.extend(text.encode('utf-8') + b'\0')
        return offsets[text]
    files = [FILE.pack(o['bytes'], intern(o['entry']), bytes.fromhex(o['sha256'])) for o in objects]
    archives = [wire.STOCK_ARCHIVE.pack(intern(name)) for name in stocks]
    packed = []
    for r in sorted(rows, key=lambda r: (r['key_low'], r['key_high'], *r['source_layout'][:4])):
        packed.append(wire.ROW.pack(r['key_low'], r['key_high'], *r['source_layout'][:4],
                                   *r['target_layout'][:4], r['file'], 0, 0,
                                   objects[r['file']]['bytes'], 128, r['pixel_bytes'],
                                   r['stock'], intern(r['member'])))
    header = wire.HEADER.pack(b'TQMAP002', *map(int, version.split('.')), bytes.fromhex(digest),
                              len(files), len(stocks), len(packed), len(strings))
    return header + b''.join(files + archives + packed) + strings


def build(game, index, selected, aliases, object_root, digest, progress=lambda _: None, pause=None):
    object_root, game = core._plain(object_root), core._plain(game)
    choices = {group_key(row): row for row in selected}
    objects_by_sha = {o['sha256']: o for o in index['objects']}
    wanted = sorted({row['object_sha256'] for row in selected})
    objects = [objects_by_sha[s] for s in wanted]
    file_ids = {o['sha256']: n for n, o in enumerate(objects)}
    by_owner = defaultdict(list)
    for row in index['named_textures']:
        if group_key(row) in choices:
            by_owner[row['owner']].append(row)
    mapped, pending, stocks, key_groups = {}, [], [], {}
    for number, (owner, rows) in enumerate(sorted(by_owner.items()), 1):
        check_pause(pause)
        progress(f'Matching stock textures {number}/{len(by_owner)}: {owner}')
        path = core._plain(game / 'paks' / core._name(owner), game)
        if not path.is_file():
            pending.append(dict(owner=owner, reason='stock-archive-missing'))
            continue
        # Retain the source handle with writes denied, including while hashing
        # the exact DDS. A patch that is in progress fails, rather than guesses.
        with catalog.held(path) as (stream, identity):
            entries = {e.member: e for e in vpk.index(stream, identity[2]).entries}
            stock_id = len(stocks)
            stocks.append(owner)
            for row in rows:
                check_pause(pause)
                key = group_key(row)
                if key in mapped:
                    continue
                entry = entries.get(row['member'])
                if not entry:
                    continue
                original = vpk.member_data(stream, entry)
                if hashlib.sha256(original).hexdigest() != row['source']['dds_sha256']:
                    continue
                try:
                    layout, pixels, _ = catalog.decode_dds(original)
                except ValueError:
                    continue
                if list(layout) != row['source']['layout'] or catalog.fingerprint(layout, pixels) != row['source']['fingerprint']:
                    continue
                target = choices[key]
                obj = objects_by_sha[target['object_sha256']]
                target_layout = target['target']['layout']
                if target_layout[3:] != list(layout)[3:] or target_layout[2] < layout[2]:
                    continue
                content = xxhash.xxh3_128_intdigest(pixels)
                mapped[key] = dict(key_low=content & (2**64-1), key_high=content >> 64,
                                   source_layout=list(layout), target_layout=target_layout,
                                   file=file_ids[obj['sha256']], pixel_bytes=target['target']['mip_bytes'],
                                   stock=stock_id, member=row['member'])
                key_groups.setdefault((content, tuple(layout)), []).append(key)
    # Layout is part of runtime matching. Only a true key collision within the
    # same layout is unsafe; equal bytes in different layouts remain distinct.
    for keys in key_groups.values():
        if len(keys) > 1:
            for key in keys:
                mapped.pop(key, None)
    missing = sorted(set(choices) - set(mapped))
    pending.extend(dict(group=k, reason='current-source-unmatched') for k in missing)
    pending.extend(aliases['pending'])
    if not mapped:
        raise ValueError('No current stock textures match this release. Patch EQ2 to stock, then verify again.')
    raw = write_map(index['texture_version'], digest, objects, stocks, list(mapped.values()))
    if len(raw) > wire.MAX_BYTES:
        raise ValueError('Loose texture map exceeds supported size')
    return raw, dict(mapped_groups=len(mapped), selected_groups=len(selected), pending=pending,
                     pending_groups=len(missing) + len(aliases['pending']),
                     alias_count=len(aliases['aliases']), shared_aliases=aliases['aliases'])
