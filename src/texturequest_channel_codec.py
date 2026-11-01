"""RGB-only transport; original alpha/header/tail are reconstructed locally.

The four profiles below cover the currently released RGB corrections. Scalar,
joint and signed-channel generations require their own explicit profiles.
"""
import hashlib
import json
from pathlib import Path
import struct
import sys
import numpy as np

MAGIC = b'TQCH0001'
HEADER = struct.Struct('<8s4I')
PROFILES = {1: 'BC1_RGB_LOCAL_ALPHA', 2: 'BC2_RGB_LOCAL_ALPHA',
            3: 'BC3_RGB_LOCAL_ALPHA', 4: 'BGRA8_RGB_LOCAL_ALPHA'}


def need(ok, message):
    if not ok:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def dependencies(project):
    import texturequest_local_texcodec as codec
    import texturequest_local_resample as upscale
    return codec, upscale


def transparency(pixels):
    blocks = np.frombuffer(pixels, dtype='<u2').reshape(-1, 4)
    indices = blocks[:, 2].astype(np.uint32) | (blocks[:, 3].astype(np.uint32) << 16)
    alpha = np.zeros(indices.shape, dtype=np.uint32)
    for shift in range(0, 32, 2):
        alpha |= ((((indices >> shift) & 3) == 3) & (blocks[:, 0] <= blocks[:, 1])).astype(np.uint32) << shift
    return alpha


def protected(family, pixels):
    if family == 1:
        return transparency(pixels).tobytes()
    x = np.frombuffer(pixels, np.uint8)
    if family in (2, 3):
        return x.reshape(-1, 16)[:, :8].tobytes()
    need(family == 4, 'Unsupported generated-channel profile')
    return x[3::4].tobytes()


def extract(layout, pixels):
    w, h, mips, family, representation = layout
    need(representation == 1 and family in PROFILES, 'Unsupported generated-channel profile')
    x = np.frombuffer(pixels, np.uint8)
    if family == 1:
        blocks = np.frombuffer(pixels, dtype='<u4').reshape(-1, 2).copy()
        # Zero every transparency selector; the local alpha recipe restores it.
        blocks[:, 1] &= ~(transparency(pixels) * np.uint32(3))
        body = blocks.tobytes()
    elif family in (2, 3):
        body = x.reshape(-1, 16)[:, 8:].tobytes()
    else:
        body = x.reshape(-1, 4)[:, :3].tobytes()
    return HEADER.pack(MAGIC, w, h, mips, family) + body


def combine(payload, layout, baseline, rebuild_alpha=False):
    need(len(payload) >= HEADER.size, 'Truncated generated channels')
    magic, w, h, mips, family = HEADER.unpack_from(payload)
    need(magic == MAGIC and [w, h, mips, family, 1] == list(layout) and family in PROFILES,
         'Generated-channel layout differs')
    body = payload[HEADER.size:]
    if family == 1:
        need(len(body) == len(baseline), 'BC1 channel length differs')
        need(not np.any(transparency(body)), 'Transport contains protected BC1 transparency')
        blocks = np.frombuffer(body, dtype='<u4').reshape(-1, 2).copy()
        masks = transparency(baseline)
        endpoints = np.frombuffer(body, dtype='<u2').reshape(-1, 4)
        conflicts = (masks != 0) & (endpoints[:, 0] > endpoints[:, 1])
        need(rebuild_alpha or not np.any(conflicts), 'BC1 alpha/color mode differs')
        # Only a legacy block incompatible with the local transparency mask
        # needs colour requantization. Keep its generated endpoints, reverse
        # their order for the three-colour mode, and choose nearest colours.
        for n in np.flatnonzero(conflicts):
            c0,c1=map(int,endpoints[n,:2])
            def rgb(value):
                r,g,b=(value>>11)&31,(value>>5)&63,value&31
                return np.array([(r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2)],np.int32)
            a,b=rgb(c0),rgb(c1)
            old=np.stack((a,b,(2*a+b)//3,(a+2*b)//3))
            new=np.stack((b,a,(a+b)//2))
            nearest=np.argmin(((old[:,None,:]-new[None,:,:])**2).sum(axis=2),axis=1)
            indices=int(blocks[n,1]);replacement=0
            for shift in range(0,32,2):replacement |= int(nearest[(indices>>shift)&3]) << shift
            blocks[n,0]=np.uint32(c1 | (c0<<16));blocks[n,1]=np.uint32(replacement)
        blocks[:, 1] = (blocks[:, 1] & ~(masks * np.uint32(3))) | (masks * np.uint32(3))
        return blocks.tobytes()
    original = np.frombuffer(baseline, np.uint8)
    if family in (2, 3):
        blocks = original.reshape(-1, 16).copy()
        need(len(body) == len(blocks) * 8, 'BC color channel length differs')
        blocks[:, 8:] = np.frombuffer(body, np.uint8).reshape(-1, 8)
    else:
        blocks = original.reshape(-1, 4).copy()
        need(len(body) == len(blocks) * 3, 'Raw color channel length differs')
        blocks[:, :3] = np.frombuffer(body, np.uint8).reshape(-1, 3)
    return blocks.tobytes()


class LocalAssembler:
    def __init__(self, project, scratch, catalog):
        self.codec, self.upscale = dependencies(project)
        self.scratch = Path(scratch)
        self.scratch.mkdir(parents=True, exist_ok=True)
        self.catalog = catalog
        self.constant_blocks = {}

    def baseline(self, original, target_layout, wrap):
        """Reproduce the released alpha recipe from the player's source only."""
        need(type(wrap) is list and len(wrap) == 2 and all(type(x) is bool for x in wrap), 'Missing exact wrap recipe')
        source_layout, _, _ = self.catalog.decode_dds(original)
        w, h, mips, family, representation = target_layout
        need(family in PROFILES and source_layout[3:] == tuple(target_layout[3:]), 'Source channel profile differs')
        factor = w // source_layout[0]
        need(factor in (1, 2, 4, 8, 16, 32, 64) and max(w, h) <= 4096 and
             w == source_layout[0] * factor and h == source_layout[1] * factor,
             'Unsupported alpha resize geometry')
        texture = self.codec.decode(original, 'local.dds')
        rgba = np.asarray(texture.pixels)
        if family == 1 and rgba.dtype == np.uint8 and rgba.shape == (texture.height, texture.width, 3):
            rgba = np.concatenate((rgba, np.full((*rgba.shape[:2], 1), 255, np.uint8)), axis=2)
        need(rgba.dtype == np.uint8 and rgba.shape == (texture.height, texture.width, 4), 'Unproven source alpha decode')
        alpha = rgba[..., 3]
        constant = int(alpha.flat[0]) if np.all(alpha == alpha.flat[0]) else None
        header = self.codec.rewrite_header(texture.header, w, h, mips, texture.fmt, texture.info.bitcount)
        if constant is not None:
            levels = self.catalog.mip_levels(tuple(target_layout))
            if family == 4:
                pixels = bytes((128, 128, 128, constant)) * (sum(v[4] for v in levels) // 4)
            elif family == 1 and constant == 255:
                pixels = bytes(8) * (sum(v[4] for v in levels) // 8)
            else:
                key = (family, constant)
                if key not in self.constant_blocks:
                    px = np.full((4, 4, 4), 128, np.uint8)
                    px[..., 3] = constant
                    raw = self.codec.encode_bc_dds(texture, px, 1, workdir=str(self.scratch), mip_filter='BOX')
                    self.constant_blocks[key] = self.catalog.decode_dds(raw)[1]
                block = self.constant_blocks[key]
                pixels = block * (sum(v[4] for v in levels) // len(block))
            return header, pixels, texture.trailer
        reference = rgba.copy() if factor == 1 else self.upscale.resample(rgba, factor, 'lanczos', tuple(wrap))
        reference[..., :3] = 128
        raw = (self.codec.encode_raw_dds(texture, reference, mips) if family == 4 else
               self.codec.encode_bc_dds(texture, reference, mips, workdir=str(self.scratch), mip_filter='BOX'))
        _, pixels, tail = self.catalog.decode_dds(raw)
        return raw[:128], pixels, tail

    def assemble(self, original, payload, recipe):
        need(sha(original) == recipe['source']['dds_sha256'], 'Current game source differs; leave original')
        layout, pixels, _ = self.catalog.decode_dds(original)
        need(list(layout) == recipe['source']['layout'] and
             self.catalog.fingerprint(layout, pixels) == recipe['source']['fingerprint'], 'Current game pixels differ')
        need(sha(payload) == recipe['payload']['sha256'] and len(payload) == recipe['payload']['bytes'], 'Generated payload checksum differs')
        header, neutral, tail = self.baseline(original, recipe['target']['layout'], recipe['wrap'])
        policy=recipe.get('local_alpha_policy','EXACT_LEGACY_V1')
        need(policy in ('EXACT_LEGACY_V1','SOURCE_REFERENCE_LANCZOS4_BOX_V1'),'Unknown local alpha policy')
        pixels = combine(payload, recipe['target']['layout'], neutral,
                         rebuild_alpha=policy=='SOURCE_REFERENCE_LANCZOS4_BOX_V1')
        if policy=='SOURCE_REFERENCE_LANCZOS4_BOX_V1':
            need(protected(recipe['target']['layout'][3],pixels)==protected(recipe['target']['layout'][3],neutral),
                 'Rebuilt transparency differs from local source reference')
        result = header + pixels + tail
        need(sha(result) == recipe['object_sha256'], 'Local reconstruction differs from released correction')
        return result

    def source_reference_recipe(self, original, payload, row, wrap):
        """Explicit new output: retain colour, restore the current stock alpha recipe."""
        need(sha(original)==row['source']['dds_sha256'],'Current game source differs')
        header,neutral,tail=self.baseline(original,row['target']['layout'],wrap)
        pixels=combine(payload,row['target']['layout'],neutral,rebuild_alpha=True)
        family=row['target']['layout'][3]
        need(protected(family,pixels)==protected(family,neutral),'Local stock alpha changed')
        result=header+pixels+tail
        recipe=make_recipe(row,wrap,payload)
        recipe.update(reference_dds_sha256=row['object_sha256'],object_sha256=sha(result),
                      local_alpha_policy='SOURCE_REFERENCE_LANCZOS4_BOX_V1',
                      qualification='New local source-alpha reference; not byte-identical legacy alpha. RGB block mode may be requantized where BC1 transparency requires it.')
        recipe['target']=dict(layout=row['target']['layout'],fingerprint=self.catalog.fingerprint(tuple(row['target']['layout']),pixels),
                              mip_bytes=len(pixels),mip_sha256=sha(pixels),tail_bytes=len(tail),tail_sha256=sha(tail))
        need(self.assemble(original,payload,recipe)==result,'Local reference replay differs')
        return recipe,result


def make_recipe(row, wrap, payload):
    return dict(id=row['id'], owner=row['owner'], member=row['member'],
                category=row['category'], source=row['source'], target=row['target'],
                object_sha256=row['object_sha256'], wrap=wrap,
                profile=PROFILES[row['target']['layout'][3]],
                generated_channels=['R', 'G', 'B'], local_game_channels=['A'],
                payload=dict(sha256=sha(payload), bytes=len(payload), entry='channels/' + sha(payload) + '.tqc'))
