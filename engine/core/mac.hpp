#pragma once
// PLPT — engine/core/mac.hpp — HMAC-SHA256 (RFC 2104) au-dessus du fournisseur
// d'empreintes de l'OS (engine/core/digest.hpp : CNG / AF_ALG / libcrypto).
// Aucun algorithme maison : la construction HMAC est celle de la RFC, la
// fonction de compression vient du fournisseur. Utilisé pour la preuve
// d'authentification de l'étape 5 (ADR-017) : prouver la connaissance d'un
// secret SANS le transmettre, et dériver le secret de reprise TLS 1.3.
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "result.hpp"

namespace plpt {

// Longueur de bloc de SHA-256 (RFC 2104, §2) : ipad/opad sur 64 octets.
inline constexpr std::size_t kHmacSha256BlockSize = 64;
inline constexpr std::size_t kHmacSha256Size = 32;

// HMAC-SHA256 standard : clé plus courte complétée, clé plus longue hachée.
Result<std::vector<std::uint8_t>> hmac_sha256(std::span<const std::uint8_t> key,
    std::span<const std::uint8_t> message);
Result<std::vector<std::uint8_t>> hmac_sha256(std::string_view key, std::string_view message);

// Comparaison à temps constant des preuves : jamais de comparaison octet à octet
// qui s'arrête à la première différence sur un MAC.
bool mac_equals(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right);

} // namespace plpt
