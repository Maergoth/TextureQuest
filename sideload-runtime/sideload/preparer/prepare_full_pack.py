"""Prepare the complete released pack over current stock, with no game writes.

Ordinary Python read-only source handles and stable-stat/hash checks are preparation
evidence, not native deny-write custody. Runtime loading is a separate component.
"""
from __future__ import annotations

import argparse
from collections import Counter
from contextlib import contextmanager
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path, PureWindowsPath
import re
import stat
import struct
import sys
import time
import zlib

U32 = 0xffffffff
CHUNK = 1024 * 1024
MAX_INDEX = 64 * CHUNK
MAX_MEMBER = 256 * CHUNK
MAX_VPL = 128 * CHUNK
MAX_RECORDS = 2_000_000
MAGIC = 0x00033744
PACKAGE_ROOT = Path(__file__).resolve().parent.parent
CATALOG_H = PACKAGE_ROOT / 'catalog/HANDOFF.json'
CATALOG_SHA = '8faf23b135734cc5e899932bc2fa1f09e575e7246500d7a3478e5fdfecaab065'


class Refusal(ValueError):
    def __init__(self, reason, details=None):
        super().__init__(reason)
        self.details = {} if details is None else details


def require(ok, why):
    if not ok:
        raise Refusal(why)


def canonical(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(',', ':'), allow_nan=False).encode('utf-8')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def digest(value):
    return sha(canonical(value))


def hash_string(value):
    require(type(value) is str and re.fullmatch('[a-f0-9]{64}', value), 'SHA256 shape')
    return value


def name(value, owner=False):
    require(type(value) is str and 0 < len(value) <= 4096 and '\0' not in value, 'name shape')
    require('\\' not in value and ':' not in value and not value.startswith('/'), 'name grammar')
    require(all(p not in ('', '.', '..') for p in value.split('/')), 'name components')
    value.encode('latin-1')
    if owner:
        require(re.fullmatch('[A-Za-z0-9_][A-Za-z0-9_.-]{0,180}\\.vpk', value) and '..' not in value and '/' not in value, 'owner grammar')
    return value


def plain(path, *, output=False):
    p = Path(path)
    require(p.is_absolute() and '..' not in p.parts and not str(p).startswith('\\\\'), 'absolute ordinary path required')
    require(re.fullmatch('[A-Za-z]:',p.drive) and all(not q.endswith(('.', ' ')) and not any(c in q for c in '<>:\"|?*\0') for q in p.parts[1:]), 'ordinary Windows path components required')
    if output:
        require(p != Path(p.anchor), 'output must be an owned ordinary directory')
    for q in (p, *p.parents):
        if os.path.lexists(q):
            s = q.lstat()
            require(not q.is_symlink() and not getattr(s, 'st_file_attributes', 0) & 0x400, 'reparse path refused')
    return p


def component_data_root(package):
    """Pure Windows lexical layout check; physical paths still pass plain()."""
    root = PureWindowsPath(package)
    require(root.is_absolute() and re.fullmatch('[A-Za-z]:', root.drive)
            and not str(package).startswith('\\\\') and '..' not in root.parts,
            'ordinary component path required')
    require(all(not q.endswith(('.', ' ')) and not any(c in q for c in '<>:"|?*\0')
                for q in root.parts[1:]), 'component path grammar')
    require(len(root.parts) >= 5 and tuple(q.lower() for q in root.parts[-3:-1]) == ('sideload', 'runtimes')
            and re.fullmatch('[a-f0-9]{64}', root.name.lower()), 'portable component layout')
    data = root.parent.parent.parent
    require(data != PureWindowsPath(data.anchor), 'data root cannot be a drive root')
    return data


def portable_scopes(game, prepared, output, package=PACKAGE_ROOT):
    """Agree with native ordinary-drive component and downloads boundaries."""
    root, game, prepared, out = map(PureWindowsPath, (package, game, prepared, output))
    data = component_data_root(root)
    require(not (game == data or game.is_relative_to(data) or data.is_relative_to(game)),
            'game and app data scopes must be disjoint')
    require(prepared == data/'direct-prepared.json', 'prepared metadata must belong to component app data')
    require(out != root and out.is_relative_to(root), 'output must be inside the loader component')
    return Path(data)


def five(s):
    return [s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns]


@contextmanager
def held(path):
    p = plain(path)
    pre = p.lstat()
    require(stat.S_ISREG(pre.st_mode) and pre.st_nlink == 1, 'source must be an ordinary one-link FILE')
    with p.open('rb') as f:
        handle_before = five(os.fstat(f.fileno()))
        path_before = five(p.stat())
        require(handle_before[:3] == five(pre)[:3] == path_before[:3], 'source identity/length changed at open')
        yield f, path_before
        require(five(os.fstat(f.fileno())) == handle_before and five(p.stat()) == path_before, 'source changed during preparation')


def stream_hash(f):
    f.seek(0)
    h = hashlib.sha256()
    while True:
        b = f.read(CHUNK)
        if not b:
            break
        h.update(b)
    return h.hexdigest()


def read_json(path, expected=None, limit=MAX_INDEX):
    with held(path) as (f, s):
        require(s[2] <= limit, 'JSON byte bound')
        raw = f.read(limit + 1)
        require(len(raw) == s[2], 'short JSON')
    pin = {'path': str(Path(path)), 'bytes': len(raw), 'sha256': sha(raw), 'stat5': s}
    if expected is not None:
        require(pin['bytes'] == expected['bytes'] and pin['sha256'] == expected['sha256'], 'pinned JSON changed')
    def unique(pairs):
        out = {}
        for k, v in pairs:
            require(k not in out, 'duplicate JSON key')
            out[k] = v
        return out
    return json.loads(raw, object_pairs_hook=unique), pin


def atomic(path, data):
    p = Path(path)
    tmp = p.with_name(p.name + '.partial')
    require(not p.exists() and not tmp.exists(), 'output exists; preserve it')
    with tmp.open('xb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, p)


def inflate(raw, limit, expected=None):
    try:
        d = zlib.decompressobj()
        value = d.decompress(raw, limit + 1)
    except zlib.error as exc:
        raise Refusal('invalid zlib') from exc
    require(len(value) <= limit and d.eof and not d.unused_data and not d.unconsumed_tail, 'zlib bound/EOF/tail')
    require(expected is None or len(value) == expected, 'inflated length')
    return value


@dataclass(frozen=True)
class Entry:
    member: str
    offset: int
    csize: int


@dataclass(frozen=True)
class Index:
    entries: tuple
    offset: int
    size: int


def index_literal_name(raw):
    """An internal key, NEVER a filesystem/extraction path or catalog admission."""
    require(type(raw) is bytes and 1 <= len(raw) <= 4096 and b'\0' not in raw, 'index literal name length/NUL')
    return raw.decode('latin-1')


def index(f, size, *, owner='<unspecified>', operation='VPK_INDEX'):
    try:
        return _index(f, size)
    except Refusal as exc:
        exc.details.update({'owner':owner,'operation':operation,'file_bytes':size,'reason':str(exc)})
        raise


def _index(f, size):
    require(16 <= size <= U32, 'VPK byte bound')
    f.seek(size - 12)
    magic, off, version = struct.unpack('<III', f.read(12))
    require(magic == MAGIC and version == 2 and off + 4 < size - 12, 'VPK footer')
    f.seek(off)
    field, = struct.unpack('<I', f.read(4))
    zlen = size - off - 16
    require(0 < zlen <= MAX_INDEX and field == zlen + 12, 'VPK index span')
    raw = inflate(f.read(zlen), MAX_INDEX)
    require(len(raw) >= 8, 'index header')
    length, count = struct.unpack_from('<II', raw)
    require(length == len(raw) and 1 <= count <= MAX_RECORDS, 'index count/length')
    pos, nextoff, rows, aliases = 8, 0, [], set()
    for ordinal in range(count):
        row_pos = pos
        boff = csize = n = None
        literal = b''
        try:
            require(pos + 12 <= len(raw), 'index row truncated')
            boff, csize, n = struct.unpack_from('<III', raw, pos)
            pos += 12
            literal = raw[pos:pos + min(n,4096)]
            require(1 <= n <= 4096 and pos + n <= len(raw), 'index name bound')
            member = index_literal_name(literal)
            pos += n
            require(member.casefold() not in aliases, 'ambiguous member aliases')
            aliases.add(member.casefold())
            require(boff == nextoff and 0 < csize <= U32 and boff + csize + 4 <= off, 'block geometry')
            f.seek(boff)
            require(f.read(4) == struct.pack('<I', csize), 'block leading length')
            rows.append(Entry(member, boff, csize))
            nextoff = boff + csize + 4
        except Refusal as exc:
            exc.details.update({'index_row_zero_based':ordinal,'index_row_one_based':ordinal+1,'row_byte_offset':row_pos,'block_offset':boff,'compressed_size':csize,'name_length':n,'literal_name':literal.decode('latin-1'),'literal_name_hex':literal.hex(),'inflated_index_bytes':len(raw),'inflated_index_sha256':sha(raw),'index_offset':off,'index_compressed_bytes':zlen,'row_window_hex':raw[max(0,row_pos-32):min(len(raw),pos+min(n or 0,4096)+32)].hex()})
            raise
    require(pos == len(raw) and nextoff == off, 'index/data trailing bytes')
    return Index(tuple(rows), off, size)


def member_data(f, entry):
    require(entry.csize <= MAX_MEMBER + CHUNK, 'selected compressed member bound')
    f.seek(entry.offset + 4)
    raw = f.read(entry.csize)
    require(len(raw) == entry.csize, 'member compressed short read')
    payload = inflate(raw, MAX_MEMBER + 4105)
    require(len(payload) >= 8, 'member header')
    n, length = struct.unpack_from('<II', payload)
    expected = entry.member.encode('latin-1') + b'\0'
    require(n == len(expected) and length <= MAX_MEMBER and payload[8:8+n] == expected and 8+n+length == len(payload), 'member full-name/length')
    return payload[8+n:]


def dds(data):
    require(len(data) >= 128 and data[:4] == b'DDS ' and struct.unpack_from('<I', data, 4)[0] == 124 and struct.unpack_from('<I', data, 76)[0] == 32, 'released/source DDS header')
    require(all(struct.unpack_from('<II', data, 12)), 'DDS dimensions')


def equal_raw(a, ae, b, be):
    if ae.csize != be.csize:
        return False
    a.seek(ae.offset)
    b.seek(be.offset)
    left = ae.csize + 4
    while left:
        n = min(CHUNK, left)
        av, bv = a.read(n), b.read(n)
        require(len(av) == n and len(bv) == n, 'raw comparison short read')
        if av != bv:
            return False
        left -= n
    return True


def copy_raw(src, e, dst):
    src.seek(e.offset)
    left = e.csize + 4
    while left:
        b = src.read(min(CHUNK, left))
        require(bool(b), 'raw copy short read')
        dst.write(b)
        left -= len(b)


def footer(rows, offset):
    raw = bytearray()
    for e in rows:
        n = e.member.encode('latin-1')
        raw.extend(struct.pack('<III', e.offset, e.csize, len(n)) + n)
    raw = struct.pack('<II', len(raw) + 8, len(rows)) + raw
    require(len(raw) <= MAX_INDEX, 'output index bound')
    compressed = zlib.compress(raw, 9)
    return struct.pack('<I', len(compressed) + 12) + compressed + struct.pack('<III', MAGIC, offset, 2)


def parse_vpl(blob):
    require(52 <= len(blob) <= MAX_VPL, 'VPL byte bound')
    w = struct.unpack_from('<13I', blob)
    directory, search, aux, pn, an, count, pc, pp, pz, az, ap, flags, validation = w
    require(1 <= pc <= 4096 and 1 <= count <= MAX_RECORDS and flags == 0x80000002 and validation == 52, 'VPL header')
    require(directory == 52 + pc*8 and search == directory + count*20 and aux == search + count*4, 'VPL geometry')
    require(aux <= pn < an <= len(blob) and an-pn == pz and len(blob)-an == az and pp <= 256*CHUNK and ap <= 256*CHUNK, 'VPL sections')
    ns = inflate(blob[pn:an], 256*CHUNK, pp)
    require(ns.endswith(b'\0'), 'VPL names terminator')
    owners = ns[:-1].decode('ascii').split('\0')
    require(len(owners) == pc and len(set(owners)) == pc and all(re.fullmatch('paks/[a-z0-9_][a-z0-9_.-]{0,180}\\.vpk', n) and '..' not in n for n in owners), 'VPL provider names')
    inflate(blob[an:], 256*CHUNK, ap)
    rows = tuple(struct.unpack_from('<5I', blob, directory+i*20) for i in range(count))
    require(all(r[2] < pc and r[4] for r in rows), 'VPL provider rows')
    validations = tuple(struct.unpack_from('<II', blob, 52+i*8) for i in range(pc))
    return w, tuple(n[5:] for n in owners), rows, validations


def load_catalog(handoff=CATALOG_H, expected_sha=CATALOG_SHA):
    h, hp = read_json(handoff)
    require(hp['sha256'] == expected_sha and h['schema'] == 'tq-portable-released-member-catalog-v1' and h['status'] == 'COMPLETE_FROZEN', 'catalog authority')
    chosen = h['files']
    wanted = {'current-archives.json', *(f'members-{i:02d}.json' for i in range(7))}
    require(type(chosen) is list and len(chosen) == 8 and all(type(p) is dict and set(p) == {'path','bytes','sha256'} and p['path'] in wanted and type(p['bytes']) is int and 0 < p['bytes'] <= MAX_INDEX for p in chosen)
            and {p['path'] for p in chosen} == wanted, 'catalog exact relative shard membership')
    archive_table, by_owner, pins = None, {}, [hp]
    ids, aliases = set(), set()
    root = Path(handoff).parent
    for p in chosen:
        hash_string(p['sha256'])
        path = plain(root / p['path'])
        require(path.is_relative_to(root), 'catalog product outside frozen domain')
        doc, pin = read_json(path, p)
        pins.append(pin)
        if path.name == 'current-archives.json':
            archive_table = doc['archives']
            continue
        require(doc['schema'] == 'released-member-whitelist-shard-v1', 'catalog shard schema')
        for r in doc['members']:
            owner, member = name(r['owner'], True), name(r['member'])
            require(member.lower().endswith('.dds'), 'catalog member is not DDS')
            rid = sha((owner+'\0'+member).encode('utf-8'))
            require(r['id'] == rid and rid not in ids and (owner.lower(), member.casefold()) not in aliases, 'catalog identity/alias')
            ids.add(rid)
            aliases.add((owner.lower(), member.casefold()))
            hash_string(r['source_dds']['sha256'])
            if r['replacement_dds']['sha256'] is not None:
                hash_string(r['replacement_dds']['sha256'])
            by_owner.setdefault(owner.lower(), {})[member] = r
    require(len(ids) == h['statistics']['union_identities'] and len(archive_table) == h['statistics']['archive_count'], 'catalog count')
    archive_names = [name(a['name'], True).lower() for a in archive_table]
    require(len(set(archive_names)) == len(archive_names) and set(by_owner) <= set(archive_names), 'catalog owner aliases')
    return archive_table, by_owner, pins


def prepared_rows(doc, prepared_path, archives):
    require(doc['schema'] == 'texturequest-direct-prepared-v1', 'prepared schema')
    wanted = {a['name']: {k: a[k] for k in ('name', 'bytes', 'sha256', 'entries')} for a in archives}
    release = doc['release']
    require(release['schema'] == 'texturequest-release-v3' and doc['release_sha256'] == digest(release), 'prepared release seal')
    require({a['name']: {k: a[k] for k in ('name','bytes','sha256','entries')} for a in release['archives']} == wanted and len(release['archives']) == len(wanted), 'prepared release differs from trusted texture domain')
    rows = doc['archives']
    require(len(rows) == len(wanted) and len({a['name'] for a in rows}) == len(rows), 'prepared archive membership')
    folder = plain(Path(prepared_path).parent / 'downloads')
    for row in rows:
        require({k: row[k] for k in ('name','bytes','sha256','entries')} == wanted.get(row['name']), 'prepared archive identity')
        require(plain(row['path']) == folder / row['name'], 'prepared download path')
    require(doc['total_bytes'] == sum(a['bytes'] for a in rows), 'prepared total bytes')
    require(doc['id'] == digest([{k: a[k] for k in ('name','bytes','sha256','entries')} for a in release['archives']])[:32], 'prepared id')
    return sorted(rows, key=lambda a: a['name'].lower())


def merge_provider(current, ready, output, released, catalog, effective, source_time):
    """One provider, two bounded streams, one selected DDS at a time."""
    with held(current) as (cf, cs), held(ready) as (rf, rs):
        current_sha, ready_sha = stream_hash(cf), stream_hash(rf)
        require(rs[2] == released['bytes'] and ready_sha == released['sha256'], 'complete downloaded archive hash')
        ci = index(cf, cs[2],owner=released['name'],operation='CURRENT_SOURCE_INDEX')
        ri = index(rf, rs[2],owner=released['name'],operation='RELEASED_TARGET_INDEX')
        require(len(ri.entries) == released['entries'], 'released entry count')
        available = {(e.offset, e.csize): e for e in ci.entries}
        require(not Counter(effective) - Counter(available.keys()), 'VPL pair absent/ambiguous in current archive')
        effective = set(effective)
        ready_by_name = {e.member: e for e in ri.entries}
        chosen, decisions, applied = [], [], 0
        whole_ready = [e.member for e in ci.entries] == [e.member for e in ri.entries]
        current_names = set()
        for e in ci.entries:
            current_names.add(e.member)
            c = catalog.get(e.member)
            use, action, observed = False, 'CURRENT_UNTARGETED', None
            target = ready_by_name.get(e.member)
            if c is not None:
                require(target is not None, 'whitelisted target missing from verified release')
                if (e.offset, e.csize) not in effective:
                    action = 'CURRENT_NOT_EFFECTIVE'
                else:
                    data = member_data(cf, e)
                    observed = {'bytes': len(data), 'sha256': sha(data)}
                    expected = c['source_dds']
                    if observed['sha256'] != expected['sha256'] or expected['bytes'] is not None and observed['bytes'] != expected['bytes']:
                        action = 'CURRENT_SOURCE_CHANGED'
                    else:
                        dds(data)
                        replacement = member_data(rf, target)
                        dds(replacement)
                        expected_target = c['replacement_dds']
                        if expected_target['sha256'] is not None:
                            require(sha(replacement) == expected_target['sha256'] and len(replacement) == expected_target['bytes'], 'released member contradicts saved target')
                        use, action = True, 'RELEASED_SOURCE_COMPATIBLE'
                        applied += 1
                        observed['target'] = {'bytes': len(replacement), 'sha256': sha(replacement), 'basis': 'COMPLETE_VERIFIED_RELEASE_CONTAINER_AND_LITERAL_MEMBER'}
                        del replacement
                    del data
            chosen.append((use, target if use else e))
            if whole_ready and not use and not equal_raw(cf, e, rf, target):
                whole_ready = False
            decisions.append({'member': e.member, 'action': action, 'observed': observed})
        decisions.extend({'member': m, 'action': 'CURRENT_MEMBER_ABSENT', 'observed': None} for m in sorted(set(catalog)-current_names))
        require(cs[2] <= U32 and int(cs[3]//1_000_000_000) == source_time, 'current source time changed')
        if not applied:
            dest, oi, mode, output_sha, output_stat = Path(current), ci, 'CURRENT_STOCK', current_sha, cs
        elif whole_ready:
            dest, oi, mode, output_sha, output_stat = Path(ready), ri, 'REUSE_COHERENT_RELEASE', ready_sha, rs
        else:
            dest = Path(output)
            partial = dest.with_name(dest.name+'.partial')
            require(not dest.exists() and not partial.exists(), 'derived output exists')
            rows, pos = [], 0
            with partial.open('xb') as out:
                for use, e in chosen:
                    require(pos+e.csize+4 <= U32, 'derived archive exceeds UInt32')
                    copy_raw(rf if use else cf, e, out)
                    rows.append(Entry(e.member, pos, e.csize))
                    pos += e.csize+4
                out.write(footer(rows, pos))
                out.flush()
                os.fsync(out.fileno())
            os.replace(partial, dest)
            with held(dest) as (of, output_stat):
                oi, output_sha = index(of, output_stat[2],owner=released['name'],operation='DERIVED_OUTPUT_INDEX'), stream_hash(of)
            require(oi.entries == tuple(rows), 'derived index differs')
            mode = 'DERIVED_CURRENT_REBASE'
        require([e.member for e in ci.entries] == [e.member for e in oi.entries], 'output/current membership/order')
        mapping = {(a.offset, a.csize): (b.offset, b.csize) for a,b in zip(ci.entries, oi.entries)}
        key = digest({'current': current_sha, 'released': released, 'catalog': catalog, 'engine': sha(Path(__file__).read_bytes())})
        record = {'name': released['name'], 'mode': mode, 'source': {'path': str(current), 'bytes': cs[2], 'sha256': current_sha, 'stat5': cs}, 'released': {'path': str(ready), 'bytes': rs[2], 'sha256': ready_sha, 'stat5': rs}, 'output': {'path': str(dest), 'bytes': output_stat[2], 'sha256': output_sha, 'stat5': output_stat}, 'cache_key': key, 'applied': applied, 'decisions': decisions}
        return record, mapping


def manifest_bytes(routes):
    require(1 <= len(routes) <= 1024 and len({a.casefold() for a,b in routes}) == len(routes), 'route count/aliases')
    result = bytearray(b'TQCFW001'+struct.pack('<II', 1, len(routes)))
    for a,b in routes:
        require('\0' not in a+b, 'manifest NUL')
        av,bv = a.encode('utf-16le'),b.encode('utf-16le')
        require(1 <= len(av)//2 <= 32767 and 1 <= len(bv)//2 <= 32767, 'manifest path bound')
        result.extend(struct.pack('<II',len(av)//2,len(bv)//2)+av+bv)
    require(len(result) <= 4*CHUNK, 'manifest byte bound')
    return bytes(result)


def prepare(game, prepared, output, *, catalog_path=CATALOG_H, catalog_sha=CATALOG_SHA):
    game, prepared, out = plain(game), plain(prepared), plain(output, output=True)
    portable_scopes(game, prepared, out)
    plain(PACKAGE_ROOT)
    require(plain(catalog_path) == CATALOG_H and catalog_sha == CATALOG_SHA, 'portable catalog path/authority')
    require(not out.exists(), 'output directory must be fresh; partial/old runs are preserved')
    out.mkdir(parents=True)
    atomic(out/'INTENT.json', canonical({'schema':'tq-fullpack-intent-v1','game':str(game),'prepared':str(prepared),'catalog':str(catalog_path),'catalog_sha256':catalog_sha,'created':time.time()}))
    try:
        archive_table, catalog, pins = load_catalog(catalog_path, catalog_sha)
        prep, pp = read_json(prepared)
        rows = prepared_rows(prep, prepared, archive_table)
        vplpath = plain(game/'assetslib.vpl')
        with held(vplpath) as (f, vs):
            require(vs[2] <= MAX_VPL, 'VPL size')
            vpl = f.read(MAX_VPL+1)
        words, owners, records, validations = parse_vpl(vpl)
        original_vpl_sha = sha(vpl)
        private = bytearray(vpl)
        routes, providers, unavailable, observations = [], [], [], [{'path':str(vplpath),'bytes':len(vpl),'sha256':original_vpl_sha,'stat5':vs}, pp, *pins]
        estimate_read = sum(a['bytes'] for a in rows)
        print(json.dumps({'phase':'preparation','providers':len(rows),'released_bytes':estimate_read,'estimated_minimum_read_seconds_at_100_MiB_s':round(estimate_read/(100*CHUNK),1),'estimate_qualification':'Disk-read budget only, not completion ETA; current source reads, decompression, comparisons and derived writes add work.'}),flush=True)
        (out/'paks').mkdir()
        (out/'receipts').mkdir()
        for number, release in enumerate(rows):
            owner = release['name'].lower()
            current = plain(game/'paks'/owner)
            if owner not in owners or not current.exists():
                unavailable.append({'name':release['name'],'status':'UNAVAILABLE_FOR_THIS_INSTALLATION','reason':'ABSENT_CURRENT_PROVIDER' if owner not in owners else 'ABSENT_CURRENT_FILE','members':len(catalog.get(owner,{}))})
                print(json.dumps({'provider':owner,'done':number+1,'total':len(rows),'mode':'UNAVAILABLE_FOR_THIS_INSTALLATION'}),flush=True)
                continue
            pid = owners.index(owner)
            s = current.stat()
            require(validations[pid] == (int(s.st_mtime),s.st_size), 'current provider/VPL timestamp or length mismatch: '+owner)
            positions = [i for i,r in enumerate(records) if r[2] == pid]
            pairs = [tuple(records[i][3:5]) for i in positions]
            rec, mapping = merge_provider(current, release['path'], out/'paks'/owner, release, catalog.get(owner,{}), pairs, validations[pid][0])
            rec['cache_epoch'] = {'current_vpl_sha256':original_vpl_sha,'catalog_handoff_sha256':catalog_sha,'prepared_sha256':pp['sha256'],'binding_sha256':digest(rec)}
            atomic(out/'receipts'/(owner+'.json'),canonical(rec))
            providers.append(rec)
            observations.extend([rec['source'], rec['released'], rec['output']])
            if rec['applied']:
                for i in positions:
                    struct.pack_into('<II',private,words[0]+i*20+12,*mapping[tuple(records[i][3:5])])
                osize = rec['output']['bytes']
                otime = rec['output']['stat5'][3]//1_000_000_000
                require(0 <= otime <= U32 and 0 < osize <= U32, 'output validation UInt32')
                struct.pack_into('<II',private,52+pid*8,otime,osize)
                routes.append((str(current),rec['output']['path']))
            print(json.dumps({'provider':owner,'done':number+1,'total':len(rows),'applied':rec['applied'],'mode':rec['mode']}), flush=True)
        final = bytes(private)
        fw, fn, fr, fv = parse_vpl(final)
        changed_pids = {owners.index(p['name'].lower()) for p in providers if p['applied']}
        require(fw == words and fn == owners and final[words[1]:] == vpl[words[1]:], 'VPL unrelated sections changed')
        require(all(a[:3] == b[:3] and (a[2] in changed_pids or a == b) for a,b in zip(records,fr)), 'VPL unrelated row changed')
        require(all(i in changed_pids or a == b for i,(a,b) in enumerate(zip(validations,fv))), 'VPL unrelated validation changed')
        for item in observations:
            require(five(plain(item['path']).stat()) == item['stat5'], 'bound input changed before READY')
        atomic(out/'assetslib.vpl',final)
        private_pin = {'path':str(out/'assetslib.vpl'),'bytes':len(final),'sha256':sha(final),'stat5':five((out/'assetslib.vpl').stat())}
        routes.insert(0,(str(vplpath),str(out/'assetslib.vpl')))
        binary = manifest_bytes(routes)
        pending = Counter(d['action'] for p in providers for d in p['decisions'])
        filebindings = {}
        for b in [*observations,private_pin]:
            prior = filebindings.setdefault(b['path'].casefold(),b)
            require(prior['stat5'] == b['stat5'], 'inconsistent repeated file binding')
        ready = {'schema':'tq-fullpack-ready-v1','game':str(game),'output':str(out),'manifest':{'path':str(out/'READY.bin'),'bytes':len(binary),'sha256':sha(binary)},'source_vpl':{'path':str(vplpath),'bytes':len(vpl),'sha256':original_vpl_sha,'stat5':vs},'private_vpl':private_pin,'prepared':pp,'catalog_inputs':pins,'provider_count':len(rows),'available_provider_count':len(providers),'unavailable':unavailable,'route_count':len(routes),'applied':sum(p['applied'] for p in providers),'decisions':dict(pending),'providers':[{'name':p['name'],'receipt':'receipts/'+p['name'].lower()+'.json','cache_key':p['cache_key'],'mode':p['mode'],'applied':p['applied'],'source':p['source'],'released':p['released'],'output':p['output']} for p in providers],'bound_files':list(filebindings.values()),'routes':[{'original':a,'replacement':b} for a,b in routes],'qualification':'Preparation integrity only, not native lifetime/renderer proof. Changed source members remain current with explicit decisions; absent providers reported unavailable. No automatic generation.'}
        atomic(out/'READY.json',canonical(ready))
        atomic(out/'READY.bin',binary)
        return ready
    except BaseException as exc:
        if not (out/'FAILURE.json').exists():
            atomic(out/'FAILURE.json',canonical({'schema':'tq-fullpack-preparation-failure-v1','error':type(exc).__name__,'reason':str(exc),'details':getattr(exc,'details',{}),'time':time.time(),'ready_published':(out/'READY.bin').exists(),'partial_outputs':'PRESERVED_NO_AUTOMATIC_RETRY'}))
        raise


def verify_ready(path):
    """Explicit launch preflight; one full read of each deduplicated bound FILE."""
    ready, rp = read_json(path)
    out = plain(Path(path).parent, output=True)
    require((out/'INTENT.json').is_file() and not (out/'FAILURE.json').exists(), 'incomplete/failed preparation cannot launch')
    require(ready['schema'] == 'tq-fullpack-ready-v1' and ready['output'] == str(out), 'READY root/schema')
    game = plain(ready['game'])
    portable_scopes(game, plain(ready['prepared']['path']), out)
    plain(PACKAGE_ROOT)
    require(len(ready['catalog_inputs']) == 9 and ready['catalog_inputs'][0]['path'] == str(CATALOG_H)
            and ready['catalog_inputs'][0]['sha256'] == CATALOG_SHA, 'READY portable catalog authority')
    expected_routes = [(str(game/'assetslib.vpl'),str(out/'assetslib.vpl'))]
    for row in ready['providers']:
        require(row['source']['path'] == str(game/'paks'/name(row['name'],True).lower()), 'READY original path')
        if row['applied']:
            dest = plain(row['output']['path'])
            downloads = plain(Path(ready['prepared']['path']).parent/'downloads')
            require(dest.is_relative_to(out) or dest == downloads/row['name'], 'READY replacement outside package/downloads')
            expected_routes.append((row['source']['path'],str(dest)))
    actual_routes = [(r['original'],r['replacement']) for r in ready['routes']]
    require(expected_routes == actual_routes and ready['route_count'] == len(expected_routes), 'READY route correspondence')
    require(ready['manifest']['path'] == str(out/'READY.bin'), 'READY manifest path')
    binary = manifest_bytes(expected_routes)
    require(ready['manifest']['bytes'] == len(binary) and ready['manifest']['sha256'] == sha(binary), 'READY manifest identity')
    with held(out/'READY.bin') as (f,s):
        require(s[2] == len(binary) and f.read(4*CHUNK+1) == binary, 'READY manifest bytes changed')
    expected = [ready['source_vpl'],ready['private_vpl'],ready['prepared'],*ready['catalog_inputs']]
    expected.extend(b for r in ready['providers'] for b in (r['source'],r['released'],r['output']))
    known = {}
    for b in expected:
        prior = known.setdefault(b['path'].casefold(),b)
        require(all(prior[k] == b[k] for k in ('bytes','sha256','stat5')), 'READY repeated file binding')
    bound = {b['path'].casefold():b for b in ready['bound_files']}
    require(len(bound) == len(ready['bound_files']) and set(bound) == set(known), 'READY complete bound set')
    for key,b in known.items():
        require(all(bound[key][k] == b[k] for k in ('bytes','sha256','stat5')), 'READY bound identity correspondence')
        with held(b['path']) as (f,s):
            require(s == b['stat5'] and s[2] == b['bytes'] and stream_hash(f) == b['sha256'], 'READY bound FILE changed: '+b['path'])
    require(five(Path(path).stat()) == rp['stat5'], 'READY metadata changed during verify')
    return {'status':'VERIFIED_READY','manifest':ready['manifest'],'game':ready['game'],'ready_json':rp,'files':len(known),'routes':len(expected_routes),'qualification':'Ordinary stable preparation/preflight read; native launcher must retain replacement FILE handles before activation.'}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--game')
    p.add_argument('--prepared')
    p.add_argument('--output')
    p.add_argument('--verify-ready')
    a = p.parse_args()
    if a.verify_ready:
        require(not any((a.game,a.prepared,a.output)), 'verify and prepare arguments are exclusive')
        print(json.dumps(verify_ready(a.verify_ready)),flush=True)
        return
    require(all((a.game,a.prepared,a.output)), 'prepare requires --game, --prepared and --output')
    ready = prepare(a.game,a.prepared,a.output)
    print(json.dumps({'status':'READY','manifest':ready['manifest'],'applied':ready['applied'],'decisions':ready['decisions']}),flush=True)


if __name__ == '__main__':
    main()
