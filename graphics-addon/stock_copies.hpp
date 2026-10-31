#pragma once
// Byte budget for Original-mode stock copies, least recently bound evicted first.
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace tq {

class StockBudget {
public:
    explicit StockBudget(uint64_t limit) : Limit(limit) {}

    bool Admit(uint64_t handle, uint64_t bytes) {
        std::vector<uint64_t> evicted;
        return Admit(handle, bytes, evicted);
    }

    // Evicted handles are returned so the caller can destroy their copies.
    bool Admit(uint64_t handle, uint64_t bytes, std::vector<uint64_t>& evicted) {
        if (bytes > Limit)
            return false;
        while (Used + bytes > Limit) {
            auto oldest = Entries.begin();
            for (auto it = Entries.begin(); it != Entries.end(); ++it) {
                if (it->second.Stamp < oldest->second.Stamp)
                    oldest = it;
            }
            evicted.push_back(oldest->first);
            Used -= oldest->second.Bytes;
            Entries.erase(oldest);
        }
        Entries[handle] = {bytes, ++Clock};
        Used += bytes;
        return true;
    }

    void Touch(uint64_t handle) {
        if (auto it = Entries.find(handle); it != Entries.end())
            it->second.Stamp = ++Clock;
    }

    void Release(uint64_t handle) {
        if (auto it = Entries.find(handle); it != Entries.end()) {
            Used -= it->second.Bytes;
            Entries.erase(it);
        }
    }

    uint64_t Bytes() const { return Used; }

private:
    struct Entry { uint64_t Bytes = 0, Stamp = 0; };
    uint64_t Limit, Used = 0, Clock = 0;
    std::unordered_map<uint64_t, Entry> Entries;
};

}
