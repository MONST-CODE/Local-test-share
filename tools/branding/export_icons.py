#!/usr/bin/env python3
"""DevLink — export des ressources de marque (outil de maintenance, hors build).

Ce script n'est JAMAIS appelé par build.sh ni build.bat : le build n'a pas
besoin de Python. Il sert à régénérer, depuis la géométrie vectorielle de
référence (assets/branding/devlink-icon-master.svg), les artefacts binaires qui,
eux, sont versionnés et consommés par le build et le runtime :

  assets/branding/devlink-{16,24,32,48,64,128,256}.png   icônes raster
  assets/branding/devlink.ico                            ICO multi-tailles réel
  app/desktop_ui/design_system/window_icon_data.{hpp,cpp} RGBA embarqué pour
                                                          SDL3_SetWindowIcon

Rendu : rasteriseur vectoriel autonome (aucune dépendance : ni PIL, ni cairo, ni
numpy). Il lit un sous-ensemble SVG strict et documenté (svg/symbol/g/use/path/
rect/circle, commandes M L C Z absolues, transform translate/scale, attributs de
tracé hérités). Toute commande ou attribut non pris en charge provoque une
ERREUR explicite : rien n'est ignoré en silence.

Le ICO produit est un vrai ICO : entrées 16/24/32/48/64/128 en DIB 32 bpp
(BGRA + masque AND 1 bpp) et entrée 256 en PNG compressé, comme le veut la
convention Windows. `--verify` relit le fichier écrit et contrôle chaque entrée
(dimensions, profondeur, taille réelle des données) : un fichier simplement
renommé .ico échoue ici.

Usage :
  python3 tools/branding/export_icons.py            # écrit les artefacts
  python3 tools/branding/export_icons.py --verify    # écrit puis vérifie
  python3 tools/branding/export_icons.py --check     # vérifie sans écrire
"""
from __future__ import annotations

import argparse
import hashlib
import math
import re
import struct
import sys
import xml.etree.ElementTree as ET
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BRAND = REPO / "assets" / "branding"
MASTER = BRAND / "devlink-icon-master.svg"
ICON_SIZES = (16, 24, 32, 48, 64, 128, 256)
COMPACT_SIZES = (16, 24)
SVG_NS = "{http://www.w3.org/2000/svg}"
XLINK = "{http://www.w3.org/1999/xlink}href"

# --------------------------------------------------------------------------- #
# Analyse SVG (sous-ensemble strict)
# --------------------------------------------------------------------------- #


class SvgError(RuntimeError):
    pass


def parse_color(value: str | None) -> tuple[int, int, int] | None:
    if value is None:
        return None
    value = value.strip()
    if value.lower() == "none":
        return None
    match = re.fullmatch(r"#([0-9a-fA-F]{6})", value)
    if not match:
        raise SvgError(f"couleur non prise en charge : {value!r}")
    number = int(match.group(1), 16)
    return ((number >> 16) & 0xFF, (number >> 8) & 0xFF, number & 0xFF)


def parse_transform(value: str | None) -> tuple[float, float, float]:
    """Retourne (scale, tx, ty) pour translate()/scale() imbriqués."""
    scale, tx, ty = 1.0, 0.0, 0.0
    if not value:
        return scale, tx, ty
    for name, args in re.findall(r"(\w+)\s*\(([^)]*)\)", value):
        numbers = [float(part) for part in re.split(r"[\s,]+", args.strip()) if part]
        if name == "translate":
            if len(numbers) not in (1, 2):
                raise SvgError(f"translate invalide : {value!r}")
            tx += numbers[0] * scale
            ty += (numbers[1] if len(numbers) == 2 else 0.0) * scale
        elif name == "scale":
            if len(numbers) not in (1, 2):
                raise SvgError(f"scale invalide : {value!r}")
            sx = numbers[0]
            sy = numbers[1] if len(numbers) == 2 else sx
            if abs(sx - sy) > 1e-9:
                raise SvgError("scale non uniforme refusé (déformerait le signe)")
            scale *= sx
        else:
            raise SvgError(f"transform non prise en charge : {name}()")
    leftover = re.sub(r"\w+\s*\([^)]*\)", "", value).strip()
    if leftover:
        raise SvgError(f"transform illisible : {value!r}")
    return scale, tx, ty


def parse_path(data: str) -> list[list[tuple[float, float]]]:
    """Commandes absolues M, L, C, Z uniquement. Retourne des sous-tracés."""
    tokens = re.findall(r"[MLCZ]|-?\d*\.?\d+(?:[eE][-+]?\d+)?", data)
    subpaths: list[list[tuple[float, float]]] = []
    current: list[tuple[float, float]] = []
    index = 0
    start = (0.0, 0.0)
    while index < len(tokens):
        command = tokens[index]
        if not command.isalpha():
            raise SvgError(f"séquence de commandes invalide près de {command!r}")
        index += 1
        if command == "M":
            if current:
                subpaths.append(current)
            current = [(float(tokens[index]), float(tokens[index + 1]))]
            start = current[0]
            index += 2
        elif command == "L":
            if not current:
                raise SvgError("L sans M préalable")
            current.append((float(tokens[index]), float(tokens[index + 1])))
            index += 2
        elif command == "C":
            if not current:
                raise SvgError("C sans M préalable")
            control = [
                (float(tokens[index + i]), float(tokens[index + i + 1])) for i in (0, 2, 4)
            ]
            current.extend(flatten_cubic(current[-1], *control))
            index += 6
        elif command == "Z":
            if current and current[0] != start:
                current.append(start)
            subpaths.append(current)
            current = []
        else:
            raise SvgError(f"commande de tracé non prise en charge : {command!r}")
    if current:
        subpaths.append(current)
    return [path for path in subpaths if len(path) >= 2]


def flatten_cubic(
    p0: tuple[float, float],
    p1: tuple[float, float],
    p2: tuple[float, float],
    p3: tuple[float, float],
    tolerance: float = 0.05,
) -> list[tuple[float, float]]:
    """Subdivision récursive d'une cubique de Bézier en segments."""
    ux = 3.0 * p1[0] - 2.0 * p0[0] - p3[0]
    uy = 3.0 * p1[1] - 2.0 * p0[1] - p3[1]
    vx = 3.0 * p2[0] - p0[0] - 2.0 * p3[0]
    vy = 3.0 * p2[1] - p0[1] - 2.0 * p3[1]
    error = max(ux * ux, vx * vx) + max(uy * uy, vy * vy)
    if error <= 16.0 * tolerance * tolerance:
        return [p3]
    a = ((p0[0] + p1[0]) / 2, (p0[1] + p1[1]) / 2)
    b = ((p1[0] + p2[0]) / 2, (p1[1] + p2[1]) / 2)
    c = ((p2[0] + p3[0]) / 2, (p2[1] + p3[1]) / 2)
    d = ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)
    e = ((b[0] + c[0]) / 2, (b[1] + c[1]) / 2)
    f = ((d[0] + e[0]) / 2, (d[1] + e[1]) / 2)
    return flatten_cubic(p0, a, d, f, tolerance) + flatten_cubic(f, e, c, p3, tolerance)


Primitive = tuple  # ('stroke'|'fill_circle'|'fill_rrect', ...)


class Style:
    __slots__ = ("stroke", "stroke_width", "linecap", "linejoin", "fill")

    def __init__(self, stroke=None, stroke_width=1.0, linecap="butt", linejoin="miter", fill=None):
        self.stroke = stroke
        self.stroke_width = stroke_width
        self.linecap = linecap
        self.linejoin = linejoin
        self.fill = fill

    def child(self, element: ET.Element) -> "Style":
        stroke = element.get("stroke", None)
        width = element.get("stroke-width", None)
        cap = element.get("stroke-linecap", None)
        join = element.get("stroke-linejoin", None)
        fill = element.get("fill", None)
        return Style(
            stroke=parse_color(stroke) if stroke is not None else self.stroke,
            stroke_width=float(width) if width is not None else self.stroke_width,
            linecap=cap if cap is not None else self.linecap,
            linejoin=join if join is not None else self.linejoin,
            fill=parse_color(fill) if fill is not None else self.fill,
        )


class Renderer:
    """Collecte une liste d'affichage depuis un symbole SVG donné."""

    def __init__(self, root: ET.Element, size: int):
        self.root = root
        self.size = size
        self.symbols: dict[str, ET.Element] = {}
        for symbol in root.iter(f"{SVG_NS}symbol"):
            identifier = symbol.get("id")
            if not identifier:
                raise SvgError("symbol sans id")
            self.symbols[identifier] = symbol
        self.primitives: list[Primitive] = []

    def render_symbol(self, symbol_id: str) -> list[Primitive]:
        if symbol_id not in self.symbols:
            raise SvgError(f"symbole inconnu : {symbol_id!r}")
        symbol = self.symbols[symbol_id]
        viewbox = symbol.get("viewBox") or self.root.get("viewBox")
        if not viewbox:
            raise SvgError("viewBox manquant")
        min_x, min_y, width, height = (float(part) for part in re.split(r"[\s,]+", viewbox))
        if abs(width - height) > 1e-9:
            raise SvgError("viewBox non carré : le signe doit rester non déformé")
        scale = self.size / width
        self.primitives = []
        self.walk(symbol, Style(fill=(0, 0, 0)), scale, -min_x * scale, -min_y * scale)
        return self.primitives

    def walk(self, element: ET.Element, style: Style, scale: float, tx: float, ty: float) -> None:
        # Composition SVG exacte : transform="translate(a,b) scale(s)" envoie le
        # point local p sur (a + s*px, b + s*py) dans le repère parent, donc
        # device = (tx + scale*a) + (scale*s)*px.
        local_scale, local_tx, local_ty = parse_transform(element.get("transform"))
        new_scale = scale * local_scale
        tx = tx + scale * local_tx
        ty = ty + scale * local_ty
        scale = new_scale
        style = style.child(element)
        tag = element.tag.replace(SVG_NS, "")
        if tag == "use":
            reference = element.get("href") or element.get(XLINK)
            if not reference or not reference.startswith("#"):
                raise SvgError(f"use sans référence locale : {reference!r}")
            target = self.symbols.get(reference[1:])
            if target is None:
                raise SvgError(f"use vers un symbole inexistant : {reference!r}")
            self.walk_children(target, style, scale, tx, ty)
            return
        if tag == "path":
            data = element.get("d")
            if data is None:
                raise SvgError("path sans attribut d")
            for subpath in parse_path(data):
                points = [(x * scale + tx, y * scale + ty) for x, y in subpath]
                closed = subpath[0] == subpath[-1]
                if style.stroke is not None:
                    # L'union de capsules donne exactement des terminaisons et des
                    # jointures arrondies : c'est le seul mode implémenté. Un tracé
                    # à plus de deux points avec jointure miter/bevel est refusé
                    # plutôt que rendu approximativement.
                    if style.linecap != "round":
                        raise SvgError(
                            f"terminaison non prise en charge : {style.linecap!r} (arrondi requis)"
                        )
                    if len(points) > 2 and style.linejoin != "round":
                        raise SvgError(
                            f"jointure non prise en charge : {style.linejoin!r} (arrondi requis)"
                        )
                    self.primitives.append(
                        ("stroke", points, style.stroke_width * scale, style.stroke, closed)
                    )
                if style.fill is not None and closed:
                    self.primitives.append(("fill_polygon", points, style.fill))
        elif tag == "rect":
            width = float(element.get("width", "0"))
            height = float(element.get("height", "0"))
            radius_x = float(element.get("rx", "0") or 0)
            radius_y = float(element.get("ry", "0") or radius_x)
            color = style.fill if style.fill is not None else style.stroke
            if color is None:
                raise SvgError("rect sans couleur")
            self.primitives.append(
                (
                    "fill_rrect",
                    float(element.get("x", "0")) * scale + tx,
                    float(element.get("y", "0")) * scale + ty,
                    width * scale,
                    height * scale,
                    radius_x * scale,
                    radius_y * scale,
                    color,
                )
            )
        elif tag == "circle":
            color = style.fill if style.fill is not None else style.stroke
            if color is None:
                raise SvgError("circle sans couleur")
            self.primitives.append(
                (
                    "fill_circle",
                    float(element.get("cx", "0")) * scale + tx,
                    float(element.get("cy", "0")) * scale + ty,
                    float(element.get("r", "0")) * scale,
                    color,
                )
            )
        elif tag in ("symbol", "g", "svg", "defs"):
            self.walk_children(element, style, scale, tx, ty)
        else:
            raise SvgError(f"élément SVG non pris en charge : <{tag}>")

    def walk_children(self, element: ET.Element, style: Style, scale: float, tx: float, ty: float) -> None:
        for child in element:
            if isinstance(child.tag, str):
                self.walk(child, style, scale, tx, ty)


# --------------------------------------------------------------------------- #
# Rasterisation (couverture analytique, supersampling, aucun dépendance)
# --------------------------------------------------------------------------- #


class Canvas:
    def __init__(self, width: int, height: int):
        self.width = width
        self.height = height
        self.pixels = bytearray(width * height * 4)  # RGBA, source-over

    def blend(self, x: int, y: int, coverage: float, color: tuple[int, int, int]) -> None:
        if coverage <= 0.0:
            return
        if coverage > 1.0:
            coverage = 1.0
        index = (y * self.width + x) * 4
        alpha = coverage
        destination_alpha = self.pixels[index + 3] / 255.0
        output_alpha = alpha + destination_alpha * (1.0 - alpha)
        if output_alpha <= 0.0:
            return
        for channel in range(3):
            source = color[channel] / 255.0
            destination = self.pixels[index + channel] / 255.0
            mixed = (source * alpha + destination * destination_alpha * (1.0 - alpha)) / output_alpha
            self.pixels[index + channel] = int(round(mixed * 255.0))
        self.pixels[index + 3] = int(round(output_alpha * 255.0))

    def stroke(self, points: list[tuple[float, float]], width: float, color: tuple[int, int, int]) -> None:
        half = width / 2.0
        reach = int(math.ceil(half + 1.5))
        for index in range(len(points) - 1):
            ax, ay = points[index]
            bx, by = points[index + 1]
            dx, dy = bx - ax, by - ay
            length_squared = dx * dx + dy * dy
            min_x = max(0, int(math.floor(min(ax, bx)) - reach))
            max_x = min(self.width - 1, int(math.ceil(max(ax, bx)) + reach))
            min_y = max(0, int(math.floor(min(ay, by)) - reach))
            max_y = min(self.height - 1, int(math.ceil(max(ay, by)) + reach))
            for py in range(min_y, max_y + 1):
                fy = py + 0.5
                for px in range(min_x, max_x + 1):
                    fx = px + 0.5
                    if length_squared <= 1e-12:
                        distance = math.hypot(fx - ax, fy - ay)
                    else:
                        t = ((fx - ax) * dx + (fy - ay) * dy) / length_squared
                        t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
                        distance = math.hypot(fx - (ax + t * dx), fy - (ay + t * dy))
                    coverage = half + 0.5 - distance
                    if coverage > 0.0:
                        self.blend(px, py, coverage if coverage < 1.0 else 1.0, color)

    def fill_circle(self, cx: float, cy: float, radius: float, color: tuple[int, int, int]) -> None:
        reach = int(math.ceil(radius + 1.5))
        min_x = max(0, int(math.floor(cx)) - reach)
        max_x = min(self.width - 1, int(math.ceil(cx)) + reach)
        min_y = max(0, int(math.floor(cy)) - reach)
        max_y = min(self.height - 1, int(math.ceil(cy)) + reach)
        for py in range(min_y, max_y + 1):
            dy = py + 0.5 - cy
            for px in range(min_x, max_x + 1):
                dx = px + 0.5 - cx
                coverage = radius + 0.5 - math.hypot(dx, dy)
                if coverage > 0.0:
                    self.blend(px, py, coverage if coverage < 1.0 else 1.0, color)

    def fill_rrect(
        self,
        x: float,
        y: float,
        width: float,
        height: float,
        radius_x: float,
        radius_y: float,
        color: tuple[int, int, int],
    ) -> None:
        radius = max(radius_x, radius_y)
        min_x = max(0, int(math.floor(x) - 1))
        max_x = min(self.width - 1, int(math.ceil(x + width) + 1))
        min_y = max(0, int(math.floor(y) - 1))
        max_y = min(self.height - 1, int(math.ceil(y + height) + 1))
        for py in range(min_y, max_y + 1):
            fy = py + 0.5
            for px in range(min_x, max_x + 1):
                fx = px + 0.5
                coverage = rounded_rect_coverage(fx, fy, x, y, width, height, radius)
                if coverage > 0.0:
                    self.blend(px, py, coverage, color)

    def fill_polygon(self, points: list[tuple[float, float]], color: tuple[int, int, int]) -> None:
        min_x = max(0, int(math.floor(min(p[0] for p in points)) - 1))
        max_x = min(self.width - 1, int(math.ceil(max(p[0] for p in points)) + 1))
        min_y = max(0, int(math.floor(min(p[1] for p in points)) - 1))
        max_y = min(self.height - 1, int(math.ceil(max(p[1] for p in points)) + 1))
        for py in range(min_y, max_y + 1):
            fy = py + 0.5
            crossings = []
            for index in range(len(points) - 1):
                ax, ay = points[index]
                bx, by = points[index + 1]
                if (ay <= fy < by) or (by <= fy < ay):
                    crossings.append(ax + (fy - ay) * (bx - ax) / (by - ay))
            if not crossings:
                continue
            crossings.sort()
            for pair in range(0, len(crossings) - 1, 2):
                left, right = crossings[pair], crossings[pair + 1]
                for px in range(max(min_x, int(math.floor(left))), min(max_x, int(math.ceil(right)))):
                    fx = px + 0.5
                    coverage = min(1.0, max(0.0, min(right - fx, fx - left) + 0.5))
                    if coverage > 0.0:
                        self.blend(px, py, coverage, color)


def rounded_rect_coverage(
    fx: float, fy: float, x: float, y: float, width: float, height: float, radius: float
) -> float:
    """Couverture lissée (1 px) par distance signée d'un rectangle arrondi.

    Distance signée classique : négative à l'intérieur, positive à l'extérieur ;
    la couverture vaut 0.5 - sdf, donc 1 au cœur de la forme et 0 au-delà du lissé.
    """
    center_x = x + width * 0.5
    center_y = y + height * 0.5
    qx = abs(fx - center_x) - (width * 0.5 - radius)
    qy = abs(fy - center_y) - (height * 0.5 - radius)
    outside = math.hypot(max(qx, 0.0), max(qy, 0.0))
    inside = min(max(qx, qy), 0.0)
    signed_distance = outside + inside - radius
    coverage = 0.5 - signed_distance
    return 0.0 if coverage <= 0.0 else (1.0 if coverage >= 1.0 else coverage)


def render(size: int, supersample: int, primitives: list[Primitive]) -> bytearray:
    big = size * supersample
    canvas = Canvas(big, big)
    scale = float(supersample)
    for primitive in primitives:
        kind = primitive[0]
        if kind == "stroke":
            _, points, width, color, _closed = primitive
            canvas.stroke([(x * scale, y * scale) for x, y in points], width * scale, color)
        elif kind == "fill_circle":
            _, cx, cy, radius, color = primitive
            canvas.fill_circle(cx * scale, cy * scale, radius * scale, color)
        elif kind == "fill_rrect":
            _, x, y, width, height, radius_x, radius_y, color = primitive
            canvas.fill_rrect(
                x * scale, y * scale, width * scale, height * scale,
                radius_x * scale, radius_y * scale, color,
            )
        elif kind == "fill_polygon":
            _, points, color = primitive
            canvas.fill_polygon([(x * scale, y * scale) for x, y in points], color)
        else:
            raise SvgError(f"primitive inconnue : {kind}")
    if supersample == 1:
        return canvas.pixels
    # Filtre boîte : moyenne des supersample x supersample pixels.
    out = bytearray(size * size * 4)
    divisor = supersample * supersample
    for oy in range(size):
        for ox in range(size):
            totals = [0, 0, 0, 0]
            for sy in range(supersample):
                row_base = ((oy * supersample + sy) * big + ox * supersample) * 4
                for sx in range(supersample):
                    base = row_base + sx * 4
                    for channel in range(4):
                        totals[channel] += canvas.pixels[base + channel]
            target = (oy * size + ox) * 4
            for channel in range(4):
                out[target + channel] = int(round(totals[channel] / divisor))
    return out


def supersample_for(size: int) -> int:
    return 4 if size <= 64 else (2 if size <= 128 else 1)


# --------------------------------------------------------------------------- #
# Écriture PNG / ICO
# --------------------------------------------------------------------------- #


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + kind
        + payload
        + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
    )


def write_png(path: Path, size: int, rgba: bytes) -> None:
    raw = bytearray()
    for y in range(size):
        raw.append(0)  # filtre « none »
        raw += rgba[y * size * 4 : (y + 1) * size * 4]
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    data = (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", header)
        + png_chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + png_chunk(b"IEND", b"")
    )
    path.write_bytes(data)


def read_png_dimensions(payload: bytes) -> tuple[int, int]:
    if payload[:8] != b"\x89PNG\r\n\x1a\n":
        raise SvgError("entrée PNG invalide (signature)")
    width, height = struct.unpack(">II", payload[16:24])
    return width, height


def dib_entry(size: int, rgba: bytes) -> bytes:
    """BITMAPINFOHEADER + pixels BGRA (haut en bas) + masque AND 1 bpp."""
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor_rows = bytearray()
    for y in range(size - 1, -1, -1):
        row = rgba[y * size * 4 : (y + 1) * size * 4]
        for x in range(size):
            base = x * 4
            xor_rows += bytes((row[base + 2], row[base + 1], row[base], row[base + 3]))
    mask_row_bytes = ((size + 31) // 32) * 4
    and_rows = bytearray()
    for y in range(size - 1, -1, -1):
        row = bytearray(mask_row_bytes)
        for x in range(size):
            alpha = rgba[(y * size + x) * 4 + 3]
            if alpha < 128:
                row[x // 8] |= 0x80 >> (x % 8)
        and_rows += row
    return header + bytes(xor_rows) + bytes(and_rows)


def write_ico(path: Path, entries: list[tuple[int, bytes, bool]]) -> None:
    """entries : liste (taille, données image, est_png)."""
    count = len(entries)
    header = struct.pack("<HHH", 0, 1, count)
    directory = bytearray()
    payload = bytearray()
    offset = 6 + 16 * count
    for size, data, is_png in entries:
        width_byte = 0 if size >= 256 else size
        height_byte = width_byte
        planes, bit_count = (1, 32)
        directory += struct.pack(
            "<BBBBHHII", width_byte, height_byte, 0, 0, planes, bit_count, len(data), offset
        )
        payload += data
        offset += len(data)
    path.write_bytes(header + bytes(directory) + bytes(payload))


def verify_ico(path: Path) -> list[str]:
    payload = path.read_bytes()
    if len(payload) < 6:
        raise SvgError("ICO trop court")
    reserved, kind, count = struct.unpack("<HHH", payload[:6])
    if reserved != 0 or kind != 1:
        raise SvgError(f"en-tête ICO invalide : reserved={reserved} type={kind}")
    lines = [f"{path.name}: type=ICO entrees={count}"]
    seen: list[int] = []
    for index in range(count):
        base = 6 + 16 * index
        width_byte, height_byte, _palette, _reserved, planes, bit_count, size, offset = struct.unpack(
            "<BBBBHHII", payload[base : base + 16]
        )
        width = 256 if width_byte == 0 else width_byte
        height = 256 if height_byte == 0 else height_byte
        if width != height:
            raise SvgError(f"entree {index}: non carree {width}x{height}")
        if offset + size > len(payload):
            raise SvgError(f"entree {index}: donnees annoncees hors du fichier")
        data = payload[offset : offset + size]
        if data[:8] == b"\x89PNG\r\n\x1a\n":
            png_width, png_height = read_png_dimensions(data)
            if (png_width, png_height) != (width, height):
                raise SvgError(f"entree {index}: PNG {png_width}x{png_height} != ICO {width}x{height}")
            encoding = "PNG"
        else:
            header_size, dib_width, dib_height, _planes2, dib_bits = struct.unpack(
                "<IiiHH", data[:16]
            )
            if header_size != 40:
                raise SvgError(f"entree {index}: BITMAPINFOHEADER inattendu ({header_size})")
            if dib_width != width or dib_height != height * 2:
                raise SvgError(
                    f"entree {index}: DIB {dib_width}x{dib_height} != ICO {width}x{height * 2}"
                )
            if dib_bits != 32:
                raise SvgError(f"entree {index}: profondeur {dib_bits} != 32")
            expected = 40 + width * height * 4 + ((width + 31) // 32) * 4 * height
            if size != expected:
                raise SvgError(f"entree {index}: taille {size} != attendue {expected}")
            encoding = "DIB32"
        if bit_count != 32 or planes != 1:
            raise SvgError(f"entree {index}: planes={planes} bits={bit_count}")
        seen.append(width)
        lines.append(f"  entree {index}: {width}x{height} {encoding} bits={bit_count} octets={size} offset={offset}")
    for required in ICON_SIZES:
        if required not in seen:
            raise SvgError(f"ICO incomplet : taille {required} absente")
    return lines


def verify_png(path: Path, expected: int) -> str:
    payload = path.read_bytes()
    width, height = read_png_dimensions(payload)
    if width != expected or height != expected:
        raise SvgError(f"{path.name}: {width}x{height} != {expected}x{expected}")
    bit_depth, color_type = payload[24], payload[25]
    if bit_depth != 8 or color_type != 6:
        raise SvgError(f"{path.name}: profondeur={bit_depth} type={color_type} (attendu 8/6 RGBA)")
    digest = hashlib.sha256(payload).hexdigest()[:16]
    return f"{path.name}: {width}x{height} RGBA8 sha256={digest}"


# --------------------------------------------------------------------------- #
# Génération du RGBA embarqué pour SDL3
# --------------------------------------------------------------------------- #

EMBED_SIZE = 64
HEADER_PATH = REPO / "app" / "desktop_ui" / "design_system" / "window_icon_data.hpp"
SOURCE_PATH = REPO / "app" / "desktop_ui" / "design_system" / "window_icon_data.cpp"


def write_embedded(rgba: bytes, size: int) -> tuple[str, str]:
    header = f"""#pragma once
// DevLink — app/desktop_ui/design_system/window_icon_data.hpp
// Fichier GÉNÉRÉ par tools/branding/export_icons.py depuis
// assets/branding/devlink-icon-master.svg (variante compacte, tuile incluse).
// Ne pas éditer à la main : régénérer avec l'outil, puis vérifier le rendu.
// Sert à SDL3_SetWindowIcon sur les plateformes sans icône PE (Linux/macOS) ;
// sous Windows l'icône PE du binaire prime, ce tableau reste utilisé comme repli.
#include <cstddef>
#include <cstdint>

namespace devlink::ui::window_icon {{

inline constexpr int kWidth = {size};
inline constexpr int kHeight = {size};
inline constexpr std::size_t kBytes = static_cast<std::size_t>({size}) * {size} * 4;

// RGBA8, ligne par ligne, origine en haut à gauche.
extern const std::uint8_t kPixelData[kBytes];

}} // namespace devlink::ui::window_icon
"""
    lines = [
        "// DevLink — app/desktop_ui/design_system/window_icon_data.cpp",
        "// Fichier GÉNÉRÉ (tools/branding/export_icons.py) : ne pas éditer à la main.",
        '#include "window_icon_data.hpp"',
        "",
        "namespace devlink::ui::window_icon {",
        "",
        "const std::uint8_t kPixelData[kBytes] = {",
    ]
    row: list[str] = []
    for index, byte in enumerate(rgba):
        row.append(f"0x{byte:02x},")
        if len(row) == 16:
            lines.append("    " + "".join(row))
            row = []
    if row:
        lines.append("    " + "".join(row))
    lines += ["};", "", "} // namespace devlink::ui::window_icon", ""]
    return header, "\n".join(lines)


# --------------------------------------------------------------------------- #
# Pilotage
# --------------------------------------------------------------------------- #


def build_all(write: bool) -> list[str]:
    tree = ET.parse(MASTER)
    report: list[str] = []
    renderer_cache: dict[int, list[Primitive]] = {}

    def primitives_for(size: int) -> list[Primitive]:
        if size not in renderer_cache:
            symbol = "dl-compact-tile" if size in COMPACT_SIZES else "dl-standard-tile"
            renderer_cache[size] = Renderer(tree.getroot(), size).render_symbol(symbol)
        return renderer_cache[size]

    rasters: dict[int, bytes] = {}
    for size in ICON_SIZES:
        supersample = supersample_for(size)
        pixels = render(size, supersample, primitives_for(size))
        rasters[size] = bytes(pixels)
        target = BRAND / f"devlink-{size}.png"
        if write:
            write_png(target, size, pixels)
            report.append(verify_png(target, size))
        else:
            report.append(f"devlink-{size}.png: rendu {size}x{size} (supersampling x{supersample}) non écrit")

    entries: list[tuple[int, bytes, bool]] = []
    for size in ICON_SIZES:
        if size == 256:
            entries.append((size, png_bytes(256, rasters[256]), True))
        else:
            entries.append((size, dib_entry(size, rasters[size]), False))
    ico_path = BRAND / "devlink.ico"
    if write:
        write_ico(ico_path, entries)
    report.extend(verify_ico(ico_path) if write else [f"devlink.ico: {len(entries)} entrees preparees (non ecrit)"])

    header, source = write_embedded(rasters[EMBED_SIZE], EMBED_SIZE)
    if write:
        HEADER_PATH.write_text(header, encoding="utf-8")
        SOURCE_PATH.write_text(source, encoding="utf-8")
    report.append(
        f"window_icon_data.{{hpp,cpp}}: RGBA {EMBED_SIZE}x{EMBED_SIZE} embarqué "
        f"({EMBED_SIZE * EMBED_SIZE * 4} octets) {'écrit' if write else 'non écrit'}"
    )
    return report


def png_bytes(size: int, rgba: bytes) -> bytes:
    raw = bytearray()
    for y in range(size):
        raw.append(0)
        raw += rgba[y * size * 4 : (y + 1) * size * 4]
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", header)
        + png_chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + png_chunk(b"IEND", b"")
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", help="écrit puis relit et vérifie")
    parser.add_argument("--check", action="store_true", help="vérifie les fichiers existants sans écrire")
    options = parser.parse_args()

    if options.check:
        failures = 0
        for size in ICON_SIZES:
            path = BRAND / f"devlink-{size}.png"
            try:
                print(verify_png(path, size))
            except (SvgError, OSError) as error:
                print(f"ECHEC {path.name}: {error}")
                failures += 1
        try:
            for line in verify_ico(BRAND / "devlink.ico"):
                print(line)
        except (SvgError, OSError) as error:
            print(f"ECHEC devlink.ico: {error}")
            failures += 1
        if not HEADER_PATH.exists() or not SOURCE_PATH.exists():
            print("ECHEC window_icon_data manquant")
            failures += 1
        else:
            print(f"window_icon_data.hpp: {HEADER_PATH.stat().st_size} octets")
            print(f"window_icon_data.cpp: {SOURCE_PATH.stat().st_size} octets")
        print("VERIFICATION: " + ("OK" if failures == 0 else f"{failures} ECHEC(S)"))
        return 1 if failures else 0

    for line in build_all(write=True):
        print(line)
    if options.verify:
        failures = 0
        for size in ICON_SIZES:
            try:
                verify_png(BRAND / f"devlink-{size}.png", size)
            except SvgError as error:
                print(f"ECHEC PNG {size}: {error}")
                failures += 1
        try:
            verify_ico(BRAND / "devlink.ico")
        except SvgError as error:
            print(f"ECHEC ICO: {error}")
            failures += 1
        print("VERIFICATION: " + ("OK" if failures == 0 else f"{failures} ECHEC(S)"))
        return 1 if failures else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
