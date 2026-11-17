// DevLink — app/demo_service/main.cpp — service local de démonstration.
// Rôle : donner au proxy un amont RÉEL à ponter, pour qualifier le partage de
// bout en bout. HTTP/1.1 minimal + WebSocket RFC 6455 (poignée de main, écho de
// trames texte, ping/pong, fermeture). Il n'écoute que sur loopback : seule la
// passerelle DevLink, avec son jeton, le rend joignable depuis le LAN.
//
// Usage :
//   devlink-demo-service [--port <num>] [--host <adresse>] [--quiet] [--once]
// Le port réel est imprimé sur stdout (« port=<num> »), 0 demandant un port
// éphémère. Codes de sortie : 0 arrêt propre, 2 usage invalide, 3 échec réseau.
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include "../../engine/core/digest.hpp"
#include "../../engine/platform/connection_pool.hpp"
#include "../../engine/platform/socket.hpp"

namespace {
using namespace plpt;
using namespace std::chrono_literals;

constexpr std::size_t kMaxConnections = 8;  // connexions ACTIVES simultanées.
constexpr std::size_t kMaxQueued = 16;      // tâches EN ATTENTE au-delà des actives.
constexpr std::size_t kMaxHeadBytes = 8192;
// Délais bornés côté service : une connexion muette ne doit pas pouvoir occuper un
// ouvrier indéfiniment et épuiser la capacité annoncée. Le délai d'inactivité
// WebSocket est aligné sur idle_timeout_ms du proxy (120 s), pas sur la requête.
constexpr std::chrono::milliseconds kHeadTimeout{10000};
constexpr std::chrono::milliseconds kWriteTimeout{5000};
constexpr std::chrono::milliseconds kWebSocketIdleTimeout{120000};
constexpr std::size_t kMaxFrameBytes = 64 * 1024;
constexpr char kWebSocketGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::atomic<bool> g_stop{false};
std::atomic<unsigned> g_requests{0};
std::atomic<unsigned> g_websockets{0};
bool g_quiet = false;

void log_line(const std::string& text) {
    if (g_quiet) return;
    std::printf("%s\n", text.c_str());
    std::fflush(stdout);
}

std::string http_response(std::uint16_t status, std::string_view reason, std::string_view content_type, std::string_view body,
    std::string_view extra_headers = {}) {
    std::string text = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) + "\r\n" + "Content-Type: " +
        std::string(content_type) + "\r\n" + "Content-Length: " + std::to_string(body.size()) + "\r\n" +
        "Cache-Control: no-store\r\n";
    if (!extra_headers.empty()) text += std::string(extra_headers) + "\r\n";
    text += "Connection: close\r\n\r\n";
    text += body;
    return text;
}

std::string index_page(std::string_view target) {
    std::string body = "<!doctype html>\n<html lang=\"fr\">\n<head>\n<meta charset=\"utf-8\">\n"
                       "<title>Service de démonstration DevLink</title>\n</head>\n<body>\n"
                       "<h1>Service de démonstration DevLink</h1>\n"
                       "<p>Cette page est servie par le service local, puis pontée par la passerelle DevLink.</p>\n"
                       "<ul>\n<li><a href=\"/api/status\">/api/status</a> — état JSON</li>\n"
                       "<li>/ws — écho WebSocket</li>\n</ul>\n"
                       "<p>Requête observée : <code>" +
        std::string(target) + "</code></p>\n</body>\n</html>\n";
    return body;
}

std::string status_json() {
    return "{\"service\":\"plpt-demo\",\"requests\":" + std::to_string(g_requests.load()) + ",\"websockets\":" +
        std::to_string(g_websockets.load()) + ",\"protocol\":\"HTTP/1.1+RFC6455\"}";
}

// ---- WebSocket RFC 6455 : poignée de main puis trames. ----

bool websocket_accept(const std::string& head, std::string& accept_value) {
    std::size_t start = 0;
    while (start < head.size()) {
        const auto end = head.find("\r\n", start);
        const auto line = head.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            auto key = line.substr(0, colon);
            auto value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
            for (auto& character : key) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            if (key == "sec-websocket-key") {
                // Poignée de main RFC 6455 : SHA-1 de la clé + GUID, en base64.
                const std::string material = value + kWebSocketGuid;
                const auto hashed = digest(DigestAlgorithm::Sha1,
                    std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(material.data()), material.size()});
                if (!hashed) return false;
                const auto& bytes = hashed.value();
                accept_value = to_base64(std::span<const std::uint8_t>{bytes.data(), bytes.size()});
                return !accept_value.empty();
            }
        }
        if (end == std::string::npos) break;
        start = end + 2;
    }
    return false;
}

std::string websocket_frame(std::uint8_t opcode, std::span<const std::uint8_t> payload) {
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode)); // FIN + opcode.
    const auto size = payload.size();
    if (size < 126) frame.push_back(static_cast<char>(size));
    else if (size <= 0xFFFF) {
        frame.push_back(static_cast<char>(126));
        frame.push_back(static_cast<char>((size >> 8) & 0xFF));
        frame.push_back(static_cast<char>(size & 0xFF));
    } else {
        frame.push_back(static_cast<char>(127));
        for (int shift = 56; shift >= 0; shift -= 8) frame.push_back(static_cast<char>((size >> shift) & 0xFF));
    }
    frame.append(reinterpret_cast<const char*>(payload.data()), payload.size()); // serveur : jamais masqué.
    return frame;
}

// Lit exactement `count` octets ; false si la connexion se ferme avant.
bool read_exact(Socket& socket, std::vector<std::uint8_t>& out, std::size_t count) {
    out.resize(out.size() + count);
    std::size_t filled = 0;
    while (filled < count) {
        auto received = socket.receive({out.data() + out.size() - count + filled, count - filled});
        if (!received || received.value() == 0) {
            out.resize(out.size() - count + filled);
            return false;
        }
        filled += received.value();
    }
    return true;
}

void serve_websocket(Socket& socket) {
    g_websockets++;
    while (!g_stop.load()) {
        std::vector<std::uint8_t> header;
        if (!read_exact(socket, header, 2)) break;
        const std::uint8_t opcode = static_cast<std::uint8_t>(header[0] & 0x0F);
        const bool masked = (header[1] & 0x80) != 0;
        std::uint64_t length = header[1] & 0x7F;
        if (length == 126) {
            std::vector<std::uint8_t> extended;
            if (!read_exact(socket, extended, 2)) break;
            length = (static_cast<std::uint64_t>(extended[0]) << 8) | extended[1];
        } else if (length == 127) {
            std::vector<std::uint8_t> extended;
            if (!read_exact(socket, extended, 8)) break;
            length = 0;
            for (const auto byte : extended) length = (length << 8) | byte;
        }
        if (length > kMaxFrameBytes) { // borne stricte : jamais d'allocation aveugle.
            const auto close = websocket_frame(8, std::span<const std::uint8_t>{});
            (void)write_all(socket, {reinterpret_cast<const std::uint8_t*>(close.data()), close.size()}, 2000ms);
            break;
        }
        std::uint8_t mask[4] = {0, 0, 0, 0};
        if (masked) {
            std::vector<std::uint8_t> key;
            if (!read_exact(socket, key, 4)) break;
            std::memcpy(mask, key.data(), 4);
        }
        std::vector<std::uint8_t> payload;
        if (length > 0 && !read_exact(socket, payload, static_cast<std::size_t>(length))) break;
        if (masked) for (std::size_t index = 0; index < payload.size(); ++index) payload[index] ^= mask[index % 4];
        if (opcode == 8) {
            const auto close = websocket_frame(8, payload.size() >= 2 ? std::span<const std::uint8_t>{payload.data(), 2} : std::span<const std::uint8_t>{});
            (void)write_all(socket, {reinterpret_cast<const std::uint8_t*>(close.data()), close.size()}, 2000ms);
            break;
        }
        if (opcode == 9) { // ping -> pong avec la même charge utile.
            const auto pong = websocket_frame(10, payload);
            (void)write_all(socket, {reinterpret_cast<const std::uint8_t*>(pong.data()), pong.size()}, 2000ms);
            continue;
        }
        if (opcode == 1 || opcode == 2) { // texte ou binaire : écho exact.
            const auto reply = websocket_frame(opcode, payload);
            (void)write_all(socket, {reinterpret_cast<const std::uint8_t*>(reply.data()), reply.size()}, 2000ms);
            continue;
        }
    }
}

void serve_connection(Socket& socket, const IpAddress& peer) {
    (void)socket.set_timeouts(kHeadTimeout, kWriteTimeout);
    std::vector<std::uint8_t> head;
    head.reserve(1024);
    std::array<std::uint8_t, 4096> buffer{};
    while (head.size() < kMaxHeadBytes) {
        auto received = socket.receive(buffer);
        if (!received || received.value() == 0) return;
        head.insert(head.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(received.value()));
        const auto terminator = std::search(head.begin(), head.end(), reinterpret_cast<const std::uint8_t*>("\r\n\r\n"),
            reinterpret_cast<const std::uint8_t*>("\r\n\r\n") + 4);
        if (terminator != head.end()) break;
    }
    const std::string text(head.begin(), head.end());
    const auto first_line_end = text.find("\r\n");
    if (first_line_end == std::string::npos) return;
    const auto request_line = text.substr(0, first_line_end);
    const auto first_space = request_line.find(' ');
    const auto last_space = request_line.rfind(' ');
    if (first_space == std::string::npos || last_space == first_space) return;
    const auto method = request_line.substr(0, first_space);
    const auto target = request_line.substr(first_space + 1, last_space - first_space - 1);
    g_requests++;
    log_line("requête " + method + " " + target + " depuis " + peer.text);

    const bool wants_upgrade = text.find("Upgrade: websocket") != std::string::npos ||
        text.find("upgrade: websocket") != std::string::npos;
    if (wants_upgrade && (target == "/ws" || target.rfind("/ws?", 0) == 0)) {
        std::string accept_value;
        if (!websocket_accept(text, accept_value)) {
            const auto body = http_response(400, "Bad Request", "text/plain; charset=utf-8", "poignée de main WebSocket invalide");
            (void)write_all(socket, {reinterpret_cast<const std::uint8_t*>(body.data()), body.size()}, 2000ms);
            return;
        }
        const std::string handshake = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept_value + "\r\n\r\n";
        if (auto written = write_all(socket, {reinterpret_cast<const std::uint8_t*>(handshake.data()), handshake.size()}, 2000ms); !written) return;
        // Phase WebSocket : l'inactivité normale d'une session est longue.
        (void)socket.set_timeouts(kWebSocketIdleTimeout, kWriteTimeout);
        log_line("websocket établi sur /ws");
        serve_websocket(socket);
        return;
    }
    std::string body;
    std::string_view content_type = "text/html; charset=utf-8";
    std::uint16_t status = 200;
    std::string_view reason = "OK";
    if (target == "/" || target.rfind("/?", 0) == 0 || target.rfind("/index.html", 0) == 0) body = index_page(target);
    else if (target.rfind("/api/status", 0) == 0) { body = status_json(); content_type = "application/json; charset=utf-8"; }
    else if (target.rfind("/ws", 0) == 0) { body = "ce chemin attend un upgrade WebSocket"; content_type = "text/plain; charset=utf-8"; status = 426; reason = "Upgrade Required"; }
    else { body = "chemin inconnu"; content_type = "text/plain; charset=utf-8"; status = 404; reason = "Not Found"; }
    const auto response = http_response(status, reason, content_type, body);
    (void)write_all(socket, {reinterpret_cast<const std::uint8_t*>(response.data()), response.size()}, 2000ms);
}

// Arrêt demandé : SIGTERM/SIGINT posent g_stop. Sous POSIX le gestionnaire est
// installé par sigaction SANS SA_RESTART, sinon accept() et receive() bloquants
// seraient relancés par le noyau et la boucle ne verrait jamais l'arrêt : les
// ouvriers ne seraient pas joints et la ligne de bilan ne serait pas écrite.
// std::signal sous glibc pose SA_RESTART, d'où sigaction ici.
void install_stop_handlers() {
    const auto handler = [](int) { g_stop.store(true); };
#ifdef _WIN32
    (void)std::signal(SIGINT, handler);
    (void)std::signal(SIGTERM, handler);
#else
    struct sigaction action{};
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0; // volontairement pas de SA_RESTART.
    (void)sigaction(SIGINT, &action, nullptr);
    (void)sigaction(SIGTERM, &action, nullptr);
#endif
}

void usage() {
    std::printf("Usage: devlink-demo-service [--port <num>] [--host <adresse>] [--quiet] [--once]\n"
                "  --port  port d'écoute loopback (0 = éphémère, défaut 0)\n"
                "  --host  adresse d'écoute (défaut 127.0.0.1)\n"
                "  --quiet pas de journal des requêtes\n"
                "  --once  sert une seule connexion puis s'arrête (tests)\n");
}
} // namespace

int main(int argc, char** argv) {
    install_stop_handlers();
    std::uint16_t requested_port = 0;
    std::string host = "127.0.0.1";
    bool once = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--quiet") g_quiet = true;
        else if (argument == "--once") once = true;
        else if (argument == "--help" || argument == "-h") { usage(); return 0; }
        else if (argument == "--port" && index + 1 < argc) {
            // 0 est une valeur valide : port éphémère choisi par l'OS.
            const std::string value = argv[++index];
            char* end = nullptr;
            const auto parsed = std::strtoul(value.c_str(), &end, 10);
            if (end == value.c_str() || *end != '\0' || parsed > 65535) {
                std::fprintf(stderr, "port invalide : %s\n", value.c_str());
                return 2;
            }
            requested_port = static_cast<std::uint16_t>(parsed);
        } else if (argument == "--host" && index + 1 < argc) host = argv[++index];
        else { std::fprintf(stderr, "argument inconnu : %.*s\n", static_cast<int>(argument.size()), argument.data()); usage(); return 2; }
    }
    auto address = IpAddress::parse(host);
    if (!address) { std::fprintf(stderr, "adresse d'écoute invalide\n"); return 2; }
    auto created = Socket::create(TransportProtocol::Tcp, address.value().family);
    if (!created) { std::fprintf(stderr, "socket impossible : %s\n", created.error().message.c_str()); return 3; }
    auto listener = std::move(created).value();
    (void)listener.set_exclusive_bind(true);
    if (auto bound = listener.bind(address.value(), Port{requested_port}); !bound) {
        std::fprintf(stderr, "bind impossible : %s\n", bound.error().message.c_str());
        return 3;
    }
    auto port = listener.bound_port();
    if (!port) { std::fprintf(stderr, "port illisible\n"); return 3; }
    if (auto listening = listener.listen(16); !listening) {
        std::fprintf(stderr, "listen impossible : %s\n", listening.error().message.c_str());
        return 3;
    }
    // Écoute non bloquante : la boucle vérifie l'arrêt demandé à chaque tour. Un
    // accept() bloquant n'est interrompu par un signal que sous POSIX, et seulement
    // sans SA_RESTART ; sous Windows (Ctrl+C) il ne l'est pas du tout. Le même code
    // s'arrête donc proprement sur les deux plateformes. Les sockets acceptées sont
    // remises en mode bloquant par Socket::accept.
    if (auto nonblocking = listener.set_nonblocking(true); !nonblocking) {
        std::fprintf(stderr, "écoute non bloquante impossible : %s\n", nonblocking.error().message.c_str());
        return 3;
    }
    std::printf("port=%u\n", port.value().value);
    std::fflush(stdout);
    log_line("service de démonstration en écoute sur " + address.value().text + ":" + std::to_string(port.value().value));

    unsigned served = 0;
    unsigned refused = 0;
    // Capacité explicite et cohérente : kMaxConnections connexions ACTIVES et
    // kMaxQueued tâches EN ATTENTE, soit kMaxConnections + kMaxQueued au total.
    // Au-delà, la soumission est refusée et le client reçoit 503 immédiatement.
    // Le pool joint tous ses ouvriers à l'arrêt : aucun thread détaché, aucune file
    // infinie. C'est le même code que celui exercé par les tests d'intégration.
    ConnectionPool pool;
    if (!once) {
        auto started = pool.start(kMaxConnections, kMaxQueued, [](Socket& socket, const IpAddress& peer) {
            serve_connection(socket, peer);
        });
        if (!started) {
            std::fprintf(stderr, "pool de connexions impossible : %s\n", started.error().message.c_str());
            listener.close();
            return 3;
        }
    }
    while (!g_stop.load()) {
        IpAddress peer;
        Port peer_port{};
        auto accepted = listener.accept(&peer, &peer_port);
        if (!accepted) {
            if (accepted.error().code == ErrorCode::WouldBlock) { std::this_thread::sleep_for(50ms); continue; }
            // EINTR après SIGTERM/SIGINT : ce n'est pas une erreur, c'est l'arrêt
            // demandé. On sort de la boucle pour joindre les ouvriers proprement.
            if (g_stop.load()) break;
            std::fprintf(stderr, "accept impossible : %s\n", accepted.error().message.c_str());
            break;
        }
        if (once) {
            auto socket = std::move(accepted).value();
            serve_connection(socket, peer);
            socket.close();
            ++served;
            break;
        }
        auto socket = std::move(accepted).value();
        // En cas de refus la socket n'est pas consommée : l'appelant répond puis ferme.
        switch (pool.submit(std::move(socket), peer)) {
        case PoolSubmission::Refused: {
            const auto body = http_response(503, "Service Unavailable", "text/plain; charset=utf-8", "service de démonstration saturé");
            (void)write_all(socket, {reinterpret_cast<const std::uint8_t*>(body.data()), body.size()}, 1000ms);
            socket.close();
            ++refused;
            break;
        }
        default:
            ++served;
            break;
        }
    }
    // Arrêt propre : plus d'acceptation, les ouvriers finissent leur connexion
    // courante puis sortent ; tous sont joints, aucun n'est détaché.
    listener.close();
    pool.stop();
    const auto pool_counters = pool.counters();
    log_line("service arrêté après " + std::to_string(served) + " connexion(s) prise(s) en charge et " +
        std::to_string(refused) + " refusée(s) : actives_pic=" + std::to_string(pool_counters.max_active) +
        " attente_pic=" + std::to_string(pool_counters.max_queued) + " terminées=" +
        std::to_string(pool_counters.completed));
    return 0;
}
