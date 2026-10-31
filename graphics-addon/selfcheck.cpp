// Console checks for the add-on's pure parts: map reader, VPK reader, layout math.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "config.hpp"
#include "stock_copies.hpp"
#include "stock_paks.hpp"
#include "texture_map.hpp"
#include "vpk_reader.hpp"
#include <fstream>
#include <string>
#include <vector>

static int Failures = 0;

static void Check(bool condition, const char* name) {
    if (!condition) {
        ++Failures;
        std::printf("FAIL %s\n", name);
    }
}

static std::vector<unsigned char> ReadAll(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(file), {});
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: selfcheck <fixture dir>\n");
        return 2;
    }
    const std::wstring dir = std::wstring(argv[1]) + L"\\";
    auto raw = ReadAll(dir + L"map.bin");
    tq::TextureMap map;
    Check(map.Load(raw), "fixture map loads");
    Check(map.Rows.size() == 1 && map.Archives.size() == 1, "fixture map counts");

    auto stock = ReadAll(dir + L"stock_pixels.bin");
    tq::Layout layout{8, 8, 2, 1};
    std::vector<tq::Subresource> tight{{stock.data(), 16}, {stock.data() + 32, 8}};
    const auto key = tq::Fingerprint(layout, tight.data());
    Check(map.Find(key, layout) != nullptr, "fingerprint finds fixture row");
    Check(map.Find(key, tq::Layout{8, 8, 1, 1}) == nullptr, "layout mismatch misses");
    Check(map.Find({key.low64 + 1, key.high64}, layout) == nullptr, "absent key misses");

    std::vector<unsigned char> padded(2 * 64 + 1 * 64, 0xCD);
    std::memcpy(padded.data(), stock.data(), 16);
    std::memcpy(padded.data() + 64, stock.data() + 16, 16);
    std::memcpy(padded.data() + 128, stock.data() + 32, 8);
    std::vector<tq::Subresource> pitched{{padded.data(), 64}, {padded.data() + 128, 64}};
    const auto padded_key = tq::Fingerprint(layout, pitched.data());
    Check(padded_key.low64 == key.low64 && padded_key.high64 == key.high64, "padded pitch fingerprint matches");

    tq::VpkReader reader(dir, map);
    const auto* pixels = reader.Inflate(map.Rows[0]);
    auto expected = ReadAll(dir + L"upscaled_pixels.bin");
    Check(pixels && std::memcmp(pixels, expected.data(), expected.size()) == 0, "inflate returns upscaled mips");

    auto broken = raw;
    broken[0] = 'X';
    tq::TextureMap rejected;
    Check(!rejected.Load(broken), "wrong magic rejected");
    auto truncated = raw;
    truncated.resize(truncated.size() - 3);
    Check(!rejected.Load(truncated), "truncated map rejected");

    // The fixture's one row follows the 68-byte header, one archive entry and one stock entry.
    constexpr size_t ROW_AT = 68 + 12 + 4;
    auto unsorted = raw;
    std::vector<unsigned char> zero_key(raw.begin() + ROW_AT, raw.begin() + ROW_AT + sizeof(tq::MapRow));
    std::memset(zero_key.data(), 0, 16);
    unsorted.insert(unsorted.begin() + ROW_AT + sizeof(tq::MapRow), zero_key.begin(), zero_key.end());
    const uint32_t two_rows = 2;
    std::memcpy(unsorted.data() + 60, &two_rows, 4);
    tq::TextureMap out_of_order;
    Check(!out_of_order.Load(unsorted), "unsorted rows rejected");

    auto outside = raw;
    const uint64_t past_end = map.Archives[0].Bytes;
    std::memcpy(outside.data() + ROW_AT + offsetof(tq::MapRow, BlockOffset), &past_end, 8);
    tq::TextureMap out_of_archive;
    Check(!out_of_archive.Load(outside), "row block outside its archive rejected");
    const uint64_t wrapping = ~0ull - 3;
    std::memcpy(outside.data() + ROW_AT + offsetof(tq::MapRow, BlockOffset), &wrapping, 8);
    tq::TextureMap wrapped;
    Check(!wrapped.Load(outside), "row block offset that wraps rejected");
    for (const uint32_t mips : {16u, 6u}) {
        auto too_deep = raw;
        std::memcpy(too_deep.data() + ROW_AT + offsetof(tq::MapRow, TargetMips), &mips, 4);
        tq::TextureMap deep;
        Check(!deep.Load(too_deep), mips == 16 ? "target mips over 15 rejected" : "target mips past the 1x1 level rejected");
    }

    tq::TextureMap resized = map;
    resized.Archives[0].Bytes += 1;
    tq::VpkReader mismatch(dir, resized);
    Check(mismatch.Inflate(resized.Rows[0]) == nullptr, "archive size mismatch marks archive unavailable");

    if (argc >= 4) {
        auto real_raw = ReadAll(argv[2]);
        tq::TextureMap real;
        Check(real.Load(real_raw), "real map loads");
        tq::VpkReader real_reader(std::wstring(argv[3]) + L"\\", real);
        const auto* real_pixels = real.Rows.empty() ? nullptr : real_reader.Inflate(real.Rows[0]);
        Check(real_pixels && std::memcmp(real_pixels - 128, "DDS ", 4) == 0, "real member inflates");
    }
    {
        const std::wstring ini = dir + L"TextureQuest.ini";
        auto narrow = [](const std::wstring& text) {
            std::string out(WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), int(out.size()), nullptr, nullptr);
            return out;
        };
        auto write = [&](const std::string& downloads, bool with_map) {
            std::ofstream out(ini, std::ios::binary | std::ios::trunc);
            out << "[TextureQuest]\r\n";
            if (with_map)
                out << "Map=" << narrow(dir + L"map.bin") << "\r\n";
            out << "Downloads=" << downloads << "\r\nShortcut=" << narrow(dir + L"shortcut.txt") << "\r\nGamePaks=" << narrow(dir + L"paks") << "\r\n";
        };
        tq::Config config;
        std::string error;
        write(narrow(dir), true);
        Check(tq::ReadConfig(ini, config, error), "valid config reads");
        write(narrow(dir + L"missing-folder"), true);
        Check(!tq::ReadConfig(ini, config, error) && error.find("Downloads") != std::string::npos, "config with missing downloads folder disables replacement");
        write(narrow(dir), false);
        Check(!tq::ReadConfig(ini, config, error) && error.find("Map") != std::string::npos, "config with missing key refused");
    }
    {
        tq::StockBudget budget(100);
        Check(budget.Admit(1, 40) && budget.Admit(2, 40), "budget admits within limit");
        budget.Touch(1);
        std::vector<uint64_t> evicted;
        Check(budget.Admit(3, 40, evicted) && evicted.size() == 1 && evicted[0] == 2, "least recently bound evicted first");
        budget.Release(1);
        Check(budget.Bytes() == 40, "stock copy released when texture destroyed");
        Check(!budget.Admit(4, 200), "oversized copy refused");
    }
    {
        tq::StockPaks paks(dir + L"paks\\", map);
        std::vector<unsigned char> out;
        const size_t at = paks.Read(map.Rows[0], out);
        Check(at && std::memcmp(out.data() + at, stock.data(), stock.size()) == 0, "stock member read from game pak");
        tq::TextureMap missing = map;
        missing.StockArchives[0] = L"Absent.vpk";
        tq::StockPaks absent(dir + L"paks\\", missing);
        Check(absent.Read(missing.Rows[0], out) == 0, "missing stock pak returns 0");
    }
    std::printf("selfcheck: %d failures\n", Failures);
    return Failures ? 1 : 0;
}
