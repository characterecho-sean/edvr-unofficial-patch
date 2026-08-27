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
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace edvr::installer {

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
    bool hadBom = false;

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
    std::vector<std::string> kept;     // your value, carried into the new file
    std::vector<std::string> adopted;  // a default that changed, and you had not touched it
    std::vector<std::string> retired;  // your value for a setting this version dropped
    std::vector<std::string> carried;  // a key that was never ours, kept verbatim
    std::vector<std::string> removed;  // a line you had deleted, left commented out
    std::vector<std::string> forced;   // set by the installer (chaining)
    bool twoWay = false;               // no base copy: compared against the new defaults
};

// `base` is the shipped default of the version currently installed, or nullptr.
// `forced` is dotted key -> value, applied last and always winning.
std::string mergeIni(const std::string& next, const std::string& user, const std::string* base,
                     const std::vector<std::pair<std::string, std::string>>& forced,
                     MergeReport* report);

// Read one dotted key out of ini text, with the reader's own rules. Used to
// find out what advanced.real_dll currently says.
std::string iniValue(const std::string& text, const std::string& dotted,
                     const std::string& fallback = std::string());

}  // namespace edvr::installer
