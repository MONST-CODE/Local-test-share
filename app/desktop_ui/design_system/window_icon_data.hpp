#pragma once
// DevLink — app/desktop_ui/design_system/window_icon_data.hpp
// Fichier GÉNÉRÉ par tools/branding/export_icons.py depuis
// assets/branding/devlink-icon-master.svg (variante compacte, tuile incluse).
// Ne pas éditer à la main : régénérer avec l'outil, puis vérifier le rendu.
// Sert à SDL3_SetWindowIcon sur les plateformes sans icône PE (Linux/macOS) ;
// sous Windows l'icône PE du binaire prime, ce tableau reste utilisé comme repli.
#include <cstddef>
#include <cstdint>

namespace devlink::ui::window_icon {

inline constexpr int kWidth = 64;
inline constexpr int kHeight = 64;
inline constexpr std::size_t kBytes = static_cast<std::size_t>(64) * 64 * 4;

// RGBA8, ligne par ligne, origine en haut à gauche.
extern const std::uint8_t kPixelData[kBytes];

} // namespace devlink::ui::window_icon
