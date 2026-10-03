#pragma once
#include <array>
#include <cmath>
#include <ctime>
#include <string>
#include <cstdint>
namespace gdsa {

struct Tier { const char* name; int lo; int hi; }; // hi = INT32_MAX for open-ended

inline constexpr std::array<Tier, 5> BASIC = {{
    {"Easy", 0, 100}, {"Medium", 101, 250}, {"Hard", 251, 450},
    {"Harder", 451, 700}, {"Insane", 701, 1000}}};
inline constexpr std::array<Tier, 5> DEMON = {{
    {"Easy Demon", 1001, 1500}, {"Medium Demon", 1501, 2200}, {"Hard Demon", 2201, 3200},
    {"Insane Demon", 3201, 4500}, {"Extreme Demon", 4501, INT32_MAX}}};

inline const Tier& tierFor(int mmr) {
    for (auto& t : BASIC) if (mmr <= t.hi) return t;
    for (auto& t : DEMON) if (mmr <= t.hi) return t;
    return DEMON.back();
}

// days since 1970-01-01 for a civil date (Howard Hinnant's algorithm), portable replacement for timegm
inline int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

struct SeasonInfo { bool demon; int index; int64_t secondsLeft; };

// Rotation every 2 calendar months (UTC): even blocks = Basic, odd blocks = Demon.
inline SeasonInfo currentSeason(int64_t now = (int64_t)std::time(nullptr)) {
    std::time_t t = (std::time_t)now;
    std::tm* g = std::gmtime(&t);
    int64_t monthIdx = (int64_t)(g->tm_year + 1900) * 12 + g->tm_mon;
    int64_t block = monthIdx / 2;
    int64_t nextMonthIdx = (block + 1) * 2;
    int64_t ny = nextMonthIdx / 12;
    unsigned nm = (unsigned)(nextMonthIdx % 12) + 1;
    int64_t end = daysFromCivil(ny, nm, 1) * 86400;
    return { block % 2 == 1, (int)block, end - now };
}

inline std::string formatCountdown(int64_t s) {
    if (s < 0) s = 0;
    char buf[48];
    snprintf(buf, sizeof buf, "%lldd %02lldh %02lldm %02llds",
        (long long)(s / 86400), (long long)(s % 86400 / 3600), (long long)(s % 3600 / 60), (long long)(s % 60));
    return buf;
}

// Elo-style MMR delta; score: 1 win, 0 loss.
inline int mmrDelta(int me, int opp, bool won, int streak) {
    double expected = 1.0 / (1.0 + std::pow(10.0, (opp - me) / 400.0));
    double k = 32.0 + (won && streak >= 3 ? 4.0 : 0.0);
    return (int)std::lround(k * ((won ? 1.0 : 0.0) - expected));
}
}
