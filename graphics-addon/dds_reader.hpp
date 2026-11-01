#pragma once
// Reads exactly one content-addressed DDS. No replacement archive/index/cache.
#include "texture_map.hpp"
#include <bcrypt.h>
#include <vector>

namespace tq {

class DDSReader {
public:
    DDSReader(std::wstring directory, const TextureMap& map) : Directory(std::move(directory)), Map(map) {}

    // Same handoff lifetime as VpkReader: until this thread's next Read call.
    const unsigned char* Read(const MapRow& row) {
        if (!Map.Loose || row.Archive >= Map.Archives.size())
            return nullptr;
        const MapArchive& file = Map.Archives[row.Archive];
        thread_local std::vector<unsigned char> buffer;
        HANDLE handle = CreateFileW((Directory + file.Name).c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return nullptr;
        BY_HANDLE_FILE_INFORMATION identity{};
        LARGE_INTEGER bytes{};
        bool valid = GetFileInformationByHandle(handle, &identity) && !(identity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                     identity.nNumberOfLinks == 1 && GetFileSizeEx(handle, &bytes) && uint64_t(bytes.QuadPart) == file.Bytes;
        DWORD read = 0;
        try {
            if (valid) {
                buffer.resize(size_t(file.Bytes));
                valid = ReadFile(handle, buffer.data(), DWORD(buffer.size()), &read, nullptr) && read == buffer.size();
            }
        } catch (const std::bad_alloc&) { valid = false; }
        CloseHandle(handle); // No FILE handles survive into GPU resource creation.
        if (!valid)
            return nullptr;
        unsigned char digest[32];
        if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, buffer.data(), ULONG(buffer.size()), digest, 32) < 0 ||
            std::memcmp(digest, file.Digest.data(), 32) || std::memcmp(buffer.data(), "DDS ", 4))
            return nullptr;
        uint32_t width, height, mips;
        std::memcpy(&height, buffer.data() + 12, 4);
        std::memcpy(&width, buffer.data() + 16, 4);
        std::memcpy(&mips, buffer.data() + 28, 4);
        if (!mips) mips = 1;
        if (width != row.TargetWidth || height != row.TargetHeight || mips != row.TargetMips ||
            row.PixelOffset > buffer.size() || row.PixelBytes > buffer.size() - row.PixelOffset)
            return nullptr;
        return buffer.data() + row.PixelOffset;
    }

private:
    std::wstring Directory;
    const TextureMap& Map;
};

}
