// PLPT — engine/net/proxy.cpp — pont TCP autorisé par jeton de service.
// Le proxy n'interprète pas le trafic : il vérifie la capacité une fois, dans la
// première requête HTTP, puis copie les octets. Un upgrade WebSocket passe donc
// sans aucune connaissance du protocole de frames.
#include "proxy.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <utility>
#include "../core/digest.hpp"
#include "../core/format.hpp"
#include "../core/secrets.hpp"
#include "wire.hpp"   // sanitize_name / kMaxNameBytes : mêmes bornes que le fil de contrôle.

namespace plpt::net {
namespace {
constexpr std::size_t kMaxHeadBytes = 8192;      // au-delà : 431, jamais de tampon infini.
constexpr std::size_t kMaxEvents = 100;
constexpr std::size_t kMaxWarnings = 32;
constexpr std::size_t kMaxHeaderLineBytes = 2048;

bool equals_ignore_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) != std::tolower(static_cast<unsigned char>(right[index]))) return false;
    }
    return true;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

std::size_t find_bytes(std::span<const std::uint8_t> haystack, std::string_view needle, std::size_t from = 0) {
    if (needle.empty() || haystack.size() < needle.size()) return std::string::npos;
    for (std::size_t index = from; index + needle.size() <= haystack.size(); ++index) {
        if (std::memcmp(haystack.data() + index, needle.data(), needle.size()) == 0) return index;
    }
    return std::string::npos;
}

// Cherche name=value dans une liste séparée par `separator` (query ou cookies).
std::string find_key_value(std::string_view list, char separator, std::string_view name) {
    std::size_t start = 0;
    while (start <= list.size()) {
        const auto end = list.find(separator, start);
        const auto item = trim(list.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        const auto equal = item.find('=');
        if (equal != std::string_view::npos && equals_ignore_case(trim(item.substr(0, equal)), name))
            return std::string(trim(item.substr(equal + 1)));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return {};
}

// Retire name=… d'une query : les autres paramètres et leur ordre sont conservés.
std::string redact_query(std::string_view target, std::string_view name) {
    const auto question = target.find('?');
    if (question == std::string_view::npos) return std::string(target);
    const auto query = target.substr(question + 1);
    std::string kept;
    std::size_t start = 0;
    while (start <= query.size()) {
        const auto end = query.find('&', start);
        const auto item = query.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        const auto equal = item.find('=');
        const bool capability = equal != std::string_view::npos && equals_ignore_case(trim(item.substr(0, equal)), name);
        if (!capability) {
            if (!kept.empty()) kept += '&';
            kept += std::string(item);
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    std::string out(target.substr(0, question));
    if (!kept.empty()) {
        out += '?';
        out += kept;
    }
    return out;
}

// Réécrit la ligne de requête sans le paramètre de capacité.
std::string redact_request_line(std::string_view line, std::string_view name) {
    const auto first = line.find(' ');
    const auto last = line.rfind(' ');
    if (first == std::string_view::npos || last == first) return std::string(line);
    std::string out(line.substr(0, first));
    out += ' ';
    out += redact_query(line.substr(first + 1, last - first - 1), name);
    out += ' ';
    out += std::string(line.substr(last + 1));
    return out;
}

// Retire le cookie de capacité ; retourne une liste vide si rien d'autre n'était porté.
std::string redact_cookie_list(std::string_view value, std::string_view name) {
    const auto list = trim(value);
    std::string kept;
    std::size_t start = 0;
    while (start <= list.size()) {
        const auto end = list.find(';', start);
        const auto item = trim(list.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        const auto equal = item.find('=');
        const bool capability = equal != std::string_view::npos && equals_ignore_case(trim(item.substr(0, equal)), name);
        if (!item.empty() && !capability) {
            if (!kept.empty()) kept += "; ";
            kept += std::string(item);
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return kept;
}

void append_bytes(std::vector<std::uint8_t>& out, std::string_view text) {
    for (const char character : text) out.push_back(static_cast<std::uint8_t>(character));
}
} // namespace

std::string extract_token(std::string_view target, std::string_view headers, std::string_view name, bool* via_cookie) {
    if (name.empty()) return {};
    const auto question = target.find('?');
    if (question != std::string_view::npos) {
        auto value = find_key_value(target.substr(question + 1), '&', name);
        if (!value.empty()) return value;
    }
    // En-têtes : ligne par ligne, sans dépendre d'un parseur HTTP complet.
    std::size_t start = 0;
    while (start < headers.size()) {
        const auto end = headers.find("\r\n", start);
        const auto line = headers.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            const auto key = trim(line.substr(0, colon));
            const auto value = trim(line.substr(colon + 1));
            if (equals_ignore_case(key, "cookie")) {
                auto cookie = find_key_value(value, ';', name);
                if (!cookie.empty()) {
                    if (via_cookie != nullptr) *via_cookie = true;
                    return cookie;
                }
            }
        }
        if (end == std::string_view::npos) break;
        start = end + 2;
    }
    return {};
}

Result<HandshakeFacts> parse_http_head(std::span<const std::uint8_t> data, std::size_t max_bytes, std::string_view token_name) {
    const auto limit = max_bytes == 0 ? kMaxHeadBytes : std::min(max_bytes, kMaxHeadBytes);
    const auto terminator = find_bytes(data, "\r\n\r\n");
    if (terminator == std::string::npos) {
        if (data.size() >= limit)
            return Result<HandshakeFacts>::failure({ErrorCode::TooLarge, "en-têtes HTTP hors borne", "proxy"});
        HandshakeFacts partial;
        return Result<HandshakeFacts>::success(partial); // incomplet, pas invalide.
    }
    if (terminator + 4 > limit)
        return Result<HandshakeFacts>::failure({ErrorCode::TooLarge, "en-têtes HTTP hors borne", "proxy"});
    HandshakeFacts facts;
    facts.complete = true;
    facts.consumed = terminator + 4;
    const std::string_view head(reinterpret_cast<const char*>(data.data()), terminator + 2);
    const auto first_line_end = head.find("\r\n");
    if (first_line_end == std::string_view::npos)
        return Result<HandshakeFacts>::failure({ErrorCode::ProtocolViolation, "ligne de requête absente", "proxy"});
    const auto request_line = head.substr(0, first_line_end);
    const auto first_space = request_line.find(' ');
    const auto last_space = request_line.rfind(' ');
    if (first_space == std::string_view::npos || last_space == first_space)
        return Result<HandshakeFacts>::failure({ErrorCode::ProtocolViolation, "ligne de requête malformed", "proxy"});
    facts.method = std::string(request_line.substr(0, first_space));
    facts.target = std::string(request_line.substr(first_space + 1, last_space - first_space - 1));
    facts.version = std::string(request_line.substr(last_space + 1));
    if (facts.method.empty() || facts.target.empty() || facts.target.front() == ' ')
        return Result<HandshakeFacts>::failure({ErrorCode::ProtocolViolation, "requête HTTP invalide", "proxy"});
    const auto headers = head.substr(first_line_end + 2);
    std::size_t start = 0;
    while (start < headers.size()) {
        const auto end = headers.find("\r\n", start);
        const auto line = headers.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (line.size() > kMaxHeaderLineBytes)
            return Result<HandshakeFacts>::failure({ErrorCode::TooLarge, "en-tête HTTP trop long", "proxy"});
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            const auto key = trim(line.substr(0, colon));
            const auto value = trim(line.substr(colon + 1));
            if (equals_ignore_case(key, "upgrade") && equals_ignore_case(value, "websocket")) facts.upgrade_websocket = true;
            if (equals_ignore_case(key, "x-plpt-token") && facts.token.empty()) facts.token = std::string(value);
        }
        if (end == std::string_view::npos) break;
        start = end + 2;
    }
    bool via_cookie = false;
    if (facts.token.empty()) facts.token = extract_token(facts.target, headers, token_name, &via_cookie);
    facts.token_via_cookie = via_cookie;
    return Result<HandshakeFacts>::success(std::move(facts));
}

std::vector<std::uint8_t> http_status_response(std::uint16_t status, std::string_view reason, std::string_view body) {
    std::string text = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) + "\r\n" +
        "Content-Type: text/plain; charset=utf-8\r\n" + "Content-Length: " + std::to_string(body.size()) + "\r\n" +
        "Cache-Control: no-store\r\n" + "Connection: close\r\n" + "\r\n" + std::string(body);
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::vector<std::uint8_t> redact_capability(std::span<const std::uint8_t> head, std::string_view token_name) {
    std::vector<std::uint8_t> out;
    out.reserve(head.size());
    if (token_name.empty() || head.empty()) {
        out.assign(head.begin(), head.end());
        return out;
    }
    const std::string_view text(reinterpret_cast<const char*>(head.data()), head.size());
    std::size_t start = 0;
    bool request_line = true;
    while (start < text.size()) {
        const auto end = text.find("\r\n", start);
        const auto line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        std::string kept;
        bool drop = false;
        if (request_line) {
            request_line = false;
            kept = redact_request_line(line, token_name);
        } else {
            const auto colon = line.find(':');
            if (colon == std::string_view::npos) {
                kept = std::string(line); // ligne de fin d'en-têtes, ou ligne non conforme conservée.
            } else {
                const auto key = trim(line.substr(0, colon));
                const auto value = line.substr(colon + 1);
                if (equals_ignore_case(key, "x-plpt-token")) {
                    drop = true; // l'en-tête dédié ne traverse jamais le proxy.
                } else if (equals_ignore_case(key, "cookie")) {
                    const auto cookies = redact_cookie_list(value, token_name);
                    if (cookies.empty()) drop = true; // cookie vidé : la ligne entière disparaît.
                    else kept = "Cookie: " + cookies;
                } else if (equals_ignore_case(key, "referer")) {
                    // Le navigateur recopie l'URL d'origine, jeton de capacité compris.
                    kept = "Referer: " + redact_query(trim(value), token_name);
                } else {
                    kept = std::string(line);
                }
            }
        }
        if (!drop) {
            append_bytes(out, kept);
            out.push_back(static_cast<std::uint8_t>('\r'));
            out.push_back(static_cast<std::uint8_t>('\n'));
        }
        if (end == std::string_view::npos) break;
        start = end + 2;
    }
    return out;
}

int response_status_code(std::span<const std::uint8_t> head) {
    const std::string_view text(reinterpret_cast<const char*>(head.data()), head.size());
    const auto end = text.find("\r\n");
    const auto line = end == std::string_view::npos ? text : text.substr(0, end);
    if (line.size() < 12 || line.compare(0, 5, "HTTP/") != 0) return 0;
    const auto space = line.find(' ');
    if (space == std::string_view::npos || space + 3 >= line.size()) return 0;
    const auto digits = line.substr(space + 1, 3);
    for (const char character : digits)
        if (character < '0' || character > '9') return 0;
    if (space + 4 < line.size() && line[space + 4] != ' ') return 0; // « 200OK » refusé.
    return static_cast<int>(digits[0] - '0') * 100 + static_cast<int>(digits[1] - '0') * 10 +
        static_cast<int>(digits[2] - '0');
}

std::vector<std::uint8_t> inject_response_header(std::span<const std::uint8_t> response, std::size_t terminator,
    std::string_view line) {
    if (terminator + 2 > response.size()) return {response.begin(), response.end()};
    std::vector<std::uint8_t> out;
    out.reserve(response.size() + line.size() + 2);
    const auto split = response.begin() + static_cast<std::ptrdiff_t>(terminator + 2);
    out.insert(out.end(), response.begin(), split);
    append_bytes(out, line);
    out.push_back(static_cast<std::uint8_t>('\r'));
    out.push_back(static_cast<std::uint8_t>('\n'));
    out.insert(out.end(), split, response.end());
    return out;
}

std::string capability_cookie_line(std::string_view token_name, std::string_view token) {
    std::string line = "Set-Cookie: ";
    line += token_name;
    line += "=";
    line += token;
    // SameSite=Strict : le cookie ne quitte pas le site partagé. HttpOnly : le code
    // de la page ne peut pas le lire. Aucun Max-Age : cookie de session.
    line += "; Path=/; HttpOnly; SameSite=Strict";
    return line;
}

struct ServiceProxy::Stream {
    Socket guest;
    Socket upstream;
    IpAddress peer;
    Port peer_port{};
    enum class Phase : std::uint8_t { AwaitHead, Bridging, Closing } phase = Phase::AwaitHead;
    bool grant_pending = false;              // cookie de capacité à déposer.
    bool grant_done = false;                 // réponse finale déjà traitée.
    bool informational_logged = false;       // un seul événement par flux pour les 1xx.
    std::string grant_cookie_line;           // ligne Set-Cookie à insérer.
    std::vector<std::uint8_t> response_head; // en-têtes de la 1re réponse, bornés.
    std::vector<std::uint8_t> head;             // en-têtes reçus avant décision.
    std::vector<std::uint8_t> to_upstream;      // file bornée invité -> service.
    std::vector<std::uint8_t> to_guest;         // file bornée service -> invité.
    std::size_t to_upstream_sent = 0;
    std::size_t to_guest_sent = 0;
    bool guest_eof = false;
    bool upstream_eof = false;
    bool authorized = false;
    bool websocket = false;
    bool polled_upstream = false;   // l'amont existait-il au moment du poll ?
    std::uint64_t opened_ms = 0;
    std::uint64_t last_activity_ms = 0;
    std::uint64_t close_deadline_ms = 0;
    std::uint64_t bytes_to_guest = 0;
    std::uint64_t bytes_to_upstream = 0;
};

void ServiceProxy::process_response_head(Stream& stream, std::span<const std::uint8_t> received) {
    // Interception jusqu'à la réponse PERTINENTE : un 100 Continue ou un 103 Early
    // Hints précède la réponse finale, et un cookie inséré là ne serait pas honoré
    // par le navigateur. Plusieurs réponses informationnelles peuvent arriver dans
    // le même segment TCP : la boucle les traite toutes, et elle est bornée par les
    // octets effectivement reçus (chaque 1xx est retiré du tampon).
    stream.response_head.insert(stream.response_head.end(), received.begin(), received.end());
    for (;;) {
        const auto terminator = find_bytes(stream.response_head, "\r\n\r\n");
        if (terminator == std::string::npos) {
            if (stream.response_head.size() >= kMaxHeadBytes) {
                // En-têtes de réponse anormalement grands : renoncement explicite, les
                // octets réels sont transmis plutôt que la réponse bloquée.
                stream.to_guest.insert(stream.to_guest.end(), stream.response_head.begin(), stream.response_head.end());
                stream.grant_pending = false;
                stream.grant_done = true;
                ++counters_.grants_failed;
                record_warning({ErrorCode::TooLarge, "cookie de capacité non déposé : en-têtes de réponse hors borne", "proxy"});
                stream.response_head.clear();
                stream.response_head.shrink_to_fit();
            }
            return; // en-têtes incomplets : on attend la suite.
        }
        const auto status = response_status_code({stream.response_head.data(), terminator + 2});
        if (status >= 100 && status <= 199 && status != 101) {
            const auto next_head = stream.response_head.begin() + static_cast<std::ptrdiff_t>(terminator + 4);
            stream.to_guest.insert(stream.to_guest.end(), stream.response_head.begin(), next_head);
            stream.response_head.erase(stream.response_head.begin(), next_head);
            ++counters_.informational_responses;
            if (!stream.informational_logged) {
                stream.informational_logged = true;
                record_event("réponse informationnelle " + std::to_string(status) +
                    " transmise sans cookie de capacité");
            }
            continue; // la réponse pertinente peut suivre dans les mêmes octets.
        }
        if (status >= 200 && status <= 599) {
            const auto modified = inject_response_header(stream.response_head, terminator, stream.grant_cookie_line);
            stream.to_guest.insert(stream.to_guest.end(), modified.begin(), modified.end());
            ++counters_.grants_issued;
            record_event("cookie de capacité déposé dans la réponse " + std::to_string(status) +
                " pour " + stream.peer.text);
        } else {
            // 101 Switching Protocols ou réponse illisible : la réponse n'est jamais
            // modifiée, et le renoncement est dit.
            stream.to_guest.insert(stream.to_guest.end(), stream.response_head.begin(), stream.response_head.end());
            ++counters_.grants_skipped;
            record_warning({ErrorCode::ProtocolViolation,
                status == 101 ? "réponse 101 : aucun cookie de capacité déposé"
                              : "réponse amont illisible : aucun cookie de capacité déposé",
                "proxy"});
        }
        stream.grant_pending = false;
        stream.grant_done = true;
        stream.response_head.clear();
        stream.response_head.shrink_to_fit();
        return;
    }
}

void ServiceProxy::flush_pending_response_head(Stream& stream, const char* reason) {
    if (!stream.grant_pending || stream.grant_done || stream.response_head.empty()) return;
    // Des octets réels ont été reçus mais ne forment pas une réponse complète : ils
    // sont transmis à l'invité plutôt que perdus, et l'échec du dépôt est dit.
    stream.to_guest.insert(stream.to_guest.end(), stream.response_head.begin(), stream.response_head.end());
    stream.grant_pending = false;
    stream.grant_done = true;
    ++counters_.grants_failed;
    record_warning({ErrorCode::ProtocolViolation, std::string("cookie de capacité non déposé : ") + reason, "proxy"});
    stream.response_head.clear();
    stream.response_head.shrink_to_fit();
}

ServiceProxy::ServiceProxy(IClock& clock) : clock_(clock) {}
ServiceProxy::~ServiceProxy() { stop(); }

Result<std::uint16_t> ServiceProxy::start(ProxyConfig config) {
    if (running_.load()) return Result<std::uint16_t>::failure({ErrorCode::InvalidState, "proxy déjà démarré", "proxy"});
    if (config.upstream_port == 0)
        return Result<std::uint16_t>::failure({ErrorCode::InvalidArgument, "port du service local manquant : rien à ponter", "proxy"});
    if (!config.upstream_address.valid())
        return Result<std::uint16_t>::failure({ErrorCode::InvalidArgument, "adresse du service local invalide", "proxy"});
    if (config.token.empty())
        return Result<std::uint16_t>::failure({ErrorCode::InvalidArgument, "jeton de service absent : accès impossible à autoriser", "proxy"});
    if (config.token_name.empty() || config.token_name.size() > 32)
        return Result<std::uint16_t>::failure({ErrorCode::InvalidArgument, "nom de jeton invalide", "proxy"});
    if (config.max_connections == 0 || config.max_connections > 256)
        return Result<std::uint16_t>::failure({ErrorCode::InvalidArgument, "max_connections hors 1..256", "proxy"});
    if (config.max_buffer_bytes < 4096 || config.max_buffer_bytes > 8 * 1024 * 1024)
        return Result<std::uint16_t>::failure({ErrorCode::InvalidArgument, "tampon de pontage hors borne", "proxy"});
    if (config.handshake_timeout_ms == 0 || config.connect_timeout_ms == 0 || config.idle_timeout_ms == 0)
        return Result<std::uint16_t>::failure({ErrorCode::InvalidArgument, "délais du proxy nuls", "proxy"});
    auto label = sanitize_name(config.label, kMaxNameBytes);
    if (!label) return Result<std::uint16_t>::failure(label.error());
    config.label = label.value();

    config_ = std::move(config);
    streams_.clear();
    events_.clear();
    warnings_.clear();
    counters_ = ProxyCounters{};
    revision_ = 0;
    upstream_alive_ = false;
    upstream_checked_ms_ = 0;

    // Adresse d'écoute réellement déterminée : jamais toutes les interfaces par défaut.
    auto bind = resolve_bind_address(config_.bind_address, IpFamily::Ipv4);
    if (!bind) return Result<std::uint16_t>::failure(bind.error());
    bind_ = bind.value();
    config_.bind_address = bind_.address;

    auto created = Socket::create(TransportProtocol::Tcp, config_.bind_address.family);
    if (!created) return Result<std::uint16_t>::failure(created.error());
    listener_ = std::move(created).value();
    if (auto exclusive = listener_.set_exclusive_bind(true); !exclusive) return Result<std::uint16_t>::failure(exclusive.error());
    if (auto reuse = listener_.set_address_reuse(false); !reuse) return Result<std::uint16_t>::failure(reuse.error());
    if (auto bound = listener_.bind(config_.bind_address, Port{config_.exposed_port}); !bound) return Result<std::uint16_t>::failure(bound.error());
    auto port = listener_.bound_port();
    if (!port) return Result<std::uint16_t>::failure(port.error());
    exposed_port_ = port.value().value;
    if (auto listening = listener_.listen(32); !listening) return Result<std::uint16_t>::failure(listening.error());
    if (auto nonblocking = listener_.set_nonblocking(true); !nonblocking) return Result<std::uint16_t>::failure(nonblocking.error());
    if (auto signal = wakeup_.open(); !signal) return Result<std::uint16_t>::failure(signal.error());

    record_event("proxy « " + config_.label + " » en écoute sur le port " + std::to_string(exposed_port_) + " vers " +
        config_.upstream_address.text + ":" + std::to_string(config_.upstream_port));
    record_event("adresse d'écoute " + config_.bind_address.text + " : " + bind_.note);
    if (bind_.any_interface) record_event("exposition sur toutes les interfaces : choix explicite de la configuration");
    if (bind_.loopback_only)
        record_warning({ErrorCode::Unsupported, "aucune interface privée LAN : le service partagé n'est joignable que depuis cette machine", "proxy"});
    running_.store(true);
    publish();
    worker_ = std::jthread([this](std::stop_token token) { run(token); });
    return Result<std::uint16_t>::success(exposed_port_);
}

void ServiceProxy::stop() {
    const bool was_running = running_.load();
    running_.store(false);
    {
        std::lock_guard<std::mutex> guard(mutex_);
        commands_.push_back(Command{CommandKind::Stop});
    }
    wakeup_.notify();
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    for (auto& stream : streams_) {
        stream->guest.close();
        stream->upstream.close();
    }
    streams_.clear();
    listener_.close();
    if (was_running) publish();
}

bool ServiceProxy::enqueue(Command command) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!running_.load() && command.kind != CommandKind::Stop) return false;
        if (commands_.size() >= 64) return false;
        commands_.push_back(command);
    }
    wakeup_.notify();
    return true;
}

bool ServiceProxy::probe_upstream() { return enqueue(Command{CommandKind::ProbeUpstream}); }

std::shared_ptr<const ProxySnapshot> ServiceProxy::snapshot() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return view_ ? view_ : std::make_shared<const ProxySnapshot>();
}

bool ServiceProxy::wait_for(const std::function<bool(const ProxySnapshot&)>& predicate, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> guard(mutex_);
    return published_.wait_for(guard, timeout, [&] { return view_ && predicate(*view_); });
}

void ServiceProxy::record_event(std::string text) {
    events_.push_back(format_clock_utc(clock_.now_wall_ms()) + "  " + std::move(text));
    while (events_.size() > kMaxEvents) events_.pop_front();
    ++revision_;
}

void ServiceProxy::record_warning(Error error) {
    if (warnings_.size() < kMaxWarnings) warnings_.push_back(std::move(error));
    ++revision_;
}

void ServiceProxy::run(std::stop_token token) {
    while (true) {
        drain_commands();
        if (!running_.load() || token.stop_requested()) break;
        accept_pending();
        pump_streams(std::chrono::milliseconds(100));
        apply_timeouts();
        if (revision_ != 0) publish();
    }
    for (auto& stream : streams_) {
        stream->guest.close();
        stream->upstream.close();
    }
    streams_.clear();
    listener_.close();
    record_event("proxy arrêté, " + std::to_string(counters_.closed) + " flux fermés");
    publish();
}

void ServiceProxy::drain_commands() {
    std::deque<Command> pending;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        pending.swap(commands_);
    }
    for (const auto& command : pending) {
        switch (command.kind) {
        case CommandKind::ProbeUpstream: {
            // Une mesure réelle : connexion TCP ouverte puis refermée. Aucune
            // supposition sur la présence du service local.
            auto probe = Socket::create(TransportProtocol::Tcp, config_.upstream_address.family);
            if (!probe) { record_warning(probe.error()); break; }
            auto socket = std::move(probe).value();
            const auto connected = connect_with_timeout(socket, config_.upstream_address, Port{config_.upstream_port},
                std::chrono::milliseconds(config_.connect_timeout_ms));
            const bool alive = connected.has_value();
            socket.close();
            if (alive != upstream_alive_) {
                record_event(alive ? "service local joignable sur le port " + std::to_string(config_.upstream_port)
                                   : "service local injoignable sur le port " + std::to_string(config_.upstream_port));
            }
            upstream_alive_ = alive;
            upstream_checked_ms_ = clock_.now_wall_ms();
            if (!alive) record_warning(connected.error());
            break;
        }
        case CommandKind::Stop:
            running_.store(false);
            record_event("arrêt du proxy demandé");
            break;
        }
    }
}

void ServiceProxy::accept_pending() {
    while (streams_.size() < config_.max_connections) {
        IpAddress peer;
        Port peer_port{};
        auto accepted = listener_.accept(&peer, &peer_port);
        if (!accepted) {
            if (accepted.error().code != ErrorCode::WouldBlock) record_warning(accepted.error());
            return;
        }
        auto stream = std::make_unique<Stream>();
        stream->guest = std::move(accepted).value();
        stream->peer = peer;
        stream->peer_port = peer_port;
        (void)stream->guest.set_nonblocking(true);
        (void)stream->guest.set_no_delay(true);
        const auto now = clock_.now_monotonic_ms();
        stream->opened_ms = now;
        stream->last_activity_ms = now;
        counters_.connections_total++;
        streams_.push_back(std::move(stream));
        ++revision_;
    }
    if (streams_.size() >= config_.max_connections) {
        // Saturation réelle : on accepte encore pour pouvoir répondre 503, sinon
        // l'invité voit une connexion qui pend sans jamais comprendre pourquoi.
        IpAddress peer;
        Port peer_port{};
        auto accepted = listener_.accept(&peer, &peer_port);
        if (accepted) {
            auto socket = std::move(accepted).value();
            const auto body = "service saturé : trop de connexions simultanées";
            const auto response = http_status_response(503, "Service Unavailable", body);
            (void)write_all(socket, response, std::chrono::milliseconds(500));
            socket.close();
            counters_.refused++;
            record_event("connexion refusée (saturation) depuis " + peer.text);
        }
    }
}

bool ServiceProxy::send_status(Stream& stream, std::uint16_t status, std::string_view reason, std::string_view body) {
    const auto response = http_status_response(status, reason, body);
    stream.to_guest.insert(stream.to_guest.end(), response.begin(), response.end());
    stream.phase = Stream::Phase::Closing;
    stream.close_deadline_ms = clock_.now_monotonic_ms() + 1000;
    ++revision_;
    return true;
}

bool ServiceProxy::begin_upstream(Stream& stream) {
    auto created = Socket::create(TransportProtocol::Tcp, config_.upstream_address.family);
    if (!created) { record_warning(created.error()); return false; }
    stream.upstream = std::move(created).value();
    (void)stream.upstream.set_no_delay(true);
    auto connected = connect_with_timeout(stream.upstream, config_.upstream_address, Port{config_.upstream_port},
        std::chrono::milliseconds(config_.connect_timeout_ms));
    if (!connected) {
        stream.upstream.close();
        counters_.upstream_failures++;
        upstream_alive_ = false;
        upstream_checked_ms_ = clock_.now_wall_ms();
        record_warning(connected.error());
        record_event("service local injoignable : 502 renvoyé à " + stream.peer.text);
        return false;
    }
    (void)stream.upstream.set_nonblocking(true);
    upstream_alive_ = true;
    upstream_checked_ms_ = clock_.now_wall_ms();
    return true;
}

void ServiceProxy::pump_streams(std::chrono::milliseconds timeout) {
    if (streams_.empty()) {
        std::vector<PollEntry> idle{PollEntry{listener_.native(), true, false, false, false, false},
            PollEntry{wakeup_.native(), true, false, false, false, false}};
        auto waited = poll_sockets(idle, timeout);
        if (!waited) record_warning(waited.error());
        else if (idle[0].readable) accept_pending();
        else if (idle[1].readable) wakeup_.drain();
        return;
    }
    // Le jeu de poll est figé pour cette itération : les flux acceptés pendant le
    // tour ne sont PAS parcourus ici, sinon les indices débordent du tableau.
    const std::size_t polled_streams = streams_.size();
    std::vector<PollEntry> entries;
    entries.reserve(polled_streams * 2 + 2);
    entries.push_back(PollEntry{listener_.native(), true, false, false, false, false});
    entries.push_back(PollEntry{wakeup_.native(), true, false, false, false, false});
    for (const auto& stream : streams_) {
        const bool guest_read = stream->phase != Stream::Phase::Closing && !stream->guest_eof &&
            (stream->phase == Stream::Phase::AwaitHead ? stream->head.size() < kMaxHeadBytes
                                                       : stream->to_upstream.size() - stream->to_upstream_sent < config_.max_buffer_bytes);
        const bool guest_write = stream->to_guest_sent < stream->to_guest.size();
        entries.push_back(PollEntry{stream->guest.native(), guest_read, guest_write, false, false, false});
        const bool upstream_read = stream->phase == Stream::Phase::Bridging && stream->upstream.valid() && !stream->upstream_eof &&
            stream->to_guest.size() - stream->to_guest_sent < config_.max_buffer_bytes;
        const bool upstream_write = stream->upstream.valid() && stream->to_upstream_sent < stream->to_upstream.size();
        // Un socket invalide est signalé « failed » par poll_sockets. Sans ce
        // garde, un flux dont l'amont vient d'être créé dans cette même itération
        // serait considéré comme mort avant même d'avoir lu la réponse.
        stream->polled_upstream = stream->upstream.valid();
        entries.push_back(PollEntry{stream->polled_upstream ? stream->upstream.native() : wakeup_.native(), upstream_read,
            upstream_write, false, false, false});
    }
    auto waited = poll_sockets(entries, timeout);
    if (!waited) { record_warning(waited.error()); return; }
    if (entries[1].readable) wakeup_.drain();
    std::array<std::uint8_t, 16384> buffer{};
    for (std::size_t index = 0; index < polled_streams; ++index) {
        auto& stream = *streams_[index];
        auto& guest_entry = entries[2 + index * 2];
        auto& upstream_entry = entries[3 + index * 2];
        if (guest_entry.readable) {
            auto received = stream.guest.receive(buffer);
            if (!received) {
                if (received.error().code != ErrorCode::WouldBlock) {
                    stream.guest_eof = true;
                    if (stream.phase == Stream::Phase::AwaitHead) close_stream(stream, "connexion fermée avant requête");
                }
            } else if (received.value() == 0) {
                stream.guest_eof = true;
                if (stream.phase == Stream::Phase::AwaitHead) close_stream(stream, "connexion fermée avant requête");
            } else {
                stream.last_activity_ms = clock_.now_monotonic_ms();
                if (stream.phase == Stream::Phase::AwaitHead) {
                    stream.head.insert(stream.head.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(received.value()));
                    auto parsed = parse_http_head(stream.head, kMaxHeadBytes, config_.token_name);
                    if (!parsed) {
                        counters_.refused++;
                        record_event("requête illisible depuis " + stream.peer.text + " : " + parsed.error().message);
                        send_status(stream, 400, "Bad Request", "requête HTTP illisible");
                    } else if (parsed.value().complete) {
                        counters_.requests++;
                        const auto& facts = parsed.value();
                        if (!service_token_matches(facts.token, config_.token)) {
                            counters_.refused++;
                            record_event("accès refusé depuis " + stream.peer.text + " : jeton " +
                                (facts.token.empty() ? std::string("absent") : mask_secret(facts.token)));
                            send_status(stream, 403, "Forbidden", "jeton d'accès absent ou invalide");
                        } else if (!begin_upstream(stream)) {
                            send_status(stream, 502, "Bad Gateway", "service local injoignable");
                        } else {
                            stream.authorized = true;
                            stream.websocket = facts.upgrade_websocket;
                            if (facts.upgrade_websocket) counters_.websocket_upgrades++;
                            counters_.authorized++;
                            // La capacité PLPT est retirée avant transmission : le service
                            // local ne voit jamais le jeton, ni dans ses journaux ni dans
                            // son historique. Le reste de la requête passe octet à octet.
                            const std::span<const std::uint8_t> authorized_head(stream.head.data(), facts.consumed);
                            const auto forwarded = redact_capability(authorized_head, config_.token_name);
                            stream.to_upstream.insert(stream.to_upstream.end(), forwarded.begin(), forwarded.end());
                            ++counters_.tokens_redacted;
                            if (config_.grant_cookie && !facts.token_via_cookie && !facts.upgrade_websocket) {
                                // Le navigateur rejouera la capacité en cookie sur les
                                // sous-ressources, sans la répéter dans les URL.
                                stream.grant_pending = true;
                                stream.grant_cookie_line = capability_cookie_line(config_.token_name, config_.token);
                            }
                            // Le corps éventuel déjà reçu suit immédiatement.
                            if (stream.head.size() > facts.consumed)
                                stream.to_upstream.insert(stream.to_upstream.end(), stream.head.begin() + static_cast<std::ptrdiff_t>(facts.consumed),
                                    stream.head.end());
                            stream.head.clear();
                            stream.head.shrink_to_fit();
                            stream.phase = Stream::Phase::Bridging;
                            ++revision_;
                        }
                    }
                } else if (stream.phase == Stream::Phase::Bridging) {
                    stream.to_upstream.insert(stream.to_upstream.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(received.value()));
                }
            }
        }
        if (stream.polled_upstream && upstream_entry.readable && stream.phase == Stream::Phase::Bridging) {
            auto received = stream.upstream.receive(buffer);
            if (!received) {
                if (received.error().code != ErrorCode::WouldBlock) {
                    stream.upstream_eof = true;
                    flush_pending_response_head(stream, "service local fermé avant une réponse complète");
                }
            } else if (received.value() == 0) {
                stream.upstream_eof = true;
                flush_pending_response_head(stream, "service local fermé avant une réponse complète");
            } else {
                stream.last_activity_ms = clock_.now_monotonic_ms();
                if (stream.grant_pending && !stream.grant_done) {
                    process_response_head(stream, {buffer.data(), received.value()});
                } else {
                    const auto slice_end = buffer.begin() + static_cast<std::ptrdiff_t>(received.value());
                    stream.to_guest.insert(stream.to_guest.end(), buffer.begin(), slice_end);
                }
            }
        }
        if (guest_entry.failed) { stream.guest_eof = true; close_stream(stream, "erreur de socket invité"); }
        if (stream.polled_upstream && upstream_entry.failed && stream.upstream.valid()) {
            stream.upstream_eof = true;
            flush_pending_response_head(stream, "erreur de socket du service local");
        }
        if (guest_entry.writable && stream.to_guest_sent < stream.to_guest.size()) {
            auto sent = stream.guest.send({stream.to_guest.data() + stream.to_guest_sent, stream.to_guest.size() - stream.to_guest_sent});
            if (sent) {
                stream.to_guest_sent += sent.value();
                stream.bytes_to_guest += sent.value();
                counters_.bytes_to_guest += sent.value();
                stream.last_activity_ms = clock_.now_monotonic_ms();
            } else if (sent.error().code != ErrorCode::WouldBlock) {
                stream.guest_eof = true;
            }
        }
        if (stream.polled_upstream && upstream_entry.writable && stream.upstream.valid() && stream.to_upstream_sent < stream.to_upstream.size()) {
            auto sent = stream.upstream.send({stream.to_upstream.data() + stream.to_upstream_sent, stream.to_upstream.size() - stream.to_upstream_sent});
            if (sent) {
                stream.to_upstream_sent += sent.value();
                stream.bytes_to_upstream += sent.value();
                counters_.bytes_to_upstream += sent.value();
                stream.last_activity_ms = clock_.now_monotonic_ms();
            } else if (sent.error().code != ErrorCode::WouldBlock) {
                stream.upstream_eof = true;
            }
        }
        // Fin de flux : une fois les files vidées, les deux sockets sont fermées.
        const bool drained_to_guest = stream.to_guest_sent >= stream.to_guest.size();
        const bool drained_to_upstream = stream.to_upstream_sent >= stream.to_upstream.size();
        if (stream.phase == Stream::Phase::Bridging && drained_to_guest && drained_to_upstream && (stream.guest_eof || stream.upstream_eof))
            close_stream(stream, stream.guest_eof ? "invité déconnecté" : "service local déconnecté");
        if (stream.phase == Stream::Phase::Closing && drained_to_guest) {
            stream.guest.close();
            stream.upstream.close();
            counters_.closed++;
        }
    }
    streams_.erase(std::remove_if(streams_.begin(), streams_.end(),
        [](const std::unique_ptr<Stream>& stream) { return !stream->guest.valid() && !stream->upstream.valid(); }), streams_.end());
    // Les connexions entrantes sont traitées APRÈS le pompage : jamais au milieu
    // d'un tableau d'entrées de poll déjà construit.
    if (entries[0].readable) accept_pending();
}

void ServiceProxy::close_stream(Stream& stream, const char* reason) {
    if (stream.phase == Stream::Phase::Closing) return;
    stream.phase = Stream::Phase::Closing;
    stream.close_deadline_ms = clock_.now_monotonic_ms() + 1000;
    if (stream.authorized) {
        record_event(std::string("flux fermé (") + reason + ") : " + format_bytes(stream.bytes_to_upstream) + " vers le service, " +
            format_bytes(stream.bytes_to_guest) + " vers l'invité" + (stream.websocket ? " [websocket]" : ""));
    }
    ++revision_;
}

void ServiceProxy::apply_timeouts() {
    const auto now = clock_.now_monotonic_ms();
    for (auto& stream : streams_) {
        switch (stream->phase) {
        case Stream::Phase::AwaitHead:
            if (now - stream->opened_ms > config_.handshake_timeout_ms) {
                counters_.idle_timeouts++;
                counters_.refused++;
                send_status(*stream, 408, "Request Timeout", "aucune requête dans le délai");
            }
            break;
        case Stream::Phase::Bridging:
            if (now - stream->last_activity_ms > config_.idle_timeout_ms) {
                counters_.idle_timeouts++;
                close_stream(*stream, "flux inactif au-delà du délai");
            }
            break;
        case Stream::Phase::Closing:
            if (now >= stream->close_deadline_ms) {
                stream->guest.close();
                stream->upstream.close();
                counters_.closed++;
            }
            break;
        }
    }
    streams_.erase(std::remove_if(streams_.begin(), streams_.end(),
        [](const std::unique_ptr<Stream>& stream) { return !stream->guest.valid() && !stream->upstream.valid(); }), streams_.end());
}

void ServiceProxy::publish() {
    auto snapshot = std::make_shared<ProxySnapshot>();
    snapshot->running = running_.load();
    snapshot->revision = revision_;
    snapshot->label = config_.label;
    snapshot->bind_address = config_.bind_address.text;
    snapshot->bind_interface = bind_.interface_name;
    snapshot->bind_note = bind_.note;
    snapshot->any_interface = bind_.any_interface;
    snapshot->loopback_only = bind_.loopback_only;
    snapshot->exposed_port = exposed_port_;
    snapshot->upstream = config_.upstream_address.text + ":" + std::to_string(config_.upstream_port);
    snapshot->upstream_alive = upstream_alive_;
    snapshot->upstream_checked_ms = upstream_checked_ms_;
    snapshot->counters = counters_;
    snapshot->events.assign(events_.begin(), events_.end());
    snapshot->warnings = warnings_;
    std::size_t active = 0;
    for (const auto& stream : streams_) if (stream->phase != Stream::Phase::Closing) ++active;
    snapshot->active_connections = active;
    // L'URL publiée dépend de l'adresse d'écoute réelle : 0.0.0.0 n'est pas
    // joignable, on affiche donc l'adresse LAN que l'invité doit saisir.
    const auto host_address = config_.bind_address.is_any() ? std::string("127.0.0.1") : config_.bind_address.text;
    snapshot->url = "http://" + host_address + ":" + std::to_string(exposed_port_) + "/?" + config_.token_name + "=" + config_.token;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        view_ = std::move(snapshot);
    }
    published_.notify_all();
}

} // namespace plpt::net
