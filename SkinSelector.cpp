// SkinSelector.cpp - plugin AML (arm64, GTA SA 2.10) que lista las skins de modloader/ y se las pone al jugador.
// ZyZ ModLoader se encarga de cargar el .dff/.txd/.png; este plugin solo detecta cual es una skin y la aplica.
#include <mod/config.h>
#include <mod/amlmod.h>
#include <android/log.h>
#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>
#include "skinscan.h"

#if __has_include("isautils.h")
  #include "isautils.h"      // opcional: menu de SAUtils (copialo de su repo si lo quieres)
  #define HAVE_SAUTILS 1
#endif

MYMODCFG(net.local.skinselector, Skin Selector, 1.0.0, Tu nombre)
NEEDGAME(com.rockstargames.gtasa)

#define TAG "SkinSelector"

// ---- rutas -------------------------------------------------------------------------------------
// Carpeta "files" del juego. En algunos telefonos es Android_unprotected (asi lo muestra el log de ZyZ).
static const char* kFilesDirs[] = {
    "/storage/emulated/0/Android_unprotected/data/com.rockstargames.gtasa/files",
    "/storage/emulated/0/Android/data/com.rockstargames.gtasa/files",
    "/sdcard/Android/data/com.rockstargames.gtasa/files",
    "/storage/self/primary/Android/data/com.rockstargames.gtasa/files",
};
static std::string g_root;       // .../files/modloader
static std::string g_plugDir;    // .../files/SkinSelector  (skins_list.txt, request.txt, log.txt)

static void Log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, TAG, "%s", buf);
    if (!g_plugDir.empty()) {
        FILE* f = fopen((g_plugDir + "/log.txt").c_str(), "a");
        if (f) { fputs(buf, f); fputc('\n', f); fclose(f); }
    }
}
#define LOGI(...) Log(__VA_ARGS__)

// ---- funciones del juego (libGTASA.so 2.10 arm64) -----------------------------------------------
static void  (*RequestSpecialChar)(int, const char*, int);
static void  (*LoadAllRequestedModels)(bool);
static bool  (*HasSpecialCharLoaded)(int);
static void  (*SetMissionDoesntRequireSpecialChar)(int);
static void* (*FindPlayerPed)(int);
static void  (*SetModelIndex)(void*, unsigned int);   // CPed::SetModelIndex
static void  (*DeleteRwObject)(void*);                // CEntity::DeleteRwObject (opcional)
static void  (*RebuildPlayer)(void*, bool);           // CClothes::RebuildPlayer (opcional, para volver a CJ)
static void  (*CGame_Process_orig)();

static const int kSlot = 1;  // slot de personaje especial 1 -> modelo 290

// ---- estado --------------------------------------------------------------------------------------
static std::vector<skinscan::Skin> g_skins;
static std::atomic<int> g_pending{-2};   // -2 nada, -1 volver a CJ, >=0 indice en g_skins
static int g_frame = 0;

static void WriteList() {
    std::string p = g_plugDir + "/skins_list.txt";
    FILE* f = fopen(p.c_str(), "w");
    if (!f) return;
    fprintf(f, "# Escribe en request.txt el nombre o el numero de una skin (CJ = volver a CJ, RESCAN = releer carpeta)\n");
    for (size_t i = 0; i < g_skins.size(); i++)
        fprintf(f, "%zu = %s   (%s)\n", i + 1, g_skins[i].name.c_str(), g_skins[i].path.c_str());
    fclose(f);
}
static void Rescan() {
    g_skins.clear();
    std::vector<std::string> rejected;
    skinscan::Scan(g_root, g_skins, &rejected);
    LOGI("%zu skin(s) en %s", g_skins.size(), g_root.c_str());
    for (auto& k : g_skins) LOGI("  + skin: %s", k.name.c_str());
    for (auto& r : rejected) LOGI("  - .dff sin chunk de skin (no es ped, se ignora): %s", r.c_str());
    WriteList();
}

// Debe llamarse en el hilo del juego (lo hacemos desde el hook de CGame::Process).
static bool ApplySkin(int idx) {
    void* ped = FindPlayerPed(-1);
    if (!ped) return false;                       // aun no hay jugador (menu / carga): se reintenta
    if (idx < 0) {                                // volver a CJ
        if (DeleteRwObject) DeleteRwObject(ped);
        SetModelIndex(ped, 0);
        if (RebuildPlayer) RebuildPlayer(ped, false);
        LOGI("skin: CJ");
        return true;
    }
    if (idx >= (int)g_skins.size()) return true;
    const std::string& name = g_skins[idx].name;
    RequestSpecialChar(kSlot, name.c_str(), 0);
    LoadAllRequestedModels(false);
    if (!HasSpecialCharLoaded(kSlot)) {
        LOGI("skin '%s' no cargo (mira ZyZModLoader.log: nombre, .dff valido, textura)", name.c_str());
        return true;
    }
    if (DeleteRwObject) DeleteRwObject(ped);
    SetModelIndex(ped, 289u + kSlot);
    SetMissionDoesntRequireSpecialChar(kSlot);
    LOGI("skin: %s", name.c_str());
    return true;
}

static void PollRequestFile() {
    std::string p = g_plugDir + "/request.txt";
    FILE* f = fopen(p.c_str(), "r");
    if (!f) return;
    char buf[128] = {0};
    if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
    fclose(f);
    unlink(p.c_str());
    std::string s = buf;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    std::string l = skinscan::Lower(s);
    if (l.empty()) return;
    if (l == "cj" || l == "default" || l == "0") { g_pending = -1; return; }
    if (l == "rescan") { Rescan(); return; }
    if (l.find_first_not_of("0123456789") == std::string::npos) { g_pending = atoi(l.c_str()) - 1; return; }
    for (size_t i = 0; i < g_skins.size(); i++)
        if (skinscan::Lower(g_skins[i].name) == l) { g_pending = (int)i; return; }
    LOGI("request: '%s' no es una skin conocida", s.c_str());
}

static void CGame_Process_hook() {
    CGame_Process_orig();
    if (g_frame == 0) LOGI("hook de CGame::Process activo");
    if (++g_frame % 45 == 0) PollRequestFile();
    int p = g_pending.load();
    if (p != -2 && ApplySkin(p)) g_pending = -2;
}

// ---- menu SAUtils (opcional) --------------------------------------------------------------------
#ifdef HAVE_SAUTILS
static ISAUtils* sautils;
static std::vector<std::string> g_menuNames;
static std::vector<const char*> g_menuPtrs;
static void OnPick(int oldVal, int newVal, void*) { g_pending = newVal - 1; }  // 0 = CJ -> -1
#endif

extern "C" void OnModLoad() {
    std::string files;
    for (auto d : kFilesDirs) {          // la que tenga modloader/ es la buena
        struct stat st;
        if (stat((std::string(d) + "/modloader").c_str(), &st) == 0) { files = d; break; }
    }
    if (files.empty()) {                 // ninguna tiene modloader: usa la primera "files" que exista
        for (auto d : kFilesDirs) { struct stat st; if (stat(d, &st) == 0) { files = d; break; } }
    }
    if (files.empty()) files = kFilesDirs[0];
    g_root = files + "/modloader";
    g_plugDir = files + "/SkinSelector";
    mkdir(g_plugDir.c_str(), 0777);
    LOGI("== SkinSelector 1.0.1 cargado (OnModLoad) ==");
    LOGI("files: %s", files.c_str());

    void* h = aml->GetLibHandle("libGTASA.so");
    #define SYM(var, name) do { *(void**)&var = (void*)aml->GetSym(h, name); LOGI("  %s %s", var ? "ok     " : "FALTA  ", name); } while (0)
    SYM(RequestSpecialChar,             "_ZN10CStreaming18RequestSpecialCharEiPKci");
    SYM(LoadAllRequestedModels,         "_ZN10CStreaming22LoadAllRequestedModelsEb");
    SYM(HasSpecialCharLoaded,           "_ZN10CStreaming20HasSpecialCharLoadedEi");
    SYM(SetMissionDoesntRequireSpecialChar, "_ZN10CStreaming34SetMissionDoesntRequireSpecialCharEi");
    SYM(FindPlayerPed,                  "_Z13FindPlayerPedi");
    SYM(SetModelIndex,                  "_ZN4CPed13SetModelIndexEj");
    SYM(DeleteRwObject,                 "_ZN7CEntity14DeleteRwObjectEv");   // opcional
    SYM(RebuildPlayer,                  "_ZN8CClothes13RebuildPlayerEP10CPlayerPedb");  // opcional
    void* process = (void*)aml->GetSym(h, "_ZN5CGame7ProcessEv");
    LOGI("  %s _ZN5CGame7ProcessEv", process ? "ok     " : "FALTA  ");
    #undef SYM

    Rescan();   // lista las skins aunque falte algun simbolo

    if (!RequestSpecialChar || !LoadAllRequestedModels || !HasSpecialCharLoaded ||
        !SetMissionDoesntRequireSpecialChar || !FindPlayerPed || !SetModelIndex || !process) {
        LOGI("faltan simbolos de libGTASA.so: no se puede cambiar la skin");
        return;
    }
    aml->Hook(process, (void*)CGame_Process_hook, (void**)&CGame_Process_orig);
    LOGI("hook puesto en CGame::Process");
}

extern "C" void OnAllModsLoaded() {
#ifdef HAVE_SAUTILS
    sautils = (ISAUtils*)GetInterface("SAUtils");
    if (sautils && !g_skins.empty()) {
        g_menuNames.push_back("CJ (original)");
        for (auto& s : g_skins) g_menuNames.push_back(s.name);
        for (auto& n : g_menuNames) g_menuPtrs.push_back(n.c_str());
        sautils->AddClickableItem(SetType_Game, "Skin", 0, 0, (int)g_menuPtrs.size() - 1, g_menuPtrs.data(), OnPick);
    }
#endif
}
