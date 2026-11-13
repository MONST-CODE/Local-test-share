// PLPT — engine/core/secrets.cpp — codes d'invitation et jetons de service.
#include "secrets.hpp"

#include <algorithm>
#include <span>
#include <vector>
#include "digest.hpp"
#include "random.hpp"

namespace plpt {
namespace {
// Base32 Crockford : pas de I, L, O, U. Un code dictable au téléphone reste lisible.
constexpr std::string_view kAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

int decode_char(char raw) {
    auto c = static_cast<unsigned char>(raw);
    if (c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 'a' + 'A');
    if (c == 'I' || c == 'L') c = '1';
    if (c == 'O') c = '0';
    const auto position = kAlphabet.find(static_cast<char>(c));
    return position == std::string_view::npos ? -1 : static_cast<int>(position);
}

Result<std::vector<std::uint8_t>> random_bytes(std::size_t count) {
    SecureRandom source;
    std::vector<std::uint8_t> out(count, 0);
    std::span<std::uint8_t> view{out.data(), out.size()};
    if (!source.fill(view))
        return Result<std::vector<std::uint8_t>>::failure({ErrorCode::PlatformError, "CSPRNG de l'OS indisponible", "secrets"});
    // Un tampon rempli de zéros n'est pas un secret : vérification explicite.
    if (std::all_of(out.begin(), out.end(), [](std::uint8_t byte) { return byte == 0; }))
        return Result<std::vector<std::uint8_t>>::failure({ErrorCode::PlatformError, "CSPRNG a renvoyé des zéros", "secrets"});
    return Result<std::vector<std::uint8_t>>::success(std::move(out));
}
} // namespace

std::string format_invite_code(const std::array<std::uint8_t, kInviteCodeBytes>& bytes) {
    // 80 bits -> 16 symboles base32, exactement, sans bit de poids faible perdu.
    std::uint64_t high = 0;
    for (std::size_t index = 0; index < 5; ++index) high = (high << 8) | bytes[index];
    std::uint64_t low = 0;
    for (std::size_t index = 5; index < kInviteCodeBytes; ++index) low = (low << 8) | bytes[index];
    // 40 bits par moitié = 8 symboles base32 chacune, 16 au total, aucun bit perdu.
    std::string out;
    out.reserve(kInviteCodeChars);
    for (int position = 7; position >= 0; --position) out.push_back(kAlphabet[(high >> (position * 5)) & 0x1FU]);
    for (int position = 7; position >= 0; --position) out.push_back(kAlphabet[(low >> (position * 5)) & 0x1FU]);
    // Le groupement 4-4-4-4 n'est que de la lisibilité.
    std::string grouped;
    grouped.reserve(out.size() + 3);
    for (std::size_t index = 0; index < out.size(); ++index) {
        if (index > 0 && index % 4 == 0) grouped.push_back('-');
        grouped.push_back(out[index]);
    }
    return grouped;
}

Result<std::array<std::uint8_t, kInviteCodeBytes>> decode_invite_code(std::string_view text) {
    std::array<std::uint8_t, kInviteCodeBytes> bytes{};
    std::vector<int> symbols;
    symbols.reserve(kInviteCodeChars);
    for (const char character : text) {
        if (character == '-' || character == ' ' || character == '\t' || character == '_' || character == '.') continue;
        const auto value = decode_char(character);
        if (value < 0)
            return Result<std::array<std::uint8_t, kInviteCodeBytes>>::failure(
                {ErrorCode::InvalidArgument, "caractère hors alphabet base32 Crockford", "secrets"});
        symbols.push_back(value);
        if (symbols.size() > kInviteCodeChars)
            return Result<std::array<std::uint8_t, kInviteCodeBytes>>::failure(
                {ErrorCode::TooLarge, "code plus long que 16 symboles", "secrets"});
    }
    if (symbols.size() != kInviteCodeChars)
        return Result<std::array<std::uint8_t, kInviteCodeBytes>>::failure(
            {ErrorCode::InvalidArgument, "code incomplet : 16 symboles attendus", "secrets"});
    std::uint64_t high = 0;
    std::uint64_t low = 0;
    for (std::size_t index = 0; index < 8; ++index) high = (high << 5) | static_cast<std::uint64_t>(symbols[index]);
    for (std::size_t index = 8; index < kInviteCodeChars; ++index) low = (low << 5) | static_cast<std::uint64_t>(symbols[index]);
    for (std::size_t index = 0; index < 5; ++index)
        bytes[index] = static_cast<std::uint8_t>(high >> ((4 - index) * 8));
    for (std::size_t index = 0; index < 5; ++index)
        bytes[5 + index] = static_cast<std::uint8_t>(low >> ((4 - index) * 8));
    return Result<std::array<std::uint8_t, kInviteCodeBytes>>::success(bytes);
}

Result<InviteCode> generate_invite_code() {
    auto random = random_bytes(kInviteCodeBytes);
    if (!random) return Result<InviteCode>::failure(random.error());
    InviteCode code;
    const auto& value = random.value();
    std::copy(value.begin(), value.end(), code.bytes.begin());
    code.text = format_invite_code(code.bytes);
    // Contrôle de cohérence : le formatage doit être inversible, sinon le code
    // affiché à l'utilisateur ne pourrait jamais être accepté.
    auto decoded = decode_invite_code(code.text);
    if (!decoded || decoded.value() != code.bytes)
        return Result<InviteCode>::failure({ErrorCode::Internal, "formatage du code non inversible", "secrets"});
    return Result<InviteCode>::success(std::move(code));
}

bool invite_code_matches(std::string_view presented, const InviteCode& expected) {
    if (!expected.valid()) return false; // aucun code : aucune session n'est joignable.
    auto decoded = decode_invite_code(presented);
    if (!decoded) return false; // réponse identique pour un code invalide et un code faux.
    const auto& left = decoded.value();
    const auto& right = expected.bytes;
    return constant_time_equals({left.data(), left.size()}, {right.data(), right.size()});
}

std::string mask_secret(std::string_view text) {
    if (text.size() <= 4) return std::string(text.size(), '*');
    return std::string(text.size() - 4, '*') + std::string(text.substr(text.size() - 4));
}

Result<std::string> generate_service_token() {
    auto random = random_bytes(kServiceTokenBytes);
    if (!random) return Result<std::string>::failure(random.error());
    const auto& value = random.value();
    auto token = to_hex({value.data(), value.size()});
    if (token.size() != kServiceTokenChars)
        return Result<std::string>::failure({ErrorCode::Internal, "jeton de service mal formé", "secrets"});
    return Result<std::string>::success(std::move(token));
}

bool service_token_matches(std::string_view presented, std::string_view expected) {
    if (expected.empty() || presented.size() != expected.size()) return false;
    return constant_time_equals(presented, expected);
}

} // namespace plpt
