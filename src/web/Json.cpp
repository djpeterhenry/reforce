#include "web/Json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace reforce::web::json {

std::string quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
    return out;
}

void Writer::sep() {
    if (afterKey_) {
        afterKey_ = false;
        return;
    }
    if (!first_.empty()) {
        if (!first_.back()) out_.push_back(',');
        first_.back() = false;
    }
}

Writer& Writer::beginObject() {
    sep();
    out_.push_back('{');
    first_.push_back(true);
    return *this;
}
Writer& Writer::endObject() {
    out_.push_back('}');
    first_.pop_back();
    return *this;
}
Writer& Writer::beginArray() {
    sep();
    out_.push_back('[');
    first_.push_back(true);
    return *this;
}
Writer& Writer::endArray() {
    out_.push_back(']');
    first_.pop_back();
    return *this;
}
Writer& Writer::key(std::string_view k) {
    sep();
    out_ += quote(k);
    out_.push_back(':');
    afterKey_ = true;
    return *this;
}
Writer& Writer::value(std::string_view s) {
    sep();
    out_ += quote(s);
    return *this;
}
Writer& Writer::value(double d) {
    sep();
    if (!std::isfinite(d)) {
        out_ += "null";
        return *this;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6g", d);
    out_ += buf;
    return *this;
}
Writer& Writer::value(long long i) {
    sep();
    out_ += std::to_string(i);
    return *this;
}
Writer& Writer::value(unsigned long long i) {
    sep();
    out_ += std::to_string(i);
    return *this;
}
Writer& Writer::value(bool b) {
    sep();
    out_ += b ? "true" : "false";
    return *this;
}
Writer& Writer::null() {
    sep();
    out_ += "null";
    return *this;
}
Writer& Writer::stringArray(const std::vector<std::string>& v) {
    beginArray();
    for (const auto& s : v) value(s);
    return endArray();
}

const Value* Value::get(std::string_view key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& [k, v] : object)
        if (k == key) return &v;
    return nullptr;
}

namespace {

class Parser {
public:
    Parser(std::string_view t, int maxDepth) : t_(t), maxDepth_(maxDepth) {}

    std::optional<Value> run(std::string* error) {
        Value v;
        ws();
        if (!parseValue(v, 0)) return fail(error);
        ws();
        if (i_ != t_.size()) {
            err_ = "trailing characters";
            return fail(error);
        }
        return v;
    }

private:
    std::optional<Value> fail(std::string* error) {
        if (error) *error = err_ + " at offset " + std::to_string(i_);
        return std::nullopt;
    }
    void ws() {
        while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t' || t_[i_] == '\n' || t_[i_] == '\r')) ++i_;
    }
    bool literal(std::string_view lit) {
        if (t_.substr(i_, lit.size()) != lit) {
            err_ = "invalid literal";
            return false;
        }
        i_ += lit.size();
        return true;
    }
    bool parseValue(Value& v, int depth) {
        if (depth > maxDepth_) {
            err_ = "nesting too deep";
            return false;
        }
        if (i_ >= t_.size()) {
            err_ = "unexpected end";
            return false;
        }
        const char c = t_[i_];
        if (c == '{') return parseObject(v, depth);
        if (c == '[') return parseArray(v, depth);
        if (c == '"') {
            v.type = Value::Type::String;
            return parseString(v.str);
        }
        if (c == 't') {
            v.type = Value::Type::Bool;
            v.b = true;
            return literal("true");
        }
        if (c == 'f') {
            v.type = Value::Type::Bool;
            v.b = false;
            return literal("false");
        }
        if (c == 'n') {
            v.type = Value::Type::Null;
            return literal("null");
        }
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(v);
        err_ = "unexpected character";
        return false;
    }
    bool parseNumber(Value& v) {
        const std::size_t start = i_;
        if (t_[i_] == '-') ++i_;
        auto digits = [&] {
            const std::size_t s = i_;
            while (i_ < t_.size() && t_[i_] >= '0' && t_[i_] <= '9') ++i_;
            return i_ > s;
        };
        if (!digits()) {
            err_ = "invalid number";
            return false;
        }
        if (i_ < t_.size() && t_[i_] == '.') {
            ++i_;
            if (!digits()) {
                err_ = "invalid number";
                return false;
            }
        }
        if (i_ < t_.size() && (t_[i_] == 'e' || t_[i_] == 'E')) {
            ++i_;
            if (i_ < t_.size() && (t_[i_] == '+' || t_[i_] == '-')) ++i_;
            if (!digits()) {
                err_ = "invalid number";
                return false;
            }
        }
        const std::string s(t_.substr(start, i_ - start));
        v.type = Value::Type::Number;
        v.num = std::strtod(s.c_str(), nullptr);
        return true;
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back(char(cp));
        } else if (cp < 0x800) {
            out.push_back(char(0xC0 | (cp >> 6)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(char(0xE0 | (cp >> 12)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(char(0xF0 | (cp >> 18)));
            out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        }
    }
    bool hex4(unsigned& out) {
        if (i_ + 4 > t_.size()) return false;
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = t_[i_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') out |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= unsigned(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool parseString(std::string& out) {
        ++i_;  // opening quote
        while (i_ < t_.size()) {
            const char c = t_[i_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) {
                err_ = "control character in string";
                return false;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (i_ >= t_.size()) break;
            const char e = t_[i_++];
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
                    unsigned cp = 0;
                    if (!hex4(cp)) {
                        err_ = "invalid \\u escape";
                        return false;
                    }
                    if (cp >= 0xD800 && cp < 0xDC00 && i_ + 6 <= t_.size() && t_[i_] == '\\' && t_[i_ + 1] == 'u') {
                        i_ += 2;
                        unsigned lo = 0;
                        if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) {
                            err_ = "invalid surrogate pair";
                            return false;
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xD800 && cp < 0xE000) {
                        cp = 0xFFFD;
                    }
                    utf8(out, cp);
                    break;
                }
                default: err_ = "invalid escape"; return false;
            }
        }
        err_ = "unterminated string";
        return false;
    }
    bool parseArray(Value& v, int depth) {
        v.type = Value::Type::Array;
        ++i_;
        ws();
        if (i_ < t_.size() && t_[i_] == ']') {
            ++i_;
            return true;
        }
        while (true) {
            Value item;
            ws();
            if (!parseValue(item, depth + 1)) return false;
            v.array.push_back(std::move(item));
            ws();
            if (i_ < t_.size() && t_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < t_.size() && t_[i_] == ']') {
                ++i_;
                return true;
            }
            err_ = "expected , or ]";
            return false;
        }
    }
    bool parseObject(Value& v, int depth) {
        v.type = Value::Type::Object;
        ++i_;
        ws();
        if (i_ < t_.size() && t_[i_] == '}') {
            ++i_;
            return true;
        }
        while (true) {
            ws();
            if (i_ >= t_.size() || t_[i_] != '"') {
                err_ = "expected key";
                return false;
            }
            std::string k;
            if (!parseString(k)) return false;
            ws();
            if (i_ >= t_.size() || t_[i_] != ':') {
                err_ = "expected :";
                return false;
            }
            ++i_;
            ws();
            Value item;
            if (!parseValue(item, depth + 1)) return false;
            v.object.emplace_back(std::move(k), std::move(item));
            ws();
            if (i_ < t_.size() && t_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < t_.size() && t_[i_] == '}') {
                ++i_;
                return true;
            }
            err_ = "expected , or }";
            return false;
        }
    }

    std::string_view t_;
    std::size_t i_ = 0;
    int maxDepth_;
    std::string err_;
};

}  // namespace

std::optional<Value> parse(std::string_view text, std::string* error, int maxDepth) {
    return Parser(text, maxDepth).run(error);
}

}  // namespace reforce::web::json
