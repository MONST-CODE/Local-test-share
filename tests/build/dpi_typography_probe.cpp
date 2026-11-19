// Reproducible software-rendered typography probe for DPI-scale evidence.
// The scale is an explicit test input; this is not a physical monitor-DPI test.
#include <SDL3/SDL.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>

#include "app/desktop_ui/design_system/themes.hpp"
#include "app/desktop_ui/design_system/typography.hpp"

namespace {

constexpr int kWidth = 960;
constexpr int kHeight = 480;
constexpr std::array<float, 4> kScales{1.0F, 1.25F, 1.5F, 2.0F};
constexpr std::array<const char*, 4> kScaleLabels{"100", "125", "150", "200"};

void shutdown(SDL_Window* window, SDL_Renderer* renderer, bool renderer_backend,
              bool platform_backend, bool imgui_context, bool sdl_initialized) {
    if (renderer_backend) ImGui_ImplSDLRenderer3_Shutdown();
    if (platform_backend) ImGui_ImplSDL3_Shutdown();
    if (imgui_context) ImGui::DestroyContext();
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    if (sdl_initialized) SDL_Quit();
}

bool capture_frame(SDL_Renderer* renderer, const std::filesystem::path& output,
                   std::uint64_t& foreground_pixels, std::size_t& color_variants,
                   int& vertices, int& commands) {
    ImGui::Render();
    if (!SDL_SetRenderDrawColor(renderer, 23, 19, 18, 255) || !SDL_RenderClear(renderer)) return false;
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    if (!SDL_RenderPresent(renderer)) return false;
    SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr);
    if (!surface) return false;
    vertices = ImGui::GetDrawData()->TotalVtxCount;
    commands = ImGui::GetDrawData()->CmdListsCount;
    if (!SDL_LockSurface(surface)) {
        SDL_DestroySurface(surface);
        return false;
    }
    std::set<std::uint32_t> colors;
    foreground_pixels = 0;
    const int bytes_per_pixel = SDL_BYTESPERPIXEL(surface->format);
    const SDL_PixelFormatDetails* pixel_details = SDL_GetPixelFormatDetails(surface->format);
    if (!pixel_details || bytes_per_pixel < 3 || bytes_per_pixel > 4) {
        SDL_UnlockSurface(surface);
        SDL_DestroySurface(surface);
        return false;
    }
    for (int y = 0; y < surface->h; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(surface->pixels) +
            static_cast<std::ptrdiff_t>(y) * surface->pitch;
        for (int x = 0; x < surface->w; ++x) {
            std::uint32_t pixel = 0;
            std::memcpy(&pixel, row + static_cast<std::ptrdiff_t>(x) * bytes_per_pixel,
                        static_cast<std::size_t>(bytes_per_pixel));
            std::uint8_t r = 0, g = 0, b = 0, a = 0;
            SDL_GetRGBA(pixel, pixel_details, nullptr, &r, &g, &b, &a);
            if (r != 23 || g != 19 || b != 18) {
                ++foreground_pixels;
                colors.insert((static_cast<std::uint32_t>(r) << 24U) |
                              (static_cast<std::uint32_t>(g) << 16U) |
                              (static_cast<std::uint32_t>(b) << 8U) | a);
            }
        }
    }
    SDL_UnlockSurface(surface);
    color_variants = colors.size();
    const bool saved = SDL_SaveBMP(surface, output.string().c_str());
    SDL_DestroySurface(surface);
    return saved && vertices > 0 && commands > 0 && foreground_pixels > 1000 && color_variants > 8;
}

bool run_scale(SDL_Window* window, SDL_Renderer* renderer, const std::filesystem::path& output,
               float scale, const char* label) {
    devlink::ui::FontSet fonts;
    const devlink::ui::TypographyConfig config;
    const auto status = devlink::ui::load_fonts(scale, config, fonts);
    if (!fonts.valid() || status.source.empty() || (!status.system && !status.embedded)) {
        std::fprintf(stderr, "DPI scale=%.2f no real system/embedded font loaded: %s\n", scale, status.note.c_str());
        return false;
    }
    devlink::ui::ThemeManager::instance().apply_to_imgui(scale);
    if (ImGui::GetIO().FontGlobalScale != 1.0F) return false;

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight)), ImGuiCond_Always);
    bool lines_fit = true;
    if (ImGui::Begin("DevLink DPI typography qualification", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        const auto fits = [&] { return ImGui::GetItemRectMax().x <= static_cast<float>(kWidth - 8); };
        devlink::ui::text_hero_title("DevLink — rendu typographique réel");
        lines_fit = lines_fit && fits();
        devlink::ui::text_section_title("100 / 125 / 150 / 200 % : facteur injecté");
        lines_fit = lines_fit && fits();
        devlink::ui::text_card_title("Navigation et collaboration — Éléonore, déjà prêt");
        lines_fit = lines_fit && fits();
        devlink::ui::text_body("Texte de contrôle : café, français, œ, « guillemets », tiret — et 42 €.");
        lines_fit = lines_fit && fits();
        devlink::ui::text_muted("Une ligne doit rester dans la fenêtre sans agrandissement global.");
        lines_fit = lines_fit && fits();
        devlink::ui::text_code("http://127.0.0.1:3000  |  session 0042  |  TLS 1.3");
        lines_fit = lines_fit && fits();
        ImGui::Dummy(ImVec2(1, 8));
        ImGui::Text("Font scale %.2f (%s%%), SDL display scale %.2f", scale, label,
                    SDL_GetWindowDisplayScale(window));
    }
    ImGui::End();

    std::uint64_t foreground = 0;
    std::size_t variants = 0;
    int vertices = 0, commands = 0;
    if (!capture_frame(renderer, output, foreground, variants, vertices, commands)) return false;
    const auto rounded = [scale](float value) {
        return std::max(9, static_cast<int>(std::floor(value * scale + 0.5F)));
    };
    const int hero = rounded(config.hero_title), section = rounded(config.section_title);
    const int card = rounded(config.card_title), body = rounded(config.body);
    const int small = rounded(config.small), code = rounded(config.code);
    const bool hierarchy = hero > section && section > card && card > body && body > small;
    const bool geometry = foreground > 1000 && variants > 8 && vertices > 0 && commands > 0;
    const bool pass = hierarchy && geometry && lines_fit && ImGui::GetIO().FontGlobalScale == 1.0F;
    std::printf("DPI scale=%.2f (%s%%) SDL_window_scale=%.2f family=%s source=%s\n",
        scale, label, SDL_GetWindowDisplayScale(window), status.family.c_str(), status.source.c_str());
    std::printf("sizes=%d/%d/%d/%d/%d/%d foreground_pixels=%llu color_variants=%zu lines_fit=%d vertices=%d commands=%d FontGlobalScale=%.1f screenshot=%s %s\n",
        hero, section, card, body, small, code, static_cast<unsigned long long>(foreground),
        variants, lines_fit ? 1 : 0, vertices, commands, ImGui::GetIO().FontGlobalScale, output.string().c_str(), pass ? "PASS" : "FAIL");
    return pass;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: dpi_typography_probe <capture-directory>\n");
        return 2;
    }
    const std::filesystem::path output_dir(argv[1]);
    std::error_code error;
    std::filesystem::create_directories(output_dir, error);
    if (error) { std::fprintf(stderr, "capture directory: %s\n", error.message().c_str()); return 1; }
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_Window* window = SDL_CreateWindow("DevLink DPI probe", kWidth, kHeight, SDL_WINDOW_HIDDEN);
    if (!window) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); shutdown(nullptr, nullptr, false, false, false, true); return 1; }
    SDL_Renderer* renderer = SDL_CreateRenderer(window, "software");
    if (!renderer) { std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); shutdown(window, nullptr, false, false, false, true); return 1; }
    ImGui::CreateContext();
    const bool platform_backend = ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    const bool renderer_backend = platform_backend && ImGui_ImplSDLRenderer3_Init(renderer);
    if (!renderer_backend) {
        std::fprintf(stderr, "ImGui SDL renderer initialization failed\n");
        shutdown(window, renderer, renderer_backend, platform_backend, true, true);
        return 1;
    }
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    bool success = true;
    for (std::size_t i = 0; i < kScales.size(); ++i) {
        const auto path = output_dir / (std::string("dpi-") + kScaleLabels[i] + ".bmp");
        if (!run_scale(window, renderer, path, kScales[i], kScaleLabels[i])) success = false;
    }
    shutdown(window, renderer, renderer_backend, platform_backend, true, true);
    return success ? 0 : 1;
}
