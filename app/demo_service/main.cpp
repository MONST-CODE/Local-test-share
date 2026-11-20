// DevLink — app/demo_service/main.cpp — service local de démonstration.
// Rôle : donner au proxy un amont RÉEL à ponter, pour qualifier le partage de
// bout en bout. HTTP/1.1 minimal + WebSocket RFC 6455 (poignée de main, écho de
// trames texte, ping/pong, fermeture). Il n'écoute que sur loopback : seule la
// passerelle DevLink, avec son jeton, le rend joignable depuis le LAN.
//
// Usage :
//   devlink-demo-service [--port <num>] [--host <adresse>] [--quiet] [--once] [--run-id <16 hex>]
// Le port réel est imprimé sur stdout (« port=<num> »), 0 demandant un port
// éphémère. Codes de sortie : 0 arrêt propre, 2 usage invalide, 3 échec réseau.
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string_view>
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
std::mutex g_state_mutex;
std::uint64_t g_action_count = 0;
std::string g_last_value;
bool g_quiet = false;

void log_line(const std::string& text) {
    if (g_quiet) return;
    std::printf("%s\n", text.c_str());
    std::fflush(stdout);
}

constexpr std::string_view kDefaultCsp = "default-src 'self'; img-src 'self'; script-src 'self'; style-src 'self'; "
    "connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'";
// The qualification fixture deliberately lets the PAGE try to reach other
// origins (connect/img/frame '*'): the test must prove that DevLink's Browser
// Worker blocks them by itself, not that the page's own CSP did.
constexpr std::string_view kFixtureCsp = "default-src 'self'; script-src 'self'; style-src 'self'; "
    "img-src * data:; connect-src *; frame-src *; object-src 'none'; base-uri 'none'";

std::string http_response(std::uint16_t status, std::string_view reason, std::string_view content_type, std::string_view body,
    std::string_view extra_headers = {}, std::string_view csp = kDefaultCsp) {
    std::string text = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) + "\r\n" + "Content-Type: " +
        std::string(content_type) + "\r\n" + "Content-Length: " + std::to_string(body.size()) + "\r\n" +
        "Cache-Control: no-store\r\n";
    text += "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n";
    text += "Content-Security-Policy: " + std::string(csp) + "\r\n";
    if (!extra_headers.empty()) text += std::string(extra_headers) + "\r\n";
    text += "Connection: close\r\n\r\n";
    text += body;
    return text;
}

std::string index_page() {
    return R"HTML(<!doctype html>
<html lang="fr"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DevLink — application web locale de démonstration</title><link rel="stylesheet" href="/assets/app.css"></head><body><main>
<img src="/assets/demo-mark.svg" alt="Repère visuel DevLink">
<h1>Application web locale</h1>
<p id="recognizable">Page de démonstration servie par le processus hôte DevLink.</p>
<p>État serveur : <strong id="counter">chargement…</strong></p>
<button id="increment" type="button">Incrémenter le compteur côté hôte</button>
<form id="feedback"><label for="value">Message de test (2 à 48 caractères)</label><br>
<input id="value" name="value" required minlength="2" maxlength="48" pattern="[A-Za-z0-9 ._'-]{2,48}">
<button type="submit">Envoyer au serveur hôte</button></form>
<output id="result" aria-live="polite"></output>
<nav><a href="/details">Page détails</a><a href="/api/status">État JSON</a></nav>
</main><script src="/assets/app.js" defer></script></body></html>)HTML";
}

std::string app_styles() {
    return R"CSS(:root{color-scheme:dark;font:16px system-ui,sans-serif;background:#211916;color:#f7eee5}
body{max-width:760px;margin:3rem auto;padding:0 1.25rem}main{background:#30231f;border:1px solid #705044;border-radius:16px;padding:2rem}
h1{color:#edb27e}img{width:96px;height:96px;float:right}button{background:#bc744f;color:#fff;border:0;border-radius:8px;padding:.7rem 1rem;cursor:pointer}
input{padding:.65rem;border-radius:6px;border:1px solid #997865;background:#211916;color:#fff}a{color:#edb27e}
output{display:block;margin:.75rem 0;color:#d6ad8d}nav{display:flex;gap:1rem;margin-top:1.5rem})CSS";
}

std::string app_script() {
    return R"JS('use strict';
const counter=document.querySelector('#counter');
const result=document.querySelector('#result');
async function refresh(){const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw new Error('status '+r.status);const s=await r.json();counter.textContent=s.actions+' actions — '+s.last_value;}
async function act(data){const r=await fetch('/api/action',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(data)});const v=await r.json();if(!r.ok)throw new Error(v.error||('HTTP '+r.status));result.textContent=v.message;await refresh();}
document.querySelector('#increment').addEventListener('click',()=>act({action:'increment'}).catch(e=>result.textContent=e.message));
document.querySelector('#feedback').addEventListener('submit',e=>{e.preventDefault();act({action:'submit',value:document.querySelector('#value').value}).catch(e=>result.textContent=e.message);});
refresh().catch(e=>result.textContent='Service indisponible : '+e.message);
)JS";
}

// ---------------------------------------------------------------------------
// Qualification fixture (/fixture): a real page whose rendered PIXELS carry the
// run identifier, so a guest can prove that what it displays was rendered by
// the host browser for THIS run. Fixed layout (1280x720 CSS viewport):
//   band   : y 0..24, 64 cells of 20 px, MSB first, white = 1, black = 0
//   square : x 40..240, y 100..300, colour = clicks mod 4 (red/green/blue/yellow)
//   button : x 300..600, y 100..200   input : x 300..700, y 260..300
// ---------------------------------------------------------------------------
std::string g_run_id = "0000000000000000";

std::string fixture_page() {
    std::string band;
    for (int bit = 0; bit < 64; ++bit) {
        const char c = g_run_id[static_cast<std::size_t>(bit / 4)];
        const int nibble = c <= '9' ? c - '0' : c - 'a' + 10;
        const bool one = (nibble >> (3 - bit % 4)) & 1;
        band += one ? "<i class=\"b1\"></i>" : "<i class=\"b0\"></i>";
    }
    return "<!doctype html><html lang=\"fr\"><head><meta charset=\"utf-8\"><title>DevLink fixture " + g_run_id +
        "</title><link rel=\"stylesheet\" href=\"/fixture.css\"></head><body data-run=\"" + g_run_id + "\">"
        "<div id=\"band\">" + band + "</div><div id=\"square\" class=\"c0\"></div>"
        "<button id=\"hit\" type=\"button\">Cliquer ici</button><input id=\"field\" autocomplete=\"off\" spellcheck=\"false\">"
        "<p id=\"echo\"></p><p id=\"clicks\">clics : 0</p><p id=\"ticker\">t=0</p><p id=\"runtext\">RUN " + g_run_id + "</p>"
        "<script src=\"/fixture.js\"></script></body></html>";
}
std::string fixture_styles() {
    return R"CSS(html,body{margin:0;background:#f4ecdf;color:#2a1d17;font:22px system-ui,sans-serif;overflow:hidden}
#band{position:fixed;left:0;top:0;width:1280px;height:24px;display:flex}#band i{display:block;width:20px;height:24px}
.b0{background:#000}.b1{background:#fff}
#square{position:fixed;left:40px;top:100px;width:200px;height:200px}
.c0{background:#d01010}.c1{background:#10b010}.c2{background:#1030d0}.c3{background:#e0d010}
#hit{position:fixed;left:300px;top:100px;width:300px;height:100px;font-size:26px;background:#bc744f;color:#fff;border:0;border-radius:10px}
#field{position:fixed;left:300px;top:260px;width:400px;height:40px;font-size:22px}
#echo{position:fixed;left:300px;top:320px;margin:0}#clicks{position:fixed;left:40px;top:320px;margin:0}
#ticker{position:fixed;left:40px;top:400px;margin:0}#runtext{position:fixed;left:300px;top:400px;margin:0})CSS";
}
std::string fixture_script() {
    return R"JS('use strict';
let clicks=0,ticks=0,keys=0;
const sq=document.getElementById('square'),hit=document.getElementById('hit'),field=document.getElementById('field');
hit.addEventListener('click',e=>{if(!e.isTrusted)return;clicks++;sq.className='c'+(clicks%4);document.getElementById('clicks').textContent='clics : '+clicks;});
field.addEventListener('input',()=>{document.getElementById('echo').textContent=field.value;});
field.addEventListener('keydown',e=>{if(e.isTrusted)keys++;});
setInterval(()=>{document.getElementById('ticker').textContent='t='+(++ticks);},100);
window.devlinkFixture=()=>JSON.stringify({run:document.body.dataset.run,clicks,keys,value:field.value,ticks,
  focused:document.activeElement&&document.activeElement.id,owner:localStorage.getItem('devlink-owner'),
  session:sessionStorage.getItem('devlink-owner'),cookie:document.cookie});
)JS";
}

std::string details_page() {
    return R"HTML(<!doctype html><html lang="fr"><head><meta charset="utf-8"><title>Détails — DevLink</title></head>
<body><h1>Page détails de l’application locale</h1><p>Cette route est servie par le même serveur hôte.</p>
<p>Ressource de test : <img src="/assets/demo-mark.svg" alt="Repère DevLink" width="64" height="64"></p>
<a href="/">Retour à l’application</a></body></html>)HTML";
}

std::string status_json() {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    return "{\"service\":\"devlink-demo\",\"requests\":" + std::to_string(g_requests.load()) +
        ",\"websockets\":" + std::to_string(g_websockets.load()) +
        ",\"actions\":" + std::to_string(g_action_count) + ",\"last_value\":\"" + g_last_value +
        "\",\"protocol\":\"HTTP/1.1+RFC6455\"}";
}

constexpr std::size_t kMaxHttpBodyBytes = 4096;
std::optional<std::string_view> header_value(std::string_view headers, std::string_view wanted) {
    std::size_t start = headers.find("\r\n");
    if (start == std::string_view::npos) return std::nullopt;
    start += 2;
    while (start < headers.size()) {
        const auto end = headers.find("\r\n", start);
        if (end == std::string_view::npos || end == start) break;
        const auto line = headers.substr(start, end - start);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            std::string key(line.substr(0, colon));
            std::string target(wanted);
            for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (auto& c : target) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (key == target) {
                auto value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
                while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
                return value;
            }
        }
        start = end + 2;
    }
    return std::nullopt;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool decode_form_component(std::string_view input, std::string& output) {
    output.clear();
    for (std::size_t i = 0; i < input.size(); ++i) {
        unsigned char value = static_cast<unsigned char>(input[i]);
        if (value == '+') value = ' ';
        else if (value == '%') {
            if (i + 2 >= input.size()) return false;
            const int high = hex_digit(input[i + 1]), low = hex_digit(input[i + 2]);
            if (high < 0 || low < 0) return false;
            value = static_cast<unsigned char>((high << 4) | low);
            i += 2;
        }
        // The demo accepts safe printable ASCII only; no control characters,
        // markup injection, or malformed UTF-8 is persisted by this endpoint.
        if (value < 0x20 || value > 0x7e) return false;
        output.push_back(static_cast<char>(value));
        if (output.size() > 48) return false;
    }
    return true;
}
bool safe_demo_value(std::string_view value) {
    if (value.size() < 2 || value.size() > 48) return false;
    for (const unsigned char c : value) {
        const bool alpha_num = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alpha_num && c != ' ' && c != '.' && c != '_' && c != '-' && c != '\'') return false;
    }
    return true;
}
bool parse_demo_action(std::string_view body, std::string& action, std::string& value) {
    action.clear(); value.clear();
    bool action_seen = false, value_seen = false;
    std::size_t start = 0;
    while (start <= body.size()) {
        const auto amp = body.find('&', start);
        const auto stop = amp == std::string_view::npos ? body.size() : amp;
        const auto pair = body.substr(start, stop - start);
        const auto equal = pair.find('=');
        if (equal == std::string_view::npos) return false;
        std::string key, decoded;
        if (!decode_form_component(pair.substr(0, equal), key) ||
            !decode_form_component(pair.substr(equal + 1), decoded)) return false;
        if (key == "action" && !action_seen) { action = std::move(decoded); action_seen = true; }
        else if (key == "value" && !value_seen) { value = std::move(decoded); value_seen = true; }
        else return false;
        if (amp == std::string_view::npos) break;
        start = amp + 1;
    }
    if (!action_seen) return false;
    if (action == "increment") return !value_seen;
    if (action == "submit") return value_seen && safe_demo_value(value);
    return false;
}

std::string demo_svg() {
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 96 96\"><rect width=\"96\" height=\"96\" rx=\"18\" fill=\"#bc744f\"/><path d=\"M18 28h22L26 48l14 20H18L4 48zM56 28h22L64 48l14 20H56L42 48z\" fill=\"#f7eee5\"/><circle cx=\"48\" cy=\"48\" r=\"9\" fill=\"#211916\"/></svg>";
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
    const auto header_terminator = std::search(head.begin(), head.end(),
        reinterpret_cast<const std::uint8_t*>("\r\n\r\n"),
        reinterpret_cast<const std::uint8_t*>("\r\n\r\n") + 4);
    if (header_terminator == head.end()) {
        const auto response = http_response(431, "Request Header Fields Too Large", "text/plain; charset=utf-8", "en-têtes trop volumineux");
        (void)write_all(socket, {reinterpret_cast<const std::uint8_t*>(response.data()), response.size()}, 2000ms);
        return;
    }
    const auto header_size = static_cast<std::size_t>(header_terminator - head.begin()) + 4;
    if (header_size > kMaxHeadBytes) {
        const auto response = http_response(431, "Request Header Fields Too Large", "text/plain; charset=utf-8", "en-têtes trop volumineux");
        (void)write_all(socket, {reinterpret_cast<const std::uint8_t*>(response.data()), response.size()}, 2000ms);
        return;
    }
    const std::string text(head.begin(), head.end());
    const std::string_view headers(text.data(), header_size - 2);
    const auto first_line_end = text.find("\r\n");
    if (first_line_end == std::string::npos) return;
    const auto request_line = text.substr(0, first_line_end);
    const auto first_space = request_line.find(' ');
    const auto last_space = request_line.rfind(' ');
    if (first_space == std::string::npos || last_space == first_space) return;
    const auto method = request_line.substr(0, first_space);
    const auto target = request_line.substr(first_space + 1, last_space - first_space - 1);
    const auto transfer_encoding = header_value(headers, "transfer-encoding");
    const auto length_header = header_value(headers, "content-length");
    std::size_t content_length = 0;
    if (transfer_encoding) {
        const auto response = http_response(400, "Bad Request", "text/plain; charset=utf-8", "Transfer-Encoding non pris en charge");
        (void)write_all(socket, {reinterpret_cast<const std::uint8_t*>(response.data()), response.size()}, 2000ms);
        return;
    }
    if (length_header) {
        const auto parsed = std::from_chars(length_header->data(), length_header->data() + length_header->size(), content_length);
        if (parsed.ec != std::errc{} || parsed.ptr != length_header->data() + length_header->size()) {
            const auto response = http_response(400, "Bad Request", "text/plain; charset=utf-8", "Content-Length invalide");
            (void)write_all(socket, {reinterpret_cast<const std::uint8_t*>(response.data()), response.size()}, 2000ms);
            return;
        }
    }
    if (content_length > kMaxHttpBodyBytes) {
        const auto response = http_response(413, "Payload Too Large", "text/plain; charset=utf-8", "corps trop volumineux");
        (void)write_all(socket, {reinterpret_cast<const std::uint8_t*>(response.data()), response.size()}, 2000ms);
        return;
    }
    std::string request_body(text.data() + header_size,
        std::min(content_length, text.size() - header_size));
    while (request_body.size() < content_length) {
        const auto need = std::min(buffer.size(), content_length - request_body.size());
        auto received = socket.receive({buffer.data(), need});
        if (!received || received.value() == 0) return;
        request_body.append(reinterpret_cast<const char*>(buffer.data()), received.value());
    }
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
    std::string_view csp = kDefaultCsp;
    std::string extra;
    if ((target == "/" || target == "/index.html") && method == "GET") body = index_page();
    else if (target == "/fixture" && method == "GET") { body = fixture_page(); csp = kFixtureCsp; }
    else if (target == "/fixture.css" && method == "GET") { body = fixture_styles(); content_type = "text/css; charset=utf-8"; }
    else if (target == "/fixture.js" && method == "GET") { body = fixture_script(); content_type = "text/javascript; charset=utf-8"; }
    else if (target == "/redirect-out" && method == "GET") {
        // Redirect towards a port that is NEVER allowlisted (discard/9): the
        // Browser Worker must refuse to follow it.
        body = "redirection"; content_type = "text/plain; charset=utf-8"; status = 302; reason = "Found";
        extra = "Location: http://127.0.0.1:9/";
    }
    else if (target == "/details" && method == "GET") body = details_page();
    else if (target == "/assets/demo-mark.svg" && method == "GET") { body = demo_svg(); content_type = "image/svg+xml"; }
    else if (target == "/assets/app.css" && method == "GET") { body = app_styles(); content_type = "text/css; charset=utf-8"; }
    else if (target == "/assets/app.js" && method == "GET") { body = app_script(); content_type = "text/javascript; charset=utf-8"; }
    else if (target == "/api/status" && method == "GET") { body = status_json(); content_type = "application/json; charset=utf-8"; }
    else if (target == "/api/action" && method == "POST") {
        const auto request_type = header_value(headers, "content-type");
        std::string action, value;
        if (!request_type || request_type->substr(0, request_type->find(';')) != "application/x-www-form-urlencoded" ||
            !parse_demo_action(request_body, action, value)) {
            body = "{\"error\":\"requête invalide\"}";
            content_type = "application/json; charset=utf-8";
            status = 400; reason = "Bad Request";
        } else {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            ++g_action_count;
            g_last_value = action == "increment" ? "compteur incrémenté" : value;
            body = "{\"actions\":" + std::to_string(g_action_count) + ",\"last_value\":\"" + g_last_value + "\",\"message\":\"" +
                (action == "increment" ? "Compteur incrémenté sur l'hôte" : "Formulaire reçu par l'hôte") + "\"}";
            content_type = "application/json; charset=utf-8";
        }
    }
    else if (target == "/api/action" || target == "/api/status") {
        body = "{\"error\":\"méthode non autorisée\"}"; content_type = "application/json; charset=utf-8";
        status = 405; reason = "Method Not Allowed";
    }
    else if (target == "/ws") { body = "ce chemin attend un upgrade WebSocket"; content_type = "text/plain; charset=utf-8"; status = 426; reason = "Upgrade Required"; }
    else { body = "chemin inconnu"; content_type = "text/plain; charset=utf-8"; status = 404; reason = "Not Found"; }
    const auto response = http_response(status, reason, content_type, body, extra, csp);
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
                "  --once  sert une seule connexion puis s'arrête (tests)\n"
                "  --run-id <16 hex> identifiant de run affiché par /fixture (qualification)\n");
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
        else if (argument == "--run-id" && index + 1 < argc) {
            const std::string value = argv[++index];
            const bool hex = value.size() == 16 && std::all_of(value.begin(), value.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            });
            if (!hex) { std::fprintf(stderr, "run-id invalide : 16 chiffres hexadécimaux minuscules attendus\n"); return 2; }
            g_run_id = value;
        }
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
