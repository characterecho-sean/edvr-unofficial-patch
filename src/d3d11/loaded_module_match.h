// Which already-loaded module a LoadLibrary request may be answered with.
//
// The proxy hooks LoadLibrary so ReShade's deadlock guard is not entered for DLLs that are
// already mapped. That shortcut must only answer a request the real loader would answer with
// the same module: a bare name ("advapi32.dll") or a full path that is itself loaded. A
// qualified path whose own lookup missed (b\same_name.dll while a\same_name.dll is loaded)
// names a different file, so it goes to the real loader, never to a basename match.
//
// `lookup` is the OS query (GetModuleHandleEx with flags 0), passed in so a rig can supply a
// list of loaded modules.
#pragma once

#include <string>

namespace edvr {

template <class Char>
bool loadedModuleNameIsBare(const Char* name) {
    for (const Char* p = name; *p; ++p) {
        if (*p == Char('\\') || *p == Char('/')) return false;
    }
    return true;
}

template <class Char, class Lookup>
auto matchLoadedModule(const Char* name, Lookup&& lookup) -> decltype(lookup(name)) {
    using Handle = decltype(lookup(name));
    if (!name || !*name) return Handle{};
    if (Handle mod = lookup(name)) return mod;
    if (!loadedModuleNameIsBare(name)) return Handle{};  // qualified miss: the real loader decides

    bool hasDot = false;
    for (const Char* p = name; *p; ++p) {
        if (*p == Char('.')) hasDot = true;
    }
    if (!hasDot) {
        std::basic_string<Char> withExt(name);
        for (const char* e = ".dll"; *e; ++e) withExt.push_back(Char(*e));
        if (Handle mod = lookup(withExt.c_str())) return mod;
    }
    return Handle{};
}

}  // namespace edvr
