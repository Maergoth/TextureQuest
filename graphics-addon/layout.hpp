#pragma once
// Texture layout math shared by the map, the swap and the self-check.
#include <algorithm>
#include <cstdint>
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace tq {

struct Layout {
    uint32_t Width = 0, Height = 0, Mips = 0, Family = 0;
    bool operator==(const Layout&) const = default;
};

struct Subresource {
    const void* Data = nullptr;
    uint32_t RowPitch = 0;
};

// Families and their DXGI formats match graphics-native/engine.hpp.
inline uint32_t FamilyOf(uint32_t dxgi_format) {
    switch (dxgi_format) {
    case 70: case 71: case 72: return 1;
    case 73: case 74: case 75: return 2;
    case 76: case 77: case 78: return 3;
    case 87: case 90: case 91: return 4;
    case 88: case 92: case 93: return 5;
    case 60: case 61: return 6;
    case 31: return 7;
    default: return 0;
    }
}

inline void LevelSize(uint32_t family, uint32_t width, uint32_t height, uint32_t mip, uint32_t& row_bytes, uint32_t& rows) {
    const uint32_t w = std::max(1u, width >> mip), h = std::max(1u, height >> mip);
    row_bytes = family <= 3 ? ((w + 3) / 4) * (family == 1 ? 8 : 16) : w * (family == 6 ? 1 : 4);
    rows = family <= 3 ? (h + 3) / 4 : h;
}

inline XXH128_hash_t Fingerprint(const Layout& layout, const Subresource* levels) {
    XXH3_state_t state;
    XXH3_128bits_reset(&state);
    for (uint32_t mip = 0; mip < layout.Mips; ++mip) {
        uint32_t row_bytes, rows;
        LevelSize(layout.Family, layout.Width, layout.Height, mip, row_bytes, rows);
        const auto* bytes = static_cast<const unsigned char*>(levels[mip].Data);
        if (levels[mip].RowPitch == row_bytes) {
            XXH3_128bits_update(&state, bytes, size_t(row_bytes) * rows);
            continue;
        }
        for (uint32_t y = 0; y < rows; ++y)
            XXH3_128bits_update(&state, bytes + size_t(y) * levels[mip].RowPitch, row_bytes);
    }
    return XXH3_128bits_digest(&state);
}

}
