#include "state.h"

#include <windows.h>

#include <wchar.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "detect.h"
#include "../common/iniedit.h"

namespace edvr::installer {

namespace {

// The part of JSON a receipt uses. A receipt nests its own "target" and "files" keys, so a field scan would read the wrong one; the
// tree is parsed whole instead, and anything malformed is refused, because a receipt that does not parse proves nothing.
struct Json {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    std::string text;               // a string's value, or a number's digits as written
    std::vector<std::string> names; // an object's member names, in order
    std::vector<Json> values;       // an object's member values, or an array's elements

    // The last member with this name: a JSON reader keeps the last of a repeated key.
    const Json* member(const char* name) const {
        for (size_t i = names.size(); i-- > 0;)
            if (names[i] == name) return &values[i];
        return nullptr;
    }
};

class JsonReader {
public:
    explicit JsonReader(const std::string& text) : text_(text) {}

    bool parseDocument(Json* out) {
        if (!value(out, 0)) return false;
        skipSpace();
        return pos_ == text_.size();
    }

private:
    static const int kMaxDepth = 8;   // a receipt nests three deep
    const std::string& text_;
    size_t pos_ = 0;

    void skipSpace() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\r' || text_[pos_] == '\n'))
            ++pos_;
    }

    bool literal(const char* word) {
        const size_t n = strlen(word);
        if (text_.compare(pos_, n, word) != 0) return false;
        pos_ += n;
        return true;
    }

    bool value(Json* out, int depth) {
        if (depth > kMaxDepth) return false;
        skipSpace();
        if (pos_ >= text_.size()) return false;
        const char c = text_[pos_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            out->kind = Json::Kind::String;
            return stringValue(&out->text);
        }
        if (literal("true")) { out->kind = Json::Kind::Bool; out->text = "true"; return true; }
        if (literal("false")) { out->kind = Json::Kind::Bool; out->text = "false"; return true; }
        if (literal("null")) { out->kind = Json::Kind::Null; return true; }
        return number(out);
    }

    bool object(Json* out, int depth) {
        out->kind = Json::Kind::Object;
        ++pos_;
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == '}') { ++pos_; return true; }
        for (;;) {
            skipSpace();
            std::string name;
            if (pos_ >= text_.size() || text_[pos_] != '"' || !stringValue(&name)) return false;
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_] != ':') return false;
            ++pos_;
            Json child;
            if (!value(&child, depth + 1)) return false;
            out->names.push_back(name);
            out->values.push_back(child);
            skipSpace();
            if (pos_ >= text_.size()) return false;
            if (text_[pos_] == ',') { ++pos_; continue; }
            if (text_[pos_] == '}') { ++pos_; return true; }
            return false;
        }
    }

    bool array(Json* out, int depth) {
        out->kind = Json::Kind::Array;
        ++pos_;
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == ']') { ++pos_; return true; }
        for (;;) {
            Json child;
            if (!value(&child, depth + 1)) return false;
            out->values.push_back(child);
            skipSpace();
            if (pos_ >= text_.size()) return false;
            if (text_[pos_] == ',') { ++pos_; continue; }
            if (text_[pos_] == ']') { ++pos_; return true; }
            return false;
        }
    }

    // Called with pos_ on the opening quote.
    bool stringValue(std::string* out) {
        ++pos_;
        out->clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c != '\\') { out->push_back(c); continue; }
            if (pos_ >= text_.size()) return false;
            const char e = text_[pos_++];
            switch (e) {
                case '"': case '\\': case '/': out->push_back(e); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    unsigned code = 0;
                    if (!hex4(&code)) return false;
                    if (code >= 0xD800 && code < 0xDC00) {   // a high surrogate: its low half must follow
                        unsigned low = 0;
                        if (text_.compare(pos_, 2, "\\u") != 0) return false;
                        pos_ += 2;
                        if (!hex4(&low) || low < 0xDC00 || low > 0xDFFF) return false;
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                    appendUtf8(code, out);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool hex4(unsigned* code) {
        if (pos_ + 4 > text_.size()) return false;
        *code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            unsigned d = 0;
            if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
            else return false;
            *code = *code * 16 + d;
        }
        return true;
    }

    bool number(Json* out) {
        const size_t start = pos_;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            const bool digit = c >= '0' && c <= '9';
            if (!digit && c != '-' && c != '+' && c != '.' && c != 'e' && c != 'E') break;
            ++pos_;
        }
        if (pos_ == start) return false;
        out->kind = Json::Kind::Number;
        out->text = text_.substr(start, pos_ - start);
        return true;
    }

    static void appendUtf8(unsigned code, std::string* out) {
        if (code < 0x80) {
            out->push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (code >> 6)));
            out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (code >> 12)));
            out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (code >> 18)));
            out->push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
};

std::string stringOf(const Json* v) {
    return v && v->kind == Json::Kind::String ? v->text : std::string();
}

// A folder path without its trailing separator, so "C:\Games\ED\" and "C:\Games\ED" are the same folder.
std::wstring withoutTrailingSeparator(std::wstring path) {
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return path;
}

bool sameFolder(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(withoutTrailingSeparator(a).c_str(), withoutTrailingSeparator(b).c_str()) == 0;
}

bool allDigits(const std::wstring& s, size_t from, size_t to) {
    for (size_t i = from; i < to; ++i)
        if (s[i] < L'0' || s[i] > L'9') return false;
    return to > from;
}

// The tail of a backup name after "<family>.pre-": <tag>-<YYYYMMDD-HHMMSS>.bak, optionally followed by ".<n>". The stamp and the
// counter are what decide which backup is the newest.
bool backupTail(const std::wstring& tail, std::wstring* stamp, unsigned long* counter) {
    std::wstring core = tail;
    *counter = 0;
    const size_t dot = core.rfind(L'.');
    if (dot != std::wstring::npos && dot + 1 < core.size() && allDigits(core, dot + 1, core.size())) {
        if (core.size() - dot - 1 > 9) return false;
        *counter = wcstoul(core.c_str() + dot + 1, nullptr, 10);
        core.resize(dot);
    }
    // A tag of at least one character, a dash, the 15-character stamp and the .bak suffix.
    if (core.size() < 21 || core.compare(core.size() - 4, 4, L".bak") != 0) return false;
    core.resize(core.size() - 4);
    const size_t at = core.size() - 15;   // the stamp: 8 digits, a dash, 6 digits
    if (core[at - 1] != L'-' || !allDigits(core, at, at + 8) || core[at + 8] != L'-' ||
        !allDigits(core, at + 9, at + 15))
        return false;
    *stamp = core.substr(at);
    return true;
}

// install_edvr.py's current receipt of one family: the newest backup-named sibling when there is one, else the receipt itself.
std::wstring currentReceiptPath(const std::wstring& gameDir, const std::wstring& family) {
    const std::wstring prefix = family + L".pre-";
    std::wstring best, bestStamp;
    unsigned long bestCounter = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(joinPath(gameDir, L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            const std::wstring name = fd.cFileName;
            if (name.size() <= prefix.size() || _wcsnicmp(name.c_str(), prefix.c_str(), prefix.size()) != 0) continue;
            std::wstring stamp;
            unsigned long counter = 0;
            if (!backupTail(name.substr(prefix.size()), &stamp, &counter)) continue;
            if (best.empty() || stamp > bestStamp || (stamp == bestStamp && counter > bestCounter)) {
                best = joinPath(gameDir, name);
                bestStamp = stamp;
                bestCounter = counter;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!best.empty()) return best;
    const std::wstring base = joinPath(gameDir, family);
    return fileExists(base) ? base : std::wstring();
}

// The receipt as the tool wrote it, or false when it is not one of its installed receipts for this folder.
bool readOneReceipt(const std::wstring& gameDir, const std::wstring& path, ToolReceipt* out) {
    const std::string text = readTextFile(path, 1u << 20);
    if (text.empty()) return false;
    Json root;
    if (!JsonReader(text).parseDocument(&root) || root.kind != Json::Kind::Object) return false;
    const std::string kind = stringOf(root.member("kind"));
    const bool native = kind == "edvr-native-openxr";
    if (!native && kind != "edvr-flat") return false;
    const Json* version = root.member("version");
    if (!version || version->kind != Json::Kind::Number) return false;
    if (version->text != "1" && version->text != "2") return false;
    if (version->text == "1" && !native) return false;   // the flat kind has no version 1
    if (stringOf(root.member("state")) != "installed") return false;
    const std::wstring target = fromUtf8(stringOf(root.member("target")));
    if (target.empty() || !sameFolder(target, gameDir)) return false;
    const Json* files = root.member("files");
    if (!files || files->kind != Json::Kind::Array) return false;

    ToolReceipt receipt;
    receipt.path = path;
    receipt.native = native;
    for (const Json& entry : files->values) {
        if (entry.kind != Json::Kind::Object) return false;
        ToolReceiptFile file;
        file.key = stringOf(entry.member("key"));
        file.target = fromUtf8(stringOf(entry.member("target")));
        file.backup = fromUtf8(stringOf(entry.member("backup")));
        file.beforeSha = stringOf(entry.member("before_sha256"));
        file.installedSha = stringOf(entry.member("installed_sha256"));
        receipt.files.push_back(file);
    }
    *out = receipt;
    return true;
}

}  // namespace

std::vector<ToolReceipt> readToolReceipts(const std::wstring& gameDir) {
    std::vector<ToolReceipt> out;
    for (const wchar_t* family : {L"edvr_native_receipt.json", L"edvr_flat_receipt.json"}) {
        const std::wstring path = currentReceiptPath(gameDir, family);
        ToolReceipt receipt;
        if (!path.empty() && readOneReceipt(gameDir, path, &receipt)) out.push_back(receipt);
    }
    return out;
}

std::wstring stateDirPath(const std::wstring& gameDir) {
    return joinPath(gameDir, L"edvr_install");
}
std::wstring statePath(const std::wstring& gameDir) {
    return joinPath(stateDirPath(gameDir), L"state.ini");
}
std::wstring baseIniPath(const std::wstring& gameDir) {
    return joinPath(stateDirPath(gameDir), L"edvr.ini.base");
}
std::wstring flatBaseIniPath(const std::wstring& gameDir) {
    return joinPath(stateDirPath(gameDir), L"edvr-flat.ini.base");
}
std::wstring backupRootPath(const std::wstring& gameDir) {
    return joinPath(gameDir, L"edvr_backup");
}

std::string recordedScope(const InstallState& state) {
    if (!state.scope.empty()) return state.scope;
    if (state.profile == "flat") return "flat";
    if (state.profile == "vr") return "both";
    return std::string();
}

// The settings files a scope installs: edvr.ini for vr and both, edvr-flat.ini for flat and both.
bool scopeHasVrIni(const std::string& scope) { return scope != "flat"; }
bool scopeHasFlatIni(const std::string& scope) { return scope != "vr"; }

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
    s.scope = get("edvr", "scope");
    if (s.scope.empty()) s.scope = recordedScope(s);   // a record from before scopes: its profile says which
    s.descriptorSha = get("edvr", "descriptor_sha256");
    s.components = get("edvr", "components");
    s.openvrDir = fromUtf8(get("edvr", "openvr_dir"));

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
    s.xessSha = get("xess", "sha256");
    s.xessLicenseSha = get("xess", "license_sha256");
    s.xessThirdPartySha = get("xess", "third_party_sha256");

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
    out += "scope = " + recordedScope(state) + "\r\n";
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
    out += "\r\n[xess]\r\n";
    out += "sha256 = " + state.xessSha + "\r\n";
    out += "license_sha256 = " + state.xessLicenseSha + "\r\n";
    out += "third_party_sha256 = " + state.xessThirdPartySha + "\r\n";

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
    return out;
}

}  // namespace edvr::installer
