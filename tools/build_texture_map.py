"""Build the published texture map from current stock paks and one release.

Usage: python tools/build_texture_map.py <game paks dir> <release VPK dir> <release-manifest.json> <out dir> [--tag vX.Y.Z] [--members <catalog dir>]
Writes texturequest-map-<texture_version>.bin and texturequest-map.json into <out dir>.

Only members listed in the release's member inventory (sideload-runtime/sideload/catalog/members-*.json) get rows.
A member whose stock DDS no longer matches the source it was upscaled from is reported, not mapped.
"""
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
import argparse
import hashlib
import json
import sys

import xxhash

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
import texturequest_graphics_catalog as catalog  # noqa: E402
import texturequest_graphics_vpk as vpk  # noqa: E402
import texturequest_texture_map as tmap  # noqa: E402
from texture_swapper import core  # noqa: E402

MEMBERS = Path(__file__).resolve().parents[1] / 'sideload-runtime' / 'sideload' / 'catalog'


def inventory(folder):
    """Map each archive name (casefolded) to {member: stock DDS sha256} for the members the release replaces."""
    selected = {}
    for path in sorted(Path(folder).glob('members-*.json')):
        for member in json.loads(path.read_text(encoding='utf-8'))['members']:
            selected.setdefault(member['owner'].casefold(), {})[member['member']] = member['source_dds']['sha256']
    tmap.need(selected, 'No member inventory in ' + str(folder))
    return selected


def key(pixels):
    value = xxhash.xxh3_128_intdigest(pixels)
    return value & (2**64 - 1), value >> 64


def scan(job):
    stock_path, release_path, sources = Path(job[0]), Path(job[1]), job[2]
    rows, skipped = [], []
    with stock_path.open('rb') as stock, release_path.open('rb') as upscaled:
        stock_entries = {e.member: e for e in vpk.index(stock, stock_path.stat().st_size).entries}
        for entry in vpk.index(upscaled, release_path.stat().st_size).entries:
            if entry.member not in sources or entry.member not in stock_entries:
                continue
            old, new = vpk.member_data(stock, stock_entries[entry.member]), vpk.member_data(upscaled, entry)
            if old == new:
                continue
            if hashlib.sha256(old).hexdigest() != sources[entry.member]:
                skipped.append(dict(archive=release_path.name, member=entry.member, reason='stock changed since upscale'))
                continue
            try:
                old_layout, old_pixels, _ = catalog.decode_dds(old)
                new_layout, new_pixels, _ = catalog.decode_dds(new)
            except ValueError as error:
                skipped.append(dict(archive=release_path.name, member=entry.member, reason=str(error)))
                continue
            if old_layout[3] != new_layout[3]:
                skipped.append(dict(archive=release_path.name, member=entry.member, reason='format differs'))
                continue
            if new_layout[2] < old_layout[2]:
                skipped.append(dict(archive=release_path.name, member=entry.member, reason='fewer upscaled mips'))
                continue
            header = 8 + len(entry.member.encode('latin-1')) + 1
            low, high = key(old_pixels)
            rows.append(dict(key_low=low, key_high=high, width=old_layout[0], height=old_layout[1], mips=old_layout[2],
                             family=old_layout[3], target_width=new_layout[0], target_height=new_layout[1],
                             target_mips=new_layout[2], target_family=new_layout[3], compressed_bytes=entry.csize,
                             block_offset=entry.offset, inflated_bytes=header + len(new), pixel_offset=header + 128,
                             pixel_bytes=len(new_pixels), stock_member_name=entry.member,
                             target_digest=xxhash.xxh3_128_intdigest(new_pixels)))
    return release_path.name, stock_path.name, rows, skipped


def build(paks, releases, manifest_path, out, tag=None, members=MEMBERS):
    manifest = json.loads(Path(manifest_path).read_text(encoding='utf-8'))
    tmap.need(manifest.get('schema') == 'texturequest-release-v3', 'Release manifest must be schema v3')
    selected = inventory(members)
    stock_by_name = {p.name.casefold(): p for p in Path(paks).glob('*.vpk')}
    jobs = []
    for archive in manifest['archives']:
        stock = stock_by_name.get(archive['name'].casefold())
        tmap.need(stock is not None, 'No stock pak for ' + archive['name'])
        jobs.append((str(stock), str(Path(releases) / archive['name']), selected.get(archive['name'].casefold(), {})))
    archives, stock_archives, rows, skipped = [], [], [], []
    with ProcessPoolExecutor() as pool:
        for release_name, stock_name, archive_rows, archive_skipped in pool.map(scan, jobs):
            index = len(archives)
            archives.append((release_name, (Path(releases) / release_name).stat().st_size))
            stock_archives.append(stock_name)
            for row in archive_rows:
                row['archive'] = row['stock_archive'] = index
            rows += archive_rows
            skipped += archive_skipped
    outcomes = {}
    for row in rows:
        stock_key = (row['key_low'], row['key_high'], row['width'], row['height'], row['mips'], row['family'])
        outcomes.setdefault(stock_key, set()).add(row['target_digest'])
    ambiguous = {k for k, targets in outcomes.items() if len(targets) > 1}
    unique = {}
    for row in rows:
        stock_key = (row['key_low'], row['key_high'], row['width'], row['height'], row['mips'], row['family'])
        if stock_key not in ambiguous:
            unique.setdefault((row['key_low'], row['key_high']), row)
    version = tuple(int(x) for x in manifest['texture_version'].split('.'))
    raw = tmap.write_map(version, core._digest(manifest), archives, stock_archives, list(unique.values()))
    tmap.read_map(raw)
    name = 'texturequest-map-' + manifest['texture_version'] + '.bin'
    out = Path(out)
    (out / name).write_bytes(raw)
    tag = tag or 'v' + manifest['version']
    descriptor = dict(schema=tmap.DESCRIPTOR_SCHEMA, texture_version=manifest['texture_version'],
                      release_digest=core._digest(manifest),
                      map=dict(name=name, bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest(),
                               url=tmap.REPOSITORY + '/releases/download/' + tag + '/' + name))
    tmap.validate_descriptor(descriptor)
    (out / 'texturequest-map.json').write_text(json.dumps(descriptor, indent=1) + '\n', encoding='utf-8', newline='\n')
    return dict(replaced=len(rows) + len([s for s in skipped if s['reason'] in ('format differs', 'fewer upscaled mips')]),
                stock_changed=sum(1 for s in skipped if s['reason'] == 'stock changed since upscale'),
                rows=len(unique), ambiguous_keys=len(ambiguous),
                ambiguous_members=sum(1 for r in rows if (r['key_low'], r['key_high'], r['width'], r['height'], r['mips'], r['family']) in ambiguous),
                skipped=skipped, map_bytes=len(raw))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('paks', type=Path)
    parser.add_argument('releases', type=Path)
    parser.add_argument('manifest', type=Path)
    parser.add_argument('out', type=Path)
    parser.add_argument('--tag')
    parser.add_argument('--members', type=Path, default=MEMBERS)
    args = parser.parse_args()
    summary = build(args.paks, args.releases, args.manifest, args.out, args.tag, args.members)
    print(json.dumps({k: v for k, v in summary.items() if k != 'skipped'} | {'skipped': len(summary['skipped'])}, indent=1))
    for row in summary['skipped']:
        print('skipped', row['archive'], row['member'], row['reason'])


if __name__ == '__main__':
    main()
