#pragma once
#include <Geode/Geode.hpp>
#include <chrono>
#include <cmath>
#include <deque>
#include "Sha256.hpp"
using namespace geode::prelude;

namespace gdsa {

inline uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int b = 0; b < 8; ++b) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

// Anchor function: its bytes are checksummed to detect inline patching of the mod itself.
__attribute__((noinline)) inline int guardAnchor(int x) { return x * 31 + 7; }

struct GuardReport {
    bool clockOk = true;  double drift = 0;
    bool entropyOk = true; double stddevMs = 0; size_t clicks = 0;
    bool integrityOk = true; uint32_t crc = 0;
    std::string token;
    bool passed() const { return clockOk && entropyOk && integrityOk; }
};

class Guard {
    using Clock = std::chrono::steady_clock;
public:
    static Guard& get() { static Guard g; return g; }

    // Called every frame during a level (hook: PlayLayer::postUpdate)
    void onFrame(float dt) {
        if (!Mod::get()->getSettingValue<bool>("safeplay")) return;
        auto now = Clock::now();
        if (!m_started) { m_start = now; m_accum = 0; m_started = true; }
        m_accum += dt;
        double wall = std::chrono::duration<double>(now - m_start).count();
        if (wall >= 5.0) {
            m_drift = m_accum / wall - 1.0;
            m_clockOk = std::abs(m_drift) <= 0.02; // 2% tolerance, tune against real data
            if (!m_clockOk) log::warn("[GDSA Guard] clock delta drift {:.3f}", m_drift);
            m_started = false;
        }
    }

    void resetLevel() { m_started = false; m_clicks.clear(); }

    // Called on every input (hook: GJBaseGameLayer::pushButton)
    void onClick() {
        double ms = std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
        m_clicks.push_back(ms);
        if (m_clicks.size() > 300) m_clicks.pop_front();
    }

    double clickStddevMs() const {
        if (m_clicks.size() < 3) return -1;
        std::vector<double> iv;
        for (size_t i = 1; i < m_clicks.size(); ++i) iv.push_back(m_clicks[i] - m_clicks[i-1]);
        double mean = 0; for (double v : iv) mean += v; mean /= iv.size();
        double var = 0; for (double v : iv) var += (v - mean) * (v - mean);
        return std::sqrt(var / iv.size());
    }

    GuardReport diagnostic(int accountID, const std::string& syncKey) {
        GuardReport r;
        r.clockOk = m_clockOk; r.drift = m_drift;
        r.clicks = m_clicks.size();
        r.stddevMs = clickStddevMs();
        // Bot-like input: >= 30 clicks with near-zero interval variance
        r.entropyOk = !(r.clicks >= 30 && r.stddevMs >= 0 && r.stddevMs < 0.5);
        r.crc = crc32(reinterpret_cast<const uint8_t*>(&guardAnchor), 32);
        if (!m_baseline) m_baseline = r.crc;
        r.integrityOk = (r.crc == m_baseline);
        auto ts = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        r.token = sha256(fmt::format("{}|{}|{}|{:.4f}|{:.3f}|{}|{}",
            accountID, r.crc, r.clicks, r.drift, r.stddevMs, ts, syncKey));
        return r;
    }
private:
    Clock::time_point m_start; double m_accum = 0; bool m_started = false;
    double m_drift = 0; bool m_clockOk = true;
    std::deque<double> m_clicks; uint32_t m_baseline = 0;
};
}
