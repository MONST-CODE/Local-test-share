// DevLink — engine/net/secure_channel.cpp
// Implémentation du transport sécurisé DevLink (§13).
#include "secure_channel.hpp"
#include "../core/digest.hpp"
#include <cstring>
#include <algorithm>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <dlfcn.h>
#endif

namespace plpt::net {

Result<std::array<std::uint8_t, kMacBytes>> compute_hmac_sha256(
    std::span<const std::uint8_t> key,
    std::span<const std::uint8_t> data) {
    std::array<std::uint8_t, kMacBytes> out{};

#if defined(_WIN32)
    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!BCRYPT_SUCCESS(status) || !alg) {
        return Result<std::array<std::uint8_t, kMacBytes>>::failure(
            Error{ErrorCode::PlatformError, "échec BCryptOpenAlgorithmProvider HMAC-SHA256", "crypto"});
    }

    BCRYPT_HASH_HANDLE hash = nullptr;
    status = BCryptCreateHash(
        alg, &hash, nullptr, 0,
        const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0);
    if (!BCRYPT_SUCCESS(status) || !hash) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return Result<std::array<std::uint8_t, kMacBytes>>::failure(
            Error{ErrorCode::PlatformError, "échec BCryptCreateHash HMAC-SHA256", "crypto"});
    }

    if (!data.empty()) {
        status = BCryptHashData(
            hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0);
        if (!BCRYPT_SUCCESS(status)) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(alg, 0);
            return Result<std::array<std::uint8_t, kMacBytes>>::failure(
                Error{ErrorCode::PlatformError, "échec BCryptHashData", "crypto"});
        }
    }

    status = BCryptFinishHash(hash, out.data(), static_cast<ULONG>(out.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);

    if (!BCRYPT_SUCCESS(status)) {
        return Result<std::array<std::uint8_t, kMacBytes>>::failure(
            Error{ErrorCode::PlatformError, "échec BCryptFinishHash", "crypto"});
    }
    return Result<std::array<std::uint8_t, kMacBytes>>::success(out);
#else
    // Fallback standard HMAC-SHA256 (RFC 2104) basé sur digest(DigestAlgorithm::Sha256)
    // K_ipad = key XOR 0x36, K_opad = key XOR 0x5c
    std::array<std::uint8_t, 64> k_pad{};
    if (key.size() > 64) {
        auto hashed_key = digest(DigestAlgorithm::Sha256, key);
        if (!hashed_key) return Result<std::array<std::uint8_t, kMacBytes>>::failure(hashed_key.error());
        std::memcpy(k_pad.data(), hashed_key.value().data(), std::min(hashed_key.value().size(), std::size_t(64)));
    } else {
        std::memcpy(k_pad.data(), key.data(), key.size());
    }

    std::vector<std::uint8_t> inner(64 + data.size());
    for (std::size_t i = 0; i < 64; ++i) inner[i] = k_pad[i] ^ 0x36;
    if (!data.empty()) std::memcpy(inner.data() + 64, data.data(), data.size());
    auto inner_hash = digest(DigestAlgorithm::Sha256, inner);
    if (!inner_hash) return Result<std::array<std::uint8_t, kMacBytes>>::failure(inner_hash.error());

    std::vector<std::uint8_t> outer(64 + inner_hash.value().size());
    for (std::size_t i = 0; i < 64; ++i) outer[i] = k_pad[i] ^ 0x5c;
    std::memcpy(outer.data() + 64, inner_hash.value().data(), inner_hash.value().size());
    auto outer_hash = digest(DigestAlgorithm::Sha256, outer);
    if (!outer_hash || outer_hash.value().size() != kMacBytes) {
        return Result<std::array<std::uint8_t, kMacBytes>>::failure(
            Error{ErrorCode::PlatformError, "échec calcul HMAC-SHA256", "crypto"});
    }
    std::memcpy(out.data(), outer_hash.value().data(), kMacBytes);
    return Result<std::array<std::uint8_t, kMacBytes>>::success(out);
#endif
}

Result<SecureSessionKeys> derive_session_keys(
    std::span<const std::uint8_t> secret_ikm,
    std::span<const std::uint8_t> client_nonce,
    std::span<const std::uint8_t> server_nonce) {
    if (secret_ikm.empty()) {
        return Result<SecureSessionKeys>::failure(
            Error{ErrorCode::InvalidArgument, "secret maître IKM vide", "crypto"});
    }

    // Salt = client_nonce || server_nonce
    std::vector<std::uint8_t> salt;
    salt.reserve(client_nonce.size() + server_nonce.size());
    salt.insert(salt.end(), client_nonce.begin(), client_nonce.end());
    salt.insert(salt.end(), server_nonce.begin(), server_nonce.end());

    // 1. HKDF-Extract: PRK = HMAC-Hash(salt, IKM)
    auto prk_res = compute_hmac_sha256(salt, secret_ikm);
    if (!prk_res) return Result<SecureSessionKeys>::failure(prk_res.error());
    const auto& prk = prk_res.value();

    // 2. HKDF-Expand: T(1) = HMAC-Hash(PRK, "DevLink-Enc-Key" || 0x01)
    std::string info_enc = "DevLink-Enc-Key";
    std::vector<std::uint8_t> info_enc_bytes(info_enc.begin(), info_enc.end());
    info_enc_bytes.push_back(0x01);
    auto enc_res = compute_hmac_sha256(prk, info_enc_bytes);
    if (!enc_res) return Result<SecureSessionKeys>::failure(enc_res.error());

    // T(2) = HMAC-Hash(PRK, "DevLink-Mac-Key" || 0x02)
    std::string info_mac = "DevLink-Mac-Key";
    std::vector<std::uint8_t> info_mac_bytes(info_mac.begin(), info_mac.end());
    info_mac_bytes.push_back(0x02);
    auto mac_res = compute_hmac_sha256(prk, info_mac_bytes);
    if (!mac_res) return Result<SecureSessionKeys>::failure(mac_res.error());

    SecureSessionKeys keys;
    keys.enc_key = enc_res.value();
    keys.mac_key = mac_res.value();
    return Result<SecureSessionKeys>::success(keys);
}

Result<std::vector<std::uint8_t>> SecureChannel::seal(
    std::span<const std::uint8_t> plaintext,
    std::uint16_t flags,
    std::uint64_t now_ms) {
    if (!established_) {
        return Result<std::vector<std::uint8_t>>::failure(
            Error{ErrorCode::PermissionDenied, "canal sécurisé non établi", "secure_channel"});
    }

    SecureHeader hdr;
    hdr.magic = 0x444C4E4B;
    hdr.version = 2;
    hdr.flags = flags;
    hdr.sequence = send_sequence_++;
    hdr.timestamp_ms = now_ms;
    hdr.payload_len = static_cast<std::uint32_t>(plaintext.size());

    // Flux chiffré via flux de clé CTR / HMAC dérivé du compteur de séquence
    std::vector<std::uint8_t> seq_bytes(8);
    std::memcpy(seq_bytes.data(), &hdr.sequence, 8);
    auto keystream = compute_hmac_sha256(keys_.enc_key, seq_bytes);
    if (!keystream) return Result<std::vector<std::uint8_t>>::failure(keystream.error());

    std::vector<std::uint8_t> ciphertext(plaintext.size());
    for (std::size_t i = 0; i < plaintext.size(); ++i) {
        ciphertext[i] = plaintext[i] ^ keystream.value()[i % kMacBytes];
    }

    // Sérialisation [Header (28)] + [Ciphertext (N)] + [HMAC (32)]
    std::vector<std::uint8_t> packet(kSecureHeaderBytes + ciphertext.size() + kMacBytes);
    std::memcpy(&packet[0], &hdr.magic, 4);
    std::memcpy(&packet[4], &hdr.version, 2);
    std::memcpy(&packet[6], &hdr.flags, 2);
    std::memcpy(&packet[8], &hdr.sequence, 8);
    std::memcpy(&packet[16], &hdr.timestamp_ms, 8);
    std::memcpy(&packet[24], &hdr.payload_len, 4);

    if (!ciphertext.empty()) {
        std::memcpy(&packet[kSecureHeaderBytes], ciphertext.data(), ciphertext.size());
    }

    // Calcul du HMAC sur Header + Ciphertext
    std::span<const std::uint8_t> authenticated_range(packet.data(), kSecureHeaderBytes + ciphertext.size());
    auto mac_res = compute_hmac_sha256(keys_.mac_key, authenticated_range);
    if (!mac_res) return Result<std::vector<std::uint8_t>>::failure(mac_res.error());

    std::memcpy(&packet[kSecureHeaderBytes + ciphertext.size()], mac_res.value().data(), kMacBytes);
    return Result<std::vector<std::uint8_t>>::success(std::move(packet));
}

Result<std::vector<std::uint8_t>> SecureChannel::unseal(
    std::span<const std::uint8_t> packet,
    std::uint64_t now_ms,
    std::uint64_t max_skew_ms) {
    if (!established_) {
        return Result<std::vector<std::uint8_t>>::failure(
            Error{ErrorCode::PermissionDenied, "canal sécurisé non établi", "secure_channel"});
    }
    if (packet.size() < kSecureHeaderBytes + kMacBytes) {
        return Result<std::vector<std::uint8_t>>::failure(
            Error{ErrorCode::ProtocolViolation, "paquet sécurisé trop court", "secure_channel"});
    }

    SecureHeader hdr;
    std::memcpy(&hdr.magic, &packet[0], 4);
    std::memcpy(&hdr.version, &packet[4], 2);
    std::memcpy(&hdr.flags, &packet[6], 2);
    std::memcpy(&hdr.sequence, &packet[8], 8);
    std::memcpy(&hdr.timestamp_ms, &packet[16], 8);
    std::memcpy(&hdr.payload_len, &packet[24], 4);

    if (hdr.magic != 0x444C4E4B || hdr.version != 2) {
        return Result<std::vector<std::uint8_t>>::failure(
            Error{ErrorCode::ProtocolViolation, "magic ou version de paquet invalide", "secure_channel"});
    }

    // Protection anti-rejeu (§13) : séquence strictement croissante
    if (hdr.sequence < expected_recv_sequence_) {
        return Result<std::vector<std::uint8_t>>::failure(
            Error{ErrorCode::PermissionDenied, "rejet anti-rejeu : numéro de séquence périmé", "secure_channel"});
    }

    // Protection temporelle : écart d'horodatage borné
    if (now_ms > hdr.timestamp_ms && (now_ms - hdr.timestamp_ms) > max_skew_ms) {
        return Result<std::vector<std::uint8_t>>::failure(
            Error{ErrorCode::TimedOut, "paquet expiré", "secure_channel"});
    }

    const std::size_t expected_total = kSecureHeaderBytes + hdr.payload_len + kMacBytes;
    if (packet.size() != expected_total) {
        return Result<std::vector<std::uint8_t>>::failure(
            Error{ErrorCode::ProtocolViolation, "taille de paquet incohérente", "secure_channel"});
    }

    // Vérification de l'intégrité et de l'authenticité par HMAC en temps constant
    std::span<const std::uint8_t> authenticated_range(packet.data(), kSecureHeaderBytes + hdr.payload_len);
    auto mac_res = compute_hmac_sha256(keys_.mac_key, authenticated_range);
    if (!mac_res) return Result<std::vector<std::uint8_t>>::failure(mac_res.error());

    std::span<const std::uint8_t> received_mac(&packet[kSecureHeaderBytes + hdr.payload_len], kMacBytes);
    if (!constant_time_equals(mac_res.value(), received_mac)) {
        return Result<std::vector<std::uint8_t>>::failure(
            Error{ErrorCode::PermissionDenied, "échec d'authentification MAC : message altéré", "secure_channel"});
    }

    // Déchiffrement
    std::vector<std::uint8_t> seq_bytes(8);
    std::memcpy(seq_bytes.data(), &hdr.sequence, 8);
    auto keystream = compute_hmac_sha256(keys_.enc_key, seq_bytes);
    if (!keystream) return Result<std::vector<std::uint8_t>>::failure(keystream.error());

    std::vector<std::uint8_t> plaintext(hdr.payload_len);
    const std::uint8_t* ciphertext_ptr = &packet[kSecureHeaderBytes];
    for (std::size_t i = 0; i < hdr.payload_len; ++i) {
        plaintext[i] = ciphertext_ptr[i] ^ keystream.value()[i % kMacBytes];
    }

    expected_recv_sequence_ = hdr.sequence + 1;
    return Result<std::vector<std::uint8_t>>::success(std::move(plaintext));
}

} // namespace plpt::net
