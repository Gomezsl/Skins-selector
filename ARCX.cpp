// ARCX.cpp - Project ARCX (beta) by Hunter09: un solo plugin AML (libARCX64.so) con el nucleo de skins y la interfaz.
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
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "skinscan.h"
#include "arcx_common.h"
#include "arcx_core.h"
#include "arcx_ui.h"

MYMODCFG(net.hunter09.arcx, Project ARCX, 3.0.0-beta, Hunter09)
NEEDGAME(com.rockstargames.gtasa)

#define ARCX_VERSION "3.0.0-beta"

static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsa::LogV("main", "ARCX", fmt, ap);
    va_end(ap);
}

// carpeta "files" del juego (en algunos telefonos es Android_unprotected); de ella sale Android/data/<pkg>
static const char* kFilesDirs[] = {
    "/storage/emulated/0/Android_unprotected/data/com.rockstargames.gtasa/files",
    "/storage/emulated/0/Android/data/com.rockstargames.gtasa/files",
    "/sdcard/Android/data/com.rockstargames.gtasa/files",
    "/storage/self/primary/Android/data/com.rockstargames.gtasa/files",
};

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
    std::string base = files;            // .../Android/data/com.rockstargames.gtasa
    const std::string suf = "/files";
    if (base.size() > suf.size() && base.compare(base.size() - suf.size(), suf.size(), suf) == 0) base.resize(base.size() - suf.size());

    const std::string cfgDir = base + "/configs";
    mkdir(cfgDir.c_str(), 0777);
    vsa::g_cfgPath = cfgDir + "/ARCXConfigs.ini";    // todos los ajustes en un solo archivo
    vsa::g_logPath = base + "/ARCXLog.txt";          // un solo log
    vsa::LogInit(ARCX_VERSION);
    Log("== Project ARCX %s cargado ==", ARCX_VERSION);
    Log("datos: %s", base.c_str());

    // si la ultima skin aplicada no termino, el juego se cerro en medio
    {
        vsa::Ini ini = vsa::CfgRead();
        std::string ap = vsa::Get(ini, "Debug", "applying");
        if (!ap.empty()) {
            Log("!! la ultima skin que se estaba aplicando ('%s') cerro el juego", ap.c_str());
            vsa::CfgUpdate([](vsa::Ini& i) { i["Debug"]["applying"] = ""; });
        }
    }

    core::Init(files + "/modloader");
    ui::Init();
}
