// skinscan.h - busca skins (.dff de peds) dentro de la carpeta modloader/. C++ puro, sin dependencias de AML.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>

namespace skinscan {

struct Skin {
    std::string name;  // nombre del .dff sin extension (es el nombre que se pasa a RequestSpecialChar)
    std::string path;  // ruta completa
};

inline std::string Lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}
inline bool EndsWith(const std::string& s, const char* suf) {
    size_t n = strlen(suf);
    return s.size() >= n && Lower(s.substr(s.size() - n)) == suf;
}

static bool Rd32(FILE* f, uint32_t* v) { return fread(v, 4, 1, f) == 1; }

// Recorre los chunks de RenderWare (cabecera de 12 bytes: tipo, tamano, version).
// Un ped tiene el chunk "Skin PLG" (0x116) dentro de Clump > GeometryList > Geometry > Extension.
static bool WalkForSkin(FILE* f, long end, int depth) {
    for (;;) {
        long pos = ftell(f);
        if (pos < 0 || pos + 12 > end) return false;
        uint32_t type, size, ver;
        if (!Rd32(f, &type) || !Rd32(f, &size) || !Rd32(f, &ver)) return false;
        long next = pos + 12 + (long)size;
        if (next > end) return false;
        if (type == 0x116) return true;
        // contenedores: Clump 0x10, GeometryList 0x1A, Geometry 0x0F, Extension 0x03
        if (depth < 6 && (type == 0x10 || type == 0x1A || type == 0x0F || type == 0x03)) {
            if (WalkForSkin(f, next, depth + 1)) return true;
        }
        if (fseek(f, next, SEEK_SET) != 0) return false;
    }
}

inline bool IsPedDff(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = false;
    uint32_t first = 0;
    if (len >= 12 && Rd32(f, &first) && first == 0x10) {  // debe empezar por un Clump
        fseek(f, 0, SEEK_SET);
        ok = WalkForSkin(f, len, 0);
    }
    fclose(f);
    return ok;
}

// Recorre dir recursivamente. Una carpeta llamada "Skins" acepta cualquier .dff sin comprobar el chunk de skin.
inline void Scan(const std::string& dir, std::vector<Skin>& out, std::vector<std::string>* rejected = nullptr,
                 int depth = 0, bool forced = false) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.empty() || n[0] == '.') continue;
        std::string full = dir + "/" + n;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        std::string ln = Lower(n);
        if (S_ISDIR(st.st_mode)) {
            if (ln == "cleo" || ln == "player.img") continue;   // scripts y ropa de CJ: no son skins
            if (depth == 0 && ln == "menu") continue;           // temas del menu de ZyZ
            Scan(full, out, rejected, depth + 1, forced || ln == "skins");
        } else if (EndsWith(n, ".dff")) {
            std::string base = n.substr(0, n.size() - 4);
            if (base.empty() || base.size() > 23) continue;     // limite del nombre de modelo del juego
            if (!forced && !IsPedDff(full)) { if (rejected) rejected->push_back(full); continue; }
            bool dup = false;
            for (auto& s : out) if (Lower(s.name) == Lower(base)) { dup = true; break; }
            if (!dup) out.push_back({base, full});
        }
    }
    closedir(d);
    if (depth == 0)
        std::sort(out.begin(), out.end(), [](const Skin& a, const Skin& b) { return Lower(a.name) < Lower(b.name); });
}

}  // namespace skinscan
