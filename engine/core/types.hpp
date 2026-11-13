#pragma once
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace plpt {
template <typename Tag, typename Rep> struct Id {
    Rep value = 0;
    auto operator<=>(const Id&) const = default;
};
using SessionId = Id<struct SessionTag, std::uint64_t>;
using ParticipantId = Id<struct ParticipantTag, std::uint64_t>;
using ContextId = Id<struct ContextTag, std::uint64_t>;
using MessageId = Id<struct MessageTag, std::uint64_t>;
using ReportId = Id<struct ReportTag, std::uint64_t>;
using ServiceId = Id<struct ServiceTag, std::uint64_t>;
using ProcessId = Id<struct ProcessTag, std::uint32_t>;
using Port = Id<struct PortTag, std::uint16_t>;
inline constexpr std::uint64_t kInvalidId = 0;

enum class IpFamily : std::uint8_t { Unknown, Ipv4, Ipv6 };
enum class TransportProtocol : std::uint8_t { Unknown, Tcp, Udp };
struct Endpoint {
    IpFamily family = IpFamily::Unknown;
    std::string address;
    Port port{};
    std::uint32_t scope_id = 0; // IPv6 link-local : ne pas confondre les interfaces.
    auto operator<=>(const Endpoint&) const = default;
};
struct ProcessRecord {
    ProcessId pid{};
    ProcessId ppid{};
    std::string image_name;
    std::optional<std::string> executable_path;
    std::uint64_t start_time_ticks = 0; // 0 = identité de création inaccessible.
    bool operator==(const ProcessRecord&) const = default;
};
enum class SocketState : std::uint8_t {
    Unknown, Listening, Bound, Connected, TimeWait, Closed, Established
};
struct SocketRecord {
    TransportProtocol protocol = TransportProtocol::Unknown;
    Endpoint local{};
    Endpoint remote{};
    SocketState state = SocketState::Unknown;
    std::optional<ProcessId> owner_pid; // absent aussi lorsque plusieurs propriétaires sont observés.
    bool operator==(const SocketRecord&) const = default;
};
inline bool is_service_endpoint(const SocketRecord& s) {
    return (s.protocol == TransportProtocol::Tcp && s.state == SocketState::Listening) ||
           (s.protocol == TransportProtocol::Udp && s.state == SocketState::Bound);
}
enum class Confidence : std::uint8_t { Unknown, Low, Medium, High };
inline std::string_view to_string(Confidence c) {
    switch (c) {
    case Confidence::High: return "élevée";
    case Confidence::Medium: return "moyenne";
    case Confidence::Low: return "faible";
    default: return "inconnue";
    }
}
enum class ShareState : std::uint8_t {
    Detected, Candidate, Selected, Authorized, Protected, Unavailable, Gone
};
inline std::string_view to_string(ShareState s) {
    switch (s) {
    case ShareState::Detected: return "Détecté";
    case ShareState::Candidate: return "Candidat";
    case ShareState::Selected: return "Sélectionné (local)";
    case ShareState::Authorized: return "Autorisé";
    case ShareState::Protected: return "Protégé";
    case ShareState::Unavailable: return "Indisponible";
    case ShareState::Gone: return "Disparu";
    }
    return "Inconnu";
}
enum class Role : std::uint8_t { Host, IndependentTester, Collaborator, Observer };
enum class Capability : std::uint8_t {
    ViewStream, SendInput, SendChat, SendDm, SubmitFeedback, RequestControl, InviteOthers
};
inline bool role_has_capability(Role role, Capability cap) {
    if (cap > Capability::InviteOthers) return false; // enum non fiable : refus par défaut.
    if (role == Role::Host) return true;
    if (role != Role::IndependentTester && role != Role::Collaborator && role != Role::Observer) return false;
    if (cap == Capability::InviteOthers) return false;
    if (cap == Capability::RequestControl) return role == Role::Collaborator;
    if (cap == Capability::SendInput) return role != Role::Observer;
    return true;
}
} // namespace plpt
