// Hot-reloadable key=value config.
//
// The user is wearing a headset and cannot see a text editor, so every tunable
// lives here and is re-read when the file's write time changes. Reload is
// polled from the frame loop, not watched, to keep the cost to one cheap
// GetFileAttributesEx every N frames.
#pragma once

#include <windows.h>  // FILETIME

#include <string>

namespace edvr {

class Config {
public:
    static Config& get();

    // Looks for edvr.ini next to the host module, then next to the .exe.
    void init(const std::wstring& moduleDir);

    // Returns true if the file changed and values were re-read.
    bool reloadIfChanged();

    bool        getBool(const char* key, bool def) const;
    int         getInt(const char* key, int def) const;
    float       getFloat(const char* key, float def) const;
    std::string getString(const char* key, const char* def) const;

    const std::wstring& path() const { return m_path; }
    const std::wstring& logDir() const { return m_logDir; }

private:
    Config() = default;
    void parse();

    struct Impl;
    Impl*        m_impl = nullptr;
    std::wstring m_path;
    std::wstring m_logDir;
    FILETIME     m_lastWrite{};
};

// Directory containing the given loaded module, without trailing slash.
std::wstring moduleDirectory(void* hModule);
// Directory containing the running .exe.
std::wstring executableDirectory();
bool ensureDirectory(const std::wstring& path);

}  // namespace edvr
