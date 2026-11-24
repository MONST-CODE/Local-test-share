#pragma once
// Canonical, exact-origin policy for browser navigation.
// This deliberately accepts only ASCII HTTP(S) URLs and an explicit origin
// allowlist. It is not a general-purpose WHATWG URL parser.
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace plpt::browser::url_origin {

inline char lower_ascii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}
inline bool ascii_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower_ascii(a[i]) != lower_ascii(b[i])) return false;
    return true;
}
inline bool has_forbidden_byte(std::string_view value) {
    for (const unsigned char c : value)
        if (c <= 0x20 || c >= 0x7f || c == '\\') return true;
    return false;
}

inline bool parse_ipv4(std::string_view value, std::array<std::uint16_t, 2>* tail = nullptr) {
    std::array<unsigned, 4> octets{};
    std::size_t start = 0;
    for (std::size_t i = 0; i < octets.size(); ++i) {
        const auto dot = value.find('.', start);
        const auto stop = dot == std::string_view::npos ? value.size() : dot;
        const auto part = value.substr(start, stop - start);
        if (part.empty() || part.size() > 3 || (part.size() > 1 && part.front() == '0')) return false;
        unsigned octet = 0;
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), octet);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() || octet > 255) return false;
        octets[i] = octet;
        if (i + 1 < octets.size() && dot == std::string_view::npos) return false;
        if (i + 1 == octets.size() && dot != std::string_view::npos) return false;
        start = stop + 1;
    }
    if (tail) {
        (*tail)[0] = static_cast<std::uint16_t>((octets[0] << 8) | octets[1]);
        (*tail)[1] = static_cast<std::uint16_t>((octets[2] << 8) | octets[3]);
    }
    return true;
}

inline bool is_ipv4_number(std::string_view label) {
    if (label.empty()) return false;
    std::size_t offset = 0;
    int base = 10;
    if (label.size() >= 2 && label[0] == '0' && lower_ascii(label[1]) == 'x') {
        offset = 2;
        base = 16;
        if (offset == label.size()) return false;
    }
    unsigned value = 0;
    const auto parsed = std::from_chars(label.data() + offset, label.data() + label.size(), value, base);
    return parsed.ec == std::errc{} && parsed.ptr == label.data() + label.size();
}
inline bool looks_like_ipv4_host(std::string_view host) {
    std::size_t start = 0;
    while (start <= host.size()) {
        const auto dot = host.find('.', start);
        const auto end = dot == std::string_view::npos ? host.size() : dot;
        if (!is_ipv4_number(host.substr(start, end - start))) return false;
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return true;
}

inline bool parse_ipv6_side(std::string_view side, std::vector<std::uint16_t>& groups, bool allow_ipv4) {
    if (side.empty()) return true;
    std::size_t start = 0;
    while (start <= side.size()) {
        const auto colon = side.find(':', start);
        const auto stop = colon == std::string_view::npos ? side.size() : colon;
        const auto part = side.substr(start, stop - start);
        if (part.empty()) return false;
        if (part.find('.') != std::string_view::npos) {
            std::array<std::uint16_t, 2> tail{};
            if (!allow_ipv4 || colon != std::string_view::npos || !parse_ipv4(part, &tail)) return false;
            groups.push_back(tail[0]);
            groups.push_back(tail[1]);
        } else {
            if (part.size() > 4) return false;
            unsigned value = 0;
            const auto parsed = std::from_chars(part.data(), part.data() + part.size(), value, 16);
            if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size()) return false;
            groups.push_back(static_cast<std::uint16_t>(value));
        }
        if (colon == std::string_view::npos) break;
        start = colon + 1;
    }
    return true;
}

inline std::optional<std::string> canonical_ipv6(std::string_view value) {
    if (value.empty() || value.find('%') != std::string_view::npos || value.find(":::") != std::string_view::npos)
        return std::nullopt;
    const auto compression = value.find("::");
    if (compression != std::string_view::npos && value.find("::", compression + 2) != std::string_view::npos)
        return std::nullopt;
    const bool compressed = compression != std::string_view::npos;
    std::vector<std::uint16_t> left, right;
    if (compressed) {
        if (!parse_ipv6_side(value.substr(0, compression), left, false) ||
            !parse_ipv6_side(value.substr(compression + 2), right, true)) return std::nullopt;
    } else if (!parse_ipv6_side(value, left, true)) {
        return std::nullopt;
    }
    if ((!compressed && left.size() != 8) || (compressed && left.size() + right.size() >= 8)) return std::nullopt;
    std::array<std::uint16_t, 8> groups{};
    std::copy(left.begin(), left.end(), groups.begin());
    if (compressed) std::copy(right.begin(), right.end(), groups.end() - static_cast<std::ptrdiff_t>(right.size()));

    std::size_t zero_start = groups.size(), zero_length = 0;
    for (std::size_t i = 0; i < groups.size();) {
        if (groups[i] != 0) { ++i; continue; }
        std::size_t end = i;
        while (end < groups.size() && groups[end] == 0) ++end;
        if (end - i > zero_length) { zero_start = i; zero_length = end - i; }
        i = end;
    }
    if (zero_length < 2) { zero_start = groups.size(); zero_length = 0; }
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (std::size_t i = 0; i < groups.size();) {
        if (i == zero_start) { result += "::"; i += zero_length; continue; }
        if (!result.empty() && result.back() != ':') result.push_back(':');
        const unsigned word = groups[i++];
        bool started = false;
        for (int shift = 12; shift >= 0; shift -= 4) {
            const unsigned digit = (word >> shift) & 0xfU;
            if (digit != 0 || started || shift == 0) { result.push_back(hex[digit]); started = true; }
        }
    }
    return result;
}

// Returns canonical scheme://host[:non-default-port], or nullopt for URLs
// outside the deliberately narrow accepted grammar. If origin_only is true,
// only the origin (optionally followed by one slash) is accepted.
inline std::optional<std::string> canonicalize(std::string_view url, bool origin_only = false) {
    if (url.empty() || has_forbidden_byte(url)) return std::nullopt;
    const auto delimiter = url.find("://");
    if (delimiter == std::string_view::npos) return std::nullopt;
    std::string scheme;
    if (ascii_equal(url.substr(0, delimiter), "http")) scheme = "http";
    else if (ascii_equal(url.substr(0, delimiter), "https")) scheme = "https";
    else return std::nullopt;

    const auto authority_start = delimiter + 3;
    const auto authority_end = url.find_first_of("/?#", authority_start);
    const auto stop = authority_end == std::string_view::npos ? url.size() : authority_end;
    const auto authority = url.substr(authority_start, stop - authority_start);
    if (authority.empty() || authority.find('@') != std::string_view::npos || authority.find('%') != std::string_view::npos)
        return std::nullopt;
    if (origin_only && authority_end != std::string_view::npos && url.substr(authority_end) != "/") return std::nullopt;

    std::string host;
    std::string_view port_text;
    bool explicit_port = false;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        auto address = canonical_ipv6(authority.substr(1, close - 1));
        if (!address) return std::nullopt;
        host = "[" + *address + "]";
        const auto suffix = authority.substr(close + 1);
        if (!suffix.empty()) {
            if (suffix.front() != ':' || suffix.size() == 1) return std::nullopt;
            explicit_port = true;
            port_text = suffix.substr(1);
        }
    } else {
        const auto colon = authority.rfind(':');
        auto raw_host = authority;
        if (colon != std::string_view::npos) {
            if (authority.find(':') != colon || colon + 1 == authority.size()) return std::nullopt;
            raw_host = authority.substr(0, colon);
            port_text = authority.substr(colon + 1);
            explicit_port = true;
        }
        if (raw_host.empty() || raw_host.size() > 253) return std::nullopt;
        host.reserve(raw_host.size());
        for (const char c : raw_host) {
            const char lower = lower_ascii(c);
            if (!((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') || lower == '-' || lower == '.'))
                return std::nullopt;
            host.push_back(lower);
        }
        if (host.back() == '.') host.pop_back();
        if (host.empty()) return std::nullopt;
        const bool ipv4_candidate = looks_like_ipv4_host(host);
        if (ipv4_candidate) {
            // Reject WHATWG-style non-decimal/abbreviated/octal IP spellings;
            // never let an allowlisted DNS-looking string alias another host.
            if (!parse_ipv4(host)) return std::nullopt;
        } else {
            std::size_t start = 0;
            while (start <= host.size()) {
                const auto dot = host.find('.', start);
                const auto end = dot == std::string::npos ? host.size() : dot;
                const auto label = std::string_view(host).substr(start, end - start);
                if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return std::nullopt;
                if (dot == std::string::npos) break;
                start = dot + 1;
            }
        }
    }

    unsigned port = scheme == "https" ? 443U : 80U;
    if (explicit_port) {
        if (port_text.empty() || port_text.size() > 5) return std::nullopt;
        const auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
        if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() || port == 0 || port > 65535)
            return std::nullopt;
    }
    std::string origin = scheme + "://" + host;
    if ((scheme == "http" && port != 80) || (scheme == "https" && port != 443)) origin += ":" + std::to_string(port);
    return origin;
}

inline bool is_allowed(std::string_view target, const std::vector<std::string>& allowed_origins) {
    const auto requested = canonicalize(target);
    if (!requested) return false;
    for (const auto& configured : allowed_origins) {
        const auto allowed = canonicalize(configured, true);
        if (allowed && *allowed == *requested) return true;
    }
    return false;
}

} // namespace plpt::browser::url_origin
