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

MYMODCFG(net.local.vestidorsamenu, Vestidor SA Menu, 2.0.0, Tu nombre)
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

// ---- lista de skins (la escribe SkinSelector en skins_list.txt) -----------------------------------
static std::vector<std::string> g_skinsM;
static std::string g_current = "CJ";
static std::string g_title = "VESTIDOR SA";
static bool g_remember = false;
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
    bool rem = false;
    std::string title;
    FILE* sf = fopen((g_plugDir + "/settings.txt").c_str(), "r");
    if (sf) {
        char sl[200];
        while (fgets(sl, sizeof sl, sf)) {
            if (strncmp(sl, "remember=", 9) == 0) rem = (sl[9] == '1');
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
static bool g_btnDragged = false;
static bool g_styleDirty = false;
static const float kSizeMul[3] = {0.85f, 1.0f, 1.2f};

static void SaveMenuSettings() {
    FILE* f = fopen((g_plugDir + "/menu_settings.txt").c_str(), "w");
    if (!f) return;
    fprintf(f, "theme=%d\nlang=%d\nsize=%d\nbtnx=%.4f\nbtny=%.4f\n", g_theme, g_lang, g_size, g_btnFx, g_btnFy);
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
        else if (sscanf(l, "btnx=%f", &x) == 1 && x >= 0.0f && x <= 1.0f) g_btnFx = x;
        else if (sscanf(l, "btny=%f", &x) == 1 && x >= 0.0f && x <= 1.0f) g_btnFy = x;
    }
    fclose(f);
}

// ---- idiomas: espanol, ingles, indonesio ---------------------------------------------------------------
enum { K_CJ, K_WORN, K_REMEMBER, K_YES, K_NO, K_RESCAN, K_MISSIONS, K_HINT, K_WAIT, K_TAB_SKINS, K_TAB_SET,
       K_LANG, K_THEME, K_SIZE, K_NOSKINS, K_COUNT };
static const char* kText[3][K_COUNT] = {
    {"Volver a ser CJ", "PUESTA", "Recordar ultima skin", "SI", "NO", "Releer carpeta de skins",
     "Skin en misiones: siempre activa", "Toca una opcion", "Esperando: jugador %s...", "SKINS", "AJUSTES",
     "Idioma", "Color", "Tamano", "Sin skins (revisa skins_list.txt)"},
    {"Back to CJ", "WORN", "Remember last skin", "YES", "NO", "Reload skins folder",
     "Skin in missions: always on", "Tap an option", "Waiting: player %s...", "SKINS", "SETTINGS",
     "Language", "Color", "Size", "No skins (check skins_list.txt)"},
    {"Kembali jadi CJ", "DIPAKAI", "Ingat skin terakhir", "YA", "TIDAK", "Muat ulang folder skin",
     "Skin di misi: selalu aktif", "Ketuk satu opsi", "Menunggu: pemain %s...", "SKIN", "PENGATURAN",
     "Bahasa", "Warna", "Ukuran", "Tidak ada skin (cek skins_list.txt)"}};
static const char* kLangName[3] = {"Espanol", "English", "Indonesia"};
static const char* kSizeName[3][3] = {{"Pequeno", "Normal", "Grande"}, {"Small", "Normal", "Large"}, {"Kecil", "Normal", "Besar"}};
static const char* kThemeName[3][5] = {{"Por defecto", "Rojo", "Azul", "Verde", "Naranja"},
                                       {"Default", "Red", "Blue", "Green", "Orange"},
                                       {"Bawaan", "Merah", "Biru", "Hijau", "Oranye"}};
static const char* kStatusName[3][4] = {{"agachado", "peleando", "ocupado", "en vehiculo"},
                                        {"crouching", "fighting", "busy", "in a vehicle"},
                                        {"jongkok", "berkelahi", "sibuk", "di kendaraan"}};
static const char* Tr(int k) { return kText[g_lang][k]; }
static std::string StatusText(const std::string& st) {   // el nucleo escribe el estado en espanol
    for (int i = 0; i < 4; i++) if (st == kStatusName[0][i]) return kStatusName[g_lang][i];
    return st;
}

// ---- colores: 5 predefinidos (mejor que un selector RGB: se tocan con el dedo sin errores) ----------
struct Theme { ImVec4 accent, title; };
static const Theme kThemes[5] = {
    {ImVec4(0.16f, 0.38f, 0.62f, 1.0f), ImVec4(0.96f, 0.74f, 0.18f, 1.0f)},   // por defecto: azul + dorado
    {ImVec4(0.74f, 0.14f, 0.14f, 1.0f), ImVec4(1.00f, 0.56f, 0.50f, 1.0f)},   // rojo
    {ImVec4(0.06f, 0.52f, 0.86f, 1.0f), ImVec4(0.55f, 0.82f, 1.00f, 1.0f)},   // azul
    {ImVec4(0.14f, 0.62f, 0.26f, 1.0f), ImVec4(0.55f, 0.96f, 0.58f, 1.0f)},   // verde
    {ImVec4(0.92f, 0.46f, 0.08f, 1.0f), ImVec4(1.00f, 0.78f, 0.40f, 1.0f)},   // naranja
};

// ---- interfaz ------------------------------------------------------------------------------------
static bool g_open = false;
static int g_page = 0;
static int g_screen = 0;          // 0 = skins, 1 = ajustes (pestanas de arriba)
static void* (*FindPlayerPed)(int);
static std::vector<Rect> g_frameRects;

struct Item { std::string label, right; ImVec4 col; int action; std::string arg; };
static const ImVec4 kWhite(1.0f, 1.0f, 1.0f, 1.0f);
static const ImVec4 kGreen(0.45f, 0.95f, 0.50f, 1.0f);
static const ImVec4 kRed(0.97f, 0.42f, 0.38f, 1.0f);
static const ImVec4 kGrey(0.78f, 0.78f, 0.80f, 1.0f);

static void AddRect() {
    ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    float mg = 12.0f;
    g_frameRects.push_back({p.x - mg, p.y - mg, p.x + s.x + mg, p.y + s.y + mg});
}

// fila tipo lista: texto a la izquierda y, si hay, un texto a la derecha ("PUESTA")
static bool Row(const Item& it, float rh, float u) {
    ImGui::PushStyleColor(ImGuiCol_Text, it.col);
    bool clicked = ImGui::Button(it.label.c_str(), ImVec2(-1, rh));
    ImGui::PopStyleColor();
    if (!it.right.empty()) {
        ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        ImVec2 ts = ImGui::CalcTextSize(it.right.c_str());
        ImGui::GetWindowDrawList()->AddText(ImVec2(mx.x - ts.x - 16.0f * u, mn.y + (mx.y - mn.y - ts.y) * 0.5f),
                                            ImGui::GetColorU32(it.col), it.right.c_str());
    }
    return clicked;
}

// fila con 5 cuadros de color para elegir el tema
static void ColorRow(const Item& it, float rh, float u) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(w, rh));
    ImVec2 after = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rh), IM_COL32(28, 28, 31, 235));
    dl->AddText(ImVec2(p.x + 12.0f * u, p.y + (rh - ImGui::GetTextLineHeight()) * 0.5f), IM_COL32(255, 255, 255, 255), it.label.c_str());
    const float sw = rh * 0.62f, gap = 12.0f * u;
    const float x0 = p.x + w - (5.0f * sw + 4.0f * gap) - 14.0f * u;
    for (int i = 0; i < 5; i++) {
        ImVec2 a(x0 + i * (sw + gap), p.y + (rh - sw) * 0.5f), b(a.x + sw, a.y + sw);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(100 + i);
        if (ImGui::InvisibleButton("##sw", ImVec2(sw, sw))) { g_theme = i; SaveMenuSettings(); }
        ImGui::PopID();
        dl->AddRectFilled(a, b, ImGui::GetColorU32(kThemes[i].accent));
        if (i == g_theme)
            dl->AddRect(ImVec2(a.x - 3.0f * u, a.y - 3.0f * u), ImVec2(b.x + 3.0f * u, b.y + 3.0f * u), IM_COL32(255, 255, 255, 255), 0.0f, 0, 3.0f * u);
    }
    ImGui::SetCursorScreenPos(after);
}

// boton flotante que abre el menu: percha + nombre, se arrastra para moverlo
static void FloatingButton(float W, float H, float u, const std::string& title, const ImGuiWindowFlags fixed) {
    const Theme& th = kThemes[g_theme];
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 ts = ImGui::CalcTextSize(title.c_str());
    const float bw = ts.x + 118.0f * u, bh = 84.0f * u;
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
    if (active && ImGui::IsMouseDragging(0, 8.0f * u)) {          // arrastrar = mover el boton
        g_btnFx += io.MouseDelta.x / W;
        g_btnFy += io.MouseDelta.y / H;
        g_btnDragged = true;
    }
    if (ImGui::IsItemDeactivated()) {
        if (g_btnDragged) SaveMenuSettings();                     // se guarda la nueva posicion
        else { g_open = true; g_screen = 0; g_page = 0; }         // toque corto = abrir
        g_btnDragged = false;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetWindowPos();
    ImVec2 p1(p0.x + bw, p0.y + bh);
    const float r = 16.0f * u, th3 = 3.0f * u;
    const ImU32 acc = ImGui::GetColorU32(th.accent);
    dl->AddRectFilled(ImVec2(p0.x + 4.0f * u, p0.y + 5.0f * u), ImVec2(p1.x + 4.0f * u, p1.y + 5.0f * u), IM_COL32(0, 0, 0, 100), r);   // sombra
    dl->AddRectFilled(p0, p1, active ? IM_COL32(44, 46, 58, 238) : IM_COL32(12, 13, 18, 222), r);
    dl->AddRect(p0, p1, acc, r, 0, th3);
    const float cx = p0.x + 44.0f * u, cy = p0.y + bh * 0.5f;      // icono: percha
    dl->AddCircle(ImVec2(cx, cy - 22.0f * u), 7.0f * u, acc, 20, th3);
    dl->AddLine(ImVec2(cx, cy - 15.0f * u), ImVec2(cx, cy - 6.0f * u), acc, th3);
    dl->AddTriangle(ImVec2(cx, cy - 6.0f * u), ImVec2(cx - 28.0f * u, cy + 18.0f * u), ImVec2(cx + 28.0f * u, cy + 18.0f * u), acc, th3);
    dl->AddText(ImVec2(p0.x + 88.0f * u, cy - ts.y * 0.5f), IM_COL32(255, 255, 255, 255), title.c_str());
    AddRect();
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

static void BuildUI(float W, float H, float u) {
    g_frameRects.clear();
    std::vector<std::string> skins;
    std::string cur, title, status;
    bool rem;
    { std::lock_guard<std::mutex> lk(g_listMtx); skins = g_skinsM; cur = g_current; title = g_title; rem = g_remember; status = g_status; }

    const ImGuiWindowFlags fixed = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    const Theme& th = kThemes[g_theme];

    if (!g_open) { FloatingButton(W, H, u, title, fixed); return; }

    const bool settings = (g_screen == 1);

    // ---- elementos de la lista ----
    std::vector<Item> items;
    if (!settings) {
        items.push_back({Tr(K_CJ), "", kRed, 2, ""});
        for (auto& s : skins) {
            bool on = (s == cur);
            items.push_back({s, on ? Tr(K_WORN) : "", on ? kGreen : kWhite, 1, s});
        }
        if (skins.empty()) items.push_back({Tr(K_NOSKINS), "", kGrey, 9, ""});
    } else {
        items.push_back({std::string(Tr(K_REMEMBER)) + ": " + (rem ? Tr(K_YES) : Tr(K_NO)), "", kWhite, 3, ""});
        items.push_back({std::string(Tr(K_LANG)) + ": " + kLangName[g_lang], "", kWhite, 5, ""});
        items.push_back({std::string(Tr(K_SIZE)) + ": " + kSizeName[g_lang][g_size], "", kWhite, 6, ""});
        items.push_back({std::string(Tr(K_THEME)) + ": " + kThemeName[g_lang][g_theme], "", kWhite, 7, ""});
        items.push_back({Tr(K_RESCAN), "", kWhite, 4, ""});
        items.push_back({Tr(K_MISSIONS), "", kGrey, 9, ""});
    }

    // ---- panel principal (izquierda) ----
    const float uk = u / (H / 1080.0f);                          // multiplicador de tamano del menu
    const float pw = W * 0.40f * uk, ph = H * 0.86f;
    ImGui::SetNextWindowPos(ImVec2(W * 0.02f, H * 0.07f));
    ImGui::SetNextWindowSize(ImVec2(pw, ph));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f * u);
    ImGui::PushStyleColor(ImGuiCol_Border, th.accent);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.04f, 0.04f, 0.05f, 0.88f));
    ImGui::Begin("##vpanel", nullptr, fixed);
    const float pad = ImGui::GetStyle().WindowPadding.x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;

    // titulo y X
    ImGui::TextColored(th.title, "%s", title.c_str());
    {
        float xw = 54.0f * u;
        ImGui::SameLine(pw - xw - pad);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.09f, 0.09f, 1.0f));
        if (ImGui::Button("X", ImVec2(xw, 0))) g_open = false;
        ImGui::PopStyleColor();
    }

    // pestanas: SKINS | AJUSTES
    {
        const float tabW = (pw - 2.0f * pad - spacing) * 0.5f;
        for (int t = 0; t < 2; t++) {
            bool active = (g_screen == t);
            ImGui::PushStyleColor(ImGuiCol_Button, active ? th.accent : ImVec4(0.14f, 0.14f, 0.16f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, active ? kWhite : kGrey);
            if (ImGui::Button(t == 0 ? Tr(K_TAB_SKINS) : Tr(K_TAB_SET), ImVec2(tabW, 48.0f * u)) && g_screen != t) { g_screen = t; g_page = 0; }
            ImGui::PopStyleColor(2);
            if (t == 0) ImGui::SameLine();
        }
    }

    // cuantas filas caben entre las pestanas y el pie (con las flechas)
    const float rh = 54.0f * u;
    const float ah = 44.0f * u;
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
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.11f, 0.11f, 0.12f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.20f, 0.22f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.30f, 0.30f, 0.33f, 1.0f));
    for (int i = g_page * perPage; i < (int)items.size() && i < (g_page + 1) * perPage; i++) {
        ImGui::PushID(i);
        const Item& it = items[i];
        if (it.action == 7) ColorRow(it, rh, u);
        else if (Row(it, rh, u)) {
            if (it.action == 1) Request(it.arg);
            else if (it.action == 2) Request("CJ");
            else if (it.action == 3) {
                { std::lock_guard<std::mutex> lk(g_listMtx); g_remember = !rem; g_lastRead = time(nullptr); }
                Request(rem ? "REMEMBER_OFF" : "REMEMBER_ON");
            }
            else if (it.action == 4) Request("RESCAN");
            else if (it.action == 5) { g_lang = (g_lang + 1) % 3; SaveMenuSettings(); }
            else if (it.action == 6) { g_size = (g_size + 1) % 3; g_styleDirty = true; SaveMenuSettings(); }
        }
        ImGui::PopID();
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();

    // ---- pie: pista a la izquierda y flechas pequenas a la derecha ----
    {
        char hint[96];
        if (!status.empty()) snprintf(hint, sizeof hint, Tr(K_WAIT), StatusText(status).c_str());
        else snprintf(hint, sizeof hint, "%s", Tr(K_HINT));
        ImGui::SetCursorPos(ImVec2(pad, footerTop + ah * 0.28f));
        ImGui::SetWindowFontScale(0.7f);
        ImGui::TextColored(status.empty() ? kGrey : ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "%s", hint);
        ImGui::SetWindowFontScale(1.0f);

        const float aw = 62.0f * u, lw = 96.0f * u;
        const float x0 = pw - pad - aw * 2.0f - lw - spacing * 2.0f;
        const bool multi = pages > 1;
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, multi ? kWhite : ImVec4(0.45f, 0.45f, 0.47f, 1.0f));
        ImGui::SetCursorPos(ImVec2(x0, footerTop));
        if (ImGui::Button("<", ImVec2(aw, ah)) && multi) g_page = (g_page + pages - 1) % pages;
        ImGui::PopStyleColor(2);

        char pg[24];
        snprintf(pg, sizeof pg, "%d/%d", g_page + 1, pages);
        ImVec2 ts = ImGui::CalcTextSize(pg);
        ImGui::SetCursorPos(ImVec2(x0 + aw + spacing + (lw - ts.x) * 0.5f, footerTop + (ah - ts.y) * 0.5f));
        ImGui::TextColored(kGrey, "%s", pg);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, multi ? kWhite : ImVec4(0.45f, 0.45f, 0.47f, 1.0f));
        ImGui::SetCursorPos(ImVec2(x0 + aw + lw + spacing * 2.0f, footerTop));
        if (ImGui::Button(">", ImVec2(aw, ah)) && multi) g_page = (g_page + 1) % pages;
        ImGui::PopStyleColor(2);
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

static const float kBaseScale = 2.6f;   // tamano de letra base (a 1080p) antes del multiplicador de Ajustes

static void ApplyStyle(float H) {
    float s = kBaseScale * (H / 1080.0f) * kSizeMul[g_size];
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark();
    st.ScaleAllSizes(s);
    ImGui::GetIO().FontGlobalScale = s;
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

    if (g_styleDirty) { ApplyStyle((float)h); g_styleDirty = false; }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    BuildUI((float)w, (float)h, kBaseScale * kSizeMul[g_size] * ((float)h / 1080.0f) / 2.3f);
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
    Log("== Vestidor SA Menu 2.0.0 cargado ==");

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
