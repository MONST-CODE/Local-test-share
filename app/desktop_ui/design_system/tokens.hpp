#pragma once
// DevLink — app/desktop_ui/design_system/tokens.hpp
// Design Tokens officiels DevLink conformes au mandat d'exécution Trajectoire 1 (§5).
#include <imgui.h>
#include <cstdint>

namespace devlink::ui {

// Conversion d'une valeur hexadécimale 0xRRGGBB(AA) en ImVec4
constexpr ImVec4 hex_to_imvec4(std::uint32_t hex, float alpha = 1.0f) {
    float r = ((hex >> 16) & 0xFF) / 255.0f;
    float g = ((hex >> 8) & 0xFF) / 255.0f;
    float b = (hex & 0xFF) / 255.0f;
    return ImVec4{r, g, b, alpha};
}

enum class ThemeMode {
    DevDark,   // Dev Theme : chocolat, café, marron profond, beige, cuivre
    DevLight   // Light Theme : blanc, ivoire, sable, nuances de chocolat
};

// Tokens sémantiques unifiés pour les deux thèmes (§5.1)
struct ColorTokens {
    ImVec4 background;
    ImVec4 surface;
    ImVec4 surface_elevated;
    ImVec4 panel;
    ImVec4 accent_primary;
    ImVec4 accent_light;
    ImVec4 text_primary;
    ImVec4 text_secondary;
    ImVec4 text_disabled;
    ImVec4 border;
    
    // États interactifs
    ImVec4 button_default;
    ImVec4 button_hovered;
    ImVec4 button_active;
    
    // États sémantiques
    ImVec4 status_success;
    ImVec4 status_warning;
    ImVec4 status_error;
    ImVec4 status_info;
    ImVec4 badge_bg;
};

// Dimensions et espacements (§5.2)
struct SpacingTokens {
    float window_padding_x = 24.0f;
    float window_padding_y = 20.0f;
    float frame_padding_x = 12.0f;
    float frame_padding_y = 8.0f;
    float item_spacing_x = 12.0f;
    float item_spacing_y = 10.0f;
    float section_margin_y = 22.0f;
    float card_padding = 16.0f;
    
    float radius_window = 0.0f;     // Fenêtre principale sans bord arrondis superflus
    float radius_card = 8.0f;       // Cartes et panneaux
    float radius_button = 6.0f;     // Boutons interactifs
    float radius_input = 5.0f;      // Champs de formulaires
    float radius_badge = 12.0f;     // Badges / pills d'état
    
    float border_size_panel = 1.0f;
    float border_size_card = 1.0f;
};

} // namespace devlink::ui
