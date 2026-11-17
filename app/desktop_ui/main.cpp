// DevLink — app/desktop_ui/main.cpp
// Poste de commande desktop natif : SDL3 + Dear ImGui.
//
// Contrat d'architecture (mandat §12) : l'interface ne possède AUCUN état de
// session. Elle lit des instantanés immuables publiés par les threads du moteur
// (Monitor, HostServer, GuestClient, ServiceProxy, DiscoveryListener, Announcer)
// et n'écrit que des commandes. Aucun écran ne met en scène un état
// « connecté », « sécurisé », « streaming » ou « session active » que le moteur
// n'a pas confirmé : un démarrage est transactionnel et tout échec est affiché.
//
// Le transport chiffré standard (mandat §24) n'est pas encore intégré : cette
// interface l'annonce explicitement au lieu de prétendre à un canal sécurisé.

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
#include "engine/net/proxy.hpp"
#include "engine/platform/native.hpp"
#include "engine/port_allocator/port_allocator.hpp"

namespace {

using devlink::ui::MainNavigationTab;

// Identité publique : UNE seule source (engine/core/product.hpp). Le titre et
// l'usage en découlent pour qu'aucune chaîne ne diverge du --version.
const std::string kWindowTitle =
    std::string(devlink::kProductName) + " - " + std::string(devlink::kProductTagline);
const std::string kUsage =
    "Usage: devlink-desktop [--smoke-test [--screenshot file.bmp]] [--demo-synthetic-browser]\n";

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

plpt::Role role_of(ParticipationMode mode) {
    switch (mode) {
    case ParticipationMode::Independent: return plpt::Role::IndependentTester;
    case ParticipationMode::Collaborative: return plpt::Role::Collaborator;
    case ParticipationMode::Observation: return plpt::Role::Observer;
    }
    return plpt::Role::Observer; // valeur d'enum hors domaine : mode le plus restrictif.
}

const char* mode_label(ParticipationMode mode) {
    switch (mode) {
    case ParticipationMode::Independent: return "Mode A - test indépendant";
    case ParticipationMode::Collaborative: return "Mode B - collaboration sur une vue";
    case ParticipationMode::Observation: return "Mode C - observation seule";
    }
    return "mode inconnu";
}

// Libelle du role tel que le moteur le nomme : l'interface n'invente pas de
// vocabulaire, elle affiche celui du modele de session.
const char* role_text(plpt::Role role) {
    switch (role) {
    case plpt::Role::Host: return "hote";
    case plpt::Role::IndependentTester: return "testeur independant";
    case plpt::Role::Collaborator: return "collaborateur";
    case plpt::Role::Observer: return "observateur";
    }
    return "role inconnu";
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

bool mode_allows_input(ParticipationMode mode) { return mode != ParticipationMode::Observation; }

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
    std::unique_ptr<plpt::IRemoteBrowserBackend> backend;
    bool real = false;           // vrai moteur navigateur operationnel
    bool demo_synthetic = false; // backend synthetique, demande explicitement
    std::string status = "non initialise";

    // Une session de partage reelle exige un moteur reel : le repli synthetique
    // silencieux est interdit en production.
    bool usable_for_real_session() const { return real && backend != nullptr; }
};

BrowserEngine create_browser_engine(bool allow_synthetic_demo) {
    BrowserEngine engine;
    if (plpt::browser::WebView2Backend::is_available()) {
        engine.backend = std::make_unique<plpt::browser::WebView2Backend>();
        engine.real = true;
        engine.status = "Microsoft Edge WebView2 (moteur reel)";
        return engine;
    }
    if (allow_synthetic_demo) {
        // Chemin de demonstration explicite : jamais le repli automatique de la
        // fabrique, et toujours signale a l'ecran par un bandeau permanent.
        engine.backend = plpt::browser::create_browser_backend();
        engine.demo_synthetic = engine.backend != nullptr;
        engine.status = engine.backend
            ? "moteur synthetique (demonstration) - AUCUN partage reel"
            : "moteur navigateur indisponible";
        return engine;
    }
    // Cause exacte, jamais un message generique : le remede n'est pas le meme
    // selon que le SDK manque a la compilation ou que le runtime n'est pas installe.
#if defined(_WIN32)
    engine.status = plpt::browser::kWebView2SdkCompiled
        ? "moteur navigateur indisponible : runtime WebView2 non installe sur cette machine"
        : "moteur navigateur indisponible : SDK WebView2 absent de cette compilation";
#else
    engine.status = "moteur navigateur indisponible : WebView2 est propre a Windows";
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

// ---------------------------------------------------------------------------
// Etat de l'application : formulaires + pointeurs vers le moteur
// ---------------------------------------------------------------------------

struct AppState {
    plpt::SystemClock clock;
    std::unique_ptr<plpt::NativeNetworkInterfaceProvider> interfaces;

    // Navigation et apparence
    MainNavigationTab tab = MainNavigationTab::Dashboard;
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

    // Plan de controle et plan de donnees de l'hote
    std::unique_ptr<plpt::net::HostServer> host;
    std::unique_ptr<plpt::net::Announcer> announcer;
    std::unique_ptr<plpt::net::ServiceProxy> proxy;
    plpt::InviteCode invite_code;
    std::string service_token;
    bool host_running = false;
    std::string host_note = "aucune session hote";
    std::vector<std::string> host_errors;

    // Moteur navigateur
    BrowserEngine browser;
    plpt::ContextId browser_ctx{};
    std::string browser_profile;
    std::string browser_origin;
    bool browser_demo_requested = false;

    // Plan invite
    std::unique_ptr<plpt::net::GuestClient> guest;
    std::unique_ptr<plpt::net::DiscoveryListener> listener;
    bool guest_running = false;
    bool listener_running = false;
    std::vector<std::string> guest_errors;

    // Formulaires hote
    char session_name[64] = "Session DevLink";
    char host_name[64] = "hote";
    char bind_address[64] = "";      // vide = DevLink choisit (LAN reel sinon boucle locale)
    char allowed_origins[256] = "";  // allowlist du navigateur distant
    int max_participants = 5;
    bool approval_required = true;
    ParticipationMode mode = ParticipationMode::Independent;

    // Formulaires invite
    char guest_name[64] = "invite";
    char guest_code[64] = "";
    char guest_address[64] = "127.0.0.1";
    int guest_port = 0;

    char chat_input[512] = "";

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

// ---------------------------------------------------------------------------
// Contexte navigateur : creation et destruction reelles, resultats verifies
// ---------------------------------------------------------------------------

void destroy_browser_context(AppState& state) {
    if (state.browser.backend && state.browser_ctx.value != 0) {
        auto destroyed = state.browser.backend->destroy_context(state.browser_ctx);
        if (!destroyed) {
            state.host_errors.push_back("destruction du contexte navigateur : " + describe(destroyed.error()));
        }
    }
    state.browser_ctx = plpt::ContextId{};
    state.browser_profile.clear();
    state.browser_origin.clear();
}

// Allowlist stricte : les origines saisies, separees par des virgules. Une liste
// vide n'autorise rien (BrowserPolicy::is_origin_allowed refuse tout).
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

bool open_browser_context(AppState& state, plpt::ParticipantId owner, const std::vector<std::string>& origins) {
    if (!state.browser.backend) return false;
    plpt::BrowserPolicy policy;
    policy.allowed_origins = origins;
    auto context = state.browser.backend->create_context(owner, policy);
    if (!context) {
        state.host_errors.push_back("contexte navigateur : " + describe(context.error()));
        return false;
    }
    state.browser_ctx = context.value().context;
    state.browser_profile = context.value().profile_id;
    state.browser_origin = context.value().current_origin;
    return true;
}

// ---------------------------------------------------------------------------
// Demarrage transactionnel de la session hote (mandat §12 et §18)
// ---------------------------------------------------------------------------

bool start_host_session(AppState& state) {
    state.host_errors.clear();
    if (state.host_running) {
        state.host_errors.push_back("une session hote est deja en cours");
        return false;
    }

    // 1. Secrets reels generés par le moteur : code d'invitation et jeton.
    auto code = plpt::generate_invite_code();
    if (!code) {
        state.host_errors.push_back("code d'invitation : " + describe(code.error()));
        return false;
    }
    auto token = plpt::generate_service_token();
    if (!token) {
        state.host_errors.push_back("jeton de service : " + describe(token.error()));
        return false;
    }

    // 2. Adresse d'ecoute : vide = le moteur choisit une interface privee LAN
    //    reellement presente, sinon la boucle locale, et publie la raison dans
    //    bind_note. Jamais toutes les interfaces par defaut.
    plpt::IpAddress bind{};
    if (state.bind_address[0] != '\0') {
        auto parsed = plpt::IpAddress::parse(state.bind_address);
        if (!parsed) {
            state.host_errors.push_back("adresse d'ecoute : " + describe(parsed.error()));
            return false;
        }
        bind = parsed.value();
    }

    // 3. Plan de controle. control_port = 0 : l'OS choisit, start() renvoie le
    //    port reellement lie. Aucune reservation prealable, donc pas de TOCTOU.
    plpt::net::HostConfig config;
    config.session_name = state.session_name;
    config.host_name = state.host_name;
    config.bind_address = bind;
    config.control_port = 0;
    config.max_participants = static_cast<std::size_t>(std::max(1, state.max_participants));
    config.approval_required = state.approval_required;
    config.invite_code = code.value();

    state.host = std::make_unique<plpt::net::HostServer>(state.clock);
    auto started = state.host->start(config);
    if (!started) {
        state.host_errors.push_back("serveur hote : " + describe(started.error()));
        state.host.reset();
        return false;
    }

    // 4. Plan de donnees : proxy reel UNIQUEMENT si un service local a ete
    //    choisi dans la decouverte. Sans amont reel, aucun partage applicatif
    //    n'est pretendu.
    const std::vector<std::string> origins = parse_origins(state.allowed_origins);
    if (state.selected.valid) {
        plpt::net::ProxyConfig proxy_config;
        proxy_config.label = state.selected.label;
        proxy_config.bind_address = bind;
        proxy_config.exposed_port = 0;
        proxy_config.upstream_address = plpt::IpAddress::loopback(plpt::IpFamily::Ipv4);
        proxy_config.upstream_port = state.selected.port;
        proxy_config.token = token.value();
        state.proxy = std::make_unique<plpt::net::ServiceProxy>(state.clock);
        auto proxy_started = state.proxy->start(proxy_config);
        if (!proxy_started) {
            state.host_errors.push_back("proxy applicatif : " + describe(proxy_started.error()));
            state.proxy.reset();
            state.host->stop();
            state.host.reset();
            return false;
        }
        // Publication a l'autorite de session de ce qui est reellement joignable :
        // l'URL et l'etat amont viennent du proxy, jamais d'une hypothese.
        auto proxy_view = state.proxy->snapshot();
        if (proxy_view) {
            plpt::net::SharedServiceRecord record;
            record.key = 1;
            record.label = state.selected.label;
            record.local_port = state.selected.port;
            record.exposed_port = proxy_view->exposed_port;
            record.token = token.value();
            record.url = proxy_view->url;
            record.available = proxy_view->running;
            record.upstream_alive = proxy_view->upstream_alive;
            if (!state.host->set_services(std::vector<plpt::net::SharedServiceRecord>{record})) {
                state.host_errors.push_back("publication du service refusee par le moteur de session");
            }
            if (!proxy_view->upstream_alive) {
                state.host_errors.push_back(
                    "service local injoignable sur le port " + std::to_string(state.selected.port) +
                    " : le proxy repondra 502 tant que l'amont ne demarre pas");
            }
        }
    }

    // 5. Moteur navigateur : contexte reel par origine autorisee. Sans moteur
    //    reel, la session de partage applicatif distant n'est pas demarree.
    if (state.browser.usable_for_real_session()) {
        if (!open_browser_context(state, plpt::ParticipantId{1}, origins)) {
            if (state.proxy) { state.proxy->stop(); state.proxy.reset(); }
            state.host->stop();
            state.host.reset();
            return false;
        }
    } else if (origins.empty()) {
        state.host_errors.push_back(
            "aucune origine autorisee : le navigateur distant refusera toute navigation");
    }

    // 6. Annonce LAN sur les interfaces reellement presentes (+ boucle locale
    //    pour la demonstration sur une seule machine).
    auto snapshot = state.host->snapshot();
    if (!snapshot) {
        state.host_errors.push_back("instantane de session indisponible apres demarrage");
        destroy_browser_context(state);
        if (state.proxy) { state.proxy->stop(); state.proxy.reset(); }
        state.host->stop();
        state.host.reset();
        return false;
    }
    plpt::net::Announcement announcement;
    announcement.session_id = snapshot->session_id;
    announcement.session_name = snapshot->session_name;
    announcement.host_name = snapshot->host_name;
    announcement.control_port = snapshot->control_port;
    announcement.participants = 0;
    announcement.max_participants = static_cast<std::uint16_t>(std::max(1, state.max_participants));
    announcement.shared_services = state.proxy ? 1 : 0;
    announcement.accepting = true;
    announcement.wall_ms = state.clock.now_wall_ms();

    std::vector<plpt::NetworkInterfaceInfo> interface_list;
    if (state.interfaces) {
        auto enumerated = state.interfaces->enumerate();
        if (enumerated) {
            interface_list = enumerated.value();
        } else {
            state.host_errors.push_back("interfaces reseau : " + describe(enumerated.error()));
        }
    }
    auto targets = plpt::net::announce_targets(
        interface_list, announcement, plpt::Port{plpt::net::kDiscoveryPort}, true);
    state.announcer = std::make_unique<plpt::net::Announcer>(state.clock);
    auto announced = state.announcer->start(targets);
    if (!announced) {
        state.host_errors.push_back("annonceur LAN : " + describe(announced.error()));
        state.announcer.reset();
        destroy_browser_context(state);
        if (state.proxy) { state.proxy->stop(); state.proxy.reset(); }
        state.host->stop();
        state.host.reset();
        return false;
    }

    // 7. Succes confirme par le moteur : l'etat actif n'est affiche qu'ici.
    state.invite_code = code.value();
    state.service_token = token.value();
    state.host_running = state.host->running();
    if (!state.host_running) {
        state.host_errors.push_back("le serveur hote s'est arrete immediatement apres le demarrage");
        state.announcer->stop();
        state.announcer.reset();
        destroy_browser_context(state);
        if (state.proxy) { state.proxy->stop(); state.proxy.reset(); }
        state.host.reset();
        return false;
    }
    state.host_note = "controle sur " + snapshot->control_address + ":" + std::to_string(snapshot->control_port) +
        " - " + snapshot->bind_note;
    return true;
}

void stop_host_session(AppState& state, std::string reason) {
    if (state.announcer) { state.announcer->stop(); state.announcer.reset(); }
    if (state.host) { state.host->stop(); state.host.reset(); }
    if (state.proxy) { state.proxy->stop(); state.proxy.reset(); }
    destroy_browser_context(state);
    state.host_running = false;
    state.host_note = std::move(reason);
    // Le code d'invitation et le jeton sont des secrets : ils ne survivent pas a
    // la session et ne sont plus affiches apres l'arret.
    state.invite_code = plpt::InviteCode{};
    state.service_token.clear();
}

// ---------------------------------------------------------------------------
// Demarrage transactionnel cote invite (mandat §19)
// ---------------------------------------------------------------------------

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
    // Reconnexion automatique : jamais sans consentement explicite (§6).
    config.auto_reconnect = false;

    state.guest = std::make_unique<plpt::net::GuestClient>(state.clock);
    auto started = state.guest->start(config);
    if (!started) {
        state.guest_errors.push_back("client invite : " + describe(started.error()));
        state.guest.reset();
        return false;
    }
    // start() ouvre le plan de controle ; la connexion et l'authentification sont
    // asynchrones. L'etat reel vient de la phase publiee, jamais d'un succes suppose.
    state.guest_running = state.guest->running();
    if (!state.guest_running) {
        state.guest_errors.push_back("le client invite ne tourne pas apres le demarrage");
        state.guest.reset();
        return false;
    }
    return true;
}

void stop_guest_session(AppState& state) {
    if (state.guest) {
        // Depart volontaire : la place est rendue immediatement (Bye) puis le
        // plan de controle est arrete proprement.
        if (!state.guest->leave()) {
            state.guest_errors.push_back("depart volontaire non accepte par le moteur (arret force ensuite)");
        }
        state.guest->stop();
        state.guest.reset();
    }
    state.guest_running = false;
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
            ImGui::Spacing();
            ImGui::Text("Jeton de service :");
            ImGui::SameLine();
            text_code(plpt::mask_secret(state.service_token));
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
        status_pill("TCP CLAIR - TLS NON INTEGRE", colors.status_warning);
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Le canal chiffre standard exige par le mandat (TLS 1.3 avec authentification "
            "des pairs) n'est pas encore integre. Aucune session n'est annoncee comme securisee.");
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

    if (!view.warnings.empty() && card_begin("##discovery_warnings", ImVec2(0, 0))) {
        devlink::ui::icons::icon(devlink::ui::icons::Icon::Warning, 15.0f, ImGui::ColorConvertFloat4ToU32(devlink::ui::theme_colors().accent_light));
        ImGui::SameLine();
        text_card_title("Avertissements du moniteur");
        for (const auto& warning : view.warnings) {
            ImGui::TextWrapped("%s", describe(warning).c_str());
        }
    }
    card_end();

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
        ImGui::SliderInt("Capacite maximale d'invites", &state.max_participants, 1, 5);
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
            ImGui::TextWrapped("Sans service local reel, le plan de controle peut demarrer "
                "(invitation, chat, approbations) mais aucun partage applicatif n'est publie.");
            if (button_secondary("Choisir dans la decouverte", ImVec2(260, 32))) {
                state.tab = MainNavigationTab::DiscoveryCenter;
            }
        }
        ImGui::Spacing();
        ImGui::InputText("Origines autorisees du navigateur (separees par des virgules)",
            state.allowed_origins, sizeof(state.allowed_origins));
        text_disabled_label("Allowlist stricte : file:, javascript:, data: et les hotes non "
                            "autorises sont refuses par le moteur navigateur.");

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

        if (state.host_running) {
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
                ImGui::TextWrapped("Partage applicatif distant bloque : %s. "
                    "Le plan de controle (invitation, approbation, chat) reste disponible.",
                    state.browser.status.c_str());
                ImGui::Spacing();
            }
            if (button_primary("Demarrer le partage", ImVec2(280, 40))) {
                if (start_host_session(state)) {
                    state.tab = MainNavigationTab::ActiveSession;
                    state.notify("Session hote demarree : port lie et annonce reelle");
                } else {
                    state.notify("Demarrage refuse : voir les echecs ci-dessous");
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

    if (card_begin("##services_card", ImVec2(0, 0))) {
        text_card_title("Services publies aux invites");
        ImGui::Spacing();
        if (snapshot->services.empty()) {
            text_muted("Aucun service publie. Le partage applicatif exige un service "
                       "local reel selectionne dans la decouverte.");
        } else {
            for (const auto& service : snapshot->services) {
                text_card_title(service.label);
                ImGui::Text("Port expose %u - amont local %u - connexions %zu",
                    static_cast<unsigned int>(service.exposed_port),
                    static_cast<unsigned int>(service.local_port), service.active_connections);
                if (service.upstream_alive) {
                    status_pill("AMONT JOIGNABLE", colors.status_success);
                } else {
                    status_pill("AMONT INJOIGNABLE (502)", colors.status_error);
                }
                ImGui::Spacing();
                ImGui::TextWrapped("URL invite : %s", service.url.c_str());
                ImGui::Text("Requetes %llu - montant %llu - descendant %llu",
                    static_cast<unsigned long long>(service.requests),
                    static_cast<unsigned long long>(service.bytes_up),
                    static_cast<unsigned long long>(service.bytes_down));
                ImGui::Separator();
            }
        }
        if (state.proxy) {
            if (button_secondary("Sonder l'amont maintenant", ImVec2(240, 30))) {
                const bool alive = state.proxy->probe_upstream();
                state.notify(alive ? "Amont joignable (connexion TCP reelle)"
                                   : "Amont injoignable : le proxy repondra 502");
            }
        }
    }
    card_end();

    if (!snapshot->warnings.empty() && card_begin("##session_warnings", ImVec2(0, 0))) {
        text_card_title("Avertissements de la session");
        for (const auto& warning : snapshot->warnings) {
            ImGui::TextWrapped("%s", describe(warning).c_str());
        }
    }
    card_end();

    if (!snapshot->events.empty() && card_begin("##session_events", ImVec2(0, 0))) {
        text_card_title("Evenements recents");
        const std::size_t first = snapshot->events.size() > 12 ? snapshot->events.size() - 12 : 0;
        for (std::size_t index = first; index < snapshot->events.size(); ++index) {
            text_muted(snapshot->events[index]);
        }
    }
    card_end();

    ImGui::Spacing();
    if (button_danger("Arreter la session", ImVec2(220, 36))) {
        stop_host_session(state, "session arretee par l'hote");
        state.notify("Session arretee : sockets fermees, secrets oublies");
    }
}

void render_shared_view(AppState& state) {
    using namespace devlink::ui;
    const auto& colors = theme_colors();

    text_section_title("Vue applicative partagee");
    ImGui::Spacing();

    if (!state.browser.usable_for_real_session()) {
        status_pill("MOTEUR NAVIGATEUR INDISPONIBLE", colors.status_error);
        ImGui::Spacing();
        ImGui::TextWrapped("Etat reel : %s. Aucune image n'est affichee, parce qu'aucune "
            "application n'est reellement capturee. Une trame synthetique ne serait pas "
            "un partage et n'est jamais presentee comme tel.", state.browser.status.c_str());
        ImGui::Spacing();
        text_muted("Chaine exigee par le mandat : navigateur reel -> acquisition pixels -> "
                   "trame normalisee -> encodage -> transporte authentifie -> decodage -> "
                   "texture SDL3 -> affichage invite.");
        return;
    }

    if (state.browser_ctx.value == 0) {
        status_pill("AUCUN CONTEXTE NAVIGATEUR", colors.status_warning);
        ImGui::Spacing();
        text_muted("Le contexte est cree au demarrage d'une session, avec l'allowlist "
                   "d'origines saisie dans la configuration.");
        return;
    }

    if (card_begin("##shared_view_card", ImVec2(0, 0))) {
        text_card_title("Contexte navigateur de l'hote");
        ImGui::Text("Identifiant %llu - profil %s",
            static_cast<unsigned long long>(state.browser_ctx.value), state.browser_profile.c_str());
        ImGui::Text("Origine courante : %s",
            state.browser_origin.empty() ? "(aucune navigation reussie)" : state.browser_origin.c_str());
        ImGui::Spacing();
        status_pill("TRANSPORT VIDEO NON INTEGRE", colors.status_warning);
        ImGui::Spacing();
        ImGui::TextWrapped("La capture locale existe, mais le transport authentifie des trames "
            "vers l'invite n'est pas encore implemente : l'invite ne recoit donc rien. "
            "Cet ecran n'affiche pas d'image tant que la chaine complete n'est pas verifiee.");
    }
    card_end();
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

    if (!state.guest_errors.empty() && card_begin("##guest_errors_discovery", ImVec2(0, 0))) {
        text_card_title("Erreurs reelles");
        for (const auto& line : state.guest_errors) ImGui::TextWrapped("%s", line.c_str());
    }
    card_end();

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
                ImGui::Text("Mode demande : %s (role moteur : %s)", mode_label(state.mode),
                    role_text(role_of(state.mode)));
                text_disabled_label(mode_capability_note(state.mode));
                text_disabled_label(mode_allows_input(state.mode)
                    ? "Entree autorisee par ce mode, sous controle du bail accorde par l'hote."
                    : "Entree refusee par ce mode : l'observation seule ne transmet aucun evenement.");
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
            text_card_title("Services publies par l'hote");
            ImGui::Spacing();
            if (!snapshot || snapshot->services.empty()) {
                text_muted("Aucun service recu. La liste n'arrive qu'apres admission par l'hote.");
            } else {
                for (const auto& service : snapshot->services) {
                    text_card_title(service.label);
                    if (service.available) {
                        status_pill("DISPONIBLE", colors.status_success);
                    } else {
                        status_pill("INDISPONIBLE", colors.status_error);
                    }
                    ImGui::Spacing();
                    ImGui::TextWrapped("%s", service.url.c_str());
                    ImGui::Separator();
                }
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

void render_messaging(AppState& state) {
    using namespace devlink::ui;
    const auto& colors = theme_colors();

    text_section_title("Messagerie de session");
    text_muted("Le chat passe par le moteur : l'hote est l'autorite (quotas, messages "
               "prives, visibilite). Rien n'est fabrique localement.");
    ImGui::Spacing();

    const bool host_side = state.host_mode && state.host_running && state.host;
    const bool guest_side = !host_side && state.guest_running && state.guest;
    std::vector<plpt::net::ChatLine> lines;
    if (host_side) {
        if (auto snapshot = state.host->snapshot()) lines = snapshot->chat;
    } else if (guest_side) {
        if (auto snapshot = state.guest->snapshot()) lines = snapshot->chat;
    }

    if (!host_side && !guest_side) {
        empty_state("Aucune session connectee",
            "La messagerie n'existe que pendant une session hote active ou une connexion invite admise.");
        return;
    }

    if (card_begin("##chat_feed", ImVec2(pane_width(0.7f, 520.0f), 340.0f))) {
        if (lines.empty()) {
            text_muted("Aucun message relaye par le moteur pour l'instant.");
        } else {
            for (const auto& line : lines) {
                if (line.system) {
                    status_pill("EVENEMENT", colors.status_info);
                    ImGui::SameLine();
                    text_muted(line.body);
                    continue;
                }
                ImGui::TextColored(line.direct ? colors.status_warning : colors.accent_light, "%s%s",
                    line.direct ? "(prive) " : "", line.sender_name.c_str());
                ImGui::SameLine();
                ImGui::TextWrapped("%s", line.body.c_str());
            }
        }
    }
    card_end();

    ImGui::Spacing();
    ImGui::PushItemWidth(pane_width(0.55f, 420.0f));
    const bool entered = ImGui::InputText("##chat_input", state.chat_input, sizeof(state.chat_input),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (button_primary("Envoyer", ImVec2(130, 32)) || entered) {
        if (state.chat_input[0] == '\0') {
            state.notify("Message vide : rien n'est envoye");
        } else {
            const bool queued = host_side
                ? state.host->post_chat(state.chat_input)
                : state.guest->post_chat(state.chat_input);
            if (queued) {
                state.chat_input[0] = '\0';
                state.notify("Message transmis au moteur de session");
            } else {
                state.notify("Envoi refuse par le moteur (quota, session fermee ou file pleine)");
            }
        }
    }
}

void render_feedback(AppState& state) {
    using namespace devlink::ui;
    const auto& colors = theme_colors();

    text_section_title("Signalements et anomalies");
    ImGui::Spacing();
    status_pill("FONCTION NON RELIEE AU MOTEUR", colors.status_warning);
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Etat reel : le modele de messagerie possede la capacite SubmitFeedback et un "
        "identifiant de rapport, et le protocole reserve le type de message Feedback. "
        "Aucune route reseau ne transporte encore ces rapports : ni le serveur hote ni le "
        "client invite ne traitent ce type. Aucun signalement n'est donc envoye ni stocke.");
    ImGui::Spacing();
    text_muted("Ce qui manque exactement : la prise en charge de MessageType::Feedback dans "
               "host_server (relayage et persistance bornee) et dans guest_client (emission), "
               "plus un ecran de consultation cote hote. Tant que ce n'est pas fait, cet ecran "
               "n'affiche aucun signalement fictif.");
    ImGui::Spacing();
    text_disabled_label("Mode demande pour cette session : ");
    ImGui::SameLine();
    text_code(mode_label(state.mode));
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
            plpt::browser::WebView2Backend::is_available() ? "oui" : "non");
        ImGui::Text("Backend utilise : %s", state.browser.status.c_str());
        ImGui::Text("Contexte navigateur : %llu",
            static_cast<unsigned long long>(state.browser_ctx.value));
        ImGui::Spacing();
        status_pill("TRANSPORT TCP CLAIR", colors.status_warning);
        ImGui::Spacing();
        ImGui::TextWrapped("Aucun canal chiffre standard n'est integre. Le module maison "
            "XOR/HMAC est hors du chemin de production et doit etre remplace par TLS 1.3 "
            "avec authentification des pairs (mandat §24).");
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
        if (state.proxy) {
            if (auto proxy_view = state.proxy->snapshot()) {
                ImGui::Text("Proxy : %s - amont %s - joignable : %s", proxy_view->bind_address.c_str(),
                    proxy_view->upstream.c_str(), proxy_view->upstream_alive ? "oui" : "non");
                ImGui::Text("Autorises %llu - refuses %llu - echecs amont %llu - jetons retires %llu",
                    static_cast<unsigned long long>(proxy_view->counters.authorized),
                    static_cast<unsigned long long>(proxy_view->counters.refused),
                    static_cast<unsigned long long>(proxy_view->counters.upstream_failures),
                    static_cast<unsigned long long>(proxy_view->counters.tokens_redacted));
            }
        } else {
            text_muted("Aucun proxy applicatif.");
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
void render_navigation(AppState& state) {
    const auto push_tab = [&state](MainNavigationTab tab) {
        if (ImGui::BeginTabItem(devlink::ui::nav_label(tab))) {
            state.tab = tab;
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
    announcement.max_participants = static_cast<std::uint16_t>(std::max(1, state.max_participants));
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

} // namespace

int main(int argc, char** argv) {
    bool smoke = false;
    bool demo_browser = false;
    const char* screenshot_path = nullptr;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--smoke-test") {
            smoke = true;
        } else if (argument == "--demo-synthetic-browser") {
            // Build de demonstration explicite : seul chemin qui autorise un
            // backend synthetique, et il est signale en permanence a l'ecran.
            demo_browser = true;
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

    bool running = true;
    int smoke_frames = 0;
    static const plpt::MonitorView kEmptyView;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
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
        if (app.host_running) {
            status_text = "SESSION HOTE ACTIVE";
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
        SDL_RenderPresent(runtime.renderer);

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
    if (app.host_running) stop_host_session(app, "arret du poste de commande");
    if (app.listener) { app.listener->stop(); app.listener.reset(); }
    app.listener_running = false;
    app.monitor.reset();

    return 0;
}
