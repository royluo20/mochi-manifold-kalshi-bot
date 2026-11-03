// clock.hpp — time.time() / now_ms() / perf_counter(), with a fake wall clock
// for the deterministic replay harness (never set in a live run).
#pragma once

#include <cstdint>
#include <ctime>

namespace clk {

inline bool g_fake = false;
inline double g_fake_now = 0.0;

// Python time.time()
inline double time_sec() {
    if (g_fake) [[unlikely]]
        return g_fake_now;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// bot_core.now_ms(): int(time.time() * 1000)
inline int64_t now_ms() { return (int64_t)(time_sec() * 1000.0); }

// Python time.perf_counter() (monotonic, for durations only)
inline double perf_counter() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

}  // namespace clk
