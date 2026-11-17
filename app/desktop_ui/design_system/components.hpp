#pragma once
// DevLink — app/desktop_ui/design_system/components.hpp
// Composants d'interface standardisés DevLink (§5.2).
#include <imgui.h>
#include <string_view>
#include "themes.hpp"
#include "typography.hpp"

namespace devlink::ui {

// Conteneur de carte stylisée avec bordure et padding
inline bool card_begin(const char* str_id, const ImVec2& size = ImVec2(0, 0)) {
    const auto& c = theme_colors();
    const auto& s = theme_spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, c.surface);
    ImGui::PushStyleColor(ImGuiCol_Border, c.border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, s.radius_card);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, s.border_size_card);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(s.card_padding, s.card_padding));
    return ImGui::BeginChild(str_id, size, true, ImGuiWindowFlags_None);
}

inline void card_end() {
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

// Badge de statut (ex: ONLINE, ACTIF, EN ATTENTE, ERREUR)
inline void status_pill(std::string_view label, const ImVec4& color) {
    const auto& s = theme_spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(color.x, color.y, color.z, 0.20f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x, color.y, color.z, 0.30f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(color.x, color.y, color.z, 0.40f));
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, s.radius_badge);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 3.0f));
    ImGui::Button(label.data());
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
}

// Carte statistique avec titre, nombre et sous-titre explicatif
inline void metric_card(const char* id, const char* title, std::size_t count, const char* caption, float width = 200.0f) {
    const auto& c = theme_colors();
    if (card_begin(id, ImVec2(width, 110.0f))) {
        text_muted(title);
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextColored(c.accent_light, "%zu", count);
        ImGui::SetWindowFontScale(1.0f);
        text_disabled_label(caption);
    }
    card_end();
}

// Bouton d'action primaire (Cuivre / Accent principal DevLink)
inline bool button_primary(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    const auto& c = theme_colors();
    const auto& s = theme_spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, c.accent_primary);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c.accent_light);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, c.button_active);
    ImGui::PushStyleColor(ImGuiCol_Text, hex_to_imvec4(0xFFFFFF));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, s.radius_button);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16.0f, 9.0f));
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    return clicked;
}

// Bouton d'action secondaire (Neutre / Surface élevée)
inline bool button_secondary(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    const auto& c = theme_colors();
    const auto& s = theme_spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, c.button_default);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c.button_hovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, c.button_active);
    ImGui::PushStyleColor(ImGuiCol_Text, c.text_primary);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, s.radius_button);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 8.0f));
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    return clicked;
}

// Bouton danger (Arrêt / Révocation)
inline bool button_danger(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    const auto& c = theme_colors();
    const auto& s = theme_spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(c.status_error.x * 0.7f, c.status_error.y * 0.2f, c.status_error.z * 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c.status_error);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(c.status_error.x * 1.2f, c.status_error.y * 0.4f, c.status_error.z * 0.4f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, hex_to_imvec4(0xFFFFFF));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, s.radius_button);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 8.0f));
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    return clicked;
}

// Gestion des états vides (Empty State §6.3)
inline void empty_state(const char* title, const char* description, const char* action_label = nullptr, bool* out_action = nullptr) {
    const auto& c = theme_colors();
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Indent(20.0f);
    text_section_title(title);
    ImGui::Spacing();
    text_muted(description);
    if (action_label && out_action) {
        ImGui::Spacing();
        if (button_primary(action_label)) {
            *out_action = true;
        }
    }
    ImGui::Unindent(20.0f);
    ImGui::Spacing();
}

} // namespace devlink::ui
