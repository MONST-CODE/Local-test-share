// DevLink — app/desktop_ui/design_system/typography.cpp
// Chargement réel des polices et niveaux typographiques (mandat §17).
//
// Chaîne de résolution, dans l'ordre :
//   1. police embarquée OFL depuis assets/fonts/ (si le dépôt en fournit une, sa
//      licence est listée dans assets/fonts/NOTICE.md) ;
//   2. police système du contexte visé : Segoe UI / Consolas sur Windows,
//      DejaVu Sans / Noto Sans / Liberation Sans + mono correspondant sur Linux ;
//   3. repli explicite : police par défaut d'ImGui, signalé dans FontStatus et
//      affiché par l'écran de diagnostics (glyphes limités, aucun accent).
// Rien n'est téléchargé au démarrage : seuls des chemins locaux sont lus.
#include "typography.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace devlink::ui {

namespace {

FontSet g_fonts;
FontStatus g_status;
float g_scale = 1.0f;

// Glyphes requis par l'interface : latin de base + latin-1 supplément (accents
// français), œ/OE, ponctuation typographique (tirets, points de suspension,
// guillemets), euro, flèches et signe moins. Une police candidate qui ne les a
// pas reste utilisable, mais les glyphes manquants s'afficheront en carrés :
// c'est pourquoi le statut réel est publié plutôt que supposé.
const ImWchar kGlyphRanges[] = {
    0x0020, 0x00FF,
    0x0152, 0x0153,
    0x2013, 0x2026,
    0x20AC, 0x20AC,
    0x2190, 0x2193,
    0x2212, 0x2212,
    0x0000,
};

struct Candidate {
    std::string path;
    std::string family;
};

bool file_exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

std::vector<Candidate> ui_candidates() {
    std::vector<Candidate> list;
    // 1. Police embarquée du dépôt (extension documentée, licence dans NOTICE).
    list.push_back(Candidate{"assets/fonts/ui-regular.ttf", "police embarquée DevLink"});
    // 2. Windows : famille d'interface native.
    const char* system_root = std::getenv("SystemRoot");
    const std::string win_fonts = std::string(system_root && *system_root ? system_root : "C:\\Windows") + "\\Fonts\\";
    list.push_back(Candidate{win_fonts + "segoeui.ttf", "Segoe UI"});
    // 3. Linux : familles d'interface usuelles des distributions cibles.
    list.push_back(Candidate{"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "DejaVu Sans"});
    list.push_back(Candidate{"/usr/share/fonts/dejavu/DejaVuSans.ttf", "DejaVu Sans"});
    list.push_back(Candidate{"/usr/share/fonts/TTF/DejaVuSans.ttf", "DejaVu Sans"});
    list.push_back(Candidate{"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "Noto Sans"});
    list.push_back(Candidate{"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", "Liberation Sans"});
    return list;
}

std::vector<Candidate> semibold_candidates() {
    std::vector<Candidate> list;
    list.push_back(Candidate{"assets/fonts/ui-semibold.ttf", "police embarquée DevLink (semibold)"});
    const char* system_root = std::getenv("SystemRoot");
    const std::string win_fonts = std::string(system_root && *system_root ? system_root : "C:\\Windows") + "\\Fonts\\";
    list.push_back(Candidate{win_fonts + "seguisb.ttf", "Segoe UI Semibold"});
    list.push_back(Candidate{"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "DejaVu Sans Bold"});
    list.push_back(Candidate{"/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf", "DejaVu Sans Bold"});
    list.push_back(Candidate{"/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "DejaVu Sans Bold"});
    list.push_back(Candidate{"/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf", "Noto Sans Bold"});
    list.push_back(Candidate{"/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf", "Liberation Sans Bold"});
    return list;
}

std::vector<Candidate> mono_candidates() {
    std::vector<Candidate> list;
    list.push_back(Candidate{"assets/fonts/ui-mono.ttf", "mono embarquée DevLink"});
    const char* system_root = std::getenv("SystemRoot");
    const std::string win_fonts = std::string(system_root && *system_root ? system_root : "C:\\Windows") + "\\Fonts\\";
    list.push_back(Candidate{win_fonts + "consolas.ttf", "Consolas"});
    list.push_back(Candidate{"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "DejaVu Sans Mono"});
    list.push_back(Candidate{"/usr/share/fonts/dejavu/DejaVuSansMono.ttf", "DejaVu Sans Mono"});
    list.push_back(Candidate{"/usr/share/fonts/TTF/DejaVuSansMono.ttf", "DejaVu Sans Mono"});
    list.push_back(Candidate{"/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf", "Liberation Mono"});
    return list;
}

// Les chemins candidats contiennent des std::string temporaires : on résout
// d'abord le chemin gagnant, puis on charge. Aucune référence pendante.
std::string pick_path(const std::vector<Candidate>& candidates, bool* embedded) {
    for (const auto& candidate : candidates) {
        if (file_exists(candidate.path)) {
            if (embedded) *embedded = candidate.path.rfind("assets/fonts/", 0) == 0;
            return candidate.path;
        }
    }
    return {};
}

std::string pick_family(const std::vector<Candidate>& candidates, const std::string& path) {
    for (const auto& candidate : candidates) {
        if (path == candidate.path) return candidate.family;
    }
    return {};
}

} // namespace

FontStatus load_fonts(float display_scale, const TypographyConfig& config, FontSet& out) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    io.FontGlobalScale = 1.0f; // jamais d'échelle globale improvisée : tailles réelles.

    const float scale = display_scale > 0.0f ? display_scale : 1.0f;
    g_scale = scale;
    const auto pixels = [scale](float base) {
        return std::max(9.0f, std::floor(base * scale + 0.5f));
    };

    FontStatus status;
    bool ui_embedded = false;
    bool semibold_embedded = false;
    bool mono_embedded = false;
    const std::string ui_path = pick_path(ui_candidates(), &ui_embedded);
    const std::string semibold_path = pick_path(semibold_candidates(), &semibold_embedded);
    const std::string mono_path = pick_path(mono_candidates(), &mono_embedded);

    if (ui_path.empty()) {
        status.note = "aucune police embarquée ni système trouvée : repli ImGui (glyphes limités)";
    } else {
        status.system = !ui_embedded;
        status.embedded = ui_embedded;
        status.family = pick_family(ui_candidates(), ui_path);
        status.source = ui_path;
        if (mono_path.empty()) {
            status.note = "police monospace introuvable : les codes utilisent la famille d'interface";
        }
    }

    // Niveau corps d'abord : il sert de repli aux autres niveaux si un fichier
    // manque, pour que la hiérarchie reste complète quoi qu'il arrive.
    ImFont* body = nullptr;
    if (!ui_path.empty()) {
        body = io.Fonts->AddFontFromFileTTF(ui_path.c_str(), pixels(config.body), nullptr, kGlyphRanges);
    }
    if (!body) body = io.Fonts->AddFontDefault();

    ImFont* semibold = body;
    if (!semibold_path.empty()) {
        ImFont* loaded = io.Fonts->AddFontFromFileTTF(semibold_path.c_str(), pixels(config.card_title), nullptr, kGlyphRanges);
        if (loaded) semibold = loaded;
    }
    ImFont* mono = body;
    if (!mono_path.empty()) {
        ImFont* loaded = io.Fonts->AddFontFromFileTTF(mono_path.c_str(), pixels(config.code), nullptr, kGlyphRanges);
        if (loaded) mono = loaded;
    }

    // Titres : mêmes familles, tailles réelles supérieures (vraies métriques).
    ImFont* hero = body;
    ImFont* section = body;
    ImFont* card = semibold;
    ImFont* small = body;
    if (!ui_path.empty()) {
        if (ImFont* f = io.Fonts->AddFontFromFileTTF(ui_path.c_str(), pixels(config.hero_title), nullptr, kGlyphRanges)) hero = f;
        if (ImFont* f = io.Fonts->AddFontFromFileTTF(ui_path.c_str(), pixels(config.section_title), nullptr, kGlyphRanges)) section = f;
        if (ImFont* f = io.Fonts->AddFontFromFileTTF(ui_path.c_str(), pixels(config.small), nullptr, kGlyphRanges)) small = f;
        if (!semibold_path.empty()) {
            if (ImFont* f = io.Fonts->AddFontFromFileTTF(semibold_path.c_str(), pixels(config.card_title), nullptr, kGlyphRanges)) card = f;
        }
    }

    out.hero_title = hero;
    out.section_title = section;
    out.card_title = card;
    out.body = body;
    out.small = small;
    out.code = mono;

    status.body_pixels = pixels(config.body);
    g_fonts = out;
    g_status = status;
    return status;
}

const FontSet& current_fonts() { return g_fonts; }
const FontStatus& font_status() { return g_status; }
float current_display_scale() { return g_scale; }

namespace {
void push_level(ImFont* font) { if (font) ImGui::PushFont(font); }
void pop_level(ImFont* font) { if (font) ImGui::PopFont(); }
void write(std::string_view text, const ImVec4& color, ImFont* font) {
    push_level(font);
    ImGui::TextColored(color, "%.*s", static_cast<int>(text.size()), text.data());
    pop_level(font);
}
} // namespace

void text_hero_title(std::string_view text) { write(text, theme_colors().text_primary, g_fonts.hero_title); }
void text_section_title(std::string_view text) { write(text, theme_colors().accent_light, g_fonts.section_title); }
void text_card_title(std::string_view text) { write(text, theme_colors().text_primary, g_fonts.card_title); }
void text_body(std::string_view text) { write(text, theme_colors().text_primary, g_fonts.body); }
void text_muted(std::string_view text) { write(text, theme_colors().text_secondary, g_fonts.body); }
void text_disabled_label(std::string_view text) { write(text, theme_colors().text_disabled, g_fonts.small); }
void text_code(std::string_view text) { write(text, theme_colors().accent_light, g_fonts.code); }

} // namespace devlink::ui
