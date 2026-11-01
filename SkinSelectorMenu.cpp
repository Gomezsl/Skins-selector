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
#include <unistd.h>
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

MYMODCFG(net.local.skinselectormenu, Skin Selector Menu, 1.0.0, Tu nombre)
NEEDGAME(com.rockstargames.gtasa)

#define TAG "SkinSelectorMenu"

// ---- rutas y log ---------------------------------------------------------------------------------
static const char* kFilesDirs[] = {
    "/storage/emulated/0/Android_unprotected/data/com.rockstargames.gtasa/files",
    "/storage/emulated/0/Android/data/com.rockstargames.gtasa/files",
    "/sdcard/Android/data/com.rockstargames.gtasa/files",
    "/storage/self/primary/Android/data/com.rockstargames.gtasa/files",
};
static std::string g_plugDir;   // .../files/SkinSelector

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
static std::vector<std::string> g_names;
static std::string g_current = "CJ";
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
    std::vector<std::string> names;
    FILE* f = fopen((g_plugDir + "/skins_list.txt").c_str(), "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            if (line[0] == '#') continue;
            std::string l = line;
            size_t eq = l.find(" = ");
            size_t pa = l.find("   (/");
            if (eq == std::string::npos || pa == std::string::npos || pa < eq + 3) continue;
            names.push_back(l.substr(eq + 3, pa - eq - 3));
        }
        fclose(f);
    }
    std::string cur = ReadFirstLine(g_plugDir + "/current.txt");
    std::lock_guard<std::mutex> lk(g_listMtx);
    g_names = names;
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

static void AddRect() {
    ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    float m = 12.0f;
    g_frameRects.push_back({p.x - m, p.y - m, p.x + s.x + m, p.y + s.y + m});
}

static void BuildUI(float W, float H, float k) {
    g_frameRects.clear();
    const ImGuiWindowFlags fixed = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::SetNextWindowPos(ImVec2(10.0f * k, H * 0.30f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##skbtn", nullptr, fixed | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoScrollbar);
    if (ImGui::Button(g_open ? " X " : "SKINS", ImVec2(0, 0))) g_open = !g_open;
    AddRect();
    ImGui::End();

    if (!g_open) return;

    std::vector<std::string> names;
    std::string cur;
    { std::lock_guard<std::mutex> lk(g_listMtx); names = g_names; cur = g_current; }

    const int perPage = 5;
    int pages = (int)((names.size() + perPage - 1) / perPage);
    if (pages < 1) pages = 1;
    if (g_page >= pages) g_page = pages - 1;
    if (g_page < 0) g_page = 0;

    ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(W * 0.46f, H * 0.86f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::Begin("Skins", nullptr, fixed | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);
    const float bh = 78.0f * k;
    const ImVec4 on(0.16f, 0.55f, 0.20f, 1.0f);

    bool isCJ = (cur == "CJ");
    if (isCJ) ImGui::PushStyleColor(ImGuiCol_Button, on);
    if (ImGui::Button("CJ (original)", ImVec2(-1, bh))) Request("CJ");
    if (isCJ) ImGui::PopStyleColor();

    for (int i = g_page * perPage; i < (int)names.size() && i < (g_page + 1) * perPage; i++) {
        bool sel = (names[i] == cur);
        ImGui::PushID(i);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, on);
        if (ImGui::Button(names[i].c_str(), ImVec2(-1, bh))) Request(names[i]);
        if (sel) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    if (names.empty()) ImGui::Text("Sin skins (revisa skins_list.txt)");

    ImGui::Separator();
    float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
    if (ImGui::Button("<", ImVec2(half, bh * 0.8f))) g_page = (g_page + pages - 1) % pages;
    ImGui::SameLine();
    char pg[32];
    snprintf(pg, sizeof pg, "%d / %d", g_page + 1, pages);
    ImGui::Button(pg, ImVec2(half, bh * 0.8f));
    ImGui::SameLine();
    if (ImGui::Button(">", ImVec2(half, bh * 0.8f))) g_page = (g_page + 1) % pages;
    if (ImGui::Button("Cerrar", ImVec2(-1, bh * 0.8f))) g_open = false;
    AddRect();
    ImGui::End();
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
        if (!InitImGui((float)w, (float)h)) { g_failed = true; return; }
        g_inited = true;
        g_last = std::chrono::steady_clock::now();
    }
    if (ctx != g_ctx) return;
    if (FindPlayerPed && !FindPlayerPed(-1)) { g_open = false; std::lock_guard<std::mutex> lk(g_inMtx); g_rects.clear(); return; }

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

    GLint lastElem = 0, lastAttr[8] = {0};
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &lastElem);
    for (int i = 0; i < 8; i++) glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &lastAttr[i]);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    BuildUI((float)w, (float)h, h / 1080.0f);
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    ImGui_ImplOpenGL3_RenderDrawData(dd);
    static int nFrames = 0;
    if (nFrames < 3 || nFrames % 1200 == 0)
        Log("frame %d: vertices=%d rects=%zu abierto=%d", nFrames, dd ? dd->TotalVtxCount : -1, g_frameRects.size(), (int)g_open);
    nFrames++;

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)lastElem);
    for (int i = 0; i < 8; i++) {
        if (lastAttr[i]) glEnableVertexAttribArray(i); else glDisableVertexAttribArray(i);
    }
    { std::lock_guard<std::mutex> lk(g_inMtx); g_rects = g_frameRects; }
}

static EGLBoolean eglSwapBuffers_hook(EGLDisplay d, EGLSurface s) {
    DrawMenu(d, s);
    return eglSwapBuffers_orig(d, s);
}

extern "C" void OnModLoad() {
    std::string files;
    for (auto d : kFilesDirs) {
        struct stat st;
        if (stat((std::string(d) + "/modloader").c_str(), &st) == 0) { files = d; break; }
    }
    if (files.empty()) files = kFilesDirs[0];
    g_plugDir = files + "/SkinSelector";
    mkdir(g_plugDir.c_str(), 0777);
    FILE* lf = fopen((g_plugDir + "/menu_log.txt").c_str(), "w"); if (lf) fclose(lf);
    Log("== SkinSelectorMenu 1.0.1 cargado ==");

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
