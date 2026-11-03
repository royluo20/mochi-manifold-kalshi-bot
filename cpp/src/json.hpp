// json.hpp — a small JSON DOM: the parser behaves like orjson.loads (strict
// JSON, duplicate keys keep the LAST value in the FIRST key's position, like a
// Python dict), the serializer writes what orjson.dumps writes. Objects keep
// insertion order; lookups are linear (every frame we parse has few keys).
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mj {

enum class Type : uint8_t { Null, Bool, Int, Float, String, Array, Object };

class Value;
using Member = std::pair<std::string, Value>;

class Value {
public:
    Type type = Type::Null;
    bool b = false;
    int64_t i = 0;
    double d = 0.0;
    std::string s;
    std::vector<Value> a;
    std::vector<std::pair<std::string, Value>> o;  // insertion-ordered members

    Value() = default;
    static Value null() { return Value(); }
    static Value boolean(bool v) { Value x; x.type = Type::Bool; x.b = v; return x; }
    static Value integer(int64_t v) { Value x; x.type = Type::Int; x.i = v; return x; }
    static Value number(double v) { Value x; x.type = Type::Float; x.d = v; return x; }
    static Value string(std::string v) { Value x; x.type = Type::String; x.s = std::move(v); return x; }
    static Value array() { Value x; x.type = Type::Array; return x; }
    static Value object() { Value x; x.type = Type::Object; return x; }

    bool is_null() const { return type == Type::Null; }
    bool is_bool() const { return type == Type::Bool; }
    bool is_int() const { return type == Type::Int; }
    bool is_float() const { return type == Type::Float; }
    bool is_number() const { return type == Type::Int || type == Type::Float; }
    bool is_string() const { return type == Type::String; }
    bool is_array() const { return type == Type::Array; }
    bool is_object() const { return type == Type::Object; }

    // Object access. get() returns nullptr when absent or when this is not an object.
    const Value* get(std::string_view key) const;
    Value* get_mut(std::string_view key);
    bool contains(std::string_view key) const { return get(key) != nullptr; }
    // Insert or replace (keeps the original position on replace, like dict).
    Value& set(std::string key, Value v);
    bool erase(std::string_view key);

    void push(Value v) { a.push_back(std::move(v)); }
    size_t size() const { return type == Type::Array ? a.size() : type == Type::Object ? o.size() : 0; }

    // Python truthiness (None/False/0/0.0/""/[]/{} are falsy; nan is truthy).
    bool truthy() const;
    // Number as double (Int or Float); caller checks is_number().
    double num() const { return type == Type::Int ? (double)i : d; }

    std::string dump() const;
    void dump_to(std::string& out) const;
};

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Parse a complete JSON document. Throws ParseError.
Value parse(std::string_view text);

// Append a JSON string literal (escaped like orjson).
void dump_string(std::string& out, std::string_view s);

}  // namespace mj
