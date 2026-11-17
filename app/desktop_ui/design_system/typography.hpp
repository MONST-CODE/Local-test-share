#pragma once
// DevLink — app/desktop_ui/design_system/typography.hpp
// Hiérarchie typographique RÉELLE (mandat §17) : une famille vectorielle chargée
// depuis le disque (police embarquée OFL si présente, sinon police système du
// contexte Windows/Linux), rasterisée aux tailles exactes de la hiérarchie,
// multipliées par l'échelle d'affichage mesurée. Aucun empilement de
// SetWindowFontScale() comme système principal : chaque niveau est une vraie
// taille de police, donc de vraies métriques et un atlas net.
//
// Le chargement est honnête : si aucune police n'est trouvée, l'interface le dit
// (FontStatus) et continue avec la police par défaut d'ImGui, glyphes limités.
#include <imgui.h>
#include <string>
#include <string_view>
#include "themes.hpp"

namespace devlink::ui {

// Tailles de base à 100 % d'échelle, en pixels. Multipliées par l'échelle
// d'affichage mesurée (SDL), jamais devinées.
struct TypographyConfig {
    float hero_title = 24.0f;
    float section_title = 18.0f;
    float card_title = 15.0f;
    float body = 14.0f;
    float small = 12.0f;
    float code = 13.0f;
};

struct FontSet {
    ImFont* hero_title = nullptr;
    ImFont* section_title = nullptr;
    ImFont* card_title = nullptr;
    ImFont* body = nullptr;
    ImFont* small = nullptr;
    ImFont* code = nullptr;
    bool valid() const { return body != nullptr; }
};

// Ce qui a RÉELLEMENT été chargé. `source` est vide quand la police par défaut
// d'ImGui sert de repli : dans ce cas `note` explique pourquoi, et l'écran de
// diagnostics l'affiche. Aucun succès n'est prétendu.
struct FontStatus {
    bool embedded = false;   // police OFL embarquée (assets/fonts/)
    bool system = false;     // police système du contexte (Segoe UI / DejaVu / Noto)
    std::string family;      // nom affichable de la police effectivement chargée
    std::string source;      // chemin réellement lu (vide = repli ImGui)
    std::string note;        // phrase honnête sur un repli éventuel
    float body_pixels = 0.0f; // taille réellement demandée pour le niveau corps
};

// Charge ou recharge l'atlas à l'échelle demandée. Appelé au démarrage et à
// chaque changement d'échelle d'affichage (moniteur différent, zoom système) :
// le backend SDL3 gère l'atlas dynamique (ImGuiBackendFlags_RendererHasTextures).
FontStatus load_fonts(float display_scale, const TypographyConfig& config, FontSet& out);

// Police actuellement servie par chaque niveau (peut être le repli ImGui).
const FontSet& current_fonts();
const FontStatus& font_status();
// Échelle d'affichage réellement appliquée au dernier chargement.
float current_display_scale();

// Niveaux typographiques : poussent la vraie police du niveau, jamais un scale.
void text_hero_title(std::string_view text);
void text_section_title(std::string_view text);
void text_card_title(std::string_view text);
void text_body(std::string_view text);
void text_muted(std::string_view text);
void text_disabled_label(std::string_view text);
void text_code(std::string_view text);

} // namespace devlink::ui
