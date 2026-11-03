// pyutil.hpp — Python numeric/text semantics the engine depends on.
//
// The transcription is only faithful if the arithmetic matches Python bit for
// bit: limit prices go through round(x, 2), book prices through round(v, 4),
// quotes through round(1.0 - x, 6). Python's round() rounds the EXACT binary
// value half-to-even (it formats with dtoa and parses back), so a naive
// floor(x*100+0.5)/100 disagrees on values like 2.675. py_round() reproduces
// it exactly with an error-free FMA product. Build without -ffast-math.
#pragma once

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace py {

// int(x) for a finite double. Python's int is unbounded; casting an out-of-range
// double to int64 is undefined behaviour in C++, so this saturates instead.
inline int64_t trunc_i64(double x) {
    if (!(x == x)) return 0;
    if (x >= 9223372036854775807.0) return INT64_MAX;
    if (x <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)x;
}

// ---------------------------------------------------------------------------
// round()
// ---------------------------------------------------------------------------

// Python's round(x, n) for 0 <= n <= 22. Exact: x*10^n is split into hi + lo
// (hi = the rounded product, lo = its exact residual via FMA), rounded half to
// even as one exact value, then divided back — which IEEE division rounds
// correctly, matching Python's strtod of the rounded decimal string.
inline double round_n(double x, int n) {
    static const double P10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                 1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    if (!std::isfinite(x) || n < 0 || n > 22) return x;
    const double p = P10[n];
    const double hi = x * p;
    if (!std::isfinite(hi)) return x;
    if (std::fabs(hi) >= 4503599627370496.0) {  // 2^52: rare, do it the dtoa way
        char buf[512];
        std::snprintf(buf, sizeof buf, "%.*f", n, x);  // correctly rounded, half-even
        return std::strtod(buf, nullptr);
    }
    const double lo = std::fma(x, p, -hi);  // exact: x*p == hi + lo
    double r = std::nearbyint(hi);          // ties-to-even on hi alone
    const double f = hi - r;                // exact
    if (f == 0.5) {
        if (lo > 0) r += 1.0;               // past the tie
    } else if (f == -0.5) {
        if (lo < 0) r -= 1.0;
    }
    return r / p;
}

// Python's round(x) (no ndigits): nearest integer, ties to even.
inline double round0(double x) { return std::nearbyint(x); }

// ---------------------------------------------------------------------------
// float(str) — Python's grammar: optional whitespace, sign, digits with single
// underscores between digits, optional fraction/exponent, or inf/infinity/nan.
// Rejects hex, which strtod would otherwise accept.
// ---------------------------------------------------------------------------

inline bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f' ||
           (c >= 0x1c && c <= 0x1f);
}

inline std::optional<double> float_from_str(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    if (b == e) return std::nullopt;
    std::string t;
    t.reserve(e - b);
    size_t i = b;
    if (s[i] == '+' || s[i] == '-') t.push_back(s[i++]);
    // inf / infinity / nan, case-insensitive
    {
        std::string low;
        for (size_t k = i; k < e; ++k) low.push_back((char)std::tolower((unsigned char)s[k]));
        if (low == "inf" || low == "infinity") return (t == "-") ? -INFINITY : INFINITY;
        if (low == "nan") return (t == "-") ? -NAN : NAN;
    }
    bool digits = false, dot = false, exp = false;
    char prev = 0;
    for (; i < e; ++i) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            digits = true;
            t.push_back(c);
        } else if (c == '_') {
            // single underscore strictly between two digits
            if (!(prev >= '0' && prev <= '9') || i + 1 >= e || !(s[i + 1] >= '0' && s[i + 1] <= '9'))
                return std::nullopt;
        } else if (c == '.') {
            if (dot || exp) return std::nullopt;
            dot = true;
            t.push_back(c);
        } else if (c == 'e' || c == 'E') {
            if (exp || !digits) return std::nullopt;
            exp = true;
            t.push_back('e');
            if (i + 1 < e && (s[i + 1] == '+' || s[i + 1] == '-')) t.push_back(s[++i]);
            if (i + 1 >= e) return std::nullopt;
            digits = false;  // exponent needs its own digits
            prev = 0;
            continue;
        } else {
            return std::nullopt;
        }
        prev = c;
    }
    if (!digits) return std::nullopt;
    char* end = nullptr;
    double v = std::strtod(t.c_str(), &end);
    if (end != t.c_str() + t.size()) return std::nullopt;
    return v;
}

// int(str): optional whitespace and sign, decimal digits, single underscores.
inline std::optional<int64_t> int_from_str(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    if (b == e) return std::nullopt;
    bool neg = false;
    if (s[b] == '+' || s[b] == '-') neg = s[b++] == '-';
    if (b == e) return std::nullopt;
    __int128 v = 0;
    char prev = 0;
    for (size_t i = b; i < e; ++i) {
        char c = s[i];
        if (c == '_') {
            if (!(prev >= '0' && prev <= '9') || i + 1 >= e) return std::nullopt;
            prev = c;
            continue;
        }
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + (c - '0');
        if (v > (__int128)INT64_MAX + 1) return std::nullopt;
        prev = c;
    }
    if (prev == '_') return std::nullopt;
    v = neg ? -v : v;
    if (v > INT64_MAX || v < INT64_MIN) return std::nullopt;
    return (int64_t)v;
}

// ---------------------------------------------------------------------------
// Float formatting
// ---------------------------------------------------------------------------

// Shortest round-trip digits and decimal exponent: x = 0.DIGITS * 10^decpt.
struct ShortestDigits {
    std::string digits;  // no leading zeros, at least one digit
    int decpt = 0;       // position of the decimal point relative to digits
    bool neg = false;
};

inline ShortestDigits shortest(double x) {
    ShortestDigits r;
    r.neg = std::signbit(x);
    if (x == 0) {
        r.digits = "0";
        r.decpt = 1;
        return r;
    }
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof buf, std::fabs(x), std::chars_format::scientific);
    std::string_view sv(buf, res.ptr - buf);  // d.ddddde[+-]XX
    size_t epos = sv.find('e');
    std::string mant(sv.substr(0, epos));
    int ex = std::atoi(std::string(sv.substr(epos + 1)).c_str());
    for (char c : mant)
        if (c != '.') r.digits.push_back(c);
    r.decpt = ex + 1;
    return r;
}

inline std::string layout_fixed(const ShortestDigits& d) {
    std::string out;
    if (d.neg) out.push_back('-');
    const int n = (int)d.digits.size();
    if (d.decpt <= 0) {
        out += "0.";
        out.append((size_t)(-d.decpt), '0');
        out += d.digits;
    } else if (d.decpt >= n) {
        out += d.digits;
        out.append((size_t)(d.decpt - n), '0');
        out += ".0";
    } else {
        out += d.digits.substr(0, (size_t)d.decpt);
        out.push_back('.');
        out += d.digits.substr((size_t)d.decpt);
    }
    return out;
}

inline std::string layout_sci(const ShortestDigits& d, bool python_style) {
    std::string out;
    if (d.neg) out.push_back('-');
    out.push_back(d.digits[0]);
    if (d.digits.size() > 1) {
        out.push_back('.');
        out += d.digits.substr(1);
    }
    int ex = d.decpt - 1;
    out.push_back('e');
    if (python_style) {
        out.push_back(ex < 0 ? '-' : '+');
        char b[16];
        std::snprintf(b, sizeof b, "%02d", ex < 0 ? -ex : ex);
        out += b;
    } else {
        out += std::to_string(ex);
    }
    return out;
}

// repr(float)
inline std::string repr(double x) {
    if (std::isnan(x)) return "nan";
    if (std::isinf(x)) return x > 0 ? "inf" : "-inf";
    ShortestDigits d = shortest(x);
    if (d.decpt <= -4 || d.decpt > 16) return layout_sci(d, true);
    return layout_fixed(d);
}

// orjson.dumps(float): fixed for decimal exponent in [-5, 15], else "1e16" style.
inline std::string orjson_float(double x) {
    if (!std::isfinite(x)) return "null";  // orjson writes null for nan/inf
    ShortestDigits d = shortest(x);
    int k = d.decpt - 1;
    if (k < -5 || k >= 16) return layout_sci(d, false);
    return layout_fixed(d);
}

// '{:.Nf}'
inline std::string fixed(double x, int prec) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", prec, x);
    return b;
}
// '{:+.Nf}'
inline std::string signed_fixed(double x, int prec) {
    char b[64];
    std::snprintf(b, sizeof b, "%+.*f", prec, x);
    return b;
}
// '{:.N%}'
inline std::string pct(double x, int prec) { return fixed(x * 100.0, prec) + "%"; }
// '{:+.N%}'
inline std::string signed_pct(double x, int prec) { return signed_fixed(x * 100.0, prec) + "%"; }
// '{:g}'
inline std::string g(double x) {
    char b[64];
    std::snprintf(b, sizeof b, "%g", x);
    return b;
}
// str(int)
inline std::string str(int64_t v) { return std::to_string(v); }

}  // namespace py
