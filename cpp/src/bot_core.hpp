// bot_core.hpp — transcription of bot_core.py: PairSettings, the Manifold
// WebSocket client, the Manifold order client (deps=[] bodies, one 503 retry,
// reservation settled to the fill, warm keep-alive pool), and the
// source-agnostic arbitrage engine DualWebSocketArbitrageBot.
//
// Python -> C++ map: asyncio loop -> ev::Loop; coroutine chains -> callbacks;
// fire-and-forget order tasks -> OrderTask with done callbacks; _AsyncLog ->
// logx; aiohttp sessions -> net::HttpPool behind OrderTransport (so the replay
// harness can drive the same engine without sockets).
#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "amm_sizing.hpp"
#include "json.hpp"
#include "net.hpp"
#include "price_sources.hpp"

namespace bot {

using ps::AidOpt;
using ps::Src;

inline constexpr int64_t MAX_STALENESS_MS = 5000;
inline constexpr double POSITION_CACHE_INTERVAL = 15.0;
inline constexpr double POSITION_REFRESH_INTERVAL = 60.0;
inline constexpr int64_t POSITION_REFRESH_BATCH = 50;
inline constexpr int64_t WS_MAX_BYTES = 16LL << 20;
inline constexpr double EXTREME_HIGH = 0.99;
inline constexpr double EXTREME_CONVERGE_HIGH = 0.97;
inline constexpr int64_t PARTNER_HYSTERESIS_MS = 1000;  // a leg cross re-forming this soon keeps its trust clock
inline constexpr double EXTREME_BET_MULTIPLIER = 40;
inline constexpr int ORDER_POOL_SIZE = 16;  // warm order connections kept ready
inline constexpr double ORDER_KEEPALIVE_SEC = 300;
inline constexpr double WARM_HEARTBEAT_SEC = 20;
inline constexpr double MANIFOLD_PING_SEC = 25;
inline constexpr double CLEAR_EDGE_EPS = 1e-4;

bool debug_enabled();

// (slug or contract id, answer id or None) — Python tuple keys
struct SlugAid {
    std::string slug;
    AidOpt aid;
    bool operator==(const SlugAid& o) const { return slug == o.slug && aid == o.aid; }
};
struct SlugAidHash {
    size_t operator()(const SlugAid& k) const {
        size_t h = std::hash<std::string>()(k.slug);
        return k.aid ? h ^ (std::hash<std::string>()(*k.aid) * 31 + 7) : h;
    }
};
using PosKey = std::pair<std::string, std::string>;  // (cid, aid or '')

// Python-semantics config access ------------------------------------------
double cfg_float(const mj::Value& pair, const mj::Value& bot_cfg, const char* key, double dflt);
std::string py_str(const mj::Value* v);            // str(x)
std::string py_slice(const std::string& s, size_t n);  // s[:n] by code point
bool config_bool(const mj::Value* v);              // bot_core._config_bool
double py_float_value(const mj::Value* v, const char* what);  // float(x) or throw
int64_t py_int_value(const mj::Value* v, const char* what);   // int(x) or throw
// bot_core._start_ms: a game start as epoch ms. Accepts ms, seconds, an ISO
// string (datetime.fromisoformat, as CPython's C parser reads it) or the text
// the YAML loader keeps for a PyYAML date/datetime; None when unparseable.
std::optional<int64_t> start_ms(const mj::Value* v);

extern const std::vector<std::string> PAIR_OVERRIDE_KEYS;

struct PairSettings {
    bool reverse;
    double min_edge, max_edge, max_bet, max_position, min_cooldown, edge_scale_start, size_edge_power;
    int max_spread_cents;
    double clearing_edge, extreme_multiplier;
    bool enable_amm;
    double amm_target_fraction;
    bool settle_slam;
    std::string tie_resolves;
    double limit_offset, min_edge_floor, adding_edge_tolerance, min_clear_room, min_bet_size;
    bool cap_clear_at_position;
    int64_t partner_trust_ms;  // two-leg pairs: follow the leg that changed last this long
    double partner_max_lead;   // ...at most this far past our own leg's quote
    double underdog_threshold, underdog_min_edge;  // underdog buys need more edge (off when 0)
    double edge_range, scale_divisor, min_trigger_edge, trigger_eps;

    PairSettings(const mj::Value& pair, const mj::Value& bot_cfg, int default_spread_cents);
    double bet_size(double edge) const;
    double scale_factor(int64_t position, bool is_adding) const;
    double required_edge(int64_t position, bool is_adding) const;
    double limit_price_adjustment(int64_t position, bool is_adding, double edge) const;
};

struct Pair {
    mj::Value raw;  // the config entry
    std::optional<std::string> sid, slug;
    AidOpt aid;
    std::string name_raw;  // pair.get('name', 'Unknown')
    std::unique_ptr<PairSettings> s;
    std::string pair_key;  // f"{sid}:{cid}:{aid or ''}"
    std::optional<std::string> partner;    // pair['_partner']: the other team's leg
    std::optional<int64_t> game_start_ms;  // pair['_game_start_ms']
};

// ---------------------------------------------------------------------------
// Transport seam: aiohttp in Python; HttpPool live, scripted in the replay.
// ---------------------------------------------------------------------------

class OrderTransport {
public:
    virtual ~OrderTransport() = default;
    virtual void post_bet(const std::string& body, double timeout, net::RequestTiming* timing,
                          std::function<void(net::HttpResult)> cb) = 0;
    virtual void get_order_pool(const std::string& path, double timeout, std::function<void(net::HttpResult)> cb) = 0;
    virtual void get_read_pool(const std::string& path, double timeout, std::function<void(net::HttpResult)> cb) = 0;
    virtual void close() {}
};

class HttpOrderTransport : public OrderTransport {
public:
    explicit HttpOrderTransport(const std::string& api_key);
    void post_bet(const std::string& body, double timeout, net::RequestTiming* timing,
                  std::function<void(net::HttpResult)> cb) override;
    void get_order_pool(const std::string& path, double timeout, std::function<void(net::HttpResult)> cb) override;
    void get_read_pool(const std::string& path, double timeout, std::function<void(net::HttpResult)> cb) override;
    void close() override;

private:
    net::HttpPool orders_, reads_;
};

// ---------------------------------------------------------------------------

struct OrderTask {
    bool done = false;
    std::optional<mj::Value> result;  // the bet response dict, or None
    std::vector<std::function<void(OrderTask&)>> callbacks;
    // timing (perf_counter seconds)
    double queued_at = 0, started_at = -1, end = 0;
    net::RequestTiming rt;
    int attempts = 0;
    void add_done_callback(std::function<void(OrderTask&)> f);
};
using OrderHandle = std::shared_ptr<OrderTask>;

struct OrderMetrics {
    std::string contract_id, answer_id, status;
    int http_status = 0;
    double requested_amount = 0;
    std::optional<double> amount, shares;
    double queue_ms = 0, pool_wait_ms = 0;
    std::optional<double> write_ms;
    double response_ms = 0;
    int connection_reused = -1;
    int attempts = 0;
};

class ManifoldOrderClient {
public:
    ManifoldOrderClient(std::string api_key, std::string user_id, std::unique_ptr<OrderTransport> transport);

    std::map<PosKey, int64_t> position_cache, pending_delta;
    std::map<PosKey, int> active_orders;
    std::map<PosKey, int64_t> position_versions;
    std::set<OrderTask*> pending_tasks;
    std::deque<OrderMetrics> recent_orders;  // maxlen 128
    bool closing = false;
    std::string user_id;

    void log(std::string line);
    void warm_pool(int n, bool verbose, std::function<void()> done);
    int64_t get_cached_position(const std::string& cid, const AidOpt& aid) const;
    void record_trade(const std::string& cid, const std::string& aid, bool yes, double amount);
    void refresh_all_positions(const std::vector<Pair*>& pairs, const std::unordered_map<std::string, std::string>& slug_to_id,
                               std::function<void()> done);
    OrderHandle place_order(const std::string& cid, double amount, double limit_prob, bool yes, const AidOpt& aid,
                            int64_t expires_ms = 1);
    void close(std::function<void()> done);
    OrderTransport& transport() { return *transport_; }

    // C++ addition: ORDER_TRACE_FILE=<path> appends one JSON line per order
    // (after its response — never on the send path) for latency analysis.
    std::function<void(const mj::Value&)> trace_sink;

private:
    void fetch_position(const std::string& cid, const AidOpt& aid, std::function<void(std::optional<int64_t>)> cb);
    void attempt(const OrderHandle& task, std::shared_ptr<struct OrderCtx> ctx, int n);
    void finish(const OrderHandle& task, std::shared_ptr<struct OrderCtx> ctx, std::optional<mj::Value> result);
    std::unique_ptr<OrderTransport> transport_;
};

// ---------------------------------------------------------------------------

class ManifoldWebSocketClient {
public:
    using Payloads = std::map<std::string, mj::Value>;

    explicit ManifoldWebSocketClient(size_t max_size = (size_t)WS_MAX_BYTES);

    std::string ws_url = "wss://api.manifold.markets/ws";
    std::unordered_map<SlugAid, double, SlugAidHash> last_prices;
    std::unordered_map<SlugAid, std::pair<double, double>, SlugAidHash> last_pools;
    bool track_movers = false;
    struct Mover {
        std::optional<std::string> user;
        int dir;
        int64_t at_ms;
    };
    std::unordered_map<SlugAid, Mover, SlugAidHash> last_mover;
    bool bet_trigger = false;
    int64_t txid = 1;
    std::unordered_map<std::string, std::string> slug_to_id, id_to_slug;

    using Callback = ps::PriceCallback;

    // live
    void connect(std::function<void(bool)> done);
    void subscribe_to_markets(const std::vector<Pair*>& pairs, Payloads* payloads, std::function<void()> done);
    void receive_messages(Callback cb, const std::vector<Pair*>& pairs);
    void close();

    // shared live / replay. Throws on frames that raise in Python.
    void handle_frame(std::string_view raw, double recv_time, const Callback& cb);
    // Replay: what subscribe_to_markets sends goes here instead of a socket.
    std::function<void(const std::string&)> sender;
    // Replay hook into the HTTP the WS client would do (never used when payloads cover every slug).
    std::function<void(const std::string& path, std::function<void(net::HttpResult)>)> http_get;

    std::optional<double> get_cached_price(const std::string& slug, const AidOpt& aid) const;
    std::optional<std::pair<double, double>> get_cached_pool(const std::string& slug, const AidOpt& aid) const;

private:
    enum class TopicType { Contract, Answers, NewBet, Skip };
    struct TopicInfo {
        TopicType type;
        std::string slug;
    };
    void fetch_initial_prices(const std::vector<Pair*>& pairs, Payloads* payloads, std::function<void()> done);
    void finish_subscribe(const std::vector<Pair*>& pairs, Payloads* payloads, std::function<void()> done);
    void loop_top();
    void listening();
    void schedule_keepalive();
    void send(const std::string& s);
    void on_raw(std::string_view raw);
    void on_disconnect(const std::string& why);
    void teardown_and_retry();
    void get(const std::string& path, std::function<void(net::HttpResult)> cb);
    static std::optional<double> pick_prob(const mj::Value& obj);

    size_t max_size_;
    bool connected_ = false, listening_ = false;
    std::deque<std::string> pending_frames_;  // frames that arrive before receive_messages()
    std::unordered_map<std::string, TopicInfo> topic_cache_;
    std::unordered_map<SlugAid, double, SlugAidHash> bet_price_ct_;
    std::shared_ptr<net::WsClient> ws_;
    std::unique_ptr<net::HttpPool> http_;
    Callback cb_;
    std::vector<Pair*> pairs_;
    ev::Timer pinger_, retry_;
    uint64_t gen_ = 0;
    bool closed_ = false;
};

// ---------------------------------------------------------------------------

class DualWebSocketArbitrageBot {
public:
    DualWebSocketArbitrageBot(ps::PriceSource& source, ManifoldWebSocketClient& manifold_ws,
                              ManifoldOrderClient& manifold_orders, std::vector<std::unique_ptr<Pair>> market_pairs,
                              const mj::Value& config, std::string source_key = "kalshi_ticker",
                              std::string banner = "DUAL WEBSOCKET ARBITRAGE BOT", bool quiet = false);

    void handle_price_update(Src source, const std::string& identifier, const AidOpt& aid, double prob, double recv_ts);
    void check_arb(Pair& pair, double source_prob, double manifold_prob, double recv_ts);
    void handle_settlement(const std::string& sid, double settlement_value, double recv_ts);

    // Live: startup sequence then the loops; returns when the loop stops.
    // False if startup failed. Python crashes there (subscribing on a socket
    // that never connected), exiting 1, which systemd Restart=on-failure acts on.
    bool run();
    // Replay: install the startup state run() would fetch.
    void install_contract(const std::string& slug, const mj::Value& data);
    // Replay: engine-private state for the differential comparison.
    mj::Value debug_state() const;
    const std::vector<std::string>& source_ids() const { return source_ids_; }
    // Source ids split into per-connection lists; a partner leg rides the same
    // connection as the leg it pairs with (arrival order is only exact on one socket).
    std::vector<std::vector<std::string>> subscription_chunks(size_t chunk) const;

    std::vector<Pair*> pairs() const;
    int64_t total_trades = 0;
    int64_t fades_blocked = 0;
    bool dry_run;
    std::unordered_map<std::string, std::string> slug_to_contract_id;
    ManifoldWebSocketClient::Payloads contract_payloads;

private:
    struct Hold {
        bool yes;
        double level;
        int64_t expires_ms;
        uint64_t seq;
        int in_flight;
        std::optional<double> furthest;
        bool unknown;
    };

    bool is_adding_to_position(int64_t position, bool yes) const {
        return (position >= 0 && yes) || (position <= 0 && !yes);
    }
    std::optional<double> amm_bet_size(Pair& pair, PairSettings& s, int64_t position, double mp, double limit_prob);
    std::optional<double> bet_size_for_trade(Pair& pair, PairSettings& s, int64_t position, bool is_adding,
                                             double edge, double mp, double base_limit, double limit_prob);
    std::optional<double> settled_fair(const PairSettings& s, double settlement_value,
                                       const std::optional<std::string>& result) const;
    std::optional<double> settle_bet_size(Pair& pair, PairSettings& s) const;
    OrderHandle submit_order(const std::string& cid, double amount, double limit_prob, bool yes, const AidOpt& aid,
                             const SlugAid* hold_key = nullptr, bool join_hold = false);
    void hold_own_order(const SlugAid& key, bool yes, double limit_prob, const OrderHandle& task, bool join);
    std::optional<std::pair<double, double>> last_cent_order(const std::string& cid, const AidOpt& aid, PairSettings& s,
                                                             bool yes, double mp, double limit_prob, double base_limit);
    double expected_manifold_price(const SlugAid& key, double mp);
    void release_own_order(const SlugAid& key, double prob);
    bool fade_blocked(const std::string& sid, const std::string& cid, const AidOpt& aid, bool yes, const std::string& name,
                      const std::optional<std::string>& partner);
    using Couple = std::pair<std::string, std::string>;  // (pair's own leg, its partner)
    struct LegQuote {
        double bid, ask;
        const char* via;
        bool no_adding;
    };
    void note_leg(const std::string& tk, const std::vector<Couple>& couples, int64_t now);
    bool pair_live(const Pair& pair, const std::string& sid) const;
    LegQuote two_leg_quote(Pair& pair, PairSettings& s, const std::string& sid, const std::string& partner,
                           const ps::BidAsk& own);
    void on_contract_data(const std::string& slug, const mj::Value& data);
    void log(std::string line);

    void cache_contract_ids(std::function<void()> done);
    std::vector<Pair*> next_refresh_batch();
    void position_refresh_tick();
    void order_warm_tick();
    void start_loops();

    ps::PriceSource& source_;
    ManifoldWebSocketClient& manifold_ws_;
    ManifoldOrderClient& manifold_orders_;
    std::vector<std::unique_ptr<Pair>> market_pairs_;
    std::string source_key_, banner_;
    mj::Value bot_cfg_;

    double position_cache_interval_, position_refresh_interval_;
    int64_t position_refresh_batch_;
    size_t refresh_cursor_ = 0;
    double extreme_high_, extreme_low_, extreme_converge_high_, extreme_converge_low_;
    double min_clear_room_, limit_offset_, min_order_room_;
    int64_t own_order_hold_ms_;
    std::unordered_map<SlugAid, Hold, SlugAidHash> own_orders_;
    uint64_t own_order_seq_ = 0;
    std::unordered_set<std::string> no_fade_users_;
    int64_t no_fade_window_ms_;
    std::unordered_map<std::string, ps::BidAsk> quote_seen_;
    std::unordered_map<std::string, int64_t> quote_changed_ms_;
    double last_cent_max_bet_;
    bool skip_positions_;
    amm::AmmSizer amm_sizer_;
    amm::MarketLiquidityConfig liq_config_;
    std::unordered_map<std::string, std::vector<Pair*>> source_to_pairs_;
    std::vector<std::string> source_ids_;  // insertion order of source_to_pairs
    std::unordered_map<SlugAid, std::vector<Pair*>, SlugAidHash> contract_to_pairs_;
    std::unordered_map<std::string, double> last_trade_time_;
    std::set<PosKey> settled_answers_;
    std::vector<Pair*> amm_pairs_;
    // Partner legs (pair: partner_ticker)
    std::unordered_map<std::string, std::vector<Pair*>> partner_to_pairs_;
    std::unordered_map<std::string, std::vector<Couple>> leg_couples_;
    std::unordered_map<std::string, std::optional<ps::BidAsk>> leg_seen_;
    std::unordered_map<std::string, int64_t> leg_order_;  // arrival rank of each leg's last change
    int64_t leg_tick_ = 0;
    std::map<Couple, int64_t> legs_crossed_since_, legs_uncrossed_at_;
    // partner_quotes: '+B', '>B', '>A', '=AB', '-B'
    enum { PQ_PLUS_B, PQ_GT_B, PQ_GT_A, PQ_EQ_AB, PQ_MINUS_B, PQ_N };
    int64_t partner_quotes_[PQ_N] = {0, 0, 0, 0, 0};
    bool partner_trust_ok_ = false;
    int64_t n_partner_ = 0, unused_partner_ = 0;
    std::vector<std::string> bad_partner_;
    ev::Timer warm_timer_, refresh_timer_;
};

// Config + credentials + runner helpers ------------------------------------
mj::Value load_config(const char* path, const std::string& dflt = "config_websocket.yaml");
void load_dotenv();
std::optional<std::pair<std::string, std::string>> kalshi_credentials();
std::optional<std::pair<std::string, std::string>> manifold_credentials();
std::vector<std::unique_ptr<Pair>> build_pairs(const mj::Value& config, const std::string& source_key);
int run_bot(ps::PriceSource& source, const mj::Value& config, const std::string& source_key, const std::string& banner);

}  // namespace bot
