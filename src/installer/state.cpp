#include "state.h"

#include <windows.h>

#include <cstdio>
#include <vector>

#include "detect.h"
#include "iniedit.h"

namespace edvr::installer {

std::wstring stateDirPath(const std::wstring& gameDir) {
    return joinPath(gameDir, L"edvr_install");
}
std::wstring statePath(const std::wstring& gameDir) {
    return joinPath(stateDirPath(gameDir), L"state.ini");
}
std::wstring baseIniPath(const std::wstring& gameDir) {
    return joinPath(stateDirPath(gameDir), L"edvr.ini.base");
}
std::wstring backupRootPath(const std::wstring& gameDir) {
    return joinPath(gameDir, L"edvr_backup");
}

std::string utcNow() {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char buf[32];
    sprintf_s(buf, "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour,
              st.wMinute, st.wSecond);
    return std::string(buf);
}

std::wstring timestampName() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[32];
    _snwprintf_s(buf, _TRUNCATE, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond);
    return std::wstring(buf);
}

InstallState parseState(const std::string& text) {
    InstallState s;
    if (text.empty()) return s;
    const IniDoc doc = iniParse(text);

    auto get = [&](const char* section, const char* key) -> std::string {
        const IniLine* l = doc.findKey(section, key);
        return l ? l->value : std::string();
    };

    s.edvrVersion = get("edvr", "version");
    s.installedUtc = get("edvr", "installed_utc");
    s.openvrDir = fromUtf8(get("edvr", "openvr_dir"));

    s.d3d11Sha = get("d3d11", "sha256");
    s.d3d11Installed = !s.d3d11Sha.empty();
    s.chainTarget = fromUtf8(get("d3d11", "chain_target"));
    s.chainMod = fromUtf8(get("d3d11", "chain_mod"));

    s.openvrSha = get("openvr", "sha256");
    s.openvrInstalled = !s.openvrSha.empty();
    s.openvrOrigName = fromUtf8(get("openvr", "orig_name"));
    s.openvrOrigSha = get("openvr", "orig_sha256");

    s.iniSha = get("ini", "sha256");

    // A record with no version is not a record; it is a file that happens to
    // parse. Everything downstream keys off `present`, so it has to mean
    // "written by this installer", not "the parse did not fail".
    s.present = !s.edvrVersion.empty() || s.d3d11Installed || s.openvrInstalled;
    return s;
}

InstallState readState(const std::wstring& gameDir) {
    InstallState s = parseState(readTextFile(statePath(gameDir)));
    s.hasBaseIni = fileExists(baseIniPath(gameDir));
    return s;
}

std::string serializeState(const InstallState& state) {
    std::string out;
    out += "# EDVR install record. Written by the installer; read by it on the next run.\r\n";
    out += "# Deleting this file loses nothing the game needs -- it only makes the\r\n";
    out += "# installer fall back to guessing what a previous run did.\r\n";
    out += "\r\n[edvr]\r\n";
    out += "version = " + state.edvrVersion + "\r\n";
    out += "installed_utc = " + state.installedUtc + "\r\n";
    out += "openvr_dir = " + toUtf8(state.openvrDir) + "\r\n";

    out += "\r\n[d3d11]\r\n";
    out += "sha256 = " + (state.d3d11Installed ? state.d3d11Sha : std::string()) + "\r\n";
    out += "chain_target = " + toUtf8(state.chainTarget) + "\r\n";
    out += "chain_mod = " + toUtf8(state.chainMod) + "\r\n";

    out += "\r\n[openvr]\r\n";
    out += "sha256 = " + (state.openvrInstalled ? state.openvrSha : std::string()) + "\r\n";
    out += "orig_name = " + toUtf8(state.openvrOrigName) + "\r\n";
    out += "orig_sha256 = " + state.openvrOrigSha + "\r\n";

    out += "\r\n[ini]\r\n";
    out += "sha256 = " + state.iniSha + "\r\n";
    return out;
}

}  // namespace edvr::installer
