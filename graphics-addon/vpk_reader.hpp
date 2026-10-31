#pragma once
// Maps release VPKs once and inflates single members with libdeflate.
#include "texture_map.hpp"
#include <libdeflate.h>
#include <mutex>
#include <new>

namespace tq {

class VpkReader {
public:
    VpkReader(std::wstring directory, const TextureMap& map) : Directory(std::move(directory)), Map(map), Views(map.Archives.size()) {}

    VpkReader(const VpkReader&) = delete;
    VpkReader& operator=(const VpkReader&) = delete;

    ~VpkReader() {
        for (const View& view : Views) {
            if (view.Base)
                UnmapViewOfFile(view.Base);
        }
    }

    // Returns the start of the upscaled mip chain in this thread's buffer, or
    // nullptr. Valid until the calling thread's next Inflate.
    const unsigned char* Inflate(const MapRow& row) {
        thread_local std::vector<unsigned char> buffer;
        thread_local libdeflate_decompressor* decompressor = libdeflate_alloc_decompressor();
        const unsigned char* base = Open(row.Archive);
        if (!base || !decompressor)
            return nullptr;
        const unsigned char* block = base + row.BlockOffset;
        WIN32_MEMORY_RANGE_ENTRY range{const_cast<unsigned char*>(block), size_t(row.CompressedBytes) + 4};
        PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
        uint32_t length;
        std::memcpy(&length, block, 4);
        if (length != row.CompressedBytes)
            return nullptr;
        try {
            buffer.resize(row.InflatedBytes);
        } catch (const std::bad_alloc&) {
            return nullptr;
        }
        size_t actual = 0;
        if (libdeflate_zlib_decompress(decompressor, block + 4, row.CompressedBytes, buffer.data(), buffer.size(), &actual) != LIBDEFLATE_SUCCESS ||
            actual != row.InflatedBytes || std::memcmp(buffer.data() + row.PixelOffset - 128, "DDS ", 4))
            return nullptr;
        return buffer.data() + row.PixelOffset;
    }

private:
    struct View {
        const unsigned char* Base = nullptr;
        bool Failed = false;
    };

    // Maps the whole archive once; the view outlives its file and section handles.
    const unsigned char* Open(uint32_t index) {
        std::lock_guard lock(Lock);
        if (index >= Views.size())
            return nullptr;
        View& view = Views[index];
        if (view.Base || view.Failed)
            return view.Base;
        const MapArchive& archive = Map.Archives[index];
        HANDLE file = CreateFileW((Directory + archive.Name).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER size{};
        HANDLE section = nullptr;
        if (file != INVALID_HANDLE_VALUE && GetFileSizeEx(file, &size) && uint64_t(size.QuadPart) == archive.Bytes)
            section = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (section) {
            view.Base = static_cast<const unsigned char*>(MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0));
            CloseHandle(section);
        }
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
        view.Failed = view.Base == nullptr;
        return view.Base;
    }

    std::wstring Directory;
    const TextureMap& Map;
    std::mutex Lock;
    std::vector<View> Views;
};

}
