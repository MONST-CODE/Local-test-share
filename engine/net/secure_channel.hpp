#pragma once
// DevLink — engine/net/secure_channel.hpp
// Canal de transport sécurisé avec authentification mutuelle, chiffrement et intégrité.
// Mandat Trajectoire 2 (§13 Transport sécurisé).
// Primitives OS : Windows CNG (BCrypt) / Linux crypto kernel & OpenSSL.
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <memory>
#include "../core/types.hpp"
#include "../core/result.hpp"

namespace plpt::net {

inline constexpr std::size_t kNonceBytes = 16;
inline constexpr std::size_t kSessionKeyBytes = 32;
inline constexpr std::size_t kMacBytes = 32; // SHA-256 HMAC

struct SecureSessionKeys {
    std::array<std::uint8_t, kSessionKeyBytes> enc_key{};
    std::array<std::uint8_t, kSessionKeyBytes> mac_key{};
};

// En-tête sécurisé protégé contre le rejeu (§13)
struct SecureHeader {
    std::uint32_t magic = 0x444C4E4B; // 'DLNK'
    std::uint16_t version = 2;         // DevLink Protocol v2
    std::uint16_t flags = 0;           // bit 0 = encrypted, bit 1 = control, bit 2 = media
    std::uint64_t sequence = 0;        // Séquence strictement monotone
    std::uint64_t timestamp_ms = 0;    // Horodatage pour fenêtre d'expiration
    std::uint32_t payload_len = 0;     // Longueur des données chiffrées
};
inline constexpr std::size_t kSecureHeaderBytes = 28;

// Dérivation HKDF-Extract & Expand standard (RFC 5869) via HMAC-SHA256 de l'OS
Result<SecureSessionKeys> derive_session_keys(
    std::span<const std::uint8_t> secret_ikm,
    std::span<const std::uint8_t> client_nonce,
    std::span<const std::uint8_t> server_nonce);

// Calcul du HMAC-SHA256 via le fournisseur cryptographique de l'OS
Result<std::array<std::uint8_t, kMacBytes>> compute_hmac_sha256(
    std::span<const std::uint8_t> key,
    std::span<const std::uint8_t> data);

class SecureChannel {
public:
    SecureChannel() = default;
    // Module de MIGRATION uniquement : cette construction maison (XOR avec une
    // sortie HMAC repetee) ne doit PAS etre utilisee en production. Le transport
    // exige par le mandat §24 est TLS 1.3 avec authentification des pairs ; ce
    // canal est conserve le temps de la migration, hors des binaires produits,
    // pour ne pas perdre les tests d'integrite et d'anti-rejeu.
    // Les deux voies d'installation des cles ont la meme semantique : un canal
    // construit avec des cles est etabli, exactement comme apres set_keys().
    explicit SecureChannel(SecureSessionKeys keys, bool is_server)
        : keys_(std::move(keys)), is_server_(is_server), established_(true) {}

    bool is_established() const { return established_; }
    void set_keys(SecureSessionKeys keys, bool is_server) {
        keys_ = keys;
        is_server_ = is_server;
        established_ = true;
    }

    // Scelle un message : chiffre le payload, injecte le compteur monotone, calcule le HMAC
    Result<std::vector<std::uint8_t>> seal(
        std::span<const std::uint8_t> plaintext,
        std::uint16_t flags,
        std::uint64_t now_ms);

    // Ouvre et authentifie un message scellé reçu
    Result<std::vector<std::uint8_t>> unseal(
        std::span<const std::uint8_t> ciphertext,
        std::uint64_t now_ms,
        std::uint64_t max_skew_ms = 10000);

private:
    SecureSessionKeys keys_{};
    bool is_server_ = false;
    bool established_ = false;
    std::uint64_t send_sequence_ = 1;
    std::uint64_t expected_recv_sequence_ = 1;
};

} // namespace plpt::net
