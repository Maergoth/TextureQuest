"""Texture decode/encode for the EQ2 DDS variants found in the inventory, plus TGA/PNG.

Decode produces a `Texture` with pixels as numpy arrays:
  - colour / gray formats: uint8, shape (H, W, C) with C in {1, 3, 4}, RGB(A) channel order
  - QWVU: float32, shape (H, W, 4), values in [-1, 1], channel order (U, V, W, Q)
Only the top mip level is decoded. The original 128-byte header and the exporter trailer
(bytes after the last mip) are kept so the encoder can write an identical header with new
dimensions and re-append the trailer.

Compressed formats (DXT1/3/5) are decoded with Pillow and encoded with texconv (DirectXTex).
Uncompressed formats are handled entirely in numpy so the pixel-format block is preserved bit-for-bit.
"""
from __future__ import annotations

import io
import os
import struct
import subprocess
import tempfile
from dataclasses import dataclass, field
from typing import Optional, Tuple

import numpy as np
from PIL import Image

from texturequest_local_ddsinfo import parse_dds, DdsInfo

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
CODEC_DIR = os.path.join(TOOLS_DIR, 'cpu-codec')
if not os.path.isfile(os.path.join(CODEC_DIR, 'texconv.exe')):
    CODEC_DIR = os.path.join(os.path.dirname(TOOLS_DIR), 'cpu-codec')
TEXCONV = os.path.join(CODEC_DIR, 'texconv.exe')
DEFAULT_TMP = os.path.join(os.path.dirname(TOOLS_DIR), "work", "tmp")   # scratch on the project drive (the C: drive is nearly full)


def _tmp_root(workdir):
    if workdir:
        return workdir
    os.makedirs(DEFAULT_TMP, exist_ok=True)
    return DEFAULT_TMP

BC_BYTES_PER_BLOCK = {"DXT1": 8, "DXT3": 16, "DXT5": 16}
BC_DXGI = {"DXT1": "BC1_UNORM", "DXT3": "BC2_UNORM", "DXT5": "BC3_UNORM"}


class TexError(Exception):
    pass


@dataclass
class Texture:
    kind: str                     # 'dds' | 'tga' | 'png'
    fmt: str                      # ddsinfo fmt string, or 'TGA'/'PNG'
    width: int
    height: int
    mips: int
    pixels: np.ndarray            # see module docstring
    header: bytes = b""           # original DDS header (128 bytes) if dds
    trailer: bytes = b""          # bytes after the last mip level (exporter footer)
    info: Optional[DdsInfo] = None
    pil_mode: str = ""            # for tga/png round trip
    notes: list = field(default_factory=list)

    @property
    def channels(self) -> int:
        return self.pixels.shape[2] if self.pixels.ndim == 3 else 1


# ----------------------------------------------------------------------------- sizes

def mip_dims(w: int, h: int, level: int) -> Tuple[int, int]:
    return max(1, w >> level), max(1, h >> level)


def level_bytes(fmt: str, w: int, h: int, bits: int) -> int:
    if fmt in BC_BYTES_PER_BLOCK:
        return ((w + 3) // 4) * ((h + 3) // 4) * BC_BYTES_PER_BLOCK[fmt]
    if fmt == "QWVU":
        return w * h * 4
    return w * h * (bits // 8)


def pixel_data_size(fmt: str, w: int, h: int, mips: int, bits: int) -> int:
    return sum(level_bytes(fmt, *mip_dims(w, h, i), bits) for i in range(max(1, mips)))


def full_mip_count(w: int, h: int) -> int:
    n = 1
    while max(w, h) > 1:
        w, h = max(1, w >> 1), max(1, h >> 1)
        n += 1
    return n


# ----------------------------------------------------------------------------- decode

def _unpack_masked(raw: np.ndarray, masks: Tuple[int, int, int, int]) -> np.ndarray:
    """raw: uint32 array (H, W). masks: (r, g, b, a). Returns uint8 (H, W, 3 or 4)."""
    nz = [m for m in masks if m]
    for i in range(len(nz)):
        for j in range(i + 1, len(nz)):
            if nz[i] & nz[j]:
                raise TexError(f"malformed pixel format: overlapping channel masks {[hex(m) for m in masks]}")
    chans = []
    for m in masks:
        if m == 0:
            chans.append(None)
            continue
        shift = (m & -m).bit_length() - 1
        width = bin(m >> shift).count("1")
        v = (raw >> shift) & ((1 << width) - 1)
        if width < 8:
            v = (v * 255 + (1 << width) // 2) // ((1 << width) - 1)  # scale to 0..255
        elif width > 8:
            v = v >> (width - 8)
        chans.append(v.astype(np.uint8))
    r, g, b, a = chans
    if r is None or g is None or b is None:
        raise TexError("masked format without full RGB")
    if a is None:
        return np.stack([r, g, b], axis=-1)
    return np.stack([r, g, b, a], axis=-1)


def decode_dds(data: bytes, name: str = "") -> Texture:
    info = parse_dds(data[:148])
    if info is None:
        raise TexError("not a DDS or unsupported header")
    if info.header_size != 128:
        raise TexError("DX10 header not supported by policy")
    if info.cubemap:
        raise TexError("cubemap")
    if info.volume:
        raise TexError("volume texture")
    fmt, w, h, mips, bits = info.fmt, info.width, info.height, info.mipmaps, info.bitcount
    if fmt in ("FRES", "MASK"):
        raise TexError(f"custom EQ2 format {fmt}")
    body = data[128:]
    header = data[:128]

    if fmt in BC_BYTES_PER_BLOCK:
        need = pixel_data_size(fmt, w, h, mips, 0)
        if len(body) < level_bytes(fmt, w, h, 0):
            raise TexError("truncated BC data")
        trailer = body[need:] if len(body) >= need else b""
        # Pillow decodes the top level; it needs a well-formed header, so hand it the file bytes.
        im = Image.open(io.BytesIO(data))
        im.load()
        if im.mode not in ("RGB", "RGBA"):
            im = im.convert("RGBA")
        px = np.asarray(im)
        if px.shape[0] != h or px.shape[1] != w:
            raise TexError(f"decoded size {px.shape[1]}x{px.shape[0]} != header {w}x{h}")
        if fmt == "DXT1" and px.shape[2] == 4 and px[..., 3].min() == 255:
            px = px[..., :3]
        return Texture("dds", fmt, w, h, mips, np.ascontiguousarray(px), header, trailer, info)

    if fmt == "QWVU":
        need = pixel_data_size(fmt, w, h, mips, 32)
        if len(body) < w * h * 4:
            raise TexError("truncated QWVU data")
        raw = np.frombuffer(body, dtype=np.int8, count=w * h * 4).reshape(h, w, 4)
        px = (raw.astype(np.float32) / 127.0).clip(-1.0, 1.0)
        return Texture("dds", fmt, w, h, mips, px, header, body[need:], info)

    if bits in (8, 16, 24, 32) and not info.fourcc:
        bpp = bits // 8
        need = pixel_data_size(fmt, w, h, mips, bits)
        if len(body) < w * h * bpp:
            raise TexError("truncated raw data")
        trailer = body[need:] if len(body) >= need else b""
        top = body[: w * h * bpp]
        masks = (info.rmask, info.gmask, info.bmask, info.amask)
        if bits == 8 and masks == (0, 0, 0, 0):
            px = np.frombuffer(top, dtype=np.uint8).reshape(h, w, 1)
        elif bits == 8 and info.fmt == "L8":
            px = np.frombuffer(top, dtype=np.uint8).reshape(h, w, 1)
        elif bits == 16:
            raw = np.frombuffer(top, dtype="<u2").reshape(h, w).astype(np.uint32)
            px = _unpack_masked(raw, masks)
        elif bits == 24:
            raw = np.frombuffer(top, dtype=np.uint8).reshape(h, w, 3)
            # masks tell the byte order; common case R=0xff0000 => bytes B,G,R
            if masks[0] == 0xFF0000:
                px = raw[..., ::-1].copy()
            else:
                px = raw.copy()
        elif bits == 32:
            raw = np.frombuffer(top, dtype="<u4").reshape(h, w)
            px = _unpack_masked(raw, masks)
        else:
            raise TexError(f"unsupported raw format {fmt}")
        return Texture("dds", fmt, w, h, mips, np.ascontiguousarray(px), header, trailer, info)

    raise TexError(f"unsupported format {fmt}")


def decode_image_file(data: bytes, ext: str) -> Texture:
    im = Image.open(io.BytesIO(data))
    im.load()
    mode = im.mode
    if mode not in ("L", "RGB", "RGBA"):
        im = im.convert("RGBA")
    px = np.asarray(im)
    if px.ndim == 2:
        px = px[..., None]
    t = Texture(ext.lstrip(".").lower(), ext.lstrip(".").upper(), im.width, im.height, 1, np.ascontiguousarray(px))
    t.pil_mode = mode
    return t


def decode(data: bytes, name: str) -> Texture:
    ext = name.rsplit(".", 1)[-1].lower() if "." in name else ""
    if data[:4] == b"DDS ":
        return decode_dds(data, name)
    if ext in ("tga", "png"):
        return decode_image_file(data, "." + ext)
    raise TexError(f"not an image: {name}")


# ----------------------------------------------------------------------------- encode helpers

def rewrite_header(header: bytes, w: int, h: int, mips: int, fmt: str, bits: int) -> bytes:
    """Copy the source header, changing only height/width/pitchOrLinearSize/mipMapCount.
    pitchOrLinearSize keeps 0 if the source had 0, otherwise is recomputed for the new size."""
    hb = bytearray(header)
    struct.pack_into("<I", hb, 12, h)
    struct.pack_into("<I", hb, 16, w)
    old_pitch = struct.unpack_from("<I", header, 20)[0]
    if old_pitch:
        if fmt in BC_BYTES_PER_BLOCK:
            struct.pack_into("<I", hb, 20, level_bytes(fmt, w, h, 0))
        else:
            struct.pack_into("<I", hb, 20, w * (32 if fmt == "QWVU" else bits) // 8)
    flags = struct.unpack_from("<I", header, 8)[0]
    old_mips = struct.unpack_from("<I", header, 28)[0]
    if flags & 0x20000 or old_mips:
        struct.pack_into("<I", hb, 28, mips)
    return bytes(hb)


def _downsample_box(px: np.ndarray) -> np.ndarray:
    """2x box downsample of (H, W, C) with odd-size handling (edge replicate)."""
    h, w, c = px.shape
    if h > 1 and h % 2:
        px = np.concatenate([px, px[-1:]], axis=0)
    if w > 1 and w % 2:
        px = np.concatenate([px, px[:, -1:]], axis=1)
    h2, w2 = px.shape[0] // 2 if px.shape[0] > 1 else 1, px.shape[1] // 2 if px.shape[1] > 1 else 1
    if px.shape[0] == 1 and px.shape[1] == 1:
        return px
    f = px.astype(np.float32)
    if px.shape[0] > 1:
        f = (f[0::2] + f[1::2]) * 0.5
    if px.shape[1] > 1:
        f = (f[:, 0::2] + f[:, 1::2]) * 0.5
    return f


def build_mip_chain_raw(px: np.ndarray, mips: int, renormalize_xyz: bool = False) -> list:
    """Return list of arrays (float32 or uint8 like input) for each mip level."""
    chain = [px]
    cur = px.astype(np.float32)
    for _ in range(1, mips):
        cur = _downsample_box(cur)
        if renormalize_xyz:
            n = np.linalg.norm(cur[..., :3], axis=-1, keepdims=True)
            n[n == 0] = 1.0
            cur = cur.copy()
            cur[..., :3] /= n
        chain.append(cur)
    return chain


def _pack_masked(px: np.ndarray, masks: Tuple[int, int, int, int], bits: int) -> bytes:
    h, w = px.shape[:2]
    out = np.zeros((h, w), dtype=np.uint32)
    for ci, m in enumerate(masks):
        if m == 0:
            continue
        if ci >= px.shape[2]:
            v = np.full((h, w), 255, dtype=np.uint32)
        else:
            v = px[..., ci].astype(np.uint32)
        shift = (m & -m).bit_length() - 1
        width = bin(m >> shift).count("1")
        if width < 8:
            v = (v * ((1 << width) - 1) + 127) // 255
        elif width > 8:
            v = v << (width - 8)
        out |= (v << shift) & np.uint32(m)
    if bits == 16:
        return out.astype("<u2").tobytes()
    if bits == 32:
        return out.astype("<u4").tobytes()
    raise TexError("pack: bits")


def encode_raw_dds(t: Texture, px: np.ndarray, mips: int) -> bytes:
    """Encode uncompressed / QWVU pixels with the source's pixel format block."""
    info = t.info
    h, w = px.shape[:2]
    fmt, bits = t.fmt, info.bitcount
    out = bytearray(rewrite_header(t.header, w, h, mips, fmt, bits))
    if fmt == "QWVU":
        chain = build_mip_chain_raw(px.astype(np.float32), mips, renormalize_xyz=True)
        for lv in chain:
            q = np.rint(np.clip(lv, -1.0, 1.0) * 127.0).astype(np.int8)
            out += q.tobytes()
    else:
        chain = build_mip_chain_raw(px, mips)
        masks = (info.rmask, info.gmask, info.bmask, info.amask)
        for lv in chain:
            q = np.rint(np.clip(lv, 0, 255)).astype(np.uint8) if lv.dtype != np.uint8 else lv
            if bits == 8:
                out += q[..., 0].tobytes()
            elif bits == 24:
                out += (q[..., ::-1] if masks[0] == 0xFF0000 else q).tobytes()
            else:
                out += _pack_masked(q, masks, bits)
    out += t.trailer
    return bytes(out)


def _run(cmd: list, timeout: int = 600) -> str:
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if p.returncode != 0:
        raise TexError(f"texconv failed ({p.returncode}): {' '.join(cmd)}\n{p.stdout}\n{p.stderr}")
    return p.stdout


def encode_bc_dds(t: Texture, px: np.ndarray, mips: int, tileable: bool = False,
                  keep_coverage: Optional[float] = None, uniform_weights: bool = False,
                  workdir: Optional[str] = None, mip_filter: Optional[str] = None) -> bytes:
    """Encode RGB(A) uint8 pixels to the source's DXT fourCC via texconv, then splice the
    BC data under a header copied from the source (only dims/mips/pitch changed)."""
    fmt = t.fmt
    if fmt not in BC_DXGI:
        raise TexError(f"encode_bc: not a BC format {fmt}")
    h, w = px.shape[:2]
    if px.shape[2] == 3:
        px = np.concatenate([px, np.full((h, w, 1), 255, np.uint8)], axis=-1)
    wd = tempfile.mkdtemp(prefix="tc_", dir=_tmp_root(workdir))
    try:
        src = os.path.join(wd, "in.png")
        Image.fromarray(px, "RGBA").save(src, compress_level=1)
        # -sepalpha: filter the alpha channel separately when building mips. texconv's default resize premultiplies by
        # alpha, so the colour under low-alpha texels (tint masks, spec-in-alpha armour, cutouts) collapses to black in
        # every level below the top: "white up close, black at a distance" in game (2026-09-17).
        cmd = [TEXCONV, "-nologo", "-y", "-dx9", "-sepalpha", "-f", BC_DXGI[fmt], "-m", str(mips), "-o", wd, "-ft", "dds"]
        if mip_filter is not None:
            if mip_filter != "BOX":
                raise TexError("only the audited BOX mip filter is supported here")
            cmd += ["-if", mip_filter]
        if tileable:
            cmd.append("-wrap")
        if keep_coverage is not None:
            cmd += ["-keepcoverage", f"{keep_coverage:.3f}"]
        if uniform_weights:
            cmd += ["-bc", "u"]
        cmd.append(src)
        _run(cmd)
        with open(os.path.join(wd, "in.dds"), "rb") as f:
            outdds = f.read()
    finally:
        for fn in os.listdir(wd):
            try:
                os.remove(os.path.join(wd, fn))
            except OSError:
                pass
        try:
            os.rmdir(wd)
        except OSError:
            pass
    oi = parse_dds(outdds[:148])
    if oi is None or oi.fmt != fmt or oi.width != w or oi.height != h:
        raise TexError(f"texconv produced unexpected header {oi}")
    if oi.mipmaps != mips:
        raise TexError(f"texconv produced {oi.mipmaps} mips, wanted {mips}")
    need = pixel_data_size(fmt, w, h, mips, 0)
    bc = outdds[oi.header_size: oi.header_size + need]
    if len(bc) != need:
        raise TexError("texconv output shorter than expected")
    return rewrite_header(t.header, w, h, mips, fmt, 0) + bc + t.trailer


def encode_image_file(t: Texture, px: np.ndarray) -> bytes:
    mode = "RGBA" if px.shape[2] == 4 else ("RGB" if px.shape[2] == 3 else "L")
    im = Image.fromarray(px[..., 0] if mode == "L" else px, mode)
    buf = io.BytesIO()
    if t.kind == "tga":
        im.save(buf, format="TGA")
    else:
        im.save(buf, format="PNG", compress_level=6)
    return buf.getvalue()


def texconv_decode_png(data: bytes, workdir: Optional[str] = None) -> np.ndarray:
    """Independent decoder (for validation): DDS bytes -> RGBA uint8 via texconv."""
    wd = tempfile.mkdtemp(prefix="td_", dir=_tmp_root(workdir))
    try:
        src = os.path.join(wd, "v.dds")
        with open(src, "wb") as f:
            f.write(data)
        _run([TEXCONV, "-nologo", "-y", "-m", "1", "-f", "R8G8B8A8_UNORM", "-ft", "png", "-o", wd, src])
        im = Image.open(os.path.join(wd, "v.png")); im.load()
        return np.asarray(im.convert("RGBA"))
    finally:
        for fn in os.listdir(wd):
            try: os.remove(os.path.join(wd, fn))
            except OSError: pass
        try: os.rmdir(wd)
        except OSError: pass
