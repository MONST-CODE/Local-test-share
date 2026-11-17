#pragma once
// DevLink — app/desktop_ui/design_system/layout.hpp
// Structure d'agencement global (§6.1) : Top Bar, Navigation, Content Workspace.
#include <imgui.h>
#include <string_view>
#include "themes.hpp"
#include "typography.hpp"
#include "components.hpp"

namespace devlink::ui {

enum class MainNavigationTab {
    Dashboard,
    DiscoveryCenter,
    ShareSetup,
    ActiveSession,
    GuestArea,
    Messaging,
    Feedback,
    Diagnostics
};

// Barre supérieure avec identité DevLink, badge de statut, et toggle de thème
inline void render_top_bar(
    std::string_view session_status_text,
    bool is_host_mode,
    ThemeMode current_theme,
    ThemeMode& out_theme_toggle,
    bool& out_mode_toggle
) {
    const auto& c = theme_colors();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, c.surface);
    ImGui::PushStyleColor(ImGuiCol_Border, c.border);
    ImGui::BeginChild("##devlink_topbar", ImVec2(0, 54), true, ImGuiWindowFlags_NoScrollbar);

    // Marque officielle DevLink
    ImGui::AlignTextToFramePadding();
    ImGui::SetWindowFontScale(1.25f);
    ImGui::TextColored(c.accent_light, "DevLink");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::SameLine();
    text_muted(" | Plateforme de Partage & Test Local");

    // Statut de session au centre/droite
    ImGui::SameLine(ImGui::GetWindowWidth() - 440);
    if (!session_status_text.empty()) {
        status_pill(session_status_text, is_host_mode ? c.accent_light : c.status_info);
        ImGui::SameLine();
    }

    // Bascule de Thème (Dev Theme Sombre <-> Light Theme)
    const char* theme_btn_label = (current_theme == ThemeMode::DevDark) ? "Mode Clair" : "Mode Sombre";
    if (button_secondary(theme_btn_label, ImVec2(110, 32))) {
        out_theme_toggle = (current_theme == ThemeMode::DevDark) ? ThemeMode::DevLight : ThemeMode::DevDark;
    }

    // Bascule Espace Hôte / Espace Invité
    ImGui::SameLine();
    const char* mode_btn_label = is_host_mode ? "Espace Invite" : "Espace Hote";
    if (button_primary(mode_btn_label, ImVec2(120, 32))) {
        out_mode_toggle = !is_host_mode;
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

} // namespace devlink::ui
