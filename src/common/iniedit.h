// edvr.ini, updated without losing what the user changed.
//
// An update ships a NEW edvr.ini: new settings, new defaults, rewritten
// explanations. Copying it over the installed one throws away every value the
// user tuned in a headset; leaving the installed one in place means new
// settings arrive undocumented and a changed default never reaches anybody.
// Both failures are silent, and the second is worse -- the file still looks
// right.
//
// So: a three-way merge, against the shipped default of the version they
// currently have (the installer keeps a copy for exactly this). The output is
// the NEW file, line for line, with the user's own values written back into it:
//
//   they changed it        -> their value, in the new file's line
//   they never touched it  -> the new default, comments and all
//   they deleted the line  -> the new file's line, commented out
//   the setting is gone    -> carried to the end of its section, with a note
//   the installer must set it (chaining) -> forced, and said so in the report
//
// With no base copy -- a hand-installed rig meeting this installer for the
// first time -- it falls back to comparing against the NEW defaults, which
// keeps every value that differs. That over-preserves (a default that changed
// between versions reads as a user edit) and the report says so, out loud,
// rather than pretending it was a three-way merge.
//
// The parser here follows src/common/config.cpp exactly: sections, # and ;
// comments, inline comments that need whitespace in front, last value wins,
// and a UTF-8 BOM skipped. A merge that disagreed with the reader about which
// value is live would write a file whose settings are not the ones it reports.
//
// Lives in common, not the installer, since 2026-09-07: the in-headset
// settings menu (docs/settings-menu.md) writes edvr.ini with the same
// one-value merge the installer's settings window uses, and one copy of the
// grammar is the only way the two can never disagree about what a line
// says. Plain `edvr` namespace, so the installer's own namespace finds these
// names unqualified exactly as it did before the move.
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ini_name.h"

namespace edvr {

enum class LineKind {
    Blank,
    Comment,       // prose
    Section,       // [fix]
    Key,           // share_exposure = 1
    CommentedKey,  // #max_mb = 4   -- an expert default shown at its value
};

struct IniLine {
    LineKind    kind = LineKind::Blank;
    std::string text;     // the line, without its terminator
    std::string eol;      // "\r\n", "\n", or "" on a last line with no terminator
    std::string section;  // the section this line is in ("" before the first header)
    std::string key;      // for Key / CommentedKey
    std::string value;    // for Key / CommentedKey, trimmed, inline comment removed
};

struct IniDoc {
    std::vector<IniLine> lines;

    // Last match wins, as Config::parse does by assigning into a flat map.
    // Matching is case-insensitive: a key whose case does not match the shipped
    // spelling does nothing in the game today, so treating it as the same
    // setting fixes it rather than duplicating it.
    const IniLine* findKey(const std::string& section, const std::string& key) const;
    std::string    dominantEol() const;
    std::string    text() const;
};

IniDoc iniParse(const std::string& text);

// A dotted "section.key", as the log and the docs name settings.
std::string dottedName(const std::string& section, const std::string& key);

struct MergeReport {
    std::vector<std::string> followed;  // a value that followed its setting to a new key
    std::vector<std::string> kept;     // your value, carried into the new file
    std::vector<std::string> adopted;  // a default that changed, and you had not touched it
    std::vector<std::string> retired;  // your value for a setting this version dropped
    std::vector<std::string> carried;  // a key that was never ours, kept verbatim
    std::vector<std::string> removed;  // a line you had deleted, left commented out
    std::vector<std::string> forced;   // set by the installer (chaining)
    bool twoWay = false;               // no base copy: compared against the new defaults
};

// Settings move between sections as the project changes its mind about what is
// a fix and what is an instrument, and the section is part of the key -- so a
// move renames it. Without help, a user's tuned value is left behind under a
// name nothing reads any more.
//
// The new file says where a setting used to live, on its own line above the
// key, which is documentation for whoever is reading the ini as well as
// instructions for this merge:
//
//     # moved-from: fix.exposure_damping
//     #exposure_damping = 0
//
// `base` is the shipped default of the version currently installed, or nullptr.
// `forced` is dotted key -> value, applied last and always winning.
// `userFile` names the file `user` was read from, for the note written above a setting this
// version no longer has ("# carried over from your <userFile>; ..."): edvr.ini unless the
// caller is merging another file, as the flat edition does with its own edvr-flat.ini.
std::string mergeIni(const std::string& next, const std::string& user, const std::string* base,
                     const std::vector<std::pair<std::string, std::string>>& forced,
                     MergeReport* report, const char* userFile = kIniNameVr);

// Read one dotted key out of ini text, with the reader's own rules. Used to
// find out what advanced.real_dll currently says.
std::string iniValue(const std::string& text, const std::string& dotted,
                     const std::string& fallback = std::string());

// ---------------------------------------------------------------------------
// Writing a file back, whole (2026-09-29)
//
// Two programs write the live edvr.ini -- the installer's settings window and
// the in-headset menu -- and both refresh the copy the installer keeps outside
// the game folder. Each carried its own temp-and-rename, and each had a
// different half of it: one never flushed, the other never retried, and a reader
// holding the file open at the wrong instant made the classic rename fail with a
// refusal that the menu showed as a failed write. One writer, here, beside the
// grammar both of them share.
//
// The game re-reads edvr.ini about once a second and Config::parse refuses a
// file it cannot read whole, so a save that truncates first (or that dies half
// way) is seen by the very next poll. Every write here is a complete new file,
// beside the old one, and then one replace: the target is the old bytes or all
// of the new ones, never a mixture.

struct AtomicWriteOptions {
    // Tries after the first one, spent only on a sharing violation, an access
    // denial or a lock violation from the replace: a reader that has the target
    // open without sharing DELETE, an editor mid-save, an antivirus scanning the
    // file that was just written. Each clears in milliseconds. Anything else (no
    // such folder, a full disk) fails at once. A reader that DOES share DELETE
    // does not need this: see the replace, below.
    int      retries = 5;
    unsigned backoffMs = 20;  // slept before each of those tries
};

// Replaces the file at `path` with `bytes`, all or nothing.
//
//   1. the bytes go to <path>.edvr-tmp-<pid>-<tid> in the same folder (one
//      volume, so the replace is a rename) and are flushed to disk. The flush
//      is before the replace, and it is where the durability lives: on one
//      volume MOVEFILE_WRITE_THROUGH adds little to a rename;
//   2. the temp file is renamed over the target with POSIX semantics
//      (SetFileInformationByHandle with FileRenameInfoEx and the flags
//      FILE_RENAME_FLAG_REPLACE_IF_EXISTS and FILE_RENAME_FLAG_POSIX_SEMANTICS;
//      Windows 10 1607+, NTFS), through a handle opened write-through, which is
//      what MOVEFILE_WRITE_THROUGH asks of the classic one. The target's name
//      moves to the new file at once, and a reader that has the old one open and
//      shares DELETE goes on reading it: the replace goes through under such a
//      reader on the first attempt;
//   3. where the OS or the volume refuses that as unsupported (older Windows,
//      FAT and exFAT, some network shares: ERROR_INVALID_PARAMETER,
//      ERROR_NOT_SUPPORTED, ERROR_INVALID_FUNCTION and their kind) it falls back
//      to the classic MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH) in the same
//      attempt, and remembers the refusal for the process so it is not asked
//      again on every write. A failure it does not recognise falls back for that
//      attempt only. A read-only target goes straight to the classic rename, so
//      it is refused exactly as it always was;
//   4. a replace refused for a reason that passes -- a sharing violation, an
//      access denial, a lock violation -- is tried again as `options` says;
//   5. on any failure the temp file is deleted and the target is untouched.
//
// Why 2: the classic rename is refused with ERROR_ACCESS_DENIED while ANY other
// handle to the target is open, one that shares DELETE included (measured on
// Windows 11 build 26200 with cmd's move /Y), so before it only the retry got a
// menu write past Config's read of the file. A reader that does NOT share DELETE
// still refuses both kinds of rename, and the retry waits for it.
//
// Does not create folders and does not clear a read-only attribute: a read-only
// edvr.ini is a decision somebody made, and the failure says so. `error` (which
// may be null) gets a sentence for a person on failure, ASCII, without the
// file's name. `tries` (which may be null) gets the number of replace attempts
// made -- 0 when the temp file could not be written, which is what lets a test
// tell "retried and gave up" from "never got as far as trying".
bool writeFileAtomic(const std::wstring& path, const std::string& bytes,
                     std::wstring* error = nullptr,
                     const AtomicWriteOptions& options = AtomicWriteOptions(),
                     int* tries = nullptr);

// Steps 2 to 4 alone, for a caller that stages its own file (2026-09-29): puts
// the file at `from` in place of the one at `to`, or at `to` where there is none.
// The installer's apply engine writes DLLs and renames one over another, and it
// had the classic rename for both, which is refused with "access denied" for as
// long as ANY handle to the target is open. A real-time scanner or the search
// indexer looking at a file that was just read is enough, and the run failed and
// rolled back over a hold that was gone a few milliseconds later.
//
// It renames; it neither writes nor flushes, which whoever staged `from` has done.
// On failure `from` is where it was and the target is untouched, and deleting
// `from` is the caller's. `options` is the writer's, `tries` (may be null) gets
// the attempts made, `code` (may be null) the Windows error of the last one on
// failure and ERROR_SUCCESS on success. Read-only targets, refusals as
// unsupported and the seam below are all as for writeFileAtomic.
bool replaceFileAtomic(const std::wstring& from, const std::wstring& to,
                       const AtomicWriteOptions& options = AtomicWriteOptions(),
                       int* tries = nullptr, unsigned long* code = nullptr);

// The rig's seam into steps 2 and 3; nothing in the product calls these.
//
// A hook STANDS IN for the rename it is named for. It is called, with the staged
// file's path and the target's, INSTEAD of the operating system, and returns the
// Windows error that rename would have set, or 0 for "the file is in place" --
// the hook having put it there itself, or, in a test that only counts, not
// having. What it returns is read exactly as the real call's error is: from the
// POSIX-semantics hook ERROR_INVALID_PARAMETER (and its kind) is a refusal as
// unsupported, a sharing violation, an access denial or a lock violation passes
// and is tried again, and anything else is neither. A null hook is the real
// call. Calling replaceHooksForTest at all -- with both hooks null too --
// forgets a remembered refusal and zeroes both counts.
//
// It exists because the real file system takes part in a count otherwise. The
// real-time scanner and the search indexer both have every file that was just
// written open for a few milliseconds, and the classic rename is refused with a
// real "access denied" for as long as ANY handle to its target is open. The
// writer then tries again, as it should, and a rig that asserted "two writes, two
// renames" counted three (2026-09-29). Standing in for the renames takes the file
// system out of the count and lets a test script the answers instead: "busy
// once, then fine".
typedef unsigned long (*ReplaceHook)(const wchar_t* from, const wchar_t* to);
void replaceHooksForTest(ReplaceHook posix, ReplaceHook classic);

// How many attempts at each rename have been made, hooked or not, since
// replaceHooksForTest. An attempt at the POSIX-semantics rename that could not
// even open the temp file counts. After a refusal as unsupported that is
// remembered the POSIX side stays where it was, however many writes follow.
int posixReplaceAttempts();
int classicReplaceAttempts();

// Whether a refusal as unsupported has been remembered, so that later writes go
// straight to the classic rename.
bool posixReplaceRefused();

// The whole file, read with FILE_SHARE_DELETE so that reading it does not stop
// somebody replacing, renaming or deleting it, and closed before this returns:
// the hold lasts as long as the read. false when the file is missing,
// unreadable, over `limit`, or read short; an empty file is true with empty
// `bytes`.
bool readFileBytes(const std::wstring& path, std::string* bytes, size_t limit = 64u << 20);

// ---------------------------------------------------------------------------
// The mirror's generations
//
// %LOCALAPPDATA%\EDVR\<leaf>-<store>\ holds the only copy of somebody's settings
// that a game update cannot reach. It used to be one copy, overwritten in place
// by every install. A restore that failed, or that the person declined, was
// followed by a fresh install whose defaults then overwrote the saved settings:
// the safety net erased by the thing it was there to survive.
//
// Now <name> is the newest copy and <name>.1 and <name>.2 are the two before it.
// Three, not one, because the state worth keeping is the one BEFORE an install
// replaced it.
constexpr int kMirrorGenerations = 3;

// <dir>\<name> for generation 0, <dir>\<name>.<n> for the older ones.
std::wstring generationPath(const std::wstring& dir, const std::wstring& name, int generation);

// The newest generation that exists and is not empty, as a path, or empty when
// none is. "Not empty" because a restore of a zero-byte file is worse than a
// restore of the copy behind it, and a copy an older build left half-written is
// how one gets there.
std::wstring newestGeneration(const std::wstring& dir, const std::wstring& name);

// Writes `bytes` as the newest generation of <dir>\<name>.
//
// The bytes identical to the newest copy: nothing to do, nothing rotates -- an
// installer run that changed nothing must not age the real history out.
//
// `rotate` true: the copy being replaced is kept as .1 (and .1 as .2, the
// oldest dropped). The new copy is written and flushed to a temp file FIRST and
// only when that has landed does anything older move: a write that cannot be
// staged, or whose first move is refused, leaves every generation as it was, and
// one that fails later loses at most the oldest copy, never the newest. The
// newest is never absent at any instant: the old copy is written to .1 from the
// bytes read, not renamed there.
//
// `rotate` false: the newest is replaced in place and the older ones are left
// alone. That is the rule for a change made a moment ago (one slider, one
// toggle): a generation per tweak would push the copy worth keeping out within
// three of them.
//
// `dir` must exist. Returns true when the newest generation holds `bytes`.
bool writeGenerations(const std::wstring& dir, const std::wstring& name,
                      const std::string& bytes, bool rotate, std::wstring* error = nullptr);

}  // namespace edvr
