#pragma once
// DevLink — app/desktop_ui/design_system/typography.hpp
// Hiérarchie typographique et gestion DPI conforme au mandat Trajectoire 1 (§5.3, §5.4).
#include <imgui.h>
#include <string_view>
#include "themes.hpp"

namespace devlink::ui {

struct TypographyConfig {
    float font_size_large_title = 24.0f;
    float font_size_section_title = 18.0f;
    float font_size_card_title = 15.0f;
    float font_size_body = 14.0f;
    float font_size_small = 12.0f;
    float font_size_code = 13.0f;
};

// Fonctions de rendu typographique hiérarchique
inline void text_hero_title(std::string_view text) {
    const auto& c = theme_colors();
    ImGui::SetWindowFontScale(1.35f);
    ImGui::TextColored(c.text_primary, "%.*s", static_cast<int>(text.size()), text.data());
    ImGui::SetWindowFontScale(1.0f);
}

inline void text_section_title(std::string_view text) {
    const auto& c = theme_colors();
    ImGui::SetWindowFontScale(1.15f);
    ImGui::TextColored(c.accent_light, "%.*s", static_cast<int>(text.size()), text.data());
    ImGui::SetWindowFontScale(1.0f);
}

inline void text_card_title(std::string_view text) {
    const auto& c = theme_colors();
    ImGui::TextColored(c.text_primary, "%.*s", static_cast<int>(text.size()), text.data());
}

inline void text_body(std::string_view text) {
    const auto& c = theme_colors();
    ImGui::TextColored(c.text_primary, "%.*s", static_cast<int>(text.size()), text.data());
}

inline void text_muted(std::string_view text) {
    const auto& c = theme_colors();
    ImGui::TextColored(c.text_secondary, "%.*s", static_cast<int>(text.size()), text.data());
}

inline void text_disabled_label(std::string_view text) {
    const auto& c = theme_colors();
    ImGui::TextColored(c.text_disabled, "%.*s", static_cast<int>(text.size()), text.data());
}

inline void text_code(std::string_view text) {
    const auto& c = theme_colors();
    ImGui::TextColored(c.accent_light, "%.*s", static_cast<int>(text.size()), text.data());
}

} // namespace devlink::ui
