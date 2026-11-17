#pragma once
// DevLink — app/desktop_ui/design_system/icons.hpp
// Indicateurs visuels et icônes textuelles haute netteté compatibles UTF-8 sans dépendance externe fragile.
#include <string_view>

namespace devlink::ui::icons {

// Rôles & Identités
inline constexpr std::string_view kHost       = "[HOTE]";
inline constexpr std::string_view kGuest      = "[INVITE]";
inline constexpr std::string_view kShield     = "[SECURE]";
inline constexpr std::string_view kLock       = "[PROTECTED]";

// Navigation
inline constexpr std::string_view kNavDashboard   = "Tableau de Bord";
inline constexpr std::string_view kNavDiscovery   = "Decouverte Locale";
inline constexpr std::string_view kNavSetup       = "Partage Applicatif";
inline constexpr std::string_view kNavSession     = "Session Active";
inline constexpr std::string_view kNavGuestJoin   = "Espace Invite";
inline constexpr std::string_view kNavChat        = "Messagerie";
inline constexpr std::string_view kNavFeedback    = "Signalements";
inline constexpr std::string_view kNavDiagnostics = "Diagnostics";

// Statuts
inline constexpr std::string_view kStatusOnline   = "[EN LIGNE]";
inline constexpr std::string_view kStatusOffline  = "[HORS LIGNE]";
inline constexpr std::string_view kStatusPending  = "[EN ATTENTE]";
inline constexpr std::string_view kStatusError    = "[ERREUR]";
inline constexpr std::string_view kStatusWarning  = "[ATTENTION]";

} // namespace devlink::ui::icons
