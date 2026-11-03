#include "yaml.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <vector>

#include "pyutil.hpp"

namespace yamlite {
namespace {

struct Line {
    int indent;
    std::string text;  // comment-stripped, trimmed, non-empty
    int no;            // 1-based source line
};

std::string rtrim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

// '#' starts a comment at line start or after whitespace, outside quotes. A
// quote opens a quoted scalar only where a scalar can start.
std::string strip_comment(const std::string& s) {
    bool in_s = false, in_d = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (in_d) {
            if (c == '\\') { ++i; continue; }
            if (c == '"') in_d = false;
            continue;
        }
        if (in_s) {
            if (c == '\'') {
                if (i + 1 < s.size() && s[i + 1] == '\'') { ++i; continue; }
                in_s = false;
            }
            continue;
        }
        if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) return s.substr(0, i);
        if (c == '"' || c == '\'') {
            size_t j = i;
            while (j > 0 && (s[j - 1] == ' ' || s[j - 1] == '\t')) --j;
            char prev = j == 0 ? 0 : s[j - 1];
            bool spaced = j < i;
            if (j == 0 || ((prev == ':' || prev == '-') && spaced) || prev == '[' || prev == '{' ||
                prev == ',') {
                if (c == '"') in_d = true; else in_s = true;
            }
        }
    }
    return s;
}

bool is_seq_item(const std::string& t) {
    return t == "-" || (t.size() >= 2 && t[0] == '-' && (t[1] == ' ' || t[1] == '\t'));
}

// End index (exclusive) of a quoted scalar starting at p, or npos.
size_t quoted_end(const std::string& s, size_t p) {
    char q = s[p];
    for (size_t i = p + 1; i < s.size(); ++i) {
        if (q == '"' && s[i] == '\\') { ++i; continue; }
        if (s[i] == q) {
            if (q == '\'' && i + 1 < s.size() && s[i + 1] == '\'') { ++i; continue; }
            return i + 1;
        }
    }
    return std::string::npos;
}

// Position of the ':' separating a block mapping key from its value, or npos.
size_t find_map_colon(const std::string& t) {
    if (t.empty()) return std::string::npos;
    size_t i = 0;
    if (t[0] == '"' || t[0] == '\'') {
        size_t e = quoted_end(t, 0);
        if (e == std::string::npos) return std::string::npos;
        i = e;
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
        if (i < t.size() && t[i] == ':' && (i + 1 == t.size() || t[i + 1] == ' ' || t[i + 1] == '\t'))
            return i;
        return std::string::npos;
    }
    if (t[0] == '[' || t[0] == '{') return std::string::npos;
    for (; i < t.size(); ++i) {
        if (t[i] == ':' && (i + 1 == t.size() || t[i + 1] == ' ' || t[i + 1] == '\t')) return i;
    }
    return std::string::npos;
}

const std::regex& re_bool() {
    static const std::regex r("^(?:yes|Yes|YES|no|No|NO|true|True|TRUE|false|False|FALSE|on|On|ON|off|Off|OFF)$");
    return r;
}
const std::regex& re_float() {
    static const std::regex r(
        "^(?:[-+]?(?:[0-9][0-9_]*)\\.[0-9_]*(?:[eE][-+][0-9]+)?"
        "|\\.[0-9][0-9_]*(?:[eE][-+][0-9]+)?"
        "|[-+]?[0-9][0-9_]*(?::[0-5]?[0-9])+\\.[0-9_]*"
        "|[-+]?\\.(?:inf|Inf|INF)"
        "|\\.(?:nan|NaN|NAN))$");
    return r;
}
const std::regex& re_int() {
    static const std::regex r(
        "^(?:[-+]?0b[0-1_]+"
        "|[-+]?0[0-7_]+"
        "|[-+]?(?:0|[1-9][0-9_]*)"
        "|[-+]?0x[0-9a-fA-F_]+"
        "|[-+]?[1-9][0-9_]*(?::[0-5]?[0-9])+)$");
    return r;
}
const std::regex& re_null() {
    static const std::regex r("^(?:~|null|Null|NULL|)$");
    return r;
}
const std::regex& re_timestamp() {
    static const std::regex r(
        "^(?:[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]"
        "|[0-9][0-9][0-9][0-9]-[0-9][0-9]?-[0-9][0-9]?"
        "(?:[Tt]|[ \\t]+)[0-9][0-9]?"
        ":[0-9][0-9]:[0-9][0-9](?:\\.[0-9]*)?"
        "(?:[ \\t]*(?:Z|[-+][0-9][0-9]?(?::[0-9][0-9])?))?)$");
    return r;
}

std::string remove_underscores(const std::string& s) {
    std::string out;
    for (char c : s)
        if (c != '_') out.push_back(c);
    return out;
}

// PyYAML constructs a plain timestamp as datetime.date / datetime.datetime
// (construct_yaml_timestamp). The port keeps the str() of that value: the text
// Python prints for it, which datetime.fromisoformat (and so bot_core's
// game_start parsing) reads back as the same instant. Out-of-range fields raise
// like datetime's constructor, so a config PyYAML refuses is refused here too.
mj::Value construct_timestamp(const std::string& s) {
    static const std::regex r(
        "^([0-9][0-9][0-9][0-9])-([0-9][0-9]?)-([0-9][0-9]?)"
        "(?:(?:[Tt]|[ \\t]+)([0-9][0-9]?):([0-9][0-9]):([0-9][0-9])(?:\\.([0-9]*))?"
        "(?:[ \\t]*(Z|([-+])([0-9][0-9]?)(?::([0-9][0-9]))?))?)?$");
    std::smatch m;
    if (!std::regex_match(s, m, r)) return mj::Value::string(s);
    int year = std::stoi(m[1].str()), month = std::stoi(m[2].str()), day = std::stoi(m[3].str());
    if (year < 1 || year > 9999) throw YamlError("year " + std::to_string(year) + " is out of range");
    if (month < 1 || month > 12) throw YamlError("month must be in 1..12");
    static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (day < 1 || day > dim[month - 1] + (month == 2 && leap)) throw YamlError("day is out of range for month");
    char buf[80];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", year, month, day);
    std::string out = buf;
    if (!m[4].matched) return mj::Value::string(out);  // datetime.date
    int hour = std::stoi(m[4].str()), minute = std::stoi(m[5].str()), second = std::stoi(m[6].str());
    if (hour > 23) throw YamlError("hour must be in 0..23");
    if (minute > 59) throw YamlError("minute must be in 0..59");
    if (second > 59) throw YamlError("second must be in 0..59");
    int us = 0;
    if (m[7].matched && m[7].length() > 0) {  // `if values['fraction']:` ('' is falsy)
        std::string f = m[7].str().substr(0, 6);
        while (f.size() < 6) f += '0';
        us = std::stoi(f);
    }
    std::snprintf(buf, sizeof buf, " %02d:%02d:%02d", hour, minute, second);
    out += buf;
    if (us) {
        std::snprintf(buf, sizeof buf, ".%06d", us);
        out += buf;
    }
    if (m[9].matched) {  // tz_sign: datetime.timezone(+/-timedelta(hours, minutes))
        int tz_minutes = std::stoi(m[10].str()) * 60 + (m[11].matched ? std::stoi(m[11].str()) : 0);
        if (tz_minutes >= 24 * 60)
            throw YamlError("offset must be a timedelta strictly between -timedelta(hours=24) and timedelta(hours=24)");
        bool neg = m[9].str() == "-" && tz_minutes != 0;
        std::snprintf(buf, sizeof buf, "%c%02d:%02d", neg ? '-' : '+', tz_minutes / 60, tz_minutes % 60);
        out += buf;
    } else if (m[8].matched) {  // Z: timezone.utc
        out += "+00:00";
    }
    return mj::Value::string(out);
}

mj::Value construct_int(const std::string& raw) {
    std::string v = remove_underscores(raw);
    int sign = 1;
    if (v[0] == '-') sign = -1;
    if (v[0] == '+' || v[0] == '-') v = v.substr(1);
    __int128 acc = 0;
    auto big = [&](void) { return acc > (__int128)INT64_MAX; };
    if (v == "0") return mj::Value::integer(0);
    if (v.rfind("0b", 0) == 0) {
        for (size_t k = 2; k < v.size(); ++k) { acc = acc * 2 + (v[k] - '0'); if (big()) break; }
    } else if (v.rfind("0x", 0) == 0) {
        for (size_t k = 2; k < v.size(); ++k) {
            char c = v[k];
            int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : c - 'A' + 10;
            acc = acc * 16 + d;
            if (big()) break;
        }
    } else if (v[0] == '0') {
        for (size_t k = 0; k < v.size(); ++k) { acc = acc * 8 + (v[k] - '0'); if (big()) break; }
    } else if (v.find(':') != std::string::npos) {
        std::vector<int64_t> parts;
        std::stringstream ss(v);
        std::string part;
        while (std::getline(ss, part, ':')) parts.push_back(std::stoll(part));
        __int128 base = 1;
        for (size_t k = parts.size(); k-- > 0;) { acc += parts[k] * base; base *= 60; }
    } else {
        for (char c : v) { acc = acc * 10 + (c - '0'); if (big()) break; }
    }
    if (big()) return mj::Value::number((double)sign * (double)acc);  // python: big int
    return mj::Value::integer((int64_t)(sign * acc));
}

mj::Value construct_float(const std::string& raw) {
    std::string v;
    for (char c : remove_underscores(raw)) v.push_back((char)std::tolower((unsigned char)c));
    double sign = 1;
    if (v[0] == '-') sign = -1;
    if (v[0] == '+' || v[0] == '-') v = v.substr(1);
    if (v == ".inf") return mj::Value::number(sign * INFINITY);
    if (v == ".nan") return mj::Value::number(NAN);
    if (v.find(':') != std::string::npos) {
        std::vector<double> parts;
        std::stringstream ss(v);
        std::string part;
        while (std::getline(ss, part, ':')) parts.push_back(*py::float_from_str(part));
        double base = 1, acc = 0;
        for (size_t k = parts.size(); k-- > 0;) { acc += parts[k] * base; base *= 60; }
        return mj::Value::number(sign * acc);
    }
    auto f = py::float_from_str(v);
    if (!f) throw YamlError("bad float: " + raw);
    return mj::Value::number(sign * *f);
}

std::string py_key_str(const mj::Value& v) {
    switch (v.type) {
        case mj::Type::Null: return "None";
        case mj::Type::Bool: return v.b ? "True" : "False";
        case mj::Type::Int: return std::to_string(v.i);
        case mj::Type::Float: return py::repr(v.d);
        case mj::Type::String: return v.s;
        default: return v.dump();
    }
}

void put_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out.push_back((char)cp);
    else if (cp < 0x800) { out.push_back((char)(0xC0 | (cp >> 6))); out.push_back((char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

// Decode a quoted scalar s[p..end) (including the quotes).
std::string unquote(const std::string& s, size_t p, size_t end) {
    char q = s[p];
    std::string out;
    for (size_t i = p + 1; i + 1 < end; ++i) {
        char c = s[i];
        if (q == '\'') {
            if (c == '\'' && i + 1 < end - 1 && s[i + 1] == '\'') { out.push_back('\''); ++i; continue; }
            out.push_back(c);
            continue;
        }
        if (c != '\\') { out.push_back(c); continue; }
        char e = s[++i];
        auto hexn = [&](int n) {
            uint32_t v = 0;
            for (int k = 0; k < n; ++k) {
                char h = s[++i];
                v = v * 16 + (uint32_t)((h >= '0' && h <= '9') ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10 : h - 'A' + 10);
            }
            return v;
        };
        switch (e) {
            case '0': out.push_back('\0'); break;
            case 'a': out.push_back('\a'); break;
            case 'b': out.push_back('\b'); break;
            case 't': case '\t': out.push_back('\t'); break;
            case 'n': out.push_back('\n'); break;
            case 'v': out.push_back('\v'); break;
            case 'f': out.push_back('\f'); break;
            case 'r': out.push_back('\r'); break;
            case 'e': out.push_back('\x1b'); break;
            case ' ': out.push_back(' '); break;
            case '"': out.push_back('"'); break;
            case '/': out.push_back('/'); break;
            case '\\': out.push_back('\\'); break;
            case 'N': put_utf8(out, 0x85); break;
            case '_': put_utf8(out, 0xA0); break;
            case 'L': put_utf8(out, 0x2028); break;
            case 'P': put_utf8(out, 0x2029); break;
            case 'x': put_utf8(out, hexn(2)); break;
            case 'u': put_utf8(out, hexn(4)); break;
            case 'U': put_utf8(out, hexn(8)); break;
            default: throw YamlError(std::string("bad escape \\") + e);
        }
    }
    return out;
}

void check_supported(const std::string& t, int no) {
    if (t.empty()) return;
    char c = t[0];
    if (c == '&' || c == '*' || c == '!' || c == '|' || c == '>' || c == '%' || c == '@' || c == '`')
        throw YamlError("unsupported YAML construct on line " + std::to_string(no) + ": " + t);
}

// ---------------------------------------------------------------------------
// Flow collections
// ---------------------------------------------------------------------------

struct Flow {
    const std::string& s;
    size_t p = 0;
    int no;

    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p; }

    std::string plain(bool in_map_key) {
        size_t b = p;
        while (p < s.size()) {
            char c = s[p];
            if (c == ',' || c == ']' || c == '}' || c == '[' || c == '{') break;
            if (c == ':' && (p + 1 >= s.size() || s[p + 1] == ' ' || s[p + 1] == ',' || s[p + 1] == ']' ||
                             s[p + 1] == '}' || in_map_key))
                break;
            ++p;
        }
        return trim(s.substr(b, p - b));
    }

    mj::Value value(bool in_map_key = false) {
        ws();
        if (p >= s.size()) throw YamlError("unterminated flow collection on line " + std::to_string(no));
        char c = s[p];
        if (c == '[') {
            ++p;
            mj::Value arr = mj::Value::array();
            while (true) {
                ws();
                if (p < s.size() && s[p] == ']') { ++p; break; }
                arr.push(value());
                ws();
                if (p < s.size() && s[p] == ',') { ++p; continue; }
                if (p < s.size() && s[p] == ']') { ++p; break; }
                throw YamlError("bad flow sequence on line " + std::to_string(no));
            }
            return arr;
        }
        if (c == '{') {
            ++p;
            mj::Value obj = mj::Value::object();
            while (true) {
                ws();
                if (p < s.size() && s[p] == '}') { ++p; break; }
                mj::Value k = value(true);
                ws();
                mj::Value v;
                if (p < s.size() && s[p] == ':') {
                    ++p;
                    ws();
                    if (p < s.size() && (s[p] == ',' || s[p] == '}')) v = mj::Value::null();
                    else v = value();
                }
                obj.set(py_key_str(k), std::move(v));
                ws();
                if (p < s.size() && s[p] == ',') { ++p; continue; }
                if (p < s.size() && s[p] == '}') { ++p; break; }
                throw YamlError("bad flow mapping on line " + std::to_string(no));
            }
            return obj;
        }
        if (c == '"' || c == '\'') {
            size_t e = quoted_end(s, p);
            if (e == std::string::npos) throw YamlError("unterminated quote on line " + std::to_string(no));
            std::string out = unquote(s, p, e);
            p = e;
            return mj::Value::string(out);
        }
        check_supported(s.substr(p), no);
        return resolve_plain(plain(in_map_key));
    }
};

bool flow_balanced(const std::string& t) {
    int depth = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        char c = t[i];
        if (c == '"' || c == '\'') {
            size_t e = quoted_end(t, i);
            if (e == std::string::npos) return false;
            i = e - 1;
            continue;
        }
        if (c == '[' || c == '{') ++depth;
        if (c == ']' || c == '}') --depth;
    }
    return depth <= 0;
}

// ---------------------------------------------------------------------------
// Block structure
// ---------------------------------------------------------------------------

struct Block {
    std::vector<Line> lines;

    // The value text after "key:" / "- ". `i` already points past its line;
    // continuation lines (more indented than parent_ind) are consumed.
    mj::Value inline_value(std::string rest, size_t& i, int parent_ind, int no) {
        char c = rest[0];
        if (c == '[' || c == '{') {
            while (!flow_balanced(rest) && i < lines.size()) {
                rest += " " + lines[i].text;
                ++i;
            }
            Flow f{rest, 0, no};
            mj::Value v = f.value();
            f.ws();
            if (f.p != rest.size()) throw YamlError("trailing text after flow collection on line " + std::to_string(no));
            return v;
        }
        if (c == '"' || c == '\'') {
            while (quoted_end(rest, 0) == std::string::npos && i < lines.size()) {
                rest += " " + lines[i].text;  // line folding
                ++i;
            }
            size_t e = quoted_end(rest, 0);
            if (e == std::string::npos) throw YamlError("unterminated quote on line " + std::to_string(no));
            if (!trim(rest.substr(e)).empty())
                throw YamlError("trailing text after quoted scalar on line " + std::to_string(no));
            return mj::Value::string(unquote(rest, 0, e));
        }
        check_supported(rest, no);
        while (i < lines.size() && lines[i].indent > parent_ind) {
            rest += " " + lines[i].text;
            ++i;
        }
        return resolve_plain(trim(rest));
    }

    mj::Value block(size_t& i, int ind) {
        if (i >= lines.size()) return mj::Value::null();
        const Line& L = lines[i];
        if (is_seq_item(L.text)) return seq(i, L.indent);
        if (find_map_colon(L.text) != std::string::npos) return map(i, L.indent);
        std::string text = L.text;
        int no = L.no;
        ++i;
        return inline_value(text, i, ind - 1, no);
    }

    mj::Value map(size_t& i, int ind) {
        mj::Value m = mj::Value::object();
        while (i < lines.size() && lines[i].indent == ind && !is_seq_item(lines[i].text)) {
            const std::string text = lines[i].text;
            const int no = lines[i].no;
            size_t pos = find_map_colon(text);
            if (pos == std::string::npos)
                throw YamlError("expected 'key: value' on line " + std::to_string(no) + ": " + text);
            std::string keytext = trim(text.substr(0, pos));
            std::string key;
            if (!keytext.empty() && (keytext[0] == '"' || keytext[0] == '\'')) {
                key = unquote(keytext, 0, keytext.size());
            } else {
                check_supported(keytext, no);
                key = py_key_str(resolve_plain(keytext));
            }
            std::string rest = trim(text.substr(pos + 1));
            ++i;
            mj::Value val;
            if (rest.empty()) {
                if (i < lines.size() && lines[i].indent > ind) val = block(i, lines[i].indent);
                else if (i < lines.size() && lines[i].indent == ind && is_seq_item(lines[i].text)) val = seq(i, ind);
                else val = mj::Value::null();
            } else {
                val = inline_value(rest, i, ind, no);
            }
            m.set(std::move(key), std::move(val));
        }
        if (i < lines.size() && lines[i].indent > ind)
            throw YamlError("bad indentation on line " + std::to_string(lines[i].no));
        return m;
    }

    mj::Value seq(size_t& i, int ind) {
        mj::Value arr = mj::Value::array();
        while (i < lines.size() && lines[i].indent == ind && is_seq_item(lines[i].text)) {
            const std::string text = lines[i].text;
            const int no = lines[i].no;
            size_t k = 1;
            while (k < text.size() && (text[k] == ' ' || text[k] == '\t')) ++k;
            std::string rest = text.substr(k);
            if (rest.empty()) {
                ++i;
                if (i < lines.size() && lines[i].indent > ind) arr.push(block(i, lines[i].indent));
                else arr.push(mj::Value::null());
                continue;
            }
            if (is_seq_item(rest) || find_map_colon(rest) != std::string::npos) {
                int col = ind + (int)k;
                lines[i] = Line{col, rest, no};  // re-read this item as a block at its column
                arr.push(block(i, col));
                continue;
            }
            ++i;
            arr.push(inline_value(rest, i, ind, no));
        }
        return arr;
    }
};

}  // namespace

mj::Value resolve_plain(const std::string& s) {
    if (s.empty()) return mj::Value::null();
    char c = s[0];
    auto m = [&](const std::regex& r) { return std::regex_match(s, r); };
    switch (c) {
        case 'y': case 'Y': case 'n': case 'N': case 't': case 'T':
        case 'f': case 'F': case 'o': case 'O':
            if (m(re_bool())) {
                std::string low;
                for (char ch : s) low.push_back((char)std::tolower((unsigned char)ch));
                return mj::Value::boolean(low == "yes" || low == "true" || low == "on");
            }
            if ((c == 'n' || c == 'N') && m(re_null())) return mj::Value::null();
            break;
        case '-': case '+':
            if (m(re_float())) return construct_float(s);
            if (m(re_int())) return construct_int(s);
            break;
        case '.':
            if (m(re_float())) return construct_float(s);
            break;
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            if (m(re_float())) return construct_float(s);
            if (m(re_int())) return construct_int(s);
            if (m(re_timestamp())) return construct_timestamp(s);  // PyYAML: date/datetime
            break;
        case '~':
            if (m(re_null())) return mj::Value::null();
            break;
        default: break;
    }
    return mj::Value::string(s);
}

mj::Value parse(const std::string& text) {
    Block b;
    std::stringstream ss(text);
    std::string raw;
    int no = 0;
    while (std::getline(ss, raw)) {
        ++no;
        std::string line = rtrim(strip_comment(raw));
        size_t ind = 0;
        while (ind < line.size() && line[ind] == ' ') ++ind;
        if (ind == line.size()) continue;
        if (line[ind] == '\t') throw YamlError("tab indentation on line " + std::to_string(no));
        std::string body = line.substr(ind);
        if (ind == 0 && (body == "---" || body.rfind("--- ", 0) == 0 || body == "...")) continue;
        b.lines.push_back(Line{(int)ind, body, no});
    }
    if (b.lines.empty()) return mj::Value::null();
    size_t i = 0;
    mj::Value v = b.block(i, b.lines[0].indent);
    if (i != b.lines.size())
        throw YamlError("unexpected content on line " + std::to_string(b.lines[i].no));
    return v;
}

mj::Value parse_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw YamlError("cannot open " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return parse(ss.str());
}

}  // namespace yamlite
