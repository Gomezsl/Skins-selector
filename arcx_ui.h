// arcx_ui.h - interfaz ImGui de Project ARCX: boton con el logo, panel deslizable y redimensionable.
// Va dentro del mismo .so que el nucleo (arcx_core.h). Dibuja justo antes de eglSwapBuffers.
#pragma once
#if __has_include("roboto_font.h")
  #include "roboto_font.h"     // lo genera el workflow: Roboto-Medium (de ImGui) con binary_to_compressed_c
  #define HAVE_EMBEDDED_FONT 1
#endif
#if __has_include("arcx_logo.h")
  #include "arcx_logo.h"       // logo redondo 128x128 RGBA embebido
  #define HAVE_LOGO 1
#endif

namespace ui {

static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsa::LogV("ui", "ARCX", fmt, ap);
    va_end(ap);
}

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

static GLuint g_logoTex = 0;

// textura del logo: se crea una sola vez al iniciar (igual que la de la fuente de ImGui) y se restaura el binding que tenia el juego
static void CreateLogoTexture() {
#ifdef HAVE_LOGO
    GLint last = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &last);
    while (glGetError() != GL_NO_ERROR) {}
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ARCX_LOGO_W, ARCX_LOGO_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, kArcxLogoRGBA);
    glBindTexture(GL_TEXTURE_2D, (GLuint)last);
    GLenum e = glGetError();
    Log("logo: textura %u (%dx%d) error=0x%x", tex, ARCX_LOGO_W, ARCX_LOGO_H, (unsigned)e);
    if (e == GL_NO_ERROR) g_logoTex = tex;
#endif
}


// ---- ajustes de la interfaz: ARCXConfigs.ini, seccion [Menu] --------------------------------------------
static int g_theme = 0;                       // 0 por defecto, 1 rojo, 2 azul, 3 verde, 4 naranja
static int g_lang = 0;                        // 0 espanol, 1 ingles, 2 indonesio
static float g_btnMul = 1.0f;                 // tamano del boton
static float g_alpha = 1.0f;                  // opacidad del menu y del boton
static float g_btnFx = 0.012f, g_btnFy = 0.30f;               // posicion del boton (fraccion de la pantalla)
static bool g_btnLock = false;                // boton bloqueado: no se arrastra
static float g_panX = 0.02f, g_panY = 0.07f, g_panW = 0.40f, g_panH = 0.86f;   // panel: posicion y tamano (fraccion de la pantalla)
static bool g_btnDragged = false;
static const float kBtnLo = 0.60f, kBtnHi = 1.60f, kAlphaLo = 0.25f, kAlphaHi = 1.0f;
static const float kFontPx = 33.0f;           // letra a 1080p
static const int kMaxRowsPerPage = 40;        // pasado este limite las skins pasan a otra pagina

static float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void SaveMenuSettings() {
    const int th = g_theme, lg = g_lang;
    const float bm = g_btnMul, al = g_alpha, bx = g_btnFx, by = g_btnFy, px = g_panX, py = g_panY, pw = g_panW, ph = g_panH;
    const bool lk = g_btnLock;
    vsa::CfgUpdate([&](vsa::Ini& ini) {
        auto& m = ini["Menu"];
        char b[32];
        m["theme"] = std::to_string(th);
        m["lang"] = std::to_string(lg);
        snprintf(b, sizeof b, "%.2f", bm); m["button_size"] = b;
        snprintf(b, sizeof b, "%.2f", al); m["opacity"] = b;
        snprintf(b, sizeof b, "%.4f", bx); m["button_x"] = b;
        snprintf(b, sizeof b, "%.4f", by); m["button_y"] = b;
        m["button_lock"] = lk ? "1" : "0";
        snprintf(b, sizeof b, "%.4f", px); m["panel_x"] = b;
        snprintf(b, sizeof b, "%.4f", py); m["panel_y"] = b;
        snprintf(b, sizeof b, "%.4f", pw); m["panel_w"] = b;
        snprintf(b, sizeof b, "%.4f", ph); m["panel_h"] = b;
    });
}
static void LoadMenuSettings() {
    vsa::Ini ini = vsa::CfgRead();
    if (ini.find("Menu") == ini.end()) return;
    int v = vsa::GetInt(ini, "Menu", "theme", 0);  if (v >= 0 && v < 5) g_theme = v;
    v = vsa::GetInt(ini, "Menu", "lang", 0);       if (v >= 0 && v < 3) g_lang = v;
    g_btnMul = Clampf(vsa::GetFloat(ini, "Menu", "button_size", 1.0f), kBtnLo, kBtnHi);
    g_alpha = Clampf(vsa::GetFloat(ini, "Menu", "opacity", 1.0f), kAlphaLo, kAlphaHi);
    g_btnFx = Clampf(vsa::GetFloat(ini, "Menu", "button_x", 0.012f), 0.0f, 1.0f);
    g_btnFy = Clampf(vsa::GetFloat(ini, "Menu", "button_y", 0.30f), 0.0f, 1.0f);
    g_btnLock = vsa::GetInt(ini, "Menu", "button_lock", 0) == 1;
    g_panX = Clampf(vsa::GetFloat(ini, "Menu", "panel_x", 0.02f), 0.0f, 0.9f);
    g_panY = Clampf(vsa::GetFloat(ini, "Menu", "panel_y", 0.07f), 0.0f, 0.9f);
    g_panW = Clampf(vsa::GetFloat(ini, "Menu", "panel_w", 0.40f), 0.28f, 0.95f);
    g_panH = Clampf(vsa::GetFloat(ini, "Menu", "panel_h", 0.86f), 0.38f, 0.98f);
}

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


typedef void (*PFNBINDVAO)(GLuint);
static PFNBINDVAO g_bindVao = nullptr;      // glBindVertexArray (si el juego usa ES3)

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

// ---- idiomas: espanol, ingles, indonesio ---------------------------------------------------------------
enum { K_CJ, K_WORN, K_REMEMBER, K_LOCK, K_RESCAN, K_LANG, K_THEME, K_BTNSIZE, K_OPACITY, K_HINT, K_WAIT,
       K_TAB_SKINS, K_TAB_SET, K_NOSKINS, K_SEC_GENERAL, K_SEC_LOOK, K_SEC_ABOUT, K_COUNT_FMT, K_COUNT };
static const char* kText[3][K_COUNT] = {
    {"Volver a ser CJ", "PUESTA", "Recordar última skin", "Bloquear botón", "Releer carpeta de skins", "Idioma", "Color",
     "Tamaño del botón", "Opacidad", "Desliza para ver más", "Esperando: jugador %s...", "SKINS", "AJUSTES",
     "No hay skins en modloader/", "GENERAL", "APARIENCIA", "ACERCA DE", "%d skins"},
    {"Back to CJ", "WORN", "Remember last skin", "Lock button", "Reload skins folder", "Language", "Color",
     "Button size", "Opacity", "Swipe to see more", "Waiting: player %s...", "SKINS", "SETTINGS",
     "No skins in modloader/", "GENERAL", "APPEARANCE", "ABOUT", "%d skins"},
    {"Kembali jadi CJ", "DIPAKAI", "Ingat skin terakhir", "Kunci tombol", "Muat ulang folder skin", "Bahasa", "Warna",
     "Ukuran tombol", "Opasitas", "Geser untuk melihat lagi", "Menunggu: pemain %s...", "SKIN", "PENGATURAN",
     "Tidak ada skin di modloader/", "UMUM", "TAMPILAN", "TENTANG", "%d skin"}};
static const char* kLangName[3] = {"Español", "English", "Indonesia"};
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
    for (auto& mp : map) {
        size_t p;
        while ((p = s.find(mp[0])) != std::string::npos) s.replace(p, strlen(mp[0]), mp[1]);
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
    p.bg = Mix(ImVec4(0.045f, 0.047f, 0.060f, 0.95f), a, t * 0.55f);
    p.row = Mix(ImVec4(0.125f, 0.125f, 0.145f, 0.96f), a, t);
    p.rowHover = Mix(p.row, a, 0.35f);
    p.rowActive = Mix(p.row, a, 0.70f);
    p.seg = Mix(ImVec4(0.09f, 0.09f, 0.11f, 1.0f), a, t * 0.8f);
    p.bg.w *= g_alpha;                      // la opacidad afecta a los fondos; el texto siempre se lee
    p.row.w *= g_alpha;
    p.rowHover.w *= g_alpha;
    p.rowActive.w *= g_alpha;
    p.seg.w *= g_alpha;
    return p;
}

static bool g_open = false;
static int g_page = 0;
static int g_screen = 0;          // 0 = skins, 1 = ajustes
static std::vector<Rect> g_frameRects;
static const ImVec4 kWhite(1.0f, 1.0f, 1.0f, 1.0f);
static const ImVec4 kRed(1.00f, 0.50f, 0.46f, 1.0f);
static const ImVec4 kGrey(0.74f, 0.75f, 0.80f, 1.0f);

static void AddRect() {
    ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    float mg = 12.0f;
    g_frameRects.push_back({p.x - mg, p.y - mg, p.x + s.x + mg, p.y + s.y + mg});
}

static void DrawIcon(ImDrawList* dl, int kind, float cx, float cy, float s, ImU32 col, float th) {
    if (kind == 0) {
        dl->AddCircle(ImVec2(cx, cy - s * 0.62f), s * 0.17f, col, 16, th);
        dl->AddLine(ImVec2(cx, cy - s * 0.45f), ImVec2(cx, cy - s * 0.2f), col, th);
        dl->AddTriangle(ImVec2(cx, cy - s * 0.2f), ImVec2(cx - s * 0.8f, cy + s * 0.55f), ImVec2(cx + s * 0.8f, cy + s * 0.55f), col, th);
    } else {
        const float r = s * 0.5f;
        dl->AddCircle(ImVec2(cx, cy), r, col, 24, th);
        dl->AddCircleFilled(ImVec2(cx, cy), r * 0.32f, col, 16);
        for (int i = 0; i < 8; i++) {
            const float a = (float)i * 0.785398f, dx = cosf(a), dy = sinf(a);
            dl->AddLine(ImVec2(cx + dx * r * 0.95f, cy + dy * r * 0.95f), ImVec2(cx + dx * r * 1.45f, cy + dy * r * 1.45f), col, th * 1.8f);
        }
    }
}

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
        const ImU32 col = (i == active) ? IM_COL32(255, 255, 255, 255) : ImGui::GetColorU32(kGrey);
        ImVec2 ts = ImGui::CalcTextSize(lab[i]->c_str());
        const float isz = h * 0.40f, iw = isz * 1.7f, gap = 12.0f * u;
        const float x = s.x + (half - (iw + gap + ts.x)) * 0.5f;
        DrawIcon(dl, i, x + iw * 0.5f, s.y + h * 0.5f, isz, col, 2.4f * u);
        dl->AddText(ImVec2(x + iw + gap, s.y + (h - ts.y) * 0.5f), col, lab[i]->c_str());
    }
    ImGui::SetCursorScreenPos(after);
    return clicked;
}

static bool ArrowBtn(const char* id, int dir, float w, float h, bool enabled, float u, const Pal& pal) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool act = ImGui::IsItemActive() && enabled;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(act ? pal.rowActive : pal.seg), h * 0.5f);
    const float cx = p.x + w * 0.5f, cy = p.y + h * 0.5f, s = h * 0.2f;
    const float d = (dir < 0 ? 1.0f : -1.0f) * s * 0.5f;
    const ImU32 col = enabled ? IM_COL32(255, 255, 255, 255) : IM_COL32(120, 122, 132, 255);
    dl->AddLine(ImVec2(cx + d, cy - s), ImVec2(cx - d, cy), col, 3.2f * u);
    dl->AddLine(ImVec2(cx - d, cy), ImVec2(cx + d, cy + s), col, 3.2f * u);
    return clicked && enabled;
}

// ---- lista deslizable ----------------------------------------------------------------------------------
enum { R_ACTION, R_SKIN, R_SWITCH, R_VALUE, R_COLORS, R_SLIDER, R_SECTION, R_INFO };
struct Row {
    int kind = R_ACTION;
    std::string label, right;
    ImVec4 col = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    int action = 0;
    std::string arg;
    bool sw = false;             // interruptor encendido
    bool selected = false;       // skin puesta
    float* val = nullptr;        // barra: valor, minimo y maximo
    float lo = 0.0f, hi = 1.0f;
};
struct ListState {
    float scroll = 0.0f, vel = 0.0f, pressScroll = 0.0f;
    bool down = false, dragging = false, onSlider = false;
    int pressRow = -1, sliderRow = -1;
    ImVec2 pressPos;
};
struct ListResult { int clicked = -1; int sub = -1; int sliderReleased = -1; };
static ListState g_lists[2];

static float RowH(const Row& r, float u) {
    switch (r.kind) {
        case R_SECTION: return 44.0f * u;
        case R_SLIDER: return 98.0f * u;
        case R_INFO: return 46.0f * u;
        default: return 64.0f * u;
    }
}
static uint32_t Hash32(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}
static ImU32 AvatarColor(const std::string& name) {
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB((float)(Hash32(name) % 360) / 360.0f, 0.55f, 0.82f, r, g, b);
    return IM_COL32((int)(r * 255.0f), (int)(g * 255.0f), (int)(b * 255.0f), 255);
}

// Dibuja una lista que se desliza con el dedo (con inercia). Toque corto = fila pulsada; arrastre = scroll; las barras se mueven en horizontal.
static ListResult ListView(const char* id, std::vector<Row>& rows, ListState& st, const ImVec2& size, float u, const Pal& pal) {
    ListResult res;
    ImGuiIO& io = ImGui::GetIO();
    const float gap = 10.0f * u, rowW = size.x - 20.0f * u;
    std::vector<float> ys(rows.size() + 1, 0.0f);
    for (size_t i = 0; i < rows.size(); i++) ys[i + 1] = ys[i] + RowH(rows[i], u) + gap;
    const float contentH = ys.back() - gap;
    const float maxScroll = contentH > size.y ? contentH - size.y : 0.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild(id, size, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                       ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImFont* fn = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();

    auto rowAt = [&](float my, float mx) -> int {
        if (mx < p0.x || mx > p0.x + rowW) return -1;
        for (size_t i = 0; i < rows.size(); i++) {
            const float y0 = p0.y + ys[i] - st.scroll;
            if (my >= y0 && my <= y0 + RowH(rows[i], u)) return (int)i;
        }
        return -1;
    };
    auto trackX0 = [&]() { return p0.x + 32.0f * u; };
    auto trackX1 = [&]() { return p0.x + rowW - 32.0f * u; };

    // ---- entrada tactil ----
    const bool inView = io.MousePos.x >= p0.x && io.MousePos.x <= p0.x + size.x && io.MousePos.y >= p0.y && io.MousePos.y <= p0.y + size.y;
    if (io.MouseClicked[0] && inView) {
        st.down = true; st.dragging = false; st.vel = 0.0f; st.onSlider = false;
        st.pressPos = io.MousePos; st.pressScroll = st.scroll;
        st.pressRow = rowAt(io.MousePos.y, io.MousePos.x);
        if (st.pressRow >= 0 && rows[st.pressRow].kind == R_SLIDER) {
            const float y0 = p0.y + ys[st.pressRow] - st.scroll;
            if (io.MousePos.y > y0 + RowH(rows[st.pressRow], u) * 0.34f) { st.onSlider = true; st.sliderRow = st.pressRow; }
        }
    }
    const bool released = st.down && io.MouseReleased[0];
    if (st.down && !released && io.MouseDown[0]) {
        if (st.onSlider) {
            Row& r = rows[st.sliderRow];
            if (r.val) *r.val = r.lo + Clampf((io.MousePos.x - trackX0()) / (trackX1() - trackX0()), 0.0f, 1.0f) * (r.hi - r.lo);
        } else {
            const float dy = io.MousePos.y - st.pressPos.y;
            if (!st.dragging && fabsf(dy) > 14.0f * u) st.dragging = true;
            if (st.dragging) { st.scroll = st.pressScroll - dy; st.vel = -io.MouseDelta.y; }
        }
    }
    if (released) {
        st.down = false;
        if (st.onSlider) res.sliderReleased = st.sliderRow;
        else if (!st.dragging) {
            const int r = rowAt(io.MousePos.y, io.MousePos.x);
            if (r >= 0 && r == st.pressRow) {
                res.clicked = r;
                if (rows[r].kind == R_COLORS) {                      // cual de los 5 cuadros de color
                    const float rh = RowH(rows[r], u), sw = rh * 0.56f, g2 = 12.0f * u;
                    const float cx0 = p0.x + rowW - 20.0f * u - (5.0f * sw + 4.0f * g2);
                    const int k = (int)floorf((io.MousePos.x - cx0 + g2 * 0.5f) / (sw + g2));
                    res.sub = (k >= 0 && k < 5) ? k : -1;
                }
            }
        }
        st.onSlider = false;
    }
    if (!st.down) {                                                  // inercia al soltar
        st.scroll += st.vel;
        st.vel *= 0.93f;
        if (fabsf(st.vel) < 0.4f) st.vel = 0.0f;
    }
    if (st.scroll < 0.0f) { st.scroll = 0.0f; st.vel = 0.0f; }
    if (st.scroll > maxScroll) { st.scroll = maxScroll; st.vel = 0.0f; }

    // ---- dibujo ----
    const ImU32 white = IM_COL32(255, 255, 255, 255), acc = ImGui::GetColorU32(pal.accent), greyU = ImGui::GetColorU32(kGrey);
    for (size_t i = 0; i < rows.size(); i++) {
        const Row& r = rows[i];
        const float h = RowH(r, u), y0 = p0.y + ys[i] - st.scroll;
        if (y0 + h < p0.y || y0 > p0.y + size.y) continue;
        const ImVec2 a(p0.x, y0), b(p0.x + rowW, y0 + h);
        const float midY = y0 + h * 0.5f;
        const bool pressed = st.down && !st.dragging && !st.onSlider && st.pressRow == (int)i;

        if (r.kind == R_SECTION) {
            const float sfs = fs * 0.72f;
            ImVec2 ts = fn->CalcTextSizeA(sfs, 100000.0f, 0.0f, r.label.c_str());
            dl->AddText(fn, sfs, ImVec2(a.x + 6.0f * u, midY - ts.y * 0.5f + 4.0f * u), acc, r.label.c_str());
            dl->AddRectFilled(ImVec2(a.x + ts.x + 20.0f * u, midY + 4.0f * u), ImVec2(b.x, midY + 6.0f * u), IM_COL32(255, 255, 255, 28), 1.0f * u);
            continue;
        }
        if (r.kind == R_INFO) {
            ImVec2 ts = fn->CalcTextSizeA(fs * 0.80f, 100000.0f, 0.0f, r.label.c_str());
            dl->AddText(fn, fs * 0.80f, ImVec2(a.x + 6.0f * u, midY - ts.y * 0.5f), greyU, r.label.c_str());
            continue;
        }
        ImVec4 fill = pressed ? pal.rowActive : (r.selected ? Mix(pal.row, pal.accent, 0.42f) : pal.row);
        if (r.selected || pressed) fill.w = pal.row.w;
        dl->AddRectFilled(a, b, ImGui::GetColorU32(fill), 16.0f * u);
        if (r.selected) dl->AddRectFilled(ImVec2(a.x + 2.0f * u, a.y + 12.0f * u), ImVec2(a.x + 7.0f * u, b.y - 12.0f * u), acc, 3.0f * u);

        float tx = a.x + 22.0f * u;
        float reserve = 22.0f * u;                                   // espacio a la derecha para chip/valor
        if (r.kind == R_SKIN) {
            const float d = h * 0.62f;
            const ImVec2 c(a.x + 22.0f * u + d * 0.5f, midY);
            dl->AddCircleFilled(c, d * 0.5f, AvatarColor(r.label), 32);
            char ch[2] = {(char)((unsigned char)r.label[0] < 128 ? toupper((unsigned char)r.label[0]) : '?'), 0};
            ImVec2 cts = ImGui::CalcTextSize(ch);
            dl->AddText(ImVec2(c.x - cts.x * 0.5f, c.y - cts.y * 0.5f), IM_COL32(20, 20, 26, 255), ch);
            tx = c.x + d * 0.5f + 18.0f * u;
        }
        // elementos de la derecha
        if (r.kind == R_SWITCH) {
            const float w = 82.0f * u, sh = 42.0f * u, x = b.x - w - 20.0f * u, y = midY - sh * 0.5f;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + sh), r.sw ? acc : IM_COL32(74, 76, 88, 255), sh * 0.5f);
            dl->AddCircleFilled(ImVec2(r.sw ? x + w - sh * 0.5f : x + sh * 0.5f, midY), sh * 0.5f - 5.0f * u, white, 24);
            reserve = w + 40.0f * u;
        } else if (r.kind == R_SKIN && !r.right.empty()) {            // pastilla "PUESTA"
            ImVec2 ts = ImGui::CalcTextSize(r.right.c_str());
            const float padx = 16.0f * u, ph2 = ts.y + 14.0f * u, x1 = b.x - 18.0f * u, x0 = x1 - ts.x - padx * 2.0f;
            dl->AddRectFilled(ImVec2(x0, midY - ph2 * 0.5f), ImVec2(x1, midY + ph2 * 0.5f), acc, ph2 * 0.5f);
            dl->AddText(ImVec2(x0 + padx, midY - ts.y * 0.5f), white, r.right.c_str());
            reserve = (x1 - x0) + 36.0f * u;
        } else if (r.kind == R_VALUE) {
            std::string t = r.right + "  >";
            ImVec2 ts = ImGui::CalcTextSize(t.c_str());
            dl->AddText(ImVec2(b.x - ts.x - 20.0f * u, midY - ts.y * 0.5f), greyU, t.c_str());
            reserve = ts.x + 40.0f * u;
        } else if (r.kind == R_COLORS) {
            const float sw = h * 0.56f, g2 = 12.0f * u;
            const float cx0 = b.x - 20.0f * u - (5.0f * sw + 4.0f * g2);
            for (int k = 0; k < 5; k++) {
                const ImVec2 sa(cx0 + k * (sw + g2), midY - sw * 0.5f), sb(sa.x + sw, sa.y + sw);
                dl->AddRectFilled(sa, sb, ImGui::GetColorU32(kAccent[k]), sw * 0.3f);
                if (k == g_theme) dl->AddRect(ImVec2(sa.x - 4.0f * u, sa.y - 4.0f * u), ImVec2(sb.x + 4.0f * u, sb.y + 4.0f * u), white, sw * 0.38f, 0, 3.0f * u);
            }
            reserve = 5.0f * sw + 4.0f * g2 + 40.0f * u;
        }

        if (r.kind == R_SLIDER) {                                    // etiqueta y % arriba, barra abajo
            dl->AddText(ImVec2(tx, a.y + 12.0f * u), white, r.label.c_str());
            char val[16];
            snprintf(val, sizeof val, "%d%%", (int)((r.val ? *r.val : 0.0f) * 100.0f + 0.5f));
            ImVec2 ts = ImGui::CalcTextSize(val);
            dl->AddText(ImVec2(b.x - 22.0f * u - ts.x, a.y + 12.0f * u), greyU, val);
            const float x0 = trackX0(), x1 = trackX1(), ty = b.y - 30.0f * u;
            const float k2 = r.val ? (*r.val - r.lo) / (r.hi - r.lo) : 0.0f, kx = x0 + k2 * (x1 - x0);
            dl->AddRectFilled(ImVec2(x0, ty - 4.0f * u), ImVec2(x1, ty + 4.0f * u), IM_COL32(255, 255, 255, 45), 4.0f * u);
            dl->AddRectFilled(ImVec2(x0, ty - 4.0f * u), ImVec2(kx, ty + 4.0f * u), acc, 4.0f * u);
            dl->AddCircleFilled(ImVec2(kx, ty), 16.0f * u, white, 24);
            dl->AddCircle(ImVec2(kx, ty), 16.0f * u, acc, 24, 3.0f * u);
        } else {
            dl->PushClipRect(a, ImVec2(b.x - reserve, b.y), true);
            ImVec2 ts = ImGui::CalcTextSize(r.label.c_str());
            dl->AddText(ImVec2(tx, midY - ts.y * 0.5f), ImGui::GetColorU32(r.col), r.label.c_str());
            dl->PopClipRect();
        }
    }
    if (maxScroll > 0.0f) {                                          // indicador de scroll
        const float th = fmaxf(48.0f * u, size.y * size.y / contentH);
        const float ty = p0.y + (st.scroll / maxScroll) * (size.y - th);
        dl->AddRectFilled(ImVec2(p0.x + size.x - 9.0f * u, ty), ImVec2(p0.x + size.x - 3.0f * u, ty + th), IM_COL32(255, 255, 255, st.down ? 150 : 80), 3.0f * u);
    }
    ImGui::EndChild();
    return res;
}

static void FloatingButton(float W, float H, float u, const std::string& title, const ImGuiWindowFlags fixed, const Pal& pal) {
    ImGuiIO& io = ImGui::GetIO();
    const float ub = u * g_btnMul;                                   // el boton tiene su propio tamano
    ImFont* fn = ImGui::GetFont();
    const float fs = ImGui::GetFontSize() * g_btnMul;
    ImVec2 ts = fn->CalcTextSizeA(fs, 100000.0f, 0.0f, title.c_str());
    const bool useLogo = (g_logoTex != 0);
    const float bw = useLogo ? 112.0f * ub : ts.x + 124.0f * ub;
    const float bh = useLogo ? 112.0f * ub : 88.0f * ub;
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
    const float r = 20.0f * ub, th3 = 3.0f * ub;
    const ImU32 acc = ImGui::GetColorU32(pal.accent);
    ImVec4 shadow = ImVec4(0.0f, 0.0f, 0.0f, 0.43f * g_alpha);
    if (useLogo) {                                                     // logo redondo de ARCX
        const ImVec2 c(p0.x + bw * 0.5f, p0.y + bh * 0.5f);
        dl->AddCircleFilled(ImVec2(c.x + 3.0f * ub, c.y + 5.0f * ub), bw * 0.46f, ImGui::GetColorU32(shadow), 48);
        const int a = (int)(255.0f * (active ? 1.0f : g_alpha));
        dl->AddImage((ImTextureID)(uintptr_t)g_logoTex, p0, p1, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, a));
        if (active) dl->AddCircle(c, bw * 0.5f, acc, 48, th3);          // aro al pulsar
        AddRect();
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        return;
    }
    dl->AddRectFilled(ImVec2(p0.x + 4.0f * ub, p0.y + 6.0f * ub), ImVec2(p1.x + 4.0f * ub, p1.y + 6.0f * ub), ImGui::GetColorU32(shadow), r);
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(active ? pal.rowActive : pal.bg), r);
    dl->AddRect(p0, p1, acc, r, 0, th3);
    DrawIcon(dl, 0, p0.x + 46.0f * ub, p0.y + bh * 0.5f + 2.0f * ub, 40.0f * ub, acc, th3);
    dl->AddText(fn, fs, ImVec2(p0.x + 92.0f * ub, p0.y + (bh - ts.y) * 0.5f), IM_COL32(255, 255, 255, 255), title.c_str());
    AddRect();
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

static void BuildUI(float W, float H, float u) {
    g_frameRects.clear();
    core::Snapshot snap;
    core::GetSnapshot(snap);
    const Pal pal = MakePal();
    const ImGuiWindowFlags fixed = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGuiIO& io = ImGui::GetIO();

    if (!g_open) { FloatingButton(W, H, u, snap.title, fixed, pal); return; }

    // ---- posicion y tamano del panel (se pueden mover y redimensionar) ----
    g_panW = Clampf(g_panW, 0.28f, 0.95f);
    g_panH = Clampf(g_panH, 0.38f, 0.98f);
    g_panX = Clampf(g_panX, 0.0f, 1.0f - g_panW);
    g_panY = Clampf(g_panY, 0.0f, 1.0f - g_panH);
    const ImVec2 pos(g_panX * W, g_panY * H), psz(g_panW * W, g_panH * H);

    // sombra detras del panel
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(pos.x + 2.0f * u, pos.y + 10.0f * u), ImVec2(pos.x + psz.x + 2.0f * u, pos.y + psz.y + 10.0f * u),
                                                  IM_COL32(0, 0, 0, (int)(120.0f * g_alpha)), 26.0f * u);

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(psz);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f * u);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * u, 14.0f * u));
    ImGui::PushStyleColor(ImGuiCol_Border, pal.accent);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, pal.bg);
    ImGui::Begin("##arcx_panel", nullptr, fixed);
    const float pad = ImGui::GetStyle().WindowPadding.x, padY = ImGui::GetStyle().WindowPadding.y;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    ImFont* fn = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();

    // ---- cabecera: logo, nombre, credito y cerrar; arrastrar la cabecera mueve el panel ----
    const float hh = 64.0f * u;
    const ImVec2 hp(wp.x + pad, wp.y + padY);
    if (g_logoTex) dl->AddImage((ImTextureID)(uintptr_t)g_logoTex, hp, ImVec2(hp.x + hh, hp.y + hh));
    else DrawIcon(dl, 0, hp.x + hh * 0.5f, hp.y + hh * 0.55f, hh * 0.6f, ImGui::GetColorU32(pal.accent), 3.0f * u);
    dl->AddText(fn, fs * 1.12f, ImVec2(hp.x + hh + 16.0f * u, hp.y + 2.0f * u), ImGui::GetColorU32(pal.title), snap.title.c_str());
    dl->AddText(fn, fs * 0.66f, ImVec2(hp.x + hh + 16.0f * u, hp.y + 36.0f * u), ImGui::GetColorU32(kGrey), "by Hunter09");
    const float xr = 27.0f * u;                                       // boton cerrar (circulo con X)
    const ImVec2 xc(wp.x + psz.x - pad - xr, hp.y + hh * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(xc.x - xr, xc.y - xr));
    const bool closePressed = ImGui::InvisibleButton("##close", ImVec2(xr * 2.0f, xr * 2.0f));
    const bool closeActive = ImGui::IsItemActive();
    dl->AddCircleFilled(xc, xr, closeActive ? IM_COL32(210, 60, 60, 255) : IM_COL32(150, 36, 36, 255), 32);
    dl->AddLine(ImVec2(xc.x - xr * 0.34f, xc.y - xr * 0.34f), ImVec2(xc.x + xr * 0.34f, xc.y + xr * 0.34f), IM_COL32(255, 255, 255, 255), 3.0f * u);
    dl->AddLine(ImVec2(xc.x + xr * 0.34f, xc.y - xr * 0.34f), ImVec2(xc.x - xr * 0.34f, xc.y + xr * 0.34f), IM_COL32(255, 255, 255, 255), 3.0f * u);
    if (closePressed) g_open = false;
    static bool movedPanel = false;
    ImGui::SetCursorScreenPos(hp);
    ImGui::InvisibleButton("##hdr", ImVec2(psz.x - 2.0f * pad - xr * 2.0f - 12.0f * u, hh));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 6.0f * u)) {
        g_panX += io.MouseDelta.x / W;
        g_panY += io.MouseDelta.y / H;
        movedPanel = true;
    }
    if (ImGui::IsItemDeactivated()) { if (movedPanel) SaveMenuSettings(); movedPanel = false; }

    // ---- pestanas SKINS | AJUSTES ----
    ImGui::SetCursorScreenPos(ImVec2(wp.x + pad, hp.y + hh + 12.0f * u));
    {
        int c = Segmented(Tr(K_TAB_SKINS), Tr(K_TAB_SET), g_screen, psz.x - 2.0f * pad, 58.0f * u, u, pal);
        if (c >= 0 && c != g_screen) { g_screen = c; g_page = 0; }
    }

    // ---- lista (deslizable) ----
    const float footH = 54.0f * u;
    const float listTop = ImGui::GetCursorScreenPos().y + 6.0f * u;
    const float listBottom = wp.y + psz.y - padY - footH - 6.0f * u;
    const ImVec2 listSize(psz.x - 2.0f * pad, listBottom - listTop > 80.0f * u ? listBottom - listTop : 80.0f * u);
    int pages = 1;
    std::vector<Row> rows;
    if (g_screen == 0) {
        const int total = (int)snap.skins.size();
        pages = (total + kMaxRowsPerPage - 1) / kMaxRowsPerPage;
        if (pages < 1) pages = 1;
        if (g_page >= pages) g_page = pages - 1;
        if (g_page < 0) g_page = 0;
        Row cj; cj.kind = R_ACTION; cj.label = Tr(K_CJ); cj.col = kRed; cj.action = 2; rows.push_back(cj);
        for (int i = g_page * kMaxRowsPerPage; i < total && i < (g_page + 1) * kMaxRowsPerPage; i++) {
            Row r; r.kind = R_SKIN; r.label = snap.skins[i]; r.action = 1; r.arg = snap.skins[i];
            r.selected = (snap.skins[i] == snap.current);
            if (r.selected) r.right = Tr(K_WORN);
            rows.push_back(r);
        }
        if (total == 0) { Row r; r.kind = R_INFO; r.label = Tr(K_NOSKINS); rows.push_back(r); }
    } else {
        g_page = 0;
        auto section = [&](int k) { Row r; r.kind = R_SECTION; r.label = Tr(k); rows.push_back(r); };
        section(K_SEC_GENERAL);
        { Row r; r.kind = R_SWITCH; r.label = Tr(K_REMEMBER); r.action = 3; r.sw = snap.remember; rows.push_back(r); }
        { Row r; r.kind = R_SWITCH; r.label = Tr(K_LOCK); r.action = 11; r.sw = g_btnLock; rows.push_back(r); }
        { Row r; r.kind = R_ACTION; r.label = Tr(K_RESCAN); r.action = 4; rows.push_back(r); }
        section(K_SEC_LOOK);
        { Row r; r.kind = R_VALUE; r.label = Tr(K_LANG); r.right = Loc(kLangName[g_lang]); r.action = 5; rows.push_back(r); }
        { Row r; r.kind = R_COLORS; r.label = Tr(K_THEME) + ": " + Loc(kThemeName[g_lang][g_theme]); r.action = 7; rows.push_back(r); }
        { Row r; r.kind = R_SLIDER; r.label = Tr(K_BTNSIZE); r.action = 13; r.val = &g_btnMul; r.lo = kBtnLo; r.hi = kBtnHi; rows.push_back(r); }
        { Row r; r.kind = R_SLIDER; r.label = Tr(K_OPACITY); r.action = 14; r.val = &g_alpha; r.lo = kAlphaLo; r.hi = kAlphaHi; rows.push_back(r); }
        section(K_SEC_ABOUT);
        { Row r; r.kind = R_INFO; r.label = "Project ARCX (beta)  -  by Hunter09"; rows.push_back(r); }
    }
    ImGui::SetCursorScreenPos(ImVec2(wp.x + pad, listTop));
    ListResult lr = ListView(g_screen == 0 ? "##skins" : "##ajustes", rows, g_lists[g_screen], listSize, u, pal);
    if (lr.clicked >= 0) {
        const Row& r = rows[lr.clicked];
        if (r.action == 1) core::Command(r.arg);
        else if (r.action == 2) core::Command("CJ");
        else if (r.action == 3) core::Command(snap.remember ? "REMEMBER_OFF" : "REMEMBER_ON");
        else if (r.action == 4) core::Command("RESCAN");
        else if (r.action == 5) { g_lang = (g_lang + 1) % 3; SaveMenuSettings(); }
        else if (r.action == 7 && lr.sub >= 0) { g_theme = lr.sub; SaveMenuSettings(); }
        else if (r.action == 11) { g_btnLock = !g_btnLock; SaveMenuSettings(); }
    }
    if (lr.sliderReleased >= 0) SaveMenuSettings();

    // ---- pie: pista (izquierda), paginas si hacen falta y esquina para redimensionar ----
    const float footY = wp.y + psz.y - padY - footH;
    {
        char hint[160];
        if (!snap.status.empty()) snprintf(hint, sizeof hint, Loc(kText[g_lang][K_WAIT]).c_str(), StatusText(snap.status).c_str());
        else if (g_screen == 0) snprintf(hint, sizeof hint, Tr(K_COUNT_FMT).c_str(), (int)snap.skins.size());
        else snprintf(hint, sizeof hint, "%s", Tr(K_HINT).c_str());
        const ImU32 hcol = snap.status.empty() ? ImGui::GetColorU32(kGrey) : IM_COL32(255, 178, 64, 255);
        dl->AddText(fn, fs * 0.74f, ImVec2(wp.x + pad, footY + footH * 0.5f - fs * 0.37f), hcol, hint);
    }
    const float gripS = 50.0f * u;
    const ImVec2 gp(wp.x + psz.x - gripS - 8.0f * u, wp.y + psz.y - gripS - 8.0f * u);
    if (pages > 1) {
        const float aw = 70.0f * u, lw = 110.0f * u, ah = 46.0f * u, sp = 8.0f * u;
        const float x0 = gp.x - 14.0f * u - (aw * 2.0f + lw + sp * 2.0f);
        const float y0 = footY + (footH - ah) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(x0, y0));
        if (ArrowBtn("##prev", -1, aw, ah, true, u, pal)) g_page = (g_page + pages - 1) % pages;
        ImGui::SetCursorScreenPos(ImVec2(x0 + aw + lw + sp * 2.0f, y0));
        if (ArrowBtn("##next", 1, aw, ah, true, u, pal)) g_page = (g_page + 1) % pages;
        char pg[24];
        snprintf(pg, sizeof pg, "%d / %d", g_page + 1, pages);
        ImVec2 ts = ImGui::CalcTextSize(pg);
        dl->AddText(ImVec2(x0 + aw + sp + (lw - ts.x) * 0.5f, y0 + (ah - ts.y) * 0.5f), ImGui::GetColorU32(kGrey), pg);
    }
    // esquina de redimensionado (como en ARM CheatMenu): tres rayas en diagonal
    ImGui::SetCursorScreenPos(gp);
    ImGui::InvisibleButton("##grip", ImVec2(gripS, gripS));
    static ImVec2 grabOff;
    static bool resized = false;
    if (ImGui::IsItemActivated()) grabOff = ImVec2(pos.x + psz.x - io.MousePos.x, pos.y + psz.y - io.MousePos.y);
    if (ImGui::IsItemActive()) {
        g_panW = Clampf((io.MousePos.x + grabOff.x - pos.x) / W, 0.28f, 0.95f - g_panX);
        g_panH = Clampf((io.MousePos.y + grabOff.y - pos.y) / H, 0.38f, 0.98f - g_panY);
        resized = true;
    }
    if (ImGui::IsItemDeactivated()) { if (resized) SaveMenuSettings(); resized = false; }
    {
        const ImU32 gc = ImGui::IsItemActive() ? ImGui::GetColorU32(pal.accent) : IM_COL32(190, 192, 205, 200);
        const float e = gripS * 0.82f;
        for (int k = 0; k < 3; k++) {
            const float o = e * (0.30f + 0.28f * (float)k);
            dl->AddLine(ImVec2(gp.x + gripS - 6.0f * u, gp.y + gripS - o), ImVec2(gp.x + gripS - o, gp.y + gripS - 6.0f * u), gc, 3.0f * u);
        }
    }
    AddRect();
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

// ---- dibujo (hilo de render, justo antes de eglSwapBuffers) --------------------------------------
static bool g_inited = false, g_failed = false;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static std::chrono::steady_clock::time_point g_last;
static EGLBoolean (*eglSwapBuffers_orig)(EGLDisplay, EGLSurface);
static const float kInitDelaySec = 1.5f;      // el boton espera 1.5 s con el jugador en juego: carga a la par de los widgets del HUD (por tiempo, no por fotogramas: el juego va a pocos FPS)

static void ApplyStyle(float H) {
    const float px = kFontPx * (H / 1080.0f);
    const float u = px / 30.0f;
    ImGuiIO& io = ImGui::GetIO();
    LoadFont(px);                                   // letra horneada al tamano real: nitida, sin escalar
    io.FontGlobalScale = g_hasTtf ? 1.0f : px / 13.0f;
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark();
    st.ScaleAllSizes(px / 13.0f);
    st.WindowRounding = 24.0f * u;                  // esquinas redondeadas
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

static int g_swaps = 0, g_drawn = 0, g_noPlayer = 0, g_otherCtx = 0;

static void DrawMenu(EGLDisplay dpy, EGLSurface surf) {
    if (g_failed) return;
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return;
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 200 || h < 200) return;

    const bool paused = (core::g_pauseUser && *core::g_pauseUser) || (core::g_pauseCode && *core::g_pauseCode);
    static int lastPaused = -1;
    if ((int)paused != lastPaused) { lastPaused = (int)paused; Log("pausa: %d", (int)paused); }
    const bool havePlayer = !core::FindPlayerPed || core::FindPlayerPed(-1) != nullptr;
    static int lastHave = -1;
    if ((int)havePlayer != lastHave) { lastHave = (int)havePlayer; Log("jugador %s", havePlayer ? "presente" : "ausente"); }

    // carga diferida: nada se crea (contexto de ImGui, fuente, logo) hasta que el jugador lleva 1.5 s seguido en juego, a la par de los widgets del juego
    static bool playerTimed = false;
    static std::chrono::steady_clock::time_point playerSince;
    if (!g_inited) {
        if (!havePlayer || paused) { playerTimed = false; return; }
        if (!playerTimed) { playerTimed = true; playerSince = std::chrono::steady_clock::now(); return; }
        if (std::chrono::duration<float>(std::chrono::steady_clock::now() - playerSince).count() < kInitDelaySec) return;
        g_ctx = ctx;
        g_bindVao = (PFNBINDVAO)eglGetProcAddress("glBindVertexArray");
        if (!g_bindVao) g_bindVao = (PFNBINDVAO)eglGetProcAddress("glBindVertexArrayOES");
        if (!InitImGui((float)w, (float)h)) { g_failed = true; return; }
        g_inited = true;
        g_last = std::chrono::steady_clock::now();
        Log("VAO disponible: %s", g_bindVao ? "si" : "no");
        CreateLogoTexture();
        LogGlState("inicio", ctx, w, h);
    }
    if (ctx != g_ctx) {
        g_otherCtx++;
        if (g_otherCtx <= 8) Log("contexto distinto al de ImGui: actual=%p ImGui=%p hilo=%d", ctx, g_ctx, (int)syscall(SYS_gettid));
        return;
    }
    if (paused) {                       // en el menu de pausa no se muestra ni el boton ni el panel
        std::lock_guard<std::mutex> lk(g_inMtx);
        g_rects.clear();
        return;
    }
    if (!havePlayer) {
        g_noPlayer++;
        g_open = false;
        std::lock_guard<std::mutex> lk(g_inMtx);
        g_rects.clear();
        return;
    }

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

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    BuildUI((float)w, (float)h, kFontPx * ((float)h / 1080.0f) / 30.0f);
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    ImGui_ImplOpenGL3_RenderDrawData(dd);

    GLenum err = glGetError();
    static int errLogs = 0;
    if (err != GL_NO_ERROR && errLogs++ < 10) Log("glGetError tras dibujar = 0x%x", err);
    static int nFrames = 0;
    if (nFrames < 3 || nFrames % 1200 == 0) {
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

// Pone los hooks de toque y de dibujo; no crea nada de ImGui (eso se hace mas tarde, con el jugador ya en juego).
inline void Init() {
    LoadMenuSettings();
    void* h = aml->GetLibHandle("libGTASA.so");
    void* touch = (void*)aml->GetSym(h, "_Z14AND_TouchEventiiii");
    Log("  %s AND_TouchEvent", touch ? "ok" : "FALTA");
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

}  // namespace ui
