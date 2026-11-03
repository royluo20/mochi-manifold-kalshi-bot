// unit_tests.cpp — built-in checks, plus filter modes that tests/check_pyutil.py
// drives against real Python:
//   unit_tests                 run the built-in assertions
//   unit_tests round           stdin "x n" lines -> repr(round(x, n))
//   unit_tests float           stdin lines       -> repr(float(s)) or ERR
//   unit_tests repr            stdin floats (repr) -> "repr<TAB>orjson"
//   unit_tests yaml FILE       print the parsed YAML as JSON
//   unit_tests json            stdin JSON docs (one per line) -> orjson-style dump or ERR
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "amm_sizing.hpp"
#include "bot_core.hpp"
#include "clock.hpp"
#include "json.hpp"
#include "log.hpp"
#include "net.hpp"
#include <arpa/inet.h>
#include <unistd.h>
#include "price_sources.hpp"
#include "pyutil.hpp"
#include "yaml.hpp"

static int g_fail = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                        \
        }                                                                    \
    } while (0)

static void builtin() {
    // round(): the cases that defeat floor(x*10^n + 0.5)
    CHECK(py::repr(py::round_n(2.675, 2)) == "2.67");
    CHECK(py::repr(py::round_n(0.125, 2)) == "0.12");
    CHECK(py::repr(py::round_n(0.285, 2)) == "0.28");
    CHECK(py::repr(py::round_n(1.005, 2)) == "1.0");
    CHECK(py::repr(py::round_n(-0.001, 2)) == "-0.0");
    CHECK(py::repr(py::round_n(28.000000000000004, 6)) == "28.0");
    CHECK(py::repr(py::round_n(1.0 - 0.54, 6)) == "0.46");
    CHECK(py::repr(0.1 + 0.2) == "0.30000000000000004");
    CHECK(py::repr(1e16) == "1e+16" && py::orjson_float(1e16) == "1e16");
    CHECK(py::repr(1e-5) == "1e-05" && py::orjson_float(1e-5) == "0.00001");
    CHECK(py::orjson_float(50.0) == "50.0" && py::orjson_float(0.54) == "0.54");

    // float(str)
    CHECK(py::float_from_str(" 0.5 ").value() == 0.5);
    CHECK(py::float_from_str("1_000.5").value() == 1000.5);
    CHECK(!py::float_from_str("0x10"));
    CHECK(!py::float_from_str("1__0"));
    CHECK(std::isinf(py::float_from_str("-Infinity").value()));

    // JSON
    mj::Value v = mj::parse(R"({"a":1,"b":[1.5,"xé",null,true],"a":2})");
    CHECK(v.get("a")->i == 2 && v.o.size() == 2 && v.o[0].first == "a");  // dict semantics
    CHECK(v.dump() == R"({"a":2,"b":[1.5,"xé",null,true]})");
    bool threw = false;
    try { mj::parse("{\"a\":NaN}"); } catch (const mj::ParseError&) { threw = true; }
    CHECK(threw);

    // YAML 1.1 resolution (PyYAML)
    mj::Value y = yamlite::parse("a: no\nb: 0123\nc: 1e-4\nd: -.5\ne: 1.0e-4\nf: ~\ng: 'x: y'\nh: [1, \"two\", off]\n"
                                 "items:\n- name: one\n  v: 1\n-   name: two\n    v: 2.5\n");
    CHECK(y.get("a")->is_bool() && !y.get("a")->b);
    CHECK(y.get("b")->is_int() && y.get("b")->i == 83);
    CHECK(y.get("c")->is_string());
    CHECK(y.get("d")->is_string());
    CHECK(y.get("e")->is_float() && y.get("e")->d == 1e-4);
    CHECK(y.get("f")->is_null());
    CHECK(y.get("g")->s == "x: y");
    CHECK(y.get("h")->a.size() == 3 && y.get("h")->a[2].is_bool());
    CHECK(y.get("items")->a.size() == 2 && y.get("items")->a[1].get("v")->d == 2.5);

    // AMM: invariant reproduces totalLiquidity on a fresh p=0.5 pool; closed-form cost lands on target
    CHECK(std::fabs(amm::cpmm_invariant(100, 100, 0.5) - 100) < 1e-9);
    auto pool = amm::reconstruct_pool(0.46, 100, 0.5);
    double cost = amm::cpmm_amount_to_move_prob(pool.first, pool.second, 0.5, 0.54, true);
    double after = amm::cpmm_prob_after_bet(pool.first, pool.second, 0.5, cost, true);
    CHECK(after <= 0.54 + 1e-12 && after > 0.539);

    // Kalshi book: one-sided book quotes its live side under prefer_book
    clk::g_fake = true;
    clk::g_fake_now = 1792345000.0;
    auto feed = std::make_shared<ps::FeedLiveness>();
    feed->note_message();
    ps::KalshiMarketState st(25, 3000, 0.02, 20000, 0.99, feed, 8000, true);
    mj::Value yes = mj::parse(R"([["0.9700","100.00"]])"), no = mj::Value::array();
    st.apply_snapshot(yes, no, std::nullopt, true);
    auto ba = st.effective_bid_ask();
    CHECK(ba && ba->first == 0.97 && ba->second == 1.0 && st.quote_source() == "book(one-sided)");
    // integer sizes: a level consumed by deltas leaves no float residue
    st.apply_delta("yes", 0.97, -0.5, std::nullopt);
    st.apply_delta("yes", 0.97, -99.5, std::nullopt);
    CHECK(st.yes_levels.empty());
    clk::g_fake = false;

    // Connection probe: ranking by p90 then p50; ids with too few samples drop out.
    {
        std::vector<std::vector<double>> offs = {
            {1, 2, 3, 40, 40, 40, 40, 40, 40, 40},  // id 0: fine median, bad tail
            {2, 2, 2, 2, 2, 2, 2, 2, 3, 5},          // id 1: clean
            {1, 1, 1, 1, 1, 1, 1, 1, 1, 5},          // id 2: clean, lower median
            {0, 0},                                  // id 3: too few samples
            {},                                      // id 4: never measured
        };
        auto r = ps::rank_probe(offs);
        CHECK(r.size() == 3 && r[0].id == 2 && r[1].id == 1 && r[2].id == 0);
        CHECK(r[0].p90 == 1 && r[2].p90 == 40);
    }
    // delta_key: the same update on two sockets matches regardless of sid/seq and field order.
    {
        mj::Value a = mj::parse(R"({"market_ticker":"KX-A","side":"yes","price_dollars":"0.4100","delta_fp":"-5.00","ts":"2026-10-27T03:51:07.463Z"})");
        mj::Value b = mj::parse(R"({"ts":"2026-10-27T03:51:07.463Z","delta_fp":"-5.00","price_dollars":"0.4100","side":"yes","market_ticker":"KX-A"})");
        mj::Value c = mj::parse(R"({"market_ticker":"KX-A","side":"yes","price_dollars":"0.4100","delta_fp":"-6.00","ts":"2026-10-27T03:51:07.463Z"})");
        CHECK(ps::delta_key(a) == ps::delta_key(b) && ps::delta_key(a) != ps::delta_key(c));
    }
    // An adopted probe socket: frames of the probe subscription (ignored sid) never reach
    // the books or the seq check; the socket's own subscription is applied as usual.
    {
        clk::g_fake = true;
        clk::g_fake_now = 1700000000.0;
        ps::KalshiHybridClient src("replay-key", "", 40, 3000, 0.02, 5000, 0.99, 8000, true, true);
        int calls = 0;
        auto conn = src.open_replay({"KX-A"}, [&](ps::Src, const std::string&, const ps::AidOpt&, double, double) { ++calls; });
        src.replay_connect(*conn);
        conn->ignore_sids = {1};
        auto frame = [](int sid, int seq, const char* type, const char* body) {
            return std::string("{\"type\":\"") + type + "\",\"sid\":" + std::to_string(sid) + ",\"seq\":" +
                   std::to_string(seq) + ",\"msg\":" + body + "}";
        };
        const char* snap = R"({"market_ticker":"KX-A","yes_dollars_fp":[["0.4000","10.00"]],"no_dollars_fp":[["0.5900","10.00"]]})";
        const char* d1 = R"({"market_ticker":"KX-A","side":"yes","price_dollars":"0.4100","delta_fp":"5.00"})";
        using FA = ps::KalshiHybridClient::FrameAction;
        CHECK(src.handle_frame(*conn, frame(1, 57, "orderbook_delta", d1), 1.0) == FA::Continue);  // probe sid: dropped
        CHECK(src.get_state("KX-A")->get_bid_ask_prob() == std::nullopt && calls == 0);
        CHECK(src.handle_frame(*conn, frame(2, 1, "orderbook_snapshot", snap), 1.1) == FA::Continue);
        CHECK(src.handle_frame(*conn, frame(1, 58, "orderbook_delta", d1), 1.2) == FA::Continue);  // still dropped, no seq gap
        auto ba = src.get_state("KX-A")->get_bid_ask_prob();
        CHECK(ba && ba->first == 0.40 && ba->second == 0.41);
        CHECK(src.handle_frame(*conn, frame(2, 2, "orderbook_delta", d1), 1.3) == FA::Continue);  // own sid: applied
        ba = src.get_state("KX-A")->get_bid_ask_prob();
        CHECK(ba && ba->first == 0.41);
        CHECK(src.handle_frame(*conn, frame(2, 4, "orderbook_delta", d1), 1.4) == FA::Resync);    // own seq gap still detected
        clk::g_fake = false;
    }
}

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "";
    std::string line;
    if (mode == "round") {
        while (std::getline(std::cin, line)) {
            double x;
            int n;
            if (std::sscanf(line.c_str(), "%lf %d", &x, &n) != 2) continue;
            std::cout << py::repr(py::round_n(x, n)) << '\n';
        }
        return 0;
    }
    if (mode == "float") {
        while (std::getline(std::cin, line)) {
            auto f = py::float_from_str(line);
            std::cout << (f ? py::repr(*f) : std::string("ERR")) << '\n';
        }
        return 0;
    }
    if (mode == "repr") {
        while (std::getline(std::cin, line)) {
            double x = std::strtod(line.c_str(), nullptr);
            std::cout << py::repr(x) << '\t' << py::orjson_float(x) << '\n';
        }
        return 0;
    }
    if (mode == "yaml" && argc > 2) {
        try {
            std::cout << yamlite::parse_file(argv[2]).dump() << '\n';
        } catch (const std::exception& e) {
            std::cout << "ERR " << e.what() << '\n';
            return 1;
        }
        return 0;
    }
    if (mode == "dns" && argc > 3) {  // first address of N successive new connections
        net::DnsTable t;
        int n = argc > 4 ? std::atoi(argv[4]) : 8;
        for (int k = 0; k < n; ++k) {
            std::string err;
            auto addrs = t.next_addrs(argv[2], std::atoi(argv[3]), err);
            char buf[INET6_ADDRSTRLEN] = {0};
            const auto& ss = addrs.at(0).first;
            if (ss.ss_family == AF_INET6) inet_ntop(AF_INET6, &((const sockaddr_in6*)&ss)->sin6_addr, buf, sizeof buf);
            else inet_ntop(AF_INET, &((const sockaddr_in*)&ss)->sin_addr, buf, sizeof buf);
            std::cout << buf << '\n';
        }
        return 0;
    }
    if (mode == "startms") {  // "J <json value>" | "Y <yaml scalar>" -> bot_core._start_ms
        while (std::getline(std::cin, line)) {
            if (line.size() < 2) continue;
            try {
                mj::Value v;
                if (line[0] == 'J') {
                    v = mj::parse(line.substr(2));
                } else {
                    mj::Value doc = yamlite::parse("t: " + line.substr(2) + "\n");
                    const mj::Value* t = doc.is_object() ? doc.get("t") : nullptr;
                    v = t ? *t : mj::Value::null();
                }
                auto r = bot::start_ms(&v);
                std::cout << (r ? std::to_string(*r) : std::string("None")) << '\n';
            } catch (const std::exception&) {
                std::cout << "EXC\n";
            }
        }
        return 0;
    }
    if (mode == "dnsswr" && argc > 2) {  // expired entries are served stale, refreshed off-thread
        net::DnsTable t(0.001);
        std::string err;
        auto first = t.next_addrs(argv[2], 443, err);  // the only inline lookup
        double worst = 0;
        size_t got = 0;
        for (int k = 0; k < 2000; ++k) {
            double t0 = ev::Loop::monotonic();
            got += t.next_addrs(argv[2], 443, err).size();
            worst = std::max(worst, ev::Loop::monotonic() - t0);
            if (k % 200 == 0) usleep(20000);  // let background refreshes land
        }
        std::printf("first=%zu calls=2000 addrs_served=%zu worst_call_ms=%.3f\n", first.size(), got, worst * 1000);
        return worst < 0.005 && got > 0 ? 0 : 1;
    }
    if (mode == "wsstress" && argc > 3) {  // N WebSocket clients to URL for SECS: delivery and loop lag
        int n = std::atoi(argv[2]);
        std::string url = argv[3];
        double secs = argc > 4 ? std::atof(argv[4]) : 20;
        ev::Loop& L = ev::Loop::get();
        std::vector<std::shared_ptr<net::WsClient>> clients;
        long long msgs = 0;
        int opened = 0, closed = 0;
        std::string first_err;
        int ramp = argc > 5 ? std::atoi(argv[5]) : n;   // connects started per 100 ms
        for (int k = 0; k < n; ++k) {
            if (k && ramp < n && k % ramp == 0) L.run_until([] { return false; }, 0.1);
            auto c = std::make_shared<net::WsClient>();
            c->on_open = [&] { ++opened; };
            c->on_message = [&](std::string_view) { ++msgs; };
            c->on_close = [&](bool, int code, const std::string& why) {
                ++closed;
                if (first_err.empty()) first_err = std::to_string(code) + " " + why;
            };
            net::WsOptions o;
            o.ping_interval = 5;
            o.ping_timeout = 5;
            o.open_timeout = 30;
            c->connect(url, o);
            clients.push_back(c);
        }
        // Loop-lag probe: a 10 ms timer chain; lateness = how long callbacks waited.
        std::vector<double> lag;
        double t_end = ev::Loop::monotonic() + secs, t_open_all = -1, t_start = ev::Loop::monotonic();
        std::function<void(double)> tick = [&](double due) {
            double now = ev::Loop::monotonic();
            lag.push_back((now - due) * 1000);
            if (opened + closed >= n && t_open_all < 0) t_open_all = now - t_start;
            if (now < t_end) L.call_later(0.01, [&, d = now + 0.01] { tick(d); });
        };
        L.call_later(0.01, [&, d = ev::Loop::monotonic() + 0.01] { tick(d); });
        long long msgs_at_half = -1;
        L.call_later(secs / 2, [&] { msgs_at_half = msgs; });
        L.run_until([&] { return ev::Loop::monotonic() >= t_end; }, secs + 5);
        std::sort(lag.begin(), lag.end());
        auto q = [&](double p) { return lag.empty() ? 0.0 : lag[std::min(lag.size() - 1, (size_t)(p * (lag.size() - 1)))]; };
        std::printf("clients=%d opened=%d closed=%d all_settled_s=%.2f msgs=%lld msgs_2nd_half=%lld "
                    "loop_lag_ms p50=%.3f p99=%.3f max=%.3f first_close='%s'\n", n, opened, closed, t_open_all, msgs,
                    msgs_at_half < 0 ? -1 : msgs - msgs_at_half, q(0.5), q(0.99), lag.empty() ? 0 : lag.back(),
                    first_err.c_str());
        return 0;
    }
    if (mode == "mfsoak" && argc > 3) {  // the bot's Manifold WS client on SLUG[:AID]... for SECS
        // Prints "<unix time> frames N" each minute beside the client's own connect/close
        // lines; race/manifold_ws_soak.py runs several of these next to Python clients.
        double secs = std::atof(argv[2]);
        std::vector<std::unique_ptr<bot::Pair>> owned;
        std::vector<bot::Pair*> pairs;
        for (int k = 3; k < argc; ++k) {
            std::string a = argv[k];
            auto p = std::make_unique<bot::Pair>();
            size_t colon = a.find(':');
            p->slug = a.substr(0, colon);
            if (colon != std::string::npos) p->aid = a.substr(colon + 1);
            pairs.push_back(p.get());
            owned.push_back(std::move(p));
        }
        logx::start();  // the client's connect/close lines go through the async log, as in the bot
        ev::Loop& L = ev::Loop::get();
        bot::ManifoldWebSocketClient ws;
        ws.bet_trigger = true;  // production default (new_bet_trigger): new-bet topics too
        long long frames = 0;
        ws.receive_messages([&](ps::Src, const std::string&, const bot::AidOpt&, double, double) { ++frames; }, pairs);
        std::function<void()> report = [&] {
            std::printf("%.3f frames %lld\n", clk::time_sec(), frames);
            std::fflush(stdout);
            L.call_later(60, report);
        };
        L.call_later(60, report);
        L.run_until([] { return false; }, secs);
        ws.close();
        logx::close();
        std::printf("%.3f frames %lld\n", clk::time_sec(), frames);
        return 0;
    }
    if (mode == "json") {
        while (std::getline(std::cin, line)) {
            try {
                std::cout << mj::parse(line).dump() << '\n';
            } catch (const std::exception&) {
                std::cout << "ERR\n";
            }
        }
        return 0;
    }
    builtin();
    if (g_fail) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("all built-in checks passed\n");
    return 0;
}
