// SkinSelector.cpp - plugin AML (arm64, GTA SA 2.10) que lista las skins de modloader/ y se las pone al jugador.
// ZyZ ModLoader se encarga de cargar el .dff/.txd/.png; este plugin solo detecta cual es una skin y la aplica.
#include <mod/config.h>
#include <mod/amlmod.h>
#include <android/log.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/mman.h>
#include "skinscan.h"
#include "vestidor_common.h"

#if __has_include("isautils.h")
  #include "isautils.h"      // opcional: menu de SAUtils (copialo de su repo si lo quieres)
  #define HAVE_SAUTILS 1
#endif

MYMODCFG(net.hunter09.arcx, Project ARCX, 2.3.1-beta, Hunter09)
NEEDGAME(com.rockstargames.gtasa)

#define TAG "ARCX"

// ---- rutas -------------------------------------------------------------------------------------
// Carpeta "files" del juego. En algunos telefonos es Android_unprotected (asi lo muestra el log de ZyZ).
static const char* kFilesDirs[] = {
    "/storage/emulated/0/Android_unprotected/data/com.rockstargames.gtasa/files",
    "/storage/emulated/0/Android/data/com.rockstargames.gtasa/files",
    "/sdcard/Android/data/com.rockstargames.gtasa/files",
    "/storage/self/primary/Android/data/com.rockstargames.gtasa/files",
};
static std::string g_root;       // .../files/modloader
static std::string g_plugDir;    // .../files/ARCX  (ARCX.ini, state.ini, skins.ini, request.ini, log.txt)

static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsa::LogV("nucleo", TAG, fmt, ap);
    va_end(ap);
}
#define LOGI(...) Log(__VA_ARGS__)

// ---- funciones del juego (libGTASA.so 2.10 arm64) -----------------------------------------------
static void  (*RequestSpecialChar)(int, const char*, int);
static void  (*LoadAllRequestedModels)(bool);
static bool  (*HasSpecialCharLoaded)(int);
static void  (*SetMissionDoesntRequireSpecialChar)(int);
static void* (*FindPlayerPed)(int);
static void* (*FindPlayerVehicle)(int, bool);   // devuelve el vehiculo si el jugador va dentro
static void  (*SetModelIndex)(void*, unsigned int);   // CPed::SetModelIndex
static void  (*DeleteRwObject)(void*);                // CEntity::DeleteRwObject (opcional)
static void  (*RebuildPlayer)(void*, bool);           // CClothes::RebuildPlayer (opcional, para volver a CJ)
static void* (*GetModelInfoByName)(const char*, int*);   // CModelInfo::GetModelInfo(name, &index)
static void  (*CGame_Process_orig)();
static void  (*CGame_InitRestart_orig)();
static void  (*CGame_ShutRestart_orig)();

static int g_slot = 9;          // slot de personaje especial (1 normal; 9 con "skin en misiones" para no chocar con los de las misiones)
static bool g_missions = true;  // siempre activa por defecto: la skin se mantiene en misiones y solo se quita en cinematicas
static std::string g_title = "PROJECT ARCX";   // nombre del menu (editable en settings.txt: title=...) // opcion: mantener la skin tambien durante las misiones

// ---- estado --------------------------------------------------------------------------------------
static std::vector<skinscan::Skin> g_skins;
static std::atomic<int> g_pending{-2};   // -2 nada, -1 volver a CJ, >=0 indice en g_skins
static int g_frame = 0;
static int g_curSkin = -1;                 // -1 = CJ, >=0 indice de la skin puesta
static const volatile uint8_t* g_pauseUser = nullptr;   // CTimer::m_UserPause
static const volatile uint8_t* g_pauseCode = nullptr;   // CTimer::m_CodePause
static uint8_t* g_scriptSpace = nullptr;   // CTheScripts::ScriptSpace (para detectar misiones)
static int g_prevMission = 0;
static int g_missionLogs = 0;
static const int kOnMissionVar = 408;      // $ONMISSION en 2.10 (lo dice el log de ZyZ)

static int g_desired = -1;                 // skin que el jugador quiere llevar (-1 = CJ); sobrevive a cinematicas, misiones y guardados
static bool g_vehicleSwap = true;          // opcion: cambiar de skin tambien dentro del vehiculo (como ARM Cheat Menu: sin DeleteRwObject); si no, espera a que salga
static void (*FlushImmediately)(void*, bool);   // CPedIntelligence::FlushImmediately
static void (*AddInCarAnim)(const void*, void*, bool);   // CCarEnterExit::AddInCarAnim(const CVehicle&, CPed*, bool esConductor)
static bool (*VehIsDriver)(const void*, const void*);    // CVehicle::IsDriver(const CPed*) const
static bool (*VehIsPassenger)(const void*, const void*); // CVehicle::IsPassenger(const CPed*) const
static bool g_remember = false;            // opcion "recordar": mantener la ultima skin entre partidas e inicios del juego
static std::string g_lastName = "CJ";      // ultima skin elegida (se guarda en settings.txt)
static bool g_needReapply = false;
static int g_reapplyDelay = 0;
static int g_autoTries = 0;
static bool g_giveUp = false;
static int g_nextTryFrame = 0;
static int g_miOff = -1;                   // offset de CEntity::m_nModelIndex dentro del ped (se calibra solo)
static unsigned g_wornModelId = 0;
static const volatile uint8_t* g_cutPtr = nullptr;    // CCutsceneMgr::ms_running
static const volatile uint8_t* g_loadPtr = nullptr;   // CGenericGameStorage::ms_bLoading
static bool g_prevCut = false, g_prevLoading = false, g_sessionPending = false;
static int g_nullFrames = 0;

static std::string g_status;        // "esperando: ..." para el menu; vacio = libre
static std::string g_stateCur = "CJ";

static void WriteState() {          // state.ini: lo lee el menu (skin puesta y estado de espera)
    vsa::Ini ini;
    ini["State"]["current"] = g_stateCur;
    ini["State"]["status"] = g_status;
    vsa::IniWrite(vsa::P("state.ini"), ini);
}
static void WriteCurrent(const char* name) { g_stateCur = name; WriteState(); }

static void SaveSettings() {        // ARCX.ini, seccion [General] (el menu guarda la suya en [Menu])
    vsa::Ini ini = vsa::IniRead(vsa::P("ARCX.ini"));
    auto& g = ini["General"];
    g["remember"] = g_remember ? "1" : "0";
    g["missions"] = g_missions ? "1" : "0";
    g["vehicle"] = g_vehicleSwap ? "1" : "0";
    g["title"] = g_title;
    g["last"] = g_lastName;
    vsa::IniWrite(vsa::P("ARCX.ini"), ini);
}
static void LoadSettings() {
    vsa::Ini ini = vsa::IniRead(vsa::P("ARCX.ini"));
    if (ini.find("General") == ini.end()) return;
    g_remember = vsa::GetInt(ini, "General", "remember", 0) == 1;
    g_missions = vsa::GetInt(ini, "General", "missions", 1) == 1;
    g_slot = g_missions ? 9 : 1;
    g_vehicleSwap = vsa::GetInt(ini, "General", "vehicle", 1) == 1;
    std::string t = vsa::Get(ini, "General", "title");
    if (!t.empty()) g_title = t;
    std::string l = vsa::Get(ini, "General", "last");
    if (!l.empty()) g_lastName = l;
}

static void WriteList() {           // skins.ini: [Skins] 1=nombre ... y [Paths] 1=ruta
    vsa::Ini ini;
    ini["Skins"]["count"] = std::to_string(g_skins.size());
    for (size_t i = 0; i < g_skins.size(); i++) {
        ini["Skins"][std::to_string(i + 1)] = g_skins[i].name;
        ini["Paths"][std::to_string(i + 1)] = g_skins[i].path;
    }
    vsa::IniWrite(vsa::P("skins.ini"), ini);
}
static void Rescan() {
    // los indices pueden cambiar al releer: se recuerdan por nombre
    std::string desiredName = (g_desired >= 0 && g_desired < (int)g_skins.size()) ? g_skins[g_desired].name : "";
    std::string wornName = (g_curSkin >= 0 && g_curSkin < (int)g_skins.size()) ? g_skins[g_curSkin].name : "";
    g_skins.clear();
    std::vector<std::string> rejected;
    skinscan::Scan(g_root, g_skins, &rejected);
    LOGI("%zu skin(s) en %s", g_skins.size(), g_root.c_str());
    for (auto& k : g_skins)
        LOGI("  + skin: %s  [chunk de skin/huesos: %s]%s", k.name.c_str(), k.ped ? "SI" : "NO",
             k.ped ? "" : "  <- NO es un ped valido: se bloquea (usa !nombre para forzar bajo tu riesgo)");
    LOGI("  (%zu .dff sin huesos ignorados: mapa, objetos, armas...)", rejected.size());
    auto find = [&](const std::string& n) {
        if (n.empty()) return -1;
        for (size_t i = 0; i < g_skins.size(); i++)
            if (skinscan::Lower(g_skins[i].name) == skinscan::Lower(n)) return (int)i;
        return -1;
    };
    if (!desiredName.empty()) g_desired = find(desiredName);
    if (!wornName.empty()) g_curSkin = find(wornName);
    WriteList();
}

// Debe llamarse en el hilo del juego (lo hacemos desde el hook de CGame::Process).
static std::atomic<bool> g_noDelete{false};
static void* g_pedVtbl = nullptr;   // vtable de CPedModelInfo (se toma de "male01")

static void (*ClearTaskDuckSecondary)(void*);   // CPedIntelligence::ClearTaskDuckSecondary
static int g_intelOff = -1;
static bool g_intelTried = false;
static const uintptr_t kPtrMask = 0x00FFFFFFFFFFFFFFull;

static bool Readable(const void* p, size_t n) {
    uintptr_t a = (uintptr_t)p & kPtrMask;
    if (a < 0x10000) return false;
    long pg = sysconf(_SC_PAGESIZE);
    uintptr_t st = a & ~((uintptr_t)pg - 1), en = (a + n - 1) & ~((uintptr_t)pg - 1);
    size_t pages = (size_t)((en - st) / (uintptr_t)pg) + 1;
    if (pages > 2) return false;
    unsigned char vec[2];
    return mincore((void*)st, pages * (size_t)pg, vec) == 0;
}

// CPedIntelligence empieza con m_pPed (puntero al ped): asi se localiza ped->m_pIntelligence sin conocer el offset
static void* FindIntelligence(void* ped) {
    if (g_intelOff < 0 && !g_intelTried) {
        g_intelTried = true;
        int found[4], n = 0;
        for (int o = 0x200; o < 0x700 && n < 4; o += 8) {
            void* p;
            memcpy(&p, (const char*)ped + o, sizeof p);
            if (((uintptr_t)p & 7) || !Readable(p, 8)) continue;
            uintptr_t first;
            memcpy(&first, p, sizeof first);
            if ((first & kPtrMask) == ((uintptr_t)ped & kPtrMask)) found[n++] = o;
        }
        LOGI("  calibracion CPedIntelligence: %d candidato(s)", n);
        for (int i = 0; i < n; i++) LOGI("    +0x%03x", found[i]);
        if (n == 1) { g_intelOff = found[0]; LOGI("  m_pIntelligence calibrado en ped+0x%03x", g_intelOff); }
        else LOGI("  m_pIntelligence SIN calibrar: no se limpia la tarea de agacharse");
    }
    if (g_intelOff < 0) return nullptr;
    void* p;
    memcpy(&p, (const char*)ped + g_intelOff, sizeof p);
    return p;
}

// agachado, el cambio de modelo dejaba la tarea de agacharse apuntando a animaciones destruidas (el jugador no podia caminar)
static void ClearDuck(void* ped) {
    if (!ClearTaskDuckSecondary) return;
    void* in = FindIntelligence(ped);
    if (in) ClearTaskDuckSecondary(in);
}

// ---- el cambio de modelo destruye las animaciones: agachado, peleando, en coche, saltando... deja al jugador atascado ----
static int g_busySince = 0;
static int g_layoutLogs = 0;

static void WriteStatus(const std::string& st) {
    if (st == g_status) return;
    g_status = st;
    WriteState();
}

// CPedIntelligence = { CPed* m_pPed; CTaskManager { CTask* primarias[5]; CTask* secundarias[6]; CPed* m_pPed; } ... }
static bool InVehicle() { return FindPlayerVehicle && FindPlayerVehicle(-1, false); }

// ¿va realmente sentado? Al subir o bajar FindPlayerVehicle ya (o aun) devuelve el coche, pero el ped no es conductor ni pasajero.
static bool SeatedIn(void* veh, void* ped, bool* isDriver) {
    *isDriver = true;
    if (!veh || (!VehIsDriver && !VehIsPassenger)) return veh != nullptr;
    bool d = VehIsDriver && VehIsDriver(veh, ped);
    bool p = VehIsPassenger && VehIsPassenger(veh, ped);
    *isDriver = d || !p;
    return d || p;
}

// SetModelIndex deja al ped con una animacion de estar de pie (CAnimManager::AddAnimation). Dentro de un vehiculo hay que volver a
// ponerle la de sentado, como hace CTaskSimpleCarSetPedInAsDriver::ProcessPed (AddInCarAnim con true = conductor, false = pasajero).
static void LogTaskState(void* ped, const char* ctx) {   // para ajustar la deteccion de subir/bajar: tareas del jugador al aplicar
    static int n = 0;
    if (n >= 10) return;
    void* in = FindIntelligence(ped);
    if (!in || !Readable(in, 8 * 13)) return;
    uint64_t q[13];
    memcpy(q, in, sizeof q);
    n++;
    LOGI("  tareas %s: primarias=%d%d%d%d%d secundarias=%d%d%d%d%d%d", ctx, q[1] != 0, q[2] != 0, q[3] != 0, q[4] != 0, q[5] != 0,
         q[6] != 0, q[7] != 0, q[8] != 0, q[9] != 0, q[10] != 0, q[11] != 0);
}

static void ReseatInVehicle(void* ped, void* veh) {
    if (!veh || !AddInCarAnim) return;
    LogTaskState(ped, "en vehiculo al aplicar");
    bool drv;
    if (!SeatedIn(veh, ped, &drv)) { LOGI("  el jugador no va sentado (subiendo o bajando): no se restaura la animacion de sentado"); return; }
    AddInCarAnim(veh, ped, drv);
    LOGI("  en vehiculo: animacion de sentado restaurada (%s)", drv ? "conductor" : "pasajero");
}

// solo tareas (agachado, peleando, saltando...); el vehiculo se trata aparte
static bool PlayerBusy(void* ped, std::string* why) {
    void* in = FindIntelligence(ped);
    if (!in || !Readable(in, 8 * 13)) return false;
    uint64_t q[13];
    memcpy(q, in, sizeof q);
    bool layoutOk = ((q[12] & kPtrMask) == ((uintptr_t)ped & kPtrMask)) && q[5] != 0;   // m_pPed al final y tarea por defecto presente
    if (g_layoutLogs < 24) {
        g_layoutLogs++;
        LOGI("  tareas: primarias=%d%d%d%d%d secundarias=%d%d%d%d%d%d diseno=%s", q[1] != 0, q[2] != 0, q[3] != 0, q[4] != 0,
             q[5] != 0, q[6] != 0, q[7] != 0, q[8] != 0, q[9] != 0, q[10] != 0, q[11] != 0, layoutOk ? "ok" : "NO COINCIDE");
    }
    if (!layoutOk) return false;
    if (q[6]) { *why = "peleando"; return true; }
    if (q[7]) { *why = "agachado"; return true; }
    for (int i = 0; i < 4; i++)
        if (q[1 + i]) { *why = "ocupado"; return true; }
    return false;
}

// true = hay que esperar antes de cambiar el modelo. A pie el cambio es inmediato (se limpian las tareas);
// en vehiculo se espera a que salga salvo que la opcion "cambiar dentro del vehiculo" este activa.
static bool NeedWait(void* ped, std::string* why) {
    if (InVehicle()) {
        bool drv;
        if (!SeatedIn(FindPlayerVehicle(-1, false), ped, &drv)) { *why = "ocupado"; return true; }   // subiendo o bajando del coche
        // al bajar, IsDriver sigue siendo true hasta que el ped sale del todo: la tarea de bajar/subir (primarias) lo delata
        std::string tw;
        if (PlayerBusy(ped, &tw)) {
            static int vlog = 0;
            if (vlog++ < 12) LOGI("  en vehiculo: tareas activas (%s): se espera", tw.c_str());
            *why = (tw == "agachado") ? "ocupado" : tw;
            return true;
        }
        if (g_vehicleSwap) return false;
        *why = "en vehiculo";
        return true;
    }
    if (!FlushImmediately) return PlayerBusy(ped, why);   // sin forma de limpiar tareas: se espera
    return false;
}

// antes de destruir las animaciones: si el jugador esta agachado, peleando, etc. se limpian sus tareas como hace CLEAR_CHAR_TASKS_IMMEDIATELY
static void PrepareForModelChange(void* ped) {
    std::string why;
    if (!InVehicle() && FlushImmediately && PlayerBusy(ped, &why)) {
        void* in = FindIntelligence(ped);
        if (in) {
            LOGI("  jugador %s: se limpian sus tareas antes del cambio de modelo", why.c_str());
            FlushImmediately(in, true);
            return;
        }
    }
    ClearDuck(ped);
}

static int ReadPedModel(void* ped) {
    if (g_miOff < 0 || !ped) return -1;
    uint16_t v;
    memcpy(&v, (const char*)ped + g_miOff, sizeof v);
    return v;
}

static bool ApplySkin(int idx, bool force);
static bool ApplySkin(int idx, bool force = false) {
    void* ped = FindPlayerPed(-1);
    if (!ped) return false;                       // aun no hay jugador (menu / carga): se reintenta
    if (idx < 0) {                                // volver a CJ
        void* veh0 = InVehicle() ? FindPlayerVehicle(-1, false) : nullptr;
        PrepareForModelChange(ped);
        if (DeleteRwObject) DeleteRwObject(ped);
        SetModelIndex(ped, 0);
        ReseatInVehicle(ped, veh0);
        if (RebuildPlayer) RebuildPlayer(ped, false);
        if (g_miOff >= 0 && ReadPedModel(ped) != 0) { LOGI("  calibracion del modelo invalida (leo %d con CJ)", ReadPedModel(ped)); g_miOff = -1; }
        g_curSkin = -1;
        WriteCurrent("CJ");
        LOGI("skin: CJ");
        return true;
    }
    if (idx >= (int)g_skins.size()) return true;
    const std::string& name = g_skins[idx].name;
    if (!g_skins[idx].ped && !force) {
        LOGI("skin '%s' bloqueada: el .dff no tiene huesos (no es un ped). Crashearia el juego.", name.c_str());
        return true;
    }
    LOGI("aplicando '%s'...", name.c_str());
    RequestSpecialChar(g_slot, name.c_str(), 0);
    LoadAllRequestedModels(false);
    bool loaded = HasSpecialCharLoaded(g_slot);
    LOGI("  HasSpecialCharLoaded = %d", (int)loaded);
    if (!loaded) {
        LOGI("skin '%s' no cargo (mira ZyZModLoader.log: nombre, .dff valido, textura)", name.c_str());
        return true;
    }
    unsigned modelId = 290u + g_slot;   // valor por defecto; se corrige con el indice real que devuelve el juego
    {   // el juego dice en que id registro el modelo (RequestSpecialChar slot 1 -> id 291, no 290)
        int mIdx = -1;
        void* mi = GetModelInfoByName ? GetModelInfoByName(name.c_str(), &mIdx) : nullptr;
        if (mIdx >= 0) modelId = (unsigned)mIdx;
        LOGI("  GetModelInfo('%s') -> %p, id real del modelo = %d", name.c_str(), mi, mIdx);
        if (mIdx >= 0 && (mIdx < 290 || mIdx > 310) && !force) {
            LOGI("  id fuera del rango de personajes especiales (290-310): no se aplica");
            return true;
        }
        if (GetModelInfoByName && !mi && !force) {
            LOGI("  el juego no registro el nombre '%s' como modelo: no se aplica", name.c_str());
            return true;
        }
        if (mi && GetModelInfoByName) {   // el modelo debe ser un CPedModelInfo (el slot 10 -> id 300 no lo es y crashea)
            if (!g_pedVtbl) {
                int vi = -1;
                void* ref = GetModelInfoByName("male01", &vi);
                if (ref) g_pedVtbl = *(void**)ref;
                LOGI("  referencia de ped (male01, id %d): vtable %p", vi, g_pedVtbl);
            }
            if (g_pedVtbl && *(void**)mi != g_pedVtbl && !force) {
                LOGI("  el modelo %u (slot %d) NO es un ped (vtable %p): no se aplica", modelId, g_slot, *(void**)mi);
                if (g_slot != 1) { g_slot = 1; LOGI("  se usa el slot 1"); return ApplySkin(idx, force); }
                return true;
            }
        }
        if (mi) {
            const uint64_t* q = (const uint64_t*)mi;
            for (int i = 0; i < 12; i += 4)
                LOGI("  modelinfo+%02x: %016llx %016llx %016llx %016llx", i * 8,
                     (unsigned long long)q[i], (unsigned long long)q[i + 1],
                     (unsigned long long)q[i + 2], (unsigned long long)q[i + 3]);
            void* rw = (void*)q[8];   // CBaseModelInfo::m_pRwObject: +0x40 en 64 bits (va justo despues del 2000.0 de drawDistance)
            LOGI("  m_pRwObject (+0x40) = %p", rw);
            if (!rw && !force) {
                LOGI("  el modelo esta 'cargado' pero no tiene clump: no se aplica (usa !%s para forzar)", name.c_str());
                return true;
            }
        }
    }
    PrepareForModelChange(ped);
    void* veh = InVehicle() ? FindPlayerVehicle(-1, false) : nullptr;
    if (DeleteRwObject && !g_noDelete.load()) { LOGI("  DeleteRwObject(ped=%p)", ped); DeleteRwObject(ped); }
    LOGI("  SetModelIndex(ped=%p, modelo=%u)", ped, modelId);
    SetModelIndex(ped, modelId);
    LOGI("  SetModelIndex termino sin crash");
    ReseatInVehicle(ped, veh);
    if (g_miOff >= 0 && ReadPedModel(ped) != (int)modelId) {
        LOGI("  calibracion del modelo invalida (leo %d, esperaba %u)", ReadPedModel(ped), modelId);
        g_miOff = -1;
    }
    if (g_miOff < 0) {   // busca donde guarda el ped su indice de modelo (uint16 == modelId)
        int found[8], n = 0, pick = -1;
        for (int o = 0x20; o < 0x100 && n < 8; o += 2) {
            uint16_t v;
            memcpy(&v, (const char*)ped + o, sizeof v);
            if (v == modelId) found[n++] = o;
        }
        LOGI("  calibracion m_nModelIndex: %d candidato(s)", n);
        for (int i = 0; i < n; i++) { LOGI("    +0x%02x", found[i]); if (found[i] == 0x2E) pick = 0x2E; }
        if (pick < 0 && n == 1) pick = found[0];
        g_miOff = pick;
        if (pick >= 0) LOGI("  m_nModelIndex calibrado en ped+0x%02x", pick);
        else LOGI("  m_nModelIndex SIN calibrar: no se detectan resets por modelo (solo por cinematica/mision/carga)");
    }
    g_wornModelId = modelId;
    SetMissionDoesntRequireSpecialChar(g_slot);
    g_curSkin = idx;
    WriteCurrent(name.c_str());
    LOGI("skin: %s", name.c_str());
    return true;
}

static std::atomic<bool> g_force{false};

// El jugador elige una skin (-1 = CJ): se aplica y pasa a ser la deseada.
static void Choose(int idx) {
    g_pending = idx;
    g_desired = idx;
    g_lastName = idx >= 0 ? g_skins[idx].name : "CJ";
    g_needReapply = false; g_giveUp = false; g_autoTries = 0;
    SaveSettings();
}

static void PollRequestFile() {
    std::string p = vsa::P("request.ini");
    std::string w = vsa::P("request_working.ini");
    struct stat rst;
    if (stat(p.c_str(), &rst) != 0) return;
    vsa::Ini rq = vsa::IniRead(p);
    rename(p.c_str(), w.c_str());   // se queda como request_working.ini hasta que la skin se aplique sin crash
    std::string s = vsa::Get(rq, "Request", "command");
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    g_force = false;
    g_noDelete = false;
    while (!s.empty() && (s[0] == '!' || s[0] == '~')) {   // ! = forzar, ~ = no llamar DeleteRwObject
        if (s[0] == '!') g_force = true; else g_noDelete = true;
        s.erase(s.begin());
    }
    std::string l = skinscan::Lower(s);
    auto done = [&]() { unlink(w.c_str()); };
    if (l.empty()) { done(); return; }
    if (l == "remember_on" || l == "remember_off") {
        g_remember = (l == "remember_on");
        SaveSettings();
        LOGI("recordar ultima skin: %s", g_remember ? "SI" : "NO");
        done();
        return;
    }
    if (l == "missions_on" || l == "missions_off") {
        g_missions = (l == "missions_on");
        g_slot = g_missions ? 9 : 1;
        SaveSettings();
        LOGI("skin en misiones: %s (slot %d)", g_missions ? "SI" : "NO", g_slot);
        if (g_desired >= 0) { g_needReapply = true; g_reapplyDelay = 10; g_giveUp = false; g_autoTries = 0; }
        done();
        return;
    }
    if (l == "vehicle_on" || l == "vehicle_off") {
        g_vehicleSwap = (l == "vehicle_on");
        SaveSettings();
        LOGI("cambiar dentro del vehiculo: %s", g_vehicleSwap ? "SI" : "NO");
        done();
        return;
    }
    if (l == "cj" || l == "default" || l == "0") { Choose(-1); return; }
    if (l == "rescan") { Rescan(); done(); return; }
    if (l.find_first_not_of("0123456789") == std::string::npos) {
        int n = atoi(l.c_str()) - 1;
        if (n >= 0 && n < (int)g_skins.size()) { Choose(n); return; }
        LOGI("request: numero %s fuera de la lista", l.c_str());
        done();
        return;
    }
    for (size_t i = 0; i < g_skins.size(); i++)
        if (skinscan::Lower(g_skins[i].name) == l) { Choose((int)i); return; }
    LOGI("request: '%s' no es una skin conocida (el archivo debe contener solo el nombre, ej: sold)", s.c_str());
    done();
}

static bool ReadFlag(const volatile uint8_t* p) { return p && *p != 0; }

// El juego recreo al jugador (partida nueva o cargada).
static void OnNewSession() {
    g_curSkin = -1;
    g_giveUp = false; g_autoTries = 0;
    if (!g_remember) {
        g_desired = -1; g_lastName = "CJ"; g_needReapply = false;
        SaveSettings();
        WriteCurrent("CJ");
        LOGI("partida nueva/cargada: recordar=NO, se queda CJ");
    } else {
        g_needReapply = g_desired >= 0;
        g_reapplyDelay = 150;
        LOGI("partida nueva/cargada: recordar=SI, skin pendiente: %s", g_lastName.c_str());
    }
}

static void Watchdog() {
    void* ped = FindPlayerPed(-1);
    bool cut = ReadFlag(g_cutPtr), loading = ReadFlag(g_loadPtr);

    if (loading && !g_prevLoading) g_sessionPending = true;
    g_prevLoading = loading;
    if (!ped) { if (g_nullFrames < 1000) g_nullFrames++; g_prevCut = cut; return; }
    if (g_nullFrames >= 5) g_sessionPending = true;
    g_nullFrames = 0;
    if (g_sessionPending && !loading) { g_sessionPending = false; OnNewSession(); }

    if (g_prevCut && !cut) {   // fin de cinematica: el juego dejo a CJ, se vuelve a poner la skin
        LOGI("cinematica terminada");
        if (g_desired >= 0) { g_needReapply = true; g_reapplyDelay = 45; g_giveUp = false; g_autoTries = 0; }
    }
    g_prevCut = cut;

    int om = g_prevMission;
    if (g_scriptSpace) memcpy(&om, g_scriptSpace + kOnMissionVar * 4, sizeof om);
    if (g_scriptSpace && om != g_prevMission) {
        if (g_missionLogs++ < 20) LOGI("ONMISSION %d -> %d", g_prevMission, om);
        if (om != 0 && g_prevMission == 0 && !g_missions && g_curSkin >= 0 && g_pending.load() == -2) {
            LOGI("mision iniciada: se vuelve a CJ (la skin deseada se conserva)");
            g_pending = -1;     // g_desired no cambia
        }
        if (om == 0 && g_prevMission != 0 && g_desired >= 0 && g_curSkin != g_desired) {
            LOGI("mision terminada");
            g_needReapply = true; g_reapplyDelay = 60; g_giveUp = false; g_autoTries = 0;
        }
        g_prevMission = om;
    }

    // en pausa (menu, mapa, guardar partida) no se toca el modelo: se espera a que se reanude
    if ((g_pauseUser && *g_pauseUser) || (g_pauseCode && *g_pauseCode)) return;

    // peticiones explicitas: esperan a que acabe la cinematica o la carga, y a que el jugador este libre
    int p = g_pending.load();
    if (p != -2 && !cut && !loading) {
        std::string why;
        if (NeedWait(ped, &why)) {
            if (g_busySince == 0) { g_busySince = g_frame; LOGI("esperando: el jugador esta %s (cambiar el modelo ahora lo dejaria atascado)", why.c_str()); }
            WriteStatus(why);
            if (g_frame - g_busySince > 1200) {
                LOGI("se cancela el cambio: el jugador siguio ocupado (%s)", why.c_str());
                g_pending = -2; g_busySince = 0; WriteStatus("");
                unlink(vsa::P("request_working.ini").c_str());
            }
        } else {
            g_busySince = 0;
            WriteStatus("");
            if (ApplySkin(p, g_force.load())) {
                g_pending = -2;
                unlink(vsa::P("request_working.ini").c_str());   // llegamos aqui = no hubo crash
            }
        }
    }

    bool allowed = !cut && !loading && (om == 0 || g_missions) && g_pending.load() == -2;
    if (allowed && g_desired >= 0 && g_curSkin == g_desired && g_miOff >= 0) {
        int cur = ReadPedModel(ped);   // el guardado o la carga pudieron devolver al jugador a CJ
        if (cur != (int)g_wornModelId) {
            static int mlog = 0;
            if (mlog++ < 20) LOGI("el modelo del jugador cambio a %d (esperado %u): se vuelve a poner la skin", cur, g_wornModelId);
            g_curSkin = -1;
            g_needReapply = true; g_reapplyDelay = 20;
        }
    }
    if (g_needReapply && allowed && g_desired >= 0 && g_desired < (int)g_skins.size() && !g_giveUp) {
        if (g_reapplyDelay > 0) {
            g_reapplyDelay--;
        } else if (g_frame >= g_nextTryFrame) {
            std::string why;
            if (NeedWait(ped, &why)) {
                static int lastBusyLog = -1000;
                if (g_frame - lastBusyLog > 600) { lastBusyLog = g_frame; LOGI("reaplicado en espera: el jugador esta %s", why.c_str()); }
                WriteStatus(why);
                return;
            }
            WriteStatus("");
            g_nextTryFrame = g_frame + 90;
            if (++g_autoTries > 3) {
                g_giveUp = true; g_needReapply = false;
                LOGI("no se pudo mantener la skin tras 3 intentos: se detiene el reintento automatico");
            } else {
                LOGI("reaplicando '%s' (intento %d)", g_skins[g_desired].name.c_str(), g_autoTries);
                if (ApplySkin(g_desired)) g_needReapply = false;
            }
        }
    }
}

static void CGame_InitRestart_hook() {
    g_sessionPending = true;
    LOGI("CGame::InitialiseWhenRestarting (partida nueva o cargada)");
    CGame_InitRestart_orig();
}
static void CGame_ShutRestart_hook() {
    g_sessionPending = true;
    LOGI("CGame::ShutDownForRestart (partida nueva o cargada)");
    CGame_ShutRestart_orig();
}

static void CGame_Process_hook() {
    CGame_Process_orig();
    if (g_frame == 0) LOGI("hook de CGame::Process activo");
    ++g_frame;
    if (g_frame % 15 == 0) PollRequestFile();
    Watchdog();
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
    g_plugDir = files + "/ARCX";
    mkdir(g_plugDir.c_str(), 0777);
    vsa::g_dir = g_plugDir;
    vsa::LogInit();                 // log.txt unico: se reinicia una vez por sesion
    {   // si quedo request_working.ini, esa skin crasheo el juego
        std::string w = vsa::P("request_working.ini"), c = vsa::P("crashed_request.txt");
        struct stat st;
        if (stat(w.c_str(), &st) == 0) {
            remove(c.c_str());
            rename(w.c_str(), c.c_str());
            LOGI("!! la ultima skin pedida crasheo el juego (ver crashed_request.txt)");
        }
    }
    LoadSettings();
    WriteCurrent("CJ");   // proceso nuevo = CJ (state.ini); con "recordar" se actualiza al reaplicar
    LOGI("== Project ARCX 2.3.1-beta cargado (OnModLoad) ==");
    LOGI("recordar ultima skin: %s, skin en misiones: %s (slot %d), ultima: %s", g_remember ? "SI" : "NO", g_missions ? "SI" : "NO", g_slot, g_lastName.c_str());
    LOGI("files: %s", files.c_str());

    void* h = aml->GetLibHandle("libGTASA.so");
    #define SYM(var, name) do { *(void**)&var = (void*)aml->GetSym(h, name); LOGI("  %s %s", var ? "ok     " : "FALTA  ", name); } while (0)
    SYM(RequestSpecialChar,             "_ZN10CStreaming18RequestSpecialCharEiPKci");
    SYM(LoadAllRequestedModels,         "_ZN10CStreaming22LoadAllRequestedModelsEb");
    SYM(HasSpecialCharLoaded,           "_ZN10CStreaming20HasSpecialCharLoadedEi");
    SYM(SetMissionDoesntRequireSpecialChar, "_ZN10CStreaming34SetMissionDoesntRequireSpecialCharEi");
    SYM(FindPlayerPed,                  "_Z13FindPlayerPedi");
    SYM(FindPlayerVehicle,              "_Z17FindPlayerVehicleib");   // opcional
    SYM(FlushImmediately,               "_ZN16CPedIntelligence16FlushImmediatelyEb");   // opcional
    SYM(AddInCarAnim,                   "_ZN13CCarEnterExit12AddInCarAnimERK8CVehicleP4CPedb");   // opcional (animacion de sentado)
    SYM(VehIsDriver,                    "_ZNK8CVehicle8IsDriverEPK4CPed");   // opcional
    SYM(VehIsPassenger,                 "_ZNK8CVehicle11IsPassengerEPK4CPed");   // opcional
    SYM(SetModelIndex,                  "_ZN4CPed13SetModelIndexEj");
    SYM(DeleteRwObject,                 "_ZN7CEntity14DeleteRwObjectEv");   // opcional
    SYM(RebuildPlayer,                  "_ZN8CClothes13RebuildPlayerEP10CPlayerPedb");  // opcional
    SYM(GetModelInfoByName,             "_ZN10CModelInfo12GetModelInfoEPKcPi");
    *(void**)&ClearTaskDuckSecondary = (void*)aml->GetSym(h, "_ZN16CPedIntelligence22ClearTaskDuckSecondaryEv");
    LOGI("  %s CPedIntelligence::ClearTaskDuckSecondary", ClearTaskDuckSecondary ? "ok" : "FALTA");
    g_pauseUser = (const volatile uint8_t*)aml->GetSym(h, "_ZN6CTimer11m_UserPauseE");
    g_pauseCode = (const volatile uint8_t*)aml->GetSym(h, "_ZN6CTimer11m_CodePauseE");
    LOGI("  %s CTimer::m_UserPause, %s CTimer::m_CodePause", g_pauseUser ? "ok" : "FALTA", g_pauseCode ? "ok" : "FALTA");
    g_scriptSpace = (uint8_t*)aml->GetSym(h, "_ZN11CTheScripts11ScriptSpaceE");
    LOGI("  %s _ZN11CTheScripts11ScriptSpaceE", g_scriptSpace ? "ok     " : "FALTA  ");
    g_cutPtr = (const volatile uint8_t*)aml->GetSym(h, "_ZN12CCutsceneMgr10ms_runningE");
    g_loadPtr = (const volatile uint8_t*)aml->GetSym(h, "_ZN19CGenericGameStorage11ms_bLoadingE");
    LOGI("  %s CCutsceneMgr::ms_running, %s CGenericGameStorage::ms_bLoading", g_cutPtr ? "ok" : "FALTA", g_loadPtr ? "ok" : "FALTA");
    void* process = (void*)aml->GetSym(h, "_ZN5CGame7ProcessEv");
    LOGI("  %s _ZN5CGame7ProcessEv", process ? "ok     " : "FALTA  ");
    #undef SYM

    Rescan();   // lista las skins aunque falte algun simbolo
    if (g_remember && g_lastName != "CJ") {   // recordar: volver a querer la ultima skin
        for (size_t i = 0; i < g_skins.size(); i++)
            if (skinscan::Lower(g_skins[i].name) == skinscan::Lower(g_lastName)) {
                g_desired = (int)i; g_needReapply = true; g_reapplyDelay = 150;
                LOGI("ultima skin recordada: %s", g_lastName.c_str());
                break;
            }
    }

    if (!RequestSpecialChar || !LoadAllRequestedModels || !HasSpecialCharLoaded ||
        !SetMissionDoesntRequireSpecialChar || !FindPlayerPed || !SetModelIndex || !process) {
        LOGI("faltan simbolos de libGTASA.so: no se puede cambiar la skin");
        return;
    }
    aml->Hook(process, (void*)CGame_Process_hook, (void**)&CGame_Process_orig);
    LOGI("hook puesto en CGame::Process");
    void* initRestart = (void*)aml->GetSym(h, "_ZN5CGame24InitialiseWhenRestartingEv");
    void* shutRestart = (void*)aml->GetSym(h, "_ZN5CGame18ShutDownForRestartEv");
    LOGI("  %s CGame::InitialiseWhenRestarting, %s CGame::ShutDownForRestart", initRestart ? "ok" : "FALTA", shutRestart ? "ok" : "FALTA");
    if (initRestart) aml->Hook(initRestart, (void*)CGame_InitRestart_hook, (void**)&CGame_InitRestart_orig);
    if (shutRestart) aml->Hook(shutRestart, (void*)CGame_ShutRestart_hook, (void**)&CGame_ShutRestart_orig);
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
