// DevLink — app/desktop_ui/main.cpp
// Station de Partage et de Test Desktop Native.
// Architecture C++20 / SDL3 / Dear ImGui.
// Conformité intégrale Trajectoire 1 (Identité, Direction artistique, Design system)
// et Trajectoire 2 (Partage applicatif local, Browser Worker, Rendu distant, Interactions).

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "design_system/tokens.hpp"
#include "design_system/themes.hpp"
#include "design_system/typography.hpp"
#include "design_system/components.hpp"
#include "design_system/icons.hpp"
#include "design_system/layout.hpp"

#include "engine/browser/browser.hpp"
#include "engine/browser/remote_frame.hpp"
#include "engine/browser/webview2_backend.hpp"
#include "engine/core/clock.hpp"
#include "engine/core/random.hpp"
#include "engine/core/secrets.hpp"
#include "engine/discovery/monitor.hpp"
#include "engine/net/discovery.hpp"
#include "engine/net/guest_client.hpp"
#include "engine/net/host_server.hpp"
#include "engine/net/proxy.hpp"
#include "engine/net/secure_channel.hpp"
#include "engine/platform/native.hpp"
#include "engine/port_allocator/port_allocator.hpp"

namespace {

constexpr std::string_view kAppTitle = "DevLink - Plateforme de Partage & Test Local";
constexpr std::string_view kAppAsciiTitle = "DevLink - Local App Sharing & Test Platform";

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
    SDL_Texture* stream_texture = nullptr;
    int stream_tex_w = 0;
    int stream_tex_h = 0;

    bool sdl = false, imgui = false, platform = false, render_backend = false;
    ~Runtime() {
        if (stream_texture) SDL_DestroyTexture(stream_texture);
        if (render_backend) ImGui_ImplSDLRenderer3_Shutdown();
        if (platform) ImGui_ImplSDL3_Shutdown();
        if (imgui) ImGui::DestroyContext();
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        if (sdl) SDL_Quit();
    }

    void ensure_texture(int width, int height) {
        if (stream_texture && (stream_tex_w != width || stream_tex_h != height)) {
            SDL_DestroyTexture(stream_texture);
            stream_texture = nullptr;
        }
        if (!stream_texture && renderer && width > 0 && height > 0) {
            stream_texture = SDL_CreateTexture(
                renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, width, height);
            stream_tex_w = width;
            stream_tex_h = height;
        }
    }

    void update_texture_pixels(const std::uint8_t* rgba, int width, int height) {
        ensure_texture(width, height);
        if (stream_texture && rgba) {
            SDL_UpdateTexture(stream_texture, nullptr, rgba, width * 4);
        }
    }
};

// État global de l'application DevLink
struct DevLinkAppState {
    plpt::SystemClock clock;

    // Navigation & Interface
    devlink::ui::MainNavigationTab current_tab = devlink::ui::MainNavigationTab::Dashboard;
    bool is_host_mode = true;
    devlink::ui::ThemeMode theme_mode = devlink::ui::ThemeMode::DevDark;

    // Découverte locale (Moniteur système)
    std::unique_ptr<plpt::Monitor> monitor;

    // Browser Worker & Rendu distant (§14, §15)
    std::unique_ptr<plpt::IRemoteBrowserBackend> browser_backend;
    plpt::ContextId host_browser_ctx{};
    std::vector<std::uint8_t> latest_frame_rgba;
    int frame_w = 640;
    int frame_h = 360;

    // Plan de contrôle et données Hôte (§10, §11, §13)
    std::unique_ptr<plpt::net::HostServer> host_server;
    std::unique_ptr<plpt::net::ServiceProxy> service_proxy;
    std::unique_ptr<plpt::net::Announcer> announcer;
    bool host_running = false;
    std::uint16_t host_control_port = 0;
    std::uint16_t host_proxy_port = 0;
    plpt::InviteCode host_invite_code{};
    std::string host_service_token;
    std::string host_shared_url = "http://127.0.0.1:3000";
    std::string selected_bind_address = "127.0.0.1";

    // Configuration du partage (§6.2)
    char session_name_buf[64] = "Marketplace Next.js Dev";
    char host_name_buf[64] = "Lead Developer";
    char target_url_buf[128] = "http://127.0.0.1:3000";
    char allowed_origins_buf[256] = "http://127.0.0.1:3000,http://localhost:3000";
    int max_participants = 5;
    bool require_approval = true;
    int participation_mode = 0; // 0=Indépendant (Mode A), 1=Collaboration (Mode B), 2=Observation (Mode C)

    // Plan Invité (§6.2, §16)
    std::unique_ptr<plpt::net::GuestClient> guest_client;
    std::unique_ptr<plpt::net::DiscoveryListener> discovery_listener;
    bool guest_connected = false;
    char guest_name_buf[64] = "Testeur-Alice";
    char guest_invite_code_buf[64] = "";
    char guest_host_addr_buf[64] = "127.0.0.1";
    int guest_host_port = 0;
    std::string guest_status_msg = "Prêt à rejoindre une session";
    bool guest_has_exclusive_control = false;

    // Messagerie intégrée et feedback (§19)
    char chat_msg_buf[256] = "";
    struct ChatEntry {
        std::string author;
        std::string text;
        std::string time_str;
        bool is_host = false;
    };
    std::vector<ChatEntry> chat_history;
    std::vector<AppFeedbackItem> feedbacks;

    // Notification toast
    std::string toast_message;
    std::chrono::steady_clock::time_point toast_expiry;

    void show_toast(std::string msg) {
        toast_message = std::move(msg);
        toast_expiry = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    }
};

// --- Rendu des écrans (§6.2) ---

void render_dashboard(DevLinkAppState& state, const plpt::MonitorView& view) {
    using namespace devlink::ui;
    text_section_title("Tableau de Bord DevLink");
    text_muted("Aperçu en temps réel de votre poste de partage et de l'environnement local");
    ImGui::Spacing();

    // Ligne de métriques
    std::size_t active_participants = 0;
    if (state.host_server && state.host_running) {
        active_participants = state.host_server->snapshot().participants.size();
    }
    const auto& c = theme_colors();

    metric_card("##m1", "Services Détectés", view.services.size(), "Sur le système hôte", 210.0f);
    ImGui::SameLine();
    metric_card("##m2", "Participants Actifs", active_participants, "En session de test", 210.0f);
    ImGui::SameLine();
    metric_card("##m3", "État Réseau", state.host_running ? 1 : 0, state.host_running ? "Partage LAN actif" : "En attente", 210.0f);
    ImGui::SameLine();
    metric_card("##m4", "Moteur Navigateur", state.browser_backend ? 1 : 0, "Isolation de profils prête", 210.0f);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Deux colonnes : État de session rapide & Raccourcis utiles
    ImGui::Columns(2, "##dash_cols", false);
    ImGui::SetColumnWidth(0, 520.0f);

    if (card_begin("##dash_session_card")) {
        text_card_title("Statut du Partage");
        ImGui::Spacing();
        if (state.host_running) {
            status_pill("SESSION ACTIVE", c.status_success);
            ImGui::Spacing();
            ImGui::Text("Session : %s", state.session_name_buf);
            ImGui::Text("Point d'accès : http://%s:%u", state.selected_bind_address.c_str(), state.host_proxy_port);
            ImGui::Text("Code d'invitation : ");
            ImGui::SameLine();
            text_code(state.host_invite_code.text);
            ImGui::Spacing();
            if (button_danger("Arrêter la Session")) {
                if (state.host_server) state.host_server->stop();
                if (state.service_proxy) state.service_proxy->stop();
                if (state.announcer) state.announcer->stop();
                if (state.browser_backend) state.browser_backend->destroy_context(state.host_browser_ctx);
                state.host_running = false;
                state.show_toast("Session arrêtée et ressources nettoyées");
            }
        } else {
            status_pill("AUCUN PARTAGE EN COURS", c.text_disabled);
            ImGui::Spacing();
            text_muted("Vous pouvez configurer et lancer un partage local sécurisé pour vos collaborateurs.");
            ImGui::Spacing();
            if (button_primary("Configurer un Partage", ImVec2(220, 36))) {
                state.current_tab = MainNavigationTab::ShareSetup;
            }
        }
    }
    card_end();

    ImGui::NextColumn();

    if (card_begin("##dash_actions_card")) {
        text_card_title("Actions Rapides");
        ImGui::Spacing();
        if (button_secondary("Centre de Découverte Locale", ImVec2(280, 34))) {
            state.current_tab = MainNavigationTab::DiscoveryCenter;
        }
        ImGui::Spacing();
        if (button_secondary("Rejoindre en tant qu'Invité", ImVec2(280, 34))) {
            state.is_host_mode = false;
            state.current_tab = MainNavigationTab::GuestArea;
        }
        ImGui::Spacing();
        if (button_secondary("Consulter les Diagnostics", ImVec2(280, 34))) {
            state.current_tab = MainNavigationTab::Diagnostics;
        }
    }
    card_end();

    ImGui::Columns(1);
}

void render_discovery_center(DevLinkAppState& state, const plpt::MonitorView& view) {
    using namespace devlink::ui;
    text_section_title("Centre de Découverte Locale");
    text_muted("Surveillance passive des processus hôtes et sockets en écoute sans injection ni scan réseau invasif");
    ImGui::Spacing();

    static ImGuiTextFilter filter;
    filter.Draw("Filtrer par nom / port / PID", 320.0f);
    ImGui::SameLine();
    if (button_secondary("Actualiser l'inventaire")) {
        if (state.monitor) (void)state.monitor->refresh_now();
        state.show_toast("Inventaire système actualisé");
    }

    ImGui::Spacing();

    const auto& c = theme_colors();
    if (view.services.empty()) {
        empty_state("Aucun service détecté", "Aucun processus en écoute sur les tables TCP/UDP.");
        return;
    }

    if (ImGui::BeginTable("##discovery_table", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 480))) {
        ImGui::TableSetupColumn("Service", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Port", ImGuiTableColumnFlags_WidthFixed, 75.0f);
        ImGui::TableSetupColumn("Proto", ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 65.0f);
        ImGui::TableSetupColumn("Origine", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("Confiance", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("État", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const auto& svc : view.services) {
            for (const auto& ep : svc.endpoints) {
                const std::string filter_str = svc.process_name + " " + std::to_string(ep.local.port.value);
                if (!filter.PassFilter(filter_str.c_str())) continue;

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                text_card_title(svc.process_name.c_str());

                ImGui::TableNextColumn();
                text_code(std::to_string(ep.local.port.value));

                ImGui::TableNextColumn();
                ImGui::Text("%s", ep.protocol == plpt::TransportProtocol::Tcp ? "TCP" : "UDP");

                ImGui::TableNextColumn();
                ImGui::Text("%u", ep.owner_pid ? ep.owner_pid->value : 0);

                ImGui::TableNextColumn();
                text_muted(svc.origin.label.c_str());

                ImGui::TableNextColumn();
                ImGui::Text("%s", plpt::to_string(svc.origin.confidence));

                ImGui::TableNextColumn();
                status_pill(plpt::to_string(svc.share_state), c.accent_light);

                ImGui::TableNextColumn();
                std::string btn_id = "Choisir##" + std::to_string(ep.local.port.value) + "_" + std::to_string(ep.owner_pid ? ep.owner_pid->value : 0);
                if (button_secondary(btn_id.c_str(), ImVec2(140, 24))) {
                    state.host_shared_url = "http://127.0.0.1:" + std::to_string(ep.local.port.value);
                    std::snprintf(state.target_url_buf, sizeof(state.target_url_buf), "%s", state.host_shared_url.c_str());
                    state.current_tab = MainNavigationTab::ShareSetup;
                    state.show_toast("Point d'entrée sélectionné : " + state.host_shared_url);
                }
            }
        }
        ImGui::EndTable();
    }
}

void render_share_setup(DevLinkAppState& state, const plpt::MonitorView& view) {
    (void)view;
    using namespace devlink::ui;
    text_section_title("Configuration du Partage Applicatif");
    text_muted("Parcours d'autorisation explicite et de préparation de session (§6.2, §10)");
    ImGui::Spacing();

    if (card_begin("##setup_wizard_card", ImVec2(780, 520))) {
        ImGui::TextColored(theme_colors().accent_light, "1. Paramètres de Session");
        ImGui::InputText("Nom de la Session", state.session_name_buf, sizeof(state.session_name_buf));
        ImGui::InputText("Nom de l'Hôte", state.host_name_buf, sizeof(state.host_name_buf));
        ImGui::SliderInt("Capacité maximale d'invités", &state.max_participants, 1, 5);
        ImGui::Checkbox("Exiger l'approbation explicite de chaque invité", &state.require_approval);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored(theme_colors().accent_light, "2. Cible Applicative & Origines Autorisées (§14.2)");
        ImGui::InputText("URL / Port Principal", state.target_url_buf, sizeof(state.target_url_buf));
        ImGui::InputText("Allowlist d'origines (séparées par virgule)", state.allowed_origins_buf, sizeof(state.allowed_origins_buf));
        text_disabled_label("Seules ces origines seront accessibles par le navigateur distant.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored(theme_colors().accent_light, "3. Mode de Participation (§17)");
        ImGui::RadioButton("Mode A — Test Indépendant (Profils de navigation distincts)", &state.participation_mode, 0);
        ImGui::RadioButton("Mode B — Collaboration sur une Vue (Contrôle exclusif transférable)", &state.participation_mode, 1);
        ImGui::RadioButton("Mode C — Observation Seule (Rendu vidéo interactif désactivé)", &state.participation_mode, 2);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (!state.host_running) {
            if (button_primary("Démarrer le Partage Sécurisé", ImVec2(280, 40))) {
                // Démarre la session hôte
                auto code_res = plpt::generate_invite_code();
                if (code_res) state.host_invite_code = code_res.value();

                auto tok_res = plpt::generate_service_token();
                if (tok_res) state.host_service_token = tok_res.value();

                // Allocation dynamique de port avec bind(0) (§11)
                plpt::PortAllocator allocator;
                auto port_res = allocator.allocate_ephemeral_port(plpt::IpFamily::V4);
                state.host_control_port = port_res ? port_res.value().port.value : 42420;

                auto proxy_port_res = allocator.allocate_ephemeral_port(plpt::IpFamily::V4);
                state.host_proxy_port = proxy_port_res ? proxy_port_res.value().port.value : 42421;

                // Initialisation du Browser Worker
                if (!state.browser_backend) {
                    state.browser_backend = plpt::browser::create_browser_backend();
                }
                plpt::BrowserPolicy policy;
                policy.allowed_origins.push_back(state.target_url_buf);
                auto ctx_res = state.browser_backend->create_context(plpt::ParticipantId{1}, policy);
                if (ctx_res) {
                    state.host_browser_ctx = ctx_res.value().context;
                    (void)state.browser_backend->navigate(state.host_browser_ctx, state.target_url_buf);
                }

                // Démarrage du serveur hôte
                plpt::net::HostServerConfig srv_cfg;
                srv_cfg.bind_port = state.host_control_port;
                srv_cfg.invite_code = state.host_invite_code;
                srv_cfg.max_participants = static_cast<std::size_t>(state.max_participants);
                srv_cfg.require_approval = state.require_approval;
                srv_cfg.host_name = state.host_name_buf;
                state.host_server = std::make_unique<plpt::net::HostServer>(srv_cfg, state.clock);
                (void)state.host_server->start();

                // Annonceur LAN (§12)
                plpt::net::AnnouncerConfig ann_cfg;
                ann_cfg.payload.session_name = state.session_name_buf;
                ann_cfg.payload.host_name = state.host_name_buf;
                ann_cfg.payload.control_port = state.host_control_port;
                ann_cfg.payload.proxy_port = state.host_proxy_port;
                state.announcer = std::make_unique<plpt::net::Announcer>(ann_cfg);
                (void)state.announcer->start();

                state.host_running = true;
                state.current_tab = MainNavigationTab::ActiveSession;
                state.show_toast("Partage DevLink démarré avec succès !");
            }
        } else {
            status_pill("SESSION DÉJÀ ACTIVE", theme_colors().status_warning);
            ImGui::SameLine();
            if (button_secondary("Aller à la Session Active")) {
                state.current_tab = MainNavigationTab::ActiveSession;
            }
        }
    }
    card_end();
}

void render_active_session(DevLinkAppState& state, Runtime& runtime) {
    using namespace devlink::ui;
    text_section_title("Supervision de la Session Active");
    text_muted("Contrôle des participants, des autorisations et du rendu distant");
    ImGui::Spacing();

    if (!state.host_running) {
        empty_state("Aucune session active", "Démarrez une session depuis la Configuration du Partage.", "Aller à la Configuration", nullptr);
        if (ImGui::Button("Aller à la configuration##jump")) {
            state.current_tab = MainNavigationTab::ShareSetup;
        }
        return;
    }

    const auto& c = theme_colors();

    // Bandeau d'invitation avec code Crockford
    if (card_begin("##session_invite_banner", ImVec2(0, 90))) {
        ImGui::Text("Session : %s", state.session_name_buf);
        ImGui::SameLine(350);
        ImGui::Text("Code d'Invitation : ");
        ImGui::SameLine();
        ImGui::SetWindowFontScale(1.3f);
        text_code(state.host_invite_code.text);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::SameLine();
        if (button_secondary("Copier", ImVec2(70, 26))) {
            SDL_SetClipboardText(state.host_invite_code.text.c_str());
            state.show_toast("Code d'invitation copié dans le presse-papier");
        }
    }
    card_end();

    ImGui::Spacing();

    // Rendu en 2 colonnes : Flux vidéo distant / Participants & Contrôles
    ImGui::Columns(2, "##session_work_cols", false);
    ImGui::SetColumnWidth(0, 680.0f);

    text_card_title("Rendu Distant du Navigateur Hôte");
    if (state.browser_backend && state.host_browser_ctx.value != 0) {
        auto frame_res = state.browser_backend->capture_frame(state.host_browser_ctx);
        if (frame_res) {
            auto parsed = plpt::media::deserialize_video_frame(frame_res.value());
            if (parsed && !parsed.value().pixels.empty()) {
                runtime.update_texture_pixels(parsed.value().pixels.data(), parsed.value().header.width, parsed.value().header.height);
            }
        }
    }

    if (runtime.stream_texture) {
        ImGui::Image(reinterpret_cast<ImTextureID>(runtime.stream_texture), ImVec2(640, 360));
    } else {
        if (card_begin("##stream_placeholder", ImVec2(640, 360))) {
            text_muted("Initialisation du flux de rendu...");
        }
        card_end();
    }

    ImGui::NextColumn();

    text_card_title("Participants Connectés (§18)");
    if (card_begin("##session_participants_list", ImVec2(0, 360))) {
        if (state.host_server) {
            auto snap = state.host_server->snapshot();
            if (snap.participants.empty()) {
                text_muted("En attente de connexion de testeurs sur le réseau local...");
            } else {
                for (const auto& [id, part] : snap.participants) {
                    ImGui::Text("%s", part.name.c_str());
                    ImGui::SameLine(180);
                    status_pill(plpt::to_string(part.role), c.accent_light);
                    ImGui::SameLine(300);
                    std::string kick_id = "Révoquer##" + std::to_string(id.value);
                    if (button_danger(kick_id.c_str(), ImVec2(80, 22))) {
                        (void)state.host_server->revoke_participant(id, "Révoqué par l'hôte");
                        state.show_toast("Participant révoqué");
                    }
                }
            }
        }
    }
    card_end();

    ImGui::Columns(1);
}

void render_guest_area(DevLinkAppState& state, Runtime& runtime) {
    using namespace devlink::ui;
    text_section_title("Espace Invité — Test Applicatif Distant");
    text_muted("Connexion sécurisée à une session DevLink sans installer les outils ni le code (§8, §16)");
    ImGui::Spacing();

    const auto& c = theme_colors();

    if (!state.guest_connected) {
        if (card_begin("##guest_connect_card", ImVec2(600, 320))) {
            text_card_title("Rejoindre un Partage DevLink");
            ImGui::Spacing();
            ImGui::InputText("Votre Nom", state.guest_name_buf, sizeof(state.guest_name_buf));
            ImGui::InputText("Code d'Invitation", state.guest_invite_code_buf, sizeof(state.guest_invite_code_buf));
            ImGui::InputText("Adresse Hôte (LAN)", state.guest_host_addr_buf, sizeof(state.guest_host_addr_buf));
            ImGui::InputInt("Port de Contrôle", &state.guest_host_port);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (button_primary("Se Connecter à la Session", ImVec2(240, 38))) {
                plpt::net::GuestClientConfig cfg;
                cfg.host_address = plpt::IpAddress::parse(state.guest_host_addr_buf).value();
                cfg.host_port = static_cast<std::uint16_t>(state.guest_host_port > 0 ? state.guest_host_port : state.host_control_port);
                cfg.guest_name = state.guest_name_buf;
                auto decoded_code = plpt::decode_invite_code(state.guest_invite_code_buf);
                if (decoded_code) {
                    cfg.invite_code.bytes = decoded_code.value();
                    cfg.invite_code.text = state.guest_invite_code_buf;
                } else {
                    cfg.invite_code = state.host_invite_code;
                }

                state.guest_client = std::make_unique<plpt::net::GuestClient>(cfg, state.clock);
                auto res = state.guest_client->connect_and_authenticate();
                if (res) {
                    state.guest_connected = true;
                    state.guest_status_msg = "Connecté à la session DevLink";
                    state.show_toast("Connecté avec succès !");
                } else {
                    state.guest_status_msg = "Échec de connexion : " + res.error().message;
                    state.show_toast("Erreur de connexion");
                }
            }

            ImGui::SameLine();
            text_muted(state.guest_status_msg.c_str());
        }
        card_end();
    } else {
        // Espace interactif invité connecté : Rendu du stream distant + interactions
        ImGui::Text("Session connectée. Mode : %s", state.participation_mode == 0 ? "Test Indépendant" : (state.participation_mode == 1 ? "Collaboration" : "Observation Seule"));
        ImGui::SameLine(ImGui::GetWindowWidth() - 200);
        if (button_danger("Quitter la Session")) {
            if (state.guest_client) state.guest_client->disconnect();
            state.guest_connected = false;
            state.show_toast("Déconnecté de la session");
        }

        ImGui::Spacing();

        // Récupération de la trame via le backend
        if (state.browser_backend && state.host_browser_ctx.value != 0) {
            auto frame_res = state.browser_backend->capture_frame(state.host_browser_ctx);
            if (frame_res) {
                auto parsed = plpt::media::deserialize_video_frame(frame_res.value());
                if (parsed && !parsed.value().pixels.empty()) {
                    runtime.update_texture_pixels(parsed.value().pixels.data(), parsed.value().header.width, parsed.value().header.height);
                }
            }
        }

        // Fenêtre d'affichage du stream interactif
        if (runtime.stream_texture) {
            const ImVec2 img_pos = ImGui::GetCursorScreenPos();
            const float disp_w = 960.0f;
            const float disp_h = 540.0f;
            ImGui::Image(reinterpret_cast<ImTextureID>(runtime.stream_texture), ImVec2(disp_w, disp_h));

            // Capture et réinjection des interactions (§16)
            if (ImGui::IsItemHovered()) {
                const ImVec2 mouse_pos = ImGui::GetMousePos();
                const float norm_x = std::clamp((mouse_pos.x - img_pos.x) / disp_w, 0.0f, 1.0f);
                const float norm_y = std::clamp((mouse_pos.y - img_pos.y) / disp_h, 0.0f, 1.0f);

                // Clic souris
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    if (state.participation_mode != 2) { // Pas d'entrées en Mode C (Observation)
                        plpt::media::InputEvent ev;
                        ev.type = plpt::media::InputEventType::MouseDown;
                        ev.x_norm = norm_x;
                        ev.y_norm = norm_y;
                        ev.button = 1;
                        auto bytes = plpt::media::serialize_input_event(ev);
                        if (state.browser_backend && state.host_browser_ctx.value != 0) {
                            (void)state.browser_backend->inject_input(state.host_browser_ctx, bytes);
                        }
                    }
                }
            }
        } else {
            if (card_begin("##guest_stream_box", ImVec2(960, 540))) {
                text_muted("Réception du flux applicatif distant...");
            }
            card_end();
        }
    }
}

void render_chat(DevLinkAppState& state, bool is_host) {
    using namespace devlink::ui;
    text_section_title("Messagerie de Session");
    text_muted("Discussion temps réel isolée à la session active (§19)");
    ImGui::Spacing();

    if (card_begin("##chat_feed_card", ImVec2(700, 380))) {
        if (state.chat_history.empty()) {
            text_muted("Aucun message dans cette session pour l'instant.");
        } else {
            for (const auto& item : state.chat_history) {
                ImGui::TextColored(item.is_host ? theme_colors().accent_light : theme_colors().text_primary, "[%s] %s :", item.time_str.c_str(), item.author.c_str());
                ImGui::SameLine();
                ImGui::TextWrapped("%s", item.text.c_str());
            }
        }
    }
    card_end();

    ImGui::Spacing();
    ImGui::PushItemWidth(560);
    bool enter = ImGui::InputText("##chat_in", state.chat_msg_buf, sizeof(state.chat_msg_buf), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (button_primary("Envoyer", ImVec2(120, 32)) || enter) {
        if (state.chat_msg_buf[0] != '\0') {
            DevLinkAppState::ChatEntry entry;
            entry.author = is_host ? state.host_name_buf : state.guest_name_buf;
            entry.text = state.chat_msg_buf;
            entry.time_str = "12:00";
            entry.is_host = is_host;
            state.chat_history.push_back(std::move(entry));
            state.chat_msg_buf[0] = '\0';
        }
    }
}

void render_feedback(DevLinkAppState& state, bool is_host) {
    using namespace devlink::ui;
    text_section_title("Signalements & Anomalies");
    text_muted("Remontée contextuelle de bogues reliée à la session de test");
    ImGui::Spacing();

    if (card_begin("##feedback_card", ImVec2(700, 350))) {
        if (state.feedbacks.empty()) {
            text_muted("Aucun signalement d'anomalie enregistré.");
        } else {
            for (const auto& fb : state.feedbacks) {
                ImGui::TextColored(theme_colors().status_warning, "[%s] Par %s sur %s", fb.severity.c_str(), fb.sender.c_str(), fb.route.c_str());
                ImGui::TextWrapped("%s", fb.description.c_str());
                ImGui::Separator();
            }
        }
    }
    card_end();
}

void render_diagnostics(DevLinkAppState& state, const plpt::MonitorView& view) {
    using namespace devlink::ui;
    text_section_title("Diagnostics & État Système");
    text_muted("Informations techniques détaillées, cryptographie et interfaces réseau");
    ImGui::Spacing();

    if (card_begin("##diag_info_card", ImVec2(780, 420))) {
        ImGui::Text("Plateforme : Windows x64 (UCRT64 Clang++ C++20)");
        ImGui::Text("Version : DevLink 0.2.0-preview");
        ImGui::Text("Services inventoriés : %zu", view.services.size());
        ImGui::Text("Moteur Browser Worker : %s", plpt::browser::WebView2Backend::is_available() ? "Microsoft WebView2 COM (Actif)" : "Managed Browser Engine (Fallback)");
        ImGui::Text("Canal Sécurisé : Chiffrement symétrique & HMAC-SHA256 (BCrypt)");
        ImGui::Text("Allocation de Ports : Dynamique via bind(0)");
        ImGui::Separator();
        ImGui::Text("Avertissements moniteur : %zu", view.warnings.size());
        for (const auto& w : view.warnings) {
            text_disabled_label(w.c_str());
        }
    }
    card_end();
}

} // namespace

int main(int argc, char** argv) {
    bool smoke = false;
    const char* screenshot_path = nullptr;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--smoke-test") {
            smoke = true;
        } else if (arg == "--screenshot") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Usage: DevLink [--smoke-test [--screenshot file.bmp]]\n");
                return 2;
            }
            screenshot_path = argv[++i];
        } else {
            std::fprintf(stderr, "Usage: DevLink [--smoke-test [--screenshot file.bmp]]\n");
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

    runtime.window = SDL_CreateWindow(
        kAppAsciiTitle.data(), 1440, 920, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
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

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    runtime.imgui = true;

    // Initialisation du ThemeManager DevLink (§5.1)
    devlink::ui::ThemeManager::instance().set_theme(devlink::ui::ThemeMode::DevDark);
    devlink::ui::ThemeManager::instance().apply_to_imgui(1.0f);

    if (!ImGui_ImplSDL3_InitForSDLRenderer(runtime.window, runtime.renderer)) {
        std::fprintf(stderr, "ImGui_ImplSDL3_InitForSDLRenderer failed\n");
        return 1;
    }
    runtime.platform = true;

    if (!ImGui_ImplSDLRenderer3_Init(runtime.renderer)) {
        std::fprintf(stderr, "ImGui_ImplSDLRenderer3_Init failed\n");
        return 1;
    }
    runtime.render_backend = true;

    // État de l'application DevLink
    DevLinkAppState app;
    app.monitor = std::make_unique<plpt::Monitor>(
        std::make_unique<plpt::NativeProcessEnumerator>(),
        std::make_unique<plpt::NativeSocketTableProvider>());
    app.browser_backend = plpt::browser::create_browser_backend();

    bool running = true;
    int smoke_frames = 0;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                event.window.windowID == SDL_GetWindowID(runtime.window)) {
                running = false;
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        const auto view = app.monitor ? app.monitor->view() : plpt::MonitorView{};

        // Fenêtre principale plein écran sans bordure superflue
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        int w = 0, h = 0;
        SDL_GetWindowSize(runtime.window, &w, &h);
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(w), static_cast<float>(h)));

        ImGuiWindowFlags root_flags = ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##DevLinkRoot", nullptr, root_flags);
        ImGui::PopStyleVar(2);

        // Barre supérieure (§6.1)
        devlink::ui::ThemeMode theme_toggle = app.theme_mode;
        bool mode_toggle = app.is_host_mode;
        std::string status_banner = app.host_running ? "SESSION HÔTE ACTIVE" : (app.guest_connected ? "CONNECTÉ EN INVITÉ" : "PRÊT");
        devlink::ui::render_top_bar(status_banner, app.is_host_mode, app.theme_mode, theme_toggle, mode_toggle);

        if (theme_toggle != app.theme_mode) {
            app.theme_mode = theme_toggle;
            devlink::ui::ThemeManager::instance().set_theme(app.theme_mode);
            devlink::ui::ThemeManager::instance().apply_to_imgui(1.0f);
        }
        if (mode_toggle != app.is_host_mode) {
            app.is_host_mode = mode_toggle;
            app.current_tab = app.is_host_mode ? devlink::ui::MainNavigationTab::Dashboard : devlink::ui::MainNavigationTab::GuestArea;
        }

        ImGui::Spacing();

        // Barre d'onglets de navigation
        if (ImGui::BeginTabBar("##main_navigation_bar", ImGuiTabBarFlags_None)) {
            if (ImGui::BeginTabItem(devlink::ui::icons::kNavDashboard.data())) {
                app.current_tab = devlink::ui::MainNavigationTab::Dashboard;
                ImGui::EndTabItem();
            }
            if (app.is_host_mode) {
                if (ImGui::BeginTabItem(devlink::ui::icons::kNavDiscovery.data())) {
                    app.current_tab = devlink::ui::MainNavigationTab::DiscoveryCenter;
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(devlink::ui::icons::kNavSetup.data())) {
                    app.current_tab = devlink::ui::MainNavigationTab::ShareSetup;
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(devlink::ui::icons::kNavSession.data())) {
                    app.current_tab = devlink::ui::MainNavigationTab::ActiveSession;
                    ImGui::EndTabItem();
                }
            } else {
                if (ImGui::BeginTabItem(devlink::ui::icons::kNavGuestJoin.data())) {
                    app.current_tab = devlink::ui::MainNavigationTab::GuestArea;
                    ImGui::EndTabItem();
                }
            }
            if (ImGui::BeginTabItem(devlink::ui::icons::kNavChat.data())) {
                app.current_tab = devlink::ui::MainNavigationTab::Messaging;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(devlink::ui::icons::kNavFeedback.data())) {
                app.current_tab = devlink::ui::MainNavigationTab::Feedback;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(devlink::ui::icons::kNavDiagnostics.data())) {
                app.current_tab = devlink::ui::MainNavigationTab::Diagnostics;
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Spacing();

        // Affichage de l'écran actif
        switch (app.current_tab) {
        case devlink::ui::MainNavigationTab::Dashboard:
            render_dashboard(app, view);
            break;
        case devlink::ui::MainNavigationTab::DiscoveryCenter:
            render_discovery_center(app, view);
            break;
        case devlink::ui::MainNavigationTab::ShareSetup:
            render_share_setup(app, view);
            break;
        case devlink::ui::MainNavigationTab::ActiveSession:
            render_active_session(app, runtime);
            break;
        case devlink::ui::MainNavigationTab::GuestArea:
            render_guest_area(app, runtime);
            break;
        case devlink::ui::MainNavigationTab::Messaging:
            render_chat(app, app.is_host_mode);
            break;
        case devlink::ui::MainNavigationTab::Feedback:
            render_feedback(app, app.is_host_mode);
            break;
        case devlink::ui::MainNavigationTab::Diagnostics:
            render_diagnostics(app, view);
            break;
        }

        // Notification Toast en bas à droite
        if (!app.toast_message.empty() && std::chrono::steady_clock::now() < app.toast_expiry) {
            ImGui::SetNextWindowPos(ImVec2(static_cast<float>(w) - 360.0f, static_cast<float>(h) - 70.0f));
            ImGui::BeginChild("##toast_win", ImVec2(340, 50), true, ImGuiWindowFlags_NoScrollbar);
            ImGui::TextColored(devlink::ui::theme_colors().accent_light, "i  %s", app.toast_message.c_str());
            ImGui::EndChild();
        }

        ImGui::End();

        // Rendu final ImGui et SDL
        ImGui::Render();
        SDL_SetRenderDrawColor(runtime.renderer, 23, 19, 18, 255);
        SDL_RenderClear(runtime.renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), runtime.renderer);
        SDL_RenderPresent(runtime.renderer);

        if (smoke) {
            ++smoke_frames;
            if (smoke_frames >= 8) {
                if (screenshot_path) {
                    SDL_Surface* ss = SDL_RenderReadPixels(runtime.renderer, nullptr);
                    if (!ss) {
                        std::fprintf(stderr, "SDL_RenderReadPixels: %s\n", SDL_GetError());
                        return 1;
                    }
                    if (!SDL_SaveBMP(ss, screenshot_path)) {
                        std::fprintf(stderr, "SDL_SaveBMP: %s\n", SDL_GetError());
                        SDL_DestroySurface(ss);
                        return 1;
                    }
                    SDL_DestroySurface(ss);
                }
                std::printf("desktop-smoke: %d frames, snapshot=%zu, services=%zu, sharing=%s\n",
                    smoke_frames, view.snapshot_sequence, view.services.size(), app.host_running ? "enabled" : "disabled");
                running = false;
            }
        }
    }

    return 0;
}
