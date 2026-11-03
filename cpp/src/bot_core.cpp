#include "bot_core.hpp"

#include <cctype>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "clock.hpp"
#include "log.hpp"
#include "pyutil.hpp"
#include "yaml.hpp"

namespace bot {

bool debug_enabled() {
    static const bool on = [] {
        const char* v = std::getenv("BOT_DEBUG");
        if (!v) return false;
        std::string s(v);
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        size_t b = s.find_first_not_of(' '), e = s.find_last_not_of(' ');
        s = b == std::string::npos ? "" : s.substr(b, e - b + 1);
        return s == "1" || s == "true" || s == "yes" || s == "on";
    }();
    return on;
}

// ===========================================================================
// Python-semantics config helpers
// ===========================================================================

double py_float_value(const mj::Value* v, const char* what) {
    if (v) {
        switch (v->type) {
            case mj::Type::Int: return (double)v->i;
            case mj::Type::Float: return v->d;
            case mj::Type::Bool: return v->b ? 1.0 : 0.0;
            case mj::Type::String:
                if (auto f = py::float_from_str(v->s)) return *f;
                throw std::runtime_error(std::string("could not convert string to float for ") + what + ": '" + v->s + "'");
            default: break;
        }
    }
    throw std::runtime_error(std::string("float() argument must be a string or a real number: ") + what);
}

int64_t py_int_value(const mj::Value* v, const char* what) {
    if (v) {
        switch (v->type) {
            case mj::Type::Int: return v->i;
            case mj::Type::Bool: return v->b ? 1 : 0;
            case mj::Type::Float:
                if (std::isfinite(v->d)) return py::trunc_i64(v->d);
                break;
            case mj::Type::String:
                if (auto i = py::int_from_str(v->s)) return *i;
                break;
            default: break;
        }
    }
    throw std::runtime_error(std::string("invalid literal for int(): ") + what);
}

static const mj::Value* pick(const mj::Value& pair, const mj::Value& bot_cfg, const char* key) {
    if (const mj::Value* v = pair.get(key)) return v;
    return bot_cfg.get(key);
}

double cfg_float(const mj::Value& pair, const mj::Value& bot_cfg, const char* key, double dflt) {
    const mj::Value* v = pick(pair, bot_cfg, key);
    if (!v) return dflt;
    return py_float_value(v, key);
}

std::string py_str(const mj::Value* v) {
    if (!v) return "None";
    switch (v->type) {
        case mj::Type::Null: return "None";
        case mj::Type::Bool: return v->b ? "True" : "False";
        case mj::Type::Int: return std::to_string(v->i);
        case mj::Type::Float: return py::repr(v->d);
        case mj::Type::String: return v->s;
        default: return v->dump();
    }
}

std::string py_slice(const std::string& s, size_t n) {
    size_t cps = 0, i = 0;
    while (i < s.size()) {
        if (cps == n) return s.substr(0, i);
        unsigned char c = (unsigned char)s[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        i += len;
        ++cps;
    }
    return s;
}

bool config_bool(const mj::Value* v) {
    // A config flag that FIRES ORDERS must not be switched on by accident:
    // strings are read by vocabulary, anything unrecognised is OFF.
    if (!v) return false;
    if (v->is_string()) {
        std::string t = v->s;
        size_t b = 0, e = t.size();
        while (b < e && py::is_space(t[b])) ++b;
        while (e > b && py::is_space(t[e - 1])) --e;
        t = t.substr(b, e - b);
        for (char& c : t) c = (char)std::tolower((unsigned char)c);
        return t == "1" || t == "true" || t == "yes" || t == "on" || t == "y" || t == "t";
    }
    return v->truthy();
}

static bool truthy(const mj::Value* v) { return v && v->truthy(); }

const std::vector<std::string> PAIR_OVERRIDE_KEYS = {
    "reverse", "max_bet_size", "min_edge_threshold", "max_edge_threshold",
    "max_position", "min_time_between_trades", "edge_scale_start",
    "size_edge_power", "max_kalshi_spread", "max_polymarket_spread",
    "clearing_edge_threshold", "extreme_bet_multiplier",
    "enable_amm", "amm_target_fraction", "p_override", "liquidity_override",
    "settle_slam", "tie_resolves", "settle_bet_size",
    "limit_offset", "min_edge_floor", "adding_edge_tolerance", "min_clearing_room",
    "min_bet_size", "cap_clear_at_position", "partner_trust_ms", "partner_max_lead",
    "underdog_threshold", "underdog_min_edge",
};

// ===========================================================================
// game_start parsing: bot_core._start_ms over datetime.fromisoformat. The ISO
// parser follows CPython's C implementation (_datetimemodule.c, 3.13), which is
// what Python runs — it is more lenient than the pure-Python _pydatetime copy
// (e.g. "23:00:00.Z" and one stray character before an offset are accepted).
// cpp/tests/check_pyutil.py fuzzes this against the Python it runs under.
// ===========================================================================

namespace {

struct IsoDT {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, us = 0;
    bool aware = false;
    int64_t offset_us = 0;  // utcoffset() in microseconds
};

const int kDaysBeforeMonth[] = {0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
const int kDaysInMonth[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

bool iso_is_leap(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
int iso_days_in_month(int y, int m) { return m == 2 && iso_is_leap(y) ? 29 : kDaysInMonth[m]; }
int64_t iso_ymd_to_ord(int y, int m, int d) {
    int64_t py = y - 1;
    return py * 365 + py / 4 - py / 100 + py / 400 + kDaysBeforeMonth[m] + (m > 2 && iso_is_leap(y)) + d;
}
void iso_ord_to_ymd(int64_t ordinal, int& year, int& month, int& day) {
    --ordinal;
    int64_t n400 = ordinal / 146097, n = ordinal % 146097;
    year = (int)(n400 * 400 + 1);
    int64_t n100 = n / 36524;
    n %= 36524;
    int64_t n4 = n / 1461;
    n %= 1461;
    int64_t n1 = n / 365;
    n %= 365;
    year += (int)(n100 * 100 + n4 * 4 + n1);
    if (n1 == 4 || n100 == 4) {
        year -= 1;
        month = 12;
        day = 31;
        return;
    }
    bool leapyear = n1 == 3 && (n4 != 24 || n100 == 3);
    month = (int)((n + 50) >> 5);
    int64_t preceding = kDaysBeforeMonth[month] + (month > 2 && leapyear);
    if (preceding > n) {
        month -= 1;
        preceding -= iso_days_in_month(year, month);
    }
    day = (int)(n - preceding + 1);
}

bool iso_digit(char c) { return c >= '0' && c <= '9'; }

// parse_digits: NULL (here nullptr) at the first non-digit; reads the NUL
// terminator like the C code does.
const char* iso_parse_digits(const char* p, int* var, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        unsigned tmp = (unsigned)(*(p++) - '0');
        if (tmp > 9) return nullptr;
        *var = *var * 10 + (int)tmp;
    }
    return p;
}

int64_t iso_find_separator(const char* s, int64_t len) {
    if (len == 7) return 7;
    if (s[4] == '-') {
        if (s[5] == 'W') {
            if (len < 8) return -1;
            if (len > 8 && s[8] == '-') {
                if (len == 9) return -1;
                if (len > 10 && iso_digit(s[10])) return 8;
                return 10;
            }
            return 8;
        }
        return 10;
    }
    if (s[4] == 'W') {
        int64_t idx = 7;
        for (; idx < len; ++idx)
            if (!iso_digit(s[idx])) break;
        if (idx < 9) return idx;
        return idx % 2 == 0 ? 7 : 8;
    }
    return 8;
}

int iso_week_to_ymd(int iso_year, int iso_week, int iso_day, int& y, int& m, int& d) {
    if (iso_year < 1 || iso_year > 9999) return -4;
    if (iso_week <= 0 || iso_week >= 53) {
        bool out_of_range = true;
        if (iso_week == 53) {
            int64_t first_weekday = (iso_ymd_to_ord(iso_year, 1, 1) + 6) % 7;
            if (first_weekday == 3 || (first_weekday == 2 && iso_is_leap(iso_year))) out_of_range = false;
        }
        if (out_of_range) return -2;
    }
    if (iso_day <= 0 || iso_day >= 8) return -3;
    int64_t first_day = iso_ymd_to_ord(iso_year, 1, 1);
    int64_t first_weekday = (first_day + 6) % 7;
    int64_t week1_monday = first_day - first_weekday;
    if (first_weekday > 3) week1_monday += 7;
    iso_ord_to_ymd(week1_monday + (iso_week - 1) * 7 + iso_day - 1, y, m, d);
    return 0;
}

int iso_parse_date(const char* s, size_t len, int& y, int& m, int& d) {
    const char* p = iso_parse_digits(s, &y, 4);
    if (!p) return -1;
    bool sep = *p == '-';
    if (sep) ++p;
    if (*p == 'W') {
        ++p;
        int week = 0, wday = 0;
        p = iso_parse_digits(p, &week, 2);
        if (!p) return -3;
        if ((size_t)(p - s) < len) {
            if (sep && *(p++) != '-') return -2;
            p = iso_parse_digits(p, &wday, 1);
            if (!p) return -4;
        } else {
            wday = 1;
        }
        int rv = iso_week_to_ymd(y, week, wday, y, m, d);
        return rv ? -3 + rv : 0;
    }
    p = iso_parse_digits(p, &m, 2);
    if (!p) return -1;
    if (sep && *(p++) != '-') return -2;
    p = iso_parse_digits(p, &d, 2);
    return p ? 0 : -1;
}

int iso_parse_hh_mm_ss_ff(const char* p, const char* p_end, int* hour, int* minute, int* second, int* us) {
    *hour = *minute = *second = *us = 0;
    int* vals[3] = {hour, minute, second};
    bool has_sep = true;
    for (size_t i = 0; i < 3; ++i) {
        p = iso_parse_digits(p, vals[i], 2);
        if (!p) return -3;
        char c = *(p++);
        if (i == 0) has_sep = c == ':';
        // 3.13.0 has no checks for a decimal mark after the hour or minute,
        // nor for a ':' after the seconds ("23.00Z", "16:00:07:00" parse).
        if (p >= p_end) return c != '\0';
        if (has_sep && c == ':') continue;
        if (c == '.' || c == ',') break;
        if (!has_sep) --p;
        else return -4;
    }
    size_t remains = (size_t)(p_end - p), to_parse = remains >= 6 ? 6 : remains;
    p = iso_parse_digits(p, us, to_parse);
    if (!p) return -3;
    static const int correction[] = {100000, 10000, 1000, 100, 10};
    if (to_parse >= 1 && to_parse < 6) *us *= correction[to_parse - 1];
    while (iso_digit(*p)) ++p;
    return *p != '\0';
}

int iso_parse_time(const char* s, size_t len, IsoDT& dt, int* tzoffset, int* tzusec) {
    const char* p_end = s + len;
    const char* tz = s;
    do {
        if (*tz == 'Z' || *tz == '+' || *tz == '-') break;
    } while (++tz < p_end);
    int rv = iso_parse_hh_mm_ss_ff(s, tz, &dt.hour, &dt.minute, &dt.second, &dt.us);
    if (rv < 0) return rv;
    if (tz == p_end) return rv == 1 ? -5 : 0;
    if (*tz == 'Z') {
        *tzoffset = *tzusec = 0;
        return *(tz + 1) != '\0' ? -5 : 1;
    }
    int sign = *tz == '-' ? -1 : 1;
    ++tz;
    int th = 0, tm = 0, ts = 0;
    rv = iso_parse_hh_mm_ss_ff(tz, p_end, &th, &tm, &ts, tzusec);
    *tzoffset = sign * (th * 3600 + tm * 60 + ts);
    *tzusec *= sign;
    return rv ? -5 : 1;
}

// datetime.fromisoformat for an ASCII string; nullopt where Python raises ValueError.
std::optional<IsoDT> py_fromisoformat(const std::string& str) {
    if (str.size() < 7) return std::nullopt;
    for (unsigned char c : str)
        if (c >= 0x80 || c == 0) return std::nullopt;  // the port handles ASCII only
    const char* s = str.c_str();
    int64_t len = (int64_t)str.size();
    int64_t sep = iso_find_separator(s, len);
    IsoDT dt;
    int tzoffset = 0, tzusec = 0;
    int rv = iso_parse_date(s, sep < 0 ? (size_t)-1 : (size_t)sep, dt.year, dt.month, dt.day);
    if (!rv && len > sep) rv = iso_parse_time(s + sep + 1, (size_t)(len - sep - 1), dt, &tzoffset, &tzusec);
    if (rv < 0) return std::nullopt;
    if (rv == 1) {  // tzinfo_from_isoformat_results: a zero-second offset is UTC
        dt.aware = true;
        if (tzoffset != 0) {
            int64_t off = (int64_t)tzoffset * 1000000 + tzusec;
            if (off <= -86400LL * 1000000 || off >= 86400LL * 1000000) return std::nullopt;
            dt.offset_us = off;
        }
    }
    // new_datetime_ex2's range checks
    if (dt.year < 1 || dt.year > 9999 || dt.month < 1 || dt.month > 12 || dt.day < 1 ||
        dt.day > iso_days_in_month(dt.year, dt.month) || dt.hour > 23 || dt.minute > 59 || dt.second > 59 ||
        dt.us > 999999)
        return std::nullopt;
    return dt;
}

std::string py_strip(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && py::is_space(s[b])) ++b;
    while (e > b && py::is_space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

}  // namespace

std::optional<int64_t> start_ms(const mj::Value* v) {
    if (!v || v->is_null() || v->is_bool()) return std::nullopt;
    if (v->is_int()) {  // int(v if v > 1e11 else v * 1000) if v > 0 else None
        if (v->i <= 0) return std::nullopt;
        return v->i > 100000000000LL ? v->i : v->i * 1000;
    }
    if (v->is_float()) {
        double x = v->d;
        if (!(x > 0)) return std::nullopt;
        double y = x > 1e11 ? x : x * 1000;
        if (std::isinf(y)) throw std::runtime_error("cannot convert float infinity to integer");
        double t = std::trunc(y);
        // Python's int is unbounded; any of these is centuries past now_ms().
        return t >= 9.2e18 ? std::numeric_limits<int64_t>::max() : (int64_t)t;
    }
    if (!v->is_string()) return std::nullopt;  // str(list/dict) never parses as a date
    std::string s = py_strip(v->s);
    for (size_t at = s.find('Z'); at != std::string::npos; at = s.find('Z', at + 6)) s.replace(at, 1, "+00:00");
    auto dt = py_fromisoformat(s);
    if (!dt) return std::nullopt;
    // (self - EPOCH).total_seconds() * 1000, truncated: the exact microsecond
    // count, divided by 10**6 with one correct rounding (strtod of the decimal).
    int64_t days = iso_ymd_to_ord(dt->year, dt->month, dt->day) - 719163;  // ordinal of 1970-01-01
    int64_t total_us = (days * 86400 + dt->hour * 3600 + dt->minute * 60 + dt->second) * 1000000LL + dt->us -
                       dt->offset_us;
    uint64_t mag = total_us < 0 ? (uint64_t)(-(total_us + 1)) + 1 : (uint64_t)total_us;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s%llu.%06llu", total_us < 0 ? "-" : "", (unsigned long long)(mag / 1000000),
                  (unsigned long long)(mag % 1000000));
    double ts = std::strtod(buf, nullptr);
    return (int64_t)std::trunc(ts * 1000);
}

// ===========================================================================
// PairSettings
// ===========================================================================

PairSettings::PairSettings(const mj::Value& pair, const mj::Value& bot_cfg, int default_spread_cents) {
    reverse = truthy(pair.get("reverse"));
    min_edge = cfg_float(pair, bot_cfg, "min_edge_threshold", 0.05);
    max_edge = cfg_float(pair, bot_cfg, "max_edge_threshold", 0.20);
    max_bet = cfg_float(pair, bot_cfg, "max_bet_size", 1000);
    max_position = cfg_float(pair, bot_cfg, "max_position", 2000);
    min_cooldown = cfg_float(pair, bot_cfg, "min_time_between_trades", 2.0);
    edge_scale_start = cfg_float(pair, bot_cfg, "edge_scale_start", 0.10);
    size_edge_power = cfg_float(pair, bot_cfg, "size_edge_power", 0.85);
    const mj::Value* spread = pair.contains("max_kalshi_spread") ? pair.get("max_kalshi_spread")
                                                                 : pair.get("max_polymarket_spread");
    if (spread && !spread->is_null())
        max_spread_cents = (int)py::round0(py_float_value(spread, "max_kalshi_spread") * 100);
    else
        max_spread_cents = default_spread_cents;
    clearing_edge = cfg_float(pair, bot_cfg, "clearing_edge_threshold", 0.01);
    extreme_multiplier = cfg_float(pair, bot_cfg, "extreme_bet_multiplier", EXTREME_BET_MULTIPLIER);
    enable_amm = truthy(pick(pair, bot_cfg, "enable_amm"));
    amm_target_fraction = cfg_float(pair, bot_cfg, "amm_target_fraction", 1.0);
    settle_slam = config_bool(pick(pair, bot_cfg, "settle_slam"));
    // PyYAML reads an unquoted `no` as False; that is the documented value.
    const mj::Value* tie = pick(pair, bot_cfg, "tie_resolves");
    std::string t;
    if (!tie) t = "no";
    else if (tie->is_bool() && !tie->b) t = "no";
    else t = py_str(tie);
    size_t b = 0, e = t.size();
    while (b < e && py::is_space(t[b])) ++b;
    while (e > b && py::is_space(t[e - 1])) --e;
    t = t.substr(b, e - b);
    for (char& c : t) c = (char)std::tolower((unsigned char)c);
    tie_resolves = t;
    limit_offset = cfg_float(pair, bot_cfg, "limit_offset", 0.01);
    min_edge_floor = cfg_float(pair, bot_cfg, "min_edge_floor", 0.01);
    adding_edge_tolerance = cfg_float(pair, bot_cfg, "adding_edge_tolerance", 0.0);
    min_clear_room = cfg_float(pair, bot_cfg, "min_clearing_room", 0.005);
    min_bet_size = cfg_float(pair, bot_cfg, "min_bet_size", 5);
    cap_clear_at_position = config_bool(pick(pair, bot_cfg, "cap_clear_at_position"));
    // Two-leg pairs (partner_ticker): how long, in a LIVE game, to follow the
    // leg that changed last when the two legs disagree. See two_leg_quote.
    {
        const mj::Value* v = pick(pair, bot_cfg, "partner_trust_ms");
        partner_trust_ms = v ? py_int_value(v, "partner_trust_ms") : 1000;
    }
    // ...and how far past our OWN leg's quote that trusted partner may take
    // the price. A glitched partner book (one leg 0.95, the other implying
    // 0.01) must not become an 80c edge with a size to match.
    partner_max_lead = cfg_float(pair, bot_cfg, "partner_max_lead", 0.05);
    // Underdog buys need more edge (off unless both are set): an order whose bought
    // outcome (the answer for YES, its complement for NO) has a source mid below
    // underdog_threshold needs at least underdog_min_edge, adding or clearing.
    underdog_threshold = cfg_float(pair, bot_cfg, "underdog_threshold", 0.0);
    underdog_min_edge = cfg_float(pair, bot_cfg, "underdog_min_edge", 0.0);

    edge_range = max_edge - min_edge;
    scale_divisor = std::max(1.0 - edge_scale_start, 1e-9);
    min_trigger_edge = std::min(min_edge, clearing_edge);
    trigger_eps = std::max(CLEAR_EDGE_EPS, adding_edge_tolerance);
}

double PairSettings::bet_size(double edge) const {
    double edge_pct = std::fabs(edge) * 100.0;
    if (edge_pct <= 0) return 0.0;
    return max_bet * std::pow(edge_pct, size_edge_power);
}

double PairSettings::scale_factor(int64_t position, bool is_adding) const {
    if (!is_adding) return 0.0;
    double position_pct = (double)std::llabs(position) / max_position;
    if (position_pct <= edge_scale_start) return 0.0;
    return std::min((position_pct - edge_scale_start) / scale_divisor, 1.0);
}

double PairSettings::required_edge(int64_t position, bool is_adding) const {
    if (!is_adding) return clearing_edge;
    return std::max(min_edge_floor, min_edge + edge_range * scale_factor(position, is_adding));
}

double PairSettings::limit_price_adjustment(int64_t position, bool is_adding, double edge) const {
    return std::fabs(edge) * 0.50 * scale_factor(position, is_adding);
}

// ===========================================================================
// Transport
// ===========================================================================

// C++ addition for local benchmarks: MANIFOLD_API_BASE (default
// https://api.manifold.markets) and MANIFOLD_WS_URL point the bot elsewhere.
static net::Url manifold_api_base() {
    net::Url u;
    const char* env = std::getenv("MANIFOLD_API_BASE");
    if (!(env && *env && net::parse_url(env, u))) {
        u.tls = true;
        u.host = "api.manifold.markets";
        u.port = 443;
    }
    return u;
}

static net::Headers manifold_headers(const std::string& api_key) {
    return {{"Authorization", "Key " + api_key},
            {"Content-Type", "application/json"},
            {"Accept", "*/*"},
            {"User-Agent", "manifold-bot-cpp/1.0"}};
}

HttpOrderTransport::HttpOrderTransport(const std::string& api_key)
    : orders_(manifold_api_base().host, manifold_api_base().port, manifold_api_base().tls,
              manifold_headers(api_key), 100, ORDER_KEEPALIVE_SEC),
      reads_(manifold_api_base().host, manifold_api_base().port, manifold_api_base().tls,
             manifold_headers(api_key), 100, ORDER_KEEPALIVE_SEC) {}

void HttpOrderTransport::post_bet(const std::string& body, double timeout, net::RequestTiming* timing,
                                  std::function<void(net::HttpResult)> cb) {
    orders_.request("POST", "/v0/bet", body, timeout, std::move(cb), timing);
}
void HttpOrderTransport::get_order_pool(const std::string& path, double timeout,
                                        std::function<void(net::HttpResult)> cb) {
    orders_.request("GET", path, "", timeout, std::move(cb));
}
void HttpOrderTransport::get_read_pool(const std::string& path, double timeout,
                                       std::function<void(net::HttpResult)> cb) {
    reads_.request("GET", path, "", timeout, std::move(cb));
}
void HttpOrderTransport::close() {
    orders_.close_all();
    reads_.close_all();
}

// ===========================================================================
// OrderTask / ManifoldOrderClient
// ===========================================================================

void OrderTask::add_done_callback(std::function<void(OrderTask&)> f) {
    if (done) {
        // asyncio: a callback added to a finished task is scheduled, not called.
        OrderTask* self = this;
        ev::Loop::get().call_soon([self, f] { f(*self); });
        return;
    }
    callbacks.push_back(std::move(f));
}

struct OrderCtx {
    std::string cid, aid_s;
    AidOpt aid;
    double amount;
    bool yes;
    std::string body;
    PosKey key;
    std::string status = "unknown";
    int http_status = 0;
    double limit;
    double queued_wall;
};

ManifoldOrderClient::ManifoldOrderClient(std::string api_key, std::string uid, std::unique_ptr<OrderTransport> t)
    : user_id(std::move(uid)), transport_(std::move(t)) {
    (void)api_key;
}

void ManifoldOrderClient::log(std::string line) { logx::log(std::move(line)); }

void ManifoldOrderClient::warm_pool(int n, bool verbose, std::function<void()> done) {
    // Leave warm sockets available for orders during a burst.
    if (closing || !pending_tasks.empty()) {
        if (done) done();
        return;
    }
    auto ok = std::make_shared<int>(0);
    auto remaining = std::make_shared<int>(n);
    for (int i = 0; i < n; ++i) {
        transport_->get_order_pool("/v0/me", 10.0, [=](net::HttpResult r) {
            if (r.kind == net::HttpResult::Ok && r.status == 200) ++*ok;
            if (--*remaining == 0) {
                if (verbose)
                    logx::out("  → Order pool warm-up: " + std::to_string(*ok) + "/" + std::to_string(n) +
                              " authenticated requests succeeded");
                if (done) done();
            }
        });
    }
    if (n <= 0 && done) done();
}

int64_t ManifoldOrderClient::get_cached_position(const std::string& cid, const AidOpt& aid) const {
    PosKey key{cid, (aid && !aid->empty()) ? *aid : ""};
    int64_t a = 0, b = 0;
    if (auto it = position_cache.find(key); it != position_cache.end()) a = it->second;
    if (auto it = pending_delta.find(key); it != pending_delta.end()) b = it->second;
    return a + b;
}

void ManifoldOrderClient::record_trade(const std::string& cid, const std::string& aid, bool yes, double amount) {
    PosKey key{cid, aid};
    int64_t d = py::trunc_i64(amount);  // int() truncates toward zero
    pending_delta[key] += yes ? d : -d;
    position_versions[key] += 1;
}

void ManifoldOrderClient::fetch_position(const std::string& cid, const AidOpt& aid,
                                         std::function<void(std::optional<int64_t>)> cb) {
    std::string path = "/v0/market/" + cid + "/positions?userId=" + net::url_quote(user_id);
    if (aid && !aid->empty()) path += "&answerId=" + net::url_quote(*aid);
    transport_->get_read_pool(path, 10.0, [cb](net::HttpResult r) {
        if (r.kind != net::HttpResult::Ok || r.status != 200) {
            cb(std::nullopt);
            return;
        }
        try {
            mj::Value data = mj::parse(r.body);
            if (!data.truthy()) {
                cb(0);
                return;
            }
            if (!data.is_array()) {
                cb(std::nullopt);  // data[0] on a dict -> KeyError
                return;
            }
            const mj::Value& first = data.a[0];
            if (!first.is_object()) {
                cb(std::nullopt);
                return;
            }
            const mj::Value* spend = first.get("totalSpent");
            double y = 0, n = 0;
            if (spend && spend->is_object()) {
                if (const mj::Value* v = spend->get("YES")) {
                    if (!v->is_number()) { cb(std::nullopt); return; }
                    y = v->num();
                }
                if (const mj::Value* v = spend->get("NO")) {
                    if (!v->is_number()) { cb(std::nullopt); return; }
                    n = v->num();
                }
            } else if (spend) {
                cb(std::nullopt);
                return;
            }
            cb(py::trunc_i64(y - n));
        } catch (...) {
            cb(std::nullopt);
        }
    });
}

void ManifoldOrderClient::refresh_all_positions(const std::vector<Pair*>& pairs,
                                                const std::unordered_map<std::string, std::string>& slug_to_id,
                                                std::function<void()> done) {
    struct Item {
        std::string cid, aid, name;
        int64_t version;
        std::optional<int64_t> result;
    };
    auto items = std::make_shared<std::vector<Item>>();
    for (Pair* p : pairs) {
        if (!p->slug) continue;
        auto it = slug_to_id.find(*p->slug);
        if (it == slug_to_id.end() || it->second.empty()) continue;
        std::string aid = (p->aid && !p->aid->empty()) ? *p->aid : "";
        PosKey key{it->second, aid};
        if (auto a = active_orders.find(key); a != active_orders.end() && a->second) continue;
        int64_t ver = 0;
        if (auto v = position_versions.find(key); v != position_versions.end()) ver = v->second;
        const mj::Value* nm = p->raw.get("name");
        items->push_back(Item{it->second, aid, py_slice(nm ? py_str(nm) : *p->slug, 25), ver, std::nullopt});
    }
    if (items->empty()) {
        if (done) done();
        return;
    }
    auto remaining = std::make_shared<size_t>(items->size());
    for (size_t i = 0; i < items->size(); ++i) {
        const Item& it = (*items)[i];
        fetch_position(it.cid, it.aid.empty() ? AidOpt() : AidOpt(it.aid), [this, items, remaining, i, done](std::optional<int64_t> r) {
            (*items)[i].result = r;
            if (--*remaining) return;
            for (const Item& x : *items) {
                if (!x.result) continue;
                PosKey key{x.cid, x.aid};
                auto a = active_orders.find(key);
                int64_t ver = 0;
                if (auto v = position_versions.find(key); v != position_versions.end()) ver = v->second;
                if ((a != active_orders.end() && a->second) || ver != x.version) continue;
                position_cache[key] = *x.result;
                pending_delta[key] = 0;
                if (debug_enabled()) log("  [POS] " + x.name + ": " + py::signed_fixed((double)*x.result, 0));
            }
            if (done) done();
        });
    }
}

static std::string order_body(const std::string& cid, double amount, double limit_prob, bool yes,
                              const AidOpt& aid, int64_t expires_ms) {
    // {"amount", "contractId", "outcome", "limitProb", "expiresMillisAfter", "deps": [], "answerId"}
    std::string b;
    b.reserve(160);
    b += "{\"amount\":";
    b += py::orjson_float(amount);
    b += ",\"contractId\":";
    mj::dump_string(b, cid);
    b += yes ? ",\"outcome\":\"YES\"" : ",\"outcome\":\"NO\"";
    b += ",\"limitProb\":";
    b += py::orjson_float(limit_prob);
    b += ",\"expiresMillisAfter\":";
    b += std::to_string(expires_ms);
    b += ",\"deps\":[]";
    if (aid && !aid->empty()) {
        b += ",\"answerId\":";
        mj::dump_string(b, *aid);
    }
    b += "}";
    return b;
}

static std::optional<double> to_float_v(const mj::Value* v) {
    if (!v) return std::nullopt;
    switch (v->type) {
        case mj::Type::Int: return (double)v->i;
        case mj::Type::Float: return v->d;
        case mj::Type::Bool: return v->b ? 1.0 : 0.0;
        case mj::Type::String: return py::float_from_str(v->s);
        default: return std::nullopt;
    }
}

static std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

OrderHandle ManifoldOrderClient::place_order(const std::string& cid, double amount, double limit_prob, bool yes,
                                             const AidOpt& aid, int64_t expires_ms) {
    if (closing) throw std::runtime_error("Order client is closing");
    double limit = std::max(0.01, std::min(0.99, py::round_n(limit_prob, 2)));
    auto ctx = std::make_shared<OrderCtx>();
    ctx->cid = cid;
    ctx->aid_s = (aid && !aid->empty()) ? *aid : "";
    ctx->aid = aid;
    ctx->amount = amount;
    ctx->yes = yes;
    ctx->limit = limit;
    // deps=[] skips only the server's outer dependency-discovery pass.
    ctx->body = order_body(cid, amount, limit, yes, aid, expires_ms);
    ctx->key = PosKey{cid, ctx->aid_s};
    auto task = std::make_shared<OrderTask>();
    task->queued_at = clk::perf_counter();
    ctx->queued_wall = clk::time_sec();
    record_trade(cid, ctx->aid_s, yes, amount);
    active_orders[ctx->key] += 1;
    pending_tasks.insert(task.get());

    // place_order's own done callback (registered first, runs first).
    task->callbacks.push_back([this, ctx](OrderTask& t) {
        pending_tasks.erase(&t);
        auto it = active_orders.find(ctx->key);
        if (it != active_orders.end()) {
            if (--it->second == 0) active_orders.erase(it);
        }
    });

    // Eager dispatch: the request is written before place_order returns.
    task->started_at = clk::perf_counter();
    attempt(task, ctx, 0);
    return task;
}

void ManifoldOrderClient::attempt(const OrderHandle& task, std::shared_ptr<OrderCtx> ctx, int n) {
    task->attempts = n + 1;
    auto undo = [this, ctx] { record_trade(ctx->cid, ctx->aid_s, !ctx->yes, ctx->amount); };
    auto settle = [this, ctx](double spent) {
        int64_t correction = py::trunc_i64(spent) - py::trunc_i64(ctx->amount);
        if (correction) record_trade(ctx->cid, ctx->aid_s, ctx->yes, (double)correction);
    };
    transport_->post_bet(ctx->body, 10.0, &task->rt, [this, task, ctx, n, undo, settle](net::HttpResult r) {
        using K = net::HttpResult;
        if (r.kind == K::ConnectError) {
            // Connection establishment failed, so this attempt never sent a bet.
            ctx->status = "not sent";
            undo();
            log("  ✗ Order not sent: ClientConnectorError");
            finish(task, ctx, std::nullopt);
            return;
        }
        if (r.kind != K::Ok) {
            // A timeout/disconnect does NOT mean the bet failed: no retry, no rollback.
            const char* name = r.kind == K::Timeout ? "TimeoutError"
                               : r.kind == K::Disconnected ? "ServerDisconnectedError"
                                                           : "ClientResponseError";
            log(std::string("  ⚠ Order outcome unknown (") + name + "); not retrying");
            finish(task, ctx, std::nullopt);
            return;
        }
        ctx->http_status = r.status;
        if (r.status == 200) {
            mj::Value data;
            try {
                data = mj::parse(r.body);
            } catch (...) {
                log("  ⚠ Order outcome unknown (JSONDecodeError); not retrying");
                finish(task, ctx, std::nullopt);
                return;
            }
            if (!data.is_object()) {
                log("  ⚠ Order outcome unknown (ValueError); not retrying");
                finish(task, ctx, std::nullopt);
                return;
            }
            auto spent = to_float_v(data.get("amount"));
            auto shares = to_float_v(data.get("shares"));
            if (!spent || !shares) {
                ctx->status = "accepted (fill unknown)";  // keep the reservation
            } else if (*shares == 0) {
                ctx->status = "unfilled";
                undo();
            } else {
                const mj::Value* f = data.get("isFilled");
                if ((f && f->is_bool() && f->b) || *spent >= ctx->amount) ctx->status = "filled";
                else ctx->status = "partial fill";
                settle(*spent);
            }
            const mj::Value* bid = data.get("betId");
            std::string bet = py_slice(bid ? (bid->is_string() ? bid->s : py_str(bid)) : "", 8);
            log("  ✓ Order " + ctx->status + ": " + bet + " | spent " + (spent ? py::repr(*spent) : "None") +
                " / M$" + py::g(ctx->amount) + ", shares " + (shares ? py::repr(*shares) : "None"));
            finish(task, ctx, std::move(data));
            return;
        }
        std::string message = r.body;
        std::string low = lower(message);
        bool retryable = r.status == 503 && (low.find("try betting again") != std::string::npos ||
                                             low.find("high volume") != std::string::npos);
        if (retryable && n == 0) {
            attempt(task, ctx, 1);  // explicit pre-commit rejection: one immediate retry
            return;
        }
        // Other server failures may happen AFTER committing the bet: keep the reservation.
        int s = r.status;
        bool rejected = retryable || s == 400 || s == 401 || s == 403 || s == 404 || s == 409 || s == 422 || s == 429;
        if (rejected) {
            ctx->status = "rejected";
            undo();
        }
        log("  ✗ Order " + ctx->status + ": HTTP " + std::to_string(s) + " " + py_slice(message, 100));
        finish(task, ctx, std::nullopt);
    });
}

void ManifoldOrderClient::finish(const OrderHandle& task, std::shared_ptr<OrderCtx> ctx,
                                 std::optional<mj::Value> result) {
    double end = clk::perf_counter();
    task->end = end;
    double queued = task->queued_at;
    std::optional<double> write_ms;
    if (task->rt.headers_at >= 0) write_ms = (task->rt.headers_at - queued) * 1000;
    double pool_wait_ms = task->rt.pool_wait_ms;
    if (task->rt.pool_queued_at >= 0) pool_wait_ms += (end - task->rt.pool_queued_at) * 1000;
    OrderMetrics m;
    m.contract_id = ctx->cid;
    m.answer_id = ctx->aid_s;
    m.status = ctx->status;
    m.http_status = ctx->http_status;
    m.requested_amount = ctx->amount;
    if (result && result->is_object()) {
        m.amount = to_float_v(result->get("amount"));
        m.shares = to_float_v(result->get("shares"));
    }
    m.queue_ms = (task->started_at - queued) * 1000;
    m.pool_wait_ms = pool_wait_ms;
    m.write_ms = write_ms;
    m.response_ms = (end - queued) * 1000;
    m.connection_reused = task->rt.reused;
    m.attempts = task->attempts;
    recent_orders.push_back(m);
    if (recent_orders.size() > 128) recent_orders.pop_front();
    auto tri = [](int v) { return v < 0 ? std::string("None") : v ? std::string("True") : std::string("False"); };
    log("    order timing: dispatch " + py::fixed(m.queue_ms, 2) + "ms (direct) | pool " + py::fixed(pool_wait_ms, 2) +
        "ms | write " + (write_ms ? py::repr(*write_ms) : "None") + "ms | response " + py::fixed(m.response_ms, 1) +
        "ms | reuse " + tri(m.connection_reused) + " | attempts " + std::to_string(m.attempts));
    if (trace_sink) {
        mj::Value t = mj::Value::object();
        t.set("bot", mj::Value::string("cpp"));
        t.set("t_queued", mj::Value::number(ctx->queued_wall));
        if (write_ms) t.set("t_write", mj::Value::number(ctx->queued_wall + *write_ms / 1000.0));
        t.set("t_done", mj::Value::number(ctx->queued_wall + m.response_ms / 1000.0));
        t.set("amount", mj::Value::number(ctx->amount));
        t.set("limit", mj::Value::number(ctx->limit));
        t.set("outcome", mj::Value::string(ctx->yes ? "YES" : "NO"));
        t.set("status", mj::Value::string(ctx->status));
        t.set("http", mj::Value::integer(ctx->http_status));
        t.set("spent", m.amount ? mj::Value::number(*m.amount) : mj::Value::null());
        t.set("shares", m.shares ? mj::Value::number(*m.shares) : mj::Value::null());
        if (result && result->is_object()) {
            if (const mj::Value* b = result->get("betId")) t.set("betId", *b);
            if (const mj::Value* pa = result->get("probAfter")) t.set("probAfter", *pa);
        }
        t.set("attempts", mj::Value::integer(m.attempts));
        t.set("reused", mj::Value::integer(m.connection_reused));
        t.set("pool_wait_ms", mj::Value::number(pool_wait_ms));
        t.set("response_ms", mj::Value::number(m.response_ms));
        trace_sink(t);
    }
    task->result = std::move(result);
    task->done = true;
    // Done callbacks are scheduled, as asyncio does, in registration order.
    std::vector<std::function<void(OrderTask&)>> cbs;
    cbs.swap(task->callbacks);
    OrderHandle keep = task;
    for (auto& f : cbs) {
        ev::Loop::get().call_soon([keep, f] { f(*keep); });
    }
}

void ManifoldOrderClient::close(std::function<void()> done) {
    closing = true;
    double deadline = ev::Loop::monotonic() + 25;
    ev::Loop::get().run_until([this, deadline] { return pending_tasks.empty() || ev::Loop::monotonic() > deadline; });
    if (!pending_tasks.empty()) log("  ⚠ Timed out draining orders; pending outcomes may be unknown");
    transport_->close();
    if (done) done();
}

// ===========================================================================
// ManifoldWebSocketClient
// ===========================================================================

namespace {

bool is_num(const mj::Value& v) { return v.is_int() || v.is_float() || v.is_bool(); }
double numv(const mj::Value& v) { return v.is_bool() ? (v.b ? 1.0 : 0.0) : v.num(); }

// Python ==
bool py_eq(const mj::Value& a, const mj::Value& b) {
    if (is_num(a) && is_num(b)) {
        if (a.is_int() && b.is_int()) return a.i == b.i;
        return numv(a) == numv(b);
    }
    if (a.type != b.type) return false;
    switch (a.type) {
        case mj::Type::Null: return true;
        case mj::Type::String: return a.s == b.s;
        case mj::Type::Array:
            if (a.a.size() != b.a.size()) return false;
            for (size_t i = 0; i < a.a.size(); ++i)
                if (!py_eq(a.a[i], b.a[i])) return false;
            return true;
        case mj::Type::Object:
            if (a.o.size() != b.o.size()) return false;
            for (const auto& [k, v] : a.o) {
                const mj::Value* w = b.get(k);
                if (!w || !py_eq(v, *w)) return false;
            }
            return true;
        default: return false;
    }
}

// Python >  (TypeError across incomparable types)
bool py_gt(const mj::Value& a, const mj::Value& b) {
    if (is_num(a) && is_num(b)) {
        if (a.is_int() && b.is_int()) return a.i > b.i;
        return numv(a) > numv(b);
    }
    if (a.is_string() && b.is_string()) return a.s > b.s;
    throw std::runtime_error("'>' not supported between these types");
}

// A non-string answerId still has to be a distinct key that matches no pair.
AidOpt aid_key(const mj::Value* v) {
    if (!v || v->is_null()) return std::nullopt;
    if (v->is_string()) return v->s;
    return std::string("\x01") + v->dump();
}

std::optional<std::string> opt_str(const mj::Value* v) {
    if (!v || v->is_null()) return std::nullopt;
    if (v->is_string()) return v->s;
    return std::string("\x01") + v->dump();
}

}  // namespace

ManifoldWebSocketClient::ManifoldWebSocketClient(size_t max_size) : max_size_(max_size) {
    if (const char* u = std::getenv("MANIFOLD_WS_URL"); u && *u) ws_url = u;
}

std::optional<double> ManifoldWebSocketClient::get_cached_price(const std::string& slug, const AidOpt& aid) const {
    auto it = last_prices.find(SlugAid{slug, aid});
    if (it == last_prices.end()) return std::nullopt;
    return it->second;
}

std::optional<std::pair<double, double>> ManifoldWebSocketClient::get_cached_pool(const std::string& slug,
                                                                                const AidOpt& aid) const {
    auto it = last_pools.find(SlugAid{slug, aid});
    if (it == last_pools.end()) return std::nullopt;
    return it->second;
}

std::optional<double> ManifoldWebSocketClient::pick_prob(const mj::Value& obj) {
    // Never `p`: it is the CPMM weight, not a price.
    if (!obj.is_object()) throw std::runtime_error("object has no attribute 'get'");
    const mj::Value* v = obj.get("prob");
    if (v && !v->is_null()) return py_float_value(v, "prob");
    v = obj.get("probability");
    if (v && !v->is_null()) return py_float_value(v, "probability");
    return std::nullopt;
}

void ManifoldWebSocketClient::send(const std::string& s) {
    if (sender) {
        sender(s);
        return;
    }
    if (ws_) ws_->send_text(s);
}

void ManifoldWebSocketClient::get(const std::string& path, std::function<void(net::HttpResult)> cb) {
    if (http_get) {
        http_get(path, std::move(cb));
        return;
    }
    if (!http_) http_ = std::make_unique<net::HttpPool>(manifold_api_base().host, manifold_api_base().port,
                                                       manifold_api_base().tls,
                                                       net::Headers{{"Accept", "*/*"}, {"User-Agent", "manifold-bot-cpp/1.0"}});
    http_->request("GET", path, "", 5.0, std::move(cb));
}

void ManifoldWebSocketClient::connect(std::function<void(bool)> done) {
    auto ws = std::make_shared<net::WsClient>();
    ws_ = ws;
    connected_ = false;
    ws->on_open = [this, ws, done] {
        if (ws_ != ws) return;
        connected_ = true;
        logx::log("✓ Connected to Manifold WebSocket");
        done(true);
    };
    ws->on_message = [this, ws](std::string_view raw) {
        if (ws_ != ws) return;
        on_raw(raw);
    };
    ws->on_close = [this, ws, done](bool, int code, const std::string& why) {
        if (ws_ != ws) return;
        if (!connected_) {
            logx::log("✗ Failed to connect to Manifold: " + why);
            ws_.reset();
            done(false);
            return;
        }
        // The close code and reason (Python's log line has neither; it helps
        // tell a server close from a keepalive timeout).
        on_disconnect(std::to_string(code) + (why.empty() ? "" : " " + why));
    };
    net::WsOptions opt;
    opt.max_size = max_size_;
    opt.ping_interval = 20;
    opt.ping_timeout = 10;
    opt.close_timeout = 5;
    opt.open_timeout = 10;
    ws->connect(ws_url, std::move(opt));
}

void ManifoldWebSocketClient::subscribe_to_markets(const std::vector<Pair*>& pairs, Payloads* payloads,
                                                   std::function<void()> done) {
    logx::out("  → Fetching contract IDs for subscription...");
    // One request per unique slug: reuse the engine's /v0/slug payloads.
    std::shared_ptr<Payloads> owned;
    if (!payloads) {
        owned = std::make_shared<Payloads>();
        payloads = owned.get();
    }
    slug_to_id.clear();
    id_to_slug.clear();
    auto idx = std::make_shared<size_t>(0);
    // The step holds itself only weakly; a pending GET's callback holds it strongly.
    auto step = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> wstep = step;
    *step = [this, pairs, payloads, owned, idx, wstep, done] {
        while (*idx < pairs.size()) {
            Pair* p = pairs[(*idx)++];
            if (!p->slug || p->slug->empty() || slug_to_id.count(*p->slug)) continue;
            const std::string slug = *p->slug;
            auto record = [this, slug](const mj::Value& data) {
                const mj::Value* cid = data.get("id");
                if (cid && cid->truthy()) {
                    std::string c = py_str(cid);
                    slug_to_id[slug] = c;
                    id_to_slug[c] = slug;
                    logx::out("     " + py_slice(slug, 40) + " → " + c);
                }
            };
            auto it = payloads->find(slug);
            if (it != payloads->end()) {
                record(it->second);
                continue;
            }
            get("/v0/slug/" + slug, [slug, payloads, record, keep = wstep.lock()](net::HttpResult r) {
                if (r.kind == net::HttpResult::Ok && r.status == 200) {
                    try {
                        mj::Value data = mj::parse(r.body);
                        (*payloads)[slug] = data;
                        record(data);
                    } catch (const std::exception& e) {
                        logx::out("     ⚠ Failed to get ID for " + slug + ": " + e.what());
                    }
                } else if (r.kind != net::HttpResult::Ok) {
                    logx::out("     ⚠ Failed to get ID for " + slug + ": " + r.error);
                }
                if (keep) (*keep)();
            });
            return;
        }
        finish_subscribe(pairs, payloads, [owned, done] { done(); });
    };
    (*step)();
}

void ManifoldWebSocketClient::finish_subscribe(const std::vector<Pair*>& pairs, Payloads* payloads,
                                               std::function<void()> done) {
    mj::Value topics = mj::Value::array();
    topic_cache_.clear();
    // contract/{id} carries the WHOLE contract; subscribe only where a binary pair reads it.
    std::unordered_set<std::string> binary_slugs;
    bool none_slug_binary = false;
    for (Pair* p : pairs) {
        if (!(p->aid && !p->aid->empty())) {
            if (p->slug) binary_slugs.insert(*p->slug);
            else none_slug_binary = true;
        }
    }
    (void)none_slug_binary;
    int skipped = 0;
    for (Pair* p : pairs) {
        if (!p->slug) continue;
        auto it = slug_to_id.find(*p->slug);
        if (it == slug_to_id.end() || it->second.empty()) continue;
        const std::string& cid = it->second;
        std::string contract_topic = "contract/" + cid;
        if (!topic_cache_.count(contract_topic)) {
            if (binary_slugs.count(*p->slug)) {
                topics.push(mj::Value::string(contract_topic));
                topic_cache_[contract_topic] = TopicInfo{TopicType::Contract, *p->slug};
            } else {
                topic_cache_[contract_topic] = TopicInfo{TopicType::Skip, *p->slug};
                ++skipped;
            }
        }
        std::string answers_topic = contract_topic + "/updated-answers";
        if (!topic_cache_.count(answers_topic)) {
            topics.push(mj::Value::string(answers_topic));
            topic_cache_[answers_topic] = TopicInfo{TopicType::Answers, *p->slug};
        }
        if (track_movers || bet_trigger) {
            std::string bets_topic = contract_topic + "/new-bet";
            if (!topic_cache_.count(bets_topic)) {
                topics.push(mj::Value::string(bets_topic));
                topic_cache_[bets_topic] = TopicInfo{TopicType::NewBet, *p->slug};
            }
        }
    }
    size_t ntopics = topics.size();
    mj::Value sub = mj::Value::object();
    sub.set("type", mj::Value::string("subscribe"));
    sub.set("txid", mj::Value::integer(txid));
    sub.set("topics", std::move(topics));
    send(sub.dump());
    txid += 1;
    logx::out("  → Subscribed to " + std::to_string(ntopics) + " Manifold topics (using contract IDs)");
    if (skipped)
        logx::out("     (skipped " + std::to_string(skipped) +
                  " whole-contract topic(s) — multi-choice only, price+pool come from updated-answers)");
    fetch_initial_prices(pairs, payloads, std::move(done));
}

void ManifoldWebSocketClient::fetch_initial_prices(const std::vector<Pair*>& pairs, Payloads* payloads,
                                                   std::function<void()> done) {
    logx::out("  → Fetching initial Manifold prices...");
    std::vector<std::string> missing;
    std::unordered_set<std::string> seen;
    for (Pair* p : pairs) {
        if (!p->slug || p->slug->empty() || !seen.insert(*p->slug).second) continue;
        if (!payloads->count(*p->slug)) missing.push_back(*p->slug);
    }
    auto idx = std::make_shared<size_t>(0);
    auto step = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> wstep = step;
    *step = [this, pairs, payloads, missing, idx, wstep, done] {
        if (*idx < missing.size()) {
            std::string slug = missing[(*idx)++];
            get("/v0/slug/" + slug, [payloads, slug, keep = wstep.lock()](net::HttpResult r) {
                if (r.kind == net::HttpResult::Ok && r.status == 200) {
                    try {
                        (*payloads)[slug] = mj::parse(r.body);
                    } catch (const std::exception& e) {
                        logx::out("     ⚠ Failed to fetch " + slug + ": " + e.what());
                    }
                } else if (r.kind != net::HttpResult::Ok) {
                    logx::out("     ⚠ Failed to fetch " + slug + ": " + r.error);
                }
                if (keep) (*keep)();
            });
            return;
        }
        for (Pair* p : pairs) {
            if (!p->slug || p->slug->empty()) continue;
            auto it = payloads->find(*p->slug);
            if (it == payloads->end()) continue;
            const mj::Value& data = it->second;
            const mj::Value* ot = data.get("outcomeType");
            bool mc = ot && ot->is_string() && (ot->s == "MULTIPLE_CHOICE" || ot->s == "INDEPENDENT_MULTIPLE_CHOICE");
            if (p->aid && !p->aid->empty() && mc) {
                const mj::Value* answers = data.get("answers");
                if (!answers || !answers->is_array()) continue;
                for (const mj::Value& ans : answers->a) {
                    const mj::Value* id = ans.get("id");
                    if (id && id->is_string() && id->s == *p->aid) {
                        const mj::Value* pr = ans.contains("prob") ? ans.get("prob") : ans.get("probability");
                        double prob = 0;
                        if (pr && !pr->is_null()) prob = py_float_value(pr, "prob");
                        if (pr && pr->is_null()) last_prices.erase(SlugAid{*p->slug, p->aid});
                        else last_prices[SlugAid{*p->slug, p->aid}] = prob;
                        logx::out("     • " + *p->slug + "/" + py_slice(*p->aid, 8) + "... @ " + py::pct(prob, 1));
                        break;
                    }
                }
            } else {
                const mj::Value* pr = data.get("probability");
                double prob = 0;
                if (pr && !pr->is_null()) prob = py_float_value(pr, "probability");
                if (pr && pr->is_null()) last_prices.erase(SlugAid{*p->slug, std::nullopt});
                else last_prices[SlugAid{*p->slug, std::nullopt}] = prob;
                logx::out("     • " + *p->slug + " @ " + py::pct(prob, 1));
            }
        }
        done();
    };
    (*step)();
}

void ManifoldWebSocketClient::handle_frame(std::string_view raw, double recv_time, const Callback& callback) {
    mj::Value msg = mj::parse(raw);
    if (!msg.is_object()) throw std::runtime_error("'list' object has no attribute 'get'");
    const mj::Value* t = msg.get("type");
    if (!t || !t->is_string() || t->s != "broadcast") return;
    const mj::Value* topicv = msg.get("topic");
    if (topicv && (topicv->is_array() || topicv->is_object())) throw std::runtime_error("unhashable type");
    if (topicv && !topicv->is_string()) return;
    std::string topic = topicv ? topicv->s : "";
    const mj::Value* data = msg.get("data");
    if (!data || !data->truthy()) return;
    auto it = topic_cache_.find(topic);
    if (it == topic_cache_.end()) return;
    const TopicType type = it->second.type;
    const std::string slug = it->second.slug;
    if (type == TopicType::Skip) return;

    if (type == TopicType::NewBet) {
        if (!data->is_object()) throw std::runtime_error("object has no attribute 'get'");
        const mj::Value* bets = data->get("bets");
        if (!bets || !bets->truthy()) return;
        if (bets->is_int() || bets->is_float() || bets->is_bool()) throw std::runtime_error("object is not iterable");
        if (!bets->is_array()) return;  // a dict/str iterates to non-dict items: all skipped
        int64_t recv_ms = (int64_t)(recv_time * 1000);
        std::vector<std::pair<SlugAid, double>> prices;
        std::vector<SlugAid> seen, dup;
        auto contains = [](const std::vector<SlugAid>& v, const SlugAid& k) {
            return std::find(v.begin(), v.end(), k) != v.end();
        };
        for (const mj::Value& bet : bets->a) {
            if (!bet.is_object()) continue;
            const mj::Value* pb = bet.get("probBefore");
            const mj::Value* pa = bet.get("probAfter");
            // Unfilled IOC orders and real redemptions moved nothing.
            if (!pb || pb->is_null() || !pa || pa->is_null() || py_eq(*pa, *pb)) continue;
            SlugAid key{slug, aid_key(bet.get("answerId"))};
            // A sibling record (isRedemption) prices its answer but is nobody's move.
            if (!truthy(bet.get("isRedemption"))) {
                last_mover[key] = Mover{opt_str(bet.get("userId")), py_gt(*pa, *pb) ? 1 : -1, recv_ms};
            }
            if (bet_trigger) {
                if (contains(seen, key)) {
                    if (!contains(dup, key)) dup.push_back(key);
                }
                if (!contains(seen, key)) seen.push_back(key);
                const mj::Value* ctv = bet.get("createdTime");
                double ct = 0;
                if (ctv && ctv->truthy()) {
                    if (!is_num(*ctv)) throw std::runtime_error("'>=' not supported");
                    ct = numv(*ctv);
                }
                auto prev = bet_price_ct_.find(key);
                double prev_ct = prev == bet_price_ct_.end() ? 0 : prev->second;
                // Never let an older bet overwrite a newer one's price.
                if (ct >= prev_ct) {
                    bet_price_ct_[key] = ct;
                    double prob = py_float_value(pa, "probAfter");
                    auto pit = std::find_if(prices.begin(), prices.end(), [&](const auto& e) { return e.first == key; });
                    if (pit == prices.end()) prices.emplace_back(key, prob);
                    else pit->second = prob;
                }
            }
        }
        if (!dup.empty() && !prices.empty()) {
            prices.erase(std::remove_if(prices.begin(), prices.end(),
                                        [&](const auto& e) { return contains(dup, e.first); }),
                         prices.end());
        }
        for (const auto& [key, prob] : prices) {
            auto lp = last_prices.find(key);
            if (lp == last_prices.end() || !(lp->second == prob)) {
                last_prices[key] = prob;
                if (debug_enabled())
                    logx::log("[Manifold " + py_slice(key.slug, 12) + "/" + py_slice(key.aid.value_or(""), 8) + "] " +
                              py::fixed(prob, 4) + " (new-bet) @" + py::fixed(recv_time, 3));
                callback(Src::Manifold, key.slug, key.aid, prob, recv_time);
            }
        }
        return;
    }

    // Price frames often restate the new-bet price 1e-16 off. That re-runs the
    // decision on purpose: their value is Manifold's canonical one.
    if (type == TopicType::Contract) {
        if (!data->is_object()) throw std::runtime_error("object has no attribute 'get'");
        const mj::Value* contract = data->contains("contract") ? data->get("contract") : data;
        auto prob = pick_prob(*contract);
        if (prob) {
            SlugAid key{slug, std::nullopt};
            auto lp = last_prices.find(key);
            if (lp == last_prices.end() || !(lp->second == *prob)) {
                last_prices[key] = *prob;
                if (debug_enabled()) logx::log("[Manifold " + py_slice(slug, 20) + "] " + py::fixed(*prob, 4));
                callback(Src::Manifold, slug, std::nullopt, *prob, recv_time);
            }
        }
        return;
    }

    if (type == TopicType::Answers) {
        if (!data->is_object()) throw std::runtime_error("object has no attribute 'get'");
        const mj::Value* answers = data->get("answers");
        if (!answers || !answers->truthy()) return;
        if (!answers->is_array()) {
            if (answers->is_int() || answers->is_float() || answers->is_bool())
                throw std::runtime_error("object is not iterable");
            return;
        }
        for (const mj::Value& ans : answers->a) {
            if (!ans.is_object()) continue;
            const mj::Value* idv = ans.get("id");
            if (!idv || !idv->truthy()) continue;
            AidOpt ans_id = aid_key(idv);
            SlugAid key{slug, ans_id};
            const mj::Value* py_ = ans.get("poolYes");
            const mj::Value* pn = ans.get("poolNo");
            if (truthy(py_) && truthy(pn)) {
                if (!is_num(*py_) || !is_num(*pn)) throw std::runtime_error("'>' not supported");
                if (numv(*py_) > 0 && numv(*pn) > 0) last_pools[key] = {numv(*py_), numv(*pn)};
            }
            auto prob = pick_prob(ans);
            if (prob) {
                auto lp = last_prices.find(key);
                if (lp == last_prices.end() || !(lp->second == *prob)) {
                    last_prices[key] = *prob;
                    if (debug_enabled())
                        logx::log("[Manifold " + py_slice(slug, 12) + "/" + py_slice(ans_id.value_or(""), 8) + "] " +
                                  py::fixed(*prob, 4));
                    callback(Src::Manifold, slug, ans_id, *prob, recv_time);
                }
            }
        }
    }
}

void ManifoldWebSocketClient::on_raw(std::string_view raw) {
    if (!listening_) {
        pending_frames_.emplace_back(raw);  // websockets buffers until recv()
        return;
    }
    double recv_time = clk::time_sec();
    try {
        handle_frame(raw, recv_time, cb_);
    } catch (const std::exception& e) {
        logx::log(std::string("✗ Manifold error: ") + e.what() + " -> reconnecting...");
        teardown_and_retry();
    }
}

void ManifoldWebSocketClient::on_disconnect(const std::string& why) {
    if (!listening_) {
        // Closed before the receive loop started: its first recv() raises.
        ws_.reset();
        return;
    }
    logx::log("✗ Manifold connection closed (" + why + ") -> reconnecting...");
    teardown_and_retry();
}

void ManifoldWebSocketClient::teardown_and_retry() {
    listening_ = false;
    ++gen_;
    ev::Loop::get().cancel(pinger_);
    if (ws_) {
        auto ws = ws_;
        ws_.reset();
        ws->on_close = nullptr;
        ws->on_message = nullptr;
        ws->close(1000);
    }
    pending_frames_.clear();
    if (closed_) return;
    retry_ = ev::Loop::get().call_later(0.5, [this] { loop_top(); });
}

void ManifoldWebSocketClient::receive_messages(Callback cb, const std::vector<Pair*>& pairs) {
    cb_ = std::move(cb);
    pairs_ = pairs;
    loop_top();
}

void ManifoldWebSocketClient::loop_top() {
    if (closed_) return;
    if (!ws_) {
        logx::log("  → Reconnecting to Manifold...");
        connect([this](bool ok) {
            if (!ok) {
                retry_ = ev::Loop::get().call_later(0.5, [this] { loop_top(); });
                return;
            }
            subscribe_to_markets(pairs_, nullptr, [this] { listening(); });
        });
        return;
    }
    listening();
}

void ManifoldWebSocketClient::listening() {
    if (!ws_ || ws_->state() != net::WsClient::State::Open) {
        // The socket died during subscribe: go round again.
        ws_.reset();
        pending_frames_.clear();
        retry_ = ev::Loop::get().call_later(0.5, [this] { loop_top(); });
        return;
    }
    logx::log("  → Listening for Manifold updates (low-latency mode)...");
    listening_ = true;
    schedule_keepalive();
    std::deque<std::string> backlog;
    backlog.swap(pending_frames_);
    auto ws = ws_;
    for (const std::string& f : backlog) {
        if (ws_ != ws || !listening_) break;
        on_raw(f);
    }
}

void ManifoldWebSocketClient::schedule_keepalive() {
    uint64_t gen = gen_;
    pinger_ = ev::Loop::get().call_later(MANIFOLD_PING_SEC, [this, gen] {
        if (gen != gen_ || !ws_) return;
        txid += 1;
        mj::Value ping = mj::Value::object();
        ping.set("type", mj::Value::string("ping"));
        ping.set("txid", mj::Value::integer(txid));
        if (!ws_->send_text(ping.dump())) {
            ws_->abort("keepalive send failed");  // wakes the reader to reconnect
            return;
        }
        schedule_keepalive();
    });
}

void ManifoldWebSocketClient::close() {
    closed_ = true;
    ev::Loop::get().cancel(pinger_);
    ev::Loop::get().cancel(retry_);
    if (ws_) {
        ws_->on_close = nullptr;
        ws_->close(1000);
    }
    if (http_) http_->close_all();
}

// ===========================================================================
// DualWebSocketArbitrageBot
// ===========================================================================

std::vector<Pair*> DualWebSocketArbitrageBot::pairs() const {
    std::vector<Pair*> v;
    for (const auto& p : market_pairs_) v.push_back(p.get());
    return v;
}

void DualWebSocketArbitrageBot::log(std::string line) { manifold_orders_.log(std::move(line)); }

DualWebSocketArbitrageBot::DualWebSocketArbitrageBot(ps::PriceSource& source, ManifoldWebSocketClient& manifold_ws,
                                                     ManifoldOrderClient& manifold_orders,
                                                     std::vector<std::unique_ptr<Pair>> market_pairs,
                                                     const mj::Value& config, std::string source_key,
                                                     std::string banner, bool quiet)
    : source_(source),
      manifold_ws_(manifold_ws),
      manifold_orders_(manifold_orders),
      market_pairs_(std::move(market_pairs)),
      source_key_(std::move(source_key)),
      banner_(std::move(banner)) {
    const mj::Value* b = config.get("bot");
    bot_cfg_ = b ? *b : mj::Value::object();
    if (!bot_cfg_.is_object()) throw std::runtime_error("'bot' must be a mapping");
    const mj::Value& bc = bot_cfg_;
    auto bf = [&](const char* k, double d) { const mj::Value* v = bc.get(k); return v ? py_float_value(v, k) : d; };
    auto bi = [&](const char* k, int64_t d) { const mj::Value* v = bc.get(k); return v ? py_int_value(v, k) : d; };
    auto out = [&](const std::string& s) { if (!quiet) logx::out(s); };

    const mj::Value* dr = bc.get("dry_run");
    dry_run = dr ? dr->truthy() : true;
    const mj::Value* ds = bc.contains("max_kalshi_spread") ? bc.get("max_kalshi_spread") : bc.get("max_polymarket_spread");
    double default_spread = ds ? py_float_value(ds, "max_kalshi_spread") : 0.40;
    int default_spread_cents = (int)py::round0(default_spread * 100);
    position_cache_interval_ = bf("position_cache_interval_sec", POSITION_CACHE_INTERVAL);
    position_refresh_interval_ = bf("position_refresh_interval_sec", POSITION_REFRESH_INTERVAL);
    position_refresh_batch_ = bi("position_refresh_batch_size", POSITION_REFRESH_BATCH);
    extreme_high_ = bf("extreme_high", EXTREME_HIGH);
    extreme_low_ = py::round_n(1.0 - extreme_high_, 6);
    extreme_converge_high_ = bf("extreme_converge_high", EXTREME_CONVERGE_HIGH);
    extreme_converge_low_ = py::round_n(1.0 - extreme_converge_high_, 6);
    min_clear_room_ = bf("min_clearing_room", 0.005);
    limit_offset_ = bf("limit_offset", 0.01);
    min_order_room_ = bf("min_order_room", 0.001);
    own_order_hold_ms_ = bi("own_order_hold_ms", 1000);
    if (const mj::Value* nf = bc.get("no_fade_users"); nf && nf->truthy() && nf->is_array()) {
        for (const mj::Value& u : nf->a)
            if (u.truthy() && u.is_string()) no_fade_users_.insert(u.s);
    }
    no_fade_window_ms_ = bi("no_fade_window_ms", 1000);
    if (!no_fade_users_.empty()) manifold_ws_.track_movers = true;
    {
        const mj::Value* nb = bc.get("new_bet_trigger");
        manifold_ws_.bet_trigger = nb ? config_bool(nb) : true;
    }
    last_cent_max_bet_ = bf("last_cent_max_bet", 0);
    skip_positions_ = truthy(bc.get("skip_positions"));
    amm_sizer_ = amm::AmmSizer(bf("max_bet_size", 1000), bf("min_bet_size", 5), bf("max_position", 2000),
                               bf("amm_target_fraction", 1.0));
    liq_config_ = amm::MarketLiquidityConfig(bf("default_liquidity", 200.0), 0.5);
    {
        mj::Value arr = mj::Value::array();
        for (auto& p : market_pairs_) arr.push(p->raw);
        liq_config_.load_overrides_from_pairs(arr);
    }

    int n_overridden = 0;
    for (auto& p : market_pairs_) {
        p->s = std::make_unique<PairSettings>(p->raw, bc, default_spread_cents);
        std::string ov;
        for (const std::string& k : PAIR_OVERRIDE_KEYS) {
            if (const mj::Value* v = p->raw.get(k)) {
                if (!ov.empty()) ov += ", ";
                ov += k + "=" + py_str(v);
            }
        }
        if (!ov.empty()) {
            ++n_overridden;
            const mj::Value* nm = p->raw.get("name");
            out("  → Override [" + py_slice(nm ? py_str(nm) : "?", 40) + "]: " + ov);
        }
    }
    if (n_overridden)
        out("  → " + std::to_string(n_overridden) + "/" + std::to_string(market_pairs_.size()) +
            " pairs have per-market overrides");
    {
        std::string bad;
        int nbad = 0;
        for (auto& p : market_pairs_) {
            const std::string& t = p->s->tie_resolves;
            if (t != "no" && t != "half" && t != "ignore") {
                if (nbad < 5) {
                    const mj::Value* nm = p->raw.get("name");
                    if (!bad.empty()) bad += ", ";
                    bad += py_slice(nm ? py_str(nm) : "?", 30);
                }
                ++nbad;
            }
        }
        if (nbad) out("  ⚠ tie_resolves must be no / half / ignore — ties will be IGNORED on: " + bad);
    }
    if (source_key_ != "kalshi_ticker" &&
        std::any_of(market_pairs_.begin(), market_pairs_.end(), [](auto& p) { return p->s->settle_slam; }))
        out("  ⚠ settle_slam is set, but only Kalshi sends settlement events — it will never fire on this source.");

    for (auto& up : market_pairs_) {
        Pair* p = up.get();
        if (p->sid && !p->sid->empty()) {
            auto& v = source_to_pairs_[*p->sid];
            if (v.empty()) source_ids_.push_back(*p->sid);
            v.push_back(p);
        }
        if (p->slug && !p->slug->empty()) contract_to_pairs_[SlugAid{*p->slug, p->aid}].push_back(p);
    }

    // Partner legs (pair: partner_ticker). A two-way game has a Kalshi market
    // per team, and the other team's leg prices this question too: its ask is
    // our implied bid (1 - ask), its bid our implied ask. Its quote changes
    // trigger the pair exactly like our own leg's, and two_leg_quote merges
    // the two. Kalshi only: a Polymarket game is one book for both outcomes.
    // "Which leg changed last" only means "which leg has the news" when quotes
    // come from the order book; the 1 Hz ticker's last arrival is often the
    // staler leg, so without a book-first source a live cross uses the band.
    partner_trust_ok_ = source_.prefer_book();
    {
        const mj::Value* pl = bc.get("partner_legs");
        bool use_partners = (pl ? config_bool(pl) : true) && source_key_ == "kalshi_ticker";
        auto event_of = [](const std::string& t) {  // t.rsplit('-', 1)[0]
            size_t at = t.rfind('-');
            return at == std::string::npos ? t : t.substr(0, at);
        };
        for (auto& up : market_pairs_) {
            Pair* p = up.get();
            p->partner.reset();
            const mj::Value* ptv = p->raw.get("partner_ticker");
            if (!ptv || !ptv->truthy()) continue;
            if (!use_partners) {
                ++unused_partner_;
                continue;
            }
            std::string pt = py_strip(py_str(ptv));
            // Both legs of one game share the event ticker (everything before the
            // last '-'); anything else is a mapping mistake, not a partner.
            if (!p->sid || p->sid->empty() || pt == *p->sid || event_of(pt) != event_of(*p->sid)) {
                const mj::Value* nm = p->raw.get("name");
                bad_partner_.push_back(nm ? py_str(nm) : (p->sid ? *p->sid : std::string("None")));
                continue;
            }
            p->partner = pt;
            p->game_start_ms = start_ms(p->raw.get("game_start"));
            partner_to_pairs_[pt].push_back(p);
            for (const std::string& tk : {*p->sid, pt}) {
                auto& couples = leg_couples_[tk];
                Couple key{*p->sid, pt};
                if (std::find(couples.begin(), couples.end(), key) == couples.end()) couples.push_back(key);
            }
            ++n_partner_;
        }
    }

    for (auto& p : market_pairs_)
        if (p->s->enable_amm) amm_pairs_.push_back(p.get());
    mj::Value empty = mj::Value::object();
    PairSettings s0(empty, bc, default_spread_cents);
    if (!amm_pairs_.empty())
        out("  → AMM sizing ON for " + std::to_string(amm_pairs_.size()) + "/" + std::to_string(market_pairs_.size()) +
            " pairs (default_liq=" + py::fixed(liq_config_.default_liquidity, 0) +
            ", target_fraction=" + py::pct(s0.amm_target_fraction, 0) + ")");
    out("  → Default edge scaling: " + py::pct(s0.min_edge, 1) + " (at <=" + py::pct(s0.edge_scale_start, 0) +
        " pos) → " + py::pct(s0.max_edge, 1) + " (at 100% pos)");
    std::string refresh;
    int64_t n = (int64_t)market_pairs_.size();
    if (0 < position_refresh_batch_ && position_refresh_batch_ < n) {
        double cycle = (double)((n + position_refresh_batch_ - 1) / position_refresh_batch_) * position_refresh_interval_;
        refresh = std::to_string(position_refresh_batch_) + "/" + std::to_string(n) + " pairs every " +
                  py::fixed(position_refresh_interval_, 0) + "s (full cycle " + py::fixed(cycle / 60, 1) + "m, ~" +
                  py::fixed(position_refresh_batch_ * 60 / position_refresh_interval_, 0) + " req/min)";
    } else {
        refresh = "all " + std::to_string(n) + " pairs every " + py::fixed(position_cache_interval_, 0) + "s";
    }
    out("  → Default clearing edge: " + py::signed_pct(s0.clearing_edge, 1) + " | position refresh: " + refresh);
    if (s0.underdog_threshold > 0 && s0.underdog_min_edge > 0)
        out("  → Underdog buys: outcomes under " + py::pct(s0.underdog_threshold, 0) + " need >= " +
            py::fixed(s0.underdog_min_edge * 100, 1) + "c edge (adding and clearing)");
    out("  → Extreme: " + py::fixed(s0.extreme_multiplier, 0) + "x at >= " + py::fixed(extreme_high_, 2) + " / <= " +
        py::fixed(extreme_low_, 2) + " (skip if Manifold already past " + py::fixed(extreme_converge_high_, 2) + " / " +
        py::fixed(extreme_converge_low_, 2) + ")");
    out("  → Default size scaling: bet = max_bet * (edge_pct ** " + py::repr(s0.size_edge_power) + ")");
    out("  → Limit offset: " + py::fixed(limit_offset_, 3) + " inside the source touch (" +
        py::fixed(limit_offset_ * 100, 1) + "c minimum edge per fill, rounded away from the touch)");
    if (limit_offset_ >= s0.min_edge)
        out("  ⚠ limit_offset (" + py::fixed(limit_offset_, 3) + ") >= default min_edge (" + py::fixed(s0.min_edge, 3) +
            ") — every adding limit lands at or past Manifold's price, so orders will fill ~nothing. "
            "Raise min_edge or lower the offset.");
    out("  → Own-order hold: " + (own_order_hold_ms_ ? std::to_string(own_order_hold_ms_) +
                                                           "ms (Manifold treated as at our limit until our fill shows up)"
                                                     : std::string("OFF")));
    if (!no_fade_users_.empty())
        out("  → No-fade guard: " + std::to_string(no_fade_users_.size()) + " user(s), " +
            std::to_string(no_fade_window_ms_) + "ms window, allowed once our quote moves after theirs");
    out(std::string("  → Manifold price from new-bet frames: ") + (manifold_ws_.bet_trigger ? "ON" : "OFF"));
    out("  → Last-cent order: " + (last_cent_max_bet_ > 0 ? "up to M$" + py::g(last_cent_max_bet_) +
                                                                " from our limit to the touch"
                                                          : std::string("OFF")));
    if (n_partner_) {
        int64_t extra = 0;
        for (const auto& [t, v] : partner_to_pairs_)
            if (!source_to_pairs_.count(t)) ++extra;
        std::string live_rule = partner_trust_ok_
                                    ? "follow the leg that moved last for " + std::to_string(s0.partner_trust_ms) +
                                          "ms (within " + py::fixed(s0.partner_max_lead, 2) + " of ours)"
                                    : "trade only where neither objects (trust window needs bot.prefer_book: true)";
        out("  → Partner legs: " + std::to_string(n_partner_) + " pair(s) also read the other team's leg (+" +
            std::to_string(extra) + " tickers). Legs disagree, live: " + live_rule +
            "; pregame: trade only where neither objects");
    }
    if (!bad_partner_.empty()) {
        std::string names;
        for (size_t k = 0; k < bad_partner_.size() && k < 5; ++k) {
            if (k) names += ", ";
            names += py_slice(bad_partner_[k], 30);
        }
        out("  ⚠ partner_ticker ignored (same ticker or a different event): " + names);
    }
    if (unused_partner_)
        out("  → partner_ticker set on " + std::to_string(unused_partner_) + " pair(s) but partner legs are OFF here");
}

std::optional<double> DualWebSocketArbitrageBot::amm_bet_size(Pair& pair, PairSettings& s, int64_t position, double mp,
                                                              double limit_prob) {
    std::string cid = pair.slug.value_or("");
    std::string aid = (pair.aid && !pair.aid->empty()) ? *pair.aid : "";
    double p_val = liq_config_.get_p(cid, aid);
    // Prefer the LIVE pool from the WS; the startup snapshot is the fallback.
    auto live = manifold_ws_.get_cached_pool(cid, pair.aid);
    double k = live ? amm::cpmm_invariant(live->first, live->second, p_val) : liq_config_.get_liquidity(cid, aid);
    auto sizing = amm_sizer_.compute_bet(mp, limit_prob, k, p_val, position, s.max_bet, s.max_position,
                                         s.amm_target_fraction, s.min_bet_size);
    if (!sizing) return std::nullopt;
    return sizing->amount;
}

std::optional<double> DualWebSocketArbitrageBot::bet_size_for_trade(Pair& pair, PairSettings& s, int64_t position,
                                                                    bool is_adding, double edge, double mp,
                                                                    double base_limit, double limit_prob) {
    if (s.enable_amm) return amm_bet_size(pair, s, position, mp, limit_prob);
    if (is_adding || edge >= s.min_edge) return s.bet_size(edge);
    // Sub-threshold edge = pure risk reduction: cap at the position.
    return std::min(s.max_bet, (double)std::llabs(position));
}

void DualWebSocketArbitrageBot::install_contract(const std::string& slug, const mj::Value& data) {
    const mj::Value* cid = data.get("id");
    if (cid && cid->truthy()) {
        slug_to_contract_id[slug] = py_str(cid);
        contract_payloads[slug] = data;
        on_contract_data(slug, data);
    }
}

void DualWebSocketArbitrageBot::on_contract_data(const std::string& slug, const mj::Value& data) {
    liq_config_.update_from_api(slug, data);
    // A two-leg pair needs to know when its game goes live (the trust window
    // in two_leg_quote only runs then). ManifoldSports markets carry the
    // scheduled start; a pair's own game_start wins.
    auto start = start_ms(data.get("sportsStartTimestamp"));
    if (start && *start != 0) {
        for (auto& up : market_pairs_) {
            Pair* p = up.get();
            if (p->partner && p->slug && *p->slug == slug && !(p->game_start_ms && *p->game_start_ms != 0))
                p->game_start_ms = start;
        }
    }
}

void DualWebSocketArbitrageBot::cache_contract_ids(std::function<void()> done) {
    logx::out("  → Caching Manifold contract IDs...");
    auto idx = std::make_shared<size_t>(0);
    auto step = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> wstep = step;
    *step = [this, idx, wstep, done] {
        while (*idx < market_pairs_.size()) {
            Pair* p = market_pairs_[(*idx)++].get();
            if (!p->slug || p->slug->empty() || slug_to_contract_id.count(*p->slug)) continue;
            std::string slug = *p->slug;
            manifold_orders_.transport().get_read_pool("/v0/slug/" + slug, 10.0, [this, slug, keep = wstep.lock()](net::HttpResult r) {
                if (r.kind == net::HttpResult::Ok && r.status == 200) {
                    try {
                        mj::Value data = mj::parse(r.body);
                        const mj::Value* cid = data.get("id");
                        if (cid && cid->truthy()) {
                            install_contract(slug, data);
                            logx::out("     [ID] " + py_slice(slug, 40) + " -> " + py_str(cid));
                        }
                    } catch (const std::exception& e) {
                        logx::out("     ⚠ Failed: " + slug + ": " + e.what());
                    }
                } else if (r.kind != net::HttpResult::Ok) {
                    logx::out("     ⚠ Failed: " + slug + ": " + r.error);
                }
                if (keep) (*keep)();
            });
            return;
        }
        done();
    };
    (*step)();
}

std::vector<Pair*> DualWebSocketArbitrageBot::next_refresh_batch() {
    std::vector<Pair*> all = pairs();
    int64_t batch = position_refresh_batch_;
    if (batch <= 0 || batch >= (int64_t)all.size()) return all;
    std::vector<Pair*> chosen;
    std::unordered_set<size_t> seen;
    for (size_t i = 0; i < all.size(); ++i) {
        Pair* p = all[i];
        if (!p->slug) continue;
        auto it = slug_to_contract_id.find(*p->slug);
        if (it == slug_to_contract_id.end() || it->second.empty()) continue;
        PosKey key{it->second, (p->aid && !p->aid->empty()) ? *p->aid : ""};
        auto pd = manifold_orders_.pending_delta.find(key);
        if (pd != manifold_orders_.pending_delta.end() && pd->second) {
            chosen.push_back(p);
            seen.insert(i);
            if ((int64_t)chosen.size() >= batch) return chosen;
        }
    }
    // Advance the cursor by the indices actually WALKED, not by `batch`.
    size_t n = all.size(), steps = 0;
    while (steps < n && (int64_t)chosen.size() < batch) {
        size_t i = (refresh_cursor_ + steps) % n;
        ++steps;
        if (!seen.count(i)) chosen.push_back(all[i]);
    }
    refresh_cursor_ = (refresh_cursor_ + steps) % n;
    return chosen;
}

void DualWebSocketArbitrageBot::position_refresh_tick() {
    bool batched = 0 < position_refresh_batch_ && position_refresh_batch_ < (int64_t)market_pairs_.size();
    double interval = batched ? position_refresh_interval_ : position_cache_interval_;
    refresh_timer_ = ev::Loop::get().call_later(interval, [this] {
        auto batch = next_refresh_batch();
        if (debug_enabled())
            log("\n  [POSITIONS] Refreshing " + std::to_string(batch.size()) + "/" + std::to_string(market_pairs_.size()) + "...");
        manifold_orders_.refresh_all_positions(batch, slug_to_contract_id, [this] { position_refresh_tick(); });
    });
}

void DualWebSocketArbitrageBot::order_warm_tick() {
    // Keep the order pool warm so a goal's POST never pays a TLS handshake.
    warm_timer_ = ev::Loop::get().call_later(WARM_HEARTBEAT_SEC, [this] {
        manifold_orders_.warm_pool(ORDER_POOL_SIZE, false, [this] { order_warm_tick(); });
    });
}

std::optional<double> DualWebSocketArbitrageBot::settled_fair(const PairSettings& s, double settlement_value,
                                                              const std::optional<std::string>& result) const {
    if (!(0.0 <= settlement_value && settlement_value <= 1.0)) return std::nullopt;
    std::string r = result.value_or("");
    size_t b = 0, e = r.size();
    while (b < e && py::is_space(r[b])) ++b;
    while (e > b && py::is_space(r[e - 1])) --e;
    r = lower(r.substr(b, e - b));
    if (r == "yes" || r == "no") {
        double fair;
        if (r == "yes" && settlement_value >= 0.995) fair = 1.0;
        else if (r == "no" && settlement_value <= 0.005) fair = 0.0;
        else return std::nullopt;  // value contradicts result
        return s.reverse ? py::round_n(1.0 - fair, 6) : fair;
    }
    if (r == "scalar" && std::fabs(settlement_value - 0.5) <= 0.001) {
        if (s.tie_resolves == "no") return 0.0;
        if (s.tie_resolves == "half") return 0.5;
    }
    return std::nullopt;
}

std::optional<double> DualWebSocketArbitrageBot::settle_bet_size(Pair& pair, PairSettings& s) const {
    const mj::Value* v = pair.raw.contains("settle_bet_size") ? pair.raw.get("settle_bet_size") : bot_cfg_.get("settle_bet_size");
    if (v && !v->is_null()) return py_float_value(v, "settle_bet_size");
    return s.max_bet * s.extreme_multiplier;
}

void DualWebSocketArbitrageBot::handle_settlement(const std::string& sid, double settlement_value, double recv_ts) {
    ps::KalshiMarketState* state = source_.get_state(sid);
    std::optional<std::string> result = state ? state->result : std::nullopt;
    auto it = source_to_pairs_.find(sid);
    if (it == source_to_pairs_.end()) return;
    for (Pair* pair : it->second) {
        PairSettings& s = *pair->s;
        if (!s.settle_slam) continue;
        auto fair = settled_fair(s, settlement_value, result);
        if (!fair) continue;
        std::string cid = pair->slug.value_or("");
        PosKey answer_key{cid, (pair->aid && !pair->aid->empty()) ? *pair->aid : ""};
        if (settled_answers_.count(answer_key)) continue;
        auto mp = pair->slug ? manifold_ws_.get_cached_price(*pair->slug, pair->aid) : std::nullopt;
        auto ac = slug_to_contract_id.find(cid);
        if (!mp || ac == slug_to_contract_id.end() || ac->second.empty()) continue;
        bool yes = *fair > *mp;
        double limit;
        if (*fair >= 1.0) limit = extreme_high_;
        else if (*fair <= 0.0) limit = extreme_low_;
        else limit = *fair;
        double room = yes ? (limit - *mp) : (*mp - limit);
        if (room < min_clear_room_) continue;
        auto bet = settle_bet_size(*pair, s);
        if (!bet || !(*bet != 0) || *bet <= 0) continue;
        settled_answers_.insert(answer_key);
        const mj::Value* nm = pair->raw.get("name");
        std::string name = py_slice(nm ? py_str(nm) : sid, 50);
        last_trade_time_[pair->pair_key] = clk::time_sec();
        std::string res = result ? *result : "None";
        if (dry_run) {
            logx::out("\n🏁 SETTLED: " + name + "\n   Kalshi settled " + res + " at " + py::fixed(settlement_value, 2) +
                      " -> this market is worth " + py::fixed(*fair, 2) + " | Manifold " + py::fixed(*mp, 4) +
                      "\n   [DRY RUN] " + (yes ? "YES" : "NO") + " @ " + py::fixed(limit, 2) + " for M$" +
                      py::fixed(*bet, 0));
            continue;
        }
        submit_order(ac->second, *bet, limit, yes, pair->aid);
        log("\n🏁 SETTLED TRADE: " + name + "\n   Kalshi settled " + res + " at " + py::fixed(settlement_value, 2) +
            " -> worth " + py::fixed(*fair, 2) + " | Manifold was " + py::fixed(*mp, 4) + "\n   → #" +
            std::to_string(total_trades) + " | " + (yes ? "YES" : "NO") + " @ " + py::fixed(limit, 2) + " for M$" +
            py::fixed(*bet, 0));
    }
}

OrderHandle DualWebSocketArbitrageBot::submit_order(const std::string& cid, double amount, double limit_prob, bool yes,
                                                    const AidOpt& aid, const SlugAid* hold_key, bool join_hold) {
    OrderHandle task = manifold_orders_.place_order(cid, amount, limit_prob, yes, aid);
    total_trades += 1;
    if (hold_key && own_order_hold_ms_) hold_own_order(*hold_key, yes, limit_prob, task, join_hold);
    return task;
}

void DualWebSocketArbitrageBot::hold_own_order(const SlugAid& key, bool yes, double limit_prob, const OrderHandle& task,
                                               bool join) {
    double level = std::max(0.01, std::min(0.99, py::round_n(limit_prob, 2)));  // what place_order sends
    auto it = own_orders_.find(key);
    uint64_t seq;
    if (join && it != own_orders_.end() && it->second.yes == yes) {
        Hold& h = it->second;
        h.level = yes ? std::max(h.level, level) : std::min(h.level, level);
        h.in_flight += 1;
        seq = h.seq;
    } else {
        own_order_seq_ += 1;
        own_orders_[key] = Hold{yes, level, clk::now_ms() + own_order_hold_ms_, own_order_seq_, 1, std::nullopt, false};
        seq = own_order_seq_;
    }
    task->add_done_callback([this, key, seq, yes](OrderTask& t) {
        auto hit = own_orders_.find(key);
        if (hit == own_orders_.end() || hit->second.seq != seq) return;
        Hold& h = hit->second;
        h.in_flight -= 1;
        const mj::Value* data = (t.result && t.result->is_object()) ? &*t.result : nullptr;
        if (!data) {
            h.unknown = true;  // rejected or unknown
        } else {
            auto shares = to_float_v(data->get("shares"));
            if (!(shares && *shares == 0)) {  // to_float(shares) != 0 (None counts as filled)
                auto after = to_float_v(data->get("probAfter"));
                if (!after) h.unknown = true;  // filled, level not reported
                else if (!h.furthest) h.furthest = after;
                else h.furthest = yes ? std::max(*h.furthest, *after) : std::min(*h.furthest, *after);
            }
        }
        if (h.in_flight) return;  // a sibling order is still in flight
        if (h.furthest) h.level = *h.furthest;  // where our fills actually left it
        else if (!h.unknown) own_orders_.erase(hit);  // all filled nothing: moved nothing
    });
}

std::optional<std::pair<double, double>> DualWebSocketArbitrageBot::last_cent_order(const std::string& cid,
                                                                                    const AidOpt& aid, PairSettings& s,
                                                                                    bool yes, double mp,
                                                                                    double limit_prob,
                                                                                    double base_limit) {
    double cents = py::round_n(base_limit * 100, 6);
    double touch = (yes ? std::floor(cents) : std::ceil(cents)) / 100;
    touch = std::max(0.01, std::min(0.99, touch));
    double start, room;
    if (yes) {
        start = std::max(mp, limit_prob);
        room = touch - start;
    } else {
        start = std::min(mp, limit_prob);
        room = start - touch;
    }
    if (room < std::max(min_order_room_, 0.005)) return std::nullopt;
    std::string aid_s = (aid && !aid->empty()) ? *aid : "";
    double p = liq_config_.get_p(cid, aid_s);
    auto live = manifold_ws_.get_cached_pool(cid, aid);
    double k;
    if (live) k = amm::cpmm_invariant(live->first, live->second, p);
    else if (liq_config_.has_liquidity(cid, aid_s)) k = liq_config_.get_liquidity(cid, aid_s);
    else return std::nullopt;
    auto at_touch = amm::reconstruct_pool(touch, k, p);
    auto at_start = amm::reconstruct_pool(start, k, p);
    double cost = yes ? (at_touch.second - at_start.second) : (at_touch.first - at_start.first);
    double amount = py::round_n(std::min({cost, last_cent_max_bet_, s.max_bet}), 2);
    if (amount >= 1) return std::make_pair(touch, amount);
    return std::nullopt;
}

double DualWebSocketArbitrageBot::expected_manifold_price(const SlugAid& key, double mp) {
    auto it = own_orders_.find(key);
    if (it == own_orders_.end()) return mp;
    if (clk::now_ms() > it->second.expires_ms) {
        own_orders_.erase(it);
        return mp;
    }
    return it->second.yes ? std::max(mp, it->second.level) : std::min(mp, it->second.level);
}

void DualWebSocketArbitrageBot::release_own_order(const SlugAid& key, double prob) {
    auto it = own_orders_.find(key);
    if (it == own_orders_.end()) return;
    const Hold& h = it->second;
    bool reached = h.yes ? prob >= h.level - 1e-6 : prob <= h.level + 1e-6;
    if (reached || clk::now_ms() > h.expires_ms) own_orders_.erase(it);
}

bool DualWebSocketArbitrageBot::fade_blocked(const std::string& sid, const std::string& cid, const AidOpt& aid, bool yes,
                                             const std::string& name, const std::optional<std::string>& partner) {
    auto it = manifold_ws_.last_mover.find(SlugAid{cid, aid});
    if (it == manifold_ws_.last_mover.end()) return false;
    const auto& mover = it->second;
    if (!mover.user || !no_fade_users_.count(*mover.user)) return false;
    if ((mover.dir > 0) == yes) return false;  // same direction: following them, not fading
    int64_t age = clk::now_ms() - mover.at_ms;
    int64_t changed = 0;
    if (auto q = quote_changed_ms_.find(sid); q != quote_changed_ms_.end()) changed = q->second;
    if (age > no_fade_window_ms_ || changed > mover.at_ms) return false;
    if (partner) {
        auto q2 = quote_changed_ms_.find(*partner);
        if ((q2 == quote_changed_ms_.end() ? 0 : q2->second) > mover.at_ms) return false;
    }
    fades_blocked += 1;
    log("  🛡 " + name + ": skip " + (yes ? "YES" : "NO") + " — would fade " + py_slice(*mover.user, 8) + " (" +
        std::to_string(age) + "ms ago) on a quote older than their bet [blocked #" + std::to_string(fades_blocked) + "]");
    return true;
}

void DualWebSocketArbitrageBot::handle_price_update(Src source, const std::string& identifier, const AidOpt& aid,
                                                    double prob, double recv_ts) {
    if (recv_ts == 0) recv_ts = clk::time_sec();  // `recv_ts or time.time()`
    if (source == Src::KalshiSettled) {
        handle_settlement(identifier, prob, recv_ts);
        return;
    }
    if (source == Src::Manifold) {
        if (!own_orders_.empty()) release_own_order(SlugAid{identifier, aid}, prob);
        auto it = contract_to_pairs_.find(SlugAid{identifier, aid});
        if (it == contract_to_pairs_.end()) return;
        for (Pair* pair : it->second) {
            if (!pair->sid) continue;
            auto sp = source_.get_cached_price(*pair->sid);
            if (sp) check_arb(*pair, *sp, prob, recv_ts);
        }
        return;
    }
    if (!no_fade_users_.empty()) {
        // When the quote last actually changed — the no-fade exemption.
        auto ba = source_.get_cached_bid_ask(identifier);
        if (ba) {
            auto q = quote_seen_.find(identifier);
            if (q == quote_seen_.end() || !(q->second.first == ba->first && q->second.second == ba->second)) {
                quote_seen_[identifier] = *ba;
                quote_changed_ms_[identifier] = (int64_t)(recv_ts * 1000);
            }
        }
    }
    if (auto lc = leg_couples_.find(identifier); lc != leg_couples_.end() && !lc->second.empty())
        note_leg(identifier, lc->second, (int64_t)(recv_ts * 1000));
    if (auto it = source_to_pairs_.find(identifier); it != source_to_pairs_.end()) {
        for (Pair* pair : it->second) {
            if (!pair->slug) continue;
            auto mp = manifold_ws_.get_cached_price(*pair->slug, pair->aid);
            if (mp) check_arb(*pair, prob, *mp, recv_ts);
        }
    }
    // The other team's leg moved: same check, same pair.
    if (auto it = partner_to_pairs_.find(identifier); it != partner_to_pairs_.end()) {
        for (Pair* pair : it->second) {
            if (!pair->slug) continue;
            auto mp = manifold_ws_.get_cached_price(*pair->slug, pair->aid);
            if (mp) check_arb(*pair, prob, *mp, recv_ts);
        }
    }
}

void DualWebSocketArbitrageBot::note_leg(const std::string& tk, const std::vector<Couple>& couples, int64_t now) {
    // A paired leg's frame: rank when its usable quote last CHANGED (a
    // ticker-only frame under prefer_book leaves it alone) - by arrival order,
    // not clock, so two legs in one millisecond still have a last - and keep
    // each couple's crossed-since clock. A cross that re-forms within
    // PARTNER_HYSTERESIS_MS of clearing keeps its old start, so a flickering
    // book cannot keep restarting the trust window.
    auto ba = source_.get_cached_bid_ask(tk);
    auto seen = leg_seen_.find(tk);
    if (ba == (seen == leg_seen_.end() ? std::nullopt : seen->second)) return;
    leg_seen_[tk] = ba;
    leg_order_[tk] = ++leg_tick_;
    auto lookup = [this](const std::string& t) -> std::optional<ps::BidAsk> {
        auto f = leg_seen_.find(t);
        return f == leg_seen_.end() ? std::nullopt : f->second;
    };
    for (const Couple& key : couples) {
        auto a = lookup(key.first), b = lookup(key.second);
        if (a && b && (a->first + b->first > 1.0 + 1e-9 || a->second + b->second < 1.0 - 1e-9)) {
            std::optional<int64_t> cleared;
            if (auto u = legs_uncrossed_at_.find(key); u != legs_uncrossed_at_.end()) {
                cleared = u->second;
                legs_uncrossed_at_.erase(u);
            }
            if (!legs_crossed_since_.count(key) || (cleared && now - *cleared > PARTNER_HYSTERESIS_MS))
                legs_crossed_since_[key] = now;
        } else if (legs_crossed_since_.count(key) && !legs_uncrossed_at_.count(key)) {
            legs_uncrossed_at_[key] = now;
        }
    }
}

bool DualWebSocketArbitrageBot::pair_live(const Pair& pair, const std::string& sid) const {
    // Has this pair's game started? The scheduled start when known
    // (sportsStartTimestamp / game_start), else the book's own ACTIVE flag.
    if (pair.game_start_ms && *pair.game_start_ms != 0) return clk::now_ms() >= *pair.game_start_ms;
    ps::KalshiMarketState* st = source_.get_state(sid);
    return st && st->activity.is_active();
}

DualWebSocketArbitrageBot::LegQuote DualWebSocketArbitrageBot::two_leg_quote(Pair& pair, PairSettings& s,
                                                                             const std::string& sid,
                                                                             const std::string& partner,
                                                                             const ps::BidAsk& own) {
    // (bid, ask, tag, no_adding) for this pair's question from BOTH legs of a
    // two-way game, in the source ticker's terms (reverse is applied after).
    // Legs agree: the tighter of the two on each side. Legs cross: in a live
    // game, for partner_trust_ms after the cross formed, the leg that changed
    // last (never more than partner_max_lead past our own leg's quote);
    // otherwise trade only where NEITHER leg objects. Partner unusable: our leg
    // alone, and no new inventory before the game starts.
    auto other = source_.get_cached_bid_ask(partner);
    if (!other) {
        ++partner_quotes_[PQ_MINUS_B];
        return {own.first, own.second, "-B", !pair_live(pair, sid)};
    }
    double b_bid = py::round_n(1.0 - other->second, 6), b_ask = py::round_n(1.0 - other->first, 6);
    double bid = own.first >= b_bid ? own.first : b_bid;
    double ask = own.second <= b_ask ? own.second : b_ask;
    if (bid <= ask + 1e-9) {
        if (bid == own.first && ask == own.second) return {bid, ask, "", false};
        ++partner_quotes_[PQ_PLUS_B];
        return {bid, ask, "+B", false};
    }
    if (partner_trust_ok_ && pair_live(pair, sid)) {
        auto since = legs_crossed_since_.find(Couple{sid, partner});
        if (since != legs_crossed_since_.end() && clk::now_ms() - since->second < s.partner_trust_ms) {
            auto po = leg_order_.find(partner), so = leg_order_.find(sid);
            if ((po == leg_order_.end() ? 0 : po->second) > (so == leg_order_.end() ? 0 : so->second)) {
                ++partner_quotes_[PQ_GT_B];
                // Slide the partner's quote (its own width kept, so the spread
                // filter judges it fairly) until it sits at most `lead` past ours.
                double lead = s.partner_max_lead;
                double shift = std::max(0.0, b_bid - (own.second + lead)) - std::max(0.0, (own.first - lead) - b_ask);
                return {py::round_n(b_bid - shift, 6), py::round_n(b_ask - shift, 6), ">B", false};
            }
            ++partner_quotes_[PQ_GT_A];
            return {own.first, own.second, ">A", false};
        }
    }
    ++partner_quotes_[PQ_EQ_AB];
    return {ask, bid, "=AB", false};
}

void DualWebSocketArbitrageBot::check_arb(Pair& pair, double source_prob, double manifold_prob, double recv_ts) {
    if (!pair.sid || !pair.slug) return;
    const std::string& sid = *pair.sid;
    const std::string& cid = *pair.slug;
    const AidOpt& aid = pair.aid;
    PairSettings& s = *pair.s;
    if (recv_ts == 0) recv_ts = clk::time_sec();

    // Cooldown check (fast path)
    double now = clk::time_sec();
    if (auto lt = last_trade_time_.find(pair.pair_key); lt != last_trade_time_.end() && lt->second != 0 &&
                                                         (now - lt->second) < s.min_cooldown)
        return;

    auto bid_ask = source_.get_cached_bid_ask(sid);
    if (!bid_ask) return;
    // Two-leg pair: the quote merges both legs; the game-over (extreme) path
    // below still reads our own leg alone.
    double q_bid = bid_ask->first, q_ask = bid_ask->second;
    const char* via = "";
    bool no_adding = false;
    if (pair.partner) {
        LegQuote lq = two_leg_quote(pair, s, sid, *pair.partner, *bid_ask);
        q_bid = lq.bid;
        q_ask = lq.ask;
        via = lq.via;
        no_adding = lq.no_adding;
    }
    // Reverse markets: the source asks the opposite question, so flip the book.
    double src_bid, src_ask, own_bid, own_ask;
    if (s.reverse) {
        src_bid = py::round_n(1.0 - q_ask, 6);
        src_ask = py::round_n(1.0 - q_bid, 6);
        own_bid = py::round_n(1.0 - bid_ask->second, 6);
        own_ask = py::round_n(1.0 - bid_ask->first, 6);
    } else {
        src_bid = q_bid;
        src_ask = q_ask;
        own_bid = bid_ask->first;
        own_ask = bid_ask->second;
    }
    SlugAid hold_key{cid, aid};
    if (!own_orders_.empty()) manifold_prob = expected_manifold_price(hold_key, manifold_prob);

    bool is_extreme = own_bid >= extreme_high_ || own_ask <= extreme_low_;
    if (!is_extreme) {
        double quick_edge = std::max(src_bid - manifold_prob, manifold_prob - src_ask);
        if (quick_edge + s.trigger_eps < s.min_trigger_edge) return;
    }

    auto mpo = manifold_ws_.get_cached_price(cid, aid);
    if (!mpo) return;
    double mp = *mpo;
    if (!own_orders_.empty()) mp = expected_manifold_price(hold_key, mp);
    int spread = (int)py::round0((src_ask - src_bid) * 100);
    if (spread > s.max_spread_cents) {
        if (debug_enabled()) {
            const mj::Value* nm = pair.raw.get("name");
            log("  ⚠ " + py_slice(nm ? py_str(nm) : sid, 30) + ": spread " + std::to_string(spread) + "c > " +
                std::to_string(s.max_spread_cents) + "c");
        }
        return;
    }
    ps::KalshiMarketState* state = source_.get_state(sid);
    auto ac = slug_to_contract_id.find(cid);
    if (ac == slug_to_contract_id.end() || ac->second.empty()) return;
    const std::string& actual_cid = ac->second;
    int64_t position = manifold_orders_.get_cached_position(actual_cid, aid);

    std::string name = py_slice(pair.name_raw, 50);
    if (s.reverse) name += " (REV)";

    // Extreme path: source is decided. Slam to the band edge unless converged.
    std::optional<bool> extreme_yes;
    double extreme_limit = 0;
    if (own_bid >= extreme_high_ && mp < extreme_converge_high_) {
        extreme_yes = true;
        extreme_limit = extreme_high_;
    } else if (own_ask <= extreme_low_ && mp > extreme_converge_low_) {
        extreme_yes = false;
        extreme_limit = extreme_low_;
    }
    if (extreme_yes) {
        double bet_amount = s.max_bet * s.extreme_multiplier;
        last_trade_time_[pair.pair_key] = now;
        const char* oc = *extreme_yes ? "YES" : "NO";
        if (dry_run) {
            logx::out("\n🚨 EXTREME: " + name + "\n   Source: bid=" + py::fixed(own_bid, 4) + " ask=" + py::fixed(own_ask, 4) +
                      " | Manifold: " + py::fixed(mp, 4) + "\n   [DRY RUN] " + oc + " @ " + py::fixed(extreme_limit, 2) +
                      " for M$" + py::fixed(bet_amount, 0));
            return;
        }
        submit_order(actual_cid, bet_amount, extreme_limit, *extreme_yes, aid, &hold_key);
        log("\n🚨 EXTREME TRADE: " + name + "\n   Source: bid=" + py::fixed(own_bid, 4) + " ask=" + py::fixed(own_ask, 4) +
            " | Manifold: " + py::fixed(mp, 4) + "\n   → TRADE #" + std::to_string(total_trades) + " | " + oc + " @ " +
            py::fixed(extreme_limit, 2) + " M$" + py::fixed(bet_amount, 0) + " (pending...)");
        return;
    }

    // Normal path: directional confidence requires bid AND ask on the same side of mp.
    bool yes;
    double edge, base_limit;
    if (src_bid > mp) {
        yes = true;
        edge = src_bid - mp;
        base_limit = src_bid;
    } else if (src_ask < mp) {
        yes = false;
        edge = mp - src_ask;
        base_limit = src_ask;
    } else if (position != 0 && s.clearing_edge <= 0) {
        // Spread straddles Manifold; a non-positive clearing threshold still sheds inventory.
        if (position < 0) {
            yes = true;
            edge = src_bid - mp;
            base_limit = src_bid;
        } else {
            yes = false;
            edge = mp - src_ask;
            base_limit = src_ask;
        }
    } else {
        return;
    }

    if (base_limit >= extreme_high_ || base_limit <= extreme_low_) return;

    bool is_adding = is_adding_to_position(position, yes);
    if (is_adding && (no_adding || (double)std::llabs(position) >= s.max_position)) return;

    double required_edge = s.required_edge(position, is_adding);
    if (s.underdog_min_edge > required_edge && s.underdog_threshold > 0) {
        double fair = (src_bid + src_ask) / 2.0;
        if ((yes ? fair : 1.0 - fair) < s.underdog_threshold) required_edge = s.underdog_min_edge;
    }
    if (edge + (is_adding ? s.adding_edge_tolerance : CLEAR_EDGE_EPS) < required_edge) {
        if (debug_enabled() && edge >= s.min_edge)
            log("  ⚠ Edge " + py::pct(edge, 2) + " < required " + py::pct(required_edge, 2) + " (" +
                (is_adding ? "adding" : "clearing") + ", pos: " +
                py::fixed((double)std::llabs(position) / s.max_position * 100, 0) + "%)");
        return;
    }

    if (!no_fade_users_.empty() && fade_blocked(sid, cid, aid, yes, name, pair.partner)) return;

    double price_adj, limit_prob;
    bool offset_only;
    if (is_adding) {
        // limit_offset is a FLOOR, not a replacement for the inventory adjustment.
        price_adj = s.limit_price_adjustment(position, is_adding, edge);
        offset_only = price_adj <= s.limit_offset;
        if (price_adj < s.limit_offset) price_adj = s.limit_offset;
        limit_prob = yes ? base_limit - price_adj : base_limit + price_adj;
        // Round AWAY from the touch so the offset is a guarantee.
        double cents = py::round_n(limit_prob * 100, 6);
        limit_prob = (yes ? std::floor(cents) : std::ceil(cents)) / 100;
        limit_prob = std::max(0.01, std::min(0.99, limit_prob));
        double room = yes ? (limit_prob - mp) : (mp - limit_prob);
        if (room < min_order_room_) {
            if (debug_enabled())
                log("  ⚠ " + py_slice(name, 30) + ": order room " + py::signed_fixed(room, 4) + " < " +
                    py::repr(min_order_room_) + " — skip (no-op order)");
            return;
        }
    } else {
        // Clearing limit: anchored AT the fair boundary; only a NEGATIVE
        // threshold shifts it PAST fair.
        price_adj = 0.0;
        offset_only = false;
        double ext = std::min(s.clearing_edge, 0.0);
        limit_prob = yes ? base_limit - ext : base_limit + ext;
        limit_prob = std::max(0.01, std::min(0.99, limit_prob));
        double rounded_limit = py::round_n(limit_prob, 2);
        double room = yes ? (rounded_limit - mp) : (mp - rounded_limit);
        if (room < s.min_clear_room) {
            if (debug_enabled())
                log("  ⚠ " + py_slice(name, 30) + ": clear room " + py::signed_fixed(room, 4) + " < " +
                    py::repr(s.min_clear_room) + " — skip (no-op order)");
            return;
        }
    }

    auto bet_amount = bet_size_for_trade(pair, s, position, is_adding, edge, mp, base_limit, limit_prob);
    if (!bet_amount || *bet_amount <= 0) return;
    if (!is_adding && s.cap_clear_at_position) {
        bet_amount = std::min(*bet_amount, (double)std::llabs(position));
        if (*bet_amount < 1) return;  // Manifold's minimum bet
    }

    double processing_ms = (clk::time_sec() - recv_ts) * 1000;
    last_trade_time_[pair.pair_key] = now;
    double pos_pct = (double)std::llabs(position) / s.max_position * 100;
    const char* direction = is_adding ? "ADDING ↑" : "CLEARING ↓";
    std::string src_tag = state ? state->quote_source() : "?";
    src_tag += via;
    const char* oc = yes ? "YES" : "NO";

    if (dry_run) {
        std::string adj_str = price_adj > 0 ? "\n   Limit adj: " + py::signed_fixed(price_adj, 4) + " → " +
                                                  py::fixed(limit_prob, 4) + " (base " + py::fixed(base_limit, 4) + ")"
                                            : "";
        auto tail = (offset_only && last_cent_max_bet_ > 0)
                        ? last_cent_order(cid, aid, s, yes, mp, limit_prob, base_limit)
                        : std::nullopt;
        std::string tail_str = tail ? " + last cent @ " + py::fixed(tail->first, 2) + " M$" + py::g(tail->second) : "";
        logx::out("\n💰 OPPORTUNITY: " + name + " [proc: " + py::fixed(processing_ms, 1) + "ms | src: " + src_tag +
                  "]\n   Source: bid=" + py::fixed(src_bid, 4) + " ask=" + py::fixed(src_ask, 4) +
                  " | Manifold: " + py::fixed(mp, 4) + " | Edge: " + py::signed_fixed(edge, 4) + " (" + oc +
                  ") | Pos: " + py::signed_fixed((double)position, 0) + " (" + py::fixed(pos_pct, 0) + "%)\n   " +
                  direction + " | Required: " + py::pct(required_edge, 2) + " | Actual: " + py::pct(edge, 2) + " ✓" +
                  adj_str + "\n   [DRY RUN] " + oc + " @ " + py::fixed(limit_prob, 4) + " for M$" +
                  py::fixed(*bet_amount, 0) + tail_str);
        return;
    }

    submit_order(actual_cid, *bet_amount, limit_prob, yes, aid, &hold_key);
    // The last-cent order follows the main one (same uid: Manifold runs it right behind).
    auto tail = (offset_only && last_cent_max_bet_ > 0) ? last_cent_order(cid, aid, s, yes, mp, limit_prob, base_limit)
                                                        : std::nullopt;
    if (tail) submit_order(actual_cid, tail->second, tail->first, yes, aid, &hold_key, true);
    log("\n💰 TRADE: " + name + " [decision: " + py::fixed(processing_ms, 1) + "ms | src: " + src_tag +
        "]\n   Source: bid=" + py::fixed(src_bid, 4) + " ask=" + py::fixed(src_ask, 4) + " | Manifold: " +
        py::fixed(mp, 4) + " | Edge: " + py::signed_fixed(edge, 4) + " (" + oc + ") | Pos: " +
        py::signed_fixed((double)position, 0) + " (" + py::fixed(pos_pct, 0) + "%)\n   " + direction +
        " | Required: " + py::pct(required_edge, 2) + " | Limit adj: " + py::signed_fixed(price_adj, 4) +
        "\n   → TRADE #" + std::to_string(total_trades) + " | " + oc + " @ " + py::fixed(limit_prob, 4) + " M$" +
        py::fixed(*bet_amount, 0) + " (pending...)" +
        (tail ? " + last cent @ " + py::fixed(tail->first, 2) + " M$" + py::g(tail->second) : std::string()));
}

void DualWebSocketArbitrageBot::start_loops() {
    order_warm_tick();
    if (!skip_positions_) position_refresh_tick();
    // One source connection multiplexes many markets; chunk only as a safety valve.
    size_t chunk = source_.max_ids_per_conn();
    auto cb = [this](Src s, const std::string& id, const AidOpt& a, double p, double t) {
        handle_price_update(s, id, a, p, t);
    };
    for (const auto& ids : subscription_chunks(chunk)) source_.listen(ids, cb);
    manifold_ws_.receive_messages(cb, pairs());
}

std::vector<std::vector<std::string>> DualWebSocketArbitrageBot::subscription_chunks(size_t chunk) const {
    std::vector<std::vector<std::string>> chunks;
    if (partner_to_pairs_.empty()) {
        for (size_t i = 0; i < source_ids_.size(); i += chunk)
            chunks.emplace_back(source_ids_.begin() + (long)i,
                                source_ids_.begin() + (long)std::min(source_ids_.size(), i + chunk));
        return chunks;
    }
    std::vector<std::vector<std::string>> groups;
    std::unordered_set<std::string> seen;
    for (const std::string& sid : source_ids_) {
        // [t for t in (sid, *partners) if t not in seen] - `seen` grows after,
        // so a partner listed twice in one group stays twice (listen de-dupes).
        std::vector<std::string> group;
        if (!seen.count(sid)) group.push_back(sid);
        for (Pair* p : source_to_pairs_.at(sid))
            if (p->partner && !seen.count(*p->partner)) group.push_back(*p->partner);
        for (const auto& t : group) seen.insert(t);
        if (!group.empty()) groups.push_back(std::move(group));
    }
    for (auto& group : groups) {
        if (chunks.empty() || chunks.back().size() + group.size() > chunk) chunks.emplace_back();
        chunks.back().insert(chunks.back().end(), group.begin(), group.end());
    }
    return chunks;
}

bool DualWebSocketArbitrageBot::run() {
    ev::Loop& L = ev::Loop::get();
    const std::string bar = [] { std::string b; for (int i = 0; i < 80; ++i) b += "═"; return b; }();
    logx::out(bar);
    logx::out("🚀 " + banner_);
    {
        time_t t = time(nullptr);
        char buf[32];
        strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", localtime(&t));
        logx::out(std::string("Started: ") + buf + " | Markets: " + std::to_string(market_pairs_.size()));
    }
    logx::out(std::string("Mode: ") + (dry_run ? "DRY RUN" : "LIVE"));
    logx::out(std::string("Performance: C++20 + OpenSSL + ") + ev::Loop::backend_name() + " event loop");
    logx::out(bar + "\n");

    bool started = false, failed = false;
    cache_contract_ids([this, &started, &failed] {
        std::vector<Pair*> missing;
        for (Pair* p : amm_pairs_) {
            std::string aid = (p->aid && !p->aid->empty()) ? *p->aid : "";
            if (!liq_config_.has_liquidity(p->slug.value_or(""), aid)) missing.push_back(p);
        }
        if (!missing.empty()) {
            logx::out("  ⚠ AMM sizing: " + std::to_string(missing.size()) + " pair(s) have no pool data — using "
                      "default_liquidity=" + py::fixed(liq_config_.default_liquidity, 0) + ":");
            for (size_t i = 0; i < missing.size() && i < 10; ++i) {
                const mj::Value* nm = missing[i]->raw.get("name");
                logx::out("     · " + py_slice(nm ? py_str(nm) : missing[i]->slug.value_or("None"), 60));
            }
        }
        auto after_positions = [this, &started, &failed] {
            manifold_orders_.warm_pool(ORDER_POOL_SIZE, true, [this, &started, &failed] {
                manifold_ws_.connect([this, &started, &failed](bool ok) {
                    if (!ok) {
                        logx::out("✗ Manifold WebSocket connect failed at startup");
                        failed = true;
                        ev::Loop::get().stop();
                        return;
                    }
                    auto ps = pairs();
                    manifold_ws_.subscribe_to_markets(ps, &contract_payloads, [this, &started] {
                        logx::out("");
                        started = true;
                        start_loops();
                    });
                });
            });
        };
        if (skip_positions_) {
            logx::out("  → Inventory tracking OFF (skip_positions) — no /positions traffic");
            after_positions();
        } else {
            logx::out("  → Fetching initial positions + warming order connection pool...");
            manifold_orders_.refresh_all_positions(pairs(), slug_to_contract_id, after_positions);
        }
    });

    L.install_signal_stop();
    L.run();
    if (L.interrupted()) logx::out("\n⚠ Stopped | Total trades: " + std::to_string(total_trades));
    // Stop producers before draining order tasks and closing their session.
    L.resume();
    L.cancel(warm_timer_);
    L.cancel(refresh_timer_);
    source_.close();
    manifold_ws_.close();
    manifold_orders_.close(nullptr);
    (void)started;
    return !failed;
}

// ===========================================================================
// Config + credentials + runner helpers
// ===========================================================================

static bool file_exists(const std::string& p) { return access(p.c_str(), R_OK) == 0; }

mj::Value load_config(const char* path, const std::string& dflt) {
    std::string config_path = path ? path : "";
    if (config_path.empty() && file_exists(dflt)) config_path = dflt;
    if (!config_path.empty() && file_exists(config_path)) {
        logx::out("✓ Loaded config from: " + config_path);
        return yamlite::parse_file(config_path);
    }
    logx::out("⚠ No config file found (" + (config_path.empty() ? dflt : config_path) +
              ") — using built-in dry-run defaults");
    mj::Value cfg = mj::Value::object();
    mj::Value b = mj::Value::object();
    b.set("dry_run", mj::Value::boolean(true));
    b.set("min_edge_threshold", mj::Value::number(0.01));
    b.set("max_bet_size", mj::Value::integer(299));
    b.set("min_time_between_trades", mj::Value::number(2.0));
    b.set("max_position", mj::Value::integer(1000));
    cfg.set("bot", std::move(b));
    cfg.set("market_pairs", mj::Value::array());
    return cfg;
}

// python-dotenv load_dotenv(): first .env found walking up from the cwd;
// existing environment variables win.
void load_dotenv() {
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) return;
    std::string dir = cwd, found;
    while (true) {
        if (file_exists(dir + "/.env")) {
            found = dir + "/.env";
            break;
        }
        size_t slash = dir.find_last_of('/');
        if (slash == std::string::npos || dir == "/") break;
        dir = slash == 0 ? "/" : dir.substr(0, slash);
        if (dir == "/" && !file_exists("/.env")) break;
    }
    if (found.empty()) return;
    std::ifstream f(found);
    std::string line;
    while (std::getline(f, line)) {
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos || line[b] == '#') continue;
        line = line.substr(b);
        if (line.rfind("export ", 0) == 0) line = line.substr(7);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        std::string val = line.substr(eq + 1);
        size_t vb = val.find_first_not_of(" \t");
        val = vb == std::string::npos ? "" : val.substr(vb);
        if (!val.empty() && (val[0] == '"' || val[0] == '\'')) {
            char q = val[0];
            std::string out;
            size_t i = 1;
            for (; i < val.size() && val[i] != q; ++i) {
                if (q == '"' && val[i] == '\\' && i + 1 < val.size()) {
                    char e = val[++i];
                    out.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e);
                } else {
                    out.push_back(val[i]);
                }
            }
            val = out;
        } else {
            size_t hash = val.find(" #");
            if (hash != std::string::npos) val = val.substr(0, hash);
            while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r')) val.pop_back();
        }
        if (!key.empty() && !std::getenv(key.c_str())) setenv(key.c_str(), val.c_str(), 0);
    }
}

std::optional<std::pair<std::string, std::string>> kalshi_credentials() {
    const char* key_id = std::getenv("KALSHI_API_KEY");
    std::string pk;
    if (const char* k = std::getenv("KALSHI_PRIVATE_KEY"); k && *k) pk = k;
    if (pk.empty()) {
        if (const char* path = std::getenv("KALSHI_PRIVATE_KEY_FILE"); path && *path) {
            std::ifstream f(path);
            if (f) {
                std::stringstream ss;
                ss << f.rdbuf();
                pk = ss.str();
            }
        }
    }
    if (!key_id || !*key_id || pk.empty()) return std::nullopt;
    return std::make_pair(std::string(key_id), pk);
}

std::optional<std::pair<std::string, std::string>> manifold_credentials() {
    const char* k = std::getenv("MANIFOLD_API_KEY");
    if (!k || !*k) return std::nullopt;
    const char* u = std::getenv("MANIFOLD_USER_ID");
    return std::make_pair(std::string(k), std::string(u && *u ? u : "unknown"));
}

std::vector<std::unique_ptr<Pair>> build_pairs(const mj::Value& config, const std::string& source_key) {
    std::vector<std::unique_ptr<Pair>> out;
    const mj::Value* mp = config.get("market_pairs");
    if (!mp || !mp->is_array()) return out;
    for (const mj::Value& raw : mp->a) {
        if (!raw.is_object()) throw std::runtime_error("market_pairs entries must be mappings");
        auto p = std::make_unique<Pair>();
        p->raw = raw;
        auto sv = [](const mj::Value* v) -> std::optional<std::string> {
            if (!v || v->is_null()) return std::nullopt;
            return v->is_string() ? v->s : py_str(v);
        };
        p->sid = sv(raw.get(source_key));
        p->slug = sv(raw.get("manifold_slug"));
        p->aid = sv(raw.get("manifold_answer_id"));
        const mj::Value* nm = raw.get("name");
        p->name_raw = nm ? py_str(nm) : "Unknown";
        std::string aid_or = (p->aid && !p->aid->empty()) ? *p->aid : "";
        p->pair_key = (p->sid ? *p->sid : "None") + ":" + (p->slug ? *p->slug : "None") + ":" + aid_or;
        out.push_back(std::move(p));
    }
    return out;
}

int run_bot(ps::PriceSource& source, const mj::Value& config, const std::string& source_key, const std::string& banner) {
    auto creds = manifold_credentials();
    if (!creds) {
        logx::out("✗ Missing MANIFOLD_API_KEY!");
        return 1;
    }
    auto pairs = build_pairs(config, source_key);
    if (pairs.empty()) {
        logx::out("⚠ No market pairs!");
        return 1;
    }
    logx::out("\nLoaded " + std::to_string(pairs.size()) + " market pairs");
    logx::out(std::string(80, '='));
    const mj::Value* b = config.get("bot");
    int64_t ws_max = WS_MAX_BYTES;
    if (b && b->get("ws_max_bytes")) ws_max = py_int_value(b->get("ws_max_bytes"), "ws_max_bytes");
    ManifoldWebSocketClient ws((size_t)ws_max);
    ManifoldOrderClient orders(creds->first, creds->second, std::make_unique<HttpOrderTransport>(creds->first));
    if (const char* tf = std::getenv("ORDER_TRACE_FILE"); tf && *tf) {
        auto f = std::make_shared<std::ofstream>(tf, std::ios::app);
        orders.trace_sink = [f](const mj::Value& v) {
            *f << v.dump() << '\n';
            f->flush();
        };
    }
    DualWebSocketArbitrageBot bot(source, ws, orders, std::move(pairs), config, source_key, banner);
    return bot.run() ? 0 : 1;
}

}  // namespace bot

namespace bot {

mj::Value DualWebSocketArbitrageBot::debug_state() const {
    using V = mj::Value;
    V s = V::object();
    s.set("total_trades", V::integer(total_trades));
    s.set("fades_blocked", V::integer(fades_blocked));
    V oo = V::array();
    for (const auto& [k, h] : own_orders_) {
        V row = V::array();
        row.push(V::string(k.slug));
        row.push(k.aid ? V::string(*k.aid) : V::null());
        row.push(V::string(h.yes ? "YES" : "NO"));
        row.push(V::number(h.level));
        row.push(V::integer(h.expires_ms));
        row.push(V::integer((int64_t)h.seq));
        row.push(V::integer(h.in_flight));
        row.push(h.furthest ? V::number(*h.furthest) : V::null());
        row.push(V::boolean(h.unknown));
        oo.push(std::move(row));
    }
    s.set("own_orders", std::move(oo));
    V ltt = V::object();
    for (const auto& [k, v] : last_trade_time_) ltt.set(k, V::number(v));
    s.set("last_trade_time", std::move(ltt));
    V sa = V::array();
    for (const auto& [c, a] : settled_answers_) {
        V row = V::array();
        row.push(V::string(c));
        row.push(V::string(a));
        sa.push(std::move(row));
    }
    s.set("settled_answers", std::move(sa));
    V qc = V::object();
    for (const auto& [k, v] : quote_changed_ms_) qc.set(k, V::integer(v));
    s.set("quote_changed_ms", std::move(qc));
    // Partner legs
    V pq = V::object();
    static const char* pq_names[PQ_N] = {"+B", ">B", ">A", "=AB", "-B"};
    for (int k = 0; k < PQ_N; ++k) pq.set(pq_names[k], V::integer(partner_quotes_[k]));
    s.set("partner_quotes", std::move(pq));
    auto couples = [](const std::map<Couple, int64_t>& m) {
        V a = V::array();
        for (const auto& [k, v] : m) {
            V row = V::array();
            row.push(V::string(k.first));
            row.push(V::string(k.second));
            row.push(V::integer(v));
            a.push(std::move(row));
        }
        return a;
    };
    s.set("legs_crossed_since", couples(legs_crossed_since_));
    s.set("legs_uncrossed_at", couples(legs_uncrossed_at_));
    V lo = V::object();
    for (const auto& [k, v] : leg_order_) lo.set(k, V::integer(v));
    s.set("leg_order", std::move(lo));
    s.set("leg_tick", V::integer(leg_tick_));
    V ls = V::object();
    for (const auto& [k, v] : leg_seen_) {
        V row = V::null();
        if (v) {
            row = V::array();
            row.push(V::number(v->first));
            row.push(V::number(v->second));
        }
        ls.set(k, std::move(row));
    }
    s.set("leg_seen", std::move(ls));
    V gs = V::array(), pt = V::array();
    for (const auto& p : market_pairs_) {
        gs.push(p->game_start_ms ? V::integer(*p->game_start_ms) : V::null());
        pt.push(p->partner ? V::string(*p->partner) : V::null());
    }
    s.set("game_start_ms", std::move(gs));
    s.set("partners", std::move(pt));
    V sub = V::array();
    for (const auto& c : subscription_chunks(90)) {
        V row = V::array();
        for (const auto& t : c) row.push(V::string(t));
        sub.push(std::move(row));
    }
    s.set("subscription", std::move(sub));
    return s;
}

}  // namespace bot
