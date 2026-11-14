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
};

struct PeerInfo {
    ParticipantId id{};
    std::string name;
    std::uint8_t state = 0;          // ParticipantState encodé sur un octet.
    std::uint64_t connected_ms = 0;
    std::uint32_t latency_ms = 0;
    bool self = false;
};

struct SessionInfo {
    SessionId session{};
    std::string session_name;
    std::string host_name;
    std::uint8_t state = 0;          // SessionState encodé sur un octet.
    std::uint32_t max_participants = 0;
    std::uint64_t started_ms = 0;
    std::vector<PeerInfo> peers;
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

struct ErrorNotice {
    ErrorCode reason = ErrorCode::Ok;
    std::string text;
};

struct ByeNotice {
    ErrorCode reason = ErrorCode::Ok;
    std::string text;
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
std::vector<std::uint8_t> encode(const ByeNotice&);
std::vector<std::uint8_t> encode(const ErrorNotice&);
std::vector<std::uint8_t> encode(const Heartbeat&);

Result<GuestHello> decode_guest_hello(std::span<const std::uint8_t>);
Result<HostAuth> decode_host_auth(std::span<const std::uint8_t>);
Result<SessionInfo> decode_session_info(std::span<const std::uint8_t>);
Result<ServiceList> decode_service_list(std::span<const std::uint8_t>);
Result<ChatText> decode_chat(std::span<const std::uint8_t>);
Result<ByeNotice> decode_bye(std::span<const std::uint8_t>);
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
