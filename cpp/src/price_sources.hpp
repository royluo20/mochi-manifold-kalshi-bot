// price_sources.hpp — transcription of the parts of price_sources.py the hybrid
// bot runs: FeedLiveness, QuoteActivity, Kalshi RSA-PSS auth, KalshiMarketState
// (orderbook_delta book + ticker quote + quote selection) and KalshiHybridClient
// (both channels + market_lifecycle_v2 on one multiplexed connection, shared
// seq-gap detection, crossed-book resync, heartbeat, reconnect with backoff).
#pragma once

#include <openssl/evp.h>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "json.hpp"
#include "loop.hpp"
#include "net.hpp"

namespace ps {

inline constexpr const char* KALSHI_WS_URL = "wss://api.elections.kalshi.com/trade-api/ws/v2";
inline constexpr const char* KALSHI_WS_PATH = "/trade-api/ws/v2";

inline constexpr int64_t DELTA_FRESH_MS = 3000;
inline constexpr double DELTA_MOVE_THRESHOLD = 0.02;
inline constexpr double HEARTBEAT_SEC = 3.0;
inline constexpr double HEARTBEAT_TIMEOUT_SEC = 4.0;
inline constexpr int64_t MAX_FEED_SILENCE_MS = 8000;
inline constexpr int ACTIVE_MIN_TICKS = 3;
inline constexpr int64_t ACTIVE_WINDOW_MS = 60000;
inline constexpr int64_t ACTIVE_HOLD_MS = 300000;
inline constexpr int64_t CROSSED_RESYNC_MS = 2000;
inline constexpr int SOURCE_YIELD_EVERY = 64;
inline constexpr int64_t MAX_STALENESS_MS = 5000;  // bot_core.MAX_STALENESS_MS
inline constexpr double EXTREME_HIGH = 0.99;        // bot_core.EXTREME_HIGH

bool is_final_event(const std::string& e);     // determined, settled
bool is_terminal_event(const std::string& e);  // determined, settled, deactivated

using BidAsk = std::pair<double, double>;

// Per-CONNECTION proof that the socket is delivering.
struct FeedLiveness {
    std::optional<int64_t> last_msg_ms, last_pong_ms;
    std::optional<double> last_rtt_ms;
    void note_message();
    void note_pong(std::optional<double> rtt_ms = std::nullopt);
    void mark_down();
    std::optional<int64_t> silence_ms() const;
    bool alive_within(int64_t max_silence_ms) const;
};

// Is THIS book behaving like a live in-game market, or is it just quiet?
struct QuoteActivity {
    std::deque<int64_t> times;  // maxlen 64
    std::optional<int64_t> active_until;
    int min_ticks = ACTIVE_MIN_TICKS;
    int64_t window_ms = ACTIVE_WINDOW_MS;
    int64_t hold_ms = ACTIVE_HOLD_MS;
    void note_change();
    bool is_active() const;
    void reset();
};

class KalshiAuth {
public:
    KalshiAuth(std::string api_key_id, std::string private_key_pem);
    ~KalshiAuth();
    net::Headers create_ws_headers();  // signs {timestamp}GET/trade-api/ws/v2

protected:
    std::string api_key_id_;
    std::string private_key_str_;
    EVP_PKEY* pkey_ = nullptr;
    void load_private_key();
};

// Kalshi level size -> whole hundredths of a contract (throws on nan/inf, as
// Python's int(round(nan)) raises).
std::optional<int64_t> size_units(const mj::Value* x);
std::optional<double> parse_level_price(const mj::Value* p, bool is_dollars);
std::optional<int64_t> ticker_ts_to_ms(const mj::Value* ts);

class KalshiMarketState {
public:
    KalshiMarketState(int max_spread_cents, int64_t delta_fresh_ms, double delta_move_threshold,
                      int64_t max_staleness_ms, double extreme_high, std::shared_ptr<FeedLiveness> feed,
                      int64_t max_feed_silence_ms, bool prefer_book);

    // delta book: price(prob) -> size in hundredths
    std::map<double, int64_t> yes_levels, no_levels;
    std::optional<int64_t> delta_recv_ms, delta_ts_ms;
    std::optional<int64_t> seq;
    std::optional<double> ticker_bid, ticker_ask;
    std::optional<int64_t> ticker_recv_ms, ticker_ts_ms;
    bool is_live = true;
    int max_spread_cents;
    int64_t delta_fresh_ms;
    double delta_move_threshold;
    int64_t max_staleness_ms;
    double extreme_high, extreme_low;
    std::shared_ptr<FeedLiveness> feed;
    QuoteActivity activity;
    int64_t max_feed_silence_ms;
    bool prefer_book;
    std::optional<std::string> result;
    std::optional<double> settlement_value;
    bool final_ = false;
    std::optional<double> best_yes, best_no;
    std::optional<int64_t> crossed_since_ms;

    void recompute_top();
    void update_crossed();
    bool apply_snapshot(const mj::Value& yes_arr, const mj::Value& no_arr, std::optional<int64_t> ts_ms,
                        bool is_dollars);
    bool apply_delta(const std::string& side, std::optional<double> price, std::optional<double> delta,
                     std::optional<int64_t> ts_ms);
    bool update_ticker(std::optional<double> bid, std::optional<double> ask, std::optional<int64_t> ts_ms);
    bool handle_lifecycle(const std::string& event_type, const std::optional<std::string>& result,
                          std::optional<double> settlement_value);
    void reset_book();

    std::optional<BidAsk> delta_bid_ask(bool require_recent = true) const;
    std::optional<BidAsk> ticker_bid_ask() const;
    std::pair<const char*, std::optional<BidAsk>> book_first() const;
    std::optional<BidAsk> effective_bid_ask() const;
    std::optional<double> get_mid_prob() const;
    std::optional<BidAsk> get_bid_ask_prob() const { return effective_bid_ask(); }
    std::optional<int> get_spread_cents() const;
    bool is_spread_ok() const;
    std::string quote_source() const;
};

enum class Src { Kalshi, KalshiSettled, Manifold };
using AidOpt = std::optional<std::string>;
// callback(source, id, aid, prob, recv_ts) — handle_price_update's signature
using PriceCallback = std::function<void(Src, const std::string&, const AidOpt&, double, double)>;

// Kalshi connection selection (C++ only; the Python engine has no counterpart).
// A WebSocket's delivery speed can differ between connections and tends to stay
// the same for the life of a connection. So after startup the client opens `candidates` extra connections on its most active tickers,
// measures each one's delay behind the earliest copy of every update, and moves
// its busiest sockets onto the fastest connections (same TCP connection: the
// probe subscription is dropped, the real one added). It re-checks every
// `recheck_min` with fewer candidates, and runs a full probe again after a
// selected connection drops.
struct ConnProbeConfig {
    int candidates = 0;           // 0 = off
    double warmup_sec = 20;       // count per-ticker activity before choosing probe tickers
    double seconds = 60;          // minimum probe length
    double max_seconds = 180;     // stop here even without min_updates
    int probe_tickers = 12;       // most active tickers the candidates subscribe to
    int min_updates = 300;        // updates seen on most connections before deciding
    double recheck_min = 60;      // 0 = never re-check
    int recheck_candidates = 16;
    double margin_ms = 2.0;       // a candidate must beat a measured socket's p90 by this much
    int max_total_conns = 150;    // Kalshi documents 200 connections per user
};

// One probed connection's delay behind the earliest copy, in ms.
struct ProbeStat {
    int id = -1;
    size_t n = 0;
    double p50 = 0, p90 = 0;
};
// Stats for every id with at least min_share of the best-covered id's samples,
// best first (p90, then p50). Pure, so the unit tests cover it.
std::vector<ProbeStat> rank_probe(const std::vector<std::vector<double>>& offsets, double min_share = 0.6);
// Identity of one orderbook_delta across connections (each socket has its own sid/seq).
std::string delta_key(const mj::Value& payload);

// The engine's view of a price source (price_sources' common interface).
class PriceSource {
public:
    virtual ~PriceSource() = default;
    virtual std::optional<double> get_cached_price(const std::string& id) = 0;
    virtual std::optional<BidAsk> get_cached_bid_ask(const std::string& id) = 0;
    virtual KalshiMarketState* get_state(const std::string& id) = 0;
    virtual void listen(const std::vector<std::string>& ids, PriceCallback cb) = 0;
    virtual size_t max_ids_per_conn() const = 0;
    // getattr(source, 'prefer_book', False): only the hybrid client has it.
    virtual bool prefer_book() const { return false; }
    virtual void close() {}
};

class KalshiHybridClient : public KalshiAuth, public PriceSource {
public:
    KalshiHybridClient(std::string api_key_id, std::string private_key_pem, int max_spread_cents = 40,
                       int64_t delta_fresh_ms = DELTA_FRESH_MS, double delta_move_threshold = DELTA_MOVE_THRESHOLD,
                       int64_t max_staleness_ms = MAX_STALENESS_MS, double extreme_high = EXTREME_HIGH,
                       int64_t max_feed_silence_ms = MAX_FEED_SILENCE_MS, bool prefer_book = false,
                       bool quiet = false);

    std::optional<double> get_cached_price(const std::string& id) override;
    std::optional<BidAsk> get_cached_bid_ask(const std::string& id) override;
    KalshiMarketState* get_state(const std::string& id) override;
    void listen(const std::vector<std::string>& ids, PriceCallback cb) override;
    size_t max_ids_per_conn() const override { return 90; }
    bool prefer_book() const override { return prefer_book_; }
    // Enable connection selection; call before listen(). Off unless candidates > 0.
    void set_conn_probe(const ConnProbeConfig& cfg) { probe_cfg_ = cfg; }

    // One multiplexed connection's state (the body of listen()'s loop).
    struct Conn {
        int index = -1;                       // position in conns_
        bool selected = false;                // running on a connection the probe chose
        std::unordered_set<int64_t> ignore_sids;  // the probe subscription left on an adopted socket
        std::vector<std::string> tickers;
        std::shared_ptr<FeedLiveness> feed;
        PriceCallback cb;
        std::string subscribe_msg;
        std::shared_ptr<net::WsClient> ws;
        std::optional<int64_t> conn_seq;
        double backoff = 1.0;
        int received = 0;
        bool hb_running = false;
        ev::Timer hb_timer, backoff_timer;
        uint64_t generation = 0;
    };
    enum class FrameAction { Continue, Resync };

    // Shared by the live loop and the replay harness. Throws on the frames that
    // raise in Python (the live loop then reconnects with backoff).
    FrameAction handle_frame(Conn& c, std::string_view raw, double recv_time);
    // Replay: register a connection's markets (listen()'s setup, no socket).
    std::shared_ptr<Conn> open_replay(const std::vector<std::string>& tickers, PriceCallback cb);
    // Replay: the loop-top work of a (re)connect: reset books, note the subscribe.
    void replay_connect(Conn& c);
    void replay_disconnect(Conn& c);  // finally: feed.mark_down()

    std::unordered_map<std::string, std::unique_ptr<KalshiMarketState>> market_states;
    std::vector<std::shared_ptr<FeedLiveness>> feeds;
    std::string url = KALSHI_WS_URL;  // C++ extension: KALSHI_WS_URL env overrides

private:
    struct Probe;
    std::shared_ptr<Conn> setup(const std::vector<std::string>& tickers, PriceCallback cb);
    void start_connection(const std::shared_ptr<Conn>& c);
    std::shared_ptr<std::string> install_handlers(const std::shared_ptr<Conn>& c, uint64_t gen);
    void on_conn_open(const std::shared_ptr<Conn>& c, uint64_t gen);
    net::WsOptions ws_options(std::string* err);
    void schedule_probe(double delay_sec, int candidates, bool full);
    void start_probe(int candidates, bool full);
    void probe_note_incumbent(const Conn& c, const std::string& mt, const mj::Value& payload, double t);
    void probe_tick();
    void finish_probe();
    void abandon_probe(const std::string& why, double retry_sec);
    void adopt(const std::shared_ptr<Conn>& c, std::shared_ptr<net::WsClient> ws, std::unordered_set<int64_t> ignore);

    ConnProbeConfig probe_cfg_;
    std::shared_ptr<Probe> probe_;
    bool probe_armed_ = false;
    bool counting_ = false;  // per-ticker delta counts feed the probe's ticker choice and socket priority
    std::unordered_map<std::string, uint32_t> delta_counts_;
    ev::Timer probe_timer_;
    double last_full_probe_ = -1e18;
    void heartbeat(const std::shared_ptr<Conn>& c, uint64_t gen);
    void on_disconnected(const std::shared_ptr<Conn>& c, bool error, const std::string& why);

    bool prefer_book_;
    int max_spread_cents_;
    int64_t delta_fresh_ms_;
    double delta_move_threshold_;
    int64_t max_staleness_ms_;
    double extreme_high_;
    int64_t max_feed_silence_ms_;
    std::vector<std::shared_ptr<Conn>> conns_;
};

}  // namespace ps
