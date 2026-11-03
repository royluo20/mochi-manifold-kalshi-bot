#include "price_sources.hpp"

#include <cctype>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <numeric>
#include <unordered_set>

#include "clock.hpp"
#include "log.hpp"
#include "pyutil.hpp"

namespace ps {

namespace {
bool debug_on() {
    static const bool on = [] {
        const char* v = std::getenv("BOT_DEBUG");
        if (!v) return false;
        std::string s(v);
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s == "1" || s == "true" || s == "yes" || s == "on";
    }();
    return on;
}

// repr() of a parsed JSON value, for log lines that print a Python dict
// (f"{payload}"). Exact for ASCII; non-ASCII is escaped only for the C0/C1
// controls and a few invisible format characters (Python consults the full
// Unicode database for str.isprintable).
void repr_str(const std::string& s, std::string& out) {
    char q = (s.find('\'') != std::string::npos && s.find('"') == std::string::npos) ? '"' : '\'';
    out += q;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp = c;
        size_t len = 1;
        if (c >= 0xC0 && c < 0xE0 && i + 1 < s.size()) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); len = 2; }
        else if (c >= 0xE0 && c < 0xF0 && i + 2 < s.size()) {
            cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); len = 3;
        } else if (c >= 0xF0 && i + 3 < s.size()) {
            cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); len = 4;
        }
        char buf[16];
        if (cp == '\\' || cp == (uint32_t)q) { out += '\\'; out += (char)cp; }
        else if (cp == '\t') out += "\\t";
        else if (cp == '\n') out += "\\n";
        else if (cp == '\r') out += "\\r";
        else if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0xA0) || cp == 0xAD) {
            std::snprintf(buf, sizeof buf, "\\x%02x", cp); out += buf;
        } else if ((cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2064) ||
                   cp == 0xFEFF) {
            std::snprintf(buf, sizeof buf, "\\u%04x", cp); out += buf;
        } else {
            out.append(s, i, len);
        }
        i += len;
    }
    out += q;
}

void repr_to(const mj::Value& v, std::string& out) {
    switch (v.type) {
        case mj::Type::Null: out += "None"; break;
        case mj::Type::Bool: out += v.b ? "True" : "False"; break;
        case mj::Type::Int: out += std::to_string(v.i); break;
        case mj::Type::Float: out += py::repr(v.d); break;
        case mj::Type::String: repr_str(v.s, out); break;
        case mj::Type::Array:
            out += '[';
            for (size_t k = 0; k < v.a.size(); ++k) {
                if (k) out += ", ";
                repr_to(v.a[k], out);
            }
            out += ']';
            break;
        case mj::Type::Object:
            out += '{';
            for (size_t k = 0; k < v.o.size(); ++k) {
                if (k) out += ", ";
                repr_str(v.o[k].first, out);
                out += ": ";
                repr_to(v.o[k].second, out);
            }
            out += '}';
            break;
    }
}

std::string py_repr(const mj::Value& v) {
    std::string out;
    repr_to(v, out);
    return out;
}

// bot_core.to_float
std::optional<double> to_float(const mj::Value* v) {
    if (!v) return std::nullopt;
    switch (v->type) {
        case mj::Type::Int: return (double)v->i;
        case mj::Type::Float: return v->d;
        case mj::Type::Bool: return v->b ? 1.0 : 0.0;
        case mj::Type::String: return py::float_from_str(v->s);
        default: return std::nullopt;
    }
}

bool valid_prob(const std::optional<double>& x) { return x && *x >= 0.0 && *x <= 1.0; }

// Python `a or b` on dict lookups
const mj::Value* py_or(const mj::Value* a, const mj::Value* b) { return (a && a->truthy()) ? a : b; }
}  // namespace

bool is_final_event(const std::string& e) { return e == "determined" || e == "settled"; }
bool is_terminal_event(const std::string& e) { return e == "determined" || e == "settled" || e == "deactivated"; }

// ---------------------------------------------------------------------------
// FeedLiveness / QuoteActivity
// ---------------------------------------------------------------------------

void FeedLiveness::note_message() { last_msg_ms = clk::now_ms(); }
void FeedLiveness::note_pong(std::optional<double> rtt_ms) {
    last_pong_ms = clk::now_ms();
    if (rtt_ms) last_rtt_ms = rtt_ms;
}
void FeedLiveness::mark_down() {
    last_msg_ms.reset();
    last_pong_ms.reset();
}
std::optional<int64_t> FeedLiveness::silence_ms() const {
    if (!last_msg_ms && !last_pong_ms) return std::nullopt;
    int64_t latest = std::max(last_msg_ms.value_or(INT64_MIN), last_pong_ms.value_or(INT64_MIN));
    return clk::now_ms() - latest;
}
bool FeedLiveness::alive_within(int64_t max_silence_ms) const {
    auto s = silence_ms();
    return s && *s <= max_silence_ms;
}

void QuoteActivity::note_change() {
    int64_t t = clk::now_ms();
    times.push_back(t);
    if (times.size() > 64) times.pop_front();  // deque(maxlen=64)
    int64_t cutoff = t - window_ms;
    while (!times.empty() && times.front() < cutoff) times.pop_front();
    if ((int)times.size() >= min_ticks) active_until = t + hold_ms;
}
bool QuoteActivity::is_active() const { return active_until && clk::now_ms() < *active_until; }
void QuoteActivity::reset() {
    times.clear();
    active_until.reset();
}

// ---------------------------------------------------------------------------
// Kalshi auth
// ---------------------------------------------------------------------------

KalshiAuth::KalshiAuth(std::string api_key_id, std::string private_key_pem)
    : api_key_id_(std::move(api_key_id)), private_key_str_(std::move(private_key_pem)) {}

KalshiAuth::~KalshiAuth() {
    if (pkey_) EVP_PKEY_free(pkey_);
}

void KalshiAuth::load_private_key() {
    if (pkey_) return;
    // key_str.strip().replace('\\n', '\n')
    std::string k = private_key_str_;
    size_t b = 0, e = k.size();
    while (b < e && py::is_space(k[b])) ++b;
    while (e > b && py::is_space(k[e - 1])) --e;
    k = k.substr(b, e - b);
    std::string out;
    for (size_t i = 0; i < k.size(); ++i) {
        if (k[i] == '\\' && i + 1 < k.size() && k[i + 1] == 'n') {
            out.push_back('\n');
            ++i;
        } else {
            out.push_back(k[i]);
        }
    }
    BIO* bio = BIO_new_mem_buf(out.data(), (int)out.size());
    pkey_ = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey_) throw std::runtime_error("could not load Kalshi private key");
}

net::Headers KalshiAuth::create_ws_headers() {
    load_private_key();
    std::string timestamp = std::to_string(clk::now_ms());
    std::string msg = timestamp + "GET" + KALSHI_WS_PATH;
    // RSA-PSS, MGF1(SHA256), salt = PSS.MAX_LENGTH, SHA256
    EVP_MD_CTX* md = EVP_MD_CTX_new();
    EVP_PKEY_CTX* pctx = nullptr;
    std::string sig;
    if (EVP_DigestSignInit(md, &pctx, EVP_sha256(), nullptr, pkey_) == 1 &&
        EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) == 1 &&
        EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_MAX) == 1 &&
        EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, EVP_sha256()) == 1 &&
        EVP_DigestSignUpdate(md, msg.data(), msg.size()) == 1) {
        size_t len = 0;
        EVP_DigestSignFinal(md, nullptr, &len);
        std::string raw(len, '\0');
        if (EVP_DigestSignFinal(md, (unsigned char*)raw.data(), &len) == 1) {
            raw.resize(len);
            sig = net::base64((const unsigned char*)raw.data(), raw.size());
        }
    }
    EVP_MD_CTX_free(md);
    if (sig.empty()) throw std::runtime_error("Kalshi signature failed");
    return {{"KALSHI-ACCESS-KEY", api_key_id_},
            {"KALSHI-ACCESS-SIGNATURE", sig},
            {"KALSHI-ACCESS-TIMESTAMP", timestamp}};
}

// ---------------------------------------------------------------------------
// Book helpers
// ---------------------------------------------------------------------------

std::optional<int64_t> size_units(const mj::Value* x) {
    auto f = to_float(x);
    if (!f) return std::nullopt;
    double v = *f * 100;
    if (!std::isfinite(v)) throw std::runtime_error("cannot convert float NaN/inf to integer");
    return py::trunc_i64(py::round0(v));
}

static std::optional<int64_t> size_units_d(std::optional<double> f) {
    if (!f) return std::nullopt;
    double v = *f * 100;
    if (!std::isfinite(v)) throw std::runtime_error("cannot convert float NaN/inf to integer");
    return py::trunc_i64(py::round0(v));
}

std::optional<double> parse_level_price(const mj::Value* p, bool is_dollars) {
    auto f = to_float(p);
    if (!f) return std::nullopt;
    double v = is_dollars ? *f : *f / 100.0;
    if (v >= 0.0 && v <= 1.0) return py::round_n(v, 4);
    return std::nullopt;
}

std::optional<int64_t> ticker_ts_to_ms(const mj::Value* ts) {
    auto f = to_float(ts);
    if (!f) return std::nullopt;
    if (!std::isfinite(*f)) throw std::runtime_error("cannot convert float NaN/inf to integer");
    int64_t i = py::trunc_i64(*f);  // int() truncates
    return i < 1000000000000LL ? i * 1000 : i;
}

// ---------------------------------------------------------------------------
// KalshiMarketState
// ---------------------------------------------------------------------------

KalshiMarketState::KalshiMarketState(int max_spread_cents, int64_t delta_fresh_ms, double delta_move_threshold,
                                     int64_t max_staleness_ms, double extreme_high,
                                     std::shared_ptr<FeedLiveness> feed, int64_t max_feed_silence_ms,
                                     bool prefer_book)
    : max_spread_cents(max_spread_cents),
      delta_fresh_ms(delta_fresh_ms),
      delta_move_threshold(delta_move_threshold),
      max_staleness_ms(max_staleness_ms),
      extreme_high(extreme_high),
      extreme_low(py::round_n(1.0 - extreme_high, 6)),
      feed(std::move(feed)),
      max_feed_silence_ms(max_feed_silence_ms),
      prefer_book(prefer_book) {}

void KalshiMarketState::recompute_top() {
    best_yes = yes_levels.empty() ? std::nullopt : std::optional<double>(yes_levels.rbegin()->first);
    best_no = no_levels.empty() ? std::nullopt : std::optional<double>(no_levels.rbegin()->first);
    update_crossed();
}

void KalshiMarketState::update_crossed() {
    if (best_yes && best_no && *best_yes > py::round_n(1.0 - *best_no, 6)) {
        if (!crossed_since_ms) crossed_since_ms = clk::now_ms();
    } else {
        crossed_since_ms.reset();
    }
}

bool KalshiMarketState::apply_snapshot(const mj::Value& yes_arr, const mj::Value& no_arr,
                                       std::optional<int64_t> ts_ms, bool is_dollars) {
    yes_levels.clear();
    no_levels.clear();
    auto load = [&](const mj::Value& arr, std::map<double, int64_t>& book) {
        if (arr.is_null()) return;
        if (!arr.is_array()) throw std::runtime_error("book levels are not a list");
        for (const mj::Value& lvl : arr.a) {
            if (!lvl.is_array() || lvl.a.size() < 2) throw std::runtime_error("bad book level");
            auto p = parse_level_price(&lvl.a[0], is_dollars);
            auto s = size_units(&lvl.a[1]);
            if (p && s && *s > 0) book[*p] = *s;
        }
    };
    load(yes_arr, yes_levels);
    load(no_arr, no_levels);
    delta_ts_ms = ts_ms ? *ts_ms : clk::now_ms();
    delta_recv_ms = clk::now_ms();
    auto prev = std::make_pair(best_yes, best_no);
    recompute_top();
    return std::make_pair(best_yes, best_no) != prev;
}

bool KalshiMarketState::apply_delta(const std::string& side, std::optional<double> price,
                                    std::optional<double> delta, std::optional<int64_t> ts_ms) {
    auto prev = std::make_pair(best_yes, best_no);
    auto units = size_units_d(delta);
    if (price && units && (side == "yes" || side == "no")) {
        bool yes = side == "yes";
        auto& book = yes ? yes_levels : no_levels;
        std::optional<double> best = yes ? best_yes : best_no;
        auto it = book.find(*price);
        int64_t size = (it == book.end() ? 0 : it->second) + *units;
        if (size > 0) {
            book[*price] = size;
            if (!best || *price > *best) best = *price;
        } else {
            if (it != book.end()) book.erase(it);
            // Only deleting the current best requires a scan.
            if (best && *price == *best)
                best = book.empty() ? std::nullopt : std::optional<double>(book.rbegin()->first);
        }
        if (yes) best_yes = best;
        else best_no = best;
    }
    delta_ts_ms = ts_ms ? *ts_ms : clk::now_ms();
    delta_recv_ms = clk::now_ms();
    update_crossed();
    return std::make_pair(best_yes, best_no) != prev;
}

bool KalshiMarketState::update_ticker(std::optional<double> bid, std::optional<double> ask,
                                      std::optional<int64_t> ts_ms) {
    if (!bid && !ask) return false;
    auto prev_b = ticker_bid, prev_a = ticker_ask;
    if (bid) ticker_bid = bid;
    if (ask) ticker_ask = ask;
    ticker_ts_ms = ts_ms;
    ticker_recv_ms = clk::now_ms();
    // tuple != : nan compares unequal, like Python's float nan
    auto ne = [](const std::optional<double>& a, const std::optional<double>& b) {
        if (a.has_value() != b.has_value()) return true;
        return a && !(*a == *b);
    };
    bool changed = ne(ticker_bid, prev_b) || ne(ticker_ask, prev_a);
    if (changed) activity.note_change();
    return changed;
}

bool KalshiMarketState::handle_lifecycle(const std::string& event_type, const std::optional<std::string>& res,
                                         std::optional<double> sv) {
    if (res) result = res;
    if (sv) settlement_value = sv;
    if (is_final_event(event_type)) final_ = true;
    if (is_terminal_event(event_type)) {
        is_live = false;
        yes_levels.clear();
        no_levels.clear();
        best_yes.reset();
        best_no.reset();
        crossed_since_ms.reset();
        ticker_bid.reset();
        ticker_ask.reset();
        return true;
    }
    if (!final_) is_live = true;  // a halt can lift; a result cannot
    return false;
}

void KalshiMarketState::reset_book() {
    yes_levels.clear();
    no_levels.clear();
    best_yes.reset();
    best_no.reset();
    crossed_since_ms.reset();
    delta_recv_ms.reset();
    seq.reset();
    ticker_bid.reset();
    ticker_ask.reset();
    ticker_recv_ms.reset();
    ticker_ts_ms.reset();
}

std::optional<BidAsk> KalshiMarketState::delta_bid_ask(bool require_recent) const {
    if (!best_yes || !best_no) return std::nullopt;
    double bid = *best_yes;
    double ask = py::round_n(1.0 - *best_no, 6);
    if (!valid_prob(bid) || !valid_prob(ask) || bid > ask) return std::nullopt;
    int64_t now = clk::now_ms();
    if (require_recent && (!delta_recv_ms || (now - *delta_recv_ms) > delta_fresh_ms)) return std::nullopt;
    if (activity.is_active()) {
        std::optional<int64_t> age_ref = delta_ts_ms ? delta_ts_ms : delta_recv_ms;
        if (!age_ref || (clk::now_ms() - *age_ref) > max_staleness_ms) return std::nullopt;
    }
    return BidAsk{bid, ask};
}

std::optional<BidAsk> KalshiMarketState::ticker_bid_ask() const {
    if (!ticker_bid || !ticker_ask || !valid_prob(ticker_bid) || !valid_prob(ticker_ask) || *ticker_bid > *ticker_ask)
        return std::nullopt;
    if (activity.is_active()) {
        std::optional<int64_t> age_ref = ticker_ts_ms ? ticker_ts_ms : ticker_recv_ms;
        if (!age_ref || (clk::now_ms() - *age_ref) > max_staleness_ms) return std::nullopt;
    }
    return BidAsk{*ticker_bid, *ticker_ask};
}

std::pair<const char*, std::optional<BidAsk>> KalshiMarketState::book_first() const {
    if (!best_yes && !best_no) {
        auto t = ticker_bid_ask();
        if (t) return {"ticker(book-empty)", t};
        return {"none", std::nullopt};
    }
    double bid = best_yes ? *best_yes : 0.0;
    double ask = best_no ? py::round_n(1.0 - *best_no, 6) : 1.0;
    if (bid > ask) return {"none(crossed)", std::nullopt};
    if (activity.is_active()) {
        std::optional<int64_t> age_ref = delta_ts_ms ? delta_ts_ms : delta_recv_ms;
        if (!age_ref || (clk::now_ms() - *age_ref) > max_staleness_ms) {
            auto t = ticker_bid_ask();
            if (t) return {"ticker(book-stale)", t};
            return {"none", std::nullopt};
        }
    }
    return {(best_yes && best_no) ? "book" : "book(one-sided)", BidAsk{bid, ask}};
}

std::optional<BidAsk> KalshiMarketState::effective_bid_ask() const {
    if (!is_live) return std::nullopt;
    if (feed && !feed->alive_within(max_feed_silence_ms)) return std::nullopt;
    if (prefer_book) return book_first().second;
    auto t = ticker_bid_ask();
    auto d = delta_bid_ask();
    if (t && (t->first >= extreme_high || t->second <= extreme_low)) return t;
    if (d && t) {
        double d_mid = (d->first + d->second) / 2.0;
        double t_mid = (t->first + t->second) / 2.0;
        return std::fabs(d_mid - t_mid) >= delta_move_threshold ? d : t;
    }
    if (t) return t;
    return delta_bid_ask(false);
}

std::optional<double> KalshiMarketState::get_mid_prob() const {
    auto ba = effective_bid_ask();
    if (!ba) return std::nullopt;
    return (ba->first + ba->second) / 2.0;
}

std::optional<int> KalshiMarketState::get_spread_cents() const {
    auto ba = effective_bid_ask();
    if (!ba) return std::nullopt;
    return (int)py::round0((ba->second - ba->first) * 100);
}

bool KalshiMarketState::is_spread_ok() const {
    auto s = get_spread_cents();
    return s && *s <= max_spread_cents;
}

std::string KalshiMarketState::quote_source() const {
    if (!is_live) return "dead";
    if (prefer_book) return book_first().first;
    auto t = ticker_bid_ask();
    auto d = delta_bid_ask();
    if (t && (t->first >= extreme_high || t->second <= extreme_low)) return "ticker(extreme)";
    if (d && t) {
        double d_mid = (d->first + d->second) / 2.0;
        double t_mid = (t->first + t->second) / 2.0;
        return std::fabs(d_mid - t_mid) >= delta_move_threshold ? "delta(bigmove)" : "ticker(stable)";
    }
    if (t) return "ticker(fallback)";
    if (d) return "delta(only)";
    return delta_bid_ask(false) ? "delta(resting)" : "none";
}

// ---------------------------------------------------------------------------
// KalshiHybridClient
// ---------------------------------------------------------------------------

KalshiHybridClient::KalshiHybridClient(std::string api_key_id, std::string private_key_pem, int max_spread_cents,
                                       int64_t delta_fresh_ms, double delta_move_threshold,
                                       int64_t max_staleness_ms, double extreme_high,
                                       int64_t max_feed_silence_ms, bool prefer_book, bool quiet)
    : KalshiAuth(std::move(api_key_id), std::move(private_key_pem)),
      prefer_book_(prefer_book),
      max_spread_cents_(max_spread_cents),
      delta_fresh_ms_(delta_fresh_ms),
      delta_move_threshold_(delta_move_threshold),
      max_staleness_ms_(max_staleness_ms),
      extreme_high_(extreme_high),
      max_feed_silence_ms_(max_feed_silence_ms) {
    if (const char* u = std::getenv("KALSHI_WS_URL"); u && *u) url = u;
    if (quiet) return;
    logx::out(std::string("  → Using production API: ") + url);
    logx::out(std::string("  → Kalshi channels: ") +
              (prefer_book ? "orderbook_delta BOOK-FIRST (ticker only for an empty or stale book)"
                           : "ticker (baseline) + orderbook_delta (overrides on big moves)"));
    logx::out("  → Max Kalshi spread: " + std::to_string(max_spread_cents) +
              " cents | delta_fresh: " + std::to_string(delta_fresh_ms) +
              "ms | delta_move_threshold: " + py::fixed(delta_move_threshold, 3) + " (" +
              std::to_string((int64_t)(delta_move_threshold * 100)) + "c) | quote age (active books): " +
              std::to_string(max_staleness_ms) + "ms | feed silence: " + std::to_string(max_feed_silence_ms) + "ms");
}

std::optional<double> KalshiHybridClient::get_cached_price(const std::string& id) {
    auto it = market_states.find(id);
    return it == market_states.end() ? std::nullopt : it->second->get_mid_prob();
}

std::optional<BidAsk> KalshiHybridClient::get_cached_bid_ask(const std::string& id) {
    auto it = market_states.find(id);
    return it == market_states.end() ? std::nullopt : it->second->get_bid_ask_prob();
}

KalshiMarketState* KalshiHybridClient::get_state(const std::string& id) {
    auto it = market_states.find(id);
    return it == market_states.end() ? nullptr : it->second.get();
}

std::shared_ptr<KalshiHybridClient::Conn> KalshiHybridClient::setup(const std::vector<std::string>& ids,
                                                                    PriceCallback cb) {
    auto c = std::make_shared<Conn>();
    c->index = (int)conns_.size();
    std::unordered_set<std::string> seen;
    for (const auto& t : ids)
        if (seen.insert(t).second) c->tickers.push_back(t);  // dict.fromkeys: de-dupe, keep order
    c->feed = std::make_shared<FeedLiveness>();
    feeds.push_back(c->feed);
    for (const auto& t : c->tickers) {
        auto it = market_states.find(t);
        if (it == market_states.end()) {
            market_states.emplace(t, std::make_unique<KalshiMarketState>(
                                         max_spread_cents_, delta_fresh_ms_, delta_move_threshold_,
                                         max_staleness_ms_, extreme_high_, c->feed, max_feed_silence_ms_,
                                         prefer_book_));
        } else {
            it->second->feed = c->feed;
        }
    }
    mj::Value sub = mj::Value::object();
    sub.set("id", mj::Value::integer(1));
    sub.set("cmd", mj::Value::string("subscribe"));
    mj::Value params = mj::Value::object();
    mj::Value chans = mj::Value::array();
    for (const char* ch : {"orderbook_delta", "ticker", "market_lifecycle_v2"}) chans.push(mj::Value::string(ch));
    params.set("channels", std::move(chans));
    mj::Value mts = mj::Value::array();
    for (const auto& t : c->tickers) mts.push(mj::Value::string(t));
    params.set("market_tickers", std::move(mts));
    sub.set("params", std::move(params));
    c->subscribe_msg = sub.dump();
    c->cb = std::move(cb);
    conns_.push_back(c);
    return c;
}

KalshiHybridClient::FrameAction KalshiHybridClient::handle_frame(Conn& c, std::string_view raw, double recv_time) {
    mj::Value msg = mj::parse(raw);
    if (!msg.is_object()) throw std::runtime_error("'list' object has no attribute 'get'");
    const mj::Value* typev = msg.get("type");
    std::string mtype = (typev && typev->is_string()) ? typev->s : "";
    bool has_type_str = typev && typev->is_string();
    const mj::Value* pv = msg.get("msg");
    static const mj::Value kEmpty = mj::Value::object();
    const mj::Value* payload = (pv && pv->truthy()) ? pv : &kEmpty;
    if (!payload->is_object()) throw std::runtime_error("payload has no attribute 'get'");
    const mj::Value* mtv = payload->get("market_ticker");

    if (!c.ignore_sids.empty()) {
        // An adopted probe connection: its probe subscription may still deliver a
        // few frames after the unsubscribe. They are duplicates (and carry their
        // own seq), so they must never reach the books or the seq check.
        const mj::Value* sid = msg.get("sid");
        if (sid && sid->is_int() && c.ignore_sids.count(sid->i)) return FrameAction::Continue;
    }

    if (has_type_str && (mtype == "orderbook_delta" || mtype == "orderbook_snapshot")) {
        const mj::Value* sv = msg.get("seq");
        if (sv && (sv->is_int() || sv->is_bool())) {
            int64_t seq = sv->is_int() ? sv->i : (sv->b ? 1 : 0);
            if (c.conn_seq && seq != *c.conn_seq + 1) {
                logx::out("[Kalshi] seq gap " + std::to_string(*c.conn_seq) + "->" + std::to_string(seq) +
                          ", resyncing " + std::to_string(c.tickers.size()) + " markets");
                return FrameAction::Resync;
            }
            c.conn_seq = seq;
        }
    }

    auto it = (mtv && mtv->is_string()) ? market_states.find(mtv->s) : market_states.end();
    if (it == market_states.end()) {
        if (has_type_str && mtype == "error") {
            // e.g. code 26, per-subscription market limit: the
            // markets on this socket would otherwise sit dark
            logx::out("[Kalshi] error frame: " + py_repr(*payload));
        }
        return FrameAction::Continue;
    }
    KalshiMarketState& state = *it->second;
    const std::string& mt = mtv->s;

    if (state.crossed_since_ms && clk::now_ms() - *state.crossed_since_ms > CROSSED_RESYNC_MS) {
        logx::out("[Kalshi] " + mt + " book crossed for " + std::to_string(clk::now_ms() - *state.crossed_since_ms) +
                  "ms, resyncing " + std::to_string(c.tickers.size()) + " markets");
        return FrameAction::Resync;
    }
    bool changed = false;
    if (!has_type_str) return FrameAction::Continue;

    if (mtype == "orderbook_snapshot") {
        static const mj::Value kList = mj::Value::array();
        const mj::Value* yes_arr;
        const mj::Value* no_arr;
        bool is_dollars;
        if (payload->contains("yes_dollars_fp") || payload->contains("no_dollars_fp")) {
            yes_arr = py_or(payload->get("yes_dollars_fp"), &kList);
            no_arr = py_or(payload->get("no_dollars_fp"), &kList);
            is_dollars = true;
        } else {
            yes_arr = py_or(payload->get("yes"), &kList);
            no_arr = py_or(payload->get("no"), &kList);
            is_dollars = false;
        }
        if (!yes_arr) yes_arr = &kList;
        if (!no_arr) no_arr = &kList;
        auto ts_ms = ticker_ts_to_ms(py_or(payload->get("ts_ms"), payload->get("ts")));
        changed = state.apply_snapshot(*yes_arr, *no_arr, ts_ms, is_dollars);
    } else if (mtype == "orderbook_delta") {
        const mj::Value* sidev = payload->get("side");
        std::string side;
        if (sidev && sidev->truthy()) {
            if (!sidev->is_string()) throw std::runtime_error("side has no attribute 'lower'");
            for (char ch : sidev->s) side.push_back((char)std::tolower((unsigned char)ch));
        }
        std::optional<double> price;
        if (payload->contains("price_dollars")) price = parse_level_price(payload->get("price_dollars"), true);
        else price = parse_level_price(payload->get("price"), false);
        const mj::Value* dv = payload->contains("delta_fp") ? payload->get("delta_fp") : payload->get("delta");
        std::optional<double> delta = to_float(dv);
        auto ts_ms = ticker_ts_to_ms(py_or(payload->get("ts_ms"), payload->get("ts")));
        if (counting_) ++delta_counts_[mt];
        if (probe_) probe_note_incumbent(c, mt, *payload, recv_time);
        changed = state.apply_delta(side, price, delta, ts_ms);
    } else if (mtype == "ticker") {
        if (!state.is_live) return FrameAction::Continue;
        auto bid = to_float(payload->get("yes_bid_dollars"));
        auto ask = to_float(payload->get("yes_ask_dollars"));
        auto ts_ms = ticker_ts_to_ms(py_or(payload->get("ts_ms"), payload->get("ts")));
        changed = state.update_ticker(bid, ask, ts_ms);
    } else if (mtype == "market_lifecycle_v2") {
        auto sv = to_float(payload->get("settlement_value"));
        const mj::Value* et = payload->get("event_type");
        const mj::Value* rv = payload->get("result");
        std::optional<std::string> res;
        if (rv && !rv->is_null()) res = rv->is_string() ? rv->s : rv->dump();
        bool is_terminal = state.handle_lifecycle((et && et->is_string()) ? et->s : "", res, sv);
        if (debug_on())
            logx::out("[Kalshi " + mt + "] lifecycle " + ((et && et->is_string()) ? et->s : "None") +
                      " result=" + (res ? *res : "None") + " sv=" + (sv ? py::repr(*sv) : "None"));
        if (is_terminal && sv) c.cb(Src::KalshiSettled, mt, std::nullopt, *sv, recv_time);
        return FrameAction::Continue;
    } else {
        return FrameAction::Continue;
    }

    if (changed) {
        auto mid = state.get_mid_prob();
        if (mid) {
            if (debug_on()) {
                auto ba = state.get_bid_ask_prob();
                logx::out("[Kalshi " + mt + "] " + state.quote_source() + " bid=" + py::fixed(ba->first, 4) +
                          " ask=" + py::fixed(ba->second, 4) + " mid=" + py::fixed(*mid, 4));
            }
            c.cb(Src::Kalshi, mt, std::nullopt, *mid, recv_time);
        }
    }
    return FrameAction::Continue;
}

void KalshiHybridClient::listen(const std::vector<std::string>& ids, PriceCallback cb) {
    load_private_key();
    auto c = setup(ids, std::move(cb));
    start_connection(c);
    if (probe_cfg_.candidates > 0 && !probe_armed_) {
        probe_armed_ = true;
        counting_ = true;  // warmup: learn which tickers and sockets are busy
        schedule_probe(probe_cfg_.warmup_sec, probe_cfg_.candidates, true);
    }
}

net::WsOptions KalshiHybridClient::ws_options(std::string* err) {
    net::WsOptions opt;
    opt.max_size = 1 << 20;
    opt.ping_interval = 5;
    opt.ping_timeout = 5;
    opt.close_timeout = 5;
    opt.open_timeout = 10;
    try {
        opt.headers = create_ws_headers();
    } catch (const std::exception& e) {
        if (err) *err = e.what();
    }
    return opt;
}

void KalshiHybridClient::on_conn_open(const std::shared_ptr<Conn>& c, uint64_t gen) {
    if (gen != c->generation) return;
    c->ws->send_text(c->subscribe_msg);
    c->feed->note_message();
    c->hb_running = true;
    heartbeat(c, gen);
    logx::out("[Kalshi] Subscribed to " + std::to_string(c->tickers.size()) +
              " markets on 1 connection (delta+ticker)" + (c->selected ? " [selected connection]" : ""));
    c->backoff = 1.0;
}

void KalshiHybridClient::start_connection(const std::shared_ptr<Conn>& c) {
    // Loop top: snapshots re-seed after (re)connect.
    for (const auto& t : c->tickers) market_states[t]->reset_book();
    c->conn_seq.reset();
    c->received = 0;
    c->ignore_sids.clear();
    uint64_t gen = ++c->generation;
    c->ws = std::make_shared<net::WsClient>();
    auto pending_error = install_handlers(c, gen);
    c->ws->on_open = [this, c, gen] { on_conn_open(c, gen); };
    net::WsOptions opt = ws_options(pending_error.get());
    c->ws->connect(url, std::move(opt));
}

std::shared_ptr<std::string> KalshiHybridClient::install_handlers(const std::shared_ptr<Conn>& c, uint64_t gen) {
    auto pending_error = std::make_shared<std::string>();
    c->ws->on_message = [this, c, gen, pending_error](std::string_view raw) {
        if (gen != c->generation || !c->ws->is_open()) return;
        double recv_time = clk::time_sec();
        ++c->received;
        c->feed->note_message();
        try {
            if (handle_frame(*c, raw, recv_time) == FrameAction::Resync) c->ws->close(1000);
        } catch (const std::exception& e) {
            // Python: the exception leaves `async with` (closing the socket),
            // then the except block logs it and backs off.
            *pending_error = e.what();
            c->ws->close(1000);
        }
    };
    c->ws->on_close = [this, c, gen, pending_error](bool clean, int code, const std::string& reason) {
        if (gen != c->generation) return;
        bool error = !clean || !pending_error->empty();
        on_disconnected(c, error, !pending_error->empty() ? *pending_error : reason);
    };
    return pending_error;
}

void KalshiHybridClient::heartbeat(const std::shared_ptr<Conn>& c, uint64_t gen) {
    c->hb_timer = ev::Loop::get().call_later(HEARTBEAT_SEC, [this, c, gen] {
        if (gen != c->generation || !c->hb_running) return;
        double t0 = clk::time_sec();
        c->ws->ping(HEARTBEAT_TIMEOUT_SEC, [this, c, gen, t0](bool ok, double) {
            if (gen != c->generation || !c->hb_running) return;
            if (ok) {
                c->feed->note_pong((clk::time_sec() - t0) * 1000.0);
                heartbeat(c, gen);
            } else {
                c->feed->mark_down();
                c->hb_running = false;
                if (debug_on()) logx::out("[Kalshi/hybrid] heartbeat lost");
            }
        });
    });
}

void KalshiHybridClient::on_disconnected(const std::shared_ptr<Conn>& c, bool error, const std::string& why) {
    if (c->selected) {
        // The reconnect lands on a random connection again: look for a fast one
        // (a full probe at most every 10 minutes, a re-check-sized one otherwise).
        c->selected = false;
        if (probe_cfg_.candidates > 0 && !probe_) {
            bool full = clk::time_sec() - last_full_probe_ > 600;
            logx::out("[Kalshi probe] a selected connection dropped - probing again in 30 s");
            schedule_probe(30, full ? probe_cfg_.candidates : probe_cfg_.recheck_candidates, full);
        }
    }
    auto finally_ = [c] {
        // Nothing on THIS socket is current while it is down.
        c->feed->mark_down();
        c->hb_running = false;
        ev::Loop::get().cancel(c->hb_timer);
    };
    if (!error) {
        finally_();
        start_connection(c);
        return;
    }
    logx::out("[Kalshi] Error: " + why + " -> reconnect in " + py::repr(c->backoff) + "s");
    // The heartbeat keeps running through the backoff sleep (Python cancels it
    // in `finally`, after the sleep); its next ping marks the feed down.
    std::weak_ptr<Conn> w = c;
    c->backoff_timer = ev::Loop::get().call_later(c->backoff, [this, w, finally_] {
        auto cc = w.lock();
        if (!cc) return;
        cc->backoff = std::min(cc->backoff * 2, 10.0);
        finally_();
        start_connection(cc);
    });
}

std::shared_ptr<KalshiHybridClient::Conn> KalshiHybridClient::open_replay(const std::vector<std::string>& tickers,
                                                                          PriceCallback cb) {
    return setup(tickers, std::move(cb));
}

void KalshiHybridClient::replay_connect(Conn& c) {
    for (const auto& t : c.tickers) market_states[t]->reset_book();
    c.conn_seq.reset();
    c.received = 0;
    c.feed->note_message();  // after `await ws.send(subscribe_msg)`
    c.backoff = 1.0;
}

void KalshiHybridClient::replay_disconnect(Conn& c) { c.feed->mark_down(); }


// ---------------------------------------------------------------------------
// Connection selection (see ConnProbeConfig in the header)
// ---------------------------------------------------------------------------

std::string delta_key(const mj::Value& p) {
    std::string k;
    for (const char* f : {"market_ticker", "side", "price_dollars", "price", "delta_fp", "delta", "ts", "ts_ms"}) {
        const mj::Value* v = p.get(f);
        k += v ? v->dump() : "-";
        k.push_back('|');
    }
    return k;
}

std::vector<ProbeStat> rank_probe(const std::vector<std::vector<double>>& offsets, double min_share) {
    size_t best_n = 0;
    for (const auto& v : offsets) best_n = std::max(best_n, v.size());
    std::vector<ProbeStat> out;
    if (best_n == 0) return out;
    for (size_t i = 0; i < offsets.size(); ++i) {
        const auto& v = offsets[i];
        if (v.empty() || (double)v.size() < min_share * (double)best_n) continue;
        std::vector<double> s(v);
        std::sort(s.begin(), s.end());
        auto at = [&](double q) { return s[std::min(s.size() - 1, (size_t)(q * (double)(s.size() - 1)))]; };
        out.push_back({(int)i, s.size(), at(0.5), at(0.9)});
    }
    std::sort(out.begin(), out.end(), [](const ProbeStat& a, const ProbeStat& b) {
        if (a.p90 != b.p90) return a.p90 < b.p90;
        if (a.p50 != b.p50) return a.p50 < b.p50;
        return a.id < b.id;
    });
    return out;
}

struct KalshiHybridClient::Probe {
    struct Cand {
        std::shared_ptr<net::WsClient> ws;
        std::unordered_set<int64_t> sids;  // the probe subscription's ids on this socket
        bool open = false;
    };
    bool full = true;
    std::vector<Cand> cands;              // probe ids [0, cands.size())
    std::vector<bool> conn_measured;      // incumbents: id = cands.size() + conn index
    std::unordered_set<std::string> tickers;
    std::string sub_msg;
    struct Obs {
        double first = 0;
        std::vector<std::pair<int, double>> arr;
    };
    std::unordered_map<std::string, Obs> obs;
    std::vector<std::vector<double>> offs;
    std::map<std::string, int> closed;  // why candidates closed during the probe (reported once at the end)
    double started = 0;
    int updates = 0;
    ev::Timer tick;

    size_t live() const {
        size_t n = 0;
        for (const auto& c : cands) n += c.open;
        for (bool m : conn_measured) n += m;
        return n;
    }
    void note(int id, const std::string& key, double t) {
        auto [it, fresh] = obs.try_emplace(key);
        Obs& o = it->second;
        if (fresh) o.first = t;
        for (const auto& a : o.arr)
            if (a.first == id) return;  // first copy per connection
        o.arr.emplace_back(id, t);
        if (t < o.first) o.first = t;
    }
};

namespace {
void drop_ws(const std::shared_ptr<net::WsClient>& ws) {
    if (!ws) return;
    ws->on_open = nullptr;
    ws->on_message = nullptr;
    ws->on_close = nullptr;
    if (ws->state() == net::WsClient::State::Open) ws->close(1000);
    else if (ws->state() == net::WsClient::State::Connecting) ws->abort("probe closed");
}
std::string ms(double v) { return py::fixed(v, v < 10 ? 1 : 0); }
}  // namespace

void KalshiHybridClient::probe_note_incumbent(const Conn& c, const std::string& mt, const mj::Value& payload,
                                              double t) {
    if (c.index < 0 || !probe_->tickers.count(mt)) return;
    probe_->note((int)probe_->cands.size() + c.index, delta_key(payload), t);
}

void KalshiHybridClient::schedule_probe(double delay_sec, int candidates, bool full) {
    ev::Loop& L = ev::Loop::get();
    L.cancel(probe_timer_);
    probe_timer_ = L.call_later(delay_sec, [this, candidates, full] { start_probe(candidates, full); });
}

void KalshiHybridClient::start_probe(int n, bool full) {
    if (probe_ || n <= 0) return;
    std::vector<std::pair<uint32_t, std::string>> act;
    uint64_t total = 0;
    for (const auto& [t, k] : delta_counts_)
        if (k > 0) {
            act.emplace_back(k, t);
            total += k;
        }
    if (total < 50) {
        // Connections only separate under traffic (in quiet stretches every one looks clean).
        logx::out("[Kalshi probe] markets too quiet to compare connections (" + std::to_string(total) +
                  " book updates since the last count) - retrying in 5 min");
        delta_counts_.clear();  // judge the next window on its own traffic
        schedule_probe(300, n, full);
        return;
    }
    int cap = probe_cfg_.max_total_conns - (int)conns_.size();
    if (n > cap) n = cap;
    if (n <= 0) {
        logx::out("[Kalshi probe] no connection budget left (max_total_conns) - skipped");
        return;
    }
    std::sort(act.begin(), act.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    auto p = std::make_shared<Probe>();
    p->full = full;
    mj::Value mts = mj::Value::array();
    for (size_t i = 0; i < act.size() && (int)i < probe_cfg_.probe_tickers; ++i) {
        p->tickers.insert(act[i].second);
        mts.push(mj::Value::string(act[i].second));
    }
    mj::Value sub = mj::Value::object();
    sub.set("id", mj::Value::integer(77));
    sub.set("cmd", mj::Value::string("subscribe"));
    mj::Value params = mj::Value::object();
    mj::Value chans = mj::Value::array();
    chans.push(mj::Value::string("orderbook_delta"));
    params.set("channels", std::move(chans));
    params.set("market_tickers", std::move(mts));
    sub.set("params", std::move(params));
    p->sub_msg = sub.dump();
    p->cands.resize((size_t)n);
    p->conn_measured.assign(conns_.size(), false);
    for (const auto& c : conns_)
        for (const auto& t : c->tickers)
            if (p->tickers.count(t)) {
                p->conn_measured[(size_t)c->index] = true;
                break;
            }
    p->offs.assign(p->cands.size() + conns_.size(), {});
    probe_ = p;
    if (full) last_full_probe_ = clk::time_sec();
    logx::out("[Kalshi probe] opening " + std::to_string(n) + " candidate connections on the " +
              std::to_string(p->tickers.size()) + " busiest markets (" + (full ? "full probe" : "re-check") + ")");
    std::weak_ptr<Probe> wp = p;
    ev::Loop& L = ev::Loop::get();
    for (int i = 0; i < n; ++i) {
        L.call_later(0.04 * i, [this, wp, i] {
            auto pp = wp.lock();
            if (!pp || pp != probe_) return;
            auto ws = std::make_shared<net::WsClient>();
            pp->cands[(size_t)i].ws = ws;
            std::weak_ptr<net::WsClient> wws = ws;
            ws->on_open = [wp, wws, i] {
                auto pp = wp.lock();
                auto w = wws.lock();
                if (!pp || !w) return;
                pp->cands[(size_t)i].open = true;
                w->send_text(pp->sub_msg);
            };
            ws->on_message = [wp, i](std::string_view raw) {
                double t = clk::time_sec();
                auto pp = wp.lock();
                if (!pp) return;
                mj::Value m;
                try {
                    m = mj::parse(raw);
                } catch (const std::exception&) {
                    return;
                }
                if (!m.is_object()) return;
                const mj::Value* ty = m.get("type");
                if (!ty || !ty->is_string()) return;
                const mj::Value* pl = m.get("msg");
                if (ty->s == "subscribed") {
                    if (pl && pl->is_object())
                        if (const mj::Value* sid = pl->get("sid"); sid && sid->is_int())
                            pp->cands[(size_t)i].sids.insert(sid->i);
                    return;
                }
                if (ty->s == "error") {
                    logx::out("[Kalshi probe] candidate " + std::to_string(i) + " error frame: " +
                              (pl ? py_repr(*pl) : std::string("None")));
                    return;
                }
                if (ty->s != "orderbook_delta" || !pl || !pl->is_object()) return;
                const mj::Value* mt = pl->get("market_ticker");
                if (!mt || !mt->is_string() || !pp->tickers.count(mt->s)) return;
                pp->note(i, delta_key(*pl), t);
            };
            ws->on_close = [wp, i](bool clean, int code, const std::string& reason) {
                if (auto pp = wp.lock()) {
                    pp->cands[(size_t)i].open = false;
                    ++pp->closed[std::to_string(code) + (clean ? " " : " unclean ") + reason.substr(0, 60)];
                }
            };
            std::string err;
            net::WsOptions opt = ws_options(&err);
            ws->connect(url, std::move(opt));
        });
    }
    // Measure once every candidate has had time to connect and load its snapshots.
    p->started = clk::time_sec() + 0.04 * n + 2.0;
    p->tick = L.call_later(0.04 * n + 2.0, [this] { probe_tick(); });
}

void KalshiHybridClient::probe_tick() {
    auto p = probe_;
    if (!p) return;
    double now = clk::time_sec();
    size_t need = std::max<size_t>(2, (size_t)std::ceil(0.8 * (double)p->live()));
    for (auto it = p->obs.begin(); it != p->obs.end();) {
        if (now - it->second.first < 2.0) {
            ++it;
            continue;
        }
        if (it->second.first >= p->started && it->second.arr.size() >= need) {
            for (const auto& [id, t] : it->second.arr) p->offs[(size_t)id].push_back((t - it->second.first) * 1000.0);
            ++p->updates;
        }
        it = p->obs.erase(it);
    }
    double el = now - p->started;
    if ((el >= probe_cfg_.seconds && p->updates >= probe_cfg_.min_updates) || el >= probe_cfg_.max_seconds) {
        finish_probe();
        return;
    }
    p->tick = ev::Loop::get().call_later(0.5, [this] { probe_tick(); });
}

void KalshiHybridClient::abandon_probe(const std::string& why, double retry_sec) {
    auto p = probe_;
    probe_.reset();
    if (!p) return;
    ev::Loop::get().cancel(p->tick);
    for (auto& c : p->cands) drop_ws(c.ws);
    logx::out("[Kalshi probe] " + why + (retry_sec > 0 ? " - retrying in " + py::repr(retry_sec / 60) + " min" : ""));
    if (retry_sec > 0) schedule_probe(retry_sec, p->full ? probe_cfg_.candidates : probe_cfg_.recheck_candidates, p->full);
}

void KalshiHybridClient::finish_probe() {
    auto p = probe_;
    if (!p) return;
    const size_t nc = p->cands.size();
    std::vector<ProbeStat> cand;
    std::vector<ProbeStat> inc(conns_.size());
    size_t connected = 0;
    for (const auto& c : p->cands) connected += c.open;
    for (const ProbeStat& s : rank_probe(p->offs)) {
        if ((size_t)s.id < nc) {
            if (p->cands[(size_t)s.id].open) cand.push_back(s);
        } else if ((size_t)s.id - nc < inc.size()) {
            inc[(size_t)s.id - nc] = s;
        }
    }
    if (!p->closed.empty()) {
        std::string why;
        for (const auto& [r, k] : p->closed) why += (why.empty() ? "" : "; ") + std::to_string(k) + "x " + r;
        logx::out("[Kalshi probe] candidates that closed during the probe: " + why);
    }
    if (p->updates < 30 || cand.empty()) {
        abandon_probe("only " + std::to_string(p->updates) + " updates reached " + std::to_string(connected) +
                          " connected candidates in " + ms(clk::time_sec() - p->started) + " s - nothing changed",
                      600);
        return;
    }
    probe_.reset();
    ev::Loop::get().cancel(p->tick);
    std::vector<double> p90s;
    for (const auto& s : cand) p90s.push_back(s.p90);
    double med_p90 = p90s[p90s.size() / 2];
    const ProbeStat& worst = cand.back();
    logx::out("[Kalshi probe] " + std::to_string(connected) + "/" + std::to_string(nc) + " candidates, " +
              std::to_string(p->updates) + " updates in " + ms(clk::time_sec() - p->started) +
              " s: behind the first copy p50/p90 best " + ms(cand[0].p50) + "/" + ms(cand[0].p90) + " ms, median " +
              ms(cand[cand.size() / 2].p50) + "/" + ms(med_p90) + ", worst " + ms(worst.p50) + "/" + ms(worst.p90));

    // Busiest sockets first: they carry the markets that move.
    std::vector<uint64_t> activity(conns_.size(), 0);
    for (const auto& c : conns_)
        for (const auto& t : c->tickers)
            if (auto it = delta_counts_.find(t); it != delta_counts_.end()) activity[(size_t)c->index] += it->second;
    std::vector<size_t> order(conns_.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return activity[a] > activity[b]; });

    std::unordered_set<int> used;
    size_t next = 0;
    std::vector<std::pair<std::shared_ptr<Conn>, int>> plan;
    for (size_t ci : order) {
        if (next >= cand.size()) break;
        auto& c = conns_[ci];
        if (!c->ws || !c->ws->is_open() || activity[ci] == 0) continue;
        const ProbeStat& b = cand[next];
        std::string head = "[Kalshi probe] socket " + std::to_string(ci + 1) + " (" + std::to_string(c->tickers.size()) +
                           " markets, " + std::to_string(activity[ci]) + " book updates): ";
        bool swap;
        if (inc[ci].id >= 0) {
            swap = b.p90 + probe_cfg_.margin_ms < inc[ci].p90;
            logx::out(head + "current p90 " + ms(inc[ci].p90) + " ms vs candidate " + ms(b.p90) + " ms - " +
                      (swap ? "switching" : "keeping current"));
        } else {
            // Not measurable (no probe market on it): a random connection is median-quality,
            // so take any candidate that beats the median, unless it already runs on a chosen one.
            swap = !c->selected && b.p90 < med_p90;
            logx::out(head + (c->selected ? "already on a selected connection - keeping"
                                          : swap ? "not measured; switching to candidate p90 " + ms(b.p90) + " ms"
                                                 : "not measured; no candidate beats the median - keeping"));
        }
        if (swap) {
            plan.emplace_back(c, b.id);
            used.insert(b.id);
            ++next;
        }
    }
    for (size_t i = 0; i < nc; ++i)
        if (!used.count((int)i)) drop_ws(p->cands[i].ws);
    for (auto& [c, id] : plan) {
        auto ws = p->cands[(size_t)id].ws;
        auto sids = p->cands[(size_t)id].sids;
        ws->on_open = nullptr;
        ws->on_message = nullptr;
        ws->on_close = nullptr;
        mj::Value unsub = mj::Value::object();
        unsub.set("id", mj::Value::integer(78));
        unsub.set("cmd", mj::Value::string("unsubscribe"));
        mj::Value prm = mj::Value::object();
        mj::Value arr = mj::Value::array();
        for (int64_t s : sids) arr.push(mj::Value::integer(s));
        prm.set("sids", std::move(arr));
        unsub.set("params", std::move(prm));
        ws->send_text(unsub.dump());
        std::shared_ptr<Conn> cc = c;
        ev::Loop::get().call_soon([this, cc, ws, sids] { adopt(cc, ws, sids); });
    }
    delta_counts_.clear();
    counting_ = probe_cfg_.recheck_min > 0;
    if (probe_cfg_.recheck_min > 0) schedule_probe(probe_cfg_.recheck_min * 60, probe_cfg_.recheck_candidates, false);
}

void KalshiHybridClient::adopt(const std::shared_ptr<Conn>& c, std::shared_ptr<net::WsClient> ws,
                               std::unordered_set<int64_t> ignore) {
    if (!ws || !ws->is_open()) {
        logx::out("[Kalshi probe] chosen candidate closed before the switch - socket " + std::to_string(c->index + 1) +
                  " stays on its current connection");
        drop_ws(ws);
        return;
    }
    auto old = c->ws;
    ev::Loop::get().cancel(c->hb_timer);
    c->hb_running = false;
    for (const auto& t : c->tickers) market_states[t]->reset_book();  // the new subscription's snapshots re-seed
    c->conn_seq.reset();
    c->received = 0;
    uint64_t gen = ++c->generation;
    c->ws = std::move(ws);
    c->ignore_sids = std::move(ignore);
    c->selected = true;
    install_handlers(c, gen);
    on_conn_open(c, gen);
    drop_ws(old);
}

}  // namespace ps
