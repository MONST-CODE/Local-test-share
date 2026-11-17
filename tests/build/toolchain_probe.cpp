// Native compile/link/runtime and manifest probe. This does not orchestrate builds.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
#ifndef __clang__
#error LLVM clang++ is required
#endif
#if !defined(__x86_64__) && !defined(_M_X64)
#error x86-64 is required
#endif
#ifdef _WIN32
#include <_mingw.h>
#include <windows.h>
#include <bcrypt.h>
#if !defined(__MINGW64__) || !defined(_UCRT)
#error Windows requires MinGW-w64 UCRT headers from MSYS2 UCRT64
#endif
#if defined(_MSC_VER) || defined(_MSVC_STL_VERSION) || defined(_LIBCPP_VERSION) || !defined(__GLIBCXX__)
#error Windows requires the GNU ABI and libstdc++, not MSVC or CLANG64 libc++
#endif
namespace {
int manifest(const char* path) {
    const HMODULE module = LoadLibraryExA(path, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) { std::fprintf(stderr, "manifest: cannot load PE resource, error=%lu\n", GetLastError()); return 1; }
    const HRSRC resource = FindResourceA(module, MAKEINTRESOURCEA(1), MAKEINTRESOURCEA(24));
    const HGLOBAL data = resource ? LoadResource(module, resource) : nullptr;
    const char* bytes = data ? static_cast<const char*>(LockResource(data)) : nullptr;
    const DWORD size = resource ? SizeofResource(module, resource) : 0;
    const std::string_view xml(bytes ? bytes : "", bytes ? size : 0);
    const bool valid = xml.find("asInvoker") != std::string_view::npos && xml.find("PerMonitorV2") != std::string_view::npos;
    if (valid) std::printf("manifest: PASS resource #1 asInvoker + PerMonitorV2 %s\n", path);
    else std::fprintf(stderr, "manifest: missing execution-level/DPI resource %s\n", path);
    FreeLibrary(module);
    return valid ? 0 : 1;
}
bool runtime(const char* name, const std::filesystem::path& prefix) {
    const HMODULE module = GetModuleHandleA(name);
    if (!module) { std::fprintf(stderr, "runtime: required loaded module absent: %s\n", name); return false; }
    char path[32768]{};
    const DWORD size = GetModuleFileNameA(module, path, sizeof(path));
    if (size == 0 || size >= sizeof(path)) return false;
    std::error_code error;
    const auto actual = std::filesystem::canonical(path, error);
    if (error) return false;
    if (!prefix.empty() && actual.parent_path() != prefix) {
        std::fprintf(stderr, "runtime: %s loaded outside selected UCRT64 bin: %s\n", name, path);
        return false;
    }
    std::printf("runtime: %s=%s\n", name, path);
    return true;
}
}
#endif
static_assert(__cplusplus >= 202002L);
static_assert(sizeof(void*) == 8);

// ---------------------------------------------------------------------------
// Inspection REELLE des artefacts de marque : ICO multi-tailles et ressources
// PE. Pur C++ portable (aucune API Win32) : les memes controles s'executent
// sur Linux (auto-test) et sur Windows (barrieres du build applicatif).
// ---------------------------------------------------------------------------
namespace probe {

std::vector<unsigned char> read_file(const char* path, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        *error = std::string("impossible d'ouvrir ") + path;
        return {};
    }
    return std::vector<unsigned char>(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

unsigned le16(const std::vector<unsigned char>& b, std::size_t o) {
    return static_cast<unsigned>(b[o]) | (static_cast<unsigned>(b[o + 1]) << 8);
}
unsigned long long le32(const std::vector<unsigned char>& b, std::size_t o) {
    return static_cast<unsigned long long>(b[o]) |
        (static_cast<unsigned long long>(b[o + 1]) << 8) |
        (static_cast<unsigned long long>(b[o + 2]) << 16) |
        (static_cast<unsigned long long>(b[o + 3]) << 24);
}
unsigned long long be32(const std::vector<unsigned char>& b, std::size_t o) {
    return (static_cast<unsigned long long>(b[o]) << 24) |
        (static_cast<unsigned long long>(b[o + 1]) << 16) |
        (static_cast<unsigned long long>(b[o + 2]) << 8) |
        static_cast<unsigned long long>(b[o + 3]);
}

// Tailles exigees par le contrat de marque (exporteur tools/branding) :
// 16/24/32/48/64/128 en DIB 32 bits, 256 en PNG RGBA8 (format Vista+).
constexpr int kRequiredIcoSizes[] = {16, 24, 32, 48, 64, 128, 256};
constexpr unsigned char kPngSignature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

int inspect_ico(const char* path, int expect_entries) {
    std::string error;
    const auto bytes = read_file(path, &error);
    if (!error.empty()) { std::fprintf(stderr, "ico: %s\n", error.c_str()); return 1; }
    if (bytes.size() < 22) {
        std::fprintf(stderr, "ico: fichier trop petit pour un ICONDIR (%zu octets) : %s\n", bytes.size(), path);
        return 1;
    }
    if (le16(bytes, 0) != 0 || le16(bytes, 2) != 1) {
        std::fprintf(stderr, "ico: signature ICONDIR invalide (reserved/type) : fichier renomme ? %s\n", path);
        return 1;
    }
    const unsigned count = le16(bytes, 4);
    if (count == 0) { std::fprintf(stderr, "ico: ICONDIR declare 0 entree : %s\n", path); return 1; }
    if (expect_entries > 0 && count != static_cast<unsigned>(expect_entries)) {
        std::fprintf(stderr, "ico: %u entrees != %d attendues : %s\n", count, expect_entries, path);
        return 1;
    }
    if (bytes.size() < 6 + static_cast<std::size_t>(count) * 16) {
        std::fprintf(stderr, "ico: repertoire tronque pour %u entrees\n", count);
        return 1;
    }
    bool present[257] = {};
    unsigned long long payload_end = 0;
    for (unsigned i = 0; i < count; ++i) {
        const std::size_t e = 6 + static_cast<std::size_t>(i) * 16;
        const unsigned w = bytes[e] == 0 ? 256 : bytes[e];
        const unsigned h = bytes[e + 1] == 0 ? 256 : bytes[e + 1];
        const unsigned colors = bytes[e + 2];
        const unsigned reserved = bytes[e + 3];
        const unsigned planes = le16(bytes, e + 4);
        const unsigned bits = le16(bytes, e + 6);
        const unsigned long long size = le32(bytes, e + 8);
        const unsigned long long offset = le32(bytes, e + 12);
        if (w != h) { std::fprintf(stderr, "ico: entree %u non carree (%ux%u)\n", i, w, h); return 1; }
        if (reserved != 0 || colors != 0) {
            std::fprintf(stderr, "ico: entree %u : reserved=%u colorCount=%u (32 bpp impose 0/0)\n", i, reserved, colors);
            return 1;
        }
        if (planes != 1 || bits != 32) {
            std::fprintf(stderr, "ico: entree %u : planes=%u bits=%u (exige 1/32)\n", i, planes, bits);
            return 1;
        }
        if (size == 0 || offset + size > bytes.size()) {
            std::fprintf(stderr, "ico: entree %u : charge utile [%llu..%llu) hors fichier (%zu)\n",
                i, offset, offset + size, bytes.size());
            return 1;
        }
        if (offset + size > payload_end) payload_end = offset + size;
        present[w] = true;
        const std::size_t p = static_cast<std::size_t>(offset);
        if (size >= 33 && std::memcmp(&bytes[p], kPngSignature, 8) == 0) {
            if (be32(bytes, p + 8) != 13 || std::memcmp(&bytes[p + 12], "IHDR", 4) != 0) {
                std::fprintf(stderr, "ico: entree %u : IHDR PNG invalide\n", i);
                return 1;
            }
            const unsigned pw = static_cast<unsigned>(be32(bytes, p + 16));
            const unsigned ph = static_cast<unsigned>(be32(bytes, p + 20));
            const unsigned depth = bytes[p + 24];
            const unsigned color_type = bytes[p + 25];
            if (pw != w || ph != h) {
                std::fprintf(stderr, "ico: entree %u : IHDR %ux%u contredit le repertoire %ux%u\n", i, pw, ph, w, h);
                return 1;
            }
            if (depth != 8 || color_type != 6) {
                std::fprintf(stderr, "ico: entree %u : PNG depth=%u colorType=%u (exige RGBA8 8/6)\n", i, depth, color_type);
                return 1;
            }
            std::printf("ico: entree %u : %ux%u PNG RGBA8 %llu octets\n", i, w, h, size);
        } else {
            if (size < 40 || le32(bytes, p) != 40) {
                std::fprintf(stderr, "ico: entree %u : charge utile ni PNG ni BITMAPINFOHEADER(40)\n", i);
                return 1;
            }
            const long long dib_w = static_cast<long long>(le32(bytes, p + 4));
            const long long dib_h = static_cast<long long>(le32(bytes, p + 8));
            const unsigned dib_planes = le16(bytes, p + 12);
            const unsigned dib_bits = le16(bytes, p + 14);
            const unsigned long long compression = le32(bytes, p + 16);
            if (dib_w != static_cast<long long>(w) || dib_h != 2ll * static_cast<long long>(h)) {
                std::fprintf(stderr, "ico: entree %u : DIB %lldx%lld (hauteur = 2x pour masque AND) contredit %ux%u\n",
                    i, dib_w, dib_h, w, h);
                return 1;
            }
            if (dib_planes != 1 || dib_bits != 32 || compression != 0) {
                std::fprintf(stderr, "ico: entree %u : DIB planes=%u bits=%u compression=%llu (exige 1/32/BI_RGB)\n",
                    i, dib_planes, dib_bits, compression);
                return 1;
            }
            const unsigned long long exact = 40ull + static_cast<unsigned long long>(w) * h * 4 +
                ((w + 31) / 32) * 4ull * h;
            if (size != exact) {
                std::fprintf(stderr, "ico: entree %u : DIB %llu octets != taille exacte %llu (aucun bourrage tolere)\n",
                    i, size, exact);
                return 1;
            }
            std::printf("ico: entree %u : %ux%u DIB32 BI_RGB %llu octets\n", i, w, h, size);
        }
    }
    if (payload_end != bytes.size()) {
        std::fprintf(stderr, "ico: charges utiles finissent a %llu mais le fichier fait %zu octets : donnees fantomes ou manquantes\n",
            payload_end, bytes.size());
        return 1;
    }
    for (const int required : kRequiredIcoSizes) {
        if (!present[required]) {
            std::fprintf(stderr, "ico: taille Windows requise %d absente\n", required);
            return 1;
        }
    }
    std::printf("ico: %u entrees verifiees octet par octet (16 24 32 48 64 128 DIB32 + 256 PNG RGBA8), all required Windows sizes present: %s\n",
        count, path);
    return 0;
}

namespace {
struct PeSection { unsigned long long va = 0, vsize = 0, raw = 0, rawsize = 0; };

// Lecteur PE minimal, pur C++ : en-tetes, sections, table de ressources.
struct PeReader {
    std::vector<unsigned char> bytes;
    std::vector<PeSection> sections;
    long long res_base = -1;              // offset fichier de la table de ressources
    unsigned long long res_rva = 0;
    unsigned long long res_size = 0;

    long long rva_to_offset(unsigned long long rva) const {
        for (const auto& section : sections) {
            const unsigned long long span = section.vsize > section.rawsize ? section.vsize : section.rawsize;
            if (rva >= section.va && rva < section.va + span) {
                const unsigned long long off = section.raw + (rva - section.va);
                return off < bytes.size() ? static_cast<long long>(off) : -1;
            }
        }
        return -1;
    }
};

bool load_pe(const char* path, PeReader* pe, std::string* diagnostics) {
    std::string error;
    pe->bytes = read_file(path, &error);
    if (!error.empty()) { *diagnostics = "pe: " + error; return false; }
    const auto& bytes = pe->bytes;
    if (bytes.size() < 64 || bytes[0] != 'M' || bytes[1] != 'Z') {
        *diagnostics = std::string("pe: image MZ invalide : ") + path;
        return false;
    }
    const unsigned long long lfanew = le32(bytes, 0x3C);
    if (lfanew + 4 + 20 + 2 > bytes.size() || std::memcmp(&bytes[lfanew], "PE\0\0", 4) != 0) {
        *diagnostics = "pe: signature PE introuvable a e_lfanew";
        return false;
    }
    const std::size_t coff = static_cast<std::size_t>(lfanew) + 4;
    if (le16(bytes, coff) != 0x8664) {
        *diagnostics = "pe: machine != x86-64 (8664)";
        return false;
    }
    const unsigned section_count = le16(bytes, coff + 2);
    const unsigned opt_size = le16(bytes, coff + 16);
    const std::size_t opt = coff + 20;
    if (opt + opt_size > bytes.size() || section_count == 0 || section_count > 96) {
        *diagnostics = "pe: en-tetes COFF/optionnel incoherents";
        return false;
    }
    const unsigned magic = le16(bytes, opt);
    std::size_t dir_base = 0;
    if (magic == 0x20B) dir_base = opt + 112;        // PE32+
    else if (magic == 0x10B) dir_base = opt + 96;    // PE32
    if (dir_base == 0 || dir_base + 16 * 8 > opt + opt_size) {
        *diagnostics = "pe: en-tete optionnel non reconnu ou trop court";
        return false;
    }
    pe->res_rva = le32(bytes, dir_base + 2 * 8);
    pe->res_size = le32(bytes, dir_base + 2 * 8 + 4);
    if (pe->res_rva == 0 || pe->res_size == 0) {
        *diagnostics = std::string("pe: aucune table de ressources : le binaire NE PORTE PAS icone/manifeste/version : ") + path;
        return false;
    }
    pe->sections.resize(section_count);
    const std::size_t table = opt + opt_size;
    for (unsigned i = 0; i < section_count; ++i) {
        const std::size_t s = table + static_cast<std::size_t>(i) * 40;
        if (s + 40 > bytes.size()) { *diagnostics = "pe: table de sections tronquee"; return false; }
        pe->sections[i].vsize = le32(bytes, s + 8);
        pe->sections[i].va = le32(bytes, s + 12);
        pe->sections[i].rawsize = le32(bytes, s + 16);
        pe->sections[i].raw = le32(bytes, s + 20);
    }
    pe->res_base = pe->rva_to_offset(pe->res_rva);
    if (pe->res_base < 0 || static_cast<unsigned long long>(pe->res_base) + pe->res_size > bytes.size()) {
        *diagnostics = "pe: RVA ressources non mappable dans le fichier";
        return false;
    }
    return true;
}

struct ResourcePayload { unsigned long long offset = 0; unsigned long long size = 0; };

// Parcours iteratif borne de l'arbre de ressources (type/identifiant/langue).
// Toute charge utile doit etre integralement mappable, sinon echec : un PE aux
// ressources tronquees ne peut pas « passer quand meme ».
bool walk_resources(const PeReader& pe, std::vector<std::pair<unsigned, ResourcePayload>>* found,
    std::string* diagnostics) {
    struct Frame { unsigned type; unsigned long long dir; unsigned level; };
    // dir est RELATIF à la base de la table de ressources (racine = 0).
    std::vector<Frame> stack{{0, 0, 1}};
    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();
        if (frame.level > 3 || frame.dir >= pe.res_size) { *diagnostics = "pe: arbre de ressources corrompu"; return false; }
        const long long base = pe.res_base + static_cast<long long>(frame.dir);
        if (base + 16 > static_cast<long long>(pe.bytes.size())) { *diagnostics = "pe: repertoire de ressources tronque"; return false; }
        const std::size_t dir_off = static_cast<std::size_t>(base);
        const unsigned named = le16(pe.bytes, dir_off + 12);
        const unsigned ids = le16(pe.bytes, dir_off + 14);
        const unsigned total = named + ids;
        if (total == 0 || total > 4096) { *diagnostics = "pe: repertoire de ressources suspect"; return false; }
        for (unsigned i = 0; i < total; ++i) {
            const std::size_t entry = dir_off + 16 + static_cast<std::size_t>(i) * 8;
            if (entry + 8 > pe.bytes.size()) { *diagnostics = "pe: entree de ressources tronquee"; return false; }
            const unsigned long long name = le32(pe.bytes, entry);
            const unsigned long long data = le32(pe.bytes, entry + 4);
            if ((name & 0x80000000ull) != 0) continue; // entrees nommees : hors icone/manifeste/version
            if ((data & 0x80000000ull) != 0) {
                stack.push_back({frame.level == 1 ? static_cast<unsigned>(name) : frame.type,
                    data & 0x7FFFFFFFull, frame.level + 1});
                continue;
            }
            if (frame.level != 3) { *diagnostics = "pe: donnee hors niveau langue : arbre invalide"; return false; }
            const long long leaf = pe.res_base + static_cast<long long>(data);
            if (leaf + 16 > static_cast<long long>(pe.bytes.size())) { *diagnostics = "pe: feuille de ressources tronquee"; return false; }
            const unsigned long long payload_rva = le32(pe.bytes, static_cast<std::size_t>(leaf));
            ResourcePayload payload;
            payload.size = le32(pe.bytes, static_cast<std::size_t>(leaf) + 4);
            const long long payload_off = pe.rva_to_offset(payload_rva);
            if (payload_off < 0 || payload.size == 0 ||
                static_cast<unsigned long long>(payload_off) + payload.size > pe.bytes.size()) {
                *diagnostics = "pe: charge utile de ressources hors fichier";
                return false;
            }
            payload.offset = static_cast<unsigned long long>(payload_off);
            found->push_back({frame.type, payload});
        }
    }
    return true;
}
} // namespace

int inspect_pe_icons(const char* path, int expect_icons) {
    PeReader pe;
    std::string diagnostics;
    if (!load_pe(path, &pe, &diagnostics)) { std::fprintf(stderr, "%s\n", diagnostics.c_str()); return 1; }
    std::vector<std::pair<unsigned, ResourcePayload>> resources;
    if (!walk_resources(pe, &resources, &diagnostics)) { std::fprintf(stderr, "%s\n", diagnostics.c_str()); return 1; }
    unsigned group_icons = 0, icons = 0, manifests = 0, versions = 0;
    ResourcePayload group_payload{};
    for (const auto& [type, payload] : resources) {
        if (type == 3) ++icons;              // RT_ICON
        else if (type == 14) { ++group_icons; group_payload = payload; } // RT_GROUP_ICON
        else if (type == 16) ++versions;     // RT_VERSION
        else if (type == 24) ++manifests;    // RT_MANIFEST
    }
    std::printf("pe: ressources %s : RT_GROUP_ICON=%u RT_ICON=%u RT_MANIFEST=%u RT_VERSION=%u (%zu charges utiles mappables)\n",
        path, group_icons, icons, manifests, versions, resources.size());
    if (group_icons != 1) {
        std::fprintf(stderr, "pe: RT_GROUP_ICON=%u (exige exactement 1)\n", group_icons);
        return 1;
    }
    // Le groupe doit REELLEMENT referencer les icones : GRPICONDIR type 1, compte
    // coherent avec les RT_ICON presents (chaque entree = 14 octets + id u16).
    if (group_payload.size < 6 || le16(pe.bytes, static_cast<std::size_t>(group_payload.offset)) != 0 ||
        le16(pe.bytes, static_cast<std::size_t>(group_payload.offset) + 2) != 1) {
        std::fprintf(stderr, "pe: RT_GROUP_ICON sans signature GRPICONDIR (reserved=0 type=1)\n");
        return 1;
    }
    const unsigned grouped = le16(pe.bytes, static_cast<std::size_t>(group_payload.offset) + 4);
    if (grouped == 0 || grouped != icons || group_payload.size != 6ull + 14ull * grouped) {
        std::fprintf(stderr, "pe: GRPICONDIR declare %u entrees (taille %llu) mais %u RT_ICON : groupe incoherent\n",
            grouped, group_payload.size, icons);
        return 1;
    }
    if (icons == 0) { std::fprintf(stderr, "pe: aucun RT_ICON : icone absente du binaire\n"); return 1; }
    if (expect_icons > 0 && icons != static_cast<unsigned>(expect_icons)) {
        std::fprintf(stderr, "pe: RT_ICON=%u != %d attendus\n", icons, expect_icons);
        return 1;
    }
    std::printf("pe: GRPICONDIR coherent (%u entrees) + RT_MANIFEST=%u : ressources verifiees\n", grouped, manifests);
    return 0;
}

// Controle du quadruplet de version embarque (ADR-015) : VS_FIXEDFILEINFO dans
// RT_VERSION. Le link echoue si le PE diverge de engine/core/product.hpp.
int check_pe_version(const char* path, const char* expect_quad) {
    unsigned e0 = 0, e1 = 0, e2 = 0, e3 = 0;
    if (std::sscanf(expect_quad, "%u.%u.%u.%u", &e0, &e1, &e2, &e3) != 4) {
        std::fprintf(stderr, "version: quadruplet attendu invalide : %s\n", expect_quad);
        return 2;
    }
    PeReader pe;
    std::string diagnostics;
    if (!load_pe(path, &pe, &diagnostics)) { std::fprintf(stderr, "%s\n", diagnostics.c_str()); return 1; }
    std::vector<std::pair<unsigned, ResourcePayload>> resources;
    if (!walk_resources(pe, &resources, &diagnostics)) { std::fprintf(stderr, "%s\n", diagnostics.c_str()); return 1; }
    const ResourcePayload* version_payload = nullptr;
    for (const auto& [type, payload] : resources) {
        if (type == 16) { version_payload = &payload; break; } // RT_VERSION
    }
    if (version_payload == nullptr) {
        std::fprintf(stderr, "version: aucune ressource RT_VERSION dans %s\n", path);
        return 1;
    }
    const std::size_t p = static_cast<std::size_t>(version_payload->offset);
    // VS_VERSION_INFO : wLength, wValueLength, wType, cle UTF-16, alignement 4,
    // puis VS_FIXEDFILEINFO (signature 0xFEEF04BD).
    if (version_payload->size < 40 + 52) {
        std::fprintf(stderr, "version: RT_VERSION trop court (%llu octets)\n", version_payload->size);
        return 1;
    }
    if (le16(pe.bytes, p + 2) != 52) {
        std::fprintf(stderr, "version: wValueLength=%u != 52 (VS_FIXEDFILEINFO attendue)\n", le16(pe.bytes, p + 2));
        return 1;
    }
    static const unsigned char kVersionKey[32] = {
        'V', 0, 'S', 0, '_', 0, 'V', 0, 'E', 0, 'R', 0, 'S', 0, 'I', 0,
        'O', 0, 'N', 0, '_', 0, 'I', 0, 'N', 0, 'F', 0, 'O', 0, 0, 0};
    if (std::memcmp(&pe.bytes[p + 6], kVersionKey, sizeof(kVersionKey)) != 0) {
        std::fprintf(stderr, "version: cle VS_VERSION_INFO absente\n");
        return 1;
    }
    const std::size_t fixed = p + 6 + 32 + 2; // alignement DWORD apres la cle (38 -> 40)
    if (le32(pe.bytes, fixed) != 0xFEEF04BDu) {
        std::fprintf(stderr, "version: signature VS_FIXEDFILEINFO %08llx != FEEF04BD\n", le32(pe.bytes, fixed));
        return 1;
    }
    const unsigned long long file_ms = le32(pe.bytes, fixed + 8);
    const unsigned long long file_ls = le32(pe.bytes, fixed + 12);
    const unsigned long long product_ms = le32(pe.bytes, fixed + 16);
    const unsigned long long product_ls = le32(pe.bytes, fixed + 20);
    const unsigned f0 = static_cast<unsigned>(file_ms >> 16), f1 = static_cast<unsigned>(file_ms & 0xFFFF);
    const unsigned f2 = static_cast<unsigned>(file_ls >> 16), f3 = static_cast<unsigned>(file_ls & 0xFFFF);
    const unsigned p0 = static_cast<unsigned>(product_ms >> 16), p1 = static_cast<unsigned>(product_ms & 0xFFFF);
    const unsigned p2 = static_cast<unsigned>(product_ls >> 16), p3 = static_cast<unsigned>(product_ls & 0xFFFF);
    std::printf("version: %s embarque FILEVERSION %u.%u.%u.%u PRODUCTVERSION %u.%u.%u.%u (attendu %s)\n",
        path, f0, f1, f2, f3, p0, p1, p2, p3, expect_quad);
    if (f0 != e0 || f1 != e1 || f2 != e2 || f3 != e3 || p0 != e0 || p1 != e1 || p2 != e2 || p3 != e3) {
        std::fprintf(stderr, "version: le PE DIVERGE de engine/core/product.hpp (%s)\n", expect_quad);
        return 1;
    }
    return 0;
}

} // namespace probe

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--fail") == 0) {
        std::puts("intentional test failure: exit 23");
        return 23;
    }
    if (argc >= 3 && std::strcmp(argv[1], "--ico") == 0) {
        int expect = 0;
        if (argc == 5 && std::strcmp(argv[3], "--expect-entries") == 0) expect = std::atoi(argv[4]);
        else if (argc != 3) {
            std::fprintf(stderr, "usage: probe --ico <fichier.ico> [--expect-entries N]\n");
            return 2;
        }
        if (argc == 5 && expect <= 0) { std::fprintf(stderr, "ico: --expect-entries exige un entier positif\n"); return 2; }
        return probe::inspect_ico(argv[2], expect);
    }
    if (argc >= 3 && std::strcmp(argv[1], "--pe-icons") == 0) {
        int expect = 0;
        if (argc == 5 && std::strcmp(argv[3], "--expect-icons") == 0) expect = std::atoi(argv[4]);
        else if (argc != 3) {
            std::fprintf(stderr, "usage: probe --pe-icons <binaire.exe> [--expect-icons N]\n");
            return 2;
        }
        if (argc == 5 && expect <= 0) { std::fprintf(stderr, "pe: --expect-icons exige un entier positif\n"); return 2; }
        return probe::inspect_pe_icons(argv[2], expect);
    }
    if (argc >= 3 && std::strcmp(argv[1], "--version-info") == 0) {
        if (argc != 5 || std::strcmp(argv[3], "--expect-version") != 0) {
            std::fprintf(stderr, "usage: probe --version-info <binaire.exe> --expect-version A.B.C.D\n");
            return 2;
        }
        return probe::check_pe_version(argv[2], argv[4]);
    }
#ifdef _WIN32
    if ((argc == 3 || argc == 5) && std::strcmp(argv[1], "--manifest") == 0) {
        if (argc == 5 && std::strcmp(argv[3], "--expect-version") != 0) {
            std::fprintf(stderr, "usage: probe --manifest <binaire.exe> [--expect-version A.B.C.D]\n");
            return 2;
        }
        const int rc = manifest(argv[2]);
        if (rc != 0 || argc == 3) return rc;
        return probe::check_pe_version(argv[2], argv[4]);
    }
#endif
    std::atomic<bool> done{false};
    std::jthread thread([&] { done.store(true); });
    thread.join();
    if (!done.load()) return 1;
#ifdef _WIN32
    std::error_code canonical_error;
    const char* root = std::getenv("PLPT_MSYS2_ROOT");
    if (root == nullptr) { std::fprintf(stderr, "toolchain: PLPT_MSYS2_ROOT is required for provenance proof\n"); return 1; }
    const auto prefix = std::filesystem::canonical(std::filesystem::path(root) / "ucrt64" / "bin", canonical_error);
    if (canonical_error) { std::fprintf(stderr, "toolchain: selected UCRT64 bin is not canonicalizable\n"); return 1; }
    if (argc != 3 || std::strcmp(argv[1], "--expect-compiler") != 0) {
        std::fprintf(stderr, "toolchain: --expect-compiler <path> is required to prove the selected executable\n");
        return 1;
    }
    const auto executable = std::filesystem::canonical(argv[2], canonical_error);
    if (canonical_error || executable.parent_path() != prefix || executable.filename() != "clang++.exe") {
        std::fprintf(stderr, "toolchain: compiler is not <PLPT_MSYS2_ROOT>/ucrt64/bin/clang++.exe\n");
        return 1;
    }
    std::printf("toolchain-probe: selected compiler=%s\n", executable.string().c_str());
    unsigned char byte = 0;
    if (BCryptGenRandom(nullptr, &byte, 1, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return 1;
    std::printf("toolchain-probe: MSYS2 UCRT64, GNU ABI, clang=%s, C++=%ld, pointer=%zu, libstdc++=%d, GCC=%d, _UCRT=1\n", __clang_version__, __cplusplus, sizeof(void*), __GLIBCXX__, _GLIBCXX_RELEASE);
    if (!runtime("ucrtbase.dll", {}) || !runtime("libstdc++-6.dll", prefix) || !runtime("libwinpthread-1.dll", prefix)) return 1;
#else
#ifndef __linux__
#error Native Linux is required
#endif
    std::printf("toolchain-probe: native Linux x64, clang=%s, C++=%ld, pointer=%zu\n", __clang_version__, __cplusplus, sizeof(void*));
#ifdef __GLIBCXX__
    std::printf("libstdc++ headers: %d\n", __GLIBCXX__);
#endif
#endif
    return 0;
}
