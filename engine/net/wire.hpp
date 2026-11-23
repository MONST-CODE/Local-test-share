#pragma once
// PLPT — engine/net/wire.hpp — messages du partage LAN sur l'enveloppe existante.
// Toute longueur est bornée AVANT allocation, tout texte est validé, tout octet
// surnuméraire est une violation de protocole : un pair malveillant ne peut ni
// faire allouer 4 Go ni glisser du contenu non imprimé dans l'interface.
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "../core/result.hpp"
#include "../core/types.hpp"
#include "../protocol/envelope.hpp"

namespace plpt::net {

inline constexpr std::size_t kMaxNameBytes = 64;
inline constexpr std::size_t kMaxSecretBytes = 64; // secret de reprise, hexadécimal.
inline constexpr std::size_t kMaxChatBytes = 4096;
inline constexpr std::size_t kMaxReasonBytes = 200;
inline constexpr std::size_t kMaxUrlBytes = 300;
inline constexpr std::size_t kMaxPeers = 64;
inline constexpr std::size_t kMaxServices = 64;
inline constexpr std::size_t kMaxFrameBytes = 64 * 1024;

class PayloadWriter {
public:
    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void boolean(bool value);
    void text(std::string_view value);
    void blob(std::span<const std::uint8_t> value);
    const std::vector<std::uint8_t>& bytes() const { return buffer_; }
    std::vector<std::uint8_t> take() { return std::move(buffer_); }
private:
    std::vector<std::uint8_t> buffer_;
};

class PayloadReader {
public:
    explicit PayloadReader(std::span<const std::uint8_t> data) : data_(data) {}
    Result<std::uint8_t> u8();
    Result<std::uint16_t> u16();
    Result<std::uint32_t> u32();
    Result<std::uint64_t> u64();
    Result<bool> boolean();
    Result<std::string> text(std::size_t max_bytes);
    Result<std::vector<std::uint8_t>> blob(std::size_t max_bytes);
    std::size_t remaining() const { return data_.size() - offset_; }
    bool at_end() const { return offset_ == data_.size(); }
    Result<void> require_end() const;
private:
    Error truncated() const { return {ErrorCode::ProtocolViolation, "charge utile tronquée", "wire"}; }
    std::span<const std::uint8_t> data_;
    std::size_t offset_ = 0;
};

// ---- Messages typed. Un seul aller-retour de sérialisation par type. ----

struct GuestHello {
    std::string guest_name;   // <= kMaxNameBytes, sans caractère de contrôle.
    std::string invite_code;  // <= 32 caractères, jamais journalisé en clair.
    std::string client;       // version lisible du pair, <= kMaxNameBytes.
    // Secret de reprise reçu lors d'une admission précédente : vide à la première
    // connexion. C'est la seule preuve qui rende une place ; le nom n'en est pas une.
    std::string resume_secret; // <= kMaxSecretBytes, jamais journalisé en clair.
};

struct HostAuth {
    bool accepted = false;
    ErrorCode reason = ErrorCode::Ok;
    std::string reason_text;         // message affichable, jamais un secret.
    ParticipantId participant{};     // 0 tant que la demande n'est pas admise.
    std::string host_name;
    std::string session_name;
    bool approval_required = true;   // l'hôte doit cliquer « Autoriser ».
    // Troisième état réel : le code est valide, l'hôte n'a pas encore cliqué.
    // Sans lui, un invité ne peut pas distinguer « refusé » de « en attente ».
    bool awaiting_approval = false;
    // Secret de reprise émis par l'hôte : aléatoire, lié à CETTE place, à durée
    // limitée, révoqué avec elle. Vide si l'hôte n'a pas pu en générer.
    std::string resume_secret;       // <= kMaxSecretBytes, jamais journalisé.
    std::uint32_t resume_ttl_ms = 0; // validité après déconnexion ; 0 = aucune.
};

struct PeerInfo {
    ParticipantId id{};
    std::string name;
    std::uint8_t state = 0;          // ParticipantState encodé sur un octet.
    std::uint64_t connected_ms = 0;
    std::uint32_t latency_ms = 0;
    bool self = false;
    std::string transport;   // réel, par pair : "TLSv1.3 …" ou "tcp clair" (1.3+)
};

struct SessionInfo {
    SessionId session{};
    std::string session_name;
    std::string host_name;
    std::uint8_t state = 0;          // SessionState encodé sur un octet.
    std::uint32_t max_participants = 0;
    std::uint64_t started_ms = 0;
    std::vector<PeerInfo> peers;
    std::string transport;   // chemin de données réel de la session (1.3+)
    // Bloc 1.5 optionnel : identifiant public de l'hôte, pour qu'un invité puisse
    // lui écrire en privé (§4.2 du maître). 0 = absent. Ce n'est pas un secret.
    ParticipantId host_participant{};
};

struct ServiceInfo {
    std::uint64_t key = 0;
    std::string label;
    std::uint16_t exposed_port = 0;
    std::string url;                 // http://adresse:port/?plpt=<jeton> : capacité.
    bool available = true;
};

struct ServiceList {
    std::vector<ServiceInfo> services;
    std::string control_address;     // adresse que l'invité a réellement jointe.
};

struct ChatText {
    ParticipantId sender{};
    std::string sender_name;
    std::string body;                // <= kMaxChatBytes, UTF-8 validé.
    std::uint64_t sent_ms = 0;
    bool system = false;             // true = événement de session, pas un utilisateur.
    ParticipantId recipient{};       // 0 = canal général ; sinon message privé.
};

// Signalement invité -> hôte (protocole 1.5). Bornes vérifiées au décodage.
inline constexpr std::size_t kMaxFeedbackBytes = 2048;
inline constexpr std::size_t kMaxFeedbackContextBytes = 256;
struct FeedbackReport {
    std::uint8_t severity = 1;       // FeedbackSeverity
    std::string description;         // <= kMaxFeedbackBytes, UTF-8 validé
    std::string context;             // <= kMaxFeedbackContextBytes, sans contrôle
    std::uint64_t sent_ms = 0;
};

struct ErrorNotice {
    ErrorCode reason = ErrorCode::Ok;
    std::string text;
};

struct ByeNotice {
    ErrorCode reason = ErrorCode::Ok;
    std::string text;
};

// Autorisation du canal média (protocole 1.4, ADR-018). Émise par l'hôte après
// admission, sur le canal de contrôle chiffré uniquement. `key` est le secret
// PSK TLS 1.3 du canal média de CE participant : jamais journalisé, jamais
// affiché, renouvelé à chaque admission, détruit à la révocation.
struct MediaOffer {
    std::uint16_t port = 0;
    std::string identity;            // publique : devlink-media-s<session>-p<participant>
    std::string key;                 // 64 chiffres hexadécimaux
    std::uint8_t mode = 0;           // share::ShareMode
    std::uint32_t viewport_width = 0;
    std::uint32_t viewport_height = 0;
};

struct Heartbeat {
    std::uint64_t monotonic_ms = 0;
    std::uint64_t echo = 0;          // renvoyé tel quel : mesure de latence réelle.
};

std::vector<std::uint8_t> encode(const GuestHello&);
std::vector<std::uint8_t> encode(const HostAuth&);
std::vector<std::uint8_t> encode(const SessionInfo&);
std::vector<std::uint8_t> encode(const ServiceList&);
std::vector<std::uint8_t> encode(const ChatText&);
std::vector<std::uint8_t> encode(const FeedbackReport&);
Result<FeedbackReport> decode_feedback(std::span<const std::uint8_t>);
std::vector<std::uint8_t> encode(const ByeNotice&);
std::vector<std::uint8_t> encode(const ErrorNotice&);
std::vector<std::uint8_t> encode(const Heartbeat&);
std::vector<std::uint8_t> encode(const MediaOffer&);
Result<MediaOffer> decode_media_offer(std::span<const std::uint8_t>);

Result<GuestHello> decode_guest_hello(std::span<const std::uint8_t>);
Result<HostAuth> decode_host_auth(std::span<const std::uint8_t>);
Result<SessionInfo> decode_session_info(std::span<const std::uint8_t>);
Result<ServiceList> decode_service_list(std::span<const std::uint8_t>);
Result<ChatText> decode_chat(std::span<const std::uint8_t>);
Result<ByeNotice> decode_bye(std::span<const std::uint8_t>);

// ---------------------------------------------------------------------------
// Authentification par défi-réponse (protocole 1.3, ADR-017)
// ---------------------------------------------------------------------------
// Le code d'invitation et le secret de reprise ne transitent JAMAIS en clair.
// L'hôte tire un nonce public ; l'invité prouve la connaissance du secret par
// un HMAC-SHA256 (RFC 2104, empreintes fournies par l'OS : aucun algorithme
// maison) ; puis les deux côtés dérivent INDÉPENDAMMENT le secret de transport
// TLS 1.3. Un observateur du fil ne voit qu'un nonce et une preuve liée à ce
// nonce : rien de réutilisable, rien qui révèle le secret, et le cassage hors
// ligne est hors de portée (80 bits d'entropie minimum, tentatives en ligne
// limitées par l'hôte).
inline constexpr std::size_t kAuthNonceBytes = 32;
inline constexpr std::size_t kAuthProofBytes = 32;

enum class AuthProofKind : std::uint8_t { InviteCode = 0, ResumeSecret = 1 };

struct AuthChallenge { std::vector<std::uint8_t> nonce; };  // hôte -> invité, en clair
// Réponse au défi (invité -> hôte, en clair) : chaque preuve est un HMAC-SHA256
// de 32 octets, vide quand l'invité ne dispose pas du secret correspondant. Au
// moins une des deux est obligatoire. Un invité qui reprend sa place prouve le
// secret de reprise ; s'il ne le peut plus, il prouve le code d'invitation —
// exactement la sémantique de la révision 1.2, sans jamais rien transmettre.
struct AuthProof {
    std::vector<std::uint8_t> invite_proof;
    std::vector<std::uint8_t> resume_proof;
};

Result<std::vector<std::uint8_t>> encode_auth_challenge(const AuthChallenge&);
Result<AuthChallenge> decode_auth_challenge(std::span<const std::uint8_t>);
Result<std::vector<std::uint8_t>> encode_auth_proof(const AuthProof&);
Result<AuthProof> decode_auth_proof(std::span<const std::uint8_t>);

// Message authentifié : nonce || session (8 octets BE) || nature du secret.
std::vector<std::uint8_t> auth_proof_message(std::span<const std::uint8_t> nonce, std::uint64_t session,
    AuthProofKind kind);
Result<std::vector<std::uint8_t>> compute_auth_proof(std::span<const std::uint8_t> secret,
    std::span<const std::uint8_t> nonce, std::uint64_t session, AuthProofKind kind);

// Dernière trame claire avant la poignée de main (ADR-016/017) : elle ne porte
// AUCUN secret, seulement l'identité PSK publique, la nature du secret à
// dériver localement et le nonce du défi déjà vérifié.
struct TlsUpgradeNotice {
    std::string identity;
    AuthProofKind secret_kind = AuthProofKind::InviteCode;
    std::vector<std::uint8_t> nonce;
};
Result<std::vector<std::uint8_t>> encode_tls_upgrade(const TlsUpgradeNotice&);
Result<TlsUpgradeNotice> decode_tls_upgrade(std::span<const std::uint8_t>);

// Secret de transport dérivé LOCALEMENT, jamais transmis : HMAC-SHA256 du
// secret partagé, lié au nonce, à la session et à l'identité PSK (64 chiffres
// hexadécimaux, donc >= 16 octets comme l'exige la fabrique TLS).
Result<std::string> derive_transport_secret(std::span<const std::uint8_t> secret,
    std::span<const std::uint8_t> nonce, std::uint64_t session, const std::string& identity);
Result<ErrorNotice> decode_error(std::span<const std::uint8_t>);
Result<Heartbeat> decode_heartbeat(std::span<const std::uint8_t>);

// Trame complète prête à être écrite sur le fil de contrôle.
Result<std::vector<std::uint8_t>> frame(model::MessageType type, SessionId session, ParticipantId participant,
    std::uint64_t sequence, std::span<const std::uint8_t> payload);

enum class FrameStatus : std::uint8_t { Complete, Incomplete, Invalid };

// Inspecte l'en-tête SANS allouer la charge utile : c'est la garde anti-DoS.
FrameStatus inspect_frame(std::span<const std::uint8_t> buffer, std::size_t& total_bytes, Error& reason);

// Assembleur de flux TCP : accumulateur borné, aucune trame partielle illimitée.
class FrameAssembler {
public:
    explicit FrameAssembler(std::size_t max_frame_bytes = kMaxFrameBytes) : max_frame_bytes_(max_frame_bytes) {}
    Result<void> feed(std::span<const std::uint8_t> data);
    Result<model::Envelope> next();
    bool empty() const { return buffer_.empty(); }
    std::size_t buffered() const { return buffer_.size(); }
    void clear() { buffer_.clear(); }
private:
    std::vector<std::uint8_t> buffer_;
    std::size_t max_frame_bytes_;
};

// Noms de session/invité : UTF-8 valide, aucun caractère de contrôle, borne stricte.
Result<std::string> sanitize_name(std::string_view raw, std::size_t max_bytes);
Result<std::string> sanitize_chat_body(std::string_view raw);

} // namespace plpt::net
