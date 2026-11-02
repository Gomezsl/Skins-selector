// SkinSelectorMenu.cpp - menu ImGui tactil para SkinSelector (plugin AML separado: libSkinSelectorMenu64.so).
// Dibuja justo antes de eglSwapBuffers y habla con SkinSelector escribiendo files/SkinSelector/request.txt.
#include <mod/config.h>
#include <mod/amlmod.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#if __has_include("roboto_font.h")
  #include "roboto_font.h"     // lo genera el workflow: Roboto-Medium (de ImGui) con binary_to_compressed_c
  #define HAVE_EMBEDDED_FONT 1
#endif

MYMODCFG(net.local.vestidorsamenu, Vestidor SA Menu, 3.0.0, Tu nombre)
NEEDGAME(com.rockstargames.gtasa)

#define TAG "VestidorSAMenu"

// ---- rutas y log ---------------------------------------------------------------------------------
static const char* kFilesDirs[] = {
    "/storage/emulated/0/Android_unprotected/data/com.rockstargames.gtasa/files",
    "/storage/emulated/0/Android/data/com.rockstargames.gtasa/files",
    "/sdcard/Android/data/com.rockstargames.gtasa/files",
    "/storage/self/primary/Android/data/com.rockstargames.gtasa/files",
};
static std::string g_plugDir;   // .../files/VestidorSA

static void Log(const char* fmt, ...) {
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, TAG, "%s", buf);
    if (!g_plugDir.empty()) {
        FILE* f = fopen((g_plugDir + "/menu_log.txt").c_str(), "a");
        if (f) { fputs(buf, f); fputc('\n', f); fclose(f); }
    }
}

// ---- parche de GOT (cambia la direccion de una funcion importada por libGTASA.so) --------------
static bool PatchGot(const char* libName, const char* sym, void* repl, void** orig) {
    struct Ctx { const char* lib; uintptr_t base; const ElfW(Phdr)* ph; int n; } c = {libName, 0, nullptr, 0};
    dl_iterate_phdr([](struct dl_phdr_info* i, size_t, void* d) -> int {
        Ctx* x = (Ctx*)d;
        if (i->dlpi_name && strstr(i->dlpi_name, x->lib)) {
            x->base = i->dlpi_addr; x->ph = i->dlpi_phdr; x->n = i->dlpi_phnum;
            return 1;
        }
        return 0;
    }, &c);
    if (!c.ph) { Log("GOT: no encuentro %s en memoria", libName); return false; }

    const ElfW(Dyn)* dyn = nullptr;
    for (int i = 0; i < c.n; i++)
        if (c.ph[i].p_type == PT_DYNAMIC) dyn = (const ElfW(Dyn)*)(c.base + c.ph[i].p_vaddr);
    if (!dyn) return false;

    auto fix = [&](uintptr_t p) { return p < c.base ? p + c.base : p; };
    const ElfW(Sym)* symtab = nullptr;
    const char* strtab = nullptr;
    const ElfW(Rela)* jmprel = nullptr; size_t jmpsz = 0;
    const ElfW(Rela)* rela = nullptr;   size_t relasz = 0;
    for (; dyn->d_tag != DT_NULL; dyn++) {
        switch (dyn->d_tag) {
            case DT_SYMTAB:   symtab = (const ElfW(Sym)*)fix(dyn->d_un.d_ptr); break;
            case DT_STRTAB:   strtab = (const char*)fix(dyn->d_un.d_ptr); break;
            case DT_JMPREL:   jmprel = (const ElfW(Rela)*)fix(dyn->d_un.d_ptr); break;
            case DT_PLTRELSZ: jmpsz = dyn->d_un.d_val; break;
            case DT_RELA:     rela = (const ElfW(Rela)*)fix(dyn->d_un.d_ptr); break;
            case DT_RELASZ:   relasz = dyn->d_un.d_val; break;
            default: break;
        }
    }
    if (!symtab || !strtab) return false;

    auto scan = [&](const ElfW(Rela)* r, size_t bytes) -> bool {
        if (!r) return false;
        for (size_t k = 0; k < bytes / sizeof(ElfW(Rela)); k++) {
            unsigned type = ELF64_R_TYPE(r[k].r_info);
            unsigned si = ELF64_R_SYM(r[k].r_info);
            if ((type != 1026 && type != 1025) || si == 0) continue;   // JUMP_SLOT / GLOB_DAT (aarch64)
            if (strcmp(strtab + symtab[si].st_name, sym) != 0) continue;
            void** slot = (void**)(c.base + r[k].r_offset);
            long pg = sysconf(_SC_PAGESIZE);
            uintptr_t page = (uintptr_t)slot & ~((uintptr_t)pg - 1);
            if (mprotect((void*)page, (size_t)pg, PROT_READ | PROT_WRITE) != 0) return false;
            *orig = *slot;
            *slot = repl;
            return true;
        }
        return false;
    };
    return scan(jmprel, jmpsz) || scan(rela, relasz);
}

// ---- lista de skins y estado (los escribe el nucleo en files/VestidorSA/) ------------------------------
static std::vector<std::string> g_skinsM;
static std::string g_current = "CJ";
static std::string g_title = "VESTIDOR SA";
static bool g_remember = false;
static bool g_missions = true;        // opcion del nucleo: skin en misiones (activa por defecto)
static bool g_vehicle = false;        // opcion del nucleo: cambiar dentro del vehiculo
static std::string g_status;
static time_t g_lastRead = 0;
static std::mutex g_listMtx;

static std::string ReadFirstLine(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return "";
    char b[200] = {0};
    if (!fgets(b, sizeof b, f)) b[0] = 0;
    fclose(f);
    std::string s = b;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

static void RefreshList() {
    time_t now = time(nullptr);
    if (now - g_lastRead < 2) return;
    g_lastRead = now;
    std::vector<std::string> skins;
    FILE* f = fopen((g_plugDir + "/skins_list.txt").c_str(), "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            if (line[0] == '#') continue;
            std::string l = line;
            size_t eq = l.find(" = ");
            size_t pa = l.find("   (/");
            if (eq == std::string::npos || pa == std::string::npos || pa < eq + 3) continue;
            skins.push_back(l.substr(eq + 3, pa - eq - 3));
        }
        fclose(f);
    }
    std::string cur = ReadFirstLine(g_plugDir + "/current.txt");
    std::string status = ReadFirstLine(g_plugDir + "/status.txt");
    bool rem = false, mis = true, veh = false;
    std::string title;
    FILE* sf = fopen((g_plugDir + "/settings.txt").c_str(), "r");
    if (sf) {
        char sl[200];
        while (fgets(sl, sizeof sl, sf)) {
            if (strncmp(sl, "remember=", 9) == 0) rem = (sl[9] == '1');
            else if (strncmp(sl, "missions=", 9) == 0) mis = (sl[9] == '1');
            else if (strncmp(sl, "vehicle=", 8) == 0) veh = (sl[8] == '1');
            else if (strncmp(sl, "title=", 6) == 0) {
                title = sl + 6;
                while (!title.empty() && (title.back() == '\n' || title.back() == '\r')) title.pop_back();
            }
        }
        fclose(sf);
    }
    std::lock_guard<std::mutex> lk(g_listMtx);
    g_skinsM = skins;
    g_remember = rem;
    g_missions = mis;
    g_vehicle = veh;
    g_status = status;
    if (!title.empty()) g_title = title;
    if (!cur.empty()) g_current = cur;
}

static void Request(const std::string& name) {
    FILE* f = fopen((g_plugDir + "/request.txt").c_str(), "w");
    if (!f) { Log("no puedo escribir request.txt"); return; }
    fputs(name.c_str(), f);
    fclose(f);
    Log("pedido: %s", name.c_str());
}

// ---- entrada tactil -----------------------------------------------------------------------------
struct Rect { float x0, y0, x1, y1; };
struct TouchEv { int kind; float x, y; };   // 0 = posicion, 1 = pulsar, 2 = soltar
static std::mutex g_inMtx;
static std::vector<Rect> g_rects;
static std::vector<TouchEv> g_evs;
static bool g_cap[16];
static uintptr_t (*AND_TouchEvent_orig)(int, int, int, int);

static uintptr_t AND_TouchEvent_hook(int type, int idx, int x, int y) {
    static int logged = 0;
    if (logged < 60) { logged++; Log("touch type=%d idx=%d x=%d y=%d", type, idx, x, y); }

    // segun el log real: cada toque empieza con type=2, se mueve con type=3 y termina con type=1
    bool down = (type == 2);
    bool up = (type == 1 || type == 4);

    bool swallow = false;
    if (idx >= 0 && idx < 16) {
        std::lock_guard<std::mutex> lk(g_inMtx);
        if (down) {
            for (auto& r : g_rects)
                if (x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1) { g_cap[idx] = true; break; }
            if (g_cap[idx]) {
                static int caps = 0;
                if (caps++ < 20) Log("toque capturado por el menu: idx=%d x=%d y=%d", idx, x, y);
                g_evs.push_back({0, (float)x, (float)y}); g_evs.push_back({1, 0, 0}); swallow = true;
            }
        } else if (g_cap[idx]) {
            swallow = true;
            g_evs.push_back({0, (float)x, (float)y});
            if (up) { g_evs.push_back({2, 0, 0}); g_cap[idx] = false; }
        }
    }
    if (swallow) return 0;
    return AND_TouchEvent_orig ? AND_TouchEvent_orig(type, idx, x, y) : 0;
}

// ---- ajustes propios del menu (menu_settings.txt) -------------------------------------------------
static int g_theme = 0;                       // 0 por defecto, 1 rojo, 2 azul, 3 verde, 4 naranja
static int g_lang = 0;                        // 0 espanol, 1 ingles, 2 indonesio
static int g_size = 1;                        // 0 pequeno, 1 normal, 2 grande
static float g_btnFx = 0.012f, g_btnFy = 0.30f;   // posicion del boton: fraccion de la pantalla
static bool g_btnLock = false;                // boton bloqueado: no se puede arrastrar
static bool g_btnDragged = false;
static bool g_styleDirty = false;
static const float kSizeMul[3] = {0.85f, 1.0f, 1.2f};
static const float kFontPx = 33.0f;           // letra a 1080p con tamano "normal"

static void SaveMenuSettings() {
    FILE* f = fopen((g_plugDir + "/menu_settings.txt").c_str(), "w");
    if (!f) return;
    fprintf(f, "theme=%d\nlang=%d\nsize=%d\nbtnx=%.4f\nbtny=%.4f\nlock=%d\n", g_theme, g_lang, g_size, g_btnFx, g_btnFy, g_btnLock ? 1 : 0);
    fclose(f);
}
static void LoadMenuSettings() {
    FILE* f = fopen((g_plugDir + "/menu_settings.txt").c_str(), "r");
    if (!f) return;
    char l[120];
    while (fgets(l, sizeof l, f)) {
        int v;
        float x;
        if (sscanf(l, "theme=%d", &v) == 1 && v >= 0 && v < 5) g_theme = v;
        else if (sscanf(l, "lang=%d", &v) == 1 && v >= 0 && v < 3) g_lang = v;
        else if (sscanf(l, "size=%d", &v) == 1 && v >= 0 && v < 3) g_size = v;
        else if (sscanf(l, "lock=%d", &v) == 1) g_btnLock = (v == 1);
        else if (sscanf(l, "btnx=%f", &x) == 1 && x >= 0.0f && x <= 1.0f) g_btnFx = x;
        else if (sscanf(l, "btny=%f", &x) == 1 && x >= 0.0f && x <= 1.0f) g_btnFy = x;
    }
    fclose(f);
}

// ---- fuente: Roboto embebida (la genera el workflow) o una del sistema; si no, la de pixeles de ImGui --------
static bool g_hasTtf = false;

static ImFont* LoadFont(float px) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    ImFont* f = nullptr;
#ifdef HAVE_EMBEDDED_FONT
    f = io.Fonts->AddFontFromMemoryCompressedBase85TTF(roboto_font_compressed_data_base85, px, &cfg, io.Fonts->GetGlyphRangesDefault());
    if (f) Log("fuente: Roboto embebida (%.0f px)", px);
#endif
    if (!f) {
        static const char* paths[] = {"/system/fonts/Roboto-Regular.ttf", "/system/fonts/Roboto-Medium.ttf",
                                      "/system/fonts/NotoSans-Regular.ttf", "/system/fonts/DroidSans.ttf",
                                      "/system/fonts/Roboto[wdth,wght].ttf", nullptr};
        for (int i = 0; paths[i] && !f; i++) {
            if (access(paths[i], R_OK) != 0) continue;
            f = io.Fonts->AddFontFromFileTTF(paths[i], px, &cfg, io.Fonts->GetGlyphRangesDefault());
            if (f) Log("fuente del sistema: %s (%.0f px)", paths[i], px);
        }
    }
    g_hasTtf = (f != nullptr);
    if (!f) { f = io.Fonts->AddFontDefault(); Log("fuente: la de pixeles de ImGui (sin TTF)"); }
    return f;
}

// ---- idiomas: espanol, ingles, indonesio ---------------------------------------------------------------
enum { K_CJ, K_WORN, K_REMEMBER, K_MISSIONS, K_VEHICLE, K_LOCK, K_HINT, K_WAIT, K_TAB_SKINS, K_TAB_SET,
       K_LANG, K_THEME, K_SIZE, K_RESCAN, K_NOSKINS, K_COUNT };
static const char* kText[3][K_COUNT] = {
    {"Volver a ser CJ", "PUESTA", "Recordar última skin", "Skin en misiones", "Cambiar dentro del vehículo", "Bloquear botón",
     "Toca una opción", "Esperando: jugador %s...", "SKINS", "AJUSTES", "Idioma", "Color", "Tamaño",
     "Releer carpeta de skins", "Sin skins (revisa skins_list.txt)"},
    {"Back to CJ", "WORN", "Remember last skin", "Skin in missions", "Change inside vehicle", "Lock button",
     "Tap an option", "Waiting: player %s...", "SKINS", "SETTINGS", "Language", "Color", "Size",
     "Reload skins folder", "No skins (check skins_list.txt)"},
    {"Kembali jadi CJ", "DIPAKAI", "Ingat skin terakhir", "Skin di misi", "Ganti di dalam kendaraan", "Kunci tombol",
     "Ketuk satu opsi", "Menunggu: pemain %s...", "SKIN", "PENGATURAN", "Bahasa", "Warna", "Ukuran",
     "Muat ulang folder skin", "Tidak ada skin (cek skins_list.txt)"}};
static const char* kLangName[3] = {"Español", "English", "Indonesia"};
static const char* kSizeName[3][3] = {{"Pequeño", "Normal", "Grande"}, {"Small", "Normal", "Large"}, {"Kecil", "Normal", "Besar"}};
static const char* kThemeName[3][5] = {{"Por defecto", "Rojo", "Azul", "Verde", "Naranja"},
                                       {"Default", "Red", "Blue", "Green", "Orange"},
                                       {"Bawaan", "Merah", "Biru", "Hijau", "Oranye"}};
static const char* kStatusKey[4] = {"agachado", "peleando", "ocupado", "en vehiculo"};   // lo que escribe el nucleo
static const char* kStatusName[3][4] = {{"agachado", "peleando", "ocupado", "en vehículo"},
                                        {"crouching", "fighting", "busy", "in a vehicle"},
                                        {"jongkok", "berkelahi", "sibuk", "di kendaraan"}};

// sin TTF la fuente de pixeles no tiene acentos: se quitan
static std::string Ascii(std::string s) {
    static const char* map[][2] = {{"á", "a"}, {"é", "e"}, {"í", "i"}, {"ó", "o"}, {"ú", "u"}, {"ñ", "n"}, {"ü", "u"},
                                   {"Á", "A"}, {"É", "E"}, {"Í", "I"}, {"Ó", "O"}, {"Ú", "U"}, {"Ñ", "N"}, {"¿", ""}, {"¡", ""}};
    for (auto& m : map) {
        size_t p;
        while ((p = s.find(m[0])) != std::string::npos) s.replace(p, strlen(m[0]), m[1]);
    }
    return s;
}
static std::string Loc(const char* s) { return g_hasTtf ? std::string(s) : Ascii(s); }
static std::string Tr(int k) { return Loc(kText[g_lang][k]); }
static std::string StatusText(const std::string& st) {
    for (int i = 0; i < 4; i++) if (st == kStatusKey[i]) return Loc(kStatusName[g_lang][i]);
    return st;
}

// ---- colores: 5 temas; el color tine el panel, las filas, las pestanas y el boton ----------------------
static const ImVec4 kAccent[5] = {ImVec4(0.20f, 0.45f, 0.80f, 1.0f), ImVec4(0.88f, 0.18f, 0.18f, 1.0f),
                                  ImVec4(0.08f, 0.56f, 0.96f, 1.0f), ImVec4(0.16f, 0.74f, 0.32f, 1.0f),
                                  ImVec4(0.98f, 0.52f, 0.06f, 1.0f)};
static const float kTint[5] = {0.16f, 0.36f, 0.36f, 0.36f, 0.36f};

struct Pal { ImVec4 accent, title, bg, row, rowHover, rowActive, seg; };
static ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w);
}
static Pal MakePal() {
    const ImVec4 a = kAccent[g_theme];
    const float t = kTint[g_theme];
    Pal p;
    p.accent = a;
    p.title = Mix(a, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.55f);
    p.bg = Mix(ImVec4(0.045f, 0.047f, 0.060f, 0.94f), a, t * 0.55f);
    p.row = Mix(ImVec4(0.125f, 0.125f, 0.145f, 0.96f), a, t);
    p.rowHover = Mix(p.row, a, 0.35f);
    p.rowActive = Mix(p.row, a, 0.70f);
    p.seg = Mix(ImVec4(0.09f, 0.09f, 0.11f, 1.0f), a, t * 0.8f);
    return p;
}

// ---- interfaz ------------------------------------------------------------------------------------
static bool g_open = false;
static int g_page = 0;
static int g_screen = 0;          // 0 = skins, 1 = ajustes
static void* (*FindPlayerPed)(int);
static std::vector<Rect> g_frameRects;

struct Item {
    std::string label, right;
    ImVec4 col;
    int action;
    std::string arg;
    int sw = -1;            // >= 0: interruptor (0 apagado, 1 encendido)
    bool chip = false;      // texto de la derecha dentro de una pastilla del color del tema
    bool selected = false;  // fila resaltada (skin puesta)
};
static const ImVec4 kWhite(1.0f, 1.0f, 1.0f, 1.0f);
static const ImVec4 kRed(1.00f, 0.50f, 0.46f, 1.0f);
static const ImVec4 kGrey(0.74f, 0.75f, 0.80f, 1.0f);

static void AddRect() {
    ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    float mg = 12.0f;
    g_frameRects.push_back({p.x - mg, p.y - mg, p.x + s.x + mg, p.y + s.y + mg});
}

// fila: texto a la izquierda; a la derecha un interruptor, una pastilla o un valor con ">"
static bool Row(const Item& it, float rh, float u, const Pal& pal) {
    ImGui::PushStyleColor(ImGuiCol_Text, it.col);
    if (it.selected) ImGui::PushStyleColor(ImGuiCol_Button, pal.rowActive);
    bool clicked = ImGui::Button(it.label.c_str(), ImVec2(-1, rh));
    if (it.selected) ImGui::PopStyleColor();
    ImGui::PopStyleColor();
    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float midY = (mn.y + mx.y) * 0.5f;
    if (it.sw >= 0) {                                    // interruptor
        const float w = 82.0f * u, h = 42.0f * u;
        const float x = mx.x - w - 18.0f * u, y = midY - h * 0.5f;
        const bool on = (it.sw == 1);
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), on ? ImGui::GetColorU32(pal.accent) : IM_COL32(74, 76, 88, 255), h * 0.5f);
        dl->AddCircleFilled(ImVec2(on ? x + w - h * 0.5f : x + h * 0.5f, midY), h * 0.5f - 5.0f * u, IM_COL32(255, 255, 255, 255), 24);
    } else if (it.chip && !it.right.empty()) {          // pastilla (skin puesta)
        ImVec2 ts = ImGui::CalcTextSize(it.right.c_str());
        const float padx = 16.0f * u, h = ts.y + 14.0f * u;
        const float x1 = mx.x - 16.0f * u, x0 = x1 - ts.x - padx * 2.0f;
        dl->AddRectFilled(ImVec2(x0, midY - h * 0.5f), ImVec2(x1, midY + h * 0.5f), ImGui::GetColorU32(pal.accent), h * 0.5f);
        dl->AddText(ImVec2(x0 + padx, midY - ts.y * 0.5f), IM_COL32(255, 255, 255, 255), it.right.c_str());
    } else if (!it.right.empty()) {                      // valor + ">"
        std::string t = it.right + "  >";
        ImVec2 ts = ImGui::CalcTextSize(t.c_str());
        dl->AddText(ImVec2(mx.x - ts.x - 18.0f * u, midY - ts.y * 0.5f), ImGui::GetColorU32(kGrey), t.c_str());
    }
    return clicked;
}

// fila con 5 cuadros de color para elegir el tema
static void ColorRow(const Item& it, float rh, float u, const Pal& pal) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(w, rh));
    ImVec2 after = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rh), ImGui::GetColorU32(pal.row), 14.0f * u);
    dl->AddText(ImVec2(p.x + 18.0f * u, p.y + (rh - ImGui::GetTextLineHeight()) * 0.5f), IM_COL32(255, 255, 255, 255), it.label.c_str());
    const float sw = rh * 0.62f, gap = 12.0f * u;
    const float x0 = p.x + w - (5.0f * sw + 4.0f * gap) - 16.0f * u;
    for (int i = 0; i < 5; i++) {
        ImVec2 a(x0 + i * (sw + gap), p.y + (rh - sw) * 0.5f), b(a.x + sw, a.y + sw);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(100 + i);
        if (ImGui::InvisibleButton("##sw", ImVec2(sw, sw))) { g_theme = i; SaveMenuSettings(); }
        ImGui::PopID();
        dl->AddRectFilled(a, b, ImGui::GetColorU32(kAccent[i]), sw * 0.3f);
        if (i == g_theme)
            dl->AddRect(ImVec2(a.x - 4.0f * u, a.y - 4.0f * u), ImVec2(b.x + 4.0f * u, b.y + 4.0f * u), IM_COL32(255, 255, 255, 255), sw * 0.38f, 0, 3.0f * u);
    }
    ImGui::SetCursorScreenPos(after);
}

// pestanas unidas: un solo control con dos mitades; la activa va en el color del tema
static int Segmented(const std::string& a, const std::string& b, int active, float w, float h, float u, const Pal& pal) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    ImVec2 after = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(pal.seg), h * 0.5f);
    const float half = w * 0.5f, inset = 5.0f * u;
    const std::string* lab[2] = {&a, &b};
    int clicked = -1;
    for (int i = 0; i < 2; i++) {
        ImVec2 s(p.x + i * half, p.y);
        ImGui::SetCursorScreenPos(s);
        ImGui::PushID(200 + i);
        if (ImGui::InvisibleButton("##seg", ImVec2(half, h))) clicked = i;
        ImGui::PopID();
        if (i == active)
            dl->AddRectFilled(ImVec2(s.x + inset, s.y + inset), ImVec2(s.x + half - inset, s.y + h - inset), ImGui::GetColorU32(pal.accent), (h - 2.0f * inset) * 0.5f);
        ImVec2 ts = ImGui::CalcTextSize(lab[i]->c_str());
        dl->AddText(ImVec2(s.x + (half - ts.x) * 0.5f, s.y + (h - ts.y) * 0.5f), i == active ? IM_COL32(255, 255, 255, 255) : ImGui::GetColorU32(kGrey), lab[i]->c_str());
    }
    ImGui::SetCursorScreenPos(after);
    return clicked;
}

// boton flotante que abre el menu: percha + nombre; se arrastra para moverlo (salvo que este bloqueado)
static void FloatingButton(float W, float H, float u, const std::string& title, const ImGuiWindowFlags fixed, const Pal& pal) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 ts = ImGui::CalcTextSize(title.c_str());
    const float bw = ts.x + 124.0f * u, bh = 88.0f * u;
    if (g_btnFx < 0.0f) g_btnFx = 0.0f;
    if (g_btnFy < 0.0f) g_btnFy = 0.0f;
    if (g_btnFx > 1.0f - bw / W) g_btnFx = 1.0f - bw / W;
    if (g_btnFy > 1.0f - bh / H) g_btnFy = 1.0f - bh / H;
    ImGui::SetNextWindowPos(ImVec2(g_btnFx * W, g_btnFy * H));
    ImGui::SetNextWindowSize(ImVec2(bw, bh));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::Begin("##vbtn", nullptr, fixed);
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    ImGui::InvisibleButton("##vopen", ImVec2(bw, bh));
    const bool active = ImGui::IsItemActive();
    if (!g_btnLock && active && ImGui::IsMouseDragging(0, 8.0f * u)) {   // arrastrar = mover
        g_btnFx += io.MouseDelta.x / W;
        g_btnFy += io.MouseDelta.y / H;
        g_btnDragged = true;
    }
    if (ImGui::IsItemDeactivated()) {
        if (g_btnDragged) SaveMenuSettings();
        else { g_open = true; g_screen = 0; g_page = 0; }               // toque corto = abrir
        g_btnDragged = false;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetWindowPos();
    ImVec2 p1(p0.x + bw, p0.y + bh);
    const float r = 20.0f * u, th3 = 3.0f * u;
    const ImU32 acc = ImGui::GetColorU32(pal.accent);
    dl->AddRectFilled(ImVec2(p0.x + 4.0f * u, p0.y + 6.0f * u), ImVec2(p1.x + 4.0f * u, p1.y + 6.0f * u), IM_COL32(0, 0, 0, 110), r);
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(active ? pal.rowActive : pal.bg), r);
    dl->AddRect(p0, p1, acc, r, 0, th3);
    const float cx = p0.x + 46.0f * u, cy = p0.y + bh * 0.5f;           // icono: percha
    dl->AddCircle(ImVec2(cx, cy - 23.0f * u), 7.0f * u, acc, 20, th3);
    dl->AddLine(ImVec2(cx, cy - 16.0f * u), ImVec2(cx, cy - 7.0f * u), acc, th3);
    dl->AddTriangle(ImVec2(cx, cy - 7.0f * u), ImVec2(cx - 29.0f * u, cy + 19.0f * u), ImVec2(cx + 29.0f * u, cy + 19.0f * u), acc, th3);
    dl->AddText(ImVec2(p0.x + 92.0f * u, cy - ts.y * 0.5f), IM_COL32(255, 255, 255, 255), title.c_str());
    AddRect();
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

static void BuildUI(float W, float H, float u) {
    g_frameRects.clear();
    std::vector<std::string> skins;
    std::string cur, title, status;
    bool rem, mis, veh;
    { std::lock_guard<std::mutex> lk(g_listMtx); skins = g_skinsM; cur = g_current; title = g_title; rem = g_remember; mis = g_missions; veh = g_vehicle; status = g_status; }

    const ImGuiWindowFlags fixed = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    const Pal pal = MakePal();

    if (!g_open) { FloatingButton(W, H, u, title, fixed, pal); return; }

    const bool settings = (g_screen == 1);

    // ---- elementos de la lista ----
    std::vector<Item> items;
    if (!settings) {
        items.push_back({Tr(K_CJ), "", kRed, 2, ""});
        for (auto& s : skins) {
            bool on = (s == cur);
            Item it = {s, on ? Tr(K_WORN) : "", kWhite, 1, s};
            it.chip = on;
            it.selected = on;
            items.push_back(it);
        }
        if (skins.empty()) items.push_back({Tr(K_NOSKINS), "", kGrey, 9, ""});
    } else {
        Item a = {Tr(K_REMEMBER), "", kWhite, 3, ""};  a.sw = rem ? 1 : 0;       items.push_back(a);
        Item b = {Tr(K_MISSIONS), "", kWhite, 8, ""};  b.sw = mis ? 1 : 0;       items.push_back(b);
        Item c = {Tr(K_VEHICLE), "", kWhite, 10, ""};  c.sw = veh ? 1 : 0;       items.push_back(c);
        Item d = {Tr(K_LOCK), "", kWhite, 11, ""};     d.sw = g_btnLock ? 1 : 0; items.push_back(d);
        items.push_back({Tr(K_LANG), Loc(kLangName[g_lang]), kWhite, 5, ""});
        items.push_back({Tr(K_SIZE), Loc(kSizeName[g_lang][g_size]), kWhite, 6, ""});
        items.push_back({Tr(K_THEME) + ": " + Loc(kThemeName[g_lang][g_theme]), "", kWhite, 7, ""});
        items.push_back({Tr(K_RESCAN), "", kWhite, 4, ""});
    }

    // ---- panel principal (izquierda) ----
    const float uk = u / (H / 1080.0f);                          // multiplicador de tamano
    const float pw = W * 0.37f * uk, ph = H * 0.86f;
    ImGui::SetNextWindowPos(ImVec2(W * 0.02f, H * 0.07f));
    ImGui::SetNextWindowSize(ImVec2(pw, ph));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f * u);
    ImGui::PushStyleColor(ImGuiCol_Border, pal.accent);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, pal.bg);
    ImGui::Begin("##vpanel", nullptr, fixed);
    const float pad = ImGui::GetStyle().WindowPadding.x;

    // titulo y X
    ImGui::TextColored(pal.title, "%s", title.c_str());
    {
        float xw = 58.0f * u;
        ImGui::SameLine(pw - xw - pad);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1.0f));
        if (ImGui::Button("X", ImVec2(xw, 0))) g_open = false;
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();

    // pestanas unidas SKINS | AJUSTES
    {
        int c = Segmented(Tr(K_TAB_SKINS), Tr(K_TAB_SET), g_screen, pw - 2.0f * pad, 56.0f * u, u, pal);
        if (c >= 0 && c != g_screen) { g_screen = c; g_page = 0; }
    }

    // cuantas filas caben entre las pestanas y el pie
    const float rh = 56.0f * u;
    const float ah = 48.0f * u;
    const float footerTop = ImGui::GetWindowHeight() - pad - ah;
    const float pitch = rh + ImGui::GetStyle().ItemSpacing.y;
    float avail = footerTop - ImGui::GetCursorPosY() - 10.0f * u;
    int perPage = (int)(avail / pitch);
    if (perPage < 3) perPage = 3;
    if (perPage > 16) perPage = 16;
    int pages = ((int)items.size() + perPage - 1) / perPage;
    if (pages < 1) pages = 1;
    if (g_page >= pages) g_page = pages - 1;
    if (g_page < 0) g_page = 0;

    // filas
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(18.0f * u, 6.0f * u));
    ImGui::PushStyleColor(ImGuiCol_Button, pal.row);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pal.rowHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, pal.rowActive);
    for (int i = g_page * perPage; i < (int)items.size() && i < (g_page + 1) * perPage; i++) {
        ImGui::PushID(i);
        const Item& it = items[i];
        if (it.action == 7) ColorRow(it, rh, u, pal);
        else if (Row(it, rh, u, pal)) {
            if (it.action == 1) Request(it.arg);
            else if (it.action == 2) Request("CJ");
            else if (it.action == 3) {
                { std::lock_guard<std::mutex> lk(g_listMtx); g_remember = !rem; g_lastRead = time(nullptr); }
                Request(rem ? "REMEMBER_OFF" : "REMEMBER_ON");
            }
            else if (it.action == 4) Request("RESCAN");
            else if (it.action == 5) { g_lang = (g_lang + 1) % 3; SaveMenuSettings(); }
            else if (it.action == 6) { g_size = (g_size + 1) % 3; g_styleDirty = true; SaveMenuSettings(); }
            else if (it.action == 8) {
                { std::lock_guard<std::mutex> lk(g_listMtx); g_missions = !mis; g_lastRead = time(nullptr); }
                Request(mis ? "MISSIONS_OFF" : "MISSIONS_ON");
            }
            else if (it.action == 10) {
                { std::lock_guard<std::mutex> lk(g_listMtx); g_vehicle = !veh; g_lastRead = time(nullptr); }
                Request(veh ? "VEHICLE_OFF" : "VEHICLE_ON");
            }
            else if (it.action == 11) { g_btnLock = !g_btnLock; SaveMenuSettings(); }
        }
        ImGui::PopID();
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);

    // ---- pie: pista a la izquierda y flechas pequenas a la derecha ----
    {
        char hint[128];
        if (!status.empty()) snprintf(hint, sizeof hint, Loc(kText[g_lang][K_WAIT]).c_str(), StatusText(status).c_str());
        else snprintf(hint, sizeof hint, "%s", Tr(K_HINT).c_str());
        ImGui::SetCursorPos(ImVec2(pad, footerTop + ah * 0.22f));
        ImGui::SetWindowFontScale(0.74f);
        ImGui::TextColored(status.empty() ? kGrey : ImVec4(1.0f, 0.70f, 0.25f, 1.0f), "%s", hint);
        ImGui::SetWindowFontScale(1.0f);

        const float aw = 66.0f * u, lw = 100.0f * u, sp = ImGui::GetStyle().ItemSpacing.x;
        const float x0 = pw - pad - aw * 2.0f - lw - sp * 2.0f;
        const bool multi = pages > 1;
        ImGui::PushStyleColor(ImGuiCol_Button, pal.seg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pal.rowHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, pal.rowActive);
        ImGui::PushStyleColor(ImGuiCol_Text, multi ? kWhite : ImVec4(0.45f, 0.45f, 0.48f, 1.0f));
        ImGui::SetCursorPos(ImVec2(x0, footerTop));
        if (ImGui::Button("<", ImVec2(aw, ah)) && multi) g_page = (g_page + pages - 1) % pages;
        ImGui::SetCursorPos(ImVec2(x0 + aw + lw + sp * 2.0f, footerTop));
        if (ImGui::Button(">", ImVec2(aw, ah)) && multi) g_page = (g_page + 1) % pages;
        ImGui::PopStyleColor(4);

        char pg[24];
        snprintf(pg, sizeof pg, "%d/%d", g_page + 1, pages);
        ImVec2 ts = ImGui::CalcTextSize(pg);
        ImGui::SetCursorPos(ImVec2(x0 + aw + sp + (lw - ts.x) * 0.5f, footerTop + (ah - ts.y) * 0.5f));
        ImGui::TextColored(kGrey, "%s", pg);
    }
    AddRect();
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

// ---- dibujo (hilo de render, justo antes de eglSwapBuffers) --------------------------------------
static bool g_inited = false, g_failed = false;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static std::chrono::steady_clock::time_point g_last;
static EGLBoolean (*eglSwapBuffers_orig)(EGLDisplay, EGLSurface);

static float UiScale(float H) { return kFontPx * (H / 1080.0f) * kSizeMul[g_size] / 30.0f; }

static void ApplyStyle(float H) {
    const float px = kFontPx * (H / 1080.0f) * kSizeMul[g_size];
    const float u = px / 30.0f;
    ImGuiIO& io = ImGui::GetIO();
    LoadFont(px);                                   // letra horneada al tamano real: nitida, sin escalar
    io.FontGlobalScale = g_hasTtf ? 1.0f : px / 13.0f;
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark();
    st.ScaleAllSizes(px / 13.0f);
    st.WindowRounding = 22.0f * u;                  // esquinas redondeadas
    st.FrameRounding = 14.0f * u;
    st.ChildRounding = 14.0f * u;
    st.PopupRounding = 14.0f * u;
    st.GrabRounding = 14.0f * u;
    st.FrameBorderSize = 0.0f;
    st.AntiAliasedLines = true;
    st.AntiAliasedFill = true;
}

static bool InitImGui(float W, float H) {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ApplyStyle(H);
    if (!ImGui_ImplOpenGL3_Init(nullptr)) { Log("ImGui_ImplOpenGL3_Init fallo"); return false; }
    Log("ImGui listo (%.0fx%.0f)", W, H);
    return true;
}

typedef void (*PFNBINDVAO)(GLuint);
static PFNBINDVAO g_bindVao = nullptr;      // glBindVertexArray (si el juego usa ES3)
static const volatile uint8_t* g_pauseUser = nullptr;   // CTimer::m_UserPause
static const volatile uint8_t* g_pauseCode = nullptr;   // CTimer::m_CodePause
static int g_swaps = 0, g_drawn = 0, g_noPlayer = 0, g_otherCtx = 0;

static void LogGlState(const char* why, EGLContext ctx, int w, int h) {
    GLint fbo = 0, vao = 0, vp[4] = {0, 0, 0, 0}, prog = 0;
    GLboolean cm[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    if (g_bindVao) glGetIntegerv(0x85B5 /* GL_VERTEX_ARRAY_BINDING */, &vao);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    int sc = glIsEnabled(GL_SCISSOR_TEST) ? 1 : 0;
    while (glGetError() != GL_NO_ERROR) {}
    Log("estado %s: ctx=%p hilo=%d superficie=%dx%d fbo=%d vao=%d viewport=%d,%d,%d,%d prog=%d scissor=%d colormask=%d%d%d%d",
        why, ctx, (int)syscall(SYS_gettid), w, h, fbo, vao, vp[0], vp[1], vp[2], vp[3], prog, sc,
        (int)cm[0], (int)cm[1], (int)cm[2], (int)cm[3]);
}

static void DrawMenu(EGLDisplay dpy, EGLSurface surf) {
    if (g_failed) return;
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return;
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 200 || h < 200) return;

    if (!g_inited) {
        g_ctx = ctx;
        g_bindVao = (PFNBINDVAO)eglGetProcAddress("glBindVertexArray");
        if (!g_bindVao) g_bindVao = (PFNBINDVAO)eglGetProcAddress("glBindVertexArrayOES");
        if (!InitImGui((float)w, (float)h)) { g_failed = true; return; }
        g_inited = true;
        g_last = std::chrono::steady_clock::now();
        Log("VAO disponible: %s", g_bindVao ? "si" : "no");
        LogGlState("inicio", ctx, w, h);
    }
    if (ctx != g_ctx) {
        g_otherCtx++;
        if (g_otherCtx <= 8) Log("contexto distinto al de ImGui: actual=%p ImGui=%p hilo=%d", ctx, g_ctx, (int)syscall(SYS_gettid));
        return;
    }
    bool paused = (g_pauseUser && *g_pauseUser) || (g_pauseCode && *g_pauseCode);
    static int lastPaused = -1;
    if ((int)paused != lastPaused) { lastPaused = (int)paused; Log("pausa: %d", (int)paused); }
    if (paused) {                       // en el menu de pausa no se muestra ni el boton ni el panel
        std::lock_guard<std::mutex> lk(g_inMtx);
        g_rects.clear();
        return;
    }
    bool havePlayer = !FindPlayerPed || FindPlayerPed(-1) != nullptr;
    static int lastHave = -1;
    if ((int)havePlayer != lastHave) { lastHave = (int)havePlayer; Log("jugador %s", havePlayer ? "presente" : "ausente"); }
    if (!havePlayer) {
        g_noPlayer++;
        g_open = false;
        std::lock_guard<std::mutex> lk(g_inMtx);
        g_rects.clear();
        return;
    }

    RefreshList();

    ImGuiIO& io = ImGui::GetIO();
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - g_last).count();
    g_last = now;
    io.DeltaTime = dt > 0.0001f ? dt : 1.0f / 60.0f;
    io.DisplaySize = ImVec2((float)w, (float)h);

    std::vector<TouchEv> evs;
    { std::lock_guard<std::mutex> lk(g_inMtx); evs.swap(g_evs); }
    for (auto& e : evs) {
        if (e.kind == 0) io.AddMousePosEvent(e.x, e.y);
        else io.AddMouseButtonEvent(0, e.kind == 1);
        if (e.kind == 2) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }

    // ---- guardar el estado de GL del juego y dejarlo limpio para ImGui ----
    struct AttribSt { GLint en, size, type, norm, stride, buf; void* ptr; };
    AttribSt as[16];
    GLint lastArrayBuf = 0;
    GLint maxAttr = 8, lastElem = 0, lastVao = 0, lastFbo = 0;
    GLboolean cm[4] = {1, 1, 1, 1};
    // en gameplay el juego deja un FBO (fuera de pantalla) enlazado al presentar: ahi no se ve nada; se dibuja en el framebuffer 0
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &lastFbo);
    if (lastFbo != 0) {
        static int fboLogs = 0;
        if (fboLogs++ < 5) Log("fbo %d enlazado al presentar: se dibuja en el framebuffer 0", lastFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxAttr);
    if (maxAttr > 16) maxAttr = 16;
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &lastElem);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &lastArrayBuf);
    for (int i = 0; i < maxAttr; i++) {   // estado completo de cada atributo: ImGui pisa los punteros de los suyos
        AttribSt& a = as[i];
        a.en = a.size = a.type = a.norm = a.stride = a.buf = 0;
        a.ptr = nullptr;
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a.en);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a.size);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a.type);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a.norm);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a.stride);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a.buf);
        glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a.ptr);
    }
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    if (g_bindVao) { glGetIntegerv(0x85B5 /* GL_VERTEX_ARRAY_BINDING */, &lastVao); g_bindVao(0); }
    for (int i = 0; i < maxAttr; i++) glDisableVertexAttribArray(i);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    while (glGetError() != GL_NO_ERROR) {}

    if (g_styleDirty) {                 // cambio de tamano: se rehornea la fuente
        ImGui_ImplOpenGL3_DestroyFontsTexture();
        ApplyStyle((float)h);
        ImGui_ImplOpenGL3_CreateFontsTexture();
        g_styleDirty = false;
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    BuildUI((float)w, (float)h, UiScale((float)h));
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    ImGui_ImplOpenGL3_RenderDrawData(dd);

    GLenum err = glGetError();
    static int errLogs = 0;
    if (err != GL_NO_ERROR && errLogs++ < 10) Log("glGetError tras dibujar = 0x%x", err);
    static int nFrames = 0;
    if (nFrames < 3 || nFrames % 600 == 0) {
        Log("frame %d: vertices=%d rects=%zu abierto=%d", nFrames, dd ? dd->TotalVtxCount : -1, g_frameRects.size(), (int)g_open);
        LogGlState("dibujo", ctx, w, h);
    }
    nFrames++;
    g_drawn++;

    // ---- devolver el estado al juego ----
    if (lastFbo != 0) glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)lastFbo);
    if (g_bindVao) g_bindVao((GLuint)lastVao);
    for (int i = 0; i < maxAttr; i++) {
        const AttribSt& a = as[i];
        glBindBuffer(GL_ARRAY_BUFFER, (GLuint)a.buf);
        glVertexAttribPointer(i, a.size > 0 ? a.size : 4, a.type ? (GLenum)a.type : GL_FLOAT, a.norm ? GL_TRUE : GL_FALSE, a.stride, a.ptr);
        if (a.en) glEnableVertexAttribArray(i); else glDisableVertexAttribArray(i);
    }
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)lastArrayBuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)lastElem);
    glColorMask(cm[0], cm[1], cm[2], cm[3]);
    { std::lock_guard<std::mutex> lk(g_inMtx); g_rects = g_frameRects; }
}

static EGLBoolean eglSwapBuffers_hook(EGLDisplay d, EGLSurface s) {
    g_swaps++;
    DrawMenu(d, s);
    if (g_swaps % 900 == 0)
        Log("swaps=%d dibujados=%d sin_jugador=%d otro_contexto=%d", g_swaps, g_drawn, g_noPlayer, g_otherCtx);
    return eglSwapBuffers_orig(d, s);
}

extern "C" void OnModLoad() {
    std::string files;
    for (auto d : kFilesDirs) {
        struct stat st;
        if (stat((std::string(d) + "/modloader").c_str(), &st) == 0) { files = d; break; }
    }
    if (files.empty()) files = kFilesDirs[0];
    g_plugDir = files + "/VestidorSA";
    mkdir(g_plugDir.c_str(), 0777);
    FILE* lf = fopen((g_plugDir + "/menu_log.txt").c_str(), "w"); if (lf) fclose(lf);
    LoadMenuSettings();
    Log("== Vestidor SA Menu 3.0.0 cargado ==");

    void* h = aml->GetLibHandle("libGTASA.so");
    *(void**)&FindPlayerPed = (void*)aml->GetSym(h, "_Z13FindPlayerPedi");
    g_pauseUser = (const volatile uint8_t*)aml->GetSym(h, "_ZN6CTimer11m_UserPauseE");
    g_pauseCode = (const volatile uint8_t*)aml->GetSym(h, "_ZN6CTimer11m_CodePauseE");
    Log("  pausa: %s m_UserPause, %s m_CodePause", g_pauseUser ? "ok" : "FALTA", g_pauseCode ? "ok" : "FALTA");
    void* touch = (void*)aml->GetSym(h, "_Z14AND_TouchEventiiii");
    Log("  %s FindPlayerPed, %s AND_TouchEvent", FindPlayerPed ? "ok" : "FALTA", touch ? "ok" : "FALTA");
    if (touch) aml->Hook(touch, (void*)AND_TouchEvent_hook, (void**)&AND_TouchEvent_orig);

    bool got = PatchGot("libGTASA.so", "eglSwapBuffers", (void*)eglSwapBuffers_hook, (void**)&eglSwapBuffers_orig);
    Log("  eglSwapBuffers por GOT: %s", got ? "ok" : "no encontrado");
    if (!got) {
        void* fn = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
        if (fn) {
            aml->Hook(fn, (void*)eglSwapBuffers_hook, (void**)&eglSwapBuffers_orig);
            Log("  eglSwapBuffers por hook directo (%p)", fn);
        } else {
            Log("  no se pudo enganchar eglSwapBuffers: el menu no se dibujara");
        }
    }
}
