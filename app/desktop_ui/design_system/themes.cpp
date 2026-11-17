// DevLink — app/desktop_ui/design_system/themes.cpp
// Implémentation des thèmes officiel Dev Theme (sombre) et Light Theme (clair).
#include "themes.hpp"
#include <algorithm>

namespace devlink::ui {

namespace {

ColorTokens create_dev_dark_tokens() {
    ColorTokens t;
    // Spécifications du mandat Trajectoire 1 §5.1 :
    // Background #171312, Surface #211B18, Elevated surface #29201D, Panel #382920,
    // Accent principal #815C43, Accent clair #C49A78,
    // Texte principal #F5E9DC, Texte secondaire #BBA99A, Bordure #493B32
    t.background       = hex_to_imvec4(0x171312);
    t.surface          = hex_to_imvec4(0x211B18);
    t.surface_elevated = hex_to_imvec4(0x29201D);
    t.panel            = hex_to_imvec4(0x382920);
    t.accent_primary   = hex_to_imvec4(0x815C43);
    t.accent_light     = hex_to_imvec4(0xC49A78);
    t.text_primary     = hex_to_imvec4(0xF5E9DC);
    t.text_secondary   = hex_to_imvec4(0xBBA99A);
    t.text_disabled    = hex_to_imvec4(0x736357);
    t.border           = hex_to_imvec4(0x493B32);

    // Boutons et contrôles
    t.button_default   = hex_to_imvec4(0x423126);
    t.button_hovered   = hex_to_imvec4(0x5A4334);
    t.button_active    = hex_to_imvec4(0x735542);

    // États sémantiques (harmonieux avec le thème chocolat/cuivre)
    t.status_success   = hex_to_imvec4(0x68A357); // Vert sauge chaleureux
    t.status_warning   = hex_to_imvec4(0xD99B43); // Ambre caramel
    t.status_error     = hex_to_imvec4(0xC75450); // Brique terracotta
    t.status_info      = hex_to_imvec4(0x6B9BBF); // Bleu ardoise
    t.badge_bg         = hex_to_imvec4(0x30241E);
    return t;
}

ColorTokens create_dev_light_tokens() {
    ColorTokens t;
    // Spécifications du mandat Trajectoire 1 §5.1 :
    // Background #F4EDE4, Surface #FFFFFF, Elevated surface #FAF7F2, Panel #E7D9CA,
    // Accent principal #755039, Accent clair #A47A59,
    // Texte principal #453329, Texte secondaire #786657, Bordure #D8C9B9
    t.background       = hex_to_imvec4(0xF4EDE4);
    t.surface          = hex_to_imvec4(0xFFFFFF);
    t.surface_elevated = hex_to_imvec4(0xFAF7F2);
    t.panel            = hex_to_imvec4(0xE7D9CA);
    t.accent_primary   = hex_to_imvec4(0x755039);
    t.accent_light     = hex_to_imvec4(0xA47A59);
    t.text_primary     = hex_to_imvec4(0x453329);
    t.text_secondary   = hex_to_imvec4(0x786657);
    t.text_disabled    = hex_to_imvec4(0x9E8D7F);
    t.border           = hex_to_imvec4(0xD8C9B9);

    // Boutons et contrôles
    t.button_default   = hex_to_imvec4(0xDECEBF);
    t.button_hovered   = hex_to_imvec4(0xD0BDAA);
    t.button_active    = hex_to_imvec4(0xBF9D7F);

    // États sémantiques
    t.status_success   = hex_to_imvec4(0x3B7A2D);
    t.status_warning   = hex_to_imvec4(0xB36E10);
    t.status_error     = hex_to_imvec4(0xA6322D);
    t.status_info      = hex_to_imvec4(0x32648A);
    t.badge_bg         = hex_to_imvec4(0xEDE0D4);
    return t;
}

} // namespace

ThemeManager& ThemeManager::instance() {
    static ThemeManager s_instance;
    return s_instance;
}

ThemeManager::ThemeManager() {
    update_tokens();
}

void ThemeManager::set_theme(ThemeMode mode) {
    current_mode_ = mode;
    update_tokens();
}

void ThemeManager::update_tokens() {
    if (current_mode_ == ThemeMode::DevDark) {
        current_tokens_ = create_dev_dark_tokens();
    } else {
        current_tokens_ = create_dev_light_tokens();
    }
}

void ThemeManager::apply_to_imgui(float dpi_scale) {
    const float scale = (dpi_scale > 0.0f) ? dpi_scale : 1.0f;
    auto& style = ImGui::GetStyle();

    // 1. Réinitialisation vers base ImGui adaptée
    if (current_mode_ == ThemeMode::DevDark) {
        ImGui::StyleColorsDark();
    } else {
        ImGui::StyleColorsLight();
    }

    // 2. Géométrie et espacements selon SpacingTokens
    style.WindowPadding    = ImVec2(spacing_.window_padding_x * scale, spacing_.window_padding_y * scale);
    style.FramePadding     = ImVec2(spacing_.frame_padding_x * scale, spacing_.frame_padding_y * scale);
    style.ItemSpacing      = ImVec2(spacing_.item_spacing_x * scale, spacing_.item_spacing_y * scale);
    style.ItemInnerSpacing = ImVec2(8.0f * scale, 6.0f * scale);
    style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
    style.IndentSpacing    = 20.0f * scale;
    style.ScrollbarSize    = 13.0f * scale;
    style.GrabMinSize      = 12.0f * scale;

    // Rayons d'arrondi
    style.WindowRounding    = spacing_.radius_window;
    style.ChildRounding     = spacing_.radius_card * scale;
    style.FrameRounding     = spacing_.radius_input * scale;
    style.PopupRounding     = spacing_.radius_card * scale;
    style.ScrollbarRounding = 6.0f * scale;
    style.GrabRounding      = spacing_.radius_button * scale;
    style.TabRounding       = spacing_.radius_button * scale;

    // Bordures
    style.WindowBorderSize  = 0.0f;
    style.ChildBorderSize   = spacing_.border_size_card;
    style.PopupBorderSize   = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.TabBorderSize     = 1.0f;

    // Aligner texte avec cadres
    style.ButtonTextAlign   = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.5f);

    // 3. Application des tokens de couleur officiels DevLink
    const auto& c = current_tokens_;
    auto* colors = style.Colors;

    colors[ImGuiCol_Text]                  = c.text_primary;
    colors[ImGuiCol_TextDisabled]          = c.text_disabled;
    colors[ImGuiCol_WindowBg]              = c.background;
    colors[ImGuiCol_ChildBg]               = c.surface;
    colors[ImGuiCol_PopupBg]               = c.surface_elevated;
    colors[ImGuiCol_Border]                = c.border;
    colors[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);

    colors[ImGuiCol_FrameBg]               = c.panel;
    colors[ImGuiCol_FrameBgHovered]        = c.button_hovered;
    colors[ImGuiCol_FrameBgActive]         = c.button_active;

    colors[ImGuiCol_TitleBg]               = c.surface;
    colors[ImGuiCol_TitleBgActive]         = c.panel;
    colors[ImGuiCol_TitleBgCollapsed]      = c.background;

    colors[ImGuiCol_MenuBarBg]             = c.surface;
    colors[ImGuiCol_ScrollbarBg]           = c.background;
    colors[ImGuiCol_ScrollbarGrab]         = c.button_default;
    colors[ImGuiCol_ScrollbarGrabHovered]  = c.button_hovered;
    colors[ImGuiCol_ScrollbarGrabActive]   = c.button_active;

    colors[ImGuiCol_CheckMark]             = c.accent_light;
    colors[ImGuiCol_SliderGrab]            = c.accent_primary;
    colors[ImGuiCol_SliderGrabActive]      = c.accent_light;

    colors[ImGuiCol_Button]                = c.button_default;
    colors[ImGuiCol_ButtonHovered]         = c.button_hovered;
    colors[ImGuiCol_ButtonActive]          = c.button_active;

    colors[ImGuiCol_Header]                = c.panel;
    colors[ImGuiCol_HeaderHovered]         = c.button_hovered;
    colors[ImGuiCol_HeaderActive]          = c.button_active;

    colors[ImGuiCol_Separator]             = c.border;
    colors[ImGuiCol_SeparatorHovered]      = c.accent_primary;
    colors[ImGuiCol_SeparatorActive]       = c.accent_light;

    colors[ImGuiCol_ResizeGrip]            = ImVec4(c.accent_primary.x, c.accent_primary.y, c.accent_primary.z, 0.25f);
    colors[ImGuiCol_ResizeGripHovered]     = c.accent_primary;
    colors[ImGuiCol_ResizeGripActive]      = c.accent_light;

    colors[ImGuiCol_Tab]                   = c.surface;
    colors[ImGuiCol_TabHovered]            = c.panel;
    colors[ImGuiCol_TabActive]             = c.panel;
    colors[ImGuiCol_TabUnfocused]          = c.background;
    colors[ImGuiCol_TabUnfocusedActive]    = c.surface;

    colors[ImGuiCol_TableHeaderBg]         = c.surface_elevated;
    colors[ImGuiCol_TableBorderStrong]     = c.border;
    colors[ImGuiCol_TableBorderLight]      = ImVec4(c.border.x, c.border.y, c.border.z, 0.40f);
    colors[ImGuiCol_TableRowBg]            = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_TableRowBgAlt]         = ImVec4(c.panel.x, c.panel.y, c.panel.z, 0.35f);

    colors[ImGuiCol_TextSelectedBg]        = ImVec4(c.accent_primary.x, c.accent_primary.y, c.accent_primary.z, 0.45f);
    colors[ImGuiCol_NavHighlight]          = c.accent_light;
    colors[ImGuiCol_NavWindowingHighlight] = c.accent_primary;
    colors[ImGuiCol_NavWindowingDimBg]     = ImVec4(0, 0, 0, 0.60f);
    colors[ImGuiCol_ModalWindowDimBg]      = ImVec4(0, 0, 0, 0.65f);
}

} // namespace devlink::ui
