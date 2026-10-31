#pragma once
// Reader for the owner-published map (format: src/texturequest_texture_map.py).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "layout.hpp"
#include <bit>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace tq {

#pragma pack(push, 1)
struct MapRow {
    uint64_t KeyLow, KeyHigh;
    uint32_t Width, Height, Mips, Family;
    uint32_t TargetWidth, TargetHeight, TargetMips, TargetFamily;
    uint32_t Archive, CompressedBytes;
    uint64_t BlockOffset;
    uint32_t InflatedBytes, PixelOffset;
    uint64_t PixelBytes;
    uint32_t StockArchive, StockMember;
};
#pragma pack(pop)
static_assert(sizeof(MapRow) == 88);

struct MapArchive {
    std::wstring Name;
    uint64_t Bytes = 0;
};

struct LayoutHash {
    size_t operator()(const Layout& l) const { return (size_t(l.Width) << 40) ^ (size_t(l.Height) << 16) ^ (l.Mips << 4) ^ l.Family; }
};

class TextureMap {
public:
    std::vector<MapArchive> Archives;
    std::vector<std::wstring> StockArchives;
    std::vector<MapRow> Rows;
    std::vector<char> Strings;
    std::unordered_set<Layout, LayoutHash> Layouts;

    bool Load(const std::vector<unsigned char>& raw) {
        constexpr size_t HEADER = 68;
        if (raw.size() < HEADER || std::memcmp(raw.data(), "TQMAP001", 8))
            return false;
        uint32_t archive_count, stock_count, row_count, string_bytes;
        std::memcpy(&archive_count, raw.data() + 52, 4);
        std::memcpy(&stock_count, raw.data() + 56, 4);
        std::memcpy(&row_count, raw.data() + 60, 4);
        std::memcpy(&string_bytes, raw.data() + 64, 4);
        const uint64_t expected = HEADER + uint64_t(archive_count) * 12 + uint64_t(stock_count) * 4 + uint64_t(row_count) * sizeof(MapRow) + string_bytes;
        if (!archive_count || archive_count > 4096 || !stock_count || stock_count > 4096 || row_count > 200000 || expected != raw.size())
            return false;
        Strings.assign(raw.end() - string_bytes, raw.end());
        if (!Strings.empty() && Strings.back() != '\0')
            return false;
        size_t at = HEADER;
        for (uint32_t i = 0; i < archive_count; ++i, at += 12) {
            MapArchive archive;
            uint32_t name;
            std::memcpy(&archive.Bytes, raw.data() + at, 8);
            std::memcpy(&name, raw.data() + at + 8, 4);
            if (!Text(name, archive.Name))
                return false;
            Archives.push_back(std::move(archive));
        }
        for (uint32_t i = 0; i < stock_count; ++i, at += 4) {
            uint32_t name;
            std::memcpy(&name, raw.data() + at, 4);
            std::wstring text;
            if (!Text(name, text))
                return false;
            StockArchives.push_back(std::move(text));
        }
        Rows.resize(row_count);
        if (row_count)
            std::memcpy(Rows.data(), raw.data() + at, size_t(row_count) * sizeof(MapRow));
        for (size_t i = 0; i < Rows.size(); ++i) {
            const MapRow& row = Rows[i];
            if (row.Archive >= archive_count || row.StockArchive >= stock_count || row.StockMember >= Strings.size())
                return false;
            if (row.Family < 1 || row.Family > 7 || row.TargetFamily != row.Family || !row.Mips || row.TargetMips < row.Mips || row.TargetMips > 15)
                return false;
            if (row.Mips > uint32_t(std::bit_width(std::max(row.Width, row.Height))) ||
                row.TargetMips > uint32_t(std::bit_width(std::max(row.TargetWidth, row.TargetHeight))))
                return false;
            if (row.InflatedBytes > (256u << 20))
                return false;
            if (row.PixelOffset < 128 || row.PixelBytes > row.InflatedBytes || row.PixelOffset > row.InflatedBytes - row.PixelBytes)
                return false;
            const uint64_t bytes = Archives[row.Archive].Bytes;
            if (row.BlockOffset > bytes || bytes - row.BlockOffset < 4 + uint64_t(row.CompressedBytes))
                return false;
            if (i && !(Before(Rows[i - 1], row)))
                return false;
            Layouts.insert({row.Width, row.Height, row.Mips, row.Family});
        }
        return true;
    }

    const MapRow* Find(const XXH128_hash_t& key, const Layout& layout) const {
        size_t low = 0, high = Rows.size();
        while (low < high) {
            const size_t middle = (low + high) / 2;
            const MapRow& row = Rows[middle];
            if (row.KeyLow < key.low64 || (row.KeyLow == key.low64 && row.KeyHigh < key.high64))
                low = middle + 1;
            else
                high = middle;
        }
        if (low == Rows.size())
            return nullptr;
        const MapRow& row = Rows[low];
        if (row.KeyLow != key.low64 || row.KeyHigh != key.high64)
            return nullptr;
        return Layout{row.Width, row.Height, row.Mips, row.Family} == layout ? &row : nullptr;
    }

    std::string MemberName(uint32_t offset) const { return std::string(Strings.data() + offset); }

private:
    static bool Before(const MapRow& a, const MapRow& b) {
        return a.KeyLow < b.KeyLow || (a.KeyLow == b.KeyLow && a.KeyHigh < b.KeyHigh);
    }

    bool Text(uint32_t offset, std::wstring& text) const {
        if (offset >= Strings.size())
            return false;
        const char* start = Strings.data() + offset;
        const int length = int(std::strlen(start));
        if (!length)
            return false;
        text.resize(MultiByteToWideChar(CP_UTF8, 0, start, length, nullptr, 0));
        return !text.empty() && MultiByteToWideChar(CP_UTF8, 0, start, length, text.data(), int(text.size())) == int(text.size());
    }
};

}
