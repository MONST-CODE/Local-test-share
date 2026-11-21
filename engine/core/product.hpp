#pragma once
// DevLink — engine/core/product.hpp — identité publique du produit, SOURCE UNIQUE.
//
// Règle du mandat §16 : la version ne doit diverger nulle part. `--version`, les
// titres de fenêtres, les messages CLI publics, le manifeste Windows (quadruplet),
// les noms d'archives et la documentation sont générés ou vérifiés depuis ces
// constantes : build.sh et build.bat relisent kProductVersion, et la sonde de
// compilation compare le quadruplet embarqué dans le PE à la valeur attendue.
//
// Les identifiants internes, eux, restent volontairement stables (ADR-015) :
// namespace C++ `plpt`, magie de protocole 0x504C5054, nom de paramètre/cookie
// `plpt`. Ce sont des identifiants de fil et de compatibilité, pas une identité
// publique : les renommer casserait le protocole sans bénéfice utilisateur.
#include <cstdint>
#include <string_view>

namespace devlink {

inline constexpr std::string_view kProductName = "DevLink";
inline constexpr std::string_view kProductVersion = "0.2.0-preview";
inline constexpr std::string_view kProductTagline = "Partage et test applicatif local";

// Quadruplet Windows dérivé de kProductVersion (majeur.mineur.correctif.0).
// Il n'est PAS écrit ici : les scripts de build le calculent depuis
// kProductVersion pour qu'une seule modification suffise, et la sonde vérifie
// que le PE embarque exactement cette valeur.
inline constexpr std::uint32_t kProtocolMagicStable = 0x504C5054u; // 'PLPT' : fil, pas marque.

} // namespace devlink
