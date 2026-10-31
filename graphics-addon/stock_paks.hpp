#pragma once
// The game's stock paks, mapped once on first use, with each pak's member index
// parsed once. Used only to build Original-mode stock copies on the worker thread.
#include "texture_map.hpp"
#include <libdeflate.h>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>

namespace tq {

inline bool InflateUnknown(const unsigned char* data, size_t size, std::vector<unsigned char>& out) {
    thread_local libdeflate_decompressor* decompressor = libdeflate_alloc_decompressor();
    if (!decompressor)
        return false;
    for (size_t capacity = std::max<size_t>(size * 4, 1 << 16); capacity <= (256u << 20); capacity *= 2) {
        try {
            out.resize(capacity);
        } catch (const std::bad_alloc&) {
            return false;
        }
        size_t actual = 0;
        const auto result = libdeflate_zlib_decompress(decompressor, data, size, out.data(), out.size(), &actual);
        if (result == LIBDEFLATE_SUCCESS) {
            out.resize(actual);
            return true;
        }
        if (result != LIBDEFLATE_INSUFFICIENT_SPACE)
            return false;
    }
    return false;
}

class StockPaks {
public:
    StockPaks(std::wstring folder, const TextureMap& map) : Folder(std::move(folder)), Map(map) {}

    StockPaks(const StockPaks&) = delete;
    StockPaks& operator=(const StockPaks&) = delete;

    // Inflates the row's stock member into out and returns the offset of its
    // mip chain, or 0 when the pak, member or DDS header is unusable.
    size_t Read(const MapRow& row, std::vector<unsigned char>& out) {
        const Pak* pak = Open(row.StockArchive);
        if (!pak)
            return 0;
        auto member = pak->Members.find(Map.MemberName(row.StockMember));
        if (member == pak->Members.end())
            return 0;
        const auto [offset, csize] = member->second;
        if (offset + 4 + uint64_t(csize) > pak->Bytes || !InflateUnknown(pak->Base + offset + 4, csize, out) || out.size() < 8)
            return 0;
        uint32_t name_length;
        std::memcpy(&name_length, out.data(), 4);
        const size_t pixels = 8 + size_t(name_length) + 128;
        if (pixels > out.size() || std::memcmp(out.data() + pixels - 128, "DDS ", 4))
            return 0;
        return pixels;
    }

private:
    struct Pak {
        const unsigned char* Base = nullptr;
        uint64_t Bytes = 0;
        std::unordered_map<std::string, std::pair<uint64_t, uint32_t>> Members;
        bool Tried = false;
    };

    const Pak* Open(uint32_t index) {
        std::lock_guard lock(Lock);
        Pak& pak = Paks[index];
        if (pak.Tried)
            return pak.Base ? &pak : nullptr;
        pak.Tried = true;
        if (index >= Map.StockArchives.size())
            return nullptr;
        HANDLE file = CreateFileW((Folder + Map.StockArchives[index]).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER size{};
        HANDLE section = nullptr;
        if (file != INVALID_HANDLE_VALUE && GetFileSizeEx(file, &size) && size.QuadPart >= 16)
            section = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (section) {
            pak.Base = static_cast<const unsigned char*>(MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0));
            CloseHandle(section);
        }
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
        if (!pak.Base)
            return nullptr;
        pak.Bytes = uint64_t(size.QuadPart);
        if (!ParseIndex(pak)) {
            UnmapViewOfFile(pak.Base);
            pak.Base = nullptr;
            return nullptr;
        }
        return &pak;
    }

    // Footer and index layout match texturequest_graphics_vpk.index.
    static bool ParseIndex(Pak& pak) {
        uint32_t magic, offset, version, field;
        std::memcpy(&magic, pak.Base + pak.Bytes - 12, 4);
        std::memcpy(&offset, pak.Base + pak.Bytes - 8, 4);
        std::memcpy(&version, pak.Base + pak.Bytes - 4, 4);
        if (magic != 0x33744 || version != 2 || uint64_t(offset) + 16 > pak.Bytes)
            return false;
        std::memcpy(&field, pak.Base + offset, 4);
        const uint64_t compressed = pak.Bytes - offset - 16;
        std::vector<unsigned char> raw;
        if (field != compressed + 12 || !InflateUnknown(pak.Base + offset + 4, size_t(compressed), raw) || raw.size() < 8)
            return false;
        uint32_t count;
        std::memcpy(&count, raw.data() + 4, 4);
        size_t at = 8;
        for (uint32_t i = 0; i < count; ++i) {
            if (at + 12 > raw.size())
                return false;
            uint32_t block, csize, length;
            std::memcpy(&block, raw.data() + at, 4);
            std::memcpy(&csize, raw.data() + at + 4, 4);
            std::memcpy(&length, raw.data() + at + 8, 4);
            at += 12;
            if (at + length > raw.size())
                return false;
            pak.Members.emplace(std::string(reinterpret_cast<const char*>(raw.data() + at), length), std::make_pair(uint64_t(block), csize));
            at += length;
        }
        return true;
    }

    std::wstring Folder;
    const TextureMap& Map;
    std::mutex Lock;
    std::unordered_map<uint32_t, Pak> Paks;
};

}
