#include "mac.hpp"
#include "digest.hpp"
#include <array>

namespace plpt {
namespace {

Result<std::vector<std::uint8_t>> sha256(std::span<const std::uint8_t> data) {
    auto out = digest(DigestAlgorithm::Sha256, data);
    if (!out) return Result<std::vector<std::uint8_t>>::failure(out.error());
    if (out.value().size() != kHmacSha256Size)
        return Result<std::vector<std::uint8_t>>::failure(
            Error{ErrorCode::Internal, "empreinte SHA-256 de taille inattendue", "mac", 0});
    return Result<std::vector<std::uint8_t>>::success(std::move(out).value());
}

} // namespace

Result<std::vector<std::uint8_t>> hmac_sha256(std::span<const std::uint8_t> key,
    std::span<const std::uint8_t> message) {
    // RFC 2104 : K' = K si |K| <= B, sinon K' = H(K).
    std::array<std::uint8_t, kHmacSha256BlockSize> block{};
    if (key.size() > kHmacSha256BlockSize) {
        auto hashed = sha256(key);
        if (!hashed) return Result<std::vector<std::uint8_t>>::failure(hashed.error());
        for (std::size_t index = 0; index < hashed.value().size(); ++index) block[index] = hashed.value()[index];
    } else {
        for (std::size_t index = 0; index < key.size(); ++index) block[index] = key[index];
    }

    std::array<std::uint8_t, kHmacSha256BlockSize> inner{};
    std::array<std::uint8_t, kHmacSha256BlockSize> outer{};
    for (std::size_t index = 0; index < kHmacSha256BlockSize; ++index) {
        inner[index] = static_cast<std::uint8_t>(block[index] ^ 0x36);
        outer[index] = static_cast<std::uint8_t>(block[index] ^ 0x5c);
    }

    std::vector<std::uint8_t> staged;
    staged.reserve(kHmacSha256BlockSize + message.size());
    staged.insert(staged.end(), inner.begin(), inner.end());
    staged.insert(staged.end(), message.begin(), message.end());
    auto inner_digest = sha256(staged);
    if (!inner_digest) return Result<std::vector<std::uint8_t>>::failure(inner_digest.error());

    staged.clear();
    staged.reserve(kHmacSha256BlockSize + kHmacSha256Size);
    staged.insert(staged.end(), outer.begin(), outer.end());
    staged.insert(staged.end(), inner_digest.value().begin(), inner_digest.value().end());
    return sha256(staged);
}

Result<std::vector<std::uint8_t>> hmac_sha256(std::string_view key, std::string_view message) {
    return hmac_sha256({reinterpret_cast<const std::uint8_t*>(key.data()), key.size()},
        {reinterpret_cast<const std::uint8_t*>(message.data()), message.size()});
}

bool mac_equals(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right) {
    return constant_time_equals(left, right);
}

} // namespace plpt
