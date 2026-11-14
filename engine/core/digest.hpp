#pragma once
// PLPT — engine/core/digest.hpp — empreintes et encodages via le fournisseur
// cryptographique de l'OS. Aucun algorithme maison : CNG sous Windows, API
// crypto du noyau Linux (AF_ALG) puis libcrypto chargé dynamiquement.
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "result.hpp"

namespace plpt {

enum class DigestAlgorithm : std::uint8_t { Sha256, Sha1 };

struct DigestProvider {
    bool available = false;
    std::string name;      // nom exact du fournisseur utilisé, jamais deviné.
    std::string mechanism; // BCrypt / AF_ALG / dlopen EVP.
};

// Résultat mis en cache : la détection ne se répète pas à chaque empreinte.
DigestProvider digest_provider();
std::size_t digest_size(DigestAlgorithm);
Result<std::vector<std::uint8_t>> digest(DigestAlgorithm, std::span<const std::uint8_t> data);

std::string to_hex(std::span<const std::uint8_t> data);
std::string to_base64(std::span<const std::uint8_t> data);

// Comparaison à temps constant : jamais strcmp sur un secret.
bool constant_time_equals(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right);
bool constant_time_equals(std::string_view left, std::string_view right);

} // namespace plpt
