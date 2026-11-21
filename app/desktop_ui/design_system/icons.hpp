#pragma once
// DevLink — app/desktop_ui/design_system/icons.hpp
// Système d'icônes VECTORIEL dessiné à l'ImDrawList (mandat §9.5, §17) : net à
// toutes les échelles d'affichage, aucune police d'icônes externe, aucune
// dépendance binaire. Chaque glyphe est défini sur une grille 24×24 puis mis à
// l'échelle ; l'épaisseur de trait suit l'échelle pour rester lisible à 200 %.
//
// Les anciens libellés entre crochets (« [HOTE] », « [EN LIGNE] ») ne sont pas
// des icônes : ils sont remplacés par ces glyphes, avec un libellé texte
// accessible conservé pour les infobulles et les lecteurs d'écran.
#include <imgui.h>
#include <cmath>

namespace devlink::ui::icons {

enum class Icon {
    Dashboard, Discovery, Setup, Approval, Session, SharedView,
    Guest, Chat, Feedback, Diagnostics,
    Check, Cross, Warning, Info, Lock, Shield, Users, Pulse
};

// Libellé accessible : le sens d'une icône ne doit jamais reposer sur le seul
// dessin (infobulles, lecteurs d'écran, rapports texte).
inline const char* label(Icon which) {
    switch (which) {
    case Icon::Dashboard: return "tableau de bord";
    case Icon::Discovery: return "découverte locale";
    case Icon::Setup: return "configuration du partage";
    case Icon::Approval: return "file d'approbation";
    case Icon::Session: return "session active";
    case Icon::SharedView: return "vue applicative partagée";
    case Icon::Guest: return "espace invité";
    case Icon::Chat: return "messagerie";
    case Icon::Feedback: return "signalements";
    case Icon::Diagnostics: return "diagnostics";
    case Icon::Check: return "succès";
    case Icon::Cross: return "échec";
    case Icon::Warning: return "avertissement";
    case Icon::Info: return "information";
    case Icon::Lock: return "verrouillé";
    case Icon::Shield: return "protection";
    case Icon::Users: return "participants";
    case Icon::Pulse: return "activité";
    }
    return "icône";
}

namespace detail {

// Grille 24×24 : conversion unité -> pixels écran.
struct Grid {
    ImVec2 origin;
    float size;
    float units = 24.0f; // grille des icônes : 24 ; maître de la marque : 240
    ImVec2 at(float x, float y) const {
        return ImVec2{origin.x + x * size / units, origin.y + y * size / units};
    }
    float s(float v) const { return v * size / units; }
};

inline void polyline(ImDrawList* draw, const Grid& g, ImU32 color, float stroke,
    const float* pts, int count, bool closed) {
    ImVec2 buffer[16];
    const int n = count < 16 ? count : 16;
    for (int i = 0; i < n; ++i) buffer[i] = g.at(pts[i * 2], pts[i * 2 + 1]);
    if (closed) draw->AddConvexPolyFilled(buffer, n, color);
    else draw->AddPolyline(buffer, n, color, stroke, ImDrawFlags_None);
}

// Cubique de Bézier échantillonnée : la panse du D de la marque est une vraie
// courbe, pas une approximation polygonale visible.
inline void cubic(ImDrawList* draw, const Grid& g, ImU32 color, float stroke,
    float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
    int segments = 16) {
    draw->PathLineTo(g.at(x0, y0));
    for (int i = 1; i <= segments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segments);
        const float u = 1.0f - t;
        const float a = u * u * u;
        const float b = 3.0f * u * u * t;
        const float c = 3.0f * u * t * t;
        const float d = t * t * t;
        draw->PathLineTo(g.at(a * x0 + b * x1 + c * x2 + d * x3,
                              a * y0 + b * y1 + c * y2 + d * y3));
    }
    draw->PathStroke(color, ImDrawFlags_None, stroke);
}

} // namespace detail

// Dessine l'icône dans le carré `size` dont le coin haut-gauche est `origin`,
// SANS toucher au layout : sert aux compositions (icône + texte, badges).
inline void draw_at(Icon which, ImVec2 origin, float size, ImU32 color, float stroke = -1.0f) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const detail::Grid g{origin, size};
    const float line = stroke > 0.0f ? stroke : std::max(1.0f, std::floor(size / 12.0f));
    const float half = line * 0.5f;

    switch (which) {
    case Icon::Dashboard: {
        const float pad = g.s(3.0f);
        const float cell = (size - pad * 3.0f) * 0.5f;
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 2; ++col) {
                const ImVec2 a{origin.x + pad + col * (cell + pad), origin.y + pad + row * (cell + pad)};
                draw->AddRectFilled(a, ImVec2{a.x + cell, a.y + cell}, color, g.s(2.0f));
            }
        }
        break;
    }
    case Icon::Discovery: {
        const ImVec2 c = g.at(10.5f, 10.5f);
        draw->AddCircle(c, g.s(6.0f), color, 0, line);
        draw->AddLine(g.at(15.0f, 15.0f), g.at(20.0f, 20.0f), color, line);
        break;
    }
    case Icon::Setup: {
        const float ys[3] = {7.0f, 12.0f, 17.0f};
        const float knobs[3] = {9.0f, 15.0f, 7.0f};
        for (int i = 0; i < 3; ++i) {
            draw->AddLine(g.at(4.0f, ys[i]), g.at(20.0f, ys[i]), color, line);
            draw->AddCircleFilled(g.at(knobs[i], ys[i]), g.s(2.0f), color, 0);
        }
        break;
    }
    case Icon::Approval: {
        draw->AddRect(g.at(4.0f, 4.0f), g.at(20.0f, 20.0f), color, g.s(2.0f), ImDrawFlags_None, line);
        const float check[6] = {8.5f, 12.5f, 11.0f, 15.0f, 16.0f, 9.5f};
        detail::polyline(draw, g, color, line, check, 3, false);
        break;
    }
    case Icon::Session: {
        draw->AddCircle(g.at(12.0f, 9.0f), g.s(3.5f), color, 0, line);
        const float arc[8] = {6.0f, 19.0f, 8.0f, 15.5f, 16.0f, 15.5f, 18.0f, 19.0f};
        detail::polyline(draw, g, color, line, arc, 4, false);
        break;
    }
    case Icon::SharedView: {
        draw->AddRect(g.at(3.5f, 5.0f), g.at(20.5f, 16.0f), color, g.s(1.5f), ImDrawFlags_None, line);
        draw->AddLine(g.at(12.0f, 16.0f), g.at(12.0f, 19.0f), color, line);
        draw->AddLine(g.at(8.0f, 19.5f), g.at(16.0f, 19.5f), color, line);
        break;
    }
    case Icon::Guest: {
        draw->AddRect(g.at(3.5f, 8.0f), g.at(12.5f, 16.0f), color, g.s(3.0f), ImDrawFlags_None, line);
        draw->AddRect(g.at(11.5f, 8.0f), g.at(20.5f, 16.0f), color, g.s(3.0f), ImDrawFlags_None, line);
        break;
    }
    case Icon::Chat: {
        draw->AddRect(g.at(4.0f, 5.0f), g.at(20.0f, 16.0f), color, g.s(3.0f), ImDrawFlags_None, line);
        const float tail[6] = {8.0f, 16.0f, 8.0f, 20.0f, 12.0f, 16.0f};
        detail::polyline(draw, g, color, line, tail, 3, false);
        break;
    }
    case Icon::Feedback: {
        draw->AddLine(g.at(6.0f, 4.0f), g.at(6.0f, 20.0f), color, line);
        const float flag[8] = {6.0f, 4.5f, 17.0f, 4.5f, 17.0f, 11.0f, 6.0f, 11.0f};
        detail::polyline(draw, g, color, line, flag, 4, true);
        break;
    }
    case Icon::Diagnostics: {
        const float pulse[12] = {3.0f, 12.0f, 8.0f, 12.0f, 10.5f, 6.5f, 13.5f, 17.5f, 16.0f, 12.0f, 21.0f, 12.0f};
        detail::polyline(draw, g, color, line, pulse, 6, false);
        break;
    }
    case Icon::Check: {
        const float pts[6] = {5.5f, 12.5f, 10.0f, 17.0f, 18.5f, 7.5f};
        detail::polyline(draw, g, color, line, pts, 3, false);
        break;
    }
    case Icon::Cross: {
        draw->AddLine(g.at(6.5f, 6.5f), g.at(17.5f, 17.5f), color, line);
        draw->AddLine(g.at(17.5f, 6.5f), g.at(6.5f, 17.5f), color, line);
        break;
    }
    case Icon::Warning: {
        const float tri[6] = {12.0f, 4.5f, 20.5f, 19.0f, 3.5f, 19.0f};
        detail::polyline(draw, g, color, line, tri, 3, false);
        draw->AddLine(g.at(12.0f, 10.0f), g.at(12.0f, 14.5f), color, line);
        draw->AddCircleFilled(g.at(12.0f, 16.8f), half, color, 0);
        break;
    }
    case Icon::Info: {
        draw->AddCircle(g.at(12.0f, 12.0f), g.s(8.0f), color, 0, line);
        draw->AddLine(g.at(12.0f, 11.0f), g.at(12.0f, 16.5f), color, line);
        draw->AddCircleFilled(g.at(12.0f, 8.2f), half, color, 0);
        break;
    }
    case Icon::Lock: {
        draw->AddRect(g.at(6.0f, 11.0f), g.at(18.0f, 20.0f), color, g.s(2.0f), ImDrawFlags_None, line);
        // Arceau : demi-cercle supérieur construit par chemin d'arc.
        draw->PathArcTo(g.at(12.0f, 11.0f), g.s(4.0f), 3.14159265f, 6.28318531f, 0);
        draw->PathStroke(color, ImDrawFlags_None, line);
        break;
    }
    case Icon::Shield: {
        const float pts[10] = {12.0f, 3.5f, 19.5f, 6.5f, 19.5f, 12.5f, 12.0f, 20.5f, 4.5f, 12.5f};
        detail::polyline(draw, g, color, line, pts, 5, false);
        draw->AddLine(g.at(4.5f, 12.5f), g.at(4.5f, 6.5f), color, line);
        draw->AddLine(g.at(4.5f, 6.5f), g.at(12.0f, 3.5f), color, line);
        break;
    }
    case Icon::Users: {
        draw->AddCircle(g.at(9.0f, 9.0f), g.s(3.0f), color, 0, line);
        draw->AddCircle(g.at(16.5f, 10.0f), g.s(2.4f), color, 0, line);
        const float a1[8] = {4.0f, 19.0f, 6.0f, 15.5f, 12.0f, 15.5f, 14.0f, 19.0f};
        detail::polyline(draw, g, color, line, a1, 4, false);
        const float a2[6] = {15.0f, 15.0f, 18.5f, 15.0f, 20.0f, 18.5f};
        detail::polyline(draw, g, color, line, a2, 3, false);
        break;
    }
    case Icon::Pulse: {
        const float pts[8] = {3.0f, 12.0f, 9.0f, 12.0f, 12.0f, 6.0f, 15.0f, 18.0f};
        detail::polyline(draw, g, color, line, pts, 4, false);
        draw->AddLine(g.at(15.0f, 18.0f), g.at(17.0f, 12.0f), color, line);
        draw->AddLine(g.at(17.0f, 12.0f), g.at(21.0f, 12.0f), color, line);
        break;
    }
    }
}

// Marque `<-D->` : géométrie IDENTIQUE au maître vectoriel
// (assets/branding/devlink-icon-master.svg, symbole #dl-standard, grille 240),
// redessinée ici pour l'en-tête de l'application et tout emplacement vectoriel
// du runtime. Deux rôles de couleur : chevrons/tirets = accent, D = texte.
// Aucun slogan, aucune flèche triangulaire, aucune tuile de fond.
inline void brand_mark(ImVec2 origin, float size, ImU32 chevron_color, ImU32 letter_color) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // Coordonnées du maître SVG : grille 240, pas 24.
    const detail::Grid g{origin, size, 240.0f};
    const float chevron_stroke = std::max(1.0f, g.s(15.0f));
    const float dash_stroke = std::max(1.0f, g.s(13.0f));
    const float letter_stroke = std::max(1.0f, g.s(16.0f));
    const float left_chevron[6] = {61.0f, 83.0f, 25.0f, 120.0f, 61.0f, 157.0f};
    const float right_chevron[6] = {179.0f, 83.0f, 215.0f, 120.0f, 179.0f, 157.0f};
    const float left_dash[4] = {73.0f, 120.0f, 85.0f, 120.0f};
    const float right_dash[4] = {155.0f, 120.0f, 167.0f, 120.0f};
    detail::polyline(draw, g, chevron_color, chevron_stroke, left_chevron, 3, false);
    detail::polyline(draw, g, chevron_color, chevron_stroke, right_chevron, 3, false);
    detail::polyline(draw, g, chevron_color, dash_stroke, left_dash, 2, false);
    detail::polyline(draw, g, chevron_color, dash_stroke, right_dash, 2, false);
    // D : hampe verticale puis panse en deux cubiques (94/146, 74..166).
    draw->PathLineTo(g.at(94.0f, 166.0f));
    draw->PathLineTo(g.at(94.0f, 74.0f));
    detail::cubic(draw, g, letter_color, letter_stroke,
        94.0f, 74.0f, 126.0f, 74.0f, 146.0f, 94.0f, 146.0f, 120.0f);
    detail::cubic(draw, g, letter_color, letter_stroke,
        146.0f, 120.0f, 126.0f, 146.0f, 94.0f, 166.0f, 94.0f, 166.0f);
    // Le dernier PathStroke de cubic() a fermé la panse ; la hampe fait partie
    // du même chemin : rien à refermer.
}

// Dessine l'icône à la position du curseur ET réserve son carré dans le layout
// (ImGui::Dummy) : aucun chevauchement possible avec le contenu suivant.
inline void icon(Icon which, float size, ImU32 color, float stroke = -1.0f) {
    draw_at(which, ImGui::GetCursorScreenPos(), size, color, stroke);
    ImGui::Dummy(ImVec2{size, size});
}

// Icône puis texte sur une même ligne, alignés verticalement au centre. La
// boîte complète est réservée d'abord : le rendu ne déborde jamais.
inline void icon_then(Icon which, float size, ImU32 color, const char* text) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float height = size > text_size.y ? size : text_size.y;
    ImGui::Dummy(ImVec2{size + spacing + text_size.x, height});
    draw_at(which, ImVec2{origin.x, origin.y + (height - size) * 0.5f}, size, color);
    ImGui::SetCursorScreenPos(ImVec2{origin.x + size + spacing, origin.y + (height - text_size.y) * 0.5f});
    ImGui::TextUnformatted(text);
}

} // namespace devlink::ui::icons
