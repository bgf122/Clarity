#pragma once

// Finding hitches: a scope that takes longer than kSlowMs is logged with its name and time (at most a few
// lines a second), so a stutter in game shows up in Clarity.log as the part of Clarity that caused it.
#include "log.h"

#include <windows.h>

namespace prk::perf {

constexpr double kSlowMs = 4.0;

inline double Now()
{
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

class Slow {
public:
    explicit Slow(const char* what, unsigned detail = 0, double slowMs = kSlowMs)
        : what_(what), detail_(detail), slowMs_(slowMs), start_(Now())
    {
    }
    ~Slow()
    {
        const double ms = Now() - start_;
        if (ms < slowMs_)
            return;
        static DWORD second = 0;
        static int lines = 0;
        const DWORD now = GetTickCount();
        if (now - second >= 1000) {
            second = now;
            lines = 0;
        }
        if (++lines <= 8)
            Log("[perf] %s (0x%X) took %.1f ms", what_, detail_, ms);
    }
    Slow(const Slow&) = delete;
    Slow& operator=(const Slow&) = delete;

private:
    const char* what_;
    unsigned detail_;
    double slowMs_;
    double start_;
};

} // namespace prk::perf
