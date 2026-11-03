// replay_main.cpp — deterministic replay of a scripted scenario through the C++
// engine (the same code the live bot runs, minus sockets), for the differential
// test against the original Python engine (tests/diff_replay.py drives both).
//
//   ./build/replay scenario.json out.jsonl
//
// Scenario: {"t0", "config", "payloads", "positions", "events": [{"t","kind","raw"}],
//            "responses": [response templates]}. The k-th order POST is answered
// with responses[k % n] after its delay_ms, under a fake wall clock.
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <sstream>

#include "bot_core.hpp"
#include "clock.hpp"
#include "log.hpp"
#include "pyutil.hpp"

using mj::Value;

namespace {

struct Event {
    double t;
    uint64_t seq;
    int kind;  // 0 kalshi, 1 manifold, 2 order response
    std::string raw;
    int k;
};
struct Later {
    bool operator()(const Event& a, const Event& b) const { return a.t != b.t ? a.t > b.t : a.seq > b.seq; }
};
using Queue = std::priority_queue<Event, std::vector<Event>, Later>;

std::ofstream* g_out = nullptr;
void emit(const Value& v) { *g_out << v.dump() << '\n'; }

double num(const Value* v, double d) { return v && v->is_number() ? v->num() : d; }

class ReplayTransport : public bot::OrderTransport {
public:
    ReplayTransport(Queue& q, std::vector<Value> templates, uint64_t base_seq)
        : q_(q), templates_(std::move(templates)), base_seq_(base_seq) {}

    void post_bet(const std::string& body, double, net::RequestTiming* timing,
                  std::function<void(net::HttpResult)> cb) override {
        int k = posts_++;
        if (timing && timing->headers_at < 0) timing->headers_at = clk::perf_counter();
        if (timing) timing->reused = 1;
        Value ev = Value::object();
        ev.set("ev", Value::string("post"));
        ev.set("i", Value::integer(k));
        ev.set("t", Value::number(clk::g_fake_now));
        ev.set("body", mj::parse(body));
        emit(ev);
        const Value& tmpl = templates_[(size_t)k % templates_.size()];
        double delay = num(tmpl.get("delay_ms"), 50);
        pending_[k] = {body, std::move(cb)};
        q_.push(Event{clk::g_fake_now + delay / 1000.0, base_seq_ + (uint64_t)k, 2, "", k});
    }
    void get_order_pool(const std::string&, double, std::function<void(net::HttpResult)> cb) override {
        cb(net::HttpResult{net::HttpResult::Ok, 200, "{}", ""});
    }
    void get_read_pool(const std::string&, double, std::function<void(net::HttpResult)> cb) override {
        cb(net::HttpResult{net::HttpResult::Ok, 404, "", ""});
    }

    void deliver(int k) {
        auto it = pending_.find(k);
        if (it == pending_.end()) return;
        std::string body = it->second.first;
        auto cb = std::move(it->second.second);
        pending_.erase(it);
        const Value& tmpl = templates_[(size_t)k % templates_.size()];
        std::string kind = tmpl.get("kind")->s;
        net::HttpResult r;
        if (kind == "ok") {
            Value req = mj::parse(body);
            double requested = req.get("amount")->num();
            double spent = py::round_n(num(tmpl.get("fill"), 0) * requested, 2);
            double shares = py::round_n(spent * num(tmpl.get("shares_mult"), 2), 2);
            Value resp = Value::object();
            resp.set("betId", Value::string("bet" + std::to_string(k)));
            resp.set("amount", Value::number(spent));
            resp.set("shares", Value::number(shares));
            resp.set("isFilled", Value::boolean(num(tmpl.get("fill"), 0) >= 1.0));
            const Value* pa = tmpl.get("prob_after");
            if (pa && !pa->is_null()) resp.set("probAfter", *pa);
            r = net::HttpResult{net::HttpResult::Ok, 200, resp.dump(), ""};
        } else if (kind == "http") {
            r = net::HttpResult{net::HttpResult::Ok, (int)tmpl.get("status")->i, tmpl.get("message")->s, ""};
        } else if (kind == "timeout") {
            r = net::HttpResult{net::HttpResult::Timeout, 0, "", "timeout"};
        } else if (kind == "connect_error") {
            r = net::HttpResult{net::HttpResult::ConnectError, 0, "", "refused"};
        } else if (kind == "badjson") {
            r = net::HttpResult{net::HttpResult::Ok, 200, "not json", ""};
        } else if (kind == "list") {
            r = net::HttpResult{net::HttpResult::Ok, 200, "[1]", ""};
        }
        cb(r);
    }

private:
    Queue& q_;
    std::vector<Value> templates_;
    uint64_t base_seq_;
    int posts_ = 0;
    std::map<int, std::pair<std::string, std::function<void(net::HttpResult)>>> pending_;
};

Value opt(const std::optional<double>& v) { return v ? Value::number(*v) : Value::null(); }
Value opti(const std::optional<int64_t>& v) { return v ? Value::integer(*v) : Value::null(); }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: replay scenario.json out.jsonl\n";
        return 2;
    }
    logx::set_quiet(true);
    std::ifstream in(argv[1]);
    std::stringstream ss;
    ss << in.rdbuf();
    Value sc = mj::parse(ss.str());
    std::ofstream out(argv[2]);
    g_out = &out;

    clk::g_fake = true;
    clk::g_fake_now = sc.get("t0")->num();
    const Value& config = *sc.get("config");
    const Value* b = config.get("bot");
    Value bot_cfg = b ? *b : Value::object();
    auto f = [&](const char* k, double d) { const Value* v = bot_cfg.get(k); return v ? bot::py_float_value(v, k) : d; };
    auto i = [&](const char* k, int64_t d) { const Value* v = bot_cfg.get(k); return v ? bot::py_int_value(v, k) : d; };
    const Value* pb = bot_cfg.get("prefer_book");
    ps::KalshiHybridClient source("replay-key", "", (int)py::round0(f("max_kalshi_spread", 0.40) * 100),
                                  i("delta_fresh_ms", ps::DELTA_FRESH_MS), f("delta_move_threshold", ps::DELTA_MOVE_THRESHOLD),
                                  i("max_staleness_ms", bot::MAX_STALENESS_MS), f("extreme_high", bot::EXTREME_HIGH),
                                  i("max_feed_silence_ms", ps::MAX_FEED_SILENCE_MS), pb && pb->truthy(), true);

    Queue q;
    const Value& events = *sc.get("events");
    std::vector<Value> templates = sc.get("responses")->a;
    auto* transport = new ReplayTransport(q, templates, (uint64_t)events.a.size());
    bot::ManifoldOrderClient orders("replay-key", "test-user", std::unique_ptr<bot::OrderTransport>(transport));
    bot::ManifoldWebSocketClient ws;
    auto pairs = bot::build_pairs(config, "kalshi_ticker");
    bot::DualWebSocketArbitrageBot engine(source, ws, orders, std::move(pairs), config, "kalshi_ticker", "replay", true);

    for (const auto& [slug, data] : sc.get("payloads")->o) engine.install_contract(slug, data);
    for (const Value& p : sc.get("positions")->a)
        orders.position_cache[{p.a[0].s, p.a[1].s}] = p.a[2].i;

    ws.sender = [](const std::string&) {};
    bool subscribed = false;
    ws.subscribe_to_markets(engine.pairs(), &engine.contract_payloads, [&] { subscribed = true; });
    if (!subscribed) {
        std::cerr << "subscribe did not complete synchronously\n";
        return 1;
    }

    ps::PriceCallback cb = [&](ps::Src s, const std::string& id, const ps::AidOpt& aid, double prob, double t) {
        Value ev = Value::object();
        ev.set("ev", Value::string("cb"));
        ev.set("src", Value::string(s == ps::Src::Kalshi ? "kalshi" : s == ps::Src::KalshiSettled ? "kalshi_settled" : "manifold"));
        ev.set("id", Value::string(id));
        ev.set("aid", aid ? Value::string(*aid) : Value::null());
        ev.set("prob", Value::number(prob));
        ev.set("t", Value::number(t));
        emit(ev);
        engine.handle_price_update(s, id, aid, prob, t);
    };
    std::vector<std::string> ids;  // the live bot's per-connection lists, on one replay connection
    for (const auto& chunk : engine.subscription_chunks(SIZE_MAX))
        ids.insert(ids.end(), chunk.begin(), chunk.end());
    auto conn = source.open_replay(ids, cb);
    source.replay_connect(*conn);

    for (size_t k = 0; k < events.a.size(); ++k) {
        const Value& e = events.a[k];
        q.push(Event{e.get("t")->num(), (uint64_t)k, e.get("kind")->s == "kalshi" ? 0 : 1, e.get("raw")->s, 0});
    }
    while (!q.empty()) {
        Event e = q.top();
        q.pop();
        clk::g_fake_now = e.t;
        try {
            if (e.kind == 0) {
                ++conn->received;
                conn->feed->note_message();
                if (source.handle_frame(*conn, e.raw, e.t) == ps::KalshiHybridClient::FrameAction::Resync) {
                    source.replay_disconnect(*conn);
                    source.replay_connect(*conn);
                }
            } else if (e.kind == 1) {
                ws.handle_frame(e.raw, e.t, cb);
            } else {
                transport->deliver(e.k);
            }
        } catch (const std::exception& ex) {
            Value err = Value::object();
            err.set("ev", Value::string("error"));
            err.set("t", Value::number(e.t));
            err.set("what", Value::string(ex.what()));
            emit(err);
        }
        ev::Loop::get().drain_soon();
    }

    // Final state
    Value fin = Value::object();
    fin.set("ev", Value::string("final"));
    auto keyed = [](const std::map<bot::PosKey, int64_t>& m) {
        Value a = Value::array();
        for (const auto& [k, v] : m) {
            Value row = Value::array();
            row.push(Value::string(k.first));
            row.push(Value::string(k.second));
            row.push(Value::integer(v));
            a.push(std::move(row));
        }
        return a;
    };
    fin.set("position_cache", keyed(orders.position_cache));
    fin.set("pending_delta", keyed(orders.pending_delta));
    fin.set("engine", engine.debug_state());
    Value lp = Value::array();
    for (const auto& [k, v] : ws.last_prices) {
        Value row = Value::array();
        row.push(Value::string(k.slug));
        row.push(k.aid ? Value::string(*k.aid) : Value::null());
        row.push(Value::number(v));
        lp.push(std::move(row));
    }
    fin.set("last_prices", std::move(lp));
    Value pools = Value::array();
    for (const auto& [k, v] : ws.last_pools) {
        Value row = Value::array();
        row.push(Value::string(k.slug));
        row.push(k.aid ? Value::string(*k.aid) : Value::null());
        row.push(Value::number(v.first));
        row.push(Value::number(v.second));
        pools.push(std::move(row));
    }
    fin.set("last_pools", std::move(pools));
    Value movers = Value::array();
    for (const auto& [k, m] : ws.last_mover) {
        Value row = Value::array();
        row.push(Value::string(k.slug));
        row.push(k.aid ? Value::string(*k.aid) : Value::null());
        row.push(m.user ? Value::string(*m.user) : Value::null());
        row.push(Value::integer(m.dir));
        row.push(Value::integer(m.at_ms));
        movers.push(std::move(row));
    }
    fin.set("last_mover", std::move(movers));
    Value states = Value::object();
    for (const auto& [t, st] : source.market_states) {
        Value s = Value::object();
        s.set("is_live", Value::boolean(st->is_live));
        s.set("final", Value::boolean(st->final_));
        s.set("result", st->result ? Value::string(*st->result) : Value::null());
        s.set("settlement_value", opt(st->settlement_value));
        s.set("best_yes", opt(st->best_yes));
        s.set("best_no", opt(st->best_no));
        s.set("ticker_bid", opt(st->ticker_bid));
        s.set("ticker_ask", opt(st->ticker_ask));
        s.set("crossed_since_ms", opti(st->crossed_since_ms));
        s.set("delta_ts_ms", opti(st->delta_ts_ms));
        s.set("delta_recv_ms", opti(st->delta_recv_ms));
        s.set("ticker_ts_ms", opti(st->ticker_ts_ms));
        s.set("ticker_recv_ms", opti(st->ticker_recv_ms));
        s.set("active_until", opti(st->activity.active_until));
        auto ba = st->effective_bid_ask();
        Value eba = Value::null();
        if (ba) {
            eba = Value::array();
            eba.push(Value::number(ba->first));
            eba.push(Value::number(ba->second));
        }
        s.set("effective", std::move(eba));
        s.set("quote_source", Value::string(st->quote_source()));
        auto lv = [](const std::map<double, int64_t>& m) {
            Value a = Value::array();
            for (const auto& [p, sz] : m) {
                Value row = Value::array();
                row.push(Value::number(p));
                row.push(Value::integer(sz));
                a.push(std::move(row));
            }
            return a;
        };
        s.set("yes_levels", lv(st->yes_levels));
        s.set("no_levels", lv(st->no_levels));
        states.set(t, std::move(s));
    }
    fin.set("states", std::move(states));
    Value feed = Value::object();
    feed.set("last_msg_ms", opti(conn->feed->last_msg_ms));
    feed.set("last_pong_ms", opti(conn->feed->last_pong_ms));
    fin.set("feed", std::move(feed));
    emit(fin);
    return 0;
}
