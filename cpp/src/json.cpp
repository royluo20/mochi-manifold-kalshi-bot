#include "json.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "pyutil.hpp"

namespace mj {

const Value* Value::get(std::string_view key) const {
    if (type != Type::Object) return nullptr;
    for (const Member& m : o)
        if (m.first == key) return &m.second;
    return nullptr;
}

Value* Value::get_mut(std::string_view key) {
    if (type != Type::Object) return nullptr;
    for (Member& m : o)
        if (m.first == key) return &m.second;
    return nullptr;
}

Value& Value::set(std::string key, Value v) {
    if (type != Type::Object) {
        *this = Value::object();
    }
    for (Member& m : o)
        if (m.first == key) {
            m.second = std::move(v);
            return m.second;
        }
    o.emplace_back(std::move(key), std::move(v));
    return o.back().second;
}

bool Value::erase(std::string_view key) {
    if (type != Type::Object) return false;
    for (size_t k = 0; k < o.size(); ++k)
        if (o[k].first == key) {
            o.erase(o.begin() + (long)k);
            return true;
        }
    return false;
}

bool Value::truthy() const {
    switch (type) {
        case Type::Null: return false;
        case Type::Bool: return b;
        case Type::Int: return i != 0;
        case Type::Float: return d != 0.0;  // nan != 0 -> truthy, as in Python
        case Type::String: return !s.empty();
        case Type::Array: return !a.empty();
        case Type::Object: return !o.empty();
    }
    return false;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

namespace {

struct Parser {
    const char* p;
    const char* end;
    int depth = 0;

    [[noreturn]] void fail(const char* what) {
        throw ParseError(std::string("JSON parse error: ") + what);
    }

    void ws() {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
    }

    static void put_utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
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

    uint32_t hex4() {
        if (end - p < 4) fail("short \\u escape");
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
            else fail("bad \\u escape");
        }
        return v;
    }

    void string_into(std::string& out) {
        // *p == '"'
        ++p;
        const char* run = p;
        while (true) {
            if (p >= end) fail("unterminated string");
            unsigned char c = (unsigned char)*p;
            if (c == '"') {
                out.append(run, (size_t)(p - run));
                ++p;
                return;
            }
            if (c == '\\') {
                out.append(run, (size_t)(p - run));
                ++p;
                if (p >= end) fail("bad escape");
                char e = *p++;
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp = hex4();
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                                p += 2;
                                uint32_t lo = hex4();
                                if (lo < 0xDC00 || lo > 0xDFFF) fail("bad surrogate pair");
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            } else {
                                fail("lone surrogate");
                            }
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            fail("lone surrogate");
                        }
                        put_utf8(out, cp);
                        break;
                    }
                    default: fail("bad escape");
                }
                run = p;
                continue;
            }
            if (c < 0x20) fail("control character in string");
            ++p;
        }
    }

    Value number() {
        const char* start = p;
        bool is_float = false;
        if (p < end && *p == '-') ++p;
        if (p >= end) fail("bad number");
        if (*p == '0') {
            ++p;
        } else if (*p >= '1' && *p <= '9') {
            while (p < end && *p >= '0' && *p <= '9') ++p;
        } else {
            fail("bad number");
        }
        if (p < end && *p == '.') {
            is_float = true;
            ++p;
            if (p >= end || !(*p >= '0' && *p <= '9')) fail("bad fraction");
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            is_float = true;
            ++p;
            if (p < end && (*p == '+' || *p == '-')) ++p;
            if (p >= end || !(*p >= '0' && *p <= '9')) fail("bad exponent");
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        size_t n = (size_t)(p - start);
        char buf[512];
        std::string big;
        const char* cstr;
        if (n < sizeof buf) {
            std::memcpy(buf, start, n);
            buf[n] = 0;
            cstr = buf;
        } else {
            big.assign(start, n);
            cstr = big.c_str();
        }
        if (!is_float) {
            errno = 0;
            char* e = nullptr;
            long long v = std::strtoll(cstr, &e, 10);
            if (errno == 0) return Value::integer((int64_t)v);
            // Out of int64 range: keep as a float (orjson would keep a u64).
        }
        double d = std::strtod(cstr, nullptr);
        if (std::isinf(d)) fail("number out of range");  // orjson rejects 1e400
        return Value::number(d);
    }

    Value value() {
        ws();
        if (p >= end) fail("unexpected end");
        char c = *p;
        if (c == '{') {
            if (++depth > 1024) fail("too deep");
            ++p;
            Value obj = Value::object();
            ws();
            if (p < end && *p == '}') {
                ++p;
                --depth;
                return obj;
            }
            while (true) {
                ws();
                if (p >= end || *p != '"') fail("expected key");
                std::string key;
                string_into(key);
                ws();
                if (p >= end || *p != ':') fail("expected ':'");
                ++p;
                Value v = value();
                obj.set(std::move(key), std::move(v));
                ws();
                if (p >= end) fail("unterminated object");
                if (*p == ',') {
                    ++p;
                    continue;
                }
                if (*p == '}') {
                    ++p;
                    break;
                }
                fail("expected ',' or '}'");
            }
            --depth;
            return obj;
        }
        if (c == '[') {
            if (++depth > 1024) fail("too deep");
            ++p;
            Value arr = Value::array();
            ws();
            if (p < end && *p == ']') {
                ++p;
                --depth;
                return arr;
            }
            while (true) {
                arr.a.push_back(value());
                ws();
                if (p >= end) fail("unterminated array");
                if (*p == ',') {
                    ++p;
                    continue;
                }
                if (*p == ']') {
                    ++p;
                    break;
                }
                fail("expected ',' or ']'");
            }
            --depth;
            return arr;
        }
        if (c == '"') {
            Value v = Value::string("");
            string_into(v.s);
            return v;
        }
        if (c == 't') {
            if (end - p >= 4 && std::memcmp(p, "true", 4) == 0) {
                p += 4;
                return Value::boolean(true);
            }
            fail("bad literal");
        }
        if (c == 'f') {
            if (end - p >= 5 && std::memcmp(p, "false", 5) == 0) {
                p += 5;
                return Value::boolean(false);
            }
            fail("bad literal");
        }
        if (c == 'n') {
            if (end - p >= 4 && std::memcmp(p, "null", 4) == 0) {
                p += 4;
                return Value::null();
            }
            fail("bad literal");
        }
        if (c == '-' || (c >= '0' && c <= '9')) return number();
        fail("unexpected character");
    }
};

}  // namespace

Value parse(std::string_view text) {
    Parser ps{text.data(), text.data() + text.size()};
    Value v = ps.value();
    ps.ws();
    if (ps.p != ps.end) ps.fail("trailing characters");
    return v;
}

// ---------------------------------------------------------------------------
// Serializer (orjson-compatible output)
// ---------------------------------------------------------------------------

void dump_string(std::string& out, std::string_view s) {
    static const char* HEX = "0123456789abcdef";
    out.push_back('"');
    size_t run = 0;
    for (size_t k = 0; k < s.size(); ++k) {
        unsigned char c = (unsigned char)s[k];
        const char* esc = nullptr;
        char u[7];
        switch (c) {
            case '"': esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\b': esc = "\\b"; break;
            case '\f': esc = "\\f"; break;
            case '\n': esc = "\\n"; break;
            case '\r': esc = "\\r"; break;
            case '\t': esc = "\\t"; break;
            default:
                if (c < 0x20) {
                    u[0] = '\\'; u[1] = 'u'; u[2] = '0'; u[3] = '0';
                    u[4] = HEX[c >> 4]; u[5] = HEX[c & 15]; u[6] = 0;
                    esc = u;
                }
        }
        if (esc) {
            out.append(s.data() + run, k - run);
            out += esc;
            run = k + 1;
        }
    }
    out.append(s.data() + run, s.size() - run);
    out.push_back('"');
}

void Value::dump_to(std::string& out) const {
    switch (type) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += b ? "true" : "false"; break;
        case Type::Int: out += std::to_string(i); break;
        case Type::Float: out += py::orjson_float(d); break;
        case Type::String: dump_string(out, s); break;
        case Type::Array: {
            out.push_back('[');
            for (size_t k = 0; k < a.size(); ++k) {
                if (k) out.push_back(',');
                a[k].dump_to(out);
            }
            out.push_back(']');
            break;
        }
        case Type::Object: {
            out.push_back('{');
            for (size_t k = 0; k < o.size(); ++k) {
                if (k) out.push_back(',');
                dump_string(out, o[k].first);
                out.push_back(':');
                o[k].second.dump_to(out);
            }
            out.push_back('}');
            break;
        }
    }
}

std::string Value::dump() const {
    std::string out;
    dump_to(out);
    return out;
}

}  // namespace mj
