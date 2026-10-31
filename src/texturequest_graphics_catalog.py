"""Disk-backed graphics catalog over a verified full-pack preparation.

No game writes and no generator imports. DDS payloads are streamed one member
at a time; the runtime index contains identities and offsets, never all pixels.
"""
from __future__ import annotations

from collections import Counter
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct

MAX_DDS = 64 * 1024**2
MAX_ROWS = 100_000
MAGIC = b'TQGX0001'


def need(ok, message):
    if not ok:
        raise ValueError(message)


def canonical(doc):
    return json.dumps(doc, sort_keys=True, ensure_ascii=False, separators=(',', ':'), allow_nan=False).encode('utf-8')


def fingerprint(layout, pixels):
    width, height, mips, family, representation = layout
    domain = bytearray(b'TQABUP01')
    domain += struct.pack('<7I', representation, width, height, mips, family, 1, 1)
    levels = mip_levels(layout)
    for w, h, row, rows, size in levels:
        domain += struct.pack('<4I', w, h, row, rows)
    need(sum(x[4] for x in levels) == len(pixels), 'DDS mip payload length differs')
    return hashlib.sha256(domain + pixels).hexdigest()


def mip_levels(layout):
    width, height, mips, family, representation = layout
    need(type(width) is int and type(height) is int and 0 < width <= 16384 and 0 < height <= 16384,
         'Unsupported DDS dimensions')
    need(representation == 1 and family in range(1, 8) and 0 < mips <= max(width, height).bit_length(),
         'Unsupported DDS layout')
    result = []
    for mip in range(mips):
        w, h = max(1, width >> mip), max(1, height >> mip)
        if family <= 3:
            row, rows = ((w + 3) // 4) * (8 if family == 1 else 16), (h + 3) // 4
        else:
            row, rows = w * (1 if family == 6 else 4), h
        result.append((w, h, row, rows, row * rows))
    need(sum(x[4] for x in result) <= MAX_DDS, 'DDS mip byte limit')
    return result


def decode_dds(data):
    need(128 <= len(data) <= MAX_DDS + 4096 and data[:4] == b'DDS ', 'Invalid DDS framing')
    words = struct.unpack_from('<31I', data, 4)
    need(words[0] == 124 and words[18] == 32, 'Invalid DDS header sizes')
    height, width, depth, mips = words[2], words[3], words[5], words[6] or 1
    need(not depth and not words[27] and not words[28] and not words[29] and not words[30],
         'Cube, volume, or reserved DDS profile requires separate support')
    flags, fourcc, bits, r, g, b, a = words[19:26]
    family = 0
    if flags == 4:
        family = {b'DXT1': 1, b'DXT3': 2, b'DXT5': 3, b'QWVU': 7}.get(struct.pack('<I', fourcc), 0)
        need(bits == r == g == b == a == 0, 'Unexpected FOURCC bit masks')
    elif not fourcc:
        if flags == 0x41 and bits == 32 and (r, g, b, a) == (0xff0000, 0xff00, 0xff, 0xff000000):
            family = 4
        elif flags == 0x40 and bits == 32 and (r, g, b, a) == (0xff0000, 0xff00, 0xff, 0):
            family = 5
        elif flags == 0x20000 and bits == 8 and (r, g, b, a) == (0xff, 0, 0, 0):
            family = 6
        elif flags == 0x40 and bits == 8 and (r, g, b, a) == (0, 0, 0, 0) and words[4] == 0:
            family = 6  # Exact retained PF64/bits8/zero-mask/pitch0 RAW8 profile.
    need(family != 0, 'DDS pixel format requires an explicit graphics profile')
    layout = (width, height, mips, family, 1)
    size = sum(x[4] for x in mip_levels(layout))
    need(len(data) >= 128 + size, 'Truncated DDS mip chain')
    return layout, data[128:128 + size], data[128 + size:]


def stat5(st):
    return st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns, st.st_ctime_ns


@contextmanager
def held(path):
    path = Path(path).absolute()
    for parent in (path, *path.parents):
        need(not parent.is_symlink() and not getattr(parent.stat(), 'st_file_attributes', 0) & 0x400, 'Reparse FILE refused')
    before = stat5(path.stat())
    if os.name == 'nt':
        import ctypes
        import msvcrt
        k = ctypes.WinDLL('kernel32', use_last_error=True)
        k.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
        k.CreateFileW.restype = ctypes.c_void_p
        k.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = k.CreateFileW(str(path), 0x80000000, 1, None, 3, 0x00200000, None)
        need(handle not in (None, ctypes.c_void_p(-1).value), 'Cannot hold read-only graphics source: ' + str(ctypes.get_last_error()))
        try:
            fd = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
        except BaseException:
            k.CloseHandle(handle)
            raise
        stream = os.fdopen(fd, 'rb')
    else:
        stream = path.open('rb')
    with stream:
        handle_before = stat5(os.fstat(stream.fileno()))
        # Python3.12 Windows path stat reports creation time as ctime, whereas
        # fstat can report change time. Compare matching identity/size/mtime
        # semantics across views, and all five values within each view.
        need(handle_before[:4] == before[:4], 'FILE changed at open')
        yield stream, before
        need(stat5(os.fstat(stream.fileno())) == handle_before and before == stat5(path.stat()), 'FILE changed during graphics preparation')


def sha_file(stream):
    stream.seek(0)
    result = hashlib.sha256()
    for chunk in iter(lambda: stream.read(1024**2), b''):
        result.update(chunk)
    stream.seek(0)
    return result.hexdigest()


def wire_row(row):
    owner, member = row['owner'].encode('utf-8'), row['member'].encode('utf-8')
    need(0 < len(owner) <= 256 and 0 < len(member) <= 4096 and b'\0' not in owner + member, 'Catalog label bound')
    return (struct.pack('<2I10I2Q', len(owner), len(member), *row['source_layout'], *row['target_layout'],
                        row['offset'], row['payload_bytes'])
            + bytes.fromhex(row['source_fingerprint']) + bytes.fromhex(row['target_fingerprint'])
            + bytes.fromhex(row['payload_sha256']) + bytes.fromhex(row['source_dds_sha256'])
            + bytes.fromhex(row['target_dds_sha256']) + owner + member)


def prepare(ready_path, output, archive_api, report=lambda *args: None, *, direction='stock_canonical'):
    """archive_api is the reviewed VPK index/member reader, supplied explicitly."""
    need(direction in {'stock_canonical', 'upscaled_canonical'}, 'Unknown graphics catalog direction')
    ready_path, output = Path(ready_path).absolute(), Path(output).absolute()
    need(not output.exists(), 'Preserve the previous graphics preparation; use a new output directory')
    need(not output.is_relative_to(Path(json.loads(ready_path.read_bytes())['game'])), 'Graphics cache must be outside the game')
    ready_raw = ready_path.read_bytes()
    ready = json.loads(ready_raw)
    need(ready.get('schema') == 'tq-fullpack-ready-v1' and ready.get('unavailable') == [], 'Complete full-pack preparation required')
    providers = ready['providers']
    need(type(providers) is list and len(providers) == ready['provider_count'] <= 1000, 'Provider cardinality')
    output.mkdir(parents=True)
    (output/'providers').mkdir()
    rows, gaps, keys, dedup, inputs = [], [], {}, {}, []
    payload_hash = hashlib.sha256()
    payload_path = output / 'payloads.bin.partial'
    target_count = 0
    with payload_path.open('xb') as payload:
        for number, provider in enumerate(providers):
            start_rows, start_gaps = len(rows), len(gaps)
            receipt_path = ready_path.parent / provider['receipt']
            need(receipt_path.parent == ready_path.parent / 'receipts', 'Receipt path escaped')
            receipt_raw = receipt_path.read_bytes()
            receipt = json.loads(receipt_raw)
            need(receipt['name'] == provider['name'] and receipt['source'] == provider['source']
                 and receipt['output'] == provider['output'], 'Full-pack receipt/provider join differs')
            selected = [r for r in receipt['decisions'] if r['action'] == 'RELEASED_SOURCE_COMPATIBLE']
            need(len(selected) == provider['applied'], 'Provider selected count differs')
            target_count += len(selected)
            report('Indexing ' + provider['name'], number, len(providers))
            with held(provider['source']['path']) as (source, source_stat), held(provider['output']['path']) as (target, target_stat):
                # A patch may change a stock container while preserving selected DDS.
                # Match each complete source DDS below; never require old archive SHA.
                need(target_stat[2] == provider['output']['bytes'] and sha_file(target) == provider['output']['sha256'],
                     'Replacement container changed: ' + provider['name'])
                src = archive_api.index(source, source_stat[2])
                dst = archive_api.index(target, target_stat[2])
                src_names, dst_names = {}, {}
                for index, names in ((src, src_names), (dst, dst_names)):
                    for entry in index.entries:
                        need(entry.member not in names, 'Ambiguous VPK literal member')
                        names[entry.member] = entry
                for decision in selected:
                    name, expected = decision['member'], decision['observed']
                    need(name in src_names and name in dst_names, 'Selected VPK member is missing: ' + name)
                    old = archive_api.member_data(source, src_names[name])
                    new = archive_api.member_data(target, dst_names[name])
                    old_sha, new_sha = hashlib.sha256(old).hexdigest(), hashlib.sha256(new).hexdigest()
                    need((len(old), old_sha) == (expected['bytes'], expected['sha256']), 'Stock DDS changed: ' + name)
                    need((len(new), new_sha) == (expected['target']['bytes'], expected['target']['sha256']), 'Replacement DDS changed: ' + name)
                    try:
                        a, old_pixels, old_tail = decode_dds(old)
                        b, new_pixels, new_tail = decode_dds(new)
                        need(a[3:] == b[3:] and a[0] * b[1] == a[1] * b[0], 'Replacement format/aspect differs')
                    except ValueError as error:
                        gaps.append(dict(owner=provider['name'], member=name, reason=str(error), source_dds_sha256=old_sha, target_dds_sha256=new_sha))
                        continue
                    source_fp, target_fp = fingerprint(a, old_pixels), fingerprint(b, new_pixels)
                    if direction == 'upscaled_canonical':
                        a, b = b, a
                        source_fp, target_fp = target_fp, source_fp
                        old_sha, new_sha = new_sha, old_sha
                        old_pixels, new_pixels = new_pixels, old_pixels
                        old_tail, new_tail = new_tail, old_tail
                    key = (*a, source_fp)
                    outcome = (*b, target_fp)
                    # A graphics API exposes content, not the original asset name.
                    # Preserve conflicting named records and report them; runtime
                    # matching must retain stock rather than guess one outcome.
                    keys.setdefault(key, set()).add(outcome)
                    digest = hashlib.sha256(new_pixels).hexdigest()
                    if digest not in dedup:
                        need(shutil.disk_usage(output).free > len(new_pixels) + 512 * 1024**2, 'Insufficient disk space for graphics cache')
                        dedup[digest] = (payload.tell(), len(new_pixels))
                        payload.write(new_pixels)
                        payload_hash.update(new_pixels)
                    offset, size = dedup[digest]
                    rows.append(dict(owner=provider['name'], member=name, source_layout=a, target_layout=b,
                                     source_fingerprint=source_fp, target_fingerprint=target_fp, payload_sha256=digest,
                                     source_dds_sha256=old_sha, target_dds_sha256=new_sha,
                                     offset=offset, payload_bytes=size, source_tail_sha256=hashlib.sha256(old_tail).hexdigest(),
                                     target_tail_sha256=hashlib.sha256(new_tail).hexdigest()))
                    need(len(rows) <= MAX_ROWS, 'Graphics catalog row limit')
                inputs.append(dict(owner=provider['name'], source_path=provider['source']['path'], source_stat5=source_stat,
                                   replacement_path=provider['output']['path'], replacement_stat5=target_stat,
                                   replacement_sha256=provider['output']['sha256'], receipt_sha256=hashlib.sha256(receipt_raw).hexdigest()))
            payload.flush()
            # A durable per-provider ledger preserves completed preparation
            # evidence if a later provider refuses. No implicit resume/retry.
            (output/'providers'/('%03d.json' % number)).write_bytes(canonical(dict(
                input=inputs[-1], rows=rows[start_rows:], gaps=gaps[start_gaps:],
                payload_end=payload.tell(), payload_prefix_sha256=payload_hash.hexdigest())) + b'\n')
        payload.flush()
        os.fsync(payload.fileno())
    need(target_count == ready['applied'] and len(rows) + len(gaps) == target_count, 'Complete selected-set accounting differs')
    rows.sort(key=lambda r: (r['owner'], r['member']))
    payload_bytes = payload_path.stat().st_size
    wire = MAGIC + struct.pack('<IIQ', 1, len(rows), payload_bytes) + b''.join(wire_row(r) for r in rows)
    need(len(wire) <= 64 * 1024**2, 'Graphics metadata index limit')
    (output / 'catalog.bin').write_bytes(wire)
    os.replace(payload_path, output / 'payloads.bin')
    ambiguous = [dict(owner=r['owner'], member=r['member'], source_fingerprint=r['source_fingerprint'],
                      reason='IDENTICAL_STOCK_PIXELS_HAVE_DISTINCT_RELEASED_REPLACEMENTS')
                 for r in rows if len(keys[(*r['source_layout'], r['source_fingerprint'])]) > 1]
    doc = dict(schema='texturequest-graphics-catalog-v1', rows=len(rows), selected=target_count, gaps=gaps,
               ambiguous=ambiguous, complete=not gaps and not ambiguous, full_pack=True, direction=direction,
               ready_path=str(ready_path), ready_sha256=hashlib.sha256(ready_raw).hexdigest(),
               index=dict(name='catalog.bin', bytes=len(wire), sha256=hashlib.sha256(wire).hexdigest()),
               payload=dict(name='payloads.bin', bytes=payload_bytes, sha256=payload_hash.hexdigest()),
               formats=dict(Counter(str(r['source_layout'][3]) for r in rows)), inputs=inputs,
               memory_policy='Metadata index only at startup; verify individual payload ranges on demand; bounded runtime cache.',
               game_writes=False, generator_control=False)
    (output / 'CATALOG.json').write_bytes(canonical(doc) + b'\n')
    report('Graphics catalog complete' if doc['complete'] else 'Graphics catalog has visible mapping/profile gaps', len(providers), len(providers))
    return doc
