#pragma once
// Reads content-addressed DDS files; keeps only bounded, verified session bytes.
#include "texture_map.hpp"
#include <bcrypt.h>
#include <vector>
#include <list>
#include <memory>
#include <mutex>

namespace tq {

class DDSReader {
public:
    DDSReader(std::wstring directory, const TextureMap& map) : Directory(std::move(directory)), Map(map) {}

    // Until this thread's next Read call, even when another thread evicts it.
    const unsigned char* Read(const MapRow& row) {
        thread_local std::shared_ptr<const Bytes> pinned;
        pinned.reset();
        if (!Map.Loose || row.Archive >= Map.Archives.size())
            return nullptr;
        const MapArchive& file = Map.Archives[row.Archive];
        if (file.Bytes < 128 || file.Bytes > (64ull << 20) + 4096)
            return nullptr;
        try {
            // Serialize misses too: concurrent loads of one object verify once.
            std::lock_guard lock(CacheMutex);
            auto found = std::find_if(Cache.begin(), Cache.end(), [&](const Entry& entry) {
                return entry.Digest == file.Digest && entry.Data->size() == file.Bytes;
            });
            if (found != Cache.end()) {
                pinned = found->Data;
                Cache.splice(Cache.begin(), Cache, found);
            } else {
                pinned = Load(file);
                if (!pinned)
                    return nullptr;
                if (pinned->size() <= CacheLimit) {
                    while (!Cache.empty() && (Cache.size() >= EntryLimit || CachedBytes + pinned->size() > CacheLimit)) {
                        CachedBytes -= Cache.back().Data->size();
                        Cache.pop_back();
                    }
                    try {
                        Cache.push_front({file.Digest, pinned});
                        CachedBytes += pinned->size();
                    } catch (const std::bad_alloc&) {} // Verified handoff remains usable uncached.
                }
            }
        } catch (const std::bad_alloc&) {
            pinned.reset();
            return nullptr;
        }
        // A cache hit is admitted immutable session content, never a claim
        // about the current disk file. Eviction/new Reader rechecks the file.
        // Row geometry and range still apply to every handoff, including hits.
        if (!Matches(*pinned, row)) {
            pinned.reset();
            return nullptr;
        }
        return pinned->data() + row.PixelOffset;
    }

private:
    using Bytes = std::vector<unsigned char>;
    struct Entry { std::array<unsigned char, 32> Digest; std::shared_ptr<const Bytes> Data; };
    static constexpr size_t CacheLimit = 64ull << 20, EntryLimit = 16;
    std::wstring Directory;
    const TextureMap& Map;
    std::mutex CacheMutex;
    std::list<Entry> Cache;
    size_t CachedBytes = 0; // Cache ownership only; active thread pins may outlive eviction.

    std::shared_ptr<const Bytes> Load(const MapArchive& file) const {
        auto buffer = std::make_shared<Bytes>();
        HANDLE handle = CreateFileW((Directory + file.Name).c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return {};
        BY_HANDLE_FILE_INFORMATION identity{};
        LARGE_INTEGER bytes{};
        bool valid = GetFileInformationByHandle(handle, &identity) && !(identity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                     identity.nNumberOfLinks == 1 && GetFileSizeEx(handle, &bytes) && uint64_t(bytes.QuadPart) == file.Bytes;
        DWORD read = 0;
        try {
            if (valid) {
                buffer->resize(size_t(file.Bytes));
                valid = ReadFile(handle, buffer->data(), DWORD(buffer->size()), &read, nullptr) && read == buffer->size();
            }
        } catch (const std::bad_alloc&) { valid = false; }
        CloseHandle(handle); // No FILE handles survive into GPU resource creation.
        if (!valid)
            return {};
        unsigned char digest[32];
        if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, buffer->data(), ULONG(buffer->size()), digest, 32) < 0 ||
            std::memcmp(digest, file.Digest.data(), 32) || std::memcmp(buffer->data(), "DDS ", 4))
            return {};
        return buffer;
    }

    static bool Matches(const Bytes& buffer, const MapRow& row) {
        uint32_t width, height, mips;
        std::memcpy(&height, buffer.data() + 12, 4);
        std::memcpy(&width, buffer.data() + 16, 4);
        std::memcpy(&mips, buffer.data() + 28, 4);
        if (!mips) mips = 1;
        return width == row.TargetWidth && height == row.TargetHeight && mips == row.TargetMips &&
            row.PixelOffset == 128 && row.PixelOffset <= buffer.size() && row.PixelBytes <= buffer.size() - row.PixelOffset;
    }
};

}
