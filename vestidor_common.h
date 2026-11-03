// vestidor_common.h - carpeta de trabajo, log unico (log.txt) y archivos .ini compartidos por el nucleo y el menu
#pragma once
#include <android/log.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace vsa {

inline std::string g_dir;   // .../files/VestidorSA
inline std::string P(const char* name) { return g_dir + "/" + name; }

// ---- log unico: log.txt lo escriben el nucleo y el menu (cada linea lleva [nucleo] o [menu]) ----
inline void LogInit() {                       // se empieza de cero solo una vez por sesion (la primera cabecera lleva el pid)
    if (g_dir.empty()) return;
    char mine[48];
    snprintf(mine, sizeof mine, "#pid=%d", (int)getpid());
    char first[64] = {0};
    FILE* f = fopen(P("log.txt").c_str(), "r");
    if (f) {
        if (!fgets(first, sizeof first, f)) first[0] = 0;
        fclose(f);
    }
    size_t n = strlen(first);
    while (n && (first[n - 1] == '\n' || first[n - 1] == '\r')) first[--n] = 0;
    if (strcmp(first, mine) == 0) return;      // el otro plugin ya lo inicio en esta sesion
    f = fopen(P("log.txt").c_str(), "w");
    if (f) { fprintf(f, "%s\n", mine); fclose(f); }
}
inline void LogV(const char* who, const char* tag, const char* fmt, va_list ap) {
    char buf[600];
    vsnprintf(buf, sizeof buf, fmt, ap);
    __android_log_print(ANDROID_LOG_INFO, tag, "%s", buf);
    if (g_dir.empty()) return;
    FILE* f = fopen(P("log.txt").c_str(), "a");
    if (f) { fprintf(f, "[%s] %s\n", who, buf); fclose(f); }
}

// ---- INI minimo: [Seccion] y clave=valor ----
struct KeyLess {                               // las claves numericas se ordenan como numeros (1, 2, ... 10)
    bool operator()(const std::string& a, const std::string& b) const {
        bool na = !a.empty() && a.find_first_not_of("0123456789") == std::string::npos;
        bool nb = !b.empty() && b.find_first_not_of("0123456789") == std::string::npos;
        if (na && nb) return a.size() != b.size() ? a.size() < b.size() : a < b;
        return a < b;
    }
};
using Section = std::map<std::string, std::string, KeyLess>;
using Ini = std::map<std::string, Section>;

inline std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}
inline Ini IniRead(const std::string& path) {
    Ini ini;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return ini;
    char line[1500];
    std::string sec;
    while (fgets(line, sizeof line, f)) {
        std::string l = Trim(line);
        if (l.empty() || l[0] == ';' || l[0] == '#') continue;
        if (l[0] == '[' && l.back() == ']') { sec = l.substr(1, l.size() - 2); continue; }
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        ini[sec][Trim(l.substr(0, eq))] = Trim(l.substr(eq + 1));
    }
    fclose(f);
    return ini;
}
inline bool IniWrite(const std::string& path, const Ini& ini) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return false;
    for (auto& s : ini) {
        fprintf(f, "[%s]\n", s.first.c_str());
        for (auto& kv : s.second) fprintf(f, "%s=%s\n", kv.first.c_str(), kv.second.c_str());
        fputc('\n', f);
    }
    fclose(f);
    return rename(tmp.c_str(), path.c_str()) == 0;
}
inline std::string Get(const Ini& ini, const char* sec, const char* key, const std::string& def = "") {
    auto s = ini.find(sec);
    if (s == ini.end()) return def;
    auto k = s->second.find(key);
    return k == s->second.end() ? def : k->second;
}
inline int GetInt(const Ini& ini, const char* sec, const char* key, int def) {
    std::string v = Get(ini, sec, key);
    return v.empty() ? def : atoi(v.c_str());
}
inline float GetFloat(const Ini& ini, const char* sec, const char* key, float def) {
    std::string v = Get(ini, sec, key);
    return v.empty() ? def : (float)atof(v.c_str());
}

}  // namespace vsa
