#pragma once
// PLPT — engine/core/format.hpp — mise en forme lisible, sans flux ni locale.
// Utilisée par l'interface, les journaux d'événements et le service de démo.
#include <cstdint>
#include <string>

namespace plpt {

// « 2026-11-13 17:42:07 UTC » : horodatage explicite, jamais ambigu.
std::string format_utc(std::uint64_t wall_ms);
// « 17:42:07 » : heure courte pour le journal et la messagerie.
std::string format_clock_utc(std::uint64_t wall_ms);
// « 1,2 MiB », « 843 Kio » : taille lisible en unités binaires.
std::string format_bytes(std::uint64_t bytes);
// « 12 ms », « 3,4 s », « 2 min 5 s » : durée lisible.
std::string format_duration_ms(std::uint64_t milliseconds);
// Monotone -> secondes/millisecondes relatifs depuis un instant de référence.
std::string format_elapsed_ms(std::uint64_t elapsed_ms);

} // namespace plpt
