#include "state.h"

#include <windows.h>

#include <cstdio>
#include <stdexcept>
#include <vector>

#include "detect.h"
#include "../common/iniedit.h"

namespace edvr::installer {
namespace {

std::string trimStateToken(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lowerStateToken(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

bool isPluginSectionName(const std::string& section) {
    return lowerStateToken(section) == "plugins";
}

// Also recognize a broken [plugins header so it is retained as opaque data
// rather than disappearing as an ordinary comment during a state rewrite.
bool isPluginSectionCandidate(const IniLine& line) {
    const std::string body = trimStateToken(line.text);
    if (body.empty() || body[0] != '[') return false;
    const std::string inside = trimStateToken(body.substr(1));
    if (inside.size() < 7 || lowerStateToken(inside.substr(0, 7)) != "plugins") return false;
    return inside.size() == 7 || inside[7] == ']' || inside[7] == ' ' || inside[7] == '\t';
}

bool validPluginId(const std::string& id) {
    if (id.empty() || id.front() == '-' || id.back() == '-') return false;
    bool previousDash = false;
    for (char c : id) {
        const bool dash = c == '-';
        const bool alphanumeric = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!dash && !alphanumeric) return false;
        if (dash && previousDash) return false;
        previousDash = dash;
    }
    return true;
}

bool parseSelectedPluginIds(const std::string& value, std::vector<std::string>* ids) {
    ids->clear();
    if (value.empty()) return true;
    size_t begin = 0;
    while (begin <= value.size()) {
        const size_t comma = value.find(',', begin);
        const size_t end = comma == std::string::npos ? value.size() : comma;
        const std::string id = value.substr(begin, end - begin);
        if (!validPluginId(id)) return false;
        for (const std::string& prior : *ids) {
            if (prior == id) return false;
        }
        ids->push_back(id);
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
    return true;
}

PluginSelectionRecord parsePluginSelection(const IniDoc& doc) {
    PluginSelectionRecord record;
    std::vector<const IniLine*> pluginLines;
    size_t sectionCount = 0;
    bool inPluginSection = false;
    for (const IniLine& line : doc.lines) {
        const bool candidate = isPluginSectionCandidate(line);
        if (candidate) {
            ++sectionCount;
            inPluginSection = true;
        } else if (line.kind == LineKind::Section) {
            inPluginSection = false;
        }
        if (inPluginSection) {
            record.opaqueText += line.text;
            record.opaqueText += line.eol;
            pluginLines.push_back(&line);
        }
    }
    if (sectionCount == 0) {
        record.opaqueText.clear();
        return record;
    }

    auto opaque = [&]() {
        record.state = PluginSelectionRecordState::Opaque;
        record.selectedIds.clear();
        return record;
    };
    if (sectionCount != 1 || pluginLines.empty() ||
        pluginLines.front()->kind != LineKind::Section ||
        lowerStateToken(trimStateToken(pluginLines.front()->text)) != "[plugins]") {
        return opaque();
    }

    bool haveSelectionSchema = false;
    bool haveCatalogSchema = false;
    bool haveProfile = false;
    bool haveSelectedIds = false;
    std::string selectedValue;
    for (size_t i = 1; i < pluginLines.size(); ++i) {
        const IniLine& line = *pluginLines[i];
        if (line.kind != LineKind::Key || !isPluginSectionName(line.section)) return opaque();
        // Refuse comments, inline comments and noncanonical spacing only when
        // they change the value seen by the INI parser; otherwise known keys
        // can be safely normalized by the serializer.
        const size_t eq = line.text.find('=');
        if (eq == std::string::npos || trimStateToken(line.text.substr(eq + 1)) != line.value)
            return opaque();
        if (line.key == "selection_schema_version") {
            if (haveSelectionSchema || line.value != "1") return opaque();
            haveSelectionSchema = true;
        } else if (line.key == "catalog_schema_version") {
            if (haveCatalogSchema || line.value != "2") return opaque();
            haveCatalogSchema = true;
        } else if (line.key == "profile") {
            if (haveProfile || (line.value != "vr" && line.value != "flat")) return opaque();
            record.profile = line.value;
            haveProfile = true;
        } else if (line.key == "selected_ids") {
            if (haveSelectedIds) return opaque();
            selectedValue = line.value;
            haveSelectedIds = true;
        } else {
            return opaque();
        }
    }
    if (!haveSelectionSchema || !haveCatalogSchema || !haveProfile || !haveSelectedIds ||
        !parseSelectedPluginIds(selectedValue, &record.selectedIds)) {
        return opaque();
    }

    record.state = PluginSelectionRecordState::Supported;
    record.schemaVersion = 1;
    record.catalogSchemaVersion = 2;
    record.opaqueText.clear();
    return record;
}

bool supportedPluginSelection(const PluginSelectionRecord& record) {
    if (record.state != PluginSelectionRecordState::Supported ||
        record.schemaVersion != 1 || record.catalogSchemaVersion != 2 ||
        (record.profile != "vr" && record.profile != "flat")) return false;
    std::vector<std::string> checked;
    for (const std::string& id : record.selectedIds) {
        if (!validPluginId(id)) return false;
        for (const std::string& prior : checked) {
            if (prior == id) return false;
        }
        checked.push_back(id);
    }
    return true;
}

}  // namespace

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

const wchar_t* settingsLeafFor(const std::string& profile) {
    return profile == "flat" ? L"edvr-flat.ini" : L"edvr.ini";
}
std::wstring settingsPathFor(const std::wstring& gameDir, const std::string& profile) {
    return joinPath(gameDir, settingsLeafFor(profile));
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
    s.profile = get("edvr", "profile");
    if (s.profile.empty()) s.profile = "vr";
    s.descriptorSha = get("edvr", "descriptor_sha256");
    s.components = get("edvr", "components");
    s.openvrDir = fromUtf8(get("edvr", "openvr_dir"));
    s.pluginSelection = parsePluginSelection(doc);

    s.d3d11Sha = get("d3d11", "sha256");
    s.d3d11Installed = !s.d3d11Sha.empty();
    s.chainTarget = fromUtf8(get("d3d11", "chain_target"));
    s.chainMod = fromUtf8(get("d3d11", "chain_mod"));

    s.openvrSha = get("openvr", "sha256");
    s.openvrInstalled = !s.openvrSha.empty();
    s.openvrOrigName = fromUtf8(get("openvr", "orig_name"));
    s.openvrOrigSha = get("openvr", "orig_sha256");

    s.ngxSha = get("ngx", "sha256");
    s.ngxInstalled = !s.ngxSha.empty();

    s.iniSha = get("ini", "sha256");
    s.nativeGraphicsSha = get("native", "graphics_sha256");
    s.nativeRuntimeSha = get("native", "runtime_sha256");
    s.openxrLoaderSha = get("native", "loader_sha256");
    s.openxrLicenseSha = get("native", "license_sha256");
    s.nativeConfigSha = get("native", "config_sha256");
    s.nativeOriginalName = fromUtf8(get("native", "original_name"));
    s.nativeOriginalSha = get("native", "original_sha256");
    s.nativeInstalled = !s.nativeRuntimeSha.empty();

    // A record with no version is not a record; it is a file that happens to
    // parse. Everything downstream keys off `present`, so it has to mean
    // "written by this installer", not "the parse did not fail".
    s.present = !s.edvrVersion.empty() || s.d3d11Installed || s.openvrInstalled || s.nativeInstalled;
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
    out += "profile = " + state.profile + "\r\n";
    out += "descriptor_sha256 = " + state.descriptorSha + "\r\n";
    out += "components = " + state.components + "\r\n";
    out += "openvr_dir = " + toUtf8(state.openvrDir) + "\r\n";

    out += "\r\n[d3d11]\r\n";
    out += "sha256 = " + (state.d3d11Installed ? state.d3d11Sha : std::string()) + "\r\n";
    out += "chain_target = " + toUtf8(state.chainTarget) + "\r\n";
    out += "chain_mod = " + toUtf8(state.chainMod) + "\r\n";

    out += "\r\n[openvr]\r\n";
    out += "sha256 = " + (state.openvrInstalled ? state.openvrSha : std::string()) + "\r\n";
    out += "orig_name = " + toUtf8(state.openvrOrigName) + "\r\n";
    out += "orig_sha256 = " + state.openvrOrigSha + "\r\n";

    out += "\r\n[ngx]\r\n";
    out += "sha256 = " + (state.ngxInstalled ? state.ngxSha : std::string()) + "\r\n";

    out += "\r\n[ini]\r\n";
    out += "sha256 = " + state.iniSha + "\r\n";
    out += "\r\n[native]\r\n";
    out += "graphics_sha256 = " + state.nativeGraphicsSha + "\r\n";
    out += "runtime_sha256 = " + state.nativeRuntimeSha + "\r\n";
    out += "loader_sha256 = " + state.openxrLoaderSha + "\r\n";
    out += "license_sha256 = " + state.openxrLicenseSha + "\r\n";
    out += "config_sha256 = " + state.nativeConfigSha + "\r\n";
    out += "original_name = " + toUtf8(state.nativeOriginalName) + "\r\n";
    out += "original_sha256 = " + state.nativeOriginalSha + "\r\n";
    if (supportedPluginSelection(state.pluginSelection)) {
        out += "\r\n[plugins]\r\n";
        out += "selection_schema_version = 1\r\n";
        out += "catalog_schema_version = 2\r\n";
        out += "profile = " + state.pluginSelection.profile + "\r\n";
        out += "selected_ids = ";
        for (size_t i = 0; i < state.pluginSelection.selectedIds.size(); ++i) {
            if (i) out += ',';
            out += state.pluginSelection.selectedIds[i];
        }
        out += "\r\n";
    } else if (state.pluginSelection.state == PluginSelectionRecordState::Opaque) {
        if (state.pluginSelection.opaqueText.empty())
            throw std::invalid_argument("cannot serialize an empty opaque plugin selection record");
        // A broken [plugins header is a comment to IniDoc, so its following
        // keys would otherwise inherit [native] from the generated record.
        // The valid header becomes part of opaqueText on the next parse, which
        // makes subsequent rewrites byte-stable without adding more guards.
        const IniDoc opaqueDoc = iniParse(state.pluginSelection.opaqueText);
        if (opaqueDoc.lines.empty() || opaqueDoc.lines.front().kind != LineKind::Section ||
            !isPluginSectionName(opaqueDoc.lines.front().section)) {
            out += "[plugins]\r\n";
        }
        out += state.pluginSelection.opaqueText;
    } else if (state.pluginSelection.state == PluginSelectionRecordState::Supported) {
        throw std::invalid_argument("cannot serialize an invalid supported plugin selection record");
    }
    return out;
}

}  // namespace edvr::installer
