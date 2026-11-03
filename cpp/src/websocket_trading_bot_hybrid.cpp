// websocket_trading_bot_hybrid.cpp — C++ port of websocket_trading_bot_hybrid.py.
//
// Same engine and quote selection as the Python bot: the Kalshi price source
// is orderbook_delta + ticker on ONE connection; the ticker quote is the
// baseline and the delta book overrides it on a >= delta_move_threshold move
// (or is the quote outright with bot.prefer_book: true). Reads the same YAML
// config and .env.
//
// Run:  ./build/hybrid_bot [config_websocket.yaml]
// C++-only knobs (env): KALSHI_WS_URL (default wss://api.elections.kalshi.com/
// trade-api/ws/v2), ORDER_TRACE_FILE (one JSON line per completed order).

#include <algorithm>
#include <csignal>
#include <sys/resource.h>
#include <exception>

#include "bot_core.hpp"
#include "log.hpp"
#include "pyutil.hpp"

static const char* BANNER = "DUAL WEBSOCKET ARBITRAGE BOT — KALSHI HYBRID (delta+ticker) [C++]";

// Raise the open-file soft limit to the hard limit (capped). systemd gives
// services a soft limit of 1024, and each Kalshi socket (90 tickers), order
// connection and Manifold socket is a descriptor; past the limit every new
// socket fails with EMFILE. Python does not raise it either; this is cheap.
static rlim_t raise_fd_limit() {  // returns the soft limit now in force
    struct rlimit rl {};
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return 0;
    rlim_t want = rl.rlim_max == RLIM_INFINITY ? (rlim_t)65536 : std::min(rl.rlim_max, (rlim_t)65536);
    if (rl.rlim_cur >= want) return rl.rlim_cur;
    struct rlimit nl = rl;
    nl.rlim_cur = want;
    if (setrlimit(RLIMIT_NOFILE, &nl) != 0) {
        nl.rlim_cur = std::min(want, (rlim_t)10240);  // macOS: OPEN_MAX
        if (setrlimit(RLIMIT_NOFILE, &nl) != 0) return rl.rlim_cur;
    }
    return nl.rlim_cur;
}

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    rlim_t fd_limit = raise_fd_limit();
    logx::start();
    bot::load_dotenv();
    logx::out(std::string(80, '='));
    logx::out(std::string("🤖 ") + BANNER);
    logx::out(std::string(80, '='));
    logx::out("  → Open-file limit: " + std::to_string((unsigned long long)fd_limit));

    int rc = 1;
    try {
        mj::Value config = bot::load_config(argc > 1 ? argv[1] : nullptr, "config_websocket.yaml");
        auto creds = bot::kalshi_credentials();
        if (!creds) {
            logx::out("✗ Missing Kalshi credentials!");
            logx::close();
            return 1;
        }
        const mj::Value* b = config.get("bot");
        mj::Value bot_cfg = b ? *b : mj::Value::object();
        auto f = [&](const char* k, double d) {
            const mj::Value* v = bot_cfg.get(k);
            return v ? bot::py_float_value(v, k) : d;
        };
        auto i = [&](const char* k, int64_t d) {
            const mj::Value* v = bot_cfg.get(k);
            return v ? bot::py_int_value(v, k) : d;
        };
        const mj::Value* pb = bot_cfg.get("prefer_book");
        ps::KalshiHybridClient source(creds->first, creds->second,
                                      (int)py::round0(f("max_kalshi_spread", 0.40) * 100),
                                      i("delta_fresh_ms", ps::DELTA_FRESH_MS),
                                      f("delta_move_threshold", ps::DELTA_MOVE_THRESHOLD),
                                      i("max_staleness_ms", bot::MAX_STALENESS_MS),
                                      f("extreme_high", bot::EXTREME_HIGH),
                                      i("max_feed_silence_ms", ps::MAX_FEED_SILENCE_MS),
                                      // bot.prefer_book: make the ORDER BOOK the quote.
                                      pb && pb->truthy());
        // bot.kalshi_probe_*: pick the fastest Kalshi connections after every start (C++ only).
        ps::ConnProbeConfig probe;
        probe.candidates = (int)i("kalshi_probe_candidates", 0);
        probe.warmup_sec = f("kalshi_probe_warmup_sec", probe.warmup_sec);
        probe.seconds = f("kalshi_probe_seconds", probe.seconds);
        probe.max_seconds = f("kalshi_probe_max_seconds", probe.max_seconds);
        probe.probe_tickers = (int)i("kalshi_probe_tickers", probe.probe_tickers);
        probe.min_updates = (int)i("kalshi_probe_min_updates", probe.min_updates);
        probe.recheck_min = f("kalshi_probe_recheck_min", probe.recheck_min);
        probe.recheck_candidates = (int)i("kalshi_probe_recheck_candidates", probe.recheck_candidates);
        probe.margin_ms = f("kalshi_probe_margin_ms", probe.margin_ms);
        probe.max_total_conns = (int)i("kalshi_probe_max_conns", probe.max_total_conns);
        if (probe.candidates > 0) {
            logx::out("  → Kalshi connection probe: " + std::to_string(probe.candidates) + " candidates after " +
                      py::repr(probe.warmup_sec) + "s, " + py::repr(probe.seconds) + "-" + py::repr(probe.max_seconds) +
                      "s on the " + std::to_string(probe.probe_tickers) + " busiest markets; re-check every " +
                      py::repr(probe.recheck_min) + " min with " + std::to_string(probe.recheck_candidates));
        }
        source.set_conn_probe(probe);
        rc = bot::run_bot(source, config, "kalshi_ticker", BANNER);
    } catch (const std::exception& e) {
        logx::out(std::string("✗ Fatal: ") + e.what());
        rc = 1;
    }
    logx::close();
    return rc;
}
