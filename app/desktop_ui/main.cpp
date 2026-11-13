#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string_view>
#include "engine/discovery/monitor.hpp"
#include "engine/platform/native.hpp"

namespace {
// SDL 3.4.20's X11 title conversion leaks XTextProperty.value when XTextStyle
// cannot represent a character (conv > 0). Keep the OS title ASCII in every
// locale; French/Unicode content is still rendered inside the window by ImGui.
// No vendor patch or sanitizer suppression; re-evaluate on the next SDL update.
constexpr std::string_view window_title = "PLPT - Moniteur local / preview 0.1.0";
static_assert(std::all_of(window_title.begin(), window_title.end(), [](unsigned char c) { return c < 0x80; }));
const ImVec4 mint{0.39F, 0.87F, 0.71F, 1};
const ImVec4 muted{0.55F, 0.61F, 0.69F, 1};
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
void theme(float scale) {
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowPadding = {24, 22}; style.FramePadding = {12, 8}; style.ItemSpacing = {12, 12};
    style.WindowRounding = 0; style.ChildRounding = 10; style.FrameRounding = 6;
    style.GrabRounding = 5; style.WindowBorderSize = 0; style.ChildBorderSize = 1;
    style.FontSizeBase = 17;
    auto* c = style.Colors;
    c[ImGuiCol_WindowBg] = {0.055F, 0.067F, 0.085F, 1};
    c[ImGuiCol_ChildBg] = {0.075F, 0.09F, 0.112F, 1};
    c[ImGuiCol_Border] = {0.15F, 0.18F, 0.22F, 1};
    c[ImGuiCol_Text] = {0.89F, 0.92F, 0.95F, 1};
    c[ImGuiCol_TextDisabled] = muted;
    c[ImGuiCol_FrameBg] = {0.105F, 0.13F, 0.16F, 1};
    c[ImGuiCol_Button] = {0.10F, 0.24F, 0.21F, 1};
    c[ImGuiCol_ButtonHovered] = {0.14F, 0.34F, 0.29F, 1};
    c[ImGuiCol_ButtonActive] = {0.19F, 0.44F, 0.36F, 1};
    c[ImGuiCol_Header] = {0.12F, 0.23F, 0.21F, 1};
    c[ImGuiCol_HeaderHovered] = {0.14F, 0.29F, 0.25F, 1};
    c[ImGuiCol_TableHeaderBg] = {0.10F, 0.12F, 0.15F, 1};
    c[ImGuiCol_TableRowBgAlt] = {0.11F, 0.14F, 0.17F, 0.35F};
    style.ScaleAllSizes(scale); style.FontScaleDpi = scale;
}
void label(const char* text) { ImGui::TextColored(muted, "%s", text); }
void stat(const char* title, std::size_t number, const char* caption) {
    label(title); ImGui::TextColored(mint, "%zu", number); ImGui::TextWrapped("%s", caption);
}
void guarantees() {
    ImGui::SeparatorText("Ce que cette preview fait réellement");
    ImGui::BulletText("Observe les tables locales de sockets et de processus.");
    ImGui::BulletText("Présente une origine probable, jamais une identité garantie.");
    ImGui::BulletText("Conserve la sélection en mémoire, sans activer de partage.");
    ImGui::BulletText("N'ouvre aucun port de transport et ne sonde pas le LAN.");
    ImGui::Spacing();
    ImGui::SeparatorText("Portes de validation encore ouvertes");
    ImGui::TextWrapped("Transport QUIC authentifié, invitations, isolation WebView2, capture vidéo, "
        "nettoyage des profils et scénario Wi-Fi à cinq invités : non intégrés. "
        "Une sélection de port ne vaut jamais autorisation réseau.");
    ImGui::Spacing();
    ImGui::TextColored(mint, "Windows x64 en priorité / C++20 / clang++");
    ImGui::TextWrapped("Le moniteur fonctionne sans élévation permanente. Les informations auxquelles "
        "le système refuse l'accès restent inconnues. Aucun historique de terminal, variable d'environnement "
        "ou fichier du projet n'est lu.");
}
void dashboard(const plpt::MonitorView& view) {
    std::size_t tcp = 0, udp = 0, known = 0;
    for (const auto& service : view.services) for (const auto& ep : service.endpoints) {
        if (service.share_state == plpt::ShareState::Gone || service.share_state == plpt::ShareState::Unavailable) continue;
        if (ep.protocol == plpt::TransportProtocol::Tcp) ++tcp; else ++udp;
        if (ep.owner_pid) ++known;
    }
    if (ImGui::BeginTable("metrics", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn(); stat("LISTENERS TCP", tcp, "Points d'entrée possibles");
        ImGui::TableNextColumn(); stat("ENDPOINTS UDP", udp, "Observés, non partagés");
        ImGui::TableNextColumn(); stat("PID CORRÉLÉS", known, "Identités de création stables");
        ImGui::EndTable();
    }
    ImGui::Spacing(); guarantees();
}
const char* socket_state(plpt::SocketState s) {
    if (s == plpt::SocketState::Listening) return "LISTEN";
    if (s == plpt::SocketState::Bound) return "BOUND";
    return "inconnu";
}
void inventory(plpt::Monitor& monitor, const plpt::MonitorView& view, plpt::ServiceId& selected,
    plpt::Endpoint& selected_endpoint, std::string& notice) {
    static ImGuiTextFilter filter;
    filter.Draw("Filtrer", 260);
    ImGui::SameLine();
    if (ImGui::Button("Actualiser")) notice = monitor.request_refresh() ? "Actualisation demandée." : "File occupée, réessayez.";
    label("Choisissez une ligne pour examiner son origine. Aucun service n'est partagé.");
    const auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX;
    if (ImGui::BeginTable("endpoints", 8, flags, {0, 300})) {
        ImGui::TableSetupColumn("PROTO / PORT", ImGuiTableColumnFlags_WidthFixed, 115);
        ImGui::TableSetupColumn("ADRESSE", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("PROCESSUS / PID", ImGuiTableColumnFlags_WidthFixed, 160);
        ImGui::TableSetupColumn("ORIGINE PROBABLE", ImGuiTableColumnFlags_WidthFixed, 205);
        ImGui::TableSetupColumn("CONFIANCE", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("SOCKET", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("PARTAGE", ImGuiTableColumnFlags_WidthFixed, 180);
        ImGui::TableSetupColumn("PROJET PROBABLE", ImGuiTableColumnFlags_WidthFixed, 160);
        ImGui::TableSetupScrollFreeze(1, 1); ImGui::TableHeadersRow();
        for (const auto& service : view.services) {
            const auto service_key = std::to_string(service.service_id.value);
            ImGui::PushID(service_key.c_str());
            int index = 0;
            for (const auto& ep : service.endpoints) {
                const auto process = service.process ? service.process->image_name : std::string("inconnu");
                const auto searchable = ep.local.address + " " + std::to_string(ep.local.port.value) + " " + process;
                if (!filter.PassFilter(searchable.c_str())) continue;
                ImGui::PushID(index++);
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                const auto name = std::string(ep.protocol == plpt::TransportProtocol::Tcp ? "TCP  " : "UDP  ") + std::to_string(ep.local.port.value);
                if (ImGui::Selectable(name.c_str(), selected == service.service_id && selected_endpoint == ep.local,
                    ImGuiSelectableFlags_SpanAllColumns)) { selected = service.service_id; selected_endpoint = ep.local; }
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ep.local.address.c_str());
                if (ep.local.scope_id != 0) ImGui::TextDisabled("scope %u", ep.local.scope_id);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(process.c_str());
                if (ep.owner_pid) ImGui::TextDisabled("PID %u", ep.owner_pid->value); else ImGui::TextDisabled("PID inconnu");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(service.origin.label.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(plpt::to_string(service.origin.confidence).data());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(socket_state(ep.state));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(plpt::to_string(service.share_state).data());
                ImGui::TableNextColumn(); ImGui::TextDisabled("Non attribué");
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Inspection du service");
    const auto found = std::find_if(view.services.begin(), view.services.end(), [&](const auto& s) { return s.service_id == selected; });
    if (found == view.services.end()) {
        ImGui::TextWrapped("Sélectionnez une ligne. Les processus, les ports et les projets sont des notions distinctes.");
    } else {
        const auto& service = *found;
        ImGui::Text("Service #%llu  /  %zu endpoint(s)", static_cast<unsigned long long>(selected.value), service.endpoints.size());
        for (const auto& clue : service.origin.evidence) ImGui::BulletText("%s", clue.c_str());
        ImGui::BeginDisabled(service.share_state != plpt::ShareState::Detected && service.share_state != plpt::ShareState::Selected);
        if (ImGui::Button("Sélection locale du point d'entrée"))
            notice = monitor.request_selection(selected, selected_endpoint) ? "Commande envoyée. Le partage reste désactivé." : "File occupée.";
        ImGui::SameLine();
        if (ImGui::Button("Marquer protégé")) notice = monitor.request_protection(selected) ? "Protection locale demandée." : "File occupée.";
        ImGui::EndDisabled();
        ImGui::TextWrapped("Les changements d'identité ou la disparition d'un endpoint invalident la sélection. "
            "Une nouvelle observation peut laisser le propriétaire inconnu.");
    }
}
}
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
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    runtime.sdl = true;
    if (SDL_GetVersion() != SDL_VERSION) {
        std::fprintf(stderr, "SDL runtime/header version mismatch: runtime=%d headers=%d\n", SDL_GetVersion(), SDL_VERSION);
        return 1;
    }
    runtime.window = SDL_CreateWindow(window_title.data(), 1400, 900, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!runtime.window) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    runtime.renderer = SDL_CreateRenderer(runtime.window, nullptr);
    if (!runtime.renderer) { std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }
    if (smoke) std::printf("desktop-runtime: SDL=%d video=%s renderer=%s\n", SDL_GetVersion(),
        SDL_GetCurrentVideoDriver(), SDL_GetRendererName(runtime.renderer));
    (void)SDL_SetRenderVSync(runtime.renderer, 1);
    IMGUI_CHECKVERSION(); ImGui::CreateContext(); runtime.imgui = true;
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    float scale = std::max(1.0F, SDL_GetWindowDisplayScale(runtime.window)); theme(scale);
    runtime.platform = ImGui_ImplSDL3_InitForSDLRenderer(runtime.window, runtime.renderer);
    if (!runtime.platform) { std::fprintf(stderr, "ImGui SDL3 initialization failed\n"); return 1; }
    runtime.render_backend = ImGui_ImplSDLRenderer3_Init(runtime.renderer);
    if (!runtime.render_backend) { std::fprintf(stderr, "ImGui renderer initialization failed\n"); return 1; }
    plpt::Monitor monitor(std::make_unique<plpt::NativeProcessEnumerator>(), std::make_unique<plpt::NativeSocketTableProvider>());
    int page = 1; unsigned frames = 0;
    plpt::ServiceId selected{}; plpt::Endpoint selected_endpoint{};
    std::string notice;
    bool quit = false;
    while (!quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(runtime.window))) quit = true;
            if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
                scale = std::max(1.0F, SDL_GetWindowDisplayScale(runtime.window));
                ImGui::GetStyle() = ImGuiStyle{}; theme(scale);
            }
        }
        if (!smoke && (SDL_GetWindowFlags(runtime.window) & SDL_WINDOW_MINIMIZED)) { SDL_Delay(50); continue; }
        ImGui_ImplSDLRenderer3_NewFrame(); ImGui_ImplSDL3_NewFrame(); ImGui::NewFrame();
        const auto view = monitor.view();
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("PLPT", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextColored(mint, "PLPT  /  LOCAL TEST LAB"); ImGui::SameLine(); ImGui::TextDisabled("    0.1.0  ·  PREVIEW TECHNIQUE");
        ImGui::Separator();
        ImGui::BeginChild("navigation", {208 * scale, 0}, ImGuiChildFlags_Borders);
        label("ESPACE HÔTE"); ImGui::Spacing();
        const char* pages[] = {"Vue d'ensemble", "Détection locale", "Sécurité & limites"};
        for (int i = 0; i < 3; ++i) if (ImGui::Selectable(pages[i], page == i, 0, {0, 36 * scale})) page = i;
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        ImGui::TextColored(mint, "Aucun partage actif");
        ImGui::TextWrapped("L'observation n'ouvre aucun accès au PC.");
        ImGui::Spacing(); label("PROCHAINES ÉTAPES");
        ImGui::TextWrapped("Navigateur distant et transport non intégrés.");
        ImGui::EndChild(); ImGui::SameLine();
        ImGui::BeginChild("content", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
        label("MONITEUR LOCAL  /  SOURCES NATIVES DU SYSTÈME");
        ImGui::Spacing(); ImGui::TextUnformatted(pages[page]);
        ImGui::TextDisabled("Observation #%llu  ·  Actualisation en arrière-plan : 2 s", static_cast<unsigned long long>(view->revision));
        ImGui::Spacing();
        if (view->revision == 0) ImGui::TextUnformatted("Lecture des tables locales en cours…");
        else if (!view->complete) ImGui::TextColored({1, 0.72F, 0.36F, 1}, "Observation incomplète — ne pas conclure à une absence de service.");
        if (page == 0) dashboard(*view);
        else if (page == 1) inventory(monitor, *view, selected, selected_endpoint, notice);
        else guarantees();
        if (!notice.empty()) ImGui::TextWrapped("%s", notice.c_str());
        for (const auto& warning : view->warnings) ImGui::TextWrapped("%s : %s (%lld)", warning.context.c_str(), warning.message.c_str(), static_cast<long long>(warning.native_code));
        ImGui::EndChild(); ImGui::End();
        ImGui::Render();
        if (!SDL_SetRenderScale(runtime.renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y) ||
            !SDL_SetRenderDrawColor(runtime.renderer, 14, 17, 22, 255) || !SDL_RenderClear(runtime.renderer)) {
            std::fprintf(stderr, "SDL render preparation: %s\n", SDL_GetError()); return 1;
        }
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), runtime.renderer);
        ++frames;
        if (smoke && frames >= 8 && view->revision > 0) {
            if (!screenshot.empty()) {
                std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
                    SDL_RenderReadPixels(runtime.renderer, nullptr), SDL_DestroySurface);
                if (!surface) { std::fprintf(stderr, "SDL_RenderReadPixels: %s\n", SDL_GetError()); return 1; }
                // SDL 3.4.20 : SDL_SaveBMP fuit sa conversion si l'ouverture échoue.
                // Ouvrir nous-mêmes, garder closeio=false et vérifier la fermeture.
                std::unique_ptr<SDL_IOStream, decltype(&SDL_CloseIO)> stream(
                    SDL_IOFromFile(screenshot.c_str(), "wb"), SDL_CloseIO);
                if (!stream) { std::fprintf(stderr, "SDL_IOFromFile: %s\n", SDL_GetError()); return 1; }
                const bool saved = SDL_SaveBMP_IO(surface.get(), stream.get(), false);
                if (!saved) std::fprintf(stderr, "SDL_SaveBMP_IO: %s\n", SDL_GetError());
                const bool closed = SDL_CloseIO(stream.release());
                if (!closed) std::fprintf(stderr, "SDL_CloseIO: %s\n", SDL_GetError());
                if (!saved || !closed) return 1;
            }
            std::printf("desktop-smoke: %u frames, snapshot=%llu, services=%zu, sharing=disabled\n", frames,
                static_cast<unsigned long long>(view->revision), view->services.size());
            quit = true;
        }
        if (!SDL_RenderPresent(runtime.renderer)) { std::fprintf(stderr, "SDL_RenderPresent: %s\n", SDL_GetError()); return 1; }
        if (smoke && frames > 600) { std::fprintf(stderr, "desktop-smoke: observation timeout\n"); return 1; }
        SDL_Delay(16);
    }
    return 0;
}
