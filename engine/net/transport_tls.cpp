// DevLink — engine/net/transport_tls.cpp
// Transport TLS 1.3 réel (mandat §24) : OpenSSL, bibliothèque auditée, chargée à
// l'exécution (dlopen / LoadLibrary). Aucune crypto maison : la poignée de
// main, l'AEAD et la dérivation de clés TLS sont celles d'OpenSSL ; nous ne
// fournissons que le secret partagé hors-bande (code d'invitation / reprise),
// transformé en PSK par SHA-256 standard (EVP_Q_digest) avec un libellé de
// domaine versionné.
//
// Authentification mutuelle : TLS 1.3 PSK. Le client prouve qu'il connaît le
// secret de session, le serveur prouve la même chose en répondant à l'identité
// PSK attendue ; un pair sans le secret échoue à la poignée de main, et le
// canal refusé n'est JAMAIS rendu comme connecté (pas de repli silencieux).
//
// Chargement : libssl.so.3 puis libssl.so (Linux) ; sous Windows aucune
// bibliothèque n'est encore câblée (Schannel ou OpenSSL MSYS2 : ADR-016), la
// fabrique se déclare alors indisponible avec la raison exacte. Un chemin
// explicite DEVLINK_TLS_LIBRARY_PATH (variable de TEST) force la bibliothèque
// chargée, ce qui permet de prouver le chemin « indisponible » sans mentir.
#include "transport.hpp"
#include "transport_socket.hpp"

#include <array>
#include <cstring>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace plpt::net {
namespace {

// Types opaques OpenSSL : nous ne décrivons JAMAIS leur contenu, seuls les
// pointeurs circulent (ABI respectée, aucun accès hors contrat).
struct SslContext;
struct SslObject;
struct SslSession;
struct SslCipher;
struct EvpMd;

using PskUseSessionCb = int (*)(SslObject*, const EvpMd*, const unsigned char**,
    std::size_t*, SslSession**);
using PskFindSessionCb = int (*)(SslObject*, const unsigned char*, std::size_t,
    SslSession**);

// Table des symboles OpenSSL requis. Toute absence = fabrique indisponible,
// raison explicite : jamais de fonction inventée, jamais de repli clair.
struct OpenSslApi {
    bool loaded = false;
    std::string failure;
    void* ssl_handle = nullptr;
    void* crypto_handle = nullptr;

    const void* (*tls_client_method)() = nullptr;
    const void* (*tls_server_method)() = nullptr;
    SslContext* (*ssl_ctx_new)(const void*) = nullptr;
    void (*ssl_ctx_free)(SslContext*) = nullptr;
    // SSL_CTX_set_min/max_proto_version sont des MACROS enveloppant
    // SSL_CTX_ctrl (include/openssl/ssl.h.in, openssl-3.0.20) : on lie donc
    // SSL_CTX_ctrl avec les constantes de contrôle publiées 123/124.
    long (*ssl_ctx_ctrl)(SslContext*, int, long, void*) = nullptr;
    void (*ssl_ctx_set_psk_use_session_callback)(SslContext*, PskUseSessionCb) = nullptr;
    void (*ssl_ctx_set_psk_find_session_callback)(SslContext*, PskFindSessionCb) = nullptr;
    SslObject* (*ssl_new)(SslContext*) = nullptr;
    void (*ssl_free)(SslObject*) = nullptr;
    int (*ssl_set_fd)(SslObject*, int) = nullptr;
    int (*ssl_connect)(SslObject*) = nullptr;
    int (*ssl_accept)(SslObject*) = nullptr;
    int (*ssl_read)(SslObject*, void*, int) = nullptr;
    int (*ssl_write)(SslObject*, const void*, int) = nullptr;
    int (*ssl_shutdown)(SslObject*) = nullptr;
    int (*ssl_get_error)(const SslObject*, int) = nullptr;
    const char* (*ssl_get_version)(const SslObject*) = nullptr;
    // SSL_get_cipher_name est une macro : SSL_CIPHER_get_name(SSL_get_current_cipher(s)).
    const SslCipher* (*ssl_get_current_cipher)(const SslObject*) = nullptr;
    const char* (*ssl_cipher_get_name)(const SslCipher*) = nullptr;
    const SslCipher* (*ssl_cipher_find)(const SslObject*, const unsigned char*) = nullptr;
    SslSession* (*ssl_session_new)() = nullptr;
    void (*ssl_session_free)(SslSession*) = nullptr;
    int (*ssl_session_set1_id)(SslSession*, const unsigned char*, std::size_t) = nullptr;
    int (*ssl_session_set_protocol_version)(SslSession*, int) = nullptr;
    int (*ssl_session_set_cipher)(SslSession*, const SslCipher*) = nullptr;
    int (*ssl_session_set1_master_key)(SslSession*, const unsigned char*, std::size_t) = nullptr;
    unsigned long (*err_get_error)() = nullptr;
    void (*err_error_string_n)(unsigned long, char*, std::size_t) = nullptr;
    int (*ssl_ctx_set_ciphersuites)(SslContext*, const char*) = nullptr;
    int (*evp_md_is_a)(const EvpMd*, const char*) = nullptr;
    int (*evp_q_digest)(void*, const char*, const char*, const void*, std::size_t,
        unsigned char*, std::size_t*) = nullptr;
};

#ifndef _WIN32
void* bind_symbol(void* handle, const char* name, OpenSslApi& api) {
    void* symbol = dlsym(handle, name);
    if (symbol == nullptr && api.failure.empty()) {
        api.failure = std::string("symbole OpenSSL manquant : ") + name;
    }
    return symbol;
}
#endif

const OpenSslApi& openssl_api() {
    static OpenSslApi api = [] {
        OpenSslApi loaded;
#ifdef _WIN32
        loaded.failure = "runtime TLS non cable sur Windows (Schannel/OpenSSL MSYS2 : ADR-016)";
        return loaded;
#else
        const char* override_path = std::getenv("DEVLINK_TLS_LIBRARY_PATH");
        const char* ssl_names[] = {override_path != nullptr ? override_path : "libssl.so.3",
            override_path != nullptr ? override_path : "libssl.so"};
        const char* crypto_names[] = {"libcrypto.so.3", "libcrypto.so"};
        const int attempts = override_path != nullptr ? 1 : 2;
        for (int i = 0; i < attempts && loaded.ssl_handle == nullptr; ++i) {
            loaded.ssl_handle = dlopen(ssl_names[i], RTLD_NOW | RTLD_LOCAL);
        }
        if (loaded.ssl_handle == nullptr) {
            loaded.failure = std::string("libssl introuvable (dlopen) : ") + dlerror();
            return loaded;
        }
        for (int i = 0; i < 2 && loaded.crypto_handle == nullptr; ++i) {
            loaded.crypto_handle = dlopen(crypto_names[i], RTLD_NOW | RTLD_LOCAL);
        }
        if (loaded.crypto_handle == nullptr) {
            loaded.failure = std::string("libcrypto introuvable (dlopen) : ") + dlerror();
            return loaded;
        }
#define DEVLINK_BIND(field, name, handle) \
    loaded.field = reinterpret_cast<decltype(loaded.field)>(bind_symbol(handle, name, loaded))
        DEVLINK_BIND(tls_client_method, "TLS_client_method", loaded.ssl_handle);
        DEVLINK_BIND(tls_server_method, "TLS_server_method", loaded.ssl_handle);
        DEVLINK_BIND(ssl_ctx_new, "SSL_CTX_new", loaded.ssl_handle);
        DEVLINK_BIND(ssl_ctx_free, "SSL_CTX_free", loaded.ssl_handle);
        DEVLINK_BIND(ssl_ctx_ctrl, "SSL_CTX_ctrl", loaded.ssl_handle);
        DEVLINK_BIND(ssl_ctx_set_ciphersuites, "SSL_CTX_set_ciphersuites", loaded.ssl_handle);
        DEVLINK_BIND(ssl_ctx_set_psk_use_session_callback, "SSL_CTX_set_psk_use_session_callback", loaded.ssl_handle);
        DEVLINK_BIND(ssl_ctx_set_psk_find_session_callback, "SSL_CTX_set_psk_find_session_callback", loaded.ssl_handle);
        DEVLINK_BIND(ssl_new, "SSL_new", loaded.ssl_handle);
        DEVLINK_BIND(ssl_free, "SSL_free", loaded.ssl_handle);
        DEVLINK_BIND(ssl_set_fd, "SSL_set_fd", loaded.ssl_handle);
        DEVLINK_BIND(ssl_connect, "SSL_connect", loaded.ssl_handle);
        DEVLINK_BIND(ssl_accept, "SSL_accept", loaded.ssl_handle);
        DEVLINK_BIND(ssl_read, "SSL_read", loaded.ssl_handle);
        DEVLINK_BIND(ssl_write, "SSL_write", loaded.ssl_handle);
        DEVLINK_BIND(ssl_shutdown, "SSL_shutdown", loaded.ssl_handle);
        DEVLINK_BIND(ssl_get_error, "SSL_get_error", loaded.ssl_handle);
        DEVLINK_BIND(ssl_get_version, "SSL_get_version", loaded.ssl_handle);
        DEVLINK_BIND(ssl_get_current_cipher, "SSL_get_current_cipher", loaded.ssl_handle);
        DEVLINK_BIND(ssl_cipher_get_name, "SSL_CIPHER_get_name", loaded.ssl_handle);
        DEVLINK_BIND(ssl_cipher_find, "SSL_CIPHER_find", loaded.ssl_handle);
        DEVLINK_BIND(ssl_session_new, "SSL_SESSION_new", loaded.ssl_handle);
        DEVLINK_BIND(ssl_session_free, "SSL_SESSION_free", loaded.ssl_handle);
        DEVLINK_BIND(ssl_session_set1_id, "SSL_SESSION_set1_id", loaded.ssl_handle);
        DEVLINK_BIND(ssl_session_set_protocol_version, "SSL_SESSION_set_protocol_version", loaded.ssl_handle);
        DEVLINK_BIND(ssl_session_set_cipher, "SSL_SESSION_set_cipher", loaded.ssl_handle);
        DEVLINK_BIND(ssl_session_set1_master_key, "SSL_SESSION_set1_master_key", loaded.ssl_handle);
        DEVLINK_BIND(err_get_error, "ERR_get_error", loaded.crypto_handle);
        DEVLINK_BIND(err_error_string_n, "ERR_error_string_n", loaded.crypto_handle);
        DEVLINK_BIND(evp_md_is_a, "EVP_MD_is_a", loaded.crypto_handle);
        DEVLINK_BIND(evp_q_digest, "EVP_Q_digest", loaded.crypto_handle);
#undef DEVLINK_BIND
        if (!loaded.failure.empty()) return loaded;
        loaded.loaded = true;
        return loaded;
#endif
    }();
    return api;
}

// Constantes de contrôle SSL_CTX_ctrl (valeurs publiées par ssl.h.in 3.0.20).
constexpr int kSslCtrlSetMinProtoVersion = 123;
constexpr int kSslCtrlSetMaxProtoVersion = 124;

bool set_ctx_proto_range(const OpenSslApi& api, SslContext* context,
    std::uint16_t min_version, std::uint16_t max_version) {
    return api.ssl_ctx_ctrl(context, kSslCtrlSetMinProtoVersion, min_version, nullptr) != 0 &&
        api.ssl_ctx_ctrl(context, kSslCtrlSetMaxProtoVersion, max_version, nullptr) != 0;
}

// Une seule suite TLS 1.3, épinglée des DEUX côtés (0x1301). Sans cet épinglage,
// OpenSSL sélectionne sa suite par défaut (AES-256-GCM-SHA384) dont le digest ne
// correspond pas à la session PSK SHA-256 : le serveur la JETTE alors et retombe
// sur une poignée certificats (« no suitable signature algorithm »), vérifié par
// test. C'est aussi l'exigence ADR-016 : pas de négociation de suite, pas de repli.
constexpr const char* kPinnedCipherSuite = "TLS_AES_128_GCM_SHA256";

std::string openssl_last_error(const OpenSslApi& api) {
    if (api.err_get_error == nullptr || api.err_error_string_n == nullptr) return "erreur OpenSSL inconnue";
    char buffer[256] = {};
    const unsigned long code = api.err_get_error();
    if (code == 0) return "erreur OpenSSL inconnue";
    api.err_error_string_n(code, buffer, sizeof(buffer));
    return std::string(buffer);
}

// Secret de session -> clé PSK TLS 1.3 (32 octets, SHA-256 via OpenSSL).
// Libellé de domaine versionné : un secret d'une autre génération de protocole
// ne produit jamais la même clé.
Result<std::array<std::uint8_t, 32>> derive_psk(const OpenSslApi& api,
    const TransportCredentials& credentials) {
    const std::string material = "DevLink TLS13 PSK v1|" + credentials.identity + "|" + credentials.shared_secret;
    std::array<std::uint8_t, 32> key{};
    std::size_t out_len = 0;
    if (api.evp_q_digest(nullptr, "SHA256", nullptr, material.data(), material.size(),
            key.data(), &out_len) != 1 || out_len != key.size()) {
        return Result<std::array<std::uint8_t, 32>>::failure(
            Error{ErrorCode::Internal, "EVP_Q_digest SHA-256 a echoue : " + openssl_last_error(api), "derive_psk", 0});
    }
    return Result<std::array<std::uint8_t, 32>>::success(key);
}

// Les rappels PSK tournent pendant SSL_connect/SSL_accept, sur LE thread
// appelant (poignée de main synchrone par conception). Le pointeur de secret
// est donc transmis par variable thread-local, posée avant la poignée de main
// et retirée juste après : aucun état global partagé entre sessions.
thread_local const TransportCredentials* g_handshake_credentials = nullptr;
thread_local std::vector<unsigned char> g_psk_identity_buffer;
thread_local std::array<std::uint8_t, 32> g_psk_key{};

void tls_trace(const char* what) {
    if (std::getenv("DEVLINK_TLS_TRACE") != nullptr) std::fprintf(stderr, "[tls-trace] %s\n", what);
}

int psk_use_session_client(SslObject* ssl, const EvpMd* md, const unsigned char** id,
    std::size_t* idlen, SslSession** sess) {
    const OpenSslApi& api = openssl_api();
    const TransportCredentials* credentials = g_handshake_credentials;
    tls_trace("psk_use_session_client appele");
    if (credentials == nullptr || sess == nullptr || id == nullptr || idlen == nullptr) { tls_trace("use_session: contexte absent"); return 0; }
    // OpenSSL indique le digest attendu ; notre suite unique est SHA-256 : si une
    // autre suite était demandee, on refuse plutot que de livrer une session qui
    // serait jetee silencieusement (le bug « no suitable signature algorithm »).
    if (md != nullptr && api.evp_md_is_a(md, "SHA256") != 1) {
        tls_trace("use_session: digest demande != SHA256, refuse");
        return 0;
    }
    g_psk_identity_buffer.assign(credentials->identity.begin(), credentials->identity.end());
    auto key = derive_psk(api, *credentials);
    if (!key) { tls_trace("use_session: derive_psk en echec"); return 0; }
    g_psk_key = key.value();
    SslSession* session = api.ssl_session_new();
    if (session == nullptr) return 0;
    static const unsigned char cipher_id[2] = {0x13, 0x01}; // TLS_AES_128_GCM_SHA256
    const SslCipher* cipher = api.ssl_cipher_find(ssl, cipher_id);
    if (cipher == nullptr ||
        api.ssl_session_set_protocol_version(session, kTls13Version) != 1 ||
        api.ssl_session_set_cipher(session, cipher) != 1 ||
        api.ssl_session_set1_master_key(session, g_psk_key.data(), g_psk_key.size()) != 1 ||
        api.ssl_session_set1_id(session, g_psk_identity_buffer.data(), g_psk_identity_buffer.size()) != 1) {
        tls_trace("use_session: construction de session refusee");
        api.ssl_session_free(session);
        return 0;
    }
    tls_trace("use_session: session PSK fournie");
    *id = g_psk_identity_buffer.data();
    *idlen = g_psk_identity_buffer.size();
    *sess = session;
    return 1;
}

int psk_find_session_server(SslObject* ssl, const unsigned char* identity,
    std::size_t identity_len, SslSession** sess) {
    const OpenSslApi& api = openssl_api();
    const TransportCredentials* credentials = g_handshake_credentials;
    tls_trace("psk_find_session_server appele");
    if (credentials == nullptr || sess == nullptr) { tls_trace("find_session: contexte absent"); return 0; }
    if (identity_len != credentials->identity.size() ||
        std::memcmp(identity, credentials->identity.data(), identity_len) != 0) {
        return 0; // identité inconnue : poignée de main refusée, rien d'autre.
    }
    auto key = derive_psk(api, *credentials);
    if (!key) return 0;
    g_psk_key = key.value();
    SslSession* session = api.ssl_session_new();
    if (session == nullptr) return 0;
    static const unsigned char cipher_id[2] = {0x13, 0x01};
    const SslCipher* cipher = api.ssl_cipher_find(ssl, cipher_id);
    if (cipher == nullptr ||
        api.ssl_session_set_protocol_version(session, kTls13Version) != 1 ||
        api.ssl_session_set_cipher(session, cipher) != 1 ||
        api.ssl_session_set1_master_key(session, g_psk_key.data(), g_psk_key.size()) != 1 ||
        api.ssl_session_set1_id(session, identity, identity_len) != 1) {
        api.ssl_session_free(session);
        return 0;
    }
    *sess = session;
    tls_trace("find_session: session PSK serveur fournie");
    return 1;
}

// Contexte serveur ou client complet (bornes proto, suite épinglée, callback
// PSK du bon côté). Un seul endroit : accept, connect et les deux wrap ne
// peuvent pas diverger sur la politique TLS.
Result<SslContext*> make_tls_context(const OpenSslApi& api, const TlsPolicy& policy, bool server) {
    SslContext* context = api.ssl_ctx_new(server ? api.tls_server_method() : api.tls_client_method());
    if (context == nullptr) {
        return Result<SslContext*>::failure(Error{ErrorCode::Internal,
            std::string("SSL_CTX_new ") + (server ? "serveur" : "client") + " : " + openssl_last_error(api),
            "tls contexte", 0});
    }
    const std::uint16_t min_version = server ? kTls13Version : policy.min_version;
    const std::uint16_t max_version = server ? kTls13Version : policy.max_version;
    if (!set_ctx_proto_range(api, context, min_version, max_version) ||
        api.ssl_ctx_set_ciphersuites(context, kPinnedCipherSuite) != 1) {
        const std::string diagnostic = openssl_last_error(api);
        api.ssl_ctx_free(context);
        return Result<SslContext*>::failure(Error{ErrorCode::Internal,
            std::string("contexte TLS ") + (server ? "serveur" : "client") + " : " + diagnostic,
            "tls contexte", 0});
    }
    if (server) api.ssl_ctx_set_psk_find_session_callback(context, psk_find_session_server);
    else api.ssl_ctx_set_psk_use_session_callback(context, psk_use_session_client);
    return Result<SslContext*>::success(context);
}

Result<std::string> validate_credentials(const TransportCredentials& credentials) {
    if (credentials.identity.empty()) {
        return Result<std::string>::failure(Error{ErrorCode::InvalidArgument, "identite PSK vide", "transport tls", 0});
    }
    if (credentials.shared_secret.size() < 16) {
        return Result<std::string>::failure(Error{ErrorCode::InvalidArgument, "secret hors-bande trop court (< 16 octets)", "transport tls", 0});
    }
    return Result<std::string>::success(credentials.identity);
}

// Valeurs SSL_ERROR_* : constantes ABI stables d'OpenSSL (ssl.h), nommées ici
// pour ne pas dépendre d'en-têtes absents du dépôt.
constexpr int kSslErrorWantRead = 2;
constexpr int kSslErrorWantWrite = 3;
constexpr int kSslErrorZeroReturn = 6;

class TlsChannel final : public ITransportChannel {
public:
    TlsChannel(const OpenSslApi& api, SslContext* context, SslObject* ssl, detail::socket_t fd)
        : api_(&api), context_(context), ssl_(ssl), fd_(fd) {}
    ~TlsChannel() override { close(); }

    Result<std::size_t> send(std::span<const std::uint8_t> data) override {
        if (ssl_ == nullptr) {
            return Result<std::size_t>::failure(Error{ErrorCode::Disconnected, "canal tls ferme", "send tls", 0});
        }
        std::size_t total = 0;
        while (total < data.size()) {
            const int written = api_->ssl_write(ssl_, data.data() + total,
                static_cast<int>(data.size() - total));
            if (written <= 0) {
                const int err = api_->ssl_get_error(ssl_, written);
                if (err == kSslErrorWantRead || err == kSslErrorWantWrite) {
                    // Descripteur non bloquant (contrat v2) : l'écrit part, le
                    // reste suivra au prochain tour de poll.
                    if (total > 0) return Result<std::size_t>::success(total);
                    return Result<std::size_t>::failure(Error{ErrorCode::WouldBlock,
                        "canal tls non bloquant plein", "send tls", err});
                }
                return Result<std::size_t>::failure(Error{ErrorCode::IoError,
                    "SSL_write err=" + std::to_string(err) + " " + openssl_last_error(*api_), "send tls", err});
            }
            total += static_cast<std::size_t>(written);
        }
        return Result<std::size_t>::success(total);
    }

    Result<std::size_t> receive(std::span<std::uint8_t> out) override {
        if (ssl_ == nullptr) {
            return Result<std::size_t>::failure(Error{ErrorCode::Disconnected, "canal tls ferme", "receive tls", 0});
        }
        const int got = api_->ssl_read(ssl_, out.data(), static_cast<int>(out.size()));
        if (got > 0) return Result<std::size_t>::success(static_cast<std::size_t>(got));
        const int err = api_->ssl_get_error(ssl_, got);
        if (err == kSslErrorWantRead || err == kSslErrorWantWrite) {
            return Result<std::size_t>::failure(Error{ErrorCode::WouldBlock,
                "rien a lire sur le canal tls maintenant", "receive tls", err});
        }
        if (got == 0 || err == kSslErrorZeroReturn) {
            return Result<std::size_t>::failure(Error{ErrorCode::Disconnected,
                "pair tls deconnecte (fin propre)", "receive tls", err});
        }
        return Result<std::size_t>::failure(Error{ErrorCode::IoError,
            "SSL_read err=" + std::to_string(err) + " " + openssl_last_error(*api_), "receive tls", err});
    }

    void close() override {
        if (ssl_ != nullptr) {
            api_->ssl_shutdown(ssl_);
            api_->ssl_free(ssl_);
            ssl_ = nullptr;
        }
        if (context_ != nullptr) {
            api_->ssl_ctx_free(context_);
            context_ = nullptr;
        }
        if (fd_ != detail::kInvalidSocket) {
            detail::close_socket(fd_);
            fd_ = detail::kInvalidSocket;
        }
    }

    TransportInfo info() const override {
        TransportInfo info;
        info.kind = TransportKind::Tls13Psk;
        info.interface_version = kTransportInterfaceVersion;
        if (ssl_ != nullptr) {
            const char* version = api_->ssl_get_version(ssl_);
            const SslCipher* current = api_->ssl_get_current_cipher(ssl_);
            const char* cipher = current != nullptr ? api_->ssl_cipher_get_name(current) : nullptr;
            info.protocol = version != nullptr ? version : "";
            info.cipher = cipher != nullptr ? cipher : "";
        }
        info.peer_authenticated = authenticated_;
        return info;
    }

    TransportDescriptor descriptor() const override {
        return fd_ == detail::kInvalidSocket ? TransportDescriptor{-1}
            : static_cast<TransportDescriptor>(fd_);
    }

    void mark_authenticated() { authenticated_ = true; }

private:
    const OpenSslApi* api_;
    SslContext* context_;
    SslObject* ssl_;
    detail::socket_t fd_;
    bool authenticated_ = false;
};

class TlsEndpoint final : public ITransportEndpoint {
public:
    TlsEndpoint(const OpenSslApi& api, detail::socket_t listener, std::uint16_t port,
        TransportCredentials credentials)
        : api_(&api), listener_(listener), port_(port), credentials_(std::move(credentials)) {}
    ~TlsEndpoint() override { close(); }

    Result<std::unique_ptr<ITransportChannel>> accept() override {
        auto accepted = detail::accept_one(listener_);
        if (!accepted) return Result<std::unique_ptr<ITransportChannel>>::failure(accepted.error());
        const detail::socket_t fd = accepted.value();
        auto made = make_tls_context(*api_, TlsPolicy{}, true);
        if (!made) {
            detail::close_socket(fd);
            return Result<std::unique_ptr<ITransportChannel>>::failure(made.error());
        }
        SslContext* context = made.value();
        SslObject* ssl = api_->ssl_new(context);
        if (ssl == nullptr) {
            api_->ssl_ctx_free(context);
            detail::close_socket(fd);
            return Result<std::unique_ptr<ITransportChannel>>::failure(Error{ErrorCode::Internal,
                "SSL_new serveur : " + openssl_last_error(*api_), "accept tls", 0});
        }
        api_->ssl_set_fd(ssl, static_cast<int>(fd));
        g_handshake_credentials = &credentials_;
        const int rc = api_->ssl_accept(ssl);
        const int err = api_->ssl_get_error(ssl, rc);
        g_handshake_credentials = nullptr;
        if (rc != 1) {
            const std::string diagnostic = openssl_last_error(*api_);
            api_->ssl_free(ssl);
            api_->ssl_ctx_free(context);
            detail::close_socket(fd);
            // Un pair sans le secret de session échoue ICI : aucun canal n'est
            // rendu, aucun octet applicatif n'est traité.
            return Result<std::unique_ptr<ITransportChannel>>::failure(Error{ErrorCode::Unauthorized,
                "poignee de main TLS 1.3 refusee : " + diagnostic + " (ssl_error=" + std::to_string(err) + ")",
                "accept tls", err});
        }
        auto channel = std::make_unique<TlsChannel>(*api_, context, ssl, fd);
        channel->mark_authenticated();
        return Result<std::unique_ptr<ITransportChannel>>::success(std::move(channel));
    }

    std::uint16_t port() const override { return port_; }

    void close() override {
        if (listener_ != detail::kInvalidSocket) {
            detail::close_socket(listener_);
            listener_ = detail::kInvalidSocket;
        }
    }

private:
    const OpenSslApi* api_;
    detail::socket_t listener_;
    std::uint16_t port_;
    TransportCredentials credentials_;
};

class TlsFactory final : public ITransportFactory {
public:
    explicit TlsFactory(TlsPolicy policy) : policy_(policy) {}

    std::uint32_t interface_version() const override { return kTransportInterfaceVersion; }
    TransportKind kind() const override { return TransportKind::Tls13Psk; }
    bool available() const override { return openssl_api().loaded; }
    std::string_view unavailability_reason() const override { return openssl_api().failure; }

    Result<std::unique_ptr<ITransportEndpoint>> listen(
        const std::string& bind_address, const TransportCredentials& credentials) override {
        const OpenSslApi& api = openssl_api();
        if (!api.loaded) {
            return Result<std::unique_ptr<ITransportEndpoint>>::failure(
                Error{ErrorCode::Unsupported, api.failure, "tls listen", 0});
        }
        auto checked = validate_credentials(credentials);
        if (!checked) return Result<std::unique_ptr<ITransportEndpoint>>::failure(checked.error());
        std::uint16_t port = 0;
        auto listener = detail::make_tcp_listener(bind_address, &port);
        if (!listener) return Result<std::unique_ptr<ITransportEndpoint>>::failure(listener.error());
        return Result<std::unique_ptr<ITransportEndpoint>>::success(
            std::make_unique<TlsEndpoint>(api, listener.value(), port, credentials));
    }

    Result<std::unique_ptr<ITransportChannel>> connect(
        const std::string& address, std::uint16_t port,
        const TransportCredentials& credentials) override {
        const OpenSslApi& api = openssl_api();
        if (!api.loaded) {
            return Result<std::unique_ptr<ITransportChannel>>::failure(
                Error{ErrorCode::Unsupported, api.failure, "tls connect", 0});
        }
        auto checked = validate_credentials(credentials);
        if (!checked) return Result<std::unique_ptr<ITransportChannel>>::failure(checked.error());
        auto fd = detail::make_tcp_connection(address, port);
        if (!fd) return Result<std::unique_ptr<ITransportChannel>>::failure(fd.error());
        auto made = make_tls_context(api, policy_, false);
        if (!made) {
            detail::close_socket(fd.value());
            return Result<std::unique_ptr<ITransportChannel>>::failure(made.error());
        }
        SslContext* context = made.value();
        SslObject* ssl = api.ssl_new(context);
        if (ssl == nullptr) {
            api.ssl_ctx_free(context);
            detail::close_socket(fd.value());
            return Result<std::unique_ptr<ITransportChannel>>::failure(Error{ErrorCode::Internal,
                "SSL_new client : " + openssl_last_error(api), "tls connect", 0});
        }
        api.ssl_set_fd(ssl, static_cast<int>(fd.value()));
        g_handshake_credentials = &credentials;
        const int rc = api.ssl_connect(ssl);
        const int err = api.ssl_get_error(ssl, rc);
        g_handshake_credentials = nullptr;
        if (rc != 1) {
            const std::string diagnostic = openssl_last_error(api);
            api.ssl_free(ssl);
            api.ssl_ctx_free(context);
            detail::close_socket(fd.value());
            return Result<std::unique_ptr<ITransportChannel>>::failure(Error{
                err == kSslErrorWantRead || err == kSslErrorWantWrite
                    ? ErrorCode::Disconnected : ErrorCode::Unauthorized,
                "poignee de main TLS 1.3 client refusee : " + diagnostic + " (ssl_error=" + std::to_string(err) + ")",
                "tls connect", err});
        }
        auto channel = std::make_unique<TlsChannel>(api, context, ssl, fd.value());
        channel->mark_authenticated();
        return Result<std::unique_ptr<ITransportChannel>>::success(std::move(channel));
    }

    Result<std::unique_ptr<ITransportChannel>> wrap_server(
        TransportDescriptor connected, const TransportCredentials& credentials,
        std::uint32_t handshake_timeout_ms) override {
        return wrap_handshake(connected, credentials, handshake_timeout_ms, true);
    }

    Result<std::unique_ptr<ITransportChannel>> wrap_client(
        TransportDescriptor connected, const TransportCredentials& credentials,
        std::uint32_t handshake_timeout_ms) override {
        return wrap_handshake(connected, credentials, handshake_timeout_ms, false);
    }

private:
    // Upgrade d'une connexion TCP déjà établie (basculement étape 5) : poignée
    // de main bornée par délais socket bloquants temporaires, puis canal NON
    // bloquant. Le descripteur est toujours pris en charge (fermé) en cas
    // d'échec : aucun double close chez l'appelant.
    Result<std::unique_ptr<ITransportChannel>> wrap_handshake(
        TransportDescriptor connected, const TransportCredentials& credentials,
        std::uint32_t handshake_timeout_ms, bool server) const {
        const OpenSslApi& api = openssl_api();
        if (!api.loaded) {
            return Result<std::unique_ptr<ITransportChannel>>::failure(
                Error{ErrorCode::Unsupported, api.failure, "tls wrap", 0});
        }
        auto checked = validate_credentials(credentials);
        if (!checked) return Result<std::unique_ptr<ITransportChannel>>::failure(checked.error());
        if (connected < 0) {
            return Result<std::unique_ptr<ITransportChannel>>::failure(
                Error{ErrorCode::InvalidArgument, "descripteur invalide a adopter", "tls wrap", 0});
        }
        const detail::socket_t fd = static_cast<detail::socket_t>(connected);
        const std::uint32_t timeout = handshake_timeout_ms == 0 ? 5000 : handshake_timeout_ms;
        if (!detail::set_socket_timeouts_ms(fd, timeout)) {
            detail::close_socket(fd);
            return Result<std::unique_ptr<ITransportChannel>>::failure(Error{ErrorCode::PlatformError,
                detail::socket_error_text(detail::socket_error()), "tls wrap delais", 0});
        }
        auto made = make_tls_context(api, policy_, server);
        if (!made) {
            detail::close_socket(fd);
            return Result<std::unique_ptr<ITransportChannel>>::failure(made.error());
        }
        SslContext* context = made.value();
        SslObject* ssl = api.ssl_new(context);
        if (ssl == nullptr) {
            const std::string diagnostic = openssl_last_error(api);
            api.ssl_ctx_free(context);
            detail::close_socket(fd);
            return Result<std::unique_ptr<ITransportChannel>>::failure(Error{ErrorCode::Internal,
                "SSL_new upgrade : " + diagnostic, "tls wrap", 0});
        }
        api.ssl_set_fd(ssl, static_cast<int>(fd));
        g_handshake_credentials = &credentials;
        const int rc = server ? api.ssl_accept(ssl) : api.ssl_connect(ssl);
        const int err = api.ssl_get_error(ssl, rc);
        g_handshake_credentials = nullptr;
        if (rc != 1) {
            const std::string diagnostic = openssl_last_error(api);
            api.ssl_free(ssl);
            api.ssl_ctx_free(context);
            detail::close_socket(fd);
            return Result<std::unique_ptr<ITransportChannel>>::failure(Error{ErrorCode::Unauthorized,
                "poignee de main TLS 1.3 d'upgrade refusee : " + diagnostic +
                    " (ssl_error=" + std::to_string(err) + ")",
                server ? "tls wrap serveur" : "tls wrap client", err});
        }
        detail::set_socket_timeouts_ms(fd, 0);
        if (!detail::set_socket_nonblocking(fd, true)) {
            api.ssl_free(ssl);
            api.ssl_ctx_free(context);
            detail::close_socket(fd);
            return Result<std::unique_ptr<ITransportChannel>>::failure(Error{ErrorCode::PlatformError,
                detail::socket_error_text(detail::socket_error()), "tls wrap non bloquant", 0});
        }
        auto channel = std::make_unique<TlsChannel>(api, context, ssl, fd);
        channel->mark_authenticated();
        return Result<std::unique_ptr<ITransportChannel>>::success(std::move(channel));
    }

    TlsPolicy policy_;
};

} // namespace

Result<std::unique_ptr<ITransportFactory>> make_tls13_factory(const TlsPolicy& policy) {
    if (policy.min_version > policy.max_version ||
        policy.min_version < kTls12Version || policy.max_version > kTls13Version) {
        return Result<std::unique_ptr<ITransportFactory>>::failure(Error{ErrorCode::InvalidArgument,
            "politique TLS hors bornes (min=" + std::to_string(policy.min_version) +
                " max=" + std::to_string(policy.max_version) + ")", "make_tls13_factory", 0});
    }
    return Result<std::unique_ptr<ITransportFactory>>::success(std::make_unique<TlsFactory>(policy));
}

bool tls13_runtime_available(std::string* reason) {
    const OpenSslApi& api = openssl_api();
    if (reason != nullptr) *reason = api.failure;
    return api.loaded;
}

} // namespace plpt::net
