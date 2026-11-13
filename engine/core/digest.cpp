// PLPT — engine/core/digest.cpp — fournisseurs cryptographiques reconnus.
// Windows : CNG (bcrypt.lib, déjà lié). Linux : AF_ALG (API crypto du noyau),
// sinon libcrypto.so.3 chargé par dlopen. Aucun algorithme réécrit ici.
#include "digest.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__linux__)
#include <dlfcn.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(__has_include)
#if __has_include(<linux/if_alg.h>)
#define PLPT_HAVE_AF_ALG 1
#include <linux/if_alg.h>
#endif
#endif
#endif

namespace plpt {
namespace {

constexpr std::size_t kSha256Size = 32;
constexpr std::size_t kSha1Size = 20;

struct ProviderState {
    std::once_flag flag;
    DigestProvider provider;
};
ProviderState& state() {
    static ProviderState instance;
    return instance;
}

#ifdef _WIN32
const wchar_t* algorithm_name(DigestAlgorithm algorithm) {
    return algorithm == DigestAlgorithm::Sha256 ? BCRYPT_SHA256_ALGORITHM : BCRYPT_SHA1_ALGORITHM;
}
// CNG exige un objet de hachage distinct du fournisseur d'algorithme :
// BCryptOpenAlgorithmProvider -> BCryptCreateHash -> BCryptHashData ->
// BCryptFinishHash. BCryptFinishHash ne rend aucune longueur : la taille est
// celle de l'algorithme, et toute divergence est une erreur, pas un succès.
Result<std::vector<std::uint8_t>> windows_digest(DigestAlgorithm algorithm, std::span<const std::uint8_t> data) {
    BCRYPT_ALG_HANDLE provider_handle = nullptr;
    auto status = ::BCryptOpenAlgorithmProvider(&provider_handle, algorithm_name(algorithm), nullptr, 0);
    if (status < 0 || provider_handle == nullptr)
        return Result<std::vector<std::uint8_t>>::failure(
            {ErrorCode::PlatformError, "BCryptOpenAlgorithmProvider", "digest", static_cast<std::int64_t>(status)});
    BCRYPT_HASH_HANDLE hash_handle = nullptr;
    status = ::BCryptCreateHash(provider_handle, &hash_handle, nullptr, 0, nullptr, 0, 0);
    if (status < 0 || hash_handle == nullptr) {
        ::BCryptCloseAlgorithmProvider(provider_handle, 0);
        return Result<std::vector<std::uint8_t>>::failure(
            {ErrorCode::PlatformError, "BCryptCreateHash", "digest", static_cast<std::int64_t>(status)});
    }
    std::vector<std::uint8_t> out(digest_size(algorithm));
    if (!data.empty()) status = ::BCryptHashData(hash_handle, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0);
    if (status >= 0) status = ::BCryptFinishHash(hash_handle, out.data(), static_cast<ULONG>(out.size()), 0);
    ::BCryptDestroyHash(hash_handle);
    ::BCryptCloseAlgorithmProvider(provider_handle, 0);
    if (status < 0)
        return Result<std::vector<std::uint8_t>>::failure(
            {ErrorCode::PlatformError, "BCryptHashData/BCryptFinishHash", "digest", static_cast<std::int64_t>(status)});
    return Result<std::vector<std::uint8_t>>::success(std::move(out));
}
#elif defined(__linux__)
#ifdef PLPT_HAVE_AF_ALG
const char* alg_name(DigestAlgorithm algorithm) { return algorithm == DigestAlgorithm::Sha256 ? "sha256" : "sha1"; }
// L'API crypto du noyau : aucune bibliothèque, aucun code maison.
bool af_alg_digest(DigestAlgorithm algorithm, std::span<const std::uint8_t> data, std::vector<std::uint8_t>& out) {
    const auto tfm = ::socket(AF_ALG, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (tfm < 0) return false;
    sockaddr_alg request{};
    request.salg_family = AF_ALG;
    std::snprintf(reinterpret_cast<char*>(request.salg_type), sizeof(request.salg_type), "%s", "hash");
    std::snprintf(reinterpret_cast<char*>(request.salg_name), sizeof(request.salg_name), "%s", alg_name(algorithm));
    if (::bind(tfm, reinterpret_cast<sockaddr*>(&request), sizeof(request)) != 0) { ::close(tfm); return false; }
    const auto operation = ::accept(tfm, nullptr, nullptr);
    ::close(tfm);
    if (operation < 0) return false;
    const auto written = ::send(operation, data.data(), data.size(), MSG_MORE);
    if (written < 0 || static_cast<std::size_t>(written) != data.size()) { ::close(operation); return false; }
    out.assign(digest_size(algorithm), 0);
    const auto produced = ::read(operation, out.data(), out.size());
    ::close(operation);
    if (produced <= 0 || static_cast<std::size_t>(produced) != out.size()) { out.clear(); return false; }
    return true;
}
#endif

// Second fournisseur reconnu : OpenSSL/libcrypto du système, chargé à l'exécution.
// Pas de dépendance de compilation ni d'édition de liens : un hôte sans libcrypto
// perd seulement /ws dans le service de démonstration, jamais le produit.
struct LibCrypto {
    void* handle = nullptr;
    bool probed = false;
    const void* (*sha256)() = nullptr;
    const void* (*sha1)() = nullptr;
    int (*evp_digest)(const void*, std::size_t, unsigned char*, unsigned int*, const void*, void*) = nullptr;
    std::string loaded_name; // nom exact de la bibliothèque réellement ouverte.
};
LibCrypto& libcrypto() {
    static LibCrypto library;
    if (library.probed) return library;
    library.probed = true;
    for (const char* name : {"libcrypto.so.3", "libcrypto.so.1.1", "libcrypto.so"}) {
        void* handle = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) continue;
        auto* evp = reinterpret_cast<decltype(library.evp_digest)>(::dlsym(handle, "EVP_Digest"));
        auto* sha256 = reinterpret_cast<decltype(library.sha256)>(::dlsym(handle, "EVP_sha256"));
        auto* sha1 = reinterpret_cast<decltype(library.sha1)>(::dlsym(handle, "EVP_sha1"));
        if (evp == nullptr || sha256 == nullptr || sha1 == nullptr) { ::dlclose(handle); continue; }
        library.handle = handle;
        library.evp_digest = evp;
        library.sha256 = sha256;
        library.sha1 = sha1;
        library.loaded_name = name;
        return library;
    }
    return library;
}
#endif

void detect() {
    auto& provider = state().provider;
#ifdef _WIN32
    BCRYPT_ALG_HANDLE handle = nullptr;
    if (::BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && handle != nullptr) {
        ::BCryptCloseAlgorithmProvider(handle, 0);
        provider = DigestProvider{true, "Windows CNG", "BCryptHashData"};
        return;
    }
    provider = DigestProvider{false, "aucun", "CNG indisponible"};
#elif defined(__linux__)
#ifdef PLPT_HAVE_AF_ALG
    std::vector<std::uint8_t> probe;
    const std::array<std::uint8_t, 3> sample{'a', 'b', 'c'};
    if (af_alg_digest(DigestAlgorithm::Sha256, sample, probe) && af_alg_digest(DigestAlgorithm::Sha1, sample, probe)) {
        provider = DigestProvider{true, "noyau Linux", "AF_ALG hash sha256/sha1"};
        return;
    }
#endif
    auto& library = libcrypto();
    if (library.handle != nullptr) {
        provider = DigestProvider{true, library.loaded_name.empty() ? "libcrypto" : library.loaded_name, "dlopen EVP_Digest"};
        return;
    }
    provider = DigestProvider{false, "aucun", "ni AF_ALG ni libcrypto"};
#else
    provider = DigestProvider{false, "aucun", "plateforme non prise en charge"};
#endif
}

constexpr char kHex[] = "0123456789abcdef";
constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

} // namespace

std::size_t digest_size(DigestAlgorithm algorithm) {
    return algorithm == DigestAlgorithm::Sha256 ? kSha256Size : kSha1Size;
}

DigestProvider digest_provider() {
    auto& instance = state();
    std::call_once(instance.flag, detect);
    return instance.provider;
}

Result<std::vector<std::uint8_t>> digest(DigestAlgorithm algorithm, std::span<const std::uint8_t> data) {
    if (!digest_provider().available)
        return Result<std::vector<std::uint8_t>>::failure(
            {ErrorCode::Unsupported, "aucun fournisseur cryptographique OS disponible", "digest"});
#ifdef _WIN32
    return windows_digest(algorithm, data);
#elif defined(__linux__)
#ifdef PLPT_HAVE_AF_ALG
    {
        std::vector<std::uint8_t> kernel_out;
        if (af_alg_digest(algorithm, data, kernel_out)) return Result<std::vector<std::uint8_t>>::success(std::move(kernel_out));
    }
#endif
    auto& library = libcrypto();
    if (library.handle == nullptr)
        return Result<std::vector<std::uint8_t>>::failure({ErrorCode::Unsupported, "libcrypto indisponible", "digest"});
    const void* method = algorithm == DigestAlgorithm::Sha256 ? library.sha256() : library.sha1();
    std::vector<std::uint8_t> out(digest_size(algorithm));
    unsigned int produced = 0;
    if (library.evp_digest(data.data(), data.size(), out.data(), &produced, method, nullptr) != 1 ||
        produced != out.size())
        return Result<std::vector<std::uint8_t>>::failure({ErrorCode::PlatformError, "EVP_Digest", "digest"});
    return Result<std::vector<std::uint8_t>>::success(std::move(out));
#else
    return Result<std::vector<std::uint8_t>>::failure({ErrorCode::Unsupported, "plateforme non prise en charge", "digest"});
#endif
}

std::string to_hex(std::span<const std::uint8_t> data) {
    std::string out;
    out.reserve(data.size() * 2);
    for (const auto byte : data) {
        out.push_back(kHex[(byte >> 4) & 0x0FU]);
        out.push_back(kHex[byte & 0x0FU]);
    }
    return out;
}

std::string to_base64(std::span<const std::uint8_t> data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t index = 0;
    while (index + 2 < data.size()) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(data[index]) << 16) |
            (static_cast<std::uint32_t>(data[index + 1]) << 8) | static_cast<std::uint32_t>(data[index + 2]);
        out.push_back(kBase64[(triple >> 18) & 0x3FU]);
        out.push_back(kBase64[(triple >> 12) & 0x3FU]);
        out.push_back(kBase64[(triple >> 6) & 0x3FU]);
        out.push_back(kBase64[triple & 0x3FU]);
        index += 3;
    }
    const auto remaining = data.size() - index;
    if (remaining == 1) {
        const std::uint32_t value = static_cast<std::uint32_t>(data[index]) << 16;
        out.push_back(kBase64[(value >> 18) & 0x3FU]);
        out.push_back(kBase64[(value >> 12) & 0x3FU]);
        out.push_back('=');
        out.push_back('=');
    } else if (remaining == 2) {
        const std::uint32_t value = (static_cast<std::uint32_t>(data[index]) << 16) |
            (static_cast<std::uint32_t>(data[index + 1]) << 8);
        out.push_back(kBase64[(value >> 18) & 0x3FU]);
        out.push_back(kBase64[(value >> 12) & 0x3FU]);
        out.push_back(kBase64[(value >> 6) & 0x3FU]);
        out.push_back('=');
    }
    return out;
}

bool constant_time_equals(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right) {
    // La longueur diffère : le résultat est faux, mais le temps reste identique
    // pour deux entrées de même taille.
    std::uint8_t difference = left.size() == right.size() ? 0U : 1U;
    const std::size_t common = std::min(left.size(), right.size());
    for (std::size_t index = 0; index < common; ++index) difference |= static_cast<std::uint8_t>(left[index] ^ right[index]);
    return difference == 0;
}

bool constant_time_equals(std::string_view left, std::string_view right) {
    return constant_time_equals({reinterpret_cast<const std::uint8_t*>(left.data()), left.size()},
        {reinterpret_cast<const std::uint8_t*>(right.data()), right.size()});
}

} // namespace plpt
