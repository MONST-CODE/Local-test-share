#pragma once
// DevLink — engine/browser/cdp_json.hpp
// Minimal, bounded JSON reader/writer helpers for the Chrome DevTools Protocol
// messages that the WebView2 browser worker exchanges with its OWN embedded
// Chromium (Page.screencastFrame, Input.*, Runtime results). The input comes
// from the local browser process, never from the network, but it is still
// parsed with explicit depth/size limits and fails closed on malformed data.
// This is not a general-purpose JSON library: numbers are doubles, objects keep
// insertion order, duplicate keys keep the first value.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace plpt::browser::cdp {

struct Field;

struct Value {
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<Value> items;                          // Array
    std::vector<Field> fields;                         // Object

    const Value* get(std::string_view key) const;
    std::optional<std::string_view> string_at(std::string_view key) const;
    std::optional<double> number_at(std::string_view key) const;
};

struct Field {
    std::string name;
    Value value;
};

inline const Value* Value::get(std::string_view key) const {
    if (kind != Kind::Object) return nullptr;
    for (const auto& f : fields)
        if (f.name == key) return &f.value;
    return nullptr;
}
inline std::optional<std::string_view> Value::string_at(std::string_view key) const {
    const Value* v = get(key);
    if (!v || v->kind != Kind::String) return std::nullopt;
    return std::string_view(v->text);
}
inline std::optional<double> Value::number_at(std::string_view key) const {
    const Value* v = get(key);
    if (!v || v->kind != Kind::Number) return std::nullopt;
    return v->number;
}

namespace detail {
class Parser {
public:
    Parser(std::string_view text, std::size_t max_depth) : s_(text), max_depth_(max_depth) {}
    std::optional<Value> run() {
        Value v;
        if (!value(v, 0)) return std::nullopt;
        ws();
        if (i_ != s_.size()) return std::nullopt;
        return v;
    }

private:
    void ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_;
    }
    bool literal(std::string_view word) {
        if (s_.substr(i_, word.size()) != word) return false;
        i_ += word.size();
        return true;
    }
    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    bool hex4(std::uint32_t& out) {
        if (i_ + 4 > s_.size()) return false;
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& out) {
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        ++i_;
        // Fast path: long base64 payloads contain no escapes.
        const auto start = i_;
        while (i_ < s_.size() && s_[i_] != '"' && s_[i_] != '\\' && static_cast<unsigned char>(s_[i_]) >= 0x20) ++i_;
        out.assign(s_.data() + start, i_ - start);
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') { out.push_back(c); continue; }
            if (i_ >= s_.size()) return false;
            const char e = s_[i_++];
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
                std::uint32_t cp = 0;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    std::uint32_t low = 0;
                    if (i_ + 2 > s_.size() || s_[i_] != '\\' || s_[i_ + 1] != 'u') return false;
                    i_ += 2;
                    if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return false;
                }
                append_utf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool number(double& out) {
        const auto start = i_;
        if (i_ < s_.size() && s_[i_] == '-') ++i_;
        bool digits = false;
        while (i_ < s_.size() && ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '.' || s_[i_] == 'e' ||
                   s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-')) {
            digits = true;
            ++i_;
        }
        if (!digits || i_ - start > 64) return false;
        const std::string token(s_.data() + start, i_ - start);
        char* end = nullptr;
        out = std::strtod(token.c_str(), &end);
        return end == token.c_str() + token.size();
    }
    bool value(Value& v, std::size_t depth) {
        if (depth > max_depth_) return false;
        ws();
        if (i_ >= s_.size()) return false;
        const char c = s_[i_];
        if (c == '{') {
            v.kind = Value::Kind::Object;
            ++i_;
            ws();
            if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
            while (true) {
                ws();
                std::string key;
                if (!string(key)) return false;
                ws();
                if (i_ >= s_.size() || s_[i_] != ':') return false;
                ++i_;
                Value child;
                if (!value(child, depth + 1)) return false;
                bool duplicate = false;
                for (const auto& f : v.fields) duplicate = duplicate || f.name == key;
                if (!duplicate) v.fields.push_back(Field{std::move(key), std::move(child)});
                ws();
                if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.kind = Value::Kind::Array;
            ++i_;
            ws();
            if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
            while (true) {
                Value child;
                if (!value(child, depth + 1)) return false;
                v.items.push_back(std::move(child));
                ws();
                if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
                return false;
            }
        }
        if (c == '"') { v.kind = Value::Kind::String; return string(v.text); }
        if (c == 't') { v.kind = Value::Kind::Bool; v.boolean = true; return literal("true"); }
        if (c == 'f') { v.kind = Value::Kind::Bool; v.boolean = false; return literal("false"); }
        if (c == 'n') { v.kind = Value::Kind::Null; return literal("null"); }
        v.kind = Value::Kind::Number;
        return number(v.number);
    }

    std::string_view s_;
    std::size_t i_ = 0;
    std::size_t max_depth_;
};
} // namespace detail

inline constexpr std::size_t kMaxJsonBytes = 48u * 1024u * 1024u;

inline std::optional<Value> parse(std::string_view text, std::size_t max_depth = 32) {
    if (text.size() > kMaxJsonBytes) return std::nullopt;
    return detail::Parser(text, max_depth).run();
}

// JSON string literal (with quotes). Control characters are \u-escaped; UTF-8
// is passed through unchanged (the caller validates UTF-8 beforehand).
inline std::string quote(std::string_view text) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20 || c == 0x7f) {
                out += "\\u00";
                out.push_back(kHex[c >> 4]);
                out.push_back(kHex[c & 0xF]);
            } else {
                out.push_back(ch);
            }
        }
    }
    out.push_back('"');
    return out;
}

// RFC 4648 base64 decoding with padding; rejects any non-alphabet byte.
inline std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view in, std::size_t max_output) {
    if (in.size() % 4 != 0) return std::nullopt;
    if (in.size() / 4 * 3 > max_output + 2) return std::nullopt;
    auto sextet = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<std::uint8_t> out;
    out.reserve(in.size() / 4 * 3);
    for (std::size_t i = 0; i < in.size(); i += 4) {
        const bool last = i + 4 == in.size();
        const int a = sextet(in[i]);
        const int b = sextet(in[i + 1]);
        if (a < 0 || b < 0) return std::nullopt;
        const bool pad2 = last && in[i + 2] == '=' && in[i + 3] == '=';
        const bool pad1 = last && !pad2 && in[i + 3] == '=';
        const int c = pad2 ? 0 : sextet(in[i + 2]);
        const int d = (pad1 || pad2) ? 0 : sextet(in[i + 3]);
        if (c < 0 || d < 0) return std::nullopt;
        const std::uint32_t triple = (static_cast<std::uint32_t>(a) << 18) | (static_cast<std::uint32_t>(b) << 12) |
            (static_cast<std::uint32_t>(c) << 6) | static_cast<std::uint32_t>(d);
        out.push_back(static_cast<std::uint8_t>(triple >> 16));
        if (!pad2) out.push_back(static_cast<std::uint8_t>((triple >> 8) & 0xFF));
        if (!pad1 && !pad2) out.push_back(static_cast<std::uint8_t>(triple & 0xFF));
    }
    if (out.size() > max_output) return std::nullopt;
    return out;
}

} // namespace plpt::browser::cdp
