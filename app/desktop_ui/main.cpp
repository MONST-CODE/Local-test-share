// DevLink — app/desktop_ui/main.cpp
// Poste de commande desktop natif : SDL3 + Dear ImGui.
//
// Contrat d'architecture (mandat §12) : l'interface ne possède AUCUN état de
// session. Elle lit des instantanés immuables publiés par les threads du moteur
// (Monitor, ShareHost, HostServer, GuestClient, ShareGuest, DiscoveryListener, Announcer)
// et n'écrit que des commandes. Aucun écran ne met en scène un état
// « connecté », « sécurisé », « streaming » ou « session active » que le moteur
// n'a pas confirmé : un démarrage est transactionnel et tout échec est affiché.
//
// Partage reel (ADR-018) : l'hote execute ShareHost (navigateur WebView2 reel,
// images du compositeur, canal media TLS 1.3 PSK par invite) relie au plan de
// controle HostServer ; l'invite ouvre ShareGuest apres admission, affiche les
// images decodees dans une texture SDL3 et renvoie souris/clavier, que l'hote
// autorise ou refuse cote serveur (SharePolicy). Le proxy HTTP ne fait plus
// partie du chemin de partage : il donnerait aux invites les ressources web.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "design_system/components.hpp"
#include "design_system/icons.hpp"
#include "design_system/layout.hpp"
#include "design_system/themes.hpp"
#include "design_system/tokens.hpp"
#include "design_system/typography.hpp"
#include "design_system/window_icon_data.hpp"

#include "engine/browser/browser.hpp"
#include "engine/browser/webview2_backend.hpp"
#include "engine/core/clock.hpp"
#include "engine/core/product.hpp"
#include "engine/core/secrets.hpp"
#include "engine/discovery/monitor.hpp"
#include "engine/net/discovery.hpp"
#include "engine/net/guest_client.hpp"
#include "engine/net/host_server.hpp"
#include "engine/media/jpeg_decoder.hpp"
#include "engine/share/share_guest.hpp"
#include "engine/share/share_host.hpp"
#include "engine/share/view_mapping.hpp"
#include "engine/platform/native.hpp"
#include "engine/port_allocator/port_allocator.hpp"

namespace {

using devlink::ui::MainNavigationTab;

// Identité publique : UNE seule source (engine/core/product.hpp). Le titre et
// l'usage en découlent pour qu'aucune chaîne ne diverge du --version.
const std::string kWindowTitle =
    std::string(devlink::kProductName) + " - " + std::string(devlink::kProductTagline);
const std::string kUsage =
    "Usage: devlink-desktop [--smoke-test [--screenshot file.bmp]] [--demo-synthetic-browser]\n"
    "       devlink-desktop --host <origine[,origine]> [--entry URL] [--mode A|B|C] [--bind IP] [--no-approval]\n"
    "                       [--print-invite] [--report-fixture] [--exit-when-guests-gone]\n"
    "       devlink-desktop --join <IP> <port> --code <code> [--name nom] [--request-control]\n"
    "                       [--auto-click x,y] [--auto-type texte]\n"
    "       options communes : [--capture-after secondes fichier.bmp] [--exit-after secondes]\n";

// Le moniteur publie des instantanés immuables ; l'interface demande un
// rafraîchissement périodique au lieu de bloquer sur wait_for_revision()
// pendant une frame de rendu.
constexpr int kMonitorRefreshPeriodFrames = 45;
// L'annonce est rafraîchie avec les compteurs réels de la session.
constexpr int kAnnounceUpdatePeriodFrames = 90;
constexpr int kSmokeFrameCount = 8;

// ---------------------------------------------------------------------------
// Textes dérivés de l'état réel du moteur (jamais d'invention côté UI)
// ---------------------------------------------------------------------------

std::string describe(const plpt::Error& error) {
    std::string text(plpt::to_string(error.code));
    if (!error.context.empty()) text += " [" + error.context + "]";
    if (!error.message.empty()) text += " : " + error.message;
    if (error.native_code != 0) text += " (code natif " + std::to_string(error.native_code) + ")";
    return text;
}

// Les trois modes de participation du mandat §5. Le rôle effectif est appliqué
// côté serveur par le moteur de session (role_has_capability) : l'interface le
// demande et l'affiche, elle ne l'impose jamais elle-même.
enum class ParticipationMode : int { Independent = 0, Collaborative = 1, Observation = 2 };

const char* mode_label(ParticipationMode mode) {
    switch (mode) {
    case ParticipationMode::Independent: return "Mode A - test indépendant";
    case ParticipationMode::Collaborative: return "Mode B - collaboration sur une vue";
    case ParticipationMode::Observation: return "Mode C - observation seule";
    }
    return "mode inconnu";
}

const char* mode_capability_note(ParticipationMode mode) {
    switch (mode) {
    case ParticipationMode::Independent:
        return "Profil de navigation distinct, entrée autorisée sur sa propre vue.";
    case ParticipationMode::Collaborative:
        return "Vue partagée, entrée soumise au bail de contrôle accordé par l'hote.";
    case ParticipationMode::Observation:
        return "Rendu seul : toute entrée est refusée, y compris si l'invite insiste.";
    }
    return "";
}

const char* platform_label() {
#if defined(_WIN32)
    return "Windows x64 (MSYS2 UCRT64)";
#elif defined(__linux__)
    return "Linux x64";
#else
    return "plateforme non prise en charge";
#endif
}

// Largeur de panneau calculée depuis l'espace réellement disponible : aucune
// valeur absolue devinée, ce qui casserait a 125 %, 150 % ou 200 % de DPI.
float pane_width(float fraction, float minimum) {
    const float available = ImGui::GetContentRegionAvail().x;
    return std::max(minimum, available * fraction);
}

// ---------------------------------------------------------------------------
// Moteur navigateur : séparation stricte reel / test (mandat §21)
// ---------------------------------------------------------------------------

struct BrowserEngine {
    bool real = false;           // vrai moteur navigateur operationnel (WebView2)
    bool demo_synthetic = false; // demonstration demandee explicitement : jamais partageable
    std::string status = "non initialise";

    // Une session de partage reelle exige un moteur reel : le repli synthetique
    // silencieux est interdit (ShareHost le refuse aussi, defense en profondeur).
    bool usable_for_real_session() const { return real; }
};

BrowserEngine create_browser_engine(bool allow_synthetic_demo) {
    BrowserEngine engine;
    if (plpt::browser::WebView2Backend::is_available()) {
        engine.real = true;
        engine.status = "Microsoft Edge WebView2 (moteur reel)";
        return engine;
    }
    if (allow_synthetic_demo) {
        engine.demo_synthetic = true;
        engine.status = "moteur synthetique (demonstration) - AUCUN partage reel";
        return engine;
    }
#if defined(_WIN32)
    if (!plpt::browser::kWebView2SdkCompiled) {
        engine.status = "partage navigateur indisponible : SDK WebView2 absent de cette compilation";
    } else {
        engine.status = std::string("partage navigateur indisponible : runtime WebView2 ") +
            (plpt::browser::WebView2Backend::runtime_available()
                ? "detecte mais non initialisable"
                : "absent (installer Microsoft Edge WebView2 Runtime)");
    }
#else
    engine.status = "partage navigateur indisponible : le Browser Worker reel est propre a Windows (ADR-018)";
#endif
    return engine;
}

// ---------------------------------------------------------------------------
// Service local selectionne dans la decouverte
// ---------------------------------------------------------------------------

struct SelectedService {
    bool valid = false;
    plpt::ServiceId service_id{};
    std::string label = "service inconnu";
    std::string address;
    std::uint16_t port = 0;
    plpt::TransportProtocol protocol = plpt::TransportProtocol::Unknown;
};

// Origine HTTP proposee pour un service TCP selectionne. Elle est affichee et
// modifiable AVANT le demarrage : la detection ne vaut jamais autorisation.
std::string origin_for_service(const SelectedService& service) {
    std::string host = "127.0.0.1";
    const std::string& a = service.address;
    if (a == "::1" || a == "::" || a == "[::]" || a == "[::1]") {
        host = "[::1]";
    } else if (!a.empty() && a != "0.0.0.0" && a != "127.0.0.1" && a != "*") {
        if (auto parsed = plpt::IpAddress::parse(a); parsed && parsed.value().family == plpt::IpFamily::Ipv4) host = a;
    }
    return "http://" + host + ":" + std::to_string(service.port);
}

plpt::share::ShareMode share_mode_of(ParticipationMode mode) {
    switch (mode) {
    case ParticipationMode::Independent: return plpt::share::ShareMode::Independent;
    case ParticipationMode::Collaborative: return plpt::share::ShareMode::Collaborative;
    case ParticipationMode::Observation: return plpt::share::ShareMode::ObserveOnly;
    }
    return plpt::share::ShareMode::ObserveOnly;
}

// ---------------------------------------------------------------------------
// Ressources SDL/ImGui : liberation dans l'ordre inverse de la creation,
// aucun resultat [[nodiscard]] ignore.
// ---------------------------------------------------------------------------

struct Runtime {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    bool sdl = false;
    bool imgui = false;
    bool platform_backend = false;
    bool render_backend = false;

    ~Runtime() {
        if (render_backend) ImGui_ImplSDLRenderer3_Shutdown();
        if (platform_backend) ImGui_ImplSDL3_Shutdown();
        if (imgui) ImGui::DestroyContext();
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        if (sdl) SDL_Quit();
    }
};

// Texture SDL3 d'une image distante. Creee, mise a jour et detruite sur le
// thread principal uniquement (modele d'execution SDL3) ; reallouee seulement
// quand les dimensions changent.
struct RemoteTexture {
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t sequence = 0;

    bool update(const plpt::media::DecodedImage& image, std::uint64_t seq) {
        if (!renderer || image.width == 0 || image.height == 0 ||
            image.rgba.size() != static_cast<std::size_t>(image.width) * image.height * 4) return false;
        if (!texture || width != image.width || height != image.height) {
            reset();
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                static_cast<int>(image.width), static_cast<int>(image.height));
            if (!texture) return false;
            (void)SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
            width = image.width;
            height = image.height;
        }
        if (!SDL_UpdateTexture(texture, nullptr, image.rgba.data(), static_cast<int>(image.width * 4))) return false;
        sequence = seq;
        return true;
    }
    void reset() {
        if (texture) SDL_DestroyTexture(texture);
        texture = nullptr;
        width = height = 0;
        sequence = 0;
    }
    RemoteTexture() = default;
    RemoteTexture(const RemoteTexture&) = delete;
    RemoteTexture& operator=(const RemoteTexture&) = delete;
    ~RemoteTexture() { reset(); }
};

// Resultat du demarrage asynchrone : vide = succes confirme par le moteur.
using LaunchErrors = std::vector<std::string>;

// ---------------------------------------------------------------------------
// Etat de l'application : formulaires + pointeurs vers le moteur
// ---------------------------------------------------------------------------

struct AppState {
    plpt::SystemClock clock;
    std::unique_ptr<plpt::NativeNetworkInterfaceProvider> interfaces;
    SDL_Window* window = nullptr;

    // Navigation et apparence
    MainNavigationTab tab = MainNavigationTab::Dashboard;
    MainNavigationTab nav_shown = MainNavigationTab::Dashboard; // onglet reellement affiche par ImGui
    bool host_mode = true;
    devlink::ui::ThemeMode theme_mode = devlink::ui::ThemeMode::DevDark;
    float dpi_scale = 1.0f;

    // Decouverte locale (observation passive, jamais une autorisation)
    std::unique_ptr<plpt::Monitor> monitor;
    std::shared_ptr<const plpt::MonitorView> monitor_view;
    SelectedService selected;
    int frames_since_refresh = 0;
    int frames_since_announce = 0;
    bool monitor_refresh_accepted = false;

    // Hote : partage reel (navigateur + canal media) et plan de controle
    std::unique_ptr<plpt::share::ShareHost> share;
    std::unique_ptr<plpt::net::HostServer> host;
    std::unique_ptr<plpt::net::Announcer> announcer;
    std::future<LaunchErrors> host_launch;
    bool host_launching = false;
    plpt::InviteCode invite_code;
    bool host_running = false;
    std::string host_note = "aucune session hote";
    std::vector<std::string> host_errors;
    RemoteTexture host_preview;
    plpt::media::JpegDecoder preview_decoder; // thread principal uniquement

    // Moteur navigateur
    BrowserEngine browser;
    bool browser_demo_requested = false;

    // Plan invite : controle + canal media
    std::unique_ptr<plpt::net::GuestClient> guest;
    std::unique_ptr<plpt::share::ShareGuest> media;
    std::uint64_t media_offer_revision = 0;
    plpt::IpAddress guest_host_ip{};
    RemoteTexture guest_view;
    plpt::share::ViewRect guest_rect{};  // ou l'image a ete dessinee (frame precedente)
    bool remote_capture = false;       // clavier dirige vers la vue distante
    std::uint8_t remote_buttons = 0;   // boutons appuyes depuis la vue (bits 0..2)
    float remote_last_x = -1.0f;
    float remote_last_y = -1.0f;
    std::unique_ptr<plpt::net::DiscoveryListener> listener;
    bool guest_running = false;
    bool listener_running = false;
    std::vector<std::string> guest_errors;

    // Formulaires hote
    char session_name[64] = "Session DevLink";
    char host_name[64] = "hote";
    char bind_address[64] = "";      // vide = DevLink choisit (LAN reel sinon boucle locale)
    char allowed_origins[256] = "";  // allowlist du navigateur distant
    char entry_url[256] = "";        // vide = premiere origine autorisee
    int max_participants = static_cast<int>(plpt::share::kDefaultShareGuests);
    bool approval_required = true;
    ParticipationMode mode = ParticipationMode::Collaborative;

    // Formulaires invite
    char guest_name[64] = "invite";
    char guest_code[64] = "";
    char guest_address[64] = "127.0.0.1";
    int guest_port = 0;

    char chat_input[512] = "";
    // Destinataire du prochain message : 0 = canal general, sinon un participant
    // publie par le moteur (jamais un identifiant saisi a la main).
    std::uint64_t chat_recipient = 0;

    // Signalement d'anomalie (invite -> hote, protocole 1.5). Bornes identiques a
    // celles du moteur : kMaxFeedbackBytes (2048) et kMaxFeedbackContextBytes (256).
    int feedback_severity = static_cast<int>(plpt::net::FeedbackSeverity::Minor);
    char feedback_context[257] = "";
    char feedback_description[2049] = "";

    // Notification
    std::string toast;
    std::chrono::steady_clock::time_point toast_expiry{};

    void notify(std::string message) {
        toast = std::move(message);
        toast_expiry = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    }
    bool toast_visible() const {
        return !toast.empty() && std::chrono::steady_clock::now() < toast_expiry;
    }
};

// Allowlist stricte : les origines saisies, separees par des virgules. Une liste
// vide n'autorise rien.
std::vector<std::string> parse_origins(const char* raw) {
    std::vector<std::string> origins;
    const std::string_view text(raw);
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::size_t stop = (comma == std::string_view::npos) ? text.size() : comma;
        std::string_view item = text.substr(start, stop - start);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t' || item.back() == '/')) item.remove_suffix(1);
        if (!item.empty()) origins.emplace_back(item);
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return origins;
}

// ---------------------------------------------------------------------------
// Demarrage transactionnel de la session hote (ADR-018)
//
// Le thread UI valide et prepare ; un thread de lancement execute ShareHost::
// start (navigateur reel, navigation, PREMIERE IMAGE REELLE, canal media) puis
// le plan de controle. L'UI ne bloque jamais (un contexte WebView2 prend
// plusieurs secondes) et n'affiche « actif » qu'apres confirmation du moteur.
// ---------------------------------------------------------------------------

bool start_host_session(AppState& state) {
    state.host_errors.clear();
    if (!state.browser.usable_for_real_session()) {
        state.host_errors.push_back("partage refuse : moteur navigateur reel indisponible - " + state.browser.status);
        return false;
    }
    if (state.host_running || state.host_launching) {
        state.host_errors.push_back("une session hote est deja en cours");
        return false;
    }
    // 1. Selection explicite : origines saisies, sinon celle du service choisi.
    std::vector<std::string> origins = parse_origins(state.allowed_origins);
    if (origins.empty() && state.selected.valid && state.selected.protocol == plpt::TransportProtocol::Tcp)
        origins.push_back(origin_for_service(state.selected));
    if (origins.empty()) {
        state.host_errors.push_back("aucune origine autorisee : choisissez un service dans la decouverte ou saisissez "
                                    "l'origine de l'application (ex. http://127.0.0.1:3000)");
        return false;
    }
    const std::string entry = state.entry_url[0] ? std::string(state.entry_url) : origins.front() + "/";

    // 2. Secret d'invitation genere par le moteur.
    auto code = plpt::generate_invite_code();
    if (!code) {
        state.host_errors.push_back("code d'invitation : " + describe(code.error()));
        return false;
    }

    // 3. Une seule interface pour le controle ET le media : resolue ici, une fois.
    plpt::IpAddress configured{};
    if (state.bind_address[0] != '\0') {
        auto parsed = plpt::IpAddress::parse(state.bind_address);
        if (!parsed) {
            state.host_errors.push_back("adresse d'ecoute : " + describe(parsed.error()));
            return false;
        }
        configured = parsed.value();
    }
    auto bind = plpt::resolve_bind_address(configured, plpt::IpFamily::Ipv4);
    if (!bind) {
        state.host_errors.push_back("interface d'ecoute : " + describe(bind.error()));
        return false;
    }

    plpt::share::ShareHostConfig share_config;
    share_config.mode = share_mode_of(state.mode);
    share_config.allowed_origins = origins;
    share_config.entry_url = entry;
    share_config.bind_address = bind.value().address;
    share_config.max_guests = static_cast<std::size_t>(std::clamp(state.max_participants, 1,
        static_cast<int>(plpt::share::kMaxShareGuests)));

    plpt::net::HostConfig config;
    config.session_name = state.session_name;
    config.host_name = state.host_name;
    config.bind_address = bind.value().address;
    config.control_port = 0; // l'OS choisit ; aucun TOCTOU
    config.max_participants = share_config.max_guests;
    config.approval_required = state.approval_required;
    config.invite_code = code.value();
    config.tls_required = true; // jamais de repli clair (ADR-016/017)

    state.share = std::make_unique<plpt::share::ShareHost>(std::make_unique<plpt::browser::WebView2Backend>());
    state.host = std::make_unique<plpt::net::HostServer>(state.clock);
    config.share = state.share.get();
    state.invite_code = code.value();
    state.host_launching = true;
    state.host_note = "demarrage du partage";
    state.host_launch = std::async(std::launch::async,
        [share = state.share.get(), host = state.host.get(), share_config, config]() -> LaunchErrors {
            auto shared = share->start(share_config);
            if (!shared) return {"partage applicatif : " + describe(shared.error())};
            auto started = host->start(config);
            if (!started) {
                share->stop();
                return {"serveur hote : " + describe(started.error())};
            }
            return {};
        });
    return true;
}

void stop_host_session(AppState& state, std::string reason) {
    if (state.host_launch.valid()) state.host_launch.wait(); // jamais d'objet detruit sous un lancement
    state.host_launching = false;
    if (state.announcer) { state.announcer->stop(); state.announcer.reset(); }
    if (state.host) { state.host->stop(); state.host.reset(); }
    // Coupe les canaux media, detruit chaque contexte et le processus navigateur.
    if (state.share) { state.share->stop(); state.share.reset(); }
    state.host_preview.reset();
    state.host_running = false;
    state.host_note = std::move(reason);
    state.invite_code = plpt::InviteCode{};
}

// Fin du lancement asynchrone : annonce LAN puis publication de l'etat actif.
void poll_host_launch(AppState& state) {
    if (!state.host_launching || !state.host_launch.valid()) return;
    if (state.host_launch.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    LaunchErrors errors = state.host_launch.get();
    state.host_launching = false;
    if (!errors.empty()) {
        state.host_errors = errors;
        state.host.reset();
        if (state.share) { state.share->stop(); state.share.reset(); }
        state.invite_code = plpt::InviteCode{};
        state.host_note = "demarrage refuse";
        state.notify("Demarrage refuse : voir les echecs dans la configuration");
        return;
    }
    auto snapshot = state.host->snapshot();
    plpt::net::Announcement announcement;
    announcement.session_id = snapshot->session_id;
    announcement.session_name = snapshot->session_name;
    announcement.host_name = snapshot->host_name;
    announcement.control_port = snapshot->control_port;
    announcement.participants = 0;
    announcement.max_participants = static_cast<std::uint16_t>(std::clamp(state.max_participants, 1,
        static_cast<int>(plpt::share::kMaxShareGuests)));
    announcement.shared_services = 1;
    announcement.accepting = true;
    announcement.wall_ms = state.clock.now_wall_ms();
    std::vector<plpt::NetworkInterfaceInfo> interface_list;
    if (state.interfaces) {
        if (auto enumerated = state.interfaces->enumerate()) interface_list = enumerated.value();
        else state.host_errors.push_back("interfaces reseau : " + describe(enumerated.error()));
    }
    auto targets = plpt::net::announce_targets(interface_list, announcement, plpt::Port{plpt::net::kDiscoveryPort}, true);
    state.announcer = std::make_unique<plpt::net::Announcer>(state.clock);
    if (auto announced = state.announcer->start(targets); !announced) {
        // L'annonce n'est qu'une commodite : l'invitation manuelle reste possible.
        state.host_errors.push_back("annonce LAN indisponible (invitation manuelle possible) : " + describe(announced.error()));
        state.announcer.reset();
    }
    state.host_running = state.host->running() && state.share && state.share->active();
    if (!state.host_running) {
        stop_host_session(state, "le moteur s'est arrete juste apres le demarrage");
        state.host_errors.push_back("le serveur hote ou le partage s'est arrete immediatement apres le demarrage");
        return;
    }
    state.host_note = "controle sur " + snapshot->control_address + ":" + std::to_string(snapshot->control_port) +
        " - " + snapshot->bind_note;
    state.tab = MainNavigationTab::ActiveSession;
    state.notify("Partage actif : navigateur reel, premiere image capturee, canal media TLS 1.3 ouvert");
}

// ---------------------------------------------------------------------------
// Demarrage transactionnel cote invite
// ---------------------------------------------------------------------------

void stop_guest_media(AppState& state) {
    if (state.media) { state.media->stop(); state.media.reset(); }
    state.media_offer_revision = 0;
    state.remote_capture = false;
    state.remote_buttons = 0;
    state.guest_view.reset();
}

bool start_guest_session(AppState& state) {
    state.guest_errors.clear();
    if (state.guest_running) {
        state.guest_errors.push_back("une connexion invite est deja en cours");
        return false;
    }
    auto address = plpt::IpAddress::parse(state.guest_address);
    if (!address) {
        state.guest_errors.push_back("adresse de l'hote : " + describe(address.error()));
        return false;
    }
    if (state.guest_port <= 0 || state.guest_port > 65535) {
        state.guest_errors.push_back("port de controle requis (1-65535) : aucune connexion sans port reel");
        return false;
    }
    auto decoded = plpt::decode_invite_code(state.guest_code);
    if (!decoded) {
        state.guest_errors.push_back("code d'invitation : " + describe(decoded.error()));
        return false;
    }

    plpt::net::GuestConfig config;
    config.address = address.value();
    config.control_port = static_cast<std::uint16_t>(state.guest_port);
    config.guest_name = state.guest_name;
    config.invite_code = plpt::InviteCode{decoded.value(), plpt::format_invite_code(decoded.value())};
    config.auto_reconnect = false; // jamais sans consentement explicite
    config.tls_required = true;

    state.guest = std::make_unique<plpt::net::GuestClient>(state.clock);
    auto started = state.guest->start(config);
    if (!started) {
        state.guest_errors.push_back("client invite : " + describe(started.error()));
        state.guest.reset();
        return false;
    }
    state.guest_host_ip = address.value();
    state.guest_running = state.guest->running();
    if (!state.guest_running) {
        state.guest_errors.push_back("le client invite ne tourne pas apres le demarrage");
        state.guest.reset();
        return false;
    }
    return true;
}

void stop_guest_session(AppState& state) {
    stop_guest_media(state);
    if (state.guest) {
        if (!state.guest->leave()) {
            state.guest_errors.push_back("depart volontaire non accepte par le moteur (arret force ensuite)");
        }
        state.guest->stop();
        state.guest.reset();
    }
    state.guest_running = false;
}

// Chaque frame : le canal media suit l'etat REEL du plan de controle. Une
// nouvelle offre (admission, reprise) remplace l'ancien canal ; une perte du
// controle coupe le media.
void tick_guest_media(AppState& state) {
    auto snapshot = state.guest ? state.guest->snapshot() : nullptr;
    if (!snapshot || snapshot->phase != plpt::net::GuestPhase::Connected) {
        if (state.media) stop_guest_media(state);
        return;
    }
    if (!snapshot->media_offered || snapshot->media_offer_revision == state.media_offer_revision) return;
    auto offer = state.guest->media_offer();
    if (!offer) return;
    stop_guest_media(state);
    plpt::share::ShareGuestConfig config;
    config.host = state.guest_host_ip;
    config.offer = *offer;
    config.session = plpt::SessionId{snapshot->session_id};
    config.self = snapshot->self;
    state.media = std::make_unique<plpt::share::ShareGuest>();
    auto started = state.media->start(config);
    state.media_offer_revision = snapshot->media_offer_revision;
    if (!started) {
        state.guest_errors.push_back("canal media : " + describe(started.error()));
        state.media.reset();
        return;
    }
    state.tab = MainNavigationTab::SharedView;
}

bool start_discovery_listener(AppState& state) {
    if (state.listener_running && state.listener) return true;
    plpt::net::DiscoveryListener::Config config;
    config.bind = plpt::IpAddress::any(plpt::IpFamily::Ipv4);
    config.port = plpt::Port{plpt::net::kDiscoveryPort};
    config.join_multicast = true;
    config.allow_broadcast = true;
    state.listener = std::make_unique<plpt::net::DiscoveryListener>(state.clock);
    auto started = state.listener->start(config);
    if (!started) {
        state.guest_errors.push_back("ecoute du reseau : " + describe(started.error()));
        state.listener.reset();
        return false;
    }
    state.listener_running = state.listener->running();
    return state.listener_running;
}

// ---------------------------------------------------------------------------
// Ecrans
// ---------------------------------------------------------------------------

void render_dashboard(AppState& state, const plpt::MonitorView& view) {
    using namespace devlink::ui;
    const auto& colors = theme_colors();

    text_section_title("Tableau de bord");
    text_muted("Etat reel du poste : ce qui est observe, ce qui tourne, ce qui manque.");
    ImGui::Spacing();

    std::size_t participants = 0;
    std::size_t pending = 0;
    std::uint16_t control_port = 0;
    if (state.host) {
        if (auto snapshot = state.host->snapshot()) {
            participants = snapshot->participants.size();
            pending = snapshot->pending.size();
            control_port = snapshot->control_port;
        }
    }

    metric_card("##metric_services", "Services observes", view.services.size(),
        view.complete ? "inventaire complet" : "inventaire partiel", pane_width(0.24f, 190.0f));
    ImGui::SameLine();
    metric_card("##metric_participants", "Participants admis", participants,
        state.host_running ? "session hote active" : "aucune session hote", pane_width(0.24f, 190.0f));
    ImGui::SameLine();
    metric_card("##metric_pending", "Demandes en attente", pending,
        state.approval_required ? "approbation explicite" : "admission automatique", pane_width(0.24f, 190.0f));
    ImGui::SameLine();
    metric_card("##metric_port", "Port de controle", control_port,
        control_port != 0 ? "lie par le moteur" : "aucun port lie", pane_width(0.24f, 190.0f));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const float left = pane_width(0.5f, 360.0f);
    if (card_begin("##dash_host_card", ImVec2(left, 0))) {
        devlink::ui::icons::icon(devlink::ui::icons::Icon::Setup, 15.0f, ImGui::ColorConvertFloat4ToU32(devlink::ui::theme_colors().accent_light));
        ImGui::SameLine();
        text_card_title("Partage (espace hote)");
        ImGui::Spacing();
        if (state.host_running) {
            status_pill("SESSION HOTE ACTIVE", colors.status_success);
            ImGui::Spacing();
            text_muted(state.host_note);
            ImGui::Spacing();
            ImGui::Text("Code d'invitation :");
            ImGui::SameLine();
            text_code(state.invite_code.text);
            if (state.share) {
                const auto share_view = state.share->snapshot();
                ImGui::Spacing();
                ImGui::Text("Application : %s - %zu invite(s) sur le flux media",
                    share_view.entry_url.c_str(), share_view.peers.size());
            }
            ImGui::Spacing();
            if (button_danger("Arreter la session", ImVec2(220, 34))) {
                stop_host_session(state, "session arretee par l'hote");
                state.notify("Session arretee : sockets fermees, secrets oublies");
            }
        } else {
            status_pill("AUCUN PARTAGE EN COURS", colors.text_disabled);
            ImGui::Spacing();
            text_muted(state.host_note);
            ImGui::Spacing();
            if (button_primary("Configurer un partage", ImVec2(240, 36))) {
                state.tab = MainNavigationTab::ShareSetup;
            }
        }
        if (!state.host_errors.empty()) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            devlink::ui::icons::icon(devlink::ui::icons::Icon::Cross, 15.0f, ImGui::ColorConvertFloat4ToU32(devlink::ui::theme_colors().accent_light));
        ImGui::SameLine();
        text_card_title("Derniers echecs reels");
            for (const auto& line : state.host_errors) {
                ImGui::TextWrapped("%s", line.c_str());
            }
        }
    }
    card_end();

    ImGui::SameLine();

    if (card_begin("##dash_engine_card", ImVec2(0, 0))) {
        devlink::ui::icons::icon(devlink::ui::icons::Icon::SharedView, 15.0f, ImGui::ColorConvertFloat4ToU32(devlink::ui::theme_colors().accent_light));
        ImGui::SameLine();
        text_card_title("Moteur navigateur");
        ImGui::Spacing();
        if (state.browser.real) {
            status_pill("MOTEUR REEL DISPONIBLE", colors.status_success);
        } else if (state.browser.demo_synthetic) {
            status_pill("MOTEUR SYNTHETIQUE - DEMONSTRATION", colors.status_warning);
        } else {
            status_pill("MOTEUR NAVIGATEUR INDISPONIBLE", colors.status_error);
        }
        ImGui::Spacing();
        text_muted(state.browser.status);
        ImGui::Spacing();
        if (!state.browser.usable_for_real_session()) {
            ImGui::TextWrapped(
                "Sans moteur reel, aucune session de partage applicatif distant ne peut "
                "etre demarree : les trames synthetiques ne sont pas un partage.");
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        devlink::ui::icons::icon(devlink::ui::icons::Icon::Lock, 15.0f, ImGui::ColorConvertFloat4ToU32(devlink::ui::theme_colors().accent_light));
        ImGui::SameLine();
        text_card_title("Transport");
        ImGui::Spacing();
        // Transport RÉEL de la session hôte, jamais une espérance (ADR-016).
        {
            const auto host_view = state.host ? state.host->snapshot() : nullptr;
            const std::string transport = host_view && !host_view->transport.empty()
                ? host_view->transport : std::string("aucune session demarree");
            const bool secure = transport.rfind("TLS 1.3", 0) == 0;
            status_pill(transport.c_str(), secure ? colors.status_success : colors.status_warning);
            ImGui::Spacing();
            if (secure) {
                ImGui::TextWrapped(
                    "Canal TLS 1.3 a secret partage : chaque pair admis a prouve le secret de "
                    "session par la poignee de main ; l'upgrade a lieu a l'admission, avant toute "
                    "trame utile (ADR-016).");
            } else {
                ImGui::TextWrapped(
                    "Session NON chiffree : TLS 1.3 indisponible ou desactive par configuration "
                    "(tls_required=false). Aucun pair n'est annonce securise.");
            }
            if (host_view) {
                for (const auto& peer : host_view->participants) {
                    if (!peer.transport.empty())
                        ImGui::Text("  - %s : %s", peer.name.c_str(), peer.transport.c_str());
                }
            }
        }
        ImGui::Spacing();
        if (button_secondary("Diagnostics detailles", ImVec2(220, 32))) {
            state.tab = MainNavigationTab::Diagnostics;
        }
    }
    card_end();
}

void render_discovery_center(AppState& state, const plpt::MonitorView& view) {
    using namespace devlink::ui;
    const auto& colors = theme_colors();

    text_section_title("Centre de decouverte locale");
    text_muted("Observation passive des processus et des sockets en écoute. "
               "Le registre est une observation, jamais une autorisation.");
    ImGui::Spacing();

    ImGui::Text("Revision %llu - inventaire %s - %zu avertissement(s)",
        static_cast<unsigned long long>(view.revision),
        view.complete ? "complet" : "partiel", view.warnings.size());

    static ImGuiTextFilter filter;
    filter.Draw("Filtrer (nom, port, PID)", 320.0f);
    ImGui::SameLine();
    if (button_secondary("Actualiser l'inventaire", ImVec2(220, 30))) {
        state.monitor_refresh_accepted = state.monitor ? state.monitor->request_refresh() : false;
        state.notify(state.monitor_refresh_accepted
            ? "Rafraichissement demande au moniteur"
            : "Rafraichissement refuse : le moniteur ne tourne pas");
    }

    if (!view.warnings.empty()) {
        if (card_begin("##discovery_warnings", ImVec2(0, 0))) {
            devlink::ui::icons::icon(devlink::ui::icons::Icon::Warning, 15.0f, ImGui::ColorConvertFloat4ToU32(devlink::ui::theme_colors().accent_light));
            ImGui::SameLine();
            text_card_title("Avertissements du moniteur");
            for (const auto& warning : view.warnings) {
                ImGui::TextWrapped("%s", describe(warning).c_str());
            }
        }
        card_end();
    }

    ImGui::Spacing();

    if (view.services.empty()) {
        empty_state("Aucun service observe",
            "Aucun processus en écoute sur les tables TCP/UDP accessibles.");
        return;
    }

    if (!ImGui::BeginTable("##discovery_table", 8,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp,
            ImVec2(0, 420))) {
        ImGui::EndTable();
        return;
    }
    ImGui::TableSetupColumn("Service");
    ImGui::TableSetupColumn("Port");
    ImGui::TableSetupColumn("Proto");
    ImGui::TableSetupColumn("PID");
    ImGui::TableSetupColumn("Origine probable");
    ImGui::TableSetupColumn("Confiance");
    ImGui::TableSetupColumn("Etat");
    ImGui::TableSetupColumn("Action");
    ImGui::TableHeadersRow();

    for (const auto& service : view.services) {
        const std::string process_name =
            service.process ? service.process->image_name : std::string("processus inconnu");
        const std::string pid_text =
            service.process ? std::to_string(service.process->pid.value) : std::string("?");
        for (const auto& endpoint : service.endpoints) {
            const std::string port_text = std::to_string(endpoint.local.port.value);
            const std::string haystack = process_name + " " + port_text + " " + pid_text;
            if (!filter.PassFilter(haystack.c_str())) continue;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            text_card_title(process_name);

            ImGui::TableNextColumn();
            text_code(port_text);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(endpoint.protocol == plpt::TransportProtocol::Tcp ? "TCP" : "UDP");

            ImGui::TableNextColumn();
            const std::string owner_pid =
                endpoint.owner_pid ? std::to_string(endpoint.owner_pid->value) : std::string("?");
            ImGui::TextUnformatted(owner_pid.c_str());

            ImGui::TableNextColumn();
            text_muted(service.origin.label);

            ImGui::TableNextColumn();
            const std::string_view confidence = plpt::to_string(service.origin.confidence);
            ImGui::Text("%.*s", static_cast<int>(confidence.size()), confidence.data());

            ImGui::TableNextColumn();
            status_pill(plpt::to_string(service.share_state), colors.accent_light);

            ImGui::TableNextColumn();
            // Seul un point d'écoute TCP peut être partagé par le proxy : un UDP
            // ou un socket connecté n'est pas un service local pontable.
            const bool shareable = endpoint.protocol == plpt::TransportProtocol::Tcp;
            const std::string button_id = "Choisir##" + port_text + "_" + pid_text;
            if (!shareable) {
                text_disabled_label("non pontable");
            } else if (button_secondary(button_id.c_str(), ImVec2(120, 24))) {
                SelectedService chosen;
                chosen.valid = true;
                chosen.service_id = service.service_id;
                chosen.label = process_name;
                chosen.address = endpoint.local.address;
                chosen.port = endpoint.local.port.value;
                chosen.protocol = endpoint.protocol;
                state.selected = chosen;
                // Proposition d'origine VISIBLE et modifiable avant le demarrage :
                // l'hote confirme explicitement ce que le navigateur pourra ouvrir.
                std::snprintf(state.allowed_origins, sizeof(state.allowed_origins), "%s",
                    origin_for_service(chosen).c_str());
                state.entry_url[0] = '\0';
                // Effet reel dans le moteur : la selection est enregistrée par le
                // registre (locale, sans effet reseau), pas seulement dans l'UI.
                if (state.monitor && !state.monitor->request_selection(service.service_id, endpoint.local)) {
                    state.notify("Selection refusee par le moniteur (inventaire partiel ?)");
                } else {
                    state.notify("Service selectionne : " + process_name + " :" + port_text);
                }
                state.tab = MainNavigationTab::ShareSetup;
            }
        }
    }
    ImGui::EndTable();
}

void render_share_setup(AppState& state) {
    using namespace devlink::ui;
    const auto& colors = theme_colors();

    text_section_title("Configuration du partage applicatif");
    text_muted("Autorisation explicite, amont reel verifie, démarrage transactionnel.");
    ImGui::Spacing();

    if (card_begin("##setup_card", ImVec2(pane_width(0.62f, 520.0f), 0))) {
        text_section_title("1. Parametres de session");
        ImGui::InputText("Nom de la session", state.session_name, sizeof(state.session_name));
        ImGui::InputText("Nom de l'hote", state.host_name, sizeof(state.host_name));
        ImGui::InputText("Adresse d'ecoute (vide = DevLink choisit)", state.bind_address, sizeof(state.bind_address));
        text_disabled_label("Jamais toutes les interfaces par defaut : une interface privee LAN "
                            "reellement presente, sinon la boucle locale, avec la raison affichee.");
        ImGui::SliderInt("Capacite cible d'invites", &state.max_participants, 1,
            static_cast<int>(plpt::share::kMaxShareGuests));
        text_disabled_label("Admission adaptative : DevLink suspend les nouvelles arrivees si un flux media est reellement sature.");
        ImGui::Checkbox("Exiger l'approbation explicite de chaque invite", &state.approval_required);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        text_section_title("2. Service local partage");
        if (state.selected.valid) {
            status_pill("AMONT SELECTIONNE", colors.status_success);
            ImGui::SameLine();
            ImGui::Text("%s - %s:%u", state.selected.label.c_str(), state.selected.address.c_str(),
                static_cast<unsigned int>(state.selected.port));
        } else {
            status_pill("AUCUN SERVICE SELECTIONNE", colors.status_warning);
            ImGui::Spacing();
            ImGui::TextWrapped("Choisissez le serveur de votre application dans la decouverte, ou saisissez "
                "son origine ci-dessous. Rien n'est partage sans cette selection explicite.");
            if (button_secondary("Choisir dans la decouverte", ImVec2(260, 32))) {
                state.tab = MainNavigationTab::DiscoveryCenter;
            }
        }
        ImGui::Spacing();
        ImGui::InputText("Origines autorisees du navigateur (separees par des virgules)",
            state.allowed_origins, sizeof(state.allowed_origins));
        text_disabled_label("Allowlist stricte : file:, javascript:, data:, les redirections et les "
                            "hotes:ports non listes sont refuses ; le navigateur ne sort que par la garde "
                            "reseau DevLink (aucun autre port local, ni base de donnees, ni Docker).");
        ImGui::InputText("URL d'entree (vide = premiere origine)", state.entry_url, sizeof(state.entry_url));

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        text_section_title("3. Mode de participation");
        int mode_value = static_cast<int>(state.mode);
        ImGui::RadioButton("Mode A - test independant (profils distincts)", &mode_value, 0);
        ImGui::RadioButton("Mode B - collaboration sur une vue (controle transferable)", &mode_value, 1);
        ImGui::RadioButton("Mode C - observation seule (entree refusee)", &mode_value, 2);
        state.mode = static_cast<ParticipationMode>(mode_value);
        text_muted(mode_capability_note(state.mode));
        text_disabled_label("Le mode est applique cote serveur par le moteur de session : "
                            "l'interface ne peut pas l'imposer ni le contourner.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (state.host_launching) {
            const auto view = state.share ? state.share->snapshot() : plpt::share::ShareSnapshot{};
            status_pill("DEMARRAGE EN COURS", colors.status_info);
            ImGui::SameLine();
            ImGui::Text("Etape reelle : %s", plpt::share::share_stage_text(view.stage));
            text_disabled_label("Actif seulement apres : navigateur reel cree, application chargee, premiere "
                                "image capturee, canal media TLS 1.3 ouvert, plan de controle lie.");
        } else if (state.host_running) {
            status_pill("SESSION DEJA ACTIVE", colors.status_warning);
            ImGui::SameLine();
            if (button_secondary("Aller a la session active", ImVec2(240, 32))) {
                state.tab = MainNavigationTab::ActiveSession;
            }
        } else {
            const bool engine_ready = state.browser.usable_for_real_session();
            if (!engine_ready) {
                status_pill("MOTEUR NAVIGATEUR INDISPONIBLE", colors.status_error);
                ImGui::Spacing();
                ImGui::TextWrapped("Partage applicatif bloqué : %s. Aucune session hôte ne sera "
                    "démarrée tant que le navigateur réel, la capture et l'injection ne sont pas opérationnels.",
                    state.browser.status.c_str());
                ImGui::Spacing();
            }
            ImGui::BeginDisabled(!engine_ready);
            const bool start_clicked = button_primary(engine_ready ? "Démarrer le partage" : "Partage indisponible",
                ImVec2(280, 40));
            ImGui::EndDisabled();
            if (start_clicked) {
                if (start_host_session(state)) {
                    state.notify("Demarrage du partage : navigateur reel en preparation");
                } else {
                    state.notify("Démarrage refusé : voir les échecs ci-dessous");
                }
            }
        }
    }
    card_end();

    if (!state.host_errors.empty()) {
        ImGui::Spacing();
        if (card_begin("##setup_errors", ImVec2(pane_width(0.62f, 520.0f), 0))) {
            text_card_title("Echecs reels du dernier démarrage");
            for (const auto& line : state.host_errors) {
                ImGui::TextWrapped("%s", line.c_str());
            }
        }
        card_end();
    }
}

void render_approval_queue(AppState& state) {
    using namespace devlink::ui;
    const auto& colors = theme_colors();

    text_section_title("File d'approbation");
    text_muted("Chaque demande d'admission est decidee par l'hote : autoriser, refuser "
               "avec une raison, ou ne rien faire jusqu'a l'expiration.");
    ImGui::Spacing();

    if (!state.host_running || !state.host) {
        empty_state("Aucune session hote", "La file d'approbation n'existe que pendant une session active.");
        return;
    }
    auto snapshot = state.host->snapshot();
    if (!snapshot) {
        empty_state("Instantane indisponible", "Le moteur de session n'a pas encore publie d'etat.");
        return;
    }

    ImGui::Text("Approbation explicite : %s - expiration d'une demande : %u ms",
        snapshot->pending.empty() && state.approval_required ? "active" : "voir configuration",
        120000U);

    if (snapshot->pending.empty()) {
        empty_state("Aucune demande en attente",
            "Les invitations apparaissent ici des qu'un invite presente un code valide.");
        return;
    }

    if (!ImGui::BeginTable("##pending_table", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::EndTable();
        return;
    }
    ImGui::TableSetupColumn("Demande");
    ImGui::TableSetupColumn("Invite");
    ImGui::TableSetupColumn("Adresse");
    ImGui::TableSetupColumn("Client");
    ImGui::TableSetupColumn("Age");
    ImGui::TableSetupColumn("Decision");
    ImGui::TableHeadersRow();

    const std::uint64_t now = state.clock.now_monotonic_ms();
    for (const auto& request : snapshot->pending) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        text_code(std::to_string(request.request_id));
        ImGui::TableNextColumn();
        text_card_title(request.guest_name);
        ImGui::TableNextColumn();
        ImGui::Text("%s:%u", request.address.c_str(), static_cast<unsigned int>(request.port));
        ImGui::TableNextColumn();
        text_muted(request.client);
        ImGui::TableNextColumn();
        const std::uint64_t age = (now > request.requested_ms) ? now - request.requested_ms : 0;
        ImGui::Text("%llu ms", static_cast<unsigned long long>(age));
        ImGui::TableNextColumn();
        const std::string approve_id = "Autoriser##" + std::to_string(request.request_id);
        const std::string deny_id = "Refuser##" + std::to_string(request.request_id);
        if (button_primary(approve_id.c_str(), ImVec2(110, 26))) {
            const bool accepted = state.host->approve(request.request_id);
            state.notify(accepted ? "Demande autorisee par l'hote"
                                  : "Autorisation refusee par le moteur (demande expirée ?)");
        }
        ImGui::SameLine();
        if (button_danger(deny_id.c_str(), ImVec2(100, 26))) {
            const bool accepted = state.host->deny(request.request_id, "refus manuel de l'hote");
            state.notify(accepted ? "Demande refusee et raison transmise"
                                  : "Refus non applique par le moteur");
        }
    }
    ImGui::EndTable();
    ImGui::Spacing();
    status_pill("DECISION HUMAINE REQUISE", colors.status_warning);
    ImGui::SameLine();
    text_disabled_label("Aucune admission automatique : l'hote decide de chaque place.");
}

void render_active_session(AppState& state) {
    using namespace devlink::ui;
    const auto& colors = theme_colors();

    text_section_title("Session active et controle des participants");
    ImGui::Spacing();

    if (!state.host_running || !state.host) {
        empty_state("Aucune session active",
            "Demarrez une session depuis la configuration du partage.");
        if (button_secondary("Aller a la configuration", ImVec2(240, 32))) {
            state.tab = MainNavigationTab::ShareSetup;
        }
        return;
    }
    auto snapshot = state.host->snapshot();
    if (!snapshot) {
        empty_state("Instantane indisponible", "Le moteur de session n'a pas encore publie d'etat.");
        return;
    }

    if (card_begin("##session_banner", ImVec2(0, 0))) {
        ImGui::Text("Session :");
        ImGui::SameLine();
        text_card_title(snapshot->session_name);
        ImGui::SameLine();
        status_pill(plpt::net::session_state_text(snapshot->state), colors.status_success);
        if (state.share) {
            // Mode RÉELLEMENT appliqué par le moteur de partage, pas le formulaire.
            const auto applied = state.share->snapshot().mode;
            const ParticipationMode shown = applied == plpt::share::ShareMode::Independent
                ? ParticipationMode::Independent
                : applied == plpt::share::ShareMode::Collaborative ? ParticipationMode::Collaborative
                                                                    : ParticipationMode::Observation;
            ImGui::Text("Mode appliqué :");
            ImGui::SameLine();
            text_code(mode_label(shown));
        }
        ImGui::Spacing();
        ImGui::Text("Code d'invitation :");
        ImGui::SameLine();
        text_code(snapshot->invite_code);
        ImGui::SameLine();
        if (button_secondary("Copier", ImVec2(90, 26))) {
            const bool copied = SDL_SetClipboardText(snapshot->invite_code.c_str());
            state.notify(copied ? "Code d'invitation copie dans le presse-papier"
                                : "Copie impossible : presse-papier indisponible");
        }
        ImGui::Spacing();
        ImGui::Text("Point de controle : %s:%u - %s", snapshot->control_address.c_str(),
            static_cast<unsigned int>(snapshot->control_port), snapshot->bind_note.c_str());
        if (snapshot->any_interface) {
            ImGui::Spacing();
            status_pill("ECOUTE SUR TOUTES LES INTERFACES", colors.status_warning);
        }
        if (snapshot->loopback_only) {
            ImGui::Spacing();
            status_pill("AUCUNE INTERFACE LAN : SESSION LOCALE", colors.status_info);
        }
    }
    card_end();

    ImGui::Spacing();

    const float left = pane_width(0.56f, 420.0f);
    if (card_begin("##participants_card", ImVec2(left, 0))) {
        text_card_title("Participants");
        ImGui::Spacing();
        if (snapshot->participants.empty()) {
            text_muted("Aucun participant admis. Une admission exige un code valide "
                       "puis, si l'approbation est active, une decision de l'hote.");
        } else {
            for (const auto& participant : snapshot->participants) {
                ImGui::Text("%s", participant.name.c_str());
                ImGui::SameLine(220);
                status_pill(plpt::net::participant_state_text(participant.state), colors.accent_light);
                ImGui::SameLine(340);
                text_muted(participant.address);
                ImGui::SameLine(470);
                ImGui::Text("%u ms", static_cast<unsigned int>(participant.latency_ms));
                const std::string revoke_id = "Revoquer##" + std::to_string(participant.id.value);
                ImGui::SameLine();
                if (button_danger(revoke_id.c_str(), ImVec2(110, 24))) {
                    const bool accepted = state.host->revoke(participant.id);
                    state.notify(accepted ? "Revocation transmise au moteur"
                                          : "Revocation refusee par le moteur");
                }
            }
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        const auto& counters = snapshot->counters;
        ImGui::Text("Connexions %llu - admises %llu - reprises %llu - partis %llu - refusees %llu - revoquees %llu",
            static_cast<unsigned long long>(counters.connections_total),
            static_cast<unsigned long long>(counters.admitted),
            static_cast<unsigned long long>(counters.resumed),
            static_cast<unsigned long long>(counters.left),
            static_cast<unsigned long long>(counters.denied),
            static_cast<unsigned long long>(counters.revoked));
        ImGui::Text("Codes invalides %llu - limites %llu - trames invalides %llu - chat relaye %llu - timeouts %llu",
            static_cast<unsigned long long>(counters.bad_codes),
            static_cast<unsigned long long>(counters.rate_limited),
            static_cast<unsigned long long>(counters.invalid_frames),
            static_cast<unsigned long long>(counters.chat_relayed),
            static_cast<unsigned long long>(counters.timeouts));
        ImGui::Spacing();
        if (snapshot->pending.empty()) {
            text_disabled_label("Aucune demande en attente.");
        } else if (button_secondary("Ouvrir la file d'approbation", ImVec2(280, 32))) {
            state.tab = MainNavigationTab::ApprovalQueue;
        }
    }
    card_end();

    ImGui::SameLine();

    if (card_begin("##share_card", ImVec2(0, 0))) {
        text_card_title("Partage applicatif reel");
        ImGui::Spacing();
        if (!state.share) {
            text_muted("Aucun moteur de partage.");
        } else {
            const auto share = state.share->snapshot();
            const bool active = share.stage == plpt::share::ShareStage::Active;
            status_pill(plpt::share::share_stage_text(share.stage), active ? colors.status_success : colors.status_error);
            ImGui::SameLine();
            status_pill(plpt::share::share_mode_text(share.mode), colors.accent_light);
            if (share.paused) {
                ImGui::SameLine();
                status_pill("ENTREES SUSPENDUES", colors.status_warning);
            }
            ImGui::Spacing();
            ImGui::TextWrapped("Application : %s", share.entry_url.c_str());
            std::string origins;
            for (const auto& o : share.origins) origins += (origins.empty() ? "" : ", ") + o;
            ImGui::TextWrapped("Origines autorisees : %s", origins.c_str());
            ImGui::Text("Canal media TLS 1.3 : port %u - premiere image en %llu ms - poignees ok %llu / refusees %llu",
                static_cast<unsigned int>(share.media_port), static_cast<unsigned long long>(share.first_frame_ms),
                static_cast<unsigned long long>(share.handshakes_ok), static_cast<unsigned long long>(share.handshakes_failed));
            ImGui::Text("Navigateur : contextes crees %llu - detruits %llu - images %llu - HTTP %d",
                static_cast<unsigned long long>(share.contexts_created),
                static_cast<unsigned long long>(share.contexts_destroyed),
                static_cast<unsigned long long>(share.primary.frames), share.primary.last_http_status);
            ImGui::Text("Bloques par la politique : navigations %llu - requetes %llu - fenetres %llu - telechargements %llu - permissions %llu",
                static_cast<unsigned long long>(share.primary.blocked_navigations),
                static_cast<unsigned long long>(share.primary.blocked_requests),
                static_cast<unsigned long long>(share.primary.blocked_popups),
                static_cast<unsigned long long>(share.primary.blocked_downloads),
                static_cast<unsigned long long>(share.primary.denied_permissions));
            ImGui::TextWrapped("Page courante (hote uniquement) : %s", share.primary.current_url.c_str());
            if (share.mode == plpt::share::ShareMode::Independent) {
                text_disabled_label("Mode A : profils navigateur distincts (cookies, stockage). Le backend de "
                                    "l'application (base de donnees, API) reste COMMUN a tous les invites.");
            }
            ImGui::Spacing();
            if (share.mode == plpt::share::ShareMode::Collaborative) {
                std::string holder = "personne (vue en lecture pour tous)";
                for (const auto& p : share.peers)
                    if (p.id == share.controller) holder = p.name;
                ImGui::Text("Controle exclusif : %s - en attente : %zu", holder.c_str(), share.waiting.size());
                if (share.controller.value != 0 && button_secondary("Reprendre le controle", ImVec2(220, 28))) {
                    state.notify(state.share->grant_control(plpt::ParticipantId{})
                        ? "Controle repris : plus aucun invite ne peut agir" : "Refuse par le moteur");
                }
            }
            if (share.mode != plpt::share::ShareMode::ObserveOnly) {
                if (share.controller.value != 0) ImGui::SameLine();
                if (button_secondary(share.paused ? "Reprendre les entrees" : "Suspendre les entrees", ImVec2(220, 28))) {
                    state.share->set_paused(!share.paused);
                }
            }
            ImGui::Spacing();
            if (share.peers.empty()) {
                text_muted("Aucun invite sur le flux media.");
            } else if (ImGui::BeginTable("##share_peers", 6,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Invite");
                ImGui::TableSetupColumn("Flux");
                ImGui::TableSetupColumn("Images / debit");
                ImGui::TableSetupColumn("Entrees acceptees / refusees");
                ImGui::TableSetupColumn("Dernier refus");
                ImGui::TableSetupColumn("Action");
                ImGui::TableHeadersRow();
                for (const auto& p : share.peers) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", p.name.c_str());
                    ImGui::TableNextColumn();
                    status_pill(p.media_connected ? (p.context_ready ? "RECOIT" : "PREPARATION") : "DECONNECTE",
                        p.media_connected ? colors.status_success : colors.text_disabled);
                    if (p.network_backpressured) {
                        ImGui::SameLine();
                        status_pill("RESEAU SATURE", colors.status_warning);
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu - %.1f i/s - %.1f Mo", static_cast<unsigned long long>(p.frames_sent), p.send_fps,
                        static_cast<double>(p.bytes_sent) / 1e6);
                    if (p.network_backpressured) ImGui::TextDisabled("bloque depuis %llums", static_cast<unsigned long long>(p.backpressure_ms));
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu / %llu%s", static_cast<unsigned long long>(p.inputs_accepted),
                        static_cast<unsigned long long>(p.inputs_rejected), p.may_input ? " (peut agir)" : "");
                    ImGui::TableNextColumn();
                    text_muted(p.last_rejection.empty() ? std::string("-") : p.last_rejection);
                    ImGui::TableNextColumn();
                    if (share.mode == plpt::share::ShareMode::Collaborative && share.controller != p.id) {
                        const std::string give = "Donner le controle##" + std::to_string(p.id.value);
                        if (button_secondary(give.c_str(), ImVec2(170, 24))) {
                            state.notify(state.share->grant_control(p.id) ? "Controle transfere" : "Transfert refuse par le moteur");
                        }
                    } else {
                        text_disabled_label("-");
                    }
                }
                ImGui::EndTable();
            }
        }
    }
    card_end();

    if (!snapshot->warnings.empty()) {
        if (card_begin("##session_warnings", ImVec2(0, 0))) {
            text_card_title("Avertissements de la session");
            for (const auto& warning : snapshot->warnings) {
                ImGui::TextWrapped("%s", describe(warning).c_str());
            }
        }
        card_end();
    }

    if (!snapshot->events.empty()) {
        if (card_begin("##session_events", ImVec2(0, 0))) {
            text_card_title("Evenements recents");
            const std::size_t first = snapshot->events.size() > 12 ? snapshot->events.size() - 12 : 0;
            for (std::size_t index = first; index < snapshot->events.size(); ++index) {
                text_muted(snapshot->events[index]);
            }
        }
        card_end();
    }

    ImGui::Spacing();
    if (button_danger("Arreter la session", ImVec2(220, 36))) {
        stop_host_session(state, "session arretee par l'hote");
        state.notify("Session arretee : sockets fermees, secrets oublies");
    }
}

// ---------------------------------------------------------------------------
// Vue partagee : vraies images du navigateur hote, entrees renvoyees a l'hote
// ---------------------------------------------------------------------------

std::uint16_t media_modifiers(SDL_Keymod mod) {
    std::uint16_t m = 0;
    if (mod & SDL_KMOD_SHIFT) m |= 0x1;
    if (mod & SDL_KMOD_CTRL) m |= 0x2;
    if (mod & SDL_KMOD_ALT) m |= 0x4;
    if (mod & SDL_KMOD_GUI) m |= 0x8;
    if (mod & SDL_KMOD_CAPS) m |= 0x10;
    return m;
}

std::uint16_t imgui_modifiers() {
    const ImGuiIO& io = ImGui::GetIO();
    std::uint16_t m = 0;
    if (io.KeyShift) m |= 0x1;
    if (io.KeyCtrl) m |= 0x2;
    if (io.KeyAlt) m |= 0x4;
    if (io.KeySuper) m |= 0x8;
    return m;
}

// Touche SDL -> code de touche virtuel (celui qu'attend le navigateur hote).
std::uint32_t virtual_key_of(SDL_Keycode key) {
    if (key >= SDLK_A && key <= SDLK_Z) return 'A' + static_cast<std::uint32_t>(key - SDLK_A);
    if (key >= SDLK_0 && key <= SDLK_9) return '0' + static_cast<std::uint32_t>(key - SDLK_0);
    if (key >= SDLK_F1 && key <= SDLK_F12) return 0x70 + static_cast<std::uint32_t>(key - SDLK_F1);
    switch (key) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return 0x0D;
    case SDLK_BACKSPACE: return 0x08;
    case SDLK_TAB: return 0x09;
    case SDLK_ESCAPE: return 0x1B;
    case SDLK_SPACE: return 0x20;
    case SDLK_PAGEUP: return 0x21;
    case SDLK_PAGEDOWN: return 0x22;
    case SDLK_END: return 0x23;
    case SDLK_HOME: return 0x24;
    case SDLK_LEFT: return 0x25;
    case SDLK_UP: return 0x26;
    case SDLK_RIGHT: return 0x27;
    case SDLK_DOWN: return 0x28;
    case SDLK_INSERT: return 0x2D;
    case SDLK_DELETE: return 0x2E;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10;
    case SDLK_LCTRL: case SDLK_RCTRL: return 0x11;
    case SDLK_LALT: case SDLK_RALT: return 0x12;
    case SDLK_CAPSLOCK: return 0x14;
    case SDLK_LGUI: case SDLK_RGUI: return 0x5B;
    default: return 0;
    }
}

bool remote_input_allowed(const AppState& state) {
    if (!state.media) return false;
    const auto s = state.media->snapshot();
    return s.phase == plpt::share::MediaPhase::Streaming && s.have_state && s.state.you_may_input;
}

void release_remote_capture(AppState& state) {
    if (!state.remote_capture) return;
    state.remote_capture = false;
    if (state.media) {
        plpt::media::InputEvent lost;
        lost.type = plpt::media::InputEventType::FocusLost;
        (void)state.media->send_input(lost);
    }
    if (state.window) (void)SDL_StopTextInput(state.window);
}

// Evenements clavier/texte SDL pendant la capture : renvoyes a l'hote, jamais a
// ImGui (sinon Tab/fleches pilotent aussi l'interface locale). Retourne true si
// l'evenement a ete consomme par la vue distante.
bool forward_remote_keyboard(AppState& state, const SDL_Event& event) {
    if (!state.remote_capture || !state.media) return false;
    if (event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP && event.type != SDL_EVENT_TEXT_INPUT) return false;
    if (!remote_input_allowed(state)) {
        release_remote_capture(state);
        return false;
    }
    if (event.type == SDL_EVENT_TEXT_INPUT) {
        plpt::share::split_utf8(event.text.text, plpt::media::kMaxInputTextBytes, [&](const char* p, std::size_t n) {
            plpt::media::InputEvent e;
            e.type = plpt::media::InputEventType::TextInput;
            e.text.assign(p, n);
            (void)state.media->send_input(e);
        });
        return true;
    }
    const std::uint32_t vk = virtual_key_of(event.key.key);
    if (vk == 0) return true; // touche sans equivalent : ignoree, pas transmise a ImGui non plus
    plpt::media::InputEvent e;
    e.type = event.type == SDL_EVENT_KEY_DOWN ? plpt::media::InputEventType::KeyDown : plpt::media::InputEventType::KeyUp;
    e.key_code = vk;
    e.modifiers = media_modifiers(event.key.mod);
    (void)state.media->send_input(e);
    return true;
}

// Dessine `texture` en letterbox dans tout l'espace restant ; retourne le
// rectangle reellement occupe par l'image (coordonnees ecran ImGui).
plpt::share::ViewRect draw_remote_texture(const RemoteTexture& texture, const char* id) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const plpt::share::ViewRect area{origin.x, origin.y, avail.x, std::max(avail.y, 120.0f)};
    const auto rect = plpt::share::fit_letterbox(area, texture.width, texture.height);
    ImGui::InvisibleButton(id, ImVec2(area.width, area.height),
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(area.x, area.y), ImVec2(area.x + area.width, area.y + area.height), IM_COL32(0, 0, 0, 255));
    if (texture.texture && rect.width > 0) {
        draw->AddImage(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture.texture)),
            ImVec2(rect.x, rect.y), ImVec2(rect.x + rect.width, rect.y + rect.height));
    }
    return rect;
}

// Souris : coordonnees normalisees de l'IMAGE (letterbox, taille du panneau et
// DPI neutralises) ; l'hote les reporte sur son viewport CSS.
void forward_remote_mouse(AppState& state, const plpt::share::ViewRect& rect, bool hovered) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool allowed = remote_input_allowed(state);
    const bool held = state.remote_buttons != 0;
    // Un clic hors de la vue rend le clavier a l'interface locale.
    if (state.remote_capture && !hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
        release_remote_capture(state);
    if (!allowed) {
        state.remote_buttons = 0;
        if (state.remote_capture) release_remote_capture(state);
        return;
    }
    const auto point = plpt::share::to_normalized(rect, io.MousePos.x, io.MousePos.y, held);
    if (!point) return;
    auto send = [&](plpt::media::InputEventType type, std::uint8_t button = 0, std::int16_t wheel = 0) {
        plpt::media::InputEvent e;
        e.type = type;
        e.x_norm = point->x;
        e.y_norm = point->y;
        e.button = button;
        e.wheel_delta = wheel;
        e.modifiers = imgui_modifiers();
        (void)state.media->send_input(e);
    };
    if (point->x != state.remote_last_x || point->y != state.remote_last_y) {
        send(plpt::media::InputEventType::MouseMove);
        state.remote_last_x = point->x;
        state.remote_last_y = point->y;
    }
    // ImGui : 0 gauche, 1 droit, 2 milieu -> protocole : 1 gauche, 3 droit, 2 milieu.
    static constexpr std::uint8_t kButtonOf[3] = {1, 3, 2};
    for (int b = 0; b < 3; ++b) {
        const std::uint8_t bit = static_cast<std::uint8_t>(1u << b);
        if (hovered && ImGui::IsMouseClicked(b) && !(state.remote_buttons & bit)) {
            state.remote_buttons |= bit;
            if (!state.remote_capture) {
                state.remote_capture = true;
                if (state.window) (void)SDL_StartTextInput(state.window);
            }
            send(plpt::media::InputEventType::MouseDown, kButtonOf[b]);
        }
        if ((state.remote_buttons & bit) && ImGui::IsMouseReleased(b)) {
            state.remote_buttons = static_cast<std::uint8_t>(state.remote_buttons & ~bit);
            send(plpt::media::InputEventType::MouseUp, kButtonOf[b]);
        }
    }
    if (hovered && io.MouseWheel != 0.0f) {
        const float notches = std::clamp(io.MouseWheel, -10.0f, 10.0f);
        send(plpt::media::InputEventType::MouseWheel, 0, static_cast<std::int16_t>(notches * 120.0f));
    }
}

void render_guest_remote_view(AppState& state) {
    using namespace devlink::ui;
    const auto& colors = theme_colors();
    if (!state.media) {
        status_pill("AUCUN FLUX", colors.text_disabled);
        ImGui::Spacing();
        auto snapshot = state.guest ? state.guest->snapshot() : nullptr;
        if (!snapshot) {
            text_muted("Rejoignez une session : la vue de l'application apparait ici apres admission par l'hote.");
        } else if (snapshot->phase == plpt::net::GuestPhase::Connected && !snapshot->media_offered) {
            text_muted("Admis par l'hote, en attente de l'offre du canal media (hote sans partage actif ?).");
        } else {
            ImGui::TextWrapped("Plan de controle : %s - %s", plpt::net::guest_phase_text(snapshot->phase),
                snapshot->status_text.c_str());
        }
        return;
    }
    const auto ms = state.media->snapshot();
    if (auto image = state.media->latest(state.guest_view.sequence)) {
        if (!state.guest_view.update(image->image, image->sequence)) {
            state.guest_errors.push_back(std::string("texture SDL : ") + SDL_GetError());
        }
    }
    // Bandeau d'etat : uniquement ce que le moteur a confirme.
    const bool streaming = ms.phase == plpt::share::MediaPhase::Streaming;
    status_pill(streaming ? (ms.frames ? "FLUX REEL" : "CONNECTE - ATTENTE IMAGE") : plpt::share::media_phase_text(ms.phase),
        streaming ? colors.status_success : colors.status_error);
    ImGui::SameLine();
    if (!ms.transport.empty()) status_pill(ms.transport.c_str(), colors.status_success);
    if (ms.have_state) {
        ImGui::SameLine();
        status_pill(plpt::share::share_mode_text(ms.state.mode), colors.accent_light);
        ImGui::SameLine();
        if (ms.state.paused) status_pill("ENTREES SUSPENDUES PAR L'HOTE", colors.status_warning);
        else if (ms.state.you_may_input) status_pill(state.remote_capture ? "VOUS PILOTEZ (clavier capture)" : "VOUS POUVEZ PILOTER", colors.status_success);
        else if (ms.state.mode == plpt::share::ShareMode::ObserveOnly) status_pill("OBSERVATION SEULE", colors.status_info);
        else if (ms.state.you_waiting) status_pill("EN ATTENTE DU CONTROLE", colors.status_warning);
        else status_pill("LECTURE SEULE", colors.status_info);
        if (ms.state.mode == plpt::share::ShareMode::Collaborative && streaming) {
            ImGui::SameLine();
            if (ms.state.controller == ms.state.you) {
                if (button_secondary("Rendre le controle", ImVec2(190, 26))) {
                    release_remote_capture(state);
                    (void)state.media->release_control();
                }
            } else if (!ms.state.you_waiting && button_primary("Demander le controle", ImVec2(200, 26))) {
                (void)state.media->request_control();
            }
        }
    }
    ImGui::Text("%ux%u - %.1f img/s - %.0f kbit/s - decodage %.1f ms - entree->image %.0f ms - images %llu - rejets %llu",
        ms.frame_width, ms.frame_height, ms.fps, ms.kbps, ms.decode_ms_avg, ms.input_rtt_ms_last,
        static_cast<unsigned long long>(ms.frames), static_cast<unsigned long long>(ms.invalid_records + ms.decode_failures));
    if (!streaming && !ms.status.empty()) ImGui::TextColored(colors.status_error, "%s", ms.status.c_str());
    if (state.remote_capture) text_disabled_label("Clavier envoye a l'application : cliquez hors de la vue pour le liberer.");
    ImGui::Spacing();
    if (!state.guest_view.texture) {
        state.guest_rect = plpt::share::ViewRect{};
        empty_state("Aucune image recue", streaming ? "Le canal est authentifie ; la premiere image arrive."
                                                     : "Le flux media n'est pas etabli.");
        return;
    }
    ImGui::BeginChild("##remote_view", ImVec2(0, 0), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const auto rect = draw_remote_texture(state.guest_view, "##remote_surface");
    state.guest_rect = rect;
    const bool hovered = ImGui::IsItemHovered();
    forward_remote_mouse(state, rect, hovered);
    if (hovered && remote_input_allowed(state)) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::EndChild();
}

void render_host_preview(AppState& state) {
    using namespace devlink::ui;
    const auto& colors = theme_colors();
    if (!state.share || (!state.host_running && !state.host_launching)) {
        if (!state.browser.usable_for_real_session()) {
            status_pill("MOTEUR NAVIGATEUR INDISPONIBLE", colors.status_error);
            ImGui::Spacing();
            ImGui::TextWrapped("Etat reel : %s. Aucune image n'est affichee : rien n'est capture.", state.browser.status.c_str());
        } else {
            empty_state("Aucun partage actif", "Demarrez un partage : cette vue montre alors ce que voient vos invites.");
        }
        return;
    }
    const auto share = state.share->snapshot();
    if (share.stage != plpt::share::ShareStage::Active) {
        status_pill(plpt::share::share_stage_text(share.stage), colors.status_info);
        return;
    }
    if (auto frame = state.share->preview(state.host_preview.sequence)) {
        plpt::media::DecodedImage image;
        if (state.preview_decoder.decode(frame->data, frame->width, frame->height, image))
            (void)state.host_preview.update(image, frame->sequence);
    }
    status_pill("APERCU HOTE DU CONTEXTE PARTAGE", colors.accent_light);
    ImGui::SameLine();
    text_muted(share.mode == plpt::share::ShareMode::Independent
        ? "Mode A : chaque invite pilote son propre contexte ; ceci est le contexte hote."
        : "Exactement les images envoyees aux invites de la vue partagee.");
    ImGui::Spacing();
    ImGui::BeginChild("##host_preview", ImVec2(0, 0), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    (void)draw_remote_texture(state.host_preview, "##host_preview_surface");
    ImGui::EndChild();
}

void render_shared_view(AppState& state) {
    using namespace devlink::ui;
    text_section_title(state.host_mode ? "Vue partagee (apercu hote)" : "Application partagee");
    ImGui::Spacing();
    if (state.host_mode) render_host_preview(state);
    else render_guest_remote_view(state);
}

void render_guest_sessions(AppState& state) {
    using namespace devlink::ui;
    const auto& colors = theme_colors();

    text_section_title("Sessions du reseau local");
    text_muted("Une annonce n'est JAMAIS une preuve d'identité ni une autorisation : "
               "rejoindre exige le code d'invitation communique par l'hote.");
    ImGui::Spacing();

    if (!state.listener_running) {
        if (button_primary("Ecouter les annonces du reseau", ImVec2(320, 36))) {
            if (start_discovery_listener(state)) {
                state.notify("Ecoute UDP multicast 239.255.77.88:47788 demarree");
            } else {
                state.notify("Ecoute impossible : voir les erreurs ci-dessous");
            }
        }
    } else {
        status_pill("ECOUTE ACTIVE", colors.status_success);
        ImGui::SameLine();
        if (state.listener) {
            ImGui::Text("port lie %u - datagrammes acceptes %llu - rejetes %llu",
                static_cast<unsigned int>(state.listener->bound_port().value),
                static_cast<unsigned long long>(state.listener->accepted_datagrams()),
                static_cast<unsigned long long>(state.listener->rejected_datagrams()));
        }
        ImGui::SameLine();
        if (button_secondary("Arreter l'ecoute", ImVec2(180, 30))) {
            if (state.listener) state.listener->stop();
            state.listener.reset();
            state.listener_running = false;
            state.notify("Ecoute du reseau arretee");
        }
    }

    if (!state.guest_errors.empty()) {
        if (card_begin("##guest_errors_discovery", ImVec2(0, 0))) {
            text_card_title("Erreurs reelles");
            for (const auto& line : state.guest_errors) ImGui::TextWrapped("%s", line.c_str());
        }
        card_end();
    }

    ImGui::Spacing();

    auto sessions = state.listener ? state.listener->snapshot() : nullptr;
    if (!sessions || sessions->empty()) {
        empty_state("Aucune session annoncee",
            "Aucun datagramme d'annonce recu. Une saisie manuelle reste possible ci-dessous.");
    } else {
        const std::uint64_t now = state.clock.now_monotonic_ms();
        if (!ImGui::BeginTable("##sessions_table", 7,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::EndTable();
            return;
        }
        ImGui::TableSetupColumn("Session");
        ImGui::TableSetupColumn("Hote");
        ImGui::TableSetupColumn("Adresse reelle");
        ImGui::TableSetupColumn("Port");
        ImGui::TableSetupColumn("Places");
        ImGui::TableSetupColumn("Etat");
        ImGui::TableSetupColumn("Action");
        ImGui::TableHeadersRow();
        for (const auto& session : *sessions) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            text_card_title(session.session_name);
            ImGui::TableNextColumn();
            text_muted(session.host_name);
            ImGui::TableNextColumn();
            text_code(session.address.text);
            ImGui::TableNextColumn();
            ImGui::Text("%u", static_cast<unsigned int>(session.control_port));
            ImGui::TableNextColumn();
            ImGui::Text("%zu / %zu", session.participants, session.max_participants);
            ImGui::TableNextColumn();
            if (session.expired(now)) {
                status_pill("ANNONCE EXPIREE", colors.text_disabled);
            } else if (session.accepting) {
                status_pill("ACCEPTE LES INVITES", colors.status_success);
            } else {
                status_pill("COMPLETE", colors.status_warning);
            }
            ImGui::TableNextColumn();
            const std::string join_id = "Preparer la connexion##" + std::to_string(session.session_id);
            if (button_secondary(join_id.c_str(), ImVec2(190, 24))) {
                std::snprintf(state.guest_address, sizeof(state.guest_address), "%s", session.address.text.c_str());
                state.guest_port = static_cast<int>(session.control_port);
                state.tab = MainNavigationTab::GuestJoin;
                state.notify("Adresse et port remplis : le code d'invitation reste a saisir");
            }
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (card_begin("##manual_entry", ImVec2(pane_width(0.5f, 380.0f), 0))) {
        text_card_title("Saisie manuelle");
        char manual_address[64] = "";
        int manual_port = 0;
        ImGui::InputText("Adresse de l'hote", manual_address, sizeof(manual_address));
        ImGui::InputInt("Port de controle", &manual_port);
        if (button_secondary("Ajouter a la liste", ImVec2(200, 30)) && state.listener) {
            auto parsed = plpt::IpAddress::parse(manual_address);
            if (!parsed) {
                state.notify("Adresse invalide : " + describe(parsed.error()));
            } else if (manual_port <= 0 || manual_port > 65535) {
                state.notify("Port invalide : aucune saisie manuelle sans port reel");
            } else {
                state.listener->inject_manual(parsed.value(),
                    plpt::Port{static_cast<std::uint16_t>(manual_port)}, "session saisie manuellement");
                state.notify("Saisie manuelle ajoutee : elle n'expire pas et ne prouve rien");
            }
        }
        text_disabled_label("Une saisie manuelle a le meme statut qu'une annonce : aucune "
                            "confiance ajoutee, aucune autorisation.");
    }
    card_end();
}

void render_guest_join(AppState& state) {
    using namespace devlink::ui;
    const auto& colors = theme_colors();

    text_section_title("Rejoindre une session");
    text_muted("Connexion par invitation : le code est communique hors bande par l'hote.");
    ImGui::Spacing();

    auto snapshot = state.guest ? state.guest->snapshot() : nullptr;
    const plpt::net::GuestPhase phase = snapshot ? snapshot->phase : plpt::net::GuestPhase::Idle;

    if (phase == plpt::net::GuestPhase::Idle || phase == plpt::net::GuestPhase::Stopped) {
        if (card_begin("##guest_join_card", ImVec2(pane_width(0.5f, 420.0f), 0))) {
            text_card_title("Formulaire de connexion");
            ImGui::InputText("Votre nom", state.guest_name, sizeof(state.guest_name));
            ImGui::InputText("Code d'invitation", state.guest_code, sizeof(state.guest_code));
            ImGui::InputText("Adresse de l'hote", state.guest_address, sizeof(state.guest_address));
            ImGui::InputInt("Port de controle", &state.guest_port);
            ImGui::Spacing();
            if (button_primary("Se connecter", ImVec2(220, 38))) {
                if (start_guest_session(state)) {
                    state.notify("Plan de controle invite demarre : authentification en cours");
                } else {
                    state.notify("Connexion refusee : voir les erreurs ci-dessous");
                }
            }
            ImGui::SameLine();
            if (button_secondary("Voir les sessions du reseau", ImVec2(260, 38))) {
                state.tab = MainNavigationTab::GuestSessions;
            }
        }
        card_end();
    } else {
        if (card_begin("##guest_state_card", ImVec2(0, 0))) {
            text_card_title("Etat reel de la connexion");
            ImGui::Spacing();
            switch (phase) {
            case plpt::net::GuestPhase::Connecting:
                status_pill("CONNEXION EN COURS", colors.status_info);
                break;
            case plpt::net::GuestPhase::Authenticating:
                status_pill("AUTHENTIFICATION", colors.status_info);
                break;
            case plpt::net::GuestPhase::AwaitingApproval:
                status_pill("EN ATTENTE D'AUTORISATION DE L'HOTE", colors.status_warning);
                break;
            case plpt::net::GuestPhase::Connected:
                status_pill("ADMISS PAR L'HOTE", colors.status_success);
                break;
            case plpt::net::GuestPhase::Refused:
                status_pill("REFUSE", colors.status_error);
                break;
            case plpt::net::GuestPhase::Disconnected:
                status_pill("CONNEXION PERDUE", colors.status_error);
                break;
            default:
                status_pill(plpt::net::guest_phase_text(phase), colors.text_disabled);
                break;
            }
            ImGui::Spacing();
            if (snapshot && !snapshot->status_text.empty()) text_muted(snapshot->status_text);
            if (snapshot) {
                ImGui::Text("Session %llu - hote %s - latence %u ms - reprise disponible : %s",
                    static_cast<unsigned long long>(snapshot->session_id),
                    snapshot->host_name.empty() ? "(inconnu)" : snapshot->host_name.c_str(),
                    static_cast<unsigned int>(snapshot->latency_ms),
                    snapshot->resume_available ? "oui" : "non");
                if (state.media) {
                    const auto ms = state.media->snapshot();
                    if (ms.have_state) {
                        ImGui::Text("Mode decide par l'hote : %s", plpt::share::share_mode_text(ms.state.mode));
                        text_disabled_label(ms.state.you_may_input
                            ? "Vos entrees sont acceptees par l'hote en ce moment."
                            : "Vos entrees sont refusees par l'hote en ce moment (mode, controle ou suspension).");
                    }
                }
                if (snapshot->last_error != plpt::ErrorCode::Ok) {
                    ImGui::Spacing();
                    status_pill("DERNIERE ERREUR MOTEUR", colors.status_error);
                    ImGui::SameLine();
                    const std::string_view code = plpt::to_string(snapshot->last_error);
                    ImGui::Text("%.*s", static_cast<int>(code.size()), code.data());
                }
            }
            ImGui::Spacing();
            if (button_danger("Quitter la session", ImVec2(200, 34))) {
                stop_guest_session(state);
                state.notify("Depart volontaire : la place est rendue a l'hote");
            }
        }
        card_end();

        ImGui::Spacing();
        const float left = pane_width(0.5f, 380.0f);
        if (card_begin("##guest_services_card", ImVec2(left, 0))) {
            text_card_title("Flux de l'application");
            ImGui::Spacing();
            if (!state.media) {
                text_muted("Aucun canal media : il est ouvert automatiquement apres admission, avec la cle "
                           "recue sur le canal de controle chiffre.");
            } else {
                const auto ms = state.media->snapshot();
                status_pill(plpt::share::media_phase_text(ms.phase),
                    ms.phase == plpt::share::MediaPhase::Streaming ? colors.status_success : colors.status_error);
                ImGui::Spacing();
                ImGui::TextWrapped("%s", ms.status.c_str());
                ImGui::Text("%llu image(s) - %.1f img/s - %s", static_cast<unsigned long long>(ms.frames), ms.fps,
                    ms.transport.empty() ? "transport non etabli" : ms.transport.c_str());
                if (button_primary("Ouvrir l'application partagee", ImVec2(280, 32))) state.tab = MainNavigationTab::SharedView;
            }
        }
        card_end();

        ImGui::SameLine();

        if (card_begin("##guest_peers_card", ImVec2(0, 0))) {
            text_card_title("Participants de la session");
            ImGui::Spacing();
            if (!snapshot || snapshot->peers.empty()) {
                text_muted("Aucun pair recu pour l'instant.");
            } else {
                for (const auto& peer : snapshot->peers) {
                    const std::string peer_name = peer.self ? peer.name + " (vous)" : peer.name;
                    ImGui::Text("%s", peer_name.c_str());
                    ImGui::SameLine(240);
                    status_pill(plpt::net::participant_state_text(peer.state), colors.accent_light);
                    ImGui::SameLine(360);
                    ImGui::Text("%u ms", static_cast<unsigned int>(peer.latency_ms));
                }
            }
        }
        card_end();
    }

    if (!state.guest_errors.empty()) {
        ImGui::Spacing();
        if (card_begin("##guest_error_card", ImVec2(0, 0))) {
            text_card_title("Erreurs reelles");
            for (const auto& line : state.guest_errors) ImGui::TextWrapped("%s", line.c_str());
        }
        card_end();
    }
}

// Destinataire possible d'un message prive : toujours un participant publie par
// le moteur (hote ou invite), jamais un identifiant saisi.
struct ChatRecipient {
    std::uint64_t id = 0;
    std::string label;
};

// Nom affichable d'un participant de la session, d'apres l'instantane du moteur.
std::string participant_name(const std::vector<ChatRecipient>& known, std::uint64_t id, const std::string& fallback) {
    for (const auto& entry : known)
        if (entry.id == id) return entry.label;
    return fallback;
}

void render_messaging(AppState& state) {
    using namespace devlink::ui;
    const auto& colors = theme_colors();

    text_section_title("Messagerie de session");
    text_muted("Le chat passe par le moteur : l'hôte est l'autorité (quotas, messages "
               "privés, visibilité). Rien n'est fabriqué localement. Conformément au "
               "§13.3, l'hôte peut lire les messages privés : ils ne sont pas chiffrés "
               "de bout en bout entre invités.");
    ImGui::Spacing();

    const bool host_side = state.host_mode && state.host_running && state.host;
    const bool guest_side = !host_side && state.guest_running && state.guest;
    if (!host_side && !guest_side) {
        empty_state("Aucune session connectée",
            "La messagerie n'existe que pendant une session hôte active ou une connexion invitée admise.");
        return;
    }

    std::vector<plpt::net::ChatLine> lines;
    std::vector<ChatRecipient> recipients;   // destinataires privés possibles
    std::vector<ChatRecipient> everyone;     // pour nommer les destinataires affichés
    std::uint64_t self_id = 0;
    if (host_side) {
        if (auto snapshot = state.host->snapshot()) {
            lines = snapshot->chat;
            for (const auto& participant : snapshot->participants) {
                if (participant.state != plpt::model::ParticipantState::Connected) continue;
                recipients.push_back({participant.id.value, participant.name});
            }
            everyone = recipients;
        }
    } else if (auto snapshot = state.guest->snapshot()) {
        lines = snapshot->chat;
        self_id = snapshot->self.value;
        if (snapshot->host_participant.value != 0)
            recipients.push_back({snapshot->host_participant.value, snapshot->host_name + " (hôte)"});
        for (const auto& peer : snapshot->peers) {
            if (peer.self) { everyone.push_back({peer.id.value, peer.name + " (vous)"}); continue; }
            if (peer.state != plpt::model::ParticipantState::Connected) continue;
            recipients.push_back({peer.id.value, peer.name});
        }
        everyone.insert(everyone.end(), recipients.begin(), recipients.end());
    }

    // Un destinataire parti n'est plus proposé : retour au canal général, signalé.
    if (state.chat_recipient != 0 &&
        std::none_of(recipients.begin(), recipients.end(),
            [&](const ChatRecipient& r) { return r.id == state.chat_recipient; })) {
        state.chat_recipient = 0;
        state.notify("Le destinataire privé a quitté la session : retour au canal général");
    }

    if (card_begin("##chat_feed", ImVec2(pane_width(0.7f, 520.0f), 340.0f))) {
        if (lines.empty()) {
            text_muted("Aucun message relayé par le moteur pour l'instant.");
        } else {
            for (const auto& line : lines) {
                if (line.system) {
                    status_pill("ÉVÉNEMENT", colors.status_info);
                    ImGui::SameLine();
                    text_muted(line.body);
                    continue;
                }
                std::string who = line.sender_name;
                if (line.direct) {
                    // Sens réel de l'échange : « A → B », B lu dans l'instantané.
                    const std::string to = line.recipient.value == self_id && self_id != 0
                        ? std::string("vous")
                        : participant_name(everyone, line.recipient.value, "destinataire");
                    who = "(privé) " + line.sender_name + " → " + to;
                }
                ImGui::TextColored(line.direct ? colors.status_warning : colors.accent_light, "%s", who.c_str());
                ImGui::SameLine();
                ImGui::TextWrapped("%s", line.body.c_str());
            }
            // Suit le dernier message tant que l'utilisateur n'a pas remonté le fil.
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
        }
    }
    card_end();

    ImGui::Spacing();
    const std::string current = state.chat_recipient == 0
        ? std::string("Canal général")
        : "Privé : " + participant_name(recipients, state.chat_recipient, "?");
    ImGui::PushItemWidth(pane_width(0.22f, 200.0f));
    if (ImGui::BeginCombo("##chat_recipient", current.c_str())) {
        if (ImGui::Selectable("Canal général", state.chat_recipient == 0)) state.chat_recipient = 0;
        for (const auto& recipient : recipients) {
            const std::string label = "Privé : " + recipient.label + "##dm" + std::to_string(recipient.id);
            if (ImGui::Selectable(label.c_str(), state.chat_recipient == recipient.id)) state.chat_recipient = recipient.id;
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ImGui::PushItemWidth(pane_width(0.42f, 320.0f));
    const bool entered = ImGui::InputTextWithHint("##chat_input",
        state.chat_recipient == 0 ? "Message à toute la session" : "Message privé",
        state.chat_input, sizeof(state.chat_input), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (button_primary("Envoyer", ImVec2(130, 32)) || entered) {
        if (state.chat_input[0] == '\0') {
            state.notify("Message vide : rien n'est envoyé");
        } else {
            const plpt::ParticipantId to{state.chat_recipient};
            bool queued = false;
            if (state.chat_recipient == 0) {
                queued = host_side ? state.host->post_chat(state.chat_input) : state.guest->post_chat(state.chat_input);
            } else {
                queued = host_side ? state.host->post_direct_message(to, state.chat_input)
                                   : state.guest->post_direct_message(to, state.chat_input);
            }
            if (queued) {
                state.chat_input[0] = '\0';
                state.notify(state.chat_recipient == 0 ? "Message transmis au moteur de session"
                                                       : "Message privé transmis au moteur de session");
            } else {
                state.notify("Envoi refusé par le moteur (quota, destinataire absent, session fermée ou file pleine)");
            }
        }
        ImGui::SetKeyboardFocusHere(-1);
    }
}

// Couleur d'une gravité : le sens est aussi porté par le libellé (jamais la
// couleur seule).
ImVec4 severity_color(plpt::net::FeedbackSeverity severity) {
    const auto& colors = devlink::ui::theme_colors();
    switch (severity) {
    case plpt::net::FeedbackSeverity::Informative: return colors.text_secondary;
    case plpt::net::FeedbackSeverity::Minor: return colors.status_info;
    case plpt::net::FeedbackSeverity::Major: return colors.status_warning;
    case plpt::net::FeedbackSeverity::Blocker: return colors.status_error;
    }
    return colors.text_secondary;
}

std::string severity_badge(plpt::net::FeedbackSeverity severity) {
    std::string text = plpt::net::feedback_severity_text(severity);
    for (auto& character : text)
        if (character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A');
    return text;
}

// Âge lisible d'un signalement, d'après l'horloge murale de l'hôte.
std::string age_text(std::uint64_t now_ms, std::uint64_t then_ms) {
    if (then_ms == 0 || now_ms < then_ms) return "à l'instant";
    const std::uint64_t seconds = (now_ms - then_ms) / 1000;
    if (seconds < 60) return "il y a " + std::to_string(seconds) + " s";
    if (seconds < 3600) return "il y a " + std::to_string(seconds / 60) + " min";
    return "il y a " + std::to_string(seconds / 3600) + " h " + std::to_string((seconds % 3600) / 60) + " min";
}

void render_feedback_form(AppState& state) {
    using namespace devlink::ui;
    using plpt::net::FeedbackSeverity;
    const auto& colors = theme_colors();
    const auto snapshot = state.guest->snapshot();
    const bool connected = snapshot && snapshot->phase == plpt::net::GuestPhase::Connected;

    if (card_begin("##feedback_form", ImVec2(pane_width(0.62f, 520.0f), 0))) {
        text_card_title("Nouveau signalement");
        text_muted("Envoyé à l'hôte seul, par le canal chiffré de la session. Décrivez ce que "
                   "vous avez fait, ce que vous attendiez et ce qui s'est produit.");
        ImGui::Spacing();
        text_disabled_label("Gravité");
        static constexpr FeedbackSeverity kLevels[] = {FeedbackSeverity::Informative, FeedbackSeverity::Minor,
            FeedbackSeverity::Major, FeedbackSeverity::Blocker};
        static constexpr const char* kLabels[] = {"Information", "Mineure", "Majeure", "Bloquante"};
        for (int index = 0; index < 4; ++index) {
            if (index > 0) ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_CheckMark, severity_color(kLevels[index]));
            ImGui::RadioButton(kLabels[index], &state.feedback_severity, static_cast<int>(kLevels[index]));
            ImGui::PopStyleColor();
        }
        ImGui::Spacing();
        text_disabled_label("Contexte (écran, route ou étape) — facultatif");
        ImGui::PushItemWidth(-1.0f);
        ImGui::InputTextWithHint("##feedback_context", "ex. : page Paiement, étape 2",
            state.feedback_context, sizeof(state.feedback_context));
        ImGui::PopItemWidth();
        text_disabled_label("Ne collez pas d'URL contenant un jeton, un identifiant ou une donnée personnelle (§13.2).");
        ImGui::Spacing();
        text_disabled_label("Description");
        ImGui::InputTextMultiline("##feedback_description", state.feedback_description,
            sizeof(state.feedback_description), ImVec2(-1.0f, ImGui::GetTextLineHeight() * 7.0f));
        const std::size_t used = std::strlen(state.feedback_description);
        text_disabled_label(std::to_string(used) + " / " + std::to_string(plpt::net::kMaxFeedbackBytes) + " octets");
        ImGui::Spacing();
        ImGui::BeginDisabled(!connected);
        if (button_primary("Envoyer le signalement", ImVec2(220, 34))) {
            const auto severity = static_cast<FeedbackSeverity>(state.feedback_severity);
            if (state.guest->submit_feedback(severity, state.feedback_description, state.feedback_context)) {
                state.feedback_description[0] = '\0';
                state.feedback_context[0] = '\0';
                state.notify("Signalement transmis : l'accusé de réception de l'hôte apparaîtra ci-dessous");
            } else {
                state.notify("Signalement refusé localement : description vide, texte invalide ou session non admise");
            }
        }
        ImGui::EndDisabled();
        if (!connected) {
            ImGui::SameLine();
            status_pill("SESSION NON ADMISE", colors.status_warning);
        }
    }
    card_end();

    ImGui::Spacing();
    if (card_begin("##feedback_acks", ImVec2(pane_width(0.62f, 520.0f), 0))) {
        text_card_title("Accusés de réception de l'hôte");
        std::size_t acks = 0;
        if (snapshot) {
            for (const auto& line : snapshot->chat) {
                if (!line.system || line.body.rfind("Signalement n°", 0) != 0) continue;
                status_pill(icons::Icon::Feedback, "REÇU", colors.status_success);
                ImGui::SameLine();
                text_muted(line.body);
                ++acks;
            }
        }
        if (acks == 0) text_muted("Aucun accusé pour l'instant. Seul un accusé de l'hôte prouve la réception.");
        if (snapshot) {
            text_disabled_label("Envoyés depuis ce poste : " + std::to_string(snapshot->counters.feedback_sent));
        }
    }
    card_end();
}

void render_feedback_registry(AppState& state) {
    using namespace devlink::ui;
    using plpt::net::FeedbackSeverity;
    const auto& colors = theme_colors();
    const auto snapshot = state.host->snapshot();
    std::vector<plpt::net::FeedbackEntry> entries;
    if (snapshot) entries = snapshot->feedback;

    std::size_t counts[4] = {0, 0, 0, 0};
    for (const auto& entry : entries) {
        const auto level = static_cast<std::size_t>(entry.severity);
        if (level < 4) ++counts[level];
    }
    const float width = std::max(150.0f, (ImGui::GetContentRegionAvail().x - 3.0f * ImGui::GetStyle().ItemSpacing.x) / 4.0f);
    metric_card("##fb_blocker", "Bloquants", counts[3], "à traiter en priorité", width);
    ImGui::SameLine();
    metric_card("##fb_major", "Majeurs", counts[2], "fonction dégradée", width);
    ImGui::SameLine();
    metric_card("##fb_minor", "Mineurs", counts[1], "gêne limitée", width);
    ImGui::SameLine();
    metric_card("##fb_info", "Informations", counts[0], "remarques, idées", width);
    ImGui::Spacing();
    text_muted("Mémoire de session uniquement (§13.4) : la liste est oubliée à l'arrêt de la "
               "session. Le journal ne contient que le numéro et la gravité, jamais le texte.");
    ImGui::Spacing();

    if (entries.empty()) {
        empty_state("Aucun signalement reçu",
            "Les invités admis envoient leurs anomalies depuis leur écran Signalements ; "
            "elles apparaissent ici, la plus récente en tête.");
        return;
    }
    const std::uint64_t now = state.clock.now_wall_ms();
    // Plus récent en tête : le moteur publie dans l'ordre d'arrivée.
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        const auto& entry = *it;
        const std::string id = "##feedback_" + std::to_string(entry.id);
        if (card_begin(id.c_str(), ImVec2(pane_width(0.82f, 560.0f), 0))) {
            status_pill(severity_badge(entry.severity), severity_color(entry.severity));
            ImGui::SameLine();
            ImGui::TextColored(colors.accent_light, "N°%llu", static_cast<unsigned long long>(entry.id));
            ImGui::SameLine();
            ImGui::TextUnformatted(entry.reporter_name.c_str());
            ImGui::SameLine();
            text_disabled_label(age_text(now, entry.received_ms));
            if (!entry.context.empty()) {
                text_disabled_label("Contexte :");
                ImGui::SameLine();
                text_code(entry.context);
            }
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(entry.description.c_str());
            ImGui::PopTextWrapPos();
        }
        card_end();
        ImGui::Spacing();
    }
}

void render_feedback(AppState& state) {
    using namespace devlink::ui;

    text_section_title("Signalements et anomalies");
    text_muted("Un signalement va de l'invité à l'hôte seul (protocole 1.5). Aucune "
               "capture ni pièce jointe n'est envoyée en V1 (FR-MSG-006, FR-MSG-008).");
    ImGui::Spacing();

    const bool host_side = state.host_mode && state.host_running && state.host;
    const bool guest_side = !host_side && state.guest_running && state.guest;
    if (host_side) {
        render_feedback_registry(state);
    } else if (guest_side) {
        render_feedback_form(state);
    } else {
        empty_state("Aucune session connectée",
            "Côté hôte : démarrez une session pour recevoir les signalements. "
            "Côté invité : rejoignez une session pour en envoyer.");
    }
}

void render_diagnostics(AppState& state, const plpt::MonitorView& view, const Runtime& runtime) {
    using namespace devlink::ui;
    const auto& colors = theme_colors();

    text_section_title("Diagnostics et etat reseau");
    text_muted("Chaque ligne decrit un fait verifiable, pas une intention.");
    ImGui::Spacing();

    if (card_begin("##diag_product", ImVec2(pane_width(0.5f, 420.0f), 0))) {
        text_card_title("Produit et plateforme");
        ImGui::Text("%.*s %.*s", static_cast<int>(devlink::kProductName.size()), devlink::kProductName.data(),
            static_cast<int>(devlink::kProductVersion.size()), devlink::kProductVersion.data());
        ImGui::Text("Plateforme : %s", platform_label());
        ImGui::Text("SDL %d - pilote video %s - rendu %s", static_cast<int>(SDL_GetVersion()),
            SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "(aucun)",
            SDL_GetRendererName(runtime.renderer) ? SDL_GetRendererName(runtime.renderer) : "(aucun)");
        ImGui::Text("Echelle d'affichage de la fenetre : %.2f", static_cast<double>(state.dpi_scale));
        // Typographie : ce qui a reellement ete charge, sans succes pretendu.
        const devlink::ui::FontStatus& loaded = devlink::ui::font_status();
        if (loaded.source.empty()) {
            ImGui::TextColored(devlink::ui::theme_colors().status_warning,
                "Typographie : repli ImGui (%s)", loaded.note.c_str());
        } else {
            ImGui::Text("Typographie : %s - %s - niveau corps %.0f px a %.0f %%",
                loaded.family.c_str(), loaded.embedded ? "embarquee" : "systeme",
                static_cast<double>(loaded.body_pixels),
                static_cast<double>(devlink::ui::current_display_scale() * 100.0f));
            text_muted(loaded.source);
            if (!loaded.note.empty()) {
                ImGui::TextColored(devlink::ui::theme_colors().status_warning, "%s", loaded.note.c_str());
            }
        }
        ImGui::Text("Theme : %s", state.theme_mode == devlink::ui::ThemeMode::DevDark ? "DevDark" : "DevLight");
    }
    card_end();

    ImGui::SameLine();

    if (card_begin("##diag_engine", ImVec2(0, 0))) {
        text_card_title("Moteur navigateur et transport");
        ImGui::Text("SDK WebView2 a la compilation : %s",
            plpt::browser::kWebView2SdkCompiled ? "oui" : "non");
        ImGui::Text("Runtime WebView2 detecte : %s",
            plpt::browser::WebView2Backend::runtime_available() ? "oui" : "non");
        ImGui::Text("Backend navigateur reel operationnel : %s",
            plpt::browser::WebView2Backend::is_available() ? "oui" : "non (pipeline non implemente)");
        ImGui::Text("Backend utilise : %s", state.browser.status.c_str());
        if (state.share) {
            const auto share = state.share->snapshot();
            ImGui::Text("Partage : %s - port media %u - contextes %llu/%llu", plpt::share::share_stage_text(share.stage),
                static_cast<unsigned int>(share.media_port), static_cast<unsigned long long>(share.contexts_created),
                static_cast<unsigned long long>(share.contexts_destroyed));
        }
        if (state.media) {
            const auto ms = state.media->snapshot();
            ImGui::Text("Canal media invite : %s - %s", plpt::share::media_phase_text(ms.phase),
                ms.transport.empty() ? "non etabli" : ms.transport.c_str());
        }
        ImGui::Spacing();
        // Côté invité : le transport affiché est celui du canal réel (ADR-016).
        {
            const auto guest_view = state.guest ? state.guest->snapshot() : nullptr;
            const std::string transport = guest_view && !guest_view->transport.empty()
                ? guest_view->transport : std::string("aucune session invitée");
            const bool secure = transport.rfind("TLSv1.3", 0) == 0 || transport.rfind("TLS 1.3", 0) == 0;
            status_pill(transport.c_str(), secure ? colors.status_success : colors.status_warning);
            ImGui::Spacing();
            ImGui::TextWrapped(secure
                ? "Canal invite TLS 1.3 PSK : l'hote a ete authentifie par la poignee de main "
                  "avant toute trame utile (ADR-016)."
                : "Session invitee NON chiffree (hote sans TLS, assume des deux cotes) : "
                  "aucune donnee n'est annoncee securisee.");
        }
    }
    card_end();

    ImGui::Spacing();

    if (card_begin("##diag_monitor", ImVec2(pane_width(0.5f, 420.0f), 0))) {
        text_card_title("Moniteur local");
        ImGui::Text("Revision %llu - services %zu - complet : %s",
            static_cast<unsigned long long>(view.revision), view.services.size(),
            view.complete ? "oui" : "non");
        ImGui::Text("Dernier rafraichissement accepte : %s",
            state.monitor_refresh_accepted ? "oui" : "non");
        ImGui::Text("Avertissements : %zu", view.warnings.size());
        for (const auto& warning : view.warnings) {
            ImGui::TextWrapped("%s", describe(warning).c_str());
        }
    }
    card_end();

    ImGui::SameLine();

    if (card_begin("##diag_network", ImVec2(0, 0))) {
        devlink::ui::icons::icon(devlink::ui::icons::Icon::Pulse, 15.0f, ImGui::ColorConvertFloat4ToU32(devlink::ui::theme_colors().accent_light));
        ImGui::SameLine();
        text_card_title("Reseau");
        if (state.host) {
            if (auto snapshot = state.host->snapshot()) {
                ImGui::Text("Hote : %s:%u - %s", snapshot->control_address.c_str(),
                    static_cast<unsigned int>(snapshot->control_port), snapshot->bind_note.c_str());
                ImGui::Text("Connexions ouvertes : %zu - revision %llu",
                    state.host->open_connections(),
                    static_cast<unsigned long long>(snapshot->revision));
            }
        } else {
            text_muted("Aucun serveur hote.");
        }
        if (state.announcer) {
            ImGui::Text("Annonceur : %llu datagramme(s) envoye(s) - %zu echec(s)",
                static_cast<unsigned long long>(state.announcer->sent_datagrams()),
                state.announcer->failures().size());
        }
        if (state.listener) {
            ImGui::Text("Ecoute : %llu accepte(s) - %llu rejete(s)",
                static_cast<unsigned long long>(state.listener->accepted_datagrams()),
                static_cast<unsigned long long>(state.listener->rejected_datagrams()));
        }
        if (state.guest) {
            if (auto snapshot = state.guest->snapshot()) {
                ImGui::Text("Invite : %s - %s", plpt::net::guest_phase_text(snapshot->phase),
                    snapshot->status_text.c_str());
            }
        }
    }
    card_end();
}

// Barre d'onglets : l'espace hote et l'espace invite ne mélangent pas leurs ecrans.
// Une navigation demandee par le code (state.tab change hors de la barre) est
// appliquee avec SetSelected et rejouee tant qu'ImGui ne l'a pas affichee :
// sinon l'onglet memorise par ImGui l'ecrase a la frame suivante.
void render_navigation(AppState& state) {
    const bool pending = state.tab != state.nav_shown;
    MainNavigationTab chosen = state.tab;
    bool requested_open = false;
    const auto push_tab = [&](MainNavigationTab tab) {
        const ImGuiTabItemFlags flags = (pending && tab == state.tab) ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem(devlink::ui::nav_label(tab), nullptr, flags)) {
            if (!pending) chosen = tab;
            if (tab == state.tab) requested_open = true;
            ImGui::EndTabItem();
        }
    };
    if (!ImGui::BeginTabBar("##devlink_navigation", ImGuiTabBarFlags_None)) return;
    if (state.host_mode) {
        push_tab(MainNavigationTab::Dashboard);
        push_tab(MainNavigationTab::DiscoveryCenter);
        push_tab(MainNavigationTab::ShareSetup);
        push_tab(MainNavigationTab::ApprovalQueue);
        push_tab(MainNavigationTab::ActiveSession);
        push_tab(MainNavigationTab::SharedView);
    } else {
        push_tab(MainNavigationTab::GuestSessions);
        push_tab(MainNavigationTab::GuestJoin);
        push_tab(MainNavigationTab::SharedView);
    }
    push_tab(MainNavigationTab::Messaging);
    push_tab(MainNavigationTab::Feedback);
    push_tab(MainNavigationTab::Diagnostics);
    ImGui::EndTabBar();
    state.tab = chosen;
    if (!pending || requested_open) state.nav_shown = state.tab;
}

// ---------------------------------------------------------------------------
// Cycle de vie
// ---------------------------------------------------------------------------

// Rafraichit l'annonce LAN avec les compteurs reels de la session : une annonce
// stale ferait croire a des places libres qui n'existent plus.
void refresh_announcement(AppState& state) {
    if (!state.announcer || !state.host) return;
    auto snapshot = state.host->snapshot();
    if (!snapshot) return;
    plpt::net::Announcement announcement;
    announcement.session_id = snapshot->session_id;
    announcement.session_name = snapshot->session_name;
    announcement.host_name = snapshot->host_name;
    announcement.control_port = snapshot->control_port;
    announcement.participants = static_cast<std::uint16_t>(snapshot->participants.size());
    announcement.max_participants = static_cast<std::uint16_t>(std::clamp(state.max_participants, 1,
        static_cast<int>(plpt::share::kMaxShareGuests)));
    announcement.shared_services = snapshot->services.empty() ? 0 : 1;
    announcement.accepting = snapshot->state == plpt::model::SessionState::AwaitingGuests ||
        snapshot->state == plpt::model::SessionState::Active;
    announcement.wall_ms = state.clock.now_wall_ms();
    state.announcer->update(announcement);
}

// Echelle DPI reelle de la fenetre : SDL3 la mesure, l'interface ne la devine pas.
float window_display_scale(SDL_Window* window) {
    const float scale = SDL_GetWindowDisplayScale(window);
    if (!(scale > 0.5f) || !(scale < 4.0f)) return 1.0f; // NaN ou valeur absurde : 100 %.
    return scale;
}

// ---------------------------------------------------------------------------
// Lancement scriptable (qualification, protocole LAN physique reproductible)
//
// --host / --join remplissent les MEMES formulaires et appellent les MEMES
// fonctions que les boutons ; --auto-click / --auto-type injectent des
// evenements SDL dans la fenetre reelle : ils traversent ImGui, le mappage
// letterbox, le canal media TLS et l'autorisation cote hote exactement comme
// une souris et un clavier physiques. Rien n'est court-circuite.
// ---------------------------------------------------------------------------

struct Qualification {
    // hote
    bool host = false;
    bool report_fixture = false;
    bool print_invite = false;    // secret : seulement sur demande explicite
    bool exit_when_guests_gone = false;
    bool announced_active = false;
    bool saw_guest = false;
    // invite
    bool join = false;
    bool request_control = false;
    bool requested = false;
    std::vector<std::pair<float, float>> clicks; // points normalises, dans l'ordre
    std::string type_text;
    std::size_t input_step = 0;   // 3 etapes par clic (mouvement, appui, relache) puis le texte
    int input_wait_frames = 0;
    // commun
    double capture_after_s = -1.0;
    std::string capture_path;
    bool captured = false;
    int capture_deferrals = 0;    // trames sans surface d'affichage sautees avant la capture
    double exit_after_s = -1.0;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_report = started;
    bool any() const { return host || join || capture_after_s >= 0 || exit_after_s >= 0; }
    double elapsed() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    }
};

bool parse_unit_pair(const char* text, float& x, float& y) {
    char* end = nullptr;
    x = std::strtof(text, &end);
    if (!end || *end != ',') return false;
    y = std::strtof(end + 1, &end);
    return end && *end == '\0' && x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f;
}

// Point (normalise) de l'image -> coordonnees fenetre, d'apres le rectangle
// reellement dessine a la frame precedente.
void push_mouse(SDL_Window* window, const plpt::share::ViewRect& rect, float nx, float ny, Uint32 type) {
    SDL_Event e{};
    e.type = type;
    const float x = rect.x + nx * rect.width;
    const float y = rect.y + ny * rect.height;
    // Fenetre active : le backend ImGui lit la position GLOBALE du curseur a
    // chaque frame ; le curseur reel est donc place la ou « la main » clique.
    SDL_WarpMouseInWindow(window, x, y);
    if (type == SDL_EVENT_MOUSE_MOTION) {
        e.motion.windowID = SDL_GetWindowID(window);
        e.motion.x = x;
        e.motion.y = y;
    } else {
        e.button.windowID = SDL_GetWindowID(window);
        e.button.button = SDL_BUTTON_LEFT;
        e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.clicks = 1;
        e.button.x = x;
        e.button.y = y;
    }
    (void)SDL_PushEvent(&e);
}

std::string qualification_text_storage; // vit jusqu'au traitement de l'evenement pousse

void qualification_tick(Qualification& q, AppState& app, const plpt::share::ViewRect& guest_rect, bool& running) {
    if (q.host && q.announced_active == false && app.host_running && app.host && app.share) {
        auto view = app.host->snapshot();
        const auto share = app.share->snapshot();
        std::printf("devlink-host: active invite=%s control=%s:%u media=%u mode=%u first_frame_ms=%llu\n",
            q.print_invite ? app.invite_code.text.c_str() : plpt::mask_secret(app.invite_code.text).c_str(),
            view->control_address.c_str(), static_cast<unsigned int>(view->control_port),
            static_cast<unsigned int>(share.media_port), static_cast<unsigned int>(share.mode),
            static_cast<unsigned long long>(share.first_frame_ms));
        std::fflush(stdout);
        q.announced_active = true;
    }
    if (q.host && !app.host_running && !app.host_launching && !q.announced_active && !app.host_errors.empty()) {
        for (const auto& e : app.host_errors) std::printf("devlink-host: error %s\n", e.c_str());
        std::fflush(stdout);
        running = false;
        return;
    }
    const bool report_now = std::chrono::steady_clock::now() - q.last_report >= std::chrono::milliseconds(500);
    if (report_now) q.last_report = std::chrono::steady_clock::now();
    if (q.host && app.share && app.host_running && report_now) {
        const auto share = app.share->snapshot();
        std::uint64_t accepted = 0, rejected = 0;
        for (const auto& p : share.peers) { accepted += p.inputs_accepted; rejected += p.inputs_rejected; }
        std::printf("devlink-host: peers=%zu controller=%llu inputs_accepted=%llu inputs_rejected=%llu contexts=%llu/%llu\n",
            share.peers.size(), static_cast<unsigned long long>(share.controller.value),
            static_cast<unsigned long long>(accepted), static_cast<unsigned long long>(rejected),
            static_cast<unsigned long long>(share.contexts_created), static_cast<unsigned long long>(share.contexts_destroyed));
        if (q.report_fixture) {
            // Diagnostic HOTE uniquement : lit le DOM du contexte partage.
            auto dom = app.share->evaluate_for_diagnostics(plpt::ParticipantId{},
                "window.devlinkFixture ? window.devlinkFixture() : null");
            if (dom) std::printf("devlink-host-fixture: %s\n", dom.value().c_str());
        }
        std::fflush(stdout);
        bool connected = false;
        for (const auto& p : share.peers) connected = connected || p.media_connected;
        if (connected) q.saw_guest = true;
        if (q.exit_when_guests_gone && q.saw_guest && !connected) running = false;
    }
    if (q.join && app.media) {
        const auto ms = app.media->snapshot();
        if (report_now) {
            std::printf("devlink-guest: phase=%s frames=%llu fps=%.1f transport=%s mode=%u may_input=%d controller=%llu "
                        "inputs_sent=%llu input_rtt_ms=%.1f invalid=%llu\n",
                plpt::share::media_phase_text(ms.phase), static_cast<unsigned long long>(ms.frames), ms.fps,
                ms.transport.empty() ? "-" : ms.transport.c_str(), static_cast<unsigned int>(ms.state.mode),
                ms.state.you_may_input ? 1 : 0, static_cast<unsigned long long>(ms.state.controller),
                static_cast<unsigned long long>(ms.inputs_sent), ms.input_rtt_ms_last,
                static_cast<unsigned long long>(ms.invalid_records + ms.decode_failures));
            std::fflush(stdout);
        }
        if (q.request_control && !q.requested && ms.phase == plpt::share::MediaPhase::Streaming && ms.have_state) {
            q.requested = app.media->request_control();
        }
        // Clic puis saisie, seulement quand l'hote dit que nos entrees sont admises
        // et qu'une image a ete dessinee (rectangle connu).
        const bool ready = ms.state.you_may_input && app.guest_view.texture && guest_rect.width > 0 &&
            app.tab == MainNavigationTab::SharedView;
        const std::size_t steps = q.clicks.size() * 3 + 1;
        if (!q.clicks.empty() && ready && q.input_step < steps && --q.input_wait_frames <= 0) {
            q.input_wait_frames = 4;
            if (q.input_step < q.clicks.size() * 3) {
                const auto [cx, cy] = q.clicks[q.input_step / 3];
                static constexpr Uint32 kPhase[3] = {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP};
                push_mouse(app.window, guest_rect, cx, cy, kPhase[q.input_step % 3]);
            } else {
                if (!q.type_text.empty()) {
                    qualification_text_storage = q.type_text;
                    SDL_Event e{};
                    e.type = SDL_EVENT_TEXT_INPUT;
                    e.text.windowID = SDL_GetWindowID(app.window);
                    e.text.text = qualification_text_storage.c_str();
                    (void)SDL_PushEvent(&e);
                }
            }
            ++q.input_step;
        }
    }
    if (q.capture_after_s >= 0 && !q.captured && q.elapsed() >= q.capture_after_s) q.captured = true; // capture en fin de frame
    if (q.exit_after_s >= 0 && q.elapsed() >= q.exit_after_s) running = false;
}

// Etat de la fenetre au moment d'une capture : une preuve visuelle doit dire si
// la trame avait une surface d'affichage (une fenetre minimisee ne dessine rien).
std::string window_state_text(SDL_Window* window) {
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    std::string text;
    if (flags & SDL_WINDOW_MINIMIZED) text += "minimized,";
    if (flags & SDL_WINDOW_HIDDEN) text += "hidden,";
    if (flags & SDL_WINDOW_OCCLUDED) text += "occluded,";
    if (flags & SDL_WINDOW_INPUT_FOCUS) text += "focused,";
    if (text.empty()) return "normal";
    text.pop_back();
    return text;
}

bool save_window_capture(SDL_Window* window, SDL_Renderer* renderer, const std::string& path, int deferrals) {
    SDL_Surface* capture = SDL_RenderReadPixels(renderer, nullptr);
    if (!capture) {
        std::fprintf(stderr, "SDL_RenderReadPixels: %s\n", SDL_GetError());
        return false;
    }
    const bool saved = SDL_SaveBMP(capture, path.c_str());
    if (!saved) std::fprintf(stderr, "SDL_SaveBMP: %s\n", SDL_GetError());
    std::printf("devlink-capture: %s %dx%d %s window=%s deferred_frames=%d\n", path.c_str(), capture->w, capture->h,
        saved ? "saved" : "FAILED", window_state_text(window).c_str(), deferrals);
    std::fflush(stdout);
    SDL_DestroySurface(capture);
    return saved;
}

} // namespace

int main(int argc, char** argv) {
    bool smoke = false;
    bool demo_browser = false;
    const char* screenshot_path = nullptr;
    Qualification q;
    std::string q_origins, q_entry, q_bind, q_join_ip, q_code, q_name;
    int q_join_port = 0;
    char q_mode = 'B';
    bool q_no_approval = false;
    const auto usage_error = [&]() {
        std::fprintf(stderr, "%.*s", static_cast<int>(kUsage.size()), kUsage.data());
        return 2;
    };

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--smoke-test") {
            smoke = true;
        } else if (argument == "--demo-synthetic-browser") {
            // Build de demonstration explicite : seul chemin qui autorise un
            // backend synthetique, et il est signale en permanence a l'ecran.
            demo_browser = true;
        } else if (argument == "--host" && index + 1 < argc) {
            q.host = true;
            q_origins = argv[++index];
        } else if (argument == "--entry" && index + 1 < argc) {
            q_entry = argv[++index];
        } else if (argument == "--mode" && index + 1 < argc) {
            const std::string_view m(argv[++index]);
            if (m != "A" && m != "B" && m != "C") return usage_error();
            q_mode = m[0];
        } else if (argument == "--bind" && index + 1 < argc) {
            q_bind = argv[++index];
        } else if (argument == "--no-approval") {
            q_no_approval = true;
        } else if (argument == "--print-invite") {
            q.print_invite = true;
        } else if (argument == "--report-fixture") {
            q.report_fixture = true;
        } else if (argument == "--exit-when-guests-gone") {
            q.exit_when_guests_gone = true;
        } else if (argument == "--join" && index + 2 < argc) {
            q.join = true;
            q_join_ip = argv[++index];
            q_join_port = std::atoi(argv[++index]);
        } else if (argument == "--code" && index + 1 < argc) {
            q_code = argv[++index];
        } else if (argument == "--name" && index + 1 < argc) {
            q_name = argv[++index];
        } else if (argument == "--request-control") {
            q.request_control = true;
        } else if (argument == "--auto-click" && index + 1 < argc) {
            float cx = 0.0f, cy = 0.0f;
            if (!parse_unit_pair(argv[++index], cx, cy)) return usage_error();
            q.clicks.emplace_back(cx, cy);
        } else if (argument == "--auto-type" && index + 1 < argc) {
            q.type_text = argv[++index];
        } else if (argument == "--capture-after" && index + 2 < argc) {
            q.capture_after_s = std::atof(argv[++index]);
            q.capture_path = argv[++index];
        } else if (argument == "--exit-after" && index + 1 < argc) {
            q.exit_after_s = std::atof(argv[++index]);
        } else if (argument == "--screenshot") {
            if (index + 1 >= argc) {
                std::fprintf(stderr, "%.*s", static_cast<int>(kUsage.size()), kUsage.data());
                return 2;
            }
            screenshot_path = argv[++index];
        } else {
            std::fprintf(stderr, "%.*s", static_cast<int>(kUsage.size()), kUsage.data());
            return 2;
        }
    }
    if ((q.host && q.join) || (q.join && (q_code.empty() || q_join_port <= 0)) || (smoke && q.any()))
        return usage_error();
    if (screenshot_path && !smoke) {
        std::fprintf(stderr, "--screenshot exige --smoke-test. Aucune capture interactive implicite.\n");
        return 2;
    }

    Runtime runtime;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    runtime.sdl = true;

    runtime.window = SDL_CreateWindow(kWindowTitle.data(), 1440, 920,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!runtime.window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    // Icone de fenetre : RGBA 64x64 embarque, genere depuis le maitre vectoriel
    // (tools/branding/export_icons.py, controle par la sonde --ico). SDL copie la
    // surface ; sous Windows l'icone PE du binaire prime, ici la marque reelle
    // est garantie sur toutes les plateformes.
    if (SDL_Surface* icon_surface = SDL_CreateSurface(devlink::ui::window_icon::kWidth,
            devlink::ui::window_icon::kHeight, SDL_PIXELFORMAT_RGBA32)) {
        std::memcpy(icon_surface->pixels, devlink::ui::window_icon::kPixelData,
            devlink::ui::window_icon::kBytes);
        SDL_SetWindowIcon(runtime.window, icon_surface);
        SDL_DestroySurface(icon_surface);
    } else {
        std::fprintf(stderr, "SDL_CreateSurface: icone de fenetre non definie : %s\n",
            SDL_GetError());
    }

    runtime.renderer = SDL_CreateRenderer(runtime.window, nullptr);
    if (!runtime.renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }

    if (smoke) {
        std::printf("desktop-runtime: SDL=%d video=%s renderer=%s\n",
            static_cast<int>(SDL_GetVersion()),
            SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "(none)",
            SDL_GetRendererName(runtime.renderer) ? SDL_GetRendererName(runtime.renderer) : "(none)");
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    runtime.imgui = true;

    if (!ImGui_ImplSDL3_InitForSDLRenderer(runtime.window, runtime.renderer)) {
        std::fprintf(stderr, "ImGui_ImplSDL3_InitForSDLRenderer: %s\n", SDL_GetError());
        return 1;
    }
    runtime.platform_backend = true;

    if (!ImGui_ImplSDLRenderer3_Init(runtime.renderer)) {
        std::fprintf(stderr, "ImGui_ImplSDLRenderer3_Init: %s\n", SDL_GetError());
        return 1;
    }
    runtime.render_backend = true;

    AppState app;
    app.window = runtime.window;
    app.host_preview.renderer = runtime.renderer;
    app.guest_view.renderer = runtime.renderer;
    app.browser_demo_requested = demo_browser;
    app.interfaces = std::make_unique<plpt::NativeNetworkInterfaceProvider>();
    app.monitor = std::make_unique<plpt::Monitor>(
        std::make_unique<plpt::NativeProcessEnumerator>(),
        std::make_unique<plpt::NativeSocketTableProvider>());
    app.browser = create_browser_engine(demo_browser);
    app.dpi_scale = window_display_scale(runtime.window);

    devlink::ui::ThemeManager::instance().set_theme(app.theme_mode);
    devlink::ui::ThemeManager::instance().apply_to_imgui(app.dpi_scale);

    // Typographie reelle (mandat 17) : famille vectorielle chargee depuis le
    // disque (police embarquee OFL si presente, sinon police systeme), tailles
    // reelles par niveau multipliees par l'echelle d'affichage mesuree par SDL.
    // Echec de chargement = repli ImGui honnetement signale aux diagnostics.
    devlink::ui::FontSet fonts;
    devlink::ui::load_fonts(app.dpi_scale, devlink::ui::TypographyConfig{}, fonts);

    if (app.monitor) {
        app.monitor_refresh_accepted = app.monitor->request_refresh();
    }

    if (demo_browser) {
        std::fprintf(stderr,
            "AVERTISSEMENT : backend navigateur synthetique active explicitement. "
            "Aucun partage reel n'est possible dans ce mode.\n");
    }

    if (q.host) {
        std::snprintf(app.allowed_origins, sizeof(app.allowed_origins), "%s", q_origins.c_str());
        std::snprintf(app.entry_url, sizeof(app.entry_url), "%s", q_entry.c_str());
        std::snprintf(app.bind_address, sizeof(app.bind_address), "%s", q_bind.c_str());
        app.mode = q_mode == 'A' ? ParticipationMode::Independent
            : q_mode == 'C' ? ParticipationMode::Observation : ParticipationMode::Collaborative;
        app.approval_required = !q_no_approval;
        if (!start_host_session(app)) {
            for (const auto& e : app.host_errors) std::printf("devlink-host: error %s\n", e.c_str());
            return 1;
        }
    }
    if (q.join) {
        app.host_mode = false;
        app.tab = MainNavigationTab::GuestJoin;
        std::snprintf(app.guest_address, sizeof(app.guest_address), "%s", q_join_ip.c_str());
        app.guest_port = q_join_port;
        std::snprintf(app.guest_code, sizeof(app.guest_code), "%s", q_code.c_str());
        if (!q_name.empty()) std::snprintf(app.guest_name, sizeof(app.guest_name), "%s", q_name.c_str());
        if (!start_guest_session(app)) {
            for (const auto& e : app.guest_errors) std::printf("devlink-guest: error %s\n", e.c_str());
            return 1;
        }
    }

    bool running = true;
    int smoke_frames = 0;
    static const plpt::MonitorView kEmptyView;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            // Clavier capture par la vue distante : envoye a l'hote, pas a ImGui.
            if (!forward_remote_keyboard(app, event)) ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                event.window.windowID == SDL_GetWindowID(runtime.window)) {
                running = false;
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // DPI reel : reapplication du theme quand l'echelle change (moniteur
        // different, zoom systeme). Aucune mise a l'echelle typographique improvisée.
        const float scale = window_display_scale(runtime.window);
        if (scale != app.dpi_scale) {
            app.dpi_scale = scale;
            devlink::ui::ThemeManager::instance().apply_to_imgui(app.dpi_scale);
            // Nouvel atlas aux nouvelles tailles reelles : le backend SDL3 gere
            // l'atlas dynamique, aucun SetWindowFontScale de rattrapage.
            devlink::ui::load_fonts(scale, devlink::ui::TypographyConfig{}, fonts);
        }

        // Instantanes immuables : l'interface ne lit que ce que le moteur publie.
        app.monitor_view = app.monitor ? app.monitor->view() : nullptr;
        const plpt::MonitorView& view = app.monitor_view ? *app.monitor_view : kEmptyView;

        if (++app.frames_since_refresh >= kMonitorRefreshPeriodFrames) {
            app.frames_since_refresh = 0;
            if (app.monitor) app.monitor_refresh_accepted = app.monitor->request_refresh();
        }
        if (app.host_running && ++app.frames_since_announce >= kAnnounceUpdatePeriodFrames) {
            app.frames_since_announce = 0;
            refresh_announcement(app);
        }
        // Un moteur arrete de l'exterieur (session expirée, erreur reseau) ne doit
        // jamais laisser l'interface afficher un etat actif.
        if (app.host_running && app.host && !app.host->running()) {
            app.host_running = false;
            app.host_note = "session terminee par le moteur";
            app.notify("La session hote s'est arretee cote moteur");
        }
        if (app.guest_running && app.guest && !app.guest->running()) {
            app.guest_running = false;
            app.notify("La connexion invite s'est terminee cote moteur");
        }
        poll_host_launch(app);
        tick_guest_media(app);
        // La capture clavier ne survit pas a un changement d'ecran.
        if (app.remote_capture && (app.tab != MainNavigationTab::SharedView || app.host_mode)) release_remote_capture(app);

        int width = 0;
        int height = 0;
        SDL_GetWindowSize(runtime.window, &width, &height);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(width), static_cast<float>(height)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##DevLinkRoot", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleVar(2);

        // Statut reel : une seule source, celle du moteur.
        std::string status_text = "PRET";
        ImVec4 status_color = devlink::ui::theme_colors().text_disabled;
        devlink::ui::icons::Icon status_glyph = devlink::ui::icons::Icon::Info;
        if (app.host_launching) {
            status_text = "DEMARRAGE DU PARTAGE";
            status_color = devlink::ui::theme_colors().status_info;
            status_glyph = devlink::ui::icons::Icon::Pulse;
        } else if (app.host_running) {
            status_text = "PARTAGE ACTIF";
            status_color = devlink::ui::theme_colors().status_success;
            status_glyph = devlink::ui::icons::Icon::Session;
        } else if (app.guest_running) {
            auto guest_snapshot = app.guest ? app.guest->snapshot() : nullptr;
            const plpt::net::GuestPhase phase =
                guest_snapshot ? guest_snapshot->phase : plpt::net::GuestPhase::Idle;
            status_text = std::string("INVITE : ") + plpt::net::guest_phase_text(phase);
            status_color = (phase == plpt::net::GuestPhase::Connected)
                ? devlink::ui::theme_colors().status_success
                : devlink::ui::theme_colors().status_info;
            status_glyph = (phase == plpt::net::GuestPhase::Connected)
                ? devlink::ui::icons::Icon::Check
                : devlink::ui::icons::Icon::Pulse;
        }
        if (app.browser.demo_synthetic) {
            status_text += " - MOTEUR SYNTHETIQUE (DEMONSTRATION)";
            status_color = devlink::ui::theme_colors().status_warning;
            status_glyph = devlink::ui::icons::Icon::Warning;
        }

        devlink::ui::ThemeMode requested_theme = app.theme_mode;
        bool requested_mode = app.host_mode;
        devlink::ui::render_top_bar(status_text, status_color, status_glyph, app.host_mode,
            app.theme_mode, requested_theme, requested_mode);
        if (requested_theme != app.theme_mode) {
            app.theme_mode = requested_theme;
            devlink::ui::ThemeManager::instance().set_theme(app.theme_mode);
            devlink::ui::ThemeManager::instance().apply_to_imgui(app.dpi_scale);
        }
        if (requested_mode != app.host_mode) {
            app.host_mode = requested_mode;
            app.tab = app.host_mode ? MainNavigationTab::Dashboard : MainNavigationTab::GuestSessions;
        }

        ImGui::Spacing();
        render_navigation(app);
        ImGui::Spacing();

        // Un ecran hote affiche en mode invite (et inversement) n'a pas d'etat :
        // on rebascule sur l'ecran d'accueil de l'espace courant.
        if (app.host_mode && devlink::ui::is_guest_tab(app.tab)) app.tab = MainNavigationTab::Dashboard;
        if (!app.host_mode && devlink::ui::is_host_tab(app.tab)) app.tab = MainNavigationTab::GuestSessions;

        switch (app.tab) {
        case MainNavigationTab::Dashboard: render_dashboard(app, view); break;
        case MainNavigationTab::DiscoveryCenter: render_discovery_center(app, view); break;
        case MainNavigationTab::ShareSetup: render_share_setup(app); break;
        case MainNavigationTab::ApprovalQueue: render_approval_queue(app); break;
        case MainNavigationTab::ActiveSession: render_active_session(app); break;
        case MainNavigationTab::SharedView: render_shared_view(app); break;
        case MainNavigationTab::GuestSessions: render_guest_sessions(app); break;
        case MainNavigationTab::GuestJoin: render_guest_join(app); break;
        case MainNavigationTab::Messaging: render_messaging(app); break;
        case MainNavigationTab::Feedback: render_feedback(app); break;
        case MainNavigationTab::Diagnostics: render_diagnostics(app, view, runtime); break;
        }

        if (app.toast_visible()) {
            const ImVec2 origin{static_cast<float>(width) - 380.0f, static_cast<float>(height) - 76.0f};
            ImGui::SetNextWindowPos(origin);
            ImGui::BeginChild("##devlink_toast", ImVec2(360, 56), ImGuiChildFlags_Borders,
                ImGuiWindowFlags_NoScrollbar);
            ImGui::TextWrapped("%s", app.toast.c_str());
            ImGui::EndChild();
        }

        ImGui::End();

        ImGui::Render();
        const ImVec4 background = devlink::ui::theme_colors().background;
        SDL_SetRenderDrawColorFloat(runtime.renderer, background.x, background.y, background.z, 1.0f);
        SDL_RenderClear(runtime.renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), runtime.renderer);
        // Capture de la VRAIE fenetre, avant presentation (contenu garanti).
        // Une trame sans surface d'affichage (fenetre minimisee ou masquee : le
        // backend ImGui met DisplaySize a 0 et ne dessine rien) n'est jamais
        // enregistree comme preuve : la fenetre est restauree et la capture
        // reportee a la premiere trame reellement dessinee (borne : 5 s environ).
        if (q.captured && !q.capture_path.empty()) {
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            if ((display.x <= 0.0f || display.y <= 0.0f) && q.capture_deferrals < 300) {
                if (q.capture_deferrals == 0) {
                    std::printf("devlink-capture: deferred window=%s (no drawable surface)\n",
                        window_state_text(runtime.window).c_str());
                    std::fflush(stdout);
                    (void)SDL_ShowWindow(runtime.window);
                    (void)SDL_RestoreWindow(runtime.window);
                }
                ++q.capture_deferrals;
            } else {
                (void)save_window_capture(runtime.window, runtime.renderer, q.capture_path, q.capture_deferrals);
                q.capture_path.clear();
            }
        }
        SDL_RenderPresent(runtime.renderer);
        if (q.any()) qualification_tick(q, app, app.guest_rect, running);

        if (smoke) {
            ++smoke_frames;
            if (smoke_frames >= kSmokeFrameCount) {
                if (screenshot_path) {
                    // Chaque appel SDL est verifie et nomme dans le diagnostic : un
                    // echec d'ouverture, d'ecriture ou de fermeture ne doit jamais
                    // etre confondu avec un autre (disque plein, chemin invalide).
                    SDL_Surface* capture = SDL_RenderReadPixels(runtime.renderer, nullptr);
                    if (!capture) {
                        std::fprintf(stderr, "SDL_RenderReadPixels: %s\n", SDL_GetError());
                        return 1;
                    }
                    SDL_IOStream* stream = SDL_IOFromFile(screenshot_path, "wb");
                    if (!stream) {
                        std::fprintf(stderr, "SDL_IOFromFile: %s\n", SDL_GetError());
                        SDL_DestroySurface(capture);
                        return 1;
                    }
                    // closeio = false : la fermeture est separee pour que son echec
                    // (ENOSPC sur /dev/full) soit signale explicitement.
                    const bool saved = SDL_SaveBMP_IO(capture, stream, false);
                    if (!saved) {
                        std::fprintf(stderr, "SDL_SaveBMP_IO: %s\n", SDL_GetError());
                    }
                    SDL_DestroySurface(capture);
                    const bool closed = SDL_CloseIO(stream);
                    if (!closed) {
                        std::fprintf(stderr, "SDL_CloseIO: %s\n", SDL_GetError());
                    }
                    if (!saved || !closed) return 1;
                }
                std::printf("desktop-smoke: %d frames, snapshot=%llu, services=%zu, sharing=%s\n",
                    smoke_frames, static_cast<unsigned long long>(view.revision), view.services.size(),
                    app.host_running ? "enabled" : "disabled");
                running = false;
            }
        }
    }

    // Arret propre : rien ne survit a la fenetre.
    if (app.guest_running) stop_guest_session(app);
    if (q.host && app.share) {
        app.share->stop(); // idempotent : stop_host_session le refait
        const auto end = app.share->snapshot();
        std::printf("devlink-host: stopped stage=%s contexts=%llu/%llu\n", plpt::share::share_stage_text(end.stage),
            static_cast<unsigned long long>(end.contexts_created), static_cast<unsigned long long>(end.contexts_destroyed));
        std::fflush(stdout);
    }
    if (app.host_running || app.host_launching) stop_host_session(app, "arret du poste de commande");
    if (app.listener) { app.listener->stop(); app.listener.reset(); }
    app.listener_running = false;
    app.monitor.reset();

    return 0;
}
