"""Minimal DDS header parser (no pixel decoding). Works on the first 148 bytes."""
from __future__ import annotations
import struct
from dataclasses import dataclass, asdict
from typing import Optional

DDSD_MIPMAPCOUNT = 0x20000
DDPF_ALPHAPIXELS = 0x1
DDPF_ALPHA = 0x2
DDPF_FOURCC = 0x4
DDPF_RGB = 0x40
DDPF_LUMINANCE = 0x20000
DDSCAPS2_CUBEMAP = 0x200
DDSCAPS2_VOLUME = 0x200000

DXGI_NAMES = {
    71: "BC1_UNORM", 72: "BC1_UNORM_SRGB", 74: "BC2_UNORM", 75: "BC2_UNORM_SRGB",
    77: "BC3_UNORM", 78: "BC3_UNORM_SRGB", 80: "BC4_UNORM", 81: "BC4_SNORM",
    83: "BC5_UNORM", 84: "BC5_SNORM", 95: "BC6H_UF16", 96: "BC6H_SF16",
    98: "BC7_UNORM", 99: "BC7_UNORM_SRGB", 28: "R8G8B8A8_UNORM", 29: "R8G8B8A8_UNORM_SRGB",
    87: "B8G8R8A8_UNORM", 88: "B8G8R8X8_UNORM", 61: "R8_UNORM", 49: "R8G8_UNORM",
}


@dataclass
class DdsInfo:
    width: int
    height: int
    depth: int
    mipmaps: int            # 1 when no mip chain is declared
    fmt: str                # e.g. DXT1, DXT5, RGBA32, RGB24, L8, A8L8, DX10:BC7_UNORM
    fourcc: str
    bitcount: int
    has_alpha_flag: bool    # DDPF_ALPHAPIXELS or DDPF_ALPHA
    rmask: int
    gmask: int
    bmask: int
    amask: int
    cubemap: bool
    volume: bool
    header_size: int        # 128 or 148
    dxgi: int = 0

    def as_dict(self):
        return asdict(self)


def _uncompressed_name(bits: int, r: int, g: int, b: int, a: int, flags: int) -> str:
    if flags & DDPF_LUMINANCE:
        if bits == 8: return "L8"
        if bits == 16 and a: return "A8L8"
        if bits == 16: return "L16"
        return f"LUM{bits}"
    if flags & DDPF_ALPHA and not (flags & DDPF_RGB):
        return f"A{bits}"
    if bits == 32:
        if (r, g, b, a) == (0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000): return "A8R8G8B8"
        if (r, g, b, a) == (0x00ff0000, 0x0000ff00, 0x000000ff, 0): return "X8R8G8B8"
        if (r, g, b, a) == (0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000): return "A8B8G8R8"
        if (r, g, b, a) == (0x000000ff, 0x0000ff00, 0x00ff0000, 0): return "X8B8G8R8"
        return f"RGB32(r={r:08x},g={g:08x},b={b:08x},a={a:08x})"
    if bits == 24:
        return "R8G8B8" if (r, g, b) == (0xff0000, 0xff00, 0xff) else f"RGB24(r={r:x},g={g:x},b={b:x})"
    if bits == 16:
        if (r, g, b, a) == (0xf800, 0x07e0, 0x001f, 0): return "R5G6B5"
        if (r, g, b, a) == (0x7c00, 0x03e0, 0x001f, 0x8000): return "A1R5G5B5"
        if (r, g, b, a) == (0x0f00, 0x00f0, 0x000f, 0xf000): return "A4R4G4B4"
        if (r, g, b, a) == (0x7c00, 0x03e0, 0x001f, 0): return "X1R5G5B5"
        return f"RGB16(r={r:x},g={g:x},b={b:x},a={a:x})"
    if bits == 8:
        return f"RGB8(r={r:x},g={g:x},b={b:x},a={a:x})"
    return f"UNC{bits}"


def parse_dds(head: bytes) -> Optional[DdsInfo]:
    if len(head) < 128 or head[:4] != b"DDS ":
        return None
    (hsize, flags, height, width, pitch, depth, mips) = struct.unpack_from("<7I", head, 4)
    if hsize != 124:
        return None
    (pfsize, pfflags, fourcc_raw, bits, rm, gm, bm, am) = struct.unpack_from("<II4sIIIII", head, 76)
    caps1, caps2 = struct.unpack_from("<II", head, 108)
    fourcc = fourcc_raw.decode("latin-1") if pfflags & DDPF_FOURCC else ""
    header_size = 128
    dxgi = 0
    if fourcc == "DX10":
        if len(head) < 148:
            return None
        dxgi = struct.unpack_from("<I", head, 128)[0]
        header_size = 148
        fmt = "DX10:" + DXGI_NAMES.get(dxgi, str(dxgi))
    elif fourcc:
        fmt = fourcc.strip("\0") or f"FOURCC:{fourcc_raw.hex()}"
        if fourcc_raw in (b"\x74\x00\x00\x00",): fmt = "A32B32G32R32F"
        elif fourcc_raw in (b"\x71\x00\x00\x00",): fmt = "A16B16G16R16F"
        elif fourcc_raw in (b"\x24\x00\x00\x00",): fmt = "A16B16G16R16"
    else:
        fmt = _uncompressed_name(bits, rm, gm, bm, am, pfflags)
    mipcount = mips if (flags & DDSD_MIPMAPCOUNT and mips > 0) else 1
    return DdsInfo(width, height, max(depth, 1), mipcount, fmt, fourcc.strip("\0"), bits,
                   bool(pfflags & (DDPF_ALPHAPIXELS | DDPF_ALPHA)), rm, gm, bm, am,
                   bool(caps2 & DDSCAPS2_CUBEMAP), bool(caps2 & DDSCAPS2_VOLUME), header_size, dxgi)
