#include "apply.h"

#include <windows.h>

#include "probe.h"

#include <cstdio>

namespace edvr::installer {
namespace {

struct Undo {
    enum class Kind { RemoveDir, DeleteFile, MoveBack } kind;
    std::wstring a;  // the file to delete / the directory to remove / the moved-to path
    std::wstring b;  // MoveBack: where it came from
};

std::string errorText(DWORD err) {
    char* msg = nullptr;
    const DWORD n = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<char*>(&msg), 0,
        nullptr);
    std::string out;
    if (n && msg) {
        out.assign(msg, n);
        while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' '))
            out.pop_back();
    }
    if (msg) LocalFree(msg);
    if (out.empty()) {
        char buf[64];
        sprintf_s(buf, "Windows error %lu", err);
        out = buf;
    }
    return out;
}

// Written beside the target and moved into place.
//
// CREATE_ALWAYS on the target truncates it before the first byte is written, so
// a failure halfway -- disk full, the network drive going away, antivirus
// taking the handle -- leaves a truncated DLL where a working one was, and the
// rollback below has nothing to put back because the file "already existed".
// Writing a temp file first means the target is either the old bytes or all of
// the new ones.
bool writeWholeFile(const std::wstring& path, const void* data, size_t size, DWORD* err) {
    const std::wstring temp = path + L".edvrnew";
    HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        return false;
    }
    const unsigned char* p = static_cast<const unsigned char*>(data);
    size_t left = size;
    while (left > 0) {
        const DWORD chunk = static_cast<DWORD>(left > (1u << 20) ? (1u << 20) : left);
        DWORD written = 0;
        if (!WriteFile(f, p, chunk, &written, nullptr) || written == 0) {
            *err = GetLastError();
            if (*err == 0) *err = ERROR_WRITE_FAULT;  // a short write is still a failure
            CloseHandle(f);
            DeleteFileW(temp.c_str());
            return false;
        }
        p += written;
        left -= written;
    }
    // Flushed deliberately: the next thing that happens is usually the user
    // starting the game, and a d3d11.dll still sitting in the cache when the
    // machine loses power is a corrupt file at the one moment nobody would
    // suspect the installer.
    FlushFileBuffers(f);
    CloseHandle(f);

    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        *err = GetLastError();
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

// Read-only is how a launcher's file verification sometimes leaves things, and
// it makes both CopyFile and MoveFileEx fail with access denied -- which would
// otherwise be reported as "run as administrator", which would not help.
void clearReadOnly(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return;
    if (attrs & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
}

bool ensureDirTree(const std::wstring& path, std::vector<Undo>* undo) {
    if (path.empty() || dirExists(path)) return true;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) {
        if (!ensureDirTree(path.substr(0, slash), undo)) return false;
    }
    if (CreateDirectoryW(path.c_str(), nullptr)) {
        if (undo) undo->push_back({Undo::Kind::RemoveDir, path, std::wstring()});
        return true;
    }
    // ERROR_ALREADY_EXISTS is also what a FILE of that name gives back, and
    // treating that as success meant the run carried on and failed several
    // steps later -- after the DLLs had been replaced.
    return GetLastError() == ERROR_ALREADY_EXISTS && dirExists(path);
}

}  // namespace

bool canWriteInto(const std::wstring& dir) {
    if (dir.empty()) return false;
    wchar_t leaf[64];
    _snwprintf_s(leaf, _TRUNCATE, L"edvr_write_test_%lu.tmp", GetCurrentProcessId());
    const std::wstring probe = joinPath(dir, leaf);
    HANDLE f = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    CloseHandle(f);
    return true;
}

ApplyResult applyPlan(const Plan& plan, const PayloadProvider& payload) {
    ApplyResult result;
    if (plan.blocked) {
        result.error = "The plan is blocked; nothing was done.";
        return result;
    }

    // ---- nothing is touched until the folder still matches the plan ----
    //
    // The plan was worked out, shown, and then waited on a confirmation dialog
    // for as long as somebody took to read it. In that window another
    // installer can run, a game update can land, or a second copy of this
    // window can do the whole job -- and executing the stale plan then renames
    // EDVR's own proxy on top of the game's original. Checked here, before the
    // first step, so a stale plan does nothing at all rather than half of
    // something.
    for (const Step& step : plan.steps) {
        if (step.from.empty()) continue;
        const bool there = fileExists(step.from);
        if (!there && (step.required || !step.expectSha.empty())) {
            result.error = "The folder changed while this was waiting to be confirmed: " +
                           toUtf8(leafOf(step.from)) +
                           " is not there any more. Nothing was done -- look at it again.";
            return result;
        }
        if (there && !step.expectSha.empty() && sha256File(step.from) != step.expectSha) {
            result.error = "The folder changed while this was waiting to be confirmed: " +
                           toUtf8(leafOf(step.from)) +
                           " is not the file this plan was made for. Nothing was done -- close "
                           "any other installer window and look at it again.";
            return result;
        }
    }

    std::vector<Undo> undo;
    DWORD err = 0;
    bool overwrote = false;   // a file was replaced, which no undo can reverse
    std::wstring failedAt;

    bool failed = false;
    auto fail = [&](const std::wstring& what, DWORD code) {
        // Keyed off a flag, not off err: a Windows call can fail with a last
        // error of zero, and testing the code alone let the loop carry on and
        // report the whole run as a success.
        failed = true;
        err = code;
        failedAt = what;
    };

    for (const Step& step : plan.steps) {
        switch (step.action) {
            case Action::MakeDir: {
                if (!ensureDirTree(step.to, &undo)) {
                    fail(step.to, GetLastError());
                }
                break;
            }
            case Action::Backup: {
                const size_t slash = step.to.find_last_of(L"\\/");
                if (slash != std::wstring::npos) ensureDirTree(step.to.substr(0, slash), &undo);
                if (!fileExists(step.from)) {
                    if (step.required) fail(step.from, ERROR_FILE_NOT_FOUND);
                    break;  // an optional source that is not there is not an error
                }
                clearReadOnly(step.to);
                if (!CopyFileW(step.from.c_str(), step.to.c_str(), FALSE)) {
                    fail(step.to, GetLastError());
                } else {
                    // Deliberately no undo. A backup is the only way back from a
                    // step that overwrote something, and a rollback that tidies
                    // the backups away takes that with it. They cost a few
                    // megabytes and they are the whole safety net.
                    result.done.push_back("copied " + toUtf8(leafOf(step.from)) + " -> " +
                                          toUtf8(step.to));
                }
                break;
            }
            case Action::Rename: {
                if (!fileExists(step.from)) {
                    if (step.required) fail(step.from, ERROR_FILE_NOT_FOUND);
                    break;
                }
                clearReadOnly(step.from);
                clearReadOnly(step.to);
                if (!MoveFileExW(step.from.c_str(), step.to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                    fail(step.from, GetLastError());
                } else {
                    undo.push_back({Undo::Kind::MoveBack, step.to, step.from});
                    result.done.push_back("renamed " + toUtf8(leafOf(step.from)) + " -> " +
                                          toUtf8(leafOf(step.to)));
                }
                break;
            }
            case Action::WritePayload: {
                const void* data = nullptr;
                size_t size = 0;
                if (!payload || !payload(step.item, &data, &size) || !data || size == 0) {
                    fail(step.to, ERROR_RESOURCE_DATA_NOT_FOUND);
                    break;
                }
                const bool existed = fileExists(step.to);
                clearReadOnly(step.to);
                if (!writeWholeFile(step.to, data, size, &err)) {
                    fail(step.to, err);
                } else {
                    if (!existed)
                        undo.push_back({Undo::Kind::DeleteFile, step.to, std::wstring()});
                    else
                        overwrote = true;  // no undo for this: the old bytes are gone
                    result.done.push_back("wrote " + toUtf8(leafOf(step.to)));
                }
                break;
            }
            case Action::WriteText: {
                const bool existed = fileExists(step.to);
                clearReadOnly(step.to);
                if (!writeWholeFile(step.to, step.text.data(), step.text.size(), &err)) {
                    fail(step.to, err);
                } else {
                    if (!existed)
                        undo.push_back({Undo::Kind::DeleteFile, step.to, std::wstring()});
                    else
                        overwrote = true;
                    result.done.push_back("wrote " + toUtf8(leafOf(step.to)));
                }
                break;
            }
            case Action::Delete: {
                if (!fileExists(step.from)) break;
                clearReadOnly(step.from);
                if (!DeleteFileW(step.from.c_str())) {
                    fail(step.from, GetLastError());
                } else {
                    // No undo: the plan copies anything worth keeping into the
                    // backup folder before removing it.
                    result.done.push_back("removed " + toUtf8(leafOf(step.from)));
                }
                break;
            }
        }
        if (failed) break;
    }

    if (!failed) {
        result.ok = true;
        return result;
    }

    result.needsElevation = (err == ERROR_ACCESS_DENIED || err == ERROR_WRITE_PROTECT ||
                             err == ERROR_PRIVILEGE_NOT_HELD);
    result.error = "Could not finish: " + errorText(err) + " (at " + toUtf8(failedAt) + ")";

    // Walk it back. Anything that cannot be undone is left alone rather than
    // retried: the backup folder is the safety net, and a rollback that fights
    // the filesystem is how a bad situation becomes an unexplainable one.
    for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
        switch (it->kind) {
            case Undo::Kind::RemoveDir:
                RemoveDirectoryW(it->a.c_str());
                break;
            case Undo::Kind::DeleteFile:
                clearReadOnly(it->a);
                DeleteFileW(it->a.c_str());
                break;
            case Undo::Kind::MoveBack:
                clearReadOnly(it->a);
                MoveFileExW(it->a.c_str(), it->b.c_str(), MOVEFILE_REPLACE_EXISTING);
                break;
        }
    }
    // Whether "put back" is the truth depends on what the run had already
    // done: a rename can be reversed, a file that was written over cannot.
    // Claiming a full restore when a DLL was replaced is worse than saying
    // nothing, because it stops somebody looking in edvr_backup\.
    result.rolledBack = true;
    result.overwrote = overwrote;
    return result;
}

}  // namespace edvr::installer
