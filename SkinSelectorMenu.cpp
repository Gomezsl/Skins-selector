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

MYMODCFG(net.local.vestidorsamenu, Vestidor SA Menu, 1.5.0, Tu nombre)
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

// ---- interfaz ------------------------------------------------------------------------------------
static bool g_open = false;
static int g_page = 0;
static void* (*FindPlayerPed)(int);
static std::vector<Rect> g_frameRects;

struct Item { std::string label, right; ImVec4 col; int action; std::string arg; };
static const ImVec4 kWhite(1.0f, 1.0f, 1.0f, 1.0f);
static const ImVec4 kGreen(0.45f, 0.95f, 0.50f, 1.0f);
static const ImVec4 kRed(0.97f, 0.42f, 0.38f, 1.0f);
static const ImVec4 kGold(0.96f, 0.74f, 0.18f, 1.0f);
static const ImVec4 kGrey(0.78f, 0.78f, 0.80f, 1.0f);

static void AddRect() {
    ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    float m = 12.0f;
    g_frameRects.push_back({p.x - m, p.y - m, p.x + s.x + m, p.y + s.y + m});
}

// fila tipo lista: texto a la izquierda y, si hay, un texto a la derecha (">" o "PUESTA")
static bool Row(const Item& it, float rh, float k) {
    ImGui::PushStyleColor(ImGuiCol_Text, it.col);
    bool clicked = ImGui::Button(it.label.c_str(), ImVec2(-1, rh));
    ImGui::PopStyleColor();
    if (!it.right.empty()) {
        ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        ImVec2 ts = ImGui::CalcTextSize(it.right.c_str());
        ImGui::GetWindowDrawList()->AddText(ImVec2(mx.x - ts.x - 16.0f * k, mn.y + (mx.y - mn.y - ts.y) * 0.5f),
                                            ImGui::GetColorU32(it.col), it.right.c_str());
    }
    return clicked;
}

static int g_screen = 0;          // 0 = skins, 1 = ajustes

static void BuildUI(float W, float H, float k) {
    g_frameRects.clear();
    std::vector<std::string> skins;
    std::string cur, title;
    bool rem;
    { std::lock_guard<std::mutex> lk(g_listMtx); skins = g_skinsM; cur = g_current; title = g_title; rem = g_remember; }

    const ImGuiWindowFlags fixed = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    // ---- menu cerrado: solo el boton con el nombre, en el borde izquierdo ----
    if (!g_open) {
        ImGui::SetNextWindowPos(ImVec2(10.0f * k, H * 0.30f));
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("##vbtn", nullptr, fixed | ImGuiWindowFlags_AlwaysAutoResize);
        if (ImGui::Button(title.c_str())) { g_open = true; g_screen = 0; g_page = 0; }
        AddRect();
        ImGui::End();
        return;
    }

    const bool settings = (g_screen == 1);

    // ---- elementos de la lista ----
    std::vector<Item> items;
    if (!settings) {
        items.push_back({"Volver a ser CJ", "", kRed, 2, ""});
        for (auto& s : skins) {
            bool on = (s == cur);
            items.push_back({s, on ? "PUESTA" : "", on ? kGreen : kWhite, 1, s});
        }
    } else {
        items.push_back({std::string("Recordar ultima skin: ") + (rem ? "SI" : "NO"), "", kWhite, 3, ""});
        items.push_back({"Releer carpeta de skins", "", kWhite, 4, ""});
        items.push_back({"Skin en misiones: siempre activa", "", kGrey, 9, ""});
    }
    const int perPage = 6;
    int pages = ((int)items.size() + perPage - 1) / perPage;
    if (pages < 1) pages = 1;
    if (g_page >= pages) g_page = pages - 1;
    if (g_page < 0) g_page = 0;

    // ---- panel principal (izquierda) ----
    const float pw = W * 0.43f, ph = H * 0.69f;
    ImGui::SetNextWindowPos(ImVec2(W * 0.02f, H * 0.085f));
    ImGui::SetNextWindowSize(ImVec2(pw, ph));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.04f, 0.04f, 0.05f, 0.86f));
    ImGui::Begin("##vpanel", nullptr, fixed);
    const float pad = ImGui::GetStyle().WindowPadding.x;

    ImGui::TextColored(kGold, "%s", settings ? (title + "  -  AJUSTES").c_str() : title.c_str());
    {
        float xw = 54.0f * k;
        ImGui::SameLine(pw - xw - pad);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.09f, 0.09f, 1.0f));
        if (ImGui::Button("X", ImVec2(xw, 0))) g_open = false;
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();

    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.11f, 0.11f, 0.12f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.20f, 0.22f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.30f, 0.30f, 0.33f, 1.0f));
    const float rh = 56.0f * k;
    for (int i = g_page * perPage; i < (int)items.size() && i < (g_page + 1) * perPage; i++) {
        ImGui::PushID(i);
        if (Row(items[i], rh, k)) {
            const Item& it = items[i];
            if (it.action == 1) Request(it.arg);
            else if (it.action == 2) Request("CJ");
            else if (it.action == 3) {
                { std::lock_guard<std::mutex> lk(g_listMtx); g_remember = !rem; g_lastRead = time(nullptr); }
                Request(rem ? "REMEMBER_OFF" : "REMEMBER_ON");
            }
            else if (it.action == 4) Request("RESCAN");
        }
        ImGui::PopID();
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();

    // ---- pie: pista, ATRAS y AJUSTES ----
    const float bh = 54.0f * k, bw = pw * 0.34f;
    char hint[64];
    if (pages > 1) snprintf(hint, sizeof hint, "Toca una opcion   (%d/%d)", g_page + 1, pages);
    else snprintf(hint, sizeof hint, "Toca una opcion");
    ImGui::SetCursorPos(ImVec2(pad, ImGui::GetWindowHeight() - bh - pad - 30.0f * k));
    ImGui::SetWindowFontScale(0.7f);
    ImGui::TextColored(kGrey, "%s", hint);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::SetCursorPos(ImVec2(pad, ImGui::GetWindowHeight() - bh - pad));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.22f, 0.24f, 1.0f));
    if (ImGui::Button("< ATRAS", ImVec2(bw, bh))) {
        if (settings) { g_screen = 0; g_page = 0; }      // de ajustes vuelve a las skins
        else g_open = false;                             // de las skins cierra el menu
    }
    if (!settings) {
        ImGui::SetCursorPos(ImVec2(pw - pad - bw, ImGui::GetWindowHeight() - bh - pad));
        if (ImGui::Button("AJUSTES", ImVec2(bw, bh))) { g_screen = 1; g_page = 0; }
    }
    ImGui::PopStyleColor();
    AddRect();
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    // ---- botones de pagina "<" ">": ver mas skins (abajo a la izquierda, fuera del panel) ----
    ImGui::SetNextWindowPos(ImVec2(W * 0.02f, H * 0.80f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::Begin("##vnav", nullptr, fixed | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f * k);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.85f, 0.85f, 0.85f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.88f));
    ImGui::PushStyleColor(ImGuiCol_Text, pages > 1 ? kWhite : ImVec4(0.45f, 0.45f, 0.47f, 1.0f));
    if (ImGui::Button("<", ImVec2(W * 0.095f, H * 0.13f)) && pages > 1) g_page = (g_page + pages - 1) % pages;
    ImGui::SameLine();
    if (ImGui::Button(">", ImVec2(W * 0.095f, H * 0.13f)) && pages > 1) g_page = (g_page + 1) % pages;
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    AddRect();
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    // ---- SALIR (arriba a la derecha) ----
    ImGui::SetNextWindowPos(ImVec2(W * 0.985f, H * 0.03f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::Begin("##vexit", nullptr, fixed | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f * k);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.80f, 0.22f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.40f, 0.07f, 0.07f, 0.95f));
    if (ImGui::Button("SALIR", ImVec2(W * 0.13f, H * 0.085f))) g_open = false;
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    AddRect();
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ---- dibujo (hilo de render, justo antes de eglSwapBuffers) --------------------------------------
static bool g_inited = false, g_failed = false;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static std::chrono::steady_clock::time_point g_last;
static EGLBoolean (*eglSwapBuffers_orig)(EGLDisplay, EGLSurface);

static bool InitImGui(float W, float H) {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    float k = H / 1080.0f;
    ImGui::GetStyle().ScaleAllSizes(2.3f * k);
    io.FontGlobalScale = 2.3f * k;
    if (!ImGui_ImplOpenGL3_Init(nullptr)) { Log("ImGui_ImplOpenGL3_Init fallo"); return false; }
    Log("ImGui listo (%.0fx%.0f)", W, H);
    return true;
}

typedef void (*PFNBINDVAO)(GLuint);
static PFNBINDVAO g_bindVao = nullptr;      // glBindVertexArray (si el juego usa ES3)
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
    GLint maxAttr = 8, lastElem = 0, lastVao = 0, lastAttr[16] = {0};
    GLboolean cm[4] = {1, 1, 1, 1};
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxAttr);
    if (maxAttr > 16) maxAttr = 16;
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &lastElem);
    for (int i = 0; i < maxAttr; i++) glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &lastAttr[i]);
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    if (g_bindVao) { glGetIntegerv(0x85B5 /* GL_VERTEX_ARRAY_BINDING */, &lastVao); g_bindVao(0); }
    for (int i = 0; i < maxAttr; i++) glDisableVertexAttribArray(i);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    while (glGetError() != GL_NO_ERROR) {}

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    BuildUI((float)w, (float)h, h / 1080.0f);
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
    if (g_bindVao) g_bindVao((GLuint)lastVao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)lastElem);
    for (int i = 0; i < maxAttr; i++) {
        if (lastAttr[i]) glEnableVertexAttribArray(i); else glDisableVertexAttribArray(i);
    }
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
    Log("== Vestidor SA Menu 1.5.0 cargado ==");

    void* h = aml->GetLibHandle("libGTASA.so");
    *(void**)&FindPlayerPed = (void*)aml->GetSym(h, "_Z13FindPlayerPedi");
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
