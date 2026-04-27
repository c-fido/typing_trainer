// Records per-character cumulative timestamps for ghost playback across sessions.

#pragma once
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

struct GhostData {
    std::vector<uint32_t> timestamps; // cumulative ms from session start, one per char typed

    bool empty() const { return timestamps.empty(); }
    uint32_t totalMs() const { return timestamps.empty() ? UINT32_MAX : timestamps.back(); }

    void record(uint32_t elapsed_ms) { timestamps.push_back(elapsed_ms); }

    // How many chars had the ghost typed by elapsed_ms into the session?
    int posAtMs(uint32_t elapsed_ms) const {
        auto it = std::upper_bound(timestamps.begin(), timestamps.end(), elapsed_ms);
        return (int)(it - timestamps.begin());
    }

    bool save(const std::string& filename) const {
        std::ofstream f(filename, std::ios::binary);
        if (!f) return false;
        uint32_t n = (uint32_t)timestamps.size();
        f.write(reinterpret_cast<const char*>(&n), sizeof(n));
        f.write(reinterpret_cast<const char*>(timestamps.data()), n * sizeof(uint32_t));
        return f.good();
    }

    bool load(const std::string& filename) {
        std::ifstream f(filename, std::ios::binary);
        if (!f) return false;
        uint32_t n = 0;
        f.read(reinterpret_cast<char*>(&n), sizeof(n));
        if (!f || n == 0 || n > 1'000'000) return false;
        timestamps.resize(n);
        f.read(reinterpret_cast<char*>(timestamps.data()), n * sizeof(uint32_t));
        return f.good();
    }
};
