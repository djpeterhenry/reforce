#pragma once
// Minimal JSON for the web API: a streaming writer for responses and a small
// DOM parser for request bodies (objects, arrays, strings, numbers, bools,
// null; UTF-8 passed through, \uXXXX decoded to UTF-8). Depth-limited.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reforce::web::json {

// `s` as a JSON string literal (with quotes).
std::string quote(std::string_view s);

class Writer {
public:
    Writer& beginObject();
    Writer& endObject();
    Writer& beginArray();
    Writer& endArray();
    Writer& key(std::string_view k);
    Writer& value(std::string_view s);
    Writer& value(const char* s) { return value(std::string_view(s)); }
    Writer& value(const std::string& s) { return value(std::string_view(s)); }
    Writer& value(double d);  // NaN/inf -> null
    Writer& value(int i) { return value(static_cast<long long>(i)); }
    Writer& value(long long i);
    Writer& value(unsigned long long i);
    Writer& value(std::size_t i) { return value(static_cast<unsigned long long>(i)); }
    Writer& value(bool b);
    Writer& null();
    Writer& stringArray(const std::vector<std::string>& v);

    template <class T>
    Writer& field(std::string_view k, const T& v) {
        key(k);
        return value(v);
    }

    const std::string& str() const { return out_; }
    std::string take() { return std::move(out_); }

private:
    void sep();
    std::string out_;
    std::vector<bool> first_;  // per open container: no element yet
    bool afterKey_ = false;
};

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }
    bool isString() const { return type == Type::String; }
    bool isNumber() const { return type == Type::Number; }
    bool isBool() const { return type == Type::Bool; }
    // Object member (first match), nullptr when absent or not an object.
    const Value* get(std::string_view key) const;
};

std::optional<Value> parse(std::string_view text, std::string* error = nullptr, int maxDepth = 32);

}  // namespace reforce::web::json
