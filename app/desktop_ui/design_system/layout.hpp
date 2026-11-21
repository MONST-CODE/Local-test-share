#pragma once
// DevLink — app/desktop_ui/design_system/layout.hpp
// Structure d'agencement global (mandat §17) : Top Bar, Navigation, Content Workspace.
// L'architecture d'écran sépare strictement l'espace Hôte et l'espace Invité :
// aucune responsabilité mélangée, aucun écran qui prétend à un état non confirmé
// par le moteur.
#include <imgui.h>
#include <string_view>
#include "engine/core/product.hpp"
#include "components.hpp"
#include "icons.hpp"
#include "themes.hpp"
#include "typography.hpp"

namespace devlink::ui {

// Écrans du mandat §17. Les valeurs sont stables : elles servent d'index de
// navigation et ne doivent pas être réordonnées sans raison.
enum class MainNavigationTab {
    // Espace Hôte
    Dashboard,        // tableau de bord
    DiscoveryCenter,  // centre de découverte des services locaux
    ShareSetup,       // configuration du partage
    ApprovalQueue,    // file d'approbation / gestion des invités
    ActiveSession,    // session active et contrôle des participants
    SharedView,       // vue applicative partagée
    // Espace Invité
    GuestSessions,    // découverte des sessions LAN
    GuestJoin,        // connexion par invitation + état d'approbation
    // Communs
    Messaging,        // chat et feedback de session
    Feedback,         // signalements
    Diagnostics       // diagnostics et état réseau
};

// Libellé de navigation : chaîne littérale terminée par zéro, jamais un
// std::string_view dont data() n'offre aucune garantie de terminaison.
inline const char* nav_label(MainNavigationTab tab) {
    switch (tab) {
    case MainNavigationTab::Dashboard: return "Tableau de bord";
    case MainNavigationTab::DiscoveryCenter: return "Découverte locale";
    case MainNavigationTab::ShareSetup: return "Partage applicatif";
    case MainNavigationTab::ApprovalQueue: return "File d'approbation";
    case MainNavigationTab::ActiveSession: return "Session active";
    case MainNavigationTab::SharedView: return "Vue partagée";
    case MainNavigationTab::GuestSessions: return "Sessions du réseau";
    case MainNavigationTab::GuestJoin: return "Rejoindre";
    case MainNavigationTab::Messaging: return "Messagerie";
    case MainNavigationTab::Feedback: return "Signalements";
    case MainNavigationTab::Diagnostics: return "Diagnostics";
    }
    return "Écran";
}

inline bool is_host_tab(MainNavigationTab tab) {
    return tab == MainNavigationTab::Dashboard || tab == MainNavigationTab::DiscoveryCenter ||
        tab == MainNavigationTab::ShareSetup || tab == MainNavigationTab::ApprovalQueue ||
        tab == MainNavigationTab::ActiveSession;
}

inline bool is_guest_tab(MainNavigationTab tab) {
    return tab == MainNavigationTab::GuestSessions || tab == MainNavigationTab::GuestJoin;
}

// Barre supérieure : identité DevLink, badge de statut réel, bascules thème et espace.
// Le texte de statut vient de l'appelant : il doit refléter un état confirmé par le
// moteur, jamais une intention.
inline void render_top_bar(
    std::string_view session_status_text,
    const ImVec4& status_color,
    icons::Icon status_glyph,
    bool is_host_mode,
    ThemeMode current_theme,
    ThemeMode& out_theme_toggle,
    bool& out_mode_toggle
) {
    const auto& c = theme_colors();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, c.surface);
    ImGui::PushStyleColor(ImGuiCol_Border, c.border);
    ImGui::BeginChild("##devlink_topbar", ImVec2(0, 54), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);

    // Marque officielle DevLink : symbole vectoriel reel (geometrie du maitre
    // SVG), nom et signature tires de la source unique d'identite. Aucun scale
    // de police improvise : le nom utilise le niveau hero de la hierarchie.
    const ImVec2 mark_origin = ImGui::GetCursorScreenPos();
    icons::brand_mark(ImVec2{mark_origin.x, mark_origin.y + 4.0f}, 34.0f,
        ImGui::ColorConvertFloat4ToU32(c.accent_light),
        ImGui::ColorConvertFloat4ToU32(c.text_primary));
    ImGui::Dummy(ImVec2{34.0f, 42.0f});
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    text_hero_title(std::string_view(devlink::kProductName));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    text_muted(std::string_view(devlink::kProductTagline));

    // Bascules à droite : thème puis espace. Positions calculées, pas devinées.
    const char* theme_btn_label = (current_theme == ThemeMode::DevDark) ? "Mode clair" : "Mode sombre";
    const char* mode_btn_label = is_host_mode ? "Espace invité" : "Espace hôte";
    const float theme_w = ImGui::CalcTextSize(theme_btn_label).x + 32.0f;
    const float mode_w = ImGui::CalcTextSize(mode_btn_label).x + 32.0f;
    const float available = ImGui::GetContentRegionAvail().x;
    if (available > theme_w + mode_w + 24.0f) {
        ImGui::SameLine(available - theme_w - mode_w - 12.0f);
    } else {
        ImGui::SameLine();
    }
    if (button_secondary(theme_btn_label, ImVec2(theme_w, 32))) {
        out_theme_toggle = (current_theme == ThemeMode::DevDark) ? ThemeMode::DevLight : ThemeMode::DevDark;
    }
    ImGui::SameLine();
    if (button_primary(mode_btn_label, ImVec2(mode_w, 32))) {
        out_mode_toggle = !is_host_mode;
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(2);

    // Bandeau de statut sous la barre : une phrase courte, lisible, vérifiable.
    if (!session_status_text.empty()) {
        status_pill(status_glyph, session_status_text, status_color);
    }
}

} // namespace devlink::ui
