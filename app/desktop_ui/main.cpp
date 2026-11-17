// DevLink — app/desktop_ui/main.cpp — Station de Partage et de Test Desktop Native.
// Architecture C++20 / SDL3 / Dear ImGui.
//
// Rôles principaux :
// 1. Espace Hôte : Découverte de services locaux, sélection de points d'entrée,
//    démarrage de sessions chiffrées, proxy avec capacité par jeton, gestion
//    des autorisations d'invités, messagerie temps réel, feedback et diagnostics.
// 2. Espace Invité : Découverte automatique LAN, connexion par code d'invitation,
//    navigation isolée (profils indépendants), chat collaboratif et remontée de bugs.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "engine/browser/browser.hpp"
#include "engine/core/clock.hpp"
#include "engine/core/random.hpp"
#include "engine/core/secrets.hpp"
#include "engine/discovery/monitor.hpp"
#include "engine/net/discovery.hpp"
#include "engine/net/guest_client.hpp"
#include "engine/net/host_server.hpp"
#include "engine/net/proxy.hpp"
#include "engine/platform/native.hpp"

namespace {

// Le titre de la fenêtre SDL reste en ASCII pur pour une compatibilité OS universelle.
// L'interface Dear ImGui affiche le nom complet avec accents et typographie soignée.
constexpr std::string_view window_title = "DevLink - Local App Sharing & Test Platform";
static_assert(std::all_of(window_title.begin(), window_title.end(), [](unsigned char c) { return c < 0x80; }));

// Palette de style DevLink
const ImVec4 c_mint{0.39F, 0.87F, 0.71F, 1.0F};
const ImVec4 c_cyan{0.25F, 0.78F, 0.92F, 1.0F};
const ImVec4 c_muted{0.55F, 0.61F, 0.69F, 1.0F};
const ImVec4 c_gold{0.95F, 0.77F, 0.25F, 1.0F};
const ImVec4 c_red{0.92F, 0.35F, 0.35F, 1.0F};
const ImVec4 c_card_bg{0.075F, 0.09F, 0.115F, 1.0F};

struct AppFeedbackItem {
    std::string sender;
    std::string severity;
    std::string route;
    std::string description;
    std::string time_str;
};

struct Runtime {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    bool sdl = false, imgui = false, platform = false, render_backend = false;
    ~Runtime() {
        if (render_backend) ImGui_ImplSDLRenderer3_Shutdown();
        if (platform) ImGui_ImplSDL3_Shutdown();
        if (imgui) ImGui::DestroyContext();
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        if (sdl) SDL_Quit();
    }
};

void apply_devlink_theme(float scale) {
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowPadding = {20, 18};
    style.FramePadding = {10, 7};
    style.ItemSpacing = {10, 10};
    style.WindowRounding = 0;
    style.ChildRounding = 8;
    style.FrameRounding = 5;
    style.GrabRounding = 4;
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 1;
    style.FontSizeBase = 16;

    auto* c = style.Colors;
    c[ImGuiCol_WindowBg] = {0.05F, 0.062F, 0.078F, 1.0F};
    c[ImGuiCol_ChildBg] = c_card_bg;
    c[ImGuiCol_Border] = {0.16F, 0.20F, 0.25F, 1.0F};
    c[ImGuiCol_Text] = {0.90F, 0.93F, 0.96F, 1.0F};
    c[ImGuiCol_TextDisabled] = c_muted;
    c[ImGuiCol_FrameBg] = {0.095F, 0.12F, 0.15F, 1.0F};
    c[ImGuiCol_FrameBgHovered] = {0.13F, 0.17F, 0.21F, 1.0F};
    c[ImGuiCol_Button] = {0.12F, 0.26F, 0.24F, 1.0F};
    c[ImGuiCol_ButtonHovered] = {0.16F, 0.38F, 0.33F, 1.0F};
    c[ImGuiCol_ButtonActive] = {0.20F, 0.48F, 0.40F, 1.0F};
    c[ImGuiCol_Header] = {0.12F, 0.25F, 0.23F, 1.0F};
    c[ImGuiCol_HeaderHovered] = {0.16F, 0.33F, 0.29F, 1.0F};
    c[ImGuiCol_TableHeaderBg] = {0.09F, 0.11F, 0.14F, 1.0F};
    c[ImGuiCol_TableRowBgAlt] = {0.10F, 0.13F, 0.16F, 0.40F};
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

void label(const char* text) {
    ImGui::TextColored(c_muted, "%s", text);
}

void stat_card(const char* title, std::size_t number, const char* caption) {
    label(title);
    ImGui::TextColored(c_mint, "%zu", number);
    ImGui::TextWrapped("%s", caption);
}

const char* socket_state_str(plpt::SocketState s) {
    switch (s) {
    case plpt::SocketState::Listening: return "LISTEN";
    case plpt::SocketState::Bound: return "BOUND";
    case plpt::SocketState::Connected: return "CONNECTED";
    case plpt::SocketState::Established: return "ESTABLISHED";
    default: return "INCONNU";
    }
}

// État global de l'application DevLink
struct DevLinkAppState {
    plpt::SystemClock clock;

    // Découverte locale (Moniteur système)
    std::unique_ptr<plpt::Monitor> monitor;

    // Plan de contrôle et données Hôte
    std::unique_ptr<plpt::net::HostServer> host_server;
    std::unique_ptr<plpt::net::ServiceProxy> service_proxy;
    std::unique_ptr<plpt::net::Announcer> announcer;
    bool host_running = false;
    std::uint16_t host_control_port = 0;
    std::uint16_t host_proxy_port = 0;
    plpt::InviteCode host_invite_code{};
    std::string host_service_token;
    std::string host_shared_url;

    // Configuration Hôte
    char session_name_buf[64] = "Démo DevLink";
    char host_name_buf[64] = "Développeur";
    int max_participants = 5;
    bool require_approval = true;
    int participation_mode = 0; // 0=Indépendant, 1=Collaboration, 2=Observation

    // Plan Invité
    std::unique_ptr<plpt::net::GuestClient> guest_client;
    std::unique_ptr<plpt::net::DiscoveryListener> discovery_listener;
    bool guest_connected = false;
    char guest_name_buf[64] = "Testeur";
    char guest_invite_code_buf[64] = "";
    char guest_host_addr_buf[64] = "127.0.0.1";
    int guest_host_port = 0;

    // Moteur de rendu distant / Browser Backend
    std::unique_ptr<plpt::ManagedBrowserBackend> browser_backend;
    plpt::BrowserContextInfo active_browser_context{};
    bool browser_active = false;

    // Messagerie & Feedback
    char chat_input_buf[512] = "";
    std::vector<AppFeedbackItem> feedback_reports;
    char fb_route_buf[128] = "/";
    char fb_desc_buf[512] = "";
    int fb_severity = 1; // 0=Info, 1=Mineur, 2=Majeur, 3=Bloquant

    // Navigation UI
    int app_mode = 0; // 0 = Espace Hôte, 1 = Espace Invité
    int host_page = 0; // 0=Dashboard, 1=Découverte, 2=Configuration, 3=Session, 4=Chat, 5=Feedback, 6=Diagnostics
    int guest_page = 0; // 0=Rejoindre, 1=Application, 2=Chat, 3=Feedback, 4=À Propos

    // Sélection de service dans le moniteur
    plpt::ServiceId selected_service{};
    plpt::Endpoint selected_endpoint{};
    std::string user_notice;
};

void render_host_dashboard(DevLinkAppState& state, const plpt::MonitorView& view) {
    std::size_t tcp = 0, udp = 0, known = 0;
    for (const auto& service : view.services) {
        if (service.share_state == plpt::ShareState::Gone || service.share_state == plpt::ShareState::Unavailable) continue;
        for (const auto& ep : service.endpoints) {
            if (ep.protocol == plpt::TransportProtocol::Tcp) ++tcp; else ++udp;
            if (ep.owner_pid) ++known;
        }
    }

    if (ImGui::BeginTable("metrics_tbl", 4, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn(); stat_card("LISTENERS TCP", tcp, "Serveurs locaux candidats");
        ImGui::TableNextColumn(); stat_card("ENDPOINTS UDP", udp, "Observés, non partagés");
        ImGui::TableNextColumn(); stat_card("PROCESSUS CORRÉLÉS", known, "PIDs avec identité vérifiée");
        ImGui::TableNextColumn();
        if (state.host_running) {
            auto snap = state.host_server->snapshot();
            stat_card("INVITÉS CONNECTÉS", snap->participants.size(), "Sur session active");
        } else {
            stat_card("SESSION DEVLINK", 0, "Inactif / Prêt au partage");
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("État de la Session Hôte");

    if (state.host_running) {
        auto h_snap = state.host_server->snapshot();
        auto p_snap = state.service_proxy->snapshot();

        ImGui::TextColored(c_mint, "● SESSION ACTIVE : %s", h_snap->session_name.c_str());
        ImGui::Text("Hôte : %s  |  Port de contrôle : %u  |  Port proxy exposé : %u",
            h_snap->host_name.c_str(), state.host_control_port, state.host_proxy_port);
        ImGui::TextColored(c_gold, "CODE D'INVITATION : %s", state.host_invite_code.text.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copier le code")) {
            SDL_SetClipboardText(state.host_invite_code.text.c_str());
            state.user_notice = "Code d'invitation copié dans le presse-papiers.";
        }

        ImGui::Text("URL d'accès sécurisée : %s", state.host_shared_url.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copier l'URL")) {
            SDL_SetClipboardText(state.host_shared_url.c_str());
            state.user_notice = "URL d'accès copiée.";
        }

        ImGui::Spacing();
        if (ImGui::Button("Gérer les Invités et Voir la Session")) {
            state.host_page = 3;
        }
        ImGui::SameLine();
        if (ImGui::Button("Arrêter la Session DevLink")) {
            state.host_server->stop();
            state.service_proxy->stop();
            if (state.announcer) state.announcer->stop();
            state.host_running = false;
            state.user_notice = "Session fermée. Toutes les ressources et autorisations ont été révoquées.";
        }
    } else {
        ImGui::TextWrapped("Aucune session DevLink n'est actuellement diffusée. "
            "Sélectionnez un point d'entrée dans le Centre de Découverte pour ouvrir une session sécurisée.");
        if (ImGui::Button("Ouvrir le Centre de Découverte")) {
            state.host_page = 1;
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Garanties Non Négociables DevLink");
    ImGui::BulletText("Aucun accès shell, bureau ou système de fichiers n'est accordé à l'invité.");
    ImGui::BulletText("Aucun code source ni fichier de configuration (.env) n'est transmis.");
    ImGui::BulletText("Chaque testeur indépendant dispose d'un profil navigateur isolé (cookies et stockage distincts).");
    ImGui::BulletText("La fermeture de session révoque immédiatement tous les accès et supprime les contextes temporaires.");
}

void render_discovery_center(DevLinkAppState& state, const plpt::MonitorView& view) {
    static ImGuiTextFilter filter;
    filter.Draw("Recherche / Filtrer", 300);
    ImGui::SameLine();
    if (ImGui::Button("Actualiser le système")) {
        state.user_notice = state.monitor->request_refresh() ? "Actualisation en cours..." : "File de commandes occupée.";
    }

    label("Sélectionnez le service Web à partager. Aucun port n'est partagé automatiquement.");

    const auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX;

    if (ImGui::BeginTable("discovery_tbl", 8, flags, {0, 320})) {
        ImGui::TableSetupColumn("PROTO / PORT", ImGuiTableColumnFlags_WidthFixed, 115);
        ImGui::TableSetupColumn("ADRESSE", ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableSetupColumn("PROCESSUS / PID", ImGuiTableColumnFlags_WidthFixed, 160);
        ImGui::TableSetupColumn("ORIGINE PROBABLE", ImGuiTableColumnFlags_WidthFixed, 190);
        ImGui::TableSetupColumn("CONFIANCE", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("ÉTAT", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("PARTAGE", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("ACTION", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableHeadersRow();

        int idx = 0;
        for (const auto& service : view.services) {
            for (const auto& ep : service.endpoints) {
                const auto proc_name = service.process ? service.process->image_name : std::string("inconnu");
                const auto searchable = ep.local.address + " " + std::to_string(ep.local.port.value) + " " + proc_name;
                if (!filter.PassFilter(searchable.c_str())) continue;

                ImGui::PushID(idx++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                const auto proto_port = std::string(ep.protocol == plpt::TransportProtocol::Tcp ? "TCP  " : "UDP  ") +
                    std::to_string(ep.local.port.value);
                const bool is_selected = (state.selected_service == service.service_id && state.selected_endpoint == ep.local);

                if (ImGui::Selectable(proto_port.c_str(), is_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                    state.selected_service = service.service_id;
                    state.selected_endpoint = ep.local;
                }

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(ep.local.address.c_str());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(proc_name.c_str());
                if (ep.owner_pid) ImGui::TextDisabled("PID %u", ep.owner_pid->value);

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(service.origin.label.c_str());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(plpt::to_string(service.origin.confidence).data());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(socket_state_str(ep.state));

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(plpt::to_string(service.share_state).data());

                ImGui::TableNextColumn();
                if (ep.protocol == plpt::TransportProtocol::Tcp && ep.state == plpt::SocketState::Listening) {
                    if (ImGui::SmallButton("Choisir")) {
                        state.selected_service = service.service_id;
                        state.selected_endpoint = ep.local;
                        state.host_page = 2; // Direction configuration
                    }
                } else {
                    ImGui::TextDisabled("Non partageable");
                }

                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Détails du Service Sélectionné");

    const auto found = std::find_if(view.services.begin(), view.services.end(),
        [&](const auto& s) { return s.service_id == state.selected_service; });

    if (found != view.services.end()) {
        const auto& s = *found;
        ImGui::Text("Service #%llu  |  Point d'entrée : %s:%u",
            static_cast<unsigned long long>(state.selected_service.value),
            state.selected_endpoint.address.c_str(), state.selected_endpoint.port.value);
        for (const auto& ev : s.origin.evidence) ImGui::BulletText("%s", ev.c_str());

        if (ImGui::Button("Configurer le Partage de ce Service")) {
            state.host_page = 2;
        }
    } else {
        ImGui::TextDisabled("Sélectionnez une ligne dans le tableau pour examiner ses relations et l'autoriser.");
    }
}

void render_share_setup(DevLinkAppState& state, const plpt::MonitorView& view) {
    (void)view;
    ImGui::SeparatorText("Configuration du Partage Applicatif");

    if (state.selected_endpoint.port.value == 0) {
        ImGui::TextColored(c_gold, "Aucun point d'entrée sélectionné.");
        if (ImGui::Button("Choisir un service dans le Centre de Découverte")) state.host_page = 1;
        return;
    }

    ImGui::Text("Point d'entrée cible : %s:%u", state.selected_endpoint.address.c_str(), state.selected_endpoint.port.value);
    ImGui::Spacing();

    ImGui::InputText("Nom de la session", state.session_name_buf, sizeof(state.session_name_buf));
    ImGui::InputText("Nom de l'hôte", state.host_name_buf, sizeof(state.host_name_buf));
    ImGui::SliderInt("Nombre max d'invités", &state.max_participants, 1, 10);
    ImGui::Checkbox("Exiger une validation manuelle de chaque invité", &state.require_approval);

    const char* modes[] = {"Test indépendant (profils navigateur séparés)", "Collaboration (vue partagée avec contrôle exclusif)", "Observation seule"};
    ImGui::Combo("Mode de participation", &state.participation_mode, modes, 3);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (!state.host_running) {
        if (ImGui::Button("Lancer la Session DevLink", {260, 40})) {
            // 1. Démarrer le serveur de contrôle
            plpt::net::HostConfig h_cfg;
            h_cfg.session_name = state.session_name_buf;
            h_cfg.host_name = state.host_name_buf;
            h_cfg.bind_address = plpt::IpAddress::loopback(plpt::IpFamily::Ipv4);
            h_cfg.max_participants = static_cast<std::size_t>(state.max_participants);
            h_cfg.approval_required = state.require_approval;

            auto invite_res = plpt::generate_invite_code();
            if (invite_res) {
                state.host_invite_code = invite_res.value();
                h_cfg.invite_code = state.host_invite_code;
            }

            state.host_server = std::make_unique<plpt::net::HostServer>(state.clock);
            auto start_h = state.host_server->start(h_cfg);

            if (start_h) {
                state.host_control_port = start_h.value();

                // 2. Démarrer le proxy de données sécurisé
                auto tok_res = plpt::generate_service_token();
                state.host_service_token = tok_res ? tok_res.value() : "devlink_token";

                plpt::net::ProxyConfig p_cfg;
                p_cfg.label = state.session_name_buf;
                p_cfg.bind_address = plpt::IpAddress::loopback(plpt::IpFamily::Ipv4);
                p_cfg.exposed_port = 0; // Port dynamique obligatoire
                p_cfg.upstream_address = plpt::IpAddress::loopback(plpt::IpFamily::Ipv4);
                p_cfg.upstream_port = state.selected_endpoint.port.value;
                p_cfg.token = state.host_service_token;

                state.service_proxy = std::make_unique<plpt::net::ServiceProxy>(state.clock);
                auto start_p = state.service_proxy->start(p_cfg);

                if (start_p) {
                    state.host_proxy_port = start_p.value();
                    state.host_shared_url = "http://127.0.0.1:" + std::to_string(state.host_proxy_port) + "/?plpt=" + state.host_service_token;

                    // Publier les services sur le plan de contrôle
                    std::vector<plpt::net::SharedServiceRecord> s_list;
                    plpt::net::SharedServiceRecord rec;
                    rec.key = 1;
                    rec.label = state.session_name_buf;
                    rec.local_port = state.selected_endpoint.port.value;
                    rec.exposed_port = state.host_proxy_port;
                    rec.token = state.host_service_token;
                    rec.url = state.host_shared_url;
                    s_list.push_back(rec);
                    state.host_server->set_services(s_list);

                    // 3. Démarrer l'annonceur LAN
                    plpt::net::Announcement ann;
                    ann.session_id = 1;
                    ann.session_name = state.session_name_buf;
                    ann.host_name = state.host_name_buf;
                    ann.control_port = state.host_control_port;
                    ann.participants = 0;
                    ann.max_participants = static_cast<std::uint16_t>(state.max_participants);
                    ann.shared_services = 1;
                    ann.accepting = true;

                    plpt::net::Announcer::Config a_cfg;
                    a_cfg.announcement = ann;
                    a_cfg.extra_targets.push_back({plpt::IpAddress::loopback(plpt::IpFamily::Ipv4), plpt::Port{plpt::net::kDiscoveryPort}});

                    state.announcer = std::make_unique<plpt::net::Announcer>(state.clock);
                    (void)state.announcer->start(a_cfg);

                    state.host_running = true;
                    state.host_page = 3; // Aller à la vue session active
                    state.user_notice = "Session DevLink démarrée avec succès.";
                } else {
                    state.user_notice = "Erreur au démarrage du proxy de données : " + start_p.error().message;
                    state.host_server->stop();
                }
            } else {
                state.user_notice = "Erreur au démarrage du serveur de contrôle : " + start_h.error().message;
            }
        }
    } else {
        ImGui::TextColored(c_mint, "Une session est déjà en cours d'exécution.");
        if (ImGui::Button("Gérer la Session Active")) state.host_page = 3;
    }
}

void render_active_session(DevLinkAppState& state) {
    if (!state.host_running) {
        ImGui::TextDisabled("Aucune session n'est en cours. Démarrez une session depuis la Configuration.");
        return;
    }

    auto snap = state.host_server->snapshot();
    auto p_snap = state.service_proxy->snapshot();

    ImGui::SeparatorText("Contrôle de la Session Active");
    ImGui::Text("Session : %s  |  Hôte : %s", snap->session_name.c_str(), snap->host_name.c_str());
    ImGui::TextColored(c_gold, "CODE D'INVITATION : %s", state.host_invite_code.text.c_str());
    ImGui::Text("Port de contrôle : %u  |  Port proxy : %u", state.host_control_port, state.host_proxy_port);
    ImGui::Text("URL partagée : %s", state.host_shared_url.c_str());

    // Demandes en attente d'approbation
    ImGui::Spacing();
    ImGui::SeparatorText("Demandes d'Adhésion en Attente");
    if (snap->pending.empty()) {
        ImGui::TextDisabled("Aucune demande en attente.");
    } else {
        for (const auto& req : snap->pending) {
            ImGui::PushID(static_cast<int>(req.request_id));
            ImGui::Text("● %s (%s) - Client : %s", req.guest_name.c_str(), req.address.c_str(), req.client.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Autoriser")) {
                state.host_server->approve(req.request_id);
                state.user_notice = "Invité autorisé : " + req.guest_name;
            }
            ImGui::SameLine();
            if (ImGui::Button("Refuser")) {
                state.host_server->deny(req.request_id, "refusé par l'hôte");
                state.user_notice = "Demande refusée pour : " + req.guest_name;
            }
            ImGui::PopID();
        }
    }

    // Liste des participants connectés
    ImGui::Spacing();
    ImGui::SeparatorText("Participants Connectés");
    if (snap->participants.empty()) {
        ImGui::TextDisabled("En attente de participants...");
    } else {
        if (ImGui::BeginTable("parts_tbl", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("ID");
            ImGui::TableSetupColumn("Nom");
            ImGui::TableSetupColumn("Adresse");
            ImGui::TableSetupColumn("Statut");
            ImGui::TableSetupColumn("Latence");
            ImGui::TableSetupColumn("Action");
            ImGui::TableHeadersRow();

            for (const auto& p : snap->participants) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(p.id.value));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(p.name.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(p.address.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(plpt::net::participant_state_text(p.state));
                ImGui::TableNextColumn(); ImGui::Text("%u ms", p.latency_ms);
                ImGui::TableNextColumn();
                if (p.state != plpt::model::ParticipantState::Revoked) {
                    ImGui::PushID(static_cast<int>(p.id.value));
                    if (ImGui::SmallButton("Révoquer")) {
                        state.host_server->revoke(p.id);
                        state.user_notice = "Participant révoqué : " + p.name;
                    }
                    ImGui::PopID();
                } else {
                    ImGui::TextDisabled("Révoqué");
                }
            }
            ImGui::EndTable();
        }
    }

    // Statistiques du Proxy
    ImGui::Spacing();
    ImGui::SeparatorText("Métriques Réseau du Proxy");
    ImGui::Text("Connexions actives : %zu  |  Requêtes : %llu  |  WebSocket : %llu",
        p_snap->active_connections,
        static_cast<unsigned long long>(p_snap->counters.requests),
        static_cast<unsigned long long>(p_snap->counters.websocket_upgrades));
    ImGui::Text("Octets envoyés aux invités : %llu  |  Octets vers service local : %llu",
        static_cast<unsigned long long>(p_snap->counters.bytes_to_guest),
        static_cast<unsigned long long>(p_snap->counters.bytes_to_upstream));

    ImGui::Spacing();
    if (ImGui::Button("Fermer la Session Définitivement")) {
        state.host_server->stop();
        state.service_proxy->stop();
        if (state.announcer) state.announcer->stop();
        state.host_running = false;
        state.user_notice = "Session fermée. Ressources libérées.";
    }
}

void render_chat(DevLinkAppState& state, bool is_host) {
    ImGui::SeparatorText("Messagerie de Session Intégrée");

    // Affichage des messages
    ImGui::BeginChild("chat_scroll", {0, -45}, ImGuiChildFlags_Borders);
    if (is_host && state.host_running) {
        // En mode hôte, on affiche l'historique récent
        auto snap = state.host_server->snapshot();
        for (const auto& ev : snap->events) {
            ImGui::TextColored(c_muted, "%s", ev.c_str());
        }
    } else if (!is_host && state.guest_client && state.guest_connected) {
        auto snap = state.guest_client->snapshot();
        for (const auto& msg : snap->chat) {
            if (msg.system) {
                ImGui::TextColored(c_gold, "● [SYSTÈME] %s", msg.body.c_str());
            } else if (msg.direct) {
                ImGui::TextColored(c_cyan, "[PRIVÉ] %s : %s", msg.sender_name.c_str(), msg.body.c_str());
            } else {
                ImGui::TextColored(c_mint, "%s : ", msg.sender_name.c_str());
                ImGui::SameLine();
                ImGui::TextUnformatted(msg.body.c_str());
            }
        }
    } else {
        ImGui::TextDisabled("Connectez-vous à une session pour participer au chat.");
    }
    ImGui::EndChild();

    // Saisie du message
    ImGui::SetNextItemWidth(-100);
    bool send = ImGui::InputText("##chat_in", state.chat_input_buf, sizeof(state.chat_input_buf), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Envoyer", {90, 0}) || send) {
        if (state.chat_input_buf[0] != '\0') {
            if (is_host && state.host_running) {
                state.host_server->post_chat(state.chat_input_buf);
            } else if (!is_host && state.guest_connected) {
                state.guest_client->post_chat(state.chat_input_buf);
            }
            state.chat_input_buf[0] = '\0';
        }
    }
}

void render_feedback(DevLinkAppState& state, bool is_host) {
    ImGui::SeparatorText(is_host ? "Signalements & Retours des Testeurs" : "Signaler une Anomalie à l'Hôte");

    if (is_host) {
        if (state.feedback_reports.empty()) {
            ImGui::TextDisabled("Aucun signalement d'anomalie reçu.");
        } else {
            if (ImGui::BeginTable("fb_tbl", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Heure");
                ImGui::TableSetupColumn("Auteur");
                ImGui::TableSetupColumn("Gravité");
                ImGui::TableSetupColumn("Description");
                ImGui::TableHeadersRow();

                for (const auto& fb : state.feedback_reports) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(fb.time_str.c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(fb.sender.c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(fb.severity.c_str());
                    ImGui::TableNextColumn(); ImGui::TextWrapped("%s (Page : %s)", fb.description.c_str(), fb.route.c_str());
                }
                ImGui::EndTable();
            }
        }
    } else {
        ImGui::InputText("Route / Page concernée", state.fb_route_buf, sizeof(state.fb_route_buf));
        const char* sevs[] = {"Informatif", "Mineur", "Majeur", "Bloquant"};
        ImGui::Combo("Gravité", &state.fb_severity, sevs, 4);
        ImGui::InputTextMultiline("Description de l'anomalie", state.fb_desc_buf, sizeof(state.fb_desc_buf), {0, 100});

        if (ImGui::Button("Transmettre le Signalement", {200, 32})) {
            if (state.fb_desc_buf[0] != '\0') {
                AppFeedbackItem item;
                item.sender = state.guest_name_buf;
                item.severity = sevs[state.fb_severity];
                item.route = state.fb_route_buf;
                item.description = state.fb_desc_buf;
                item.time_str = "Récemment";
                state.feedback_reports.push_back(item);

                if (state.guest_connected) {
                    state.guest_client->post_chat(std::string("[BUG ") + sevs[state.fb_severity] + "] " + state.fb_desc_buf);
                }
                state.fb_desc_buf[0] = '\0';
                state.user_notice = "Signalement d'anomalie transmis à l'hôte.";
            }
        }
    }
}

void render_guest_join(DevLinkAppState& state) {
    ImGui::SeparatorText("Rejoindre une Session DevLink sur le Réseau Local");

    // Liste des sessions découvertes
    ImGui::TextColored(c_mint, "Sessions Détectées sur le Réseau :");
    if (state.discovery_listener) {
        auto snap = state.discovery_listener->snapshot();
        if (snap && !snap->empty()) {
            for (const auto& sess : *snap) {
                ImGui::BulletText("Session : '%s' par %s (Hôte : %s:%u) [%zu/%zu invités]",
                    sess.session_name.c_str(), sess.host_name.c_str(),
                    sess.address.text.c_str(), sess.control_port,
                    sess.participants, sess.max_participants);
                ImGui::SameLine();
                ImGui::PushID(static_cast<int>(sess.control_port));
                if (ImGui::SmallButton("Sélectionner")) {
                    std::snprintf(state.guest_host_addr_buf, sizeof(state.guest_host_addr_buf), "%s", sess.address.text.c_str());
                    state.guest_host_port = sess.control_port;
                }
                ImGui::PopID();
            }
        } else {
            ImGui::TextDisabled("Recherche de sessions actives en cours (mDNS / Multicast 239.255.77.88)...");
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Connexion Manuelle ou Directe");
    ImGui::InputText("Votre nom de testeur", state.guest_name_buf, sizeof(state.guest_name_buf));
    ImGui::InputText("Adresse IP de l'hôte", state.guest_host_addr_buf, sizeof(state.guest_host_addr_buf));
    ImGui::InputInt("Port de contrôle hôte", &state.guest_host_port);
    ImGui::InputText("Code d'invitation (16 car.)", state.guest_invite_code_buf, sizeof(state.guest_invite_code_buf));

    ImGui::Spacing();
    if (!state.guest_connected) {
        if (ImGui::Button("Rejoindre la Session", {220, 36})) {
            if (state.guest_host_port <= 0 || state.guest_invite_code_buf[0] == '\0') {
                state.user_notice = "Veuillez renseigner un port valide et le code d'invitation fourni par l'hôte.";
            } else {
                auto code_res = plpt::decode_invite_code(state.guest_invite_code_buf);
                if (!code_res) {
                    state.user_notice = "Format de code d'invitation invalide.";
                } else {
                    plpt::InviteCode code;
                    code.bytes = code_res.value();
                    code.text = state.guest_invite_code_buf;

                    plpt::net::GuestConfig g_cfg;
                    g_cfg.address = plpt::IpAddress::parse(state.guest_host_addr_buf).value();
                    g_cfg.control_port = static_cast<std::uint16_t>(state.guest_host_port);
                    g_cfg.guest_name = state.guest_name_buf;
                    g_cfg.invite_code = code;

                    state.guest_client = std::make_unique<plpt::net::GuestClient>(state.clock);
                    if (state.guest_client->start(g_cfg)) {
                        state.guest_connected = true;
                        state.guest_page = 1; // Aller à la vue application
                        state.user_notice = "Demande d'admission envoyée à l'hôte...";
                    } else {
                        state.user_notice = "Échec de connexion vers l'hôte.";
                    }
                }
            }
        }
    } else {
        auto snap = state.guest_client->snapshot();
        ImGui::TextColored(c_mint, "Statut : %s", plpt::net::guest_phase_text(snap->phase));
        if (ImGui::Button("Aller à la Vue Session")) state.guest_page = 1;
        ImGui::SameLine();
        if (ImGui::Button("Se déconnecter")) {
            state.guest_client->stop();
            state.guest_connected = false;
            state.user_notice = "Déconnecté de la session.";
        }
    }
}

void render_guest_app(DevLinkAppState& state) {
    if (!state.guest_connected || !state.guest_client) {
        ImGui::TextDisabled("Vous n'êtes pas connecté à une session DevLink.");
        if (ImGui::Button("Rejoindre une Session")) state.guest_page = 0;
        return;
    }

    auto snap = state.guest_client->snapshot();
    ImGui::SeparatorText("Application Partagée");

    ImGui::Text("Session : %s  |  Hôte : %s  |  Statut : %s  |  Latence : %u ms",
        snap->session_name.c_str(), snap->host_name.c_str(),
        plpt::net::guest_phase_text(snap->phase), snap->latency_ms);

    if (snap->phase == plpt::net::GuestPhase::AwaitingApproval) {
        ImGui::TextColored(c_gold, "En attente de l'autorisation de l'hôte...");
        ImGui::TextWrapped("Votre code a été validé. L'hôte doit cliquer sur 'Autoriser' sur son écran.");
        return;
    }

    if (snap->phase != plpt::net::GuestPhase::Connected) {
        ImGui::TextColored(c_red, "Connexion non active : %s", snap->status_text.c_str());
        return;
    }

    // Affichage des services publiés
    if (snap->services.empty()) {
        ImGui::TextDisabled("Aucun service partagé disponible pour le moment.");
    } else {
        for (const auto& s : snap->services) {
            ImGui::BulletText("Service : %s (Port exposé : %u)", s.label.c_str(), s.exposed_port);
            ImGui::TextColored(c_cyan, "URL de Test : %s", s.url.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Copier")) {
                SDL_SetClipboardText(s.url.c_str());
                state.user_notice = "URL copiée dans le presse-papiers.";
            }

            // Lancement du navigateur de test
            if (ImGui::Button("Lancer le Navigateur de Test Isolé")) {
                plpt::BrowserPolicy pol;
                pol.allowed_origins.push_back("http://127.0.0.1:" + std::to_string(s.exposed_port));
                pol.allowed_origins.push_back("http://localhost:" + std::to_string(s.exposed_port));

                auto ctx_res = state.browser_backend->create_context(snap->self, pol);
                if (ctx_res) {
                    state.active_browser_context = ctx_res.value();
                    (void)state.browser_backend->navigate(state.active_browser_context.context, s.url);
                    state.browser_active = true;
                    state.user_notice = "Contexte de test isolé créé avec profil temporaire distinct.";
                }
            }
        }
    }

    // Surface de composition distante
    if (state.browser_active) {
        ImGui::Spacing();
        ImGui::SeparatorText("Flux de Rendu Distant (Off-Screen Composition)");
        auto frame_res = state.browser_backend->capture_frame(state.active_browser_context.context);
        if (frame_res) {
            ImGui::TextColored(c_mint, "● Flux actif (Résolution 128x64 composition, Profil %s)",
                state.active_browser_context.profile_id.c_str());
            ImGui::Text("Taille frame brute : %zu octets", frame_res.value().size());
        }
    }
}

void render_diagnostics(DevLinkAppState& state, const plpt::MonitorView& view) {
    ImGui::SeparatorText("Diagnostics Système et Réseau DevLink");

    ImGui::Text("Plateforme : Windows x64  |  Compilateur : Clang++ 22 UCRT64  |  Standard : C++20");
    ImGui::Text("Moteur Graphique : SDL3 3.4.20 + Dear ImGui 1.93.0");
    ImGui::Spacing();

    if (state.host_running) {
        auto h_snap = state.host_server->snapshot();
        ImGui::TextColored(c_mint, "Compteurs Sécurité Hôte :");
        ImGui::BulletText("Connexions totales : %llu", static_cast<unsigned long long>(h_snap->counters.connections_total));
        ImGui::BulletText("Invités admis : %llu", static_cast<unsigned long long>(h_snap->counters.admitted));
        ImGui::BulletText("Demandes refusées : %llu", static_cast<unsigned long long>(h_snap->counters.denied));
        ImGui::BulletText("Codes d'invitation erronés bloqués : %llu", static_cast<unsigned long long>(h_snap->counters.bad_codes));
        ImGui::BulletText("Révocations exécutées : %llu", static_cast<unsigned long long>(h_snap->counters.revoked));
        ImGui::BulletText("Messages relayés : %llu", static_cast<unsigned long long>(h_snap->counters.chat_relayed));
    }

    ImGui::Spacing();
    ImGui::TextColored(c_gold, "Avertissements et Journal Système :");
    for (const auto& w : view.warnings) {
        ImGui::TextDisabled("%s : %s (%lld)", w.context.c_str(), w.message.c_str(), static_cast<long long>(w.native_code));
    }
}

} // namespace

int main(int argc, char** argv) {
    bool smoke = false;
    std::string screenshot;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--smoke-test") smoke = true;
        else if (std::string_view(argv[i]) == "--screenshot" && i + 1 < argc) screenshot = argv[++i];
        else { std::fprintf(stderr, "Usage: plpt-desktop [--smoke-test [--screenshot file.bmp]]\n"); return 2; }
    }
    if (!smoke && !screenshot.empty()) {
        std::fprintf(stderr, "--screenshot exige --smoke-test. Aucune capture interactive implicite.\n");
        return 2;
    }

    Runtime runtime;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    runtime.sdl = true;

    if (SDL_GetVersion() != SDL_VERSION) {
        std::fprintf(stderr, "SDL runtime/header version mismatch: runtime=%d headers=%d\n", SDL_GetVersion(), SDL_VERSION);
        return 1;
    }

    runtime.window = SDL_CreateWindow(window_title.data(), 1440, 920, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!runtime.window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    runtime.renderer = SDL_CreateRenderer(runtime.window, nullptr);
    if (!runtime.renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }

    if (smoke) {
        std::printf("desktop-runtime: SDL=%d video=%s renderer=%s\n",
            SDL_GetVersion(), SDL_GetCurrentVideoDriver(), SDL_GetRendererName(runtime.renderer));
    }

    (void)SDL_SetRenderVSync(runtime.renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    runtime.imgui = true;

    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    float scale = std::max(1.0F, SDL_GetWindowDisplayScale(runtime.window));
    apply_devlink_theme(scale);

    runtime.platform = ImGui_ImplSDL3_InitForSDLRenderer(runtime.window, runtime.renderer);
    if (!runtime.platform) {
        std::fprintf(stderr, "ImGui SDL3 initialization failed\n");
        return 1;
    }

    runtime.render_backend = ImGui_ImplSDLRenderer3_Init(runtime.renderer);
    if (!runtime.render_backend) {
        std::fprintf(stderr, "ImGui renderer initialization failed\n");
        return 1;
    }

    // Instanciation de l'état applicatif DevLink
    DevLinkAppState app;
    app.monitor = std::make_unique<plpt::Monitor>(
        std::make_unique<plpt::NativeProcessEnumerator>(),
        std::make_unique<plpt::NativeSocketTableProvider>());
    app.browser_backend = std::make_unique<plpt::ManagedBrowserBackend>();

    // Écoute de découverte LAN pour les sessions sur le réseau
    app.discovery_listener = std::make_unique<plpt::net::DiscoveryListener>(app.clock);
    plpt::net::DiscoveryListener::Config disc_cfg;
    disc_cfg.port = plpt::Port{0};
    (void)app.discovery_listener->start(disc_cfg);

    unsigned frames = 0;
    bool quit = false;

    while (!quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(runtime.window))) {
                quit = true;
            }
            if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
                scale = std::max(1.0F, SDL_GetWindowDisplayScale(runtime.window));
                ImGui::GetStyle() = ImGuiStyle{};
                apply_devlink_theme(scale);
            }
        }

        if (!smoke && (SDL_GetWindowFlags(runtime.window) & SDL_WINDOW_MINIMIZED)) {
            SDL_Delay(50);
            continue;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        const auto view = app.monitor->view();

        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("DevLink_Root", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

        // En-tête DevLink
        ImGui::TextColored(c_mint, "DevLink");
        ImGui::SameLine();
        ImGui::TextDisabled("  |  PLATEFORME NATIVE DE PARTAGE TEMPORAIRE & COLLABORATIF");
        ImGui::SameLine();
        ImGui::SetCursorPosX(io.DisplaySize.x - 340 * scale);

        // Sélecteur Espace Hôte / Espace Invité
        if (ImGui::Button(app.app_mode == 0 ? "[ Espace Hôte ]" : "Espace Hôte", {140 * scale, 28 * scale})) app.app_mode = 0;
        ImGui::SameLine();
        if (ImGui::Button(app.app_mode == 1 ? "[ Espace Invité ]" : "Espace Invité", {140 * scale, 28 * scale})) app.app_mode = 1;

        ImGui::Separator();

        // Barre latérale de navigation
        ImGui::BeginChild("sidebar", {220 * scale, 0}, ImGuiChildFlags_Borders);
        if (app.app_mode == 0) {
            label("MODE HÔTE (PARTAGE)");
            ImGui::Spacing();
            const char* h_pages[] = {
                "Tableau de bord",
                "Centre de découverte",
                "Configurer le partage",
                "Session active",
                "Messagerie de session",
                "Retours & Feedback",
                "Diagnostics système"
            };
            for (int i = 0; i < 7; ++i) {
                if (ImGui::Selectable(h_pages[i], app.host_page == i, 0, {0, 32 * scale})) app.host_page = i;
            }
        } else {
            label("MODE INVITÉ (TEST)");
            ImGui::Spacing();
            const char* g_pages[] = {
                "Rejoindre une session",
                "Application partagée",
                "Messagerie de session",
                "Signaler une anomalie",
                "Diagnostics"
            };
            for (int i = 0; i < 5; ++i) {
                if (ImGui::Selectable(g_pages[i], app.guest_page == i, 0, {0, 32 * scale})) app.guest_page = i;
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (app.host_running) {
            ImGui::TextColored(c_mint, "● Hôte En Ligne");
            ImGui::TextDisabled("Port : %u", app.host_control_port);
        } else {
            ImGui::TextDisabled("○ Hôte Inactif");
        }

        if (app.guest_connected) {
            ImGui::TextColored(c_cyan, "● Invité Connecté");
        } else {
            ImGui::TextDisabled("○ Invité Déconnecté");
        }

        ImGui::EndChild();

        ImGui::SameLine();

        // Zone de contenu principal
        ImGui::BeginChild("main_content", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);

        if (!app.user_notice.empty()) {
            ImGui::TextColored(c_gold, "Information : %s", app.user_notice.c_str());
            ImGui::Separator();
            ImGui::Spacing();
        }

        if (app.app_mode == 0) {
            // Pages Hôte
            if (app.host_page == 0) render_host_dashboard(app, *view);
            else if (app.host_page == 1) render_discovery_center(app, *view);
            else if (app.host_page == 2) render_share_setup(app, *view);
            else if (app.host_page == 3) render_active_session(app);
            else if (app.host_page == 4) render_chat(app, true);
            else if (app.host_page == 5) render_feedback(app, true);
            else if (app.host_page == 6) render_diagnostics(app, *view);
        } else {
            // Pages Invité
            if (app.guest_page == 0) render_guest_join(app);
            else if (app.guest_page == 1) render_guest_app(app);
            else if (app.guest_page == 2) render_chat(app, false);
            else if (app.guest_page == 3) render_feedback(app, false);
            else if (app.guest_page == 4) render_diagnostics(app, *view);
        }

        ImGui::EndChild();
        ImGui::End();

        ImGui::Render();

        if (!SDL_SetRenderScale(runtime.renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y) ||
            !SDL_SetRenderDrawColor(runtime.renderer, 12, 15, 20, 255) ||
            !SDL_RenderClear(runtime.renderer)) {
            std::fprintf(stderr, "SDL render preparation: %s\n", SDL_GetError());
            return 1;
        }

        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), runtime.renderer);
        ++frames;

        if (smoke && frames >= 8 && view->revision > 0) {
            if (!screenshot.empty()) {
                std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
                    SDL_RenderReadPixels(runtime.renderer, nullptr), SDL_DestroySurface);
                if (!surface) {
                    std::fprintf(stderr, "SDL_RenderReadPixels: %s\n", SDL_GetError());
                    return 1;
                }
                std::unique_ptr<SDL_IOStream, decltype(&SDL_CloseIO)> stream(
                    SDL_IOFromFile(screenshot.c_str(), "wb"), SDL_CloseIO);
                if (!stream) {
                    std::fprintf(stderr, "SDL_IOFromFile: %s\n", SDL_GetError());
                    return 1;
                }
                const bool saved = SDL_SaveBMP_IO(surface.get(), stream.get(), false);
                if (!saved) std::fprintf(stderr, "SDL_SaveBMP_IO: %s\n", SDL_GetError());
                const bool closed = SDL_CloseIO(stream.release());
                if (!closed) std::fprintf(stderr, "SDL_CloseIO: %s\n", SDL_GetError());
                if (!saved || !closed) return 1;
            }
            std::printf("desktop-smoke: %u frames, snapshot=%llu, services=%zu, sharing=disabled\n",
                frames, static_cast<unsigned long long>(view->revision), view->services.size());
            quit = true;
        }

        if (!SDL_RenderPresent(runtime.renderer)) {
            std::fprintf(stderr, "SDL_RenderPresent: %s\n", SDL_GetError());
            return 1;
        }

        if (smoke && frames > 600) {
            std::fprintf(stderr, "desktop-smoke: observation timeout\n");
            return 1;
        }

        SDL_Delay(16);
    }

    // Nettoyage complet avant sortie
    if (app.host_running) {
        if (app.host_server) app.host_server->stop();
        if (app.service_proxy) app.service_proxy->stop();
        if (app.announcer) app.announcer->stop();
    }
    if (app.guest_connected && app.guest_client) {
        app.guest_client->stop();
    }
    if (app.discovery_listener) {
        app.discovery_listener->stop();
    }
    if (app.browser_backend) {
        app.browser_backend->cleanup_all();
    }

    return 0;
}
