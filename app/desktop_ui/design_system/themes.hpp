#pragma once
// DevLink — app/desktop_ui/design_system/themes.hpp
// Définition des thèmes DevDark et DevLight et gestion du style ImGui.
#include "tokens.hpp"

namespace devlink::ui {

class ThemeManager {
public:
    static ThemeManager& instance();

    void set_theme(ThemeMode mode);
    ThemeMode current_theme() const { return current_mode_; }
    const ColorTokens& colors() const { return current_tokens_; }
    const SpacingTokens& spacing() const { return spacing_; }

    // Applique le thème courant sur le style Dear ImGui avec prise en compte du facteur d'échelle DPI
    void apply_to_imgui(float dpi_scale);

private:
    ThemeManager();
    void update_tokens();

    ThemeMode current_mode_ = ThemeMode::DevDark;
    ColorTokens current_tokens_{};
    SpacingTokens spacing_{};
};

// Fonctions utilitaires d'accès rapide
inline const ColorTokens& theme_colors() {
    return ThemeManager::instance().colors();
}

inline const SpacingTokens& theme_spacing() {
    return ThemeManager::instance().spacing();
}

} // namespace devlink::ui
