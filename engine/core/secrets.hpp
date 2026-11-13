#pragma once
// PLPT — engine/core/secrets.hpp — codes d'invitation et jetons de service.
// Entropie : CSPRNG de l'OS uniquement (BCryptGenRandom / getrandom).
// Comparaison : temps constant. Aucun algorithme inventé.
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include "result.hpp"

namespace plpt {

// 10 octets = 80 bits d'entropie : suffisant pour un code dictable, à condition
// de limiter strictement le nombre de tentatives (voir host_server).
inline constexpr std::size_t kInviteCodeBytes = 10;
inline constexpr std::size_t kInviteCodeChars = 16; // base32 Crockford, 4x4.
inline constexpr std::size_t kServiceTokenBytes = 16;
inline constexpr std::size_t kServiceTokenChars = 32; // hexadécimal minuscule.

struct InviteCode {
    std::array<std::uint8_t, kInviteCodeBytes> bytes{};
    std::string text; // « K7QF-2MXA-9TRD-4BCS » : ce que l'utilisateur lit et saisit.
    bool valid() const { return !text.empty(); }
};

Result<InviteCode> generate_invite_code();
// Normalise casse, espaces, tirets et les confusions I/L->1, O->0 avant décodage.
Result<std::array<std::uint8_t, kInviteCodeBytes>> decode_invite_code(std::string_view text);
std::string format_invite_code(const std::array<std::uint8_t, kInviteCodeBytes>& bytes);
// Comparaison à temps constant des codes décodés : jamais strcmp sur un secret.
bool invite_code_matches(std::string_view presented, const InviteCode& expected);
// Masquage pour l'affichage et les journaux : les 4 derniers caractères seulement.
std::string mask_secret(std::string_view text);

// Jeton de capacité d'un service partagé, porté par l'URL et le cookie du proxy.
Result<std::string> generate_service_token();
bool service_token_matches(std::string_view presented, std::string_view expected);

} // namespace plpt
