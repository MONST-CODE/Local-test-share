#pragma once
// Bounded readers for the build's ICO and PE resource gates. This is build/test
// tooling only; no parser result is used as a runtime security decision.
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace devlink_probe {

inline constexpr std::size_t kMaxProbeFileBytes = 256U * 1024U * 1024U;

inline bool range_ok(std::size_t offset, std::size_t length, std::size_t total) {
    return offset <= total && length <= total - offset;
}

inline std::optional<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > kMaxProbeFileBytes) return std::nullopt;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream || stream.peek() != std::char_traits<char>::eof()) return std::nullopt;
    return bytes;
}

inline std::uint16_t u16(const std::vector<std::uint8_t>& b, std::size_t p) {
    return static_cast<std::uint16_t>(b[p]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(b[p + 1]) << 8U);
}

inline std::uint32_t u32(const std::vector<std::uint8_t>& b, std::size_t p) {
    return static_cast<std::uint32_t>(b[p]) |
        (static_cast<std::uint32_t>(b[p + 1]) << 8U) |
        (static_cast<std::uint32_t>(b[p + 2]) << 16U) |
        (static_cast<std::uint32_t>(b[p + 3]) << 24U);
}

inline std::uint32_t be32(const std::vector<std::uint8_t>& b, std::size_t p) {
    return (static_cast<std::uint32_t>(b[p]) << 24U) |
        (static_cast<std::uint32_t>(b[p + 1]) << 16U) |
        (static_cast<std::uint32_t>(b[p + 2]) << 8U) |
        static_cast<std::uint32_t>(b[p + 3]);
}

inline bool probe_ico(const std::filesystem::path& path, unsigned expected_entries) {
    const auto file = read_file(path);
    if (!file || file->size() < 6 || u16(*file, 0) != 0 || u16(*file, 2) != 1) return false;
    const std::size_t count = u16(*file, 4);
    if (count == 0 || count > 64 || count != expected_entries ||
        !range_ok(6, count * 16, file->size())) return false;

    constexpr std::array<unsigned, 7> required{16, 24, 32, 48, 64, 128, 256};
    std::set<unsigned> dimensions;
    std::vector<std::pair<std::size_t, std::size_t>> payload_ranges;
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t entry = 6 + index * 16;
        const unsigned width = (*file)[entry] == 0 ? 256U : (*file)[entry];
        const unsigned height = (*file)[entry + 1] == 0 ? 256U : (*file)[entry + 1];
        const unsigned planes = u16(*file, entry + 4);
        const unsigned bits = u16(*file, entry + 6);
        const std::size_t bytes = u32(*file, entry + 8);
        const std::size_t offset = u32(*file, entry + 12);
        if (width != height || !dimensions.insert(width).second || bytes == 0 ||
            offset < 6 + count * 16 || !range_ok(offset, bytes, file->size())) return false;
        payload_ranges.emplace_back(offset, offset + bytes);
        if (bytes >= 8 && (*file)[offset] == 0x89 && (*file)[offset + 1] == 'P' &&
            (*file)[offset + 2] == 'N' && (*file)[offset + 3] == 'G') {
            constexpr std::array<std::uint8_t, 8> signature{0x89, 'P', 'N', 'G', 13, 10, 26, 10};
            if (bytes < 33 || !std::equal(signature.begin(), signature.end(), file->begin() + static_cast<std::ptrdiff_t>(offset)) ||
                be32(*file, offset + 8) != 13 ||
                std::string_view(reinterpret_cast<const char*>(file->data() + offset + 12), 4) != "IHDR" ||
                be32(*file, offset + 16) != width || be32(*file, offset + 20) != height ||
                (*file)[offset + 24] != 8 || (*file)[offset + 25] != 6) return false;
        } else {
            if (planes != 1 || bits != 32 || bytes < 40 || u32(*file, offset) < 40 ||
                u32(*file, offset + 4) != width || u32(*file, offset + 8) != height * 2U ||
                u16(*file, offset + 12) != 1 || u16(*file, offset + 14) != 32) return false;
        }
    }
    for (const unsigned required_size : required) if (!dimensions.contains(required_size)) return false;
    std::sort(payload_ranges.begin(), payload_ranges.end());
    for (std::size_t i = 1; i < payload_ranges.size(); ++i)
        if (payload_ranges[i].first < payload_ranges[i - 1].second) return false;
    return true;
}

struct ResourceLocation {
    std::size_t offset = 0;
    std::size_t size = 0;
};

class PeImage {
public:
    explicit PeImage(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}

    bool initialize() {
        if (!range_ok(0, 64, bytes_.size()) || bytes_[0] != 'M' || bytes_[1] != 'Z') return false;
        const std::size_t pe = u32(bytes_, 0x3c);
        if (!range_ok(pe, 24, bytes_.size()) || bytes_[pe] != 'P' || bytes_[pe + 1] != 'E' ||
            bytes_[pe + 2] != 0 || bytes_[pe + 3] != 0) return false;
        const std::size_t coff = pe + 4;
        if (u16(bytes_, coff) != 0x8664) return false;
        const std::size_t section_count = u16(bytes_, coff + 2);
        const std::size_t optional_size = u16(bytes_, coff + 16);
        const std::size_t optional = coff + 20;
        if (section_count == 0 || section_count > 96 || optional_size < 112 ||
            !range_ok(optional, optional_size, bytes_.size()) || u16(bytes_, optional) != 0x20b) return false;
        const std::size_t directory_count = u32(bytes_, optional + 108);
        if (directory_count <= 2 || optional_size < 112 + 3 * 8) return false;
        resource_rva_ = u32(bytes_, optional + 112 + 2 * 8);
        resource_size_ = u32(bytes_, optional + 112 + 2 * 8 + 4);
        const std::size_t sections = optional + optional_size;
        if (resource_rva_ == 0 || resource_size_ < 16 ||
            !range_ok(sections, section_count * 40, bytes_.size())) return false;
        for (std::size_t i = 0; i < section_count; ++i) {
            const std::size_t s = sections + i * 40;
            sections_.push_back({u32(bytes_, s + 12), u32(bytes_, s + 8), u32(bytes_, s + 20), u32(bytes_, s + 16)});
        }
        const auto base = rva_to_file(resource_rva_, resource_size_);
        if (!base) return false;
        resource_file_offset_ = *base;
        return true;
    }

    std::optional<ResourceLocation> find_resource(std::uint32_t type, std::uint32_t id) const {
        const auto type_directory = find_directory(0, type);
        if (!type_directory) return std::nullopt;
        const auto name_directory = find_directory(*type_directory, id);
        if (!name_directory) return std::nullopt;
        const auto language_entry = first_entry(*name_directory);
        if (!language_entry || (language_entry->second & 0x80000000U) != 0) return std::nullopt;
        const std::size_t entry_offset = resource_file_offset_ + (language_entry->second & 0x7fffffffU);
        if (!resource_range(entry_offset, 16)) return std::nullopt;
        const std::uint32_t data_rva = u32(bytes_, entry_offset);
        const std::size_t data_size = u32(bytes_, entry_offset + 4);
        const std::uint64_t resource_end = static_cast<std::uint64_t>(resource_rva_) + resource_size_;
        const std::uint64_t data_end = static_cast<std::uint64_t>(data_rva) + data_size;
        if (data_rva < resource_rva_ || data_end > resource_end) return std::nullopt;
        const auto data_offset = rva_to_file(data_rva, data_size);
        if (!data_offset) return std::nullopt;
        return ResourceLocation{*data_offset, data_size};
    }

    std::optional<std::size_t> count_resources(std::uint32_t type) const {
        const auto directory = find_directory(0, type);
        if (!directory) return std::nullopt;
        const auto offset = resource_file_offset_ + *directory;
        if (!resource_range(offset, 16)) return std::nullopt;
        const std::size_t named = u16(bytes_, offset + 12);
        const std::size_t ids = u16(bytes_, offset + 14);
        if (named > 4096 || ids > 4096 || !resource_range(offset + 16, (named + ids) * 8)) return std::nullopt;
        return named + ids;
    }

    const std::vector<std::uint8_t>& bytes() const { return bytes_; }

private:
    struct Section { std::uint32_t virtual_address, virtual_size, raw_offset, raw_size; };

    bool resource_range(std::size_t offset, std::size_t length) const {
        return offset >= resource_file_offset_ && range_ok(offset, length, bytes_.size()) &&
            offset - resource_file_offset_ <= resource_size_ &&
            length <= resource_size_ - (offset - resource_file_offset_);
    }

    std::optional<std::size_t> rva_to_file(std::uint32_t rva, std::size_t size) const {
        for (const Section& section : sections_) {
            if (rva < section.virtual_address) continue;
            const std::uint64_t delta = static_cast<std::uint64_t>(rva) - section.virtual_address;
            if (delta > section.raw_size || size > section.raw_size - static_cast<std::size_t>(delta)) continue;
            const std::uint64_t offset = static_cast<std::uint64_t>(section.raw_offset) + delta;
            if (offset > std::numeric_limits<std::size_t>::max()) continue;
            const auto result = static_cast<std::size_t>(offset);
            if (range_ok(result, size, bytes_.size())) return result;
        }
        return std::nullopt;
    }

    std::optional<std::uint32_t> find_directory(std::uint32_t directory, std::uint32_t id) const {
        if ((directory & 0x80000000U) != 0) return std::nullopt;
        const std::size_t offset = resource_file_offset_ + directory;
        if (!resource_range(offset, 16)) return std::nullopt;
        const std::size_t named = u16(bytes_, offset + 12);
        const std::size_t ids = u16(bytes_, offset + 14);
        if (named > 4096 || ids > 4096 || !resource_range(offset + 16, (named + ids) * 8)) return std::nullopt;
        for (std::size_t i = 0; i < named + ids; ++i) {
            const std::size_t entry = offset + 16 + i * 8;
            const std::uint32_t name = u32(bytes_, entry);
            if ((name & 0x80000000U) == 0 && name == id) {
                const std::uint32_t child = u32(bytes_, entry + 4);
                if ((child & 0x80000000U) == 0) return std::nullopt;
                return child & 0x7fffffffU;
            }
        }
        return std::nullopt;
    }

    std::optional<std::pair<std::uint32_t, std::uint32_t>> first_entry(std::uint32_t directory) const {
        const std::size_t offset = resource_file_offset_ + directory;
        if (!resource_range(offset, 16)) return std::nullopt;
        const std::size_t named = u16(bytes_, offset + 12);
        const std::size_t ids = u16(bytes_, offset + 14);
        if (named > 4096 || ids > 4096 || named + ids == 0 ||
            !resource_range(offset + 16, (named + ids) * 8)) return std::nullopt;
        const std::size_t entry = offset + 16 + named * 8;
        return std::pair{u32(bytes_, entry), u32(bytes_, entry + 4)};
    }

    std::vector<std::uint8_t> bytes_;
    std::vector<Section> sections_;
    std::uint32_t resource_rva_ = 0;
    std::uint32_t resource_size_ = 0;
    std::size_t resource_file_offset_ = 0;
};

inline std::optional<std::array<std::uint16_t, 4>> parse_version(std::string_view text) {
    std::array<std::uint16_t, 4> result{};
    std::size_t start = 0;
    for (std::size_t i = 0; i < result.size(); ++i) {
        const std::size_t end = i + 1 == result.size() ? text.size() : text.find('.', start);
        if (end == std::string_view::npos || end == start) return std::nullopt;
        unsigned value = 0;
        const auto parsed = std::from_chars(text.data() + start, text.data() + end, value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + end || value > 65535) return std::nullopt;
        result[i] = static_cast<std::uint16_t>(value);
        start = end + 1;
    }
    if (start != text.size() + 1) return std::nullopt;
    return result;
}

inline bool probe_pe_icons(const std::filesystem::path& path, unsigned expected_icons) {
    const auto file = read_file(path);
    if (!file) return false;
    PeImage pe(*file);
    if (!pe.initialize()) return false;
    const auto group = pe.find_resource(14, 1); // RT_GROUP_ICON, resource ID 1
    const auto count = pe.count_resources(3);   // RT_ICON
    if (!group || !count || *count != expected_icons || expected_icons == 0 || expected_icons > 64 ||
        group->size < 6 || u16(pe.bytes(), group->offset) != 0 || u16(pe.bytes(), group->offset + 2) != 1 ||
        u16(pe.bytes(), group->offset + 4) != expected_icons ||
        group->size != 6 + static_cast<std::size_t>(expected_icons) * 14) return false;
    std::set<std::uint16_t> ids;
    for (std::size_t i = 0; i < expected_icons; ++i) {
        const std::size_t entry = group->offset + 6 + i * 14;
        const std::uint32_t image_size = u32(pe.bytes(), entry + 8);
        const std::uint16_t id = u16(pe.bytes(), entry + 12);
        if (id == 0 || image_size == 0 || !ids.insert(id).second) return false;
        const auto image = pe.find_resource(3, id);
        if (!image || image->size != image_size) return false;
    }
    return ids.size() == expected_icons;
}

inline bool probe_manifest(const std::filesystem::path& path, std::string_view expected_version) {
    const auto file = read_file(path);
    if (!file) return false;
    PeImage pe(*file);
    if (!pe.initialize()) return false;
    const auto manifest = pe.find_resource(24, 1); // RT_MANIFEST, resource ID 1
    const auto version = pe.find_resource(16, 1);  // RT_VERSION, resource ID 1
    if (!manifest || !version || manifest->size == 0 || version->size < 6) return false;
    const std::string_view xml(reinterpret_cast<const char*>(pe.bytes().data() + manifest->offset), manifest->size);
    if (xml.find("asInvoker") == std::string_view::npos ||
        xml.find("PerMonitorV2") == std::string_view::npos) return false;
    const std::string expected_manifest_version = "version=\"" + std::string(expected_version) + "\"";
    if (xml.find(expected_manifest_version) == std::string_view::npos) return false;

    const auto expected = parse_version(expected_version);
    if (!expected) return false;
    const auto& data = pe.bytes();
    constexpr std::array<std::uint8_t, 32> key{
        'V', 0, 'S', 0, '_', 0, 'V', 0, 'E', 0, 'R', 0, 'S', 0, 'I', 0,
        'O', 0, 'N', 0, '_', 0, 'I', 0, 'N', 0, 'F', 0, 'O', 0, 0, 0};
    const std::size_t root = version->offset;
    const std::size_t root_length = u16(data, root);
    if (root_length > version->size || root_length < 6 + key.size() + 52 ||
        u16(data, root + 2) != 52 || u16(data, root + 4) != 0 ||
        !range_ok(root + 6, key.size(), root + root_length) ||
        !std::equal(key.begin(), key.end(), data.begin() + static_cast<std::ptrdiff_t>(root + 6))) return false;
    const std::size_t aligned = (root + 6 + key.size() + 3U) & ~std::size_t{3U};
    if (!range_ok(aligned, 52, root + root_length)) return false;
    if (u32(data, aligned) != 0xfeef04bdU) return false;
    const std::uint32_t file_ms = u32(data, aligned + 8);
    const std::uint32_t file_ls = u32(data, aligned + 12);
    return (file_ms >> 16U) == (*expected)[0] && (file_ms & 0xffffU) == (*expected)[1] &&
        (file_ls >> 16U) == (*expected)[2] && (file_ls & 0xffffU) == (*expected)[3];
}

} // namespace devlink_probe
