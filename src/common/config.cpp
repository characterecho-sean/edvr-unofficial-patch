#include "config.h"

#include <windows.h>

#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace edvr {

struct Config::Impl {
    std::map<std::string, std::string> values;
};

Config& Config::get() {
    static Config instance;
    return instance;
}

std::wstring moduleDirectory(void* hModule) {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(static_cast<HMODULE>(hModule), buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".";
    std::wstring s(buf, n);
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : s.substr(0, slash);
}

std::wstring executableDirectory() { return moduleDirectory(nullptr); }

bool ensureDirectory(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

void Config::init(const std::wstring& moduleDir) {
    if (!m_impl) m_impl = new Impl();

    const std::wstring candidates[] = {
        moduleDir + L"\\edvr.ini",
        executableDirectory() + L"\\edvr.ini",
    };
    m_path = candidates[0];
    for (const std::wstring& c : candidates) {
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) {
            m_path = c;
            break;
        }
    }
    parse();

    // Defaults to <exe dir>\edvr_logs; the shader dump lands in a subdirectory.
    std::string dirUtf8 = getString("log.dir", "");
    if (dirUtf8.empty()) {
        m_logDir = executableDirectory() + L"\\edvr_logs";
    } else {
        const int need = MultiByteToWideChar(CP_UTF8, 0, dirUtf8.c_str(), -1, nullptr, 0);
        std::vector<wchar_t> w(need > 0 ? need : 1, 0);
        MultiByteToWideChar(CP_UTF8, 0, dirUtf8.c_str(), -1, w.data(), need);
        m_logDir = w.data();
    }
    // Not created here. Log::open() makes it when it actually opens a file, so
    // logging turned off leaves no empty directory behind.
}

void Config::parse() {
    m_impl->values.clear();

    HANDLE f = CreateFileW(m_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;  // absent file is fine: all defaults

    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(f, &info)) m_lastWrite = info.ftLastWriteTime;

    const DWORD size = GetFileSize(f, nullptr);
    if (size == INVALID_FILE_SIZE || size > (1u << 20)) {
        CloseHandle(f);
        return;
    }
    std::vector<char> text(size + 1, 0);
    DWORD read = 0;
    ReadFile(f, text.data(), size, &read, nullptr);
    CloseHandle(f);

    const char* p = text.data();
    const char* end = p + read;

    // Skip a UTF-8 byte order mark.
    //
    // Notepad writes one by default, so an edvr.ini a user edited and saved is
    // likely to start with EF BB BF. Without this the BOM sticks to the front of
    // the first line, "[fix]" stops looking like a section header, and every
    // setting under it is filed under the WRONG section -- the file loads
    // "successfully", the log looks clean, and nothing the user changed has any
    // effect. Exactly the bug report nobody can diagnose.
    if (read >= 3 && static_cast<unsigned char>(p[0]) == 0xEF &&
        static_cast<unsigned char>(p[1]) == 0xBB &&
        static_cast<unsigned char>(p[2]) == 0xBF) {
        p += 3;
    }

    std::string section;
    while (p < end) {
        const char* lineEnd = p;
        while (lineEnd < end && *lineEnd != '\n' && *lineEnd != '\r') ++lineEnd;
        std::string line(p, lineEnd);
        p = lineEnd;
        while (p < end && (*p == '\n' || *p == '\r')) ++p;

        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        line = line.substr(first);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line[0] == '[') {
            const size_t close = line.find(']');
            if (close != std::string::npos) section = line.substr(1, close - 1);
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        auto trim = [](std::string& s) {
            const size_t a = s.find_first_not_of(" \t");
            if (a == std::string::npos) { s.clear(); return; }
            const size_t b = s.find_last_not_of(" \t");
            s = s.substr(a, b - a + 1);
        };
        trim(key);
        trim(val);
        if (key.empty()) continue;
        // Section-qualified keys, so [openvr] hook_compositor reads as
        // "openvr.hook_compositor". Bare "a.b = c" also works.
        m_impl->values[section.empty() ? key : section + "." + key] = val;
    }
}

bool Config::reloadIfChanged() {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(m_path.c_str(), GetFileExInfoStandard, &data)) return false;
    if (CompareFileTime(&data.ftLastWriteTime, &m_lastWrite) == 0) return false;
    parse();
    return true;
}

std::string Config::getString(const char* key, const char* def) const {
    if (!m_impl) return def ? def : "";
    auto it = m_impl->values.find(key);
    return it == m_impl->values.end() ? std::string(def ? def : "") : it->second;
}

bool Config::getBool(const char* key, bool def) const {
    const std::string v = getString(key, "");
    if (v.empty()) return def;
    return v == "1" || v == "true" || v == "yes" || v == "on" ||
           v == "TRUE" || v == "True";
}

int Config::getInt(const char* key, int def) const {
    const std::string v = getString(key, "");
    if (v.empty()) return def;
    return static_cast<int>(strtol(v.c_str(), nullptr, 0));
}

float Config::getFloat(const char* key, float def) const {
    const std::string v = getString(key, "");
    if (v.empty()) return def;
    return strtof(v.c_str(), nullptr);
}

}  // namespace edvr
