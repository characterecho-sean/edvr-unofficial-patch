#include "iniedit.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <set>
#include <vector>

namespace edvr {
namespace {

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return std::string();
    const size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

std::string lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return out;
}

// The reader's rule, copied deliberately: an inline comment is a ; or # with
// whitespace in front of it. Without the whitespace requirement a value like
// a Windows path or a colour could not contain one.
std::string stripInlineComment(const std::string& value) {
    for (size_t i = 1; i < value.size(); ++i) {
        if ((value[i] == ';' || value[i] == '#') && (value[i - 1] == ' ' || value[i - 1] == '\t')) {
            return value.substr(0, i);
        }
    }
    return value;
}

// A key token is what config.cpp would accept as a name: no spaces, no
// punctuation that would make it unaddressable. This is what keeps prose from
// being read as a setting -- the shipped file is full of comment lines like
// "# 0.3, paired with panel_distance = 0.7, is a comfortable starting point",
// and a looser test files that as a commented-out key named
// "0.3, paired with panel_distance".
bool isKeyToken(const std::string& s) {
    if (s.empty() || s.size() > 96) return false;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        // Anything config.cpp would accept: it takes whatever is left of the
        // '=' after trimming, so a key with an accent in it is a key as far as
        // the game is concerned. Refusing it here meant the line was filed as
        // prose and silently dropped from the merged file -- a setting the game
        // was reading, gone without a word in the report.
        //
        // Whitespace is still refused, and that is the point of this test: it
        // is what keeps a comment line like "0.3, paired with panel_distance =
        // 0.7, is comfortable" from being read as a setting.
        if (isspace(u) || c == '#' || c == ';' || c == '[' || c == ']') return false;
    }
    return true;
}

bool splitKeyValue(const std::string& body, std::string* key, std::string* value) {
    const size_t eq = body.find('=');
    if (eq == std::string::npos) return false;
    const std::string k = trim(body.substr(0, eq));
    if (!isKeyToken(k)) return false;
    *key = k;
    *value = trim(stripInlineComment(body.substr(eq + 1)));
    return true;
}

bool sameKey(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

// Rewrite one line to hold a new value, keeping everything around it: the
// indentation, the key as it is spelled in the shipped file, the spacing
// around '=', and any inline comment. The alternative -- emitting
// "key = value" fresh -- would quietly reformat every line the user has ever
// touched, and turn a diff of their ini against the shipped one into noise.
std::string rewriteValue(const std::string& line, const std::string& newValue, bool uncomment) {
    std::string work = line;
    std::string indent;

    const size_t firstNonSpace = work.find_first_not_of(" \t");
    if (firstNonSpace != std::string::npos) {
        indent = work.substr(0, firstNonSpace);
        work = work.substr(firstNonSpace);
    }
    if (uncomment) {
        size_t i = 0;
        while (i < work.size() && (work[i] == '#' || work[i] == ';')) ++i;
        while (i < work.size() && (work[i] == ' ' || work[i] == '\t')) ++i;
        work = work.substr(i);
    }

    const size_t eq = work.find('=');
    if (eq == std::string::npos) return indent + work;  // not a key line after all

    std::string head = work.substr(0, eq + 1);
    std::string tail = work.substr(eq + 1);

    // Keep one space after '=' if the file uses one, and keep any inline
    // comment that followed the old value.
    std::string spacing;
    size_t v = 0;
    while (v < tail.size() && (tail[v] == ' ' || tail[v] == '\t')) {
        spacing += tail[v];
        ++v;
    }
    if (spacing.empty()) spacing = " ";

    const std::string rest = tail.substr(v);
    // stripInlineComment starts at index 1, since a value may legitimately
    // begin with # or ;. On a line whose value is EMPTY the comment is the
    // whole remainder, starting at index 0, so it has to be spotted here or
    // rewriting that line throws the comment away.
    const std::string stripped =
        (!rest.empty() && (rest[0] == '#' || rest[0] == ';')) ? std::string()
                                                             : stripInlineComment(rest);
    std::string comment;
    if (stripped.size() < rest.size()) comment = rest.substr(stripped.size());

    std::string out = indent + head + spacing + newValue;
    if (!comment.empty()) {
        // stripInlineComment cut at the ; or #; the whitespace before it is the
        // last of `stripped`. Re-space it to a single space so a shortened
        // value does not leave a ragged gap.
        const size_t lastNonSpace = out.find_last_not_of(" \t");
        out = out.substr(0, lastNonSpace + 1) + " " + trim(comment);
    }
    return out;
}

std::string commentOut(const std::string& line) {
    const size_t firstNonSpace = line.find_first_not_of(" \t");
    if (firstNonSpace == std::string::npos) return line;
    return line.substr(0, firstNonSpace) + "#" + line.substr(firstNonSpace);
}

struct Effective {
    bool        present = false;  // an uncommented key line exists
    std::string value;
    bool        known = false;  // the key appears at all, commented or not
};

Effective effectiveOf(const IniDoc& doc, const std::string& section, const std::string& key) {
    Effective e;
    for (const IniLine& l : doc.lines) {
        if (l.kind != LineKind::Key && l.kind != LineKind::CommentedKey) continue;
        if (!sameKey(l.section, section) || !sameKey(l.key, key)) continue;
        e.known = true;
        if (l.kind == LineKind::Key) {
            // Last LIVE line wins, exactly as config.cpp's map assignment does.
            e.present = true;
            e.value = l.value;
        }
        // A commented line is not a value. config.cpp skips any line starting
        // with # or ; before it looks for a key, so a commented copy below a
        // live one changes nothing about what the game reads -- and letting it
        // clear the value here dropped the user's setting on the floor:
        //     black_void = 0
        //     #black_void = 1
        // read as "they deleted black_void", and the merge commented the live
        // line out. It is only evidence that the KEY exists, which is what
        // `known` is for.
    }
    return e;
}

}  // namespace

namespace {

// "# moved-from: fix.exposure_damping" above a key, in the NEW file: where this
// setting used to live. Read as a map from the old dotted name to the new one.
// Annotations STACK: several old names may precede one key, which is how two
// settings merge into a single new one (fss_eye_heal + fss_reveal_sync ->
// fss_eye_sync); each old name maps to the same target, and when more than one
// old value is present in a user's file the last one in file order lands (they
// agree in every configuration that was ever a default).
struct MovedTarget {
    std::string section;
    std::string key;
    std::string oldDefault;   // the OLD key's shipped default, or empty: a
                              // user line still carrying it is stale, not a
                              // choice, and must not override the new
                              // default when it migrates
};

// "# retired-default: early" above a key, in the NEW file: a value this key
// used to SHIP as its default and no longer does.
//
// moved-from's "(default X)" covers a key that went away. This covers the
// other half: a key that stayed put while its shipped default changed, which
// the merge could not otherwise tell from a deliberate choice -- every
// install of the old version has the old default written out as a literal
// line, so "the user's value differs from what we ship now" is true for all
// of them.
//
// It exists because fix.vr_handover shipped "early" in 0.12.1 and 0.12.2 and
// then had to be withdrawn: it crashed the game on runtimes neither of this
// project's headsets could test. Flipping the default in the source reached
// nobody, because every one of those installs carries the line.
//
// The cost is real and deliberate: somebody who chose the retired value ON
// PURPOSE loses that choice once, and is told so in the report. For a value
// withdrawn because it crashed, that is the right way round.
//
// Annotations STACK, like moved-from: a key may retire more than one former
// default over its life.
std::map<std::string, std::set<std::string>> retiredDefaults(const IniDoc& doc) {
    std::map<std::string, std::set<std::string>> out;
    std::vector<std::string> pending;
    for (const IniLine& line : doc.lines) {
        if (line.kind == LineKind::Comment) {
            const std::string body = trim(line.text);
            size_t at = 0;
            while (at < body.size() && (body[at] == '#' || body[at] == ';')) ++at;
            const std::string text = trim(body.substr(at));
            if (lower(text).rfind("retired-default:", 0) == 0) {
                pending.push_back(
                    trim(text.substr(strlen("retired-default:"))));
            }
            continue;
        }
        if (line.kind == LineKind::Blank || line.kind == LineKind::Section) {
            pending.clear();
            continue;
        }
        if (line.kind == LineKind::Key || line.kind == LineKind::CommentedKey) {
            for (const std::string& v : pending) {
                out[lower(dottedName(line.section, line.key))].insert(lower(v));
            }
        }
        pending.clear();
    }
    return out;
}

std::map<std::string, MovedTarget> movedKeys(const IniDoc& doc) {
    std::map<std::string, MovedTarget> out;
    std::vector<std::pair<std::string, std::string>> pending;  // old, default
    for (const IniLine& line : doc.lines) {
        if (line.kind == LineKind::Comment) {
            const std::string body = trim(line.text);
            size_t at = 0;
            while (at < body.size() && (body[at] == '#' || body[at] == ';')) ++at;
            const std::string text = trim(body.substr(at));
            if (lower(text).rfind("moved-from:", 0) == 0) {
                std::string spec = trim(text.substr(strlen("moved-from:")));
                std::string oldDefault;
                const size_t open = spec.find("(default");
                if (open != std::string::npos) {
                    const size_t close = spec.find(')', open);
                    if (close != std::string::npos) {
                        oldDefault =
                            trim(spec.substr(open + strlen("(default"),
                                             close - open - strlen("(default")));
                        spec = trim(spec.substr(0, open));
                    }
                }
                pending.emplace_back(spec, oldDefault);
            }
            continue;
        }
        if (line.kind == LineKind::Blank || line.kind == LineKind::Section) {
            pending.clear();
            continue;
        }
        for (const auto& p : pending) {
            out[lower(p.first)] = {line.section, line.key, p.second};
        }
        pending.clear();
    }
    return out;
}

}  // namespace

std::string dottedName(const std::string& section, const std::string& key) {
    return section.empty() ? key : section + "." + key;
}

const IniLine* IniDoc::findKey(const std::string& section, const std::string& key) const {
    const IniLine* found = nullptr;
    for (const IniLine& l : lines) {
        if (l.kind != LineKind::Key) continue;
        if (sameKey(l.section, section) && sameKey(l.key, key)) found = &l;
    }
    return found;
}

std::string IniDoc::dominantEol() const {
    for (const IniLine& l : lines) {
        if (!l.eol.empty()) return l.eol;
    }
    return "\r\n";  // the shipped file is CRLF, and this file is edited on Windows
}

std::string IniDoc::text() const {
    std::string out;
    for (const IniLine& l : lines) {
        out += l.text;
        out += l.eol;
    }
    return out;
}

IniDoc iniParse(const std::string& text) {
    IniDoc doc;
    size_t p = 0;

    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        p = 3;
    }

    std::string section;
    while (p <= text.size()) {
        if (p == text.size()) break;
        size_t lineEnd = p;
        while (lineEnd < text.size() && text[lineEnd] != '\n' && text[lineEnd] != '\r') ++lineEnd;

        IniLine line;
        line.text = text.substr(p, lineEnd - p);
        size_t after = lineEnd;
        if (after < text.size() && text[after] == '\r') ++after;
        if (after < text.size() && text[after] == '\n') ++after;
        line.eol = text.substr(lineEnd, after - lineEnd);
        p = after;

        const std::string body = trim(line.text);
        if (body.empty()) {
            line.kind = LineKind::Blank;
        } else if (body[0] == '[') {
            const size_t close = body.find(']');
            if (close != std::string::npos) {
                section = body.substr(1, close - 1);
                line.kind = LineKind::Section;
            } else {
                line.kind = LineKind::Comment;
            }
        } else if (body[0] == '#' || body[0] == ';') {
            size_t i = 0;
            while (i < body.size() && (body[i] == '#' || body[i] == ';')) ++i;
            std::string key, value;
            if (splitKeyValue(trim(body.substr(i)), &key, &value)) {
                line.kind = LineKind::CommentedKey;
                line.key = key;
                line.value = value;
            } else {
                line.kind = LineKind::Comment;
            }
        } else {
            std::string key, value;
            if (splitKeyValue(body, &key, &value)) {
                line.kind = LineKind::Key;
                line.key = key;
                line.value = value;
            } else {
                line.kind = LineKind::Comment;  // junk: kept, never interpreted
            }
        }
        line.section = section;
        doc.lines.push_back(line);
    }
    return doc;
}

std::string iniValue(const std::string& text, const std::string& dotted,
                     const std::string& fallback) {
    const size_t dot = dotted.find('.');
    const std::string section = dot == std::string::npos ? std::string() : dotted.substr(0, dot);
    const std::string key = dot == std::string::npos ? dotted : dotted.substr(dot + 1);
    const IniDoc doc = iniParse(text);
    const Effective e = effectiveOf(doc, section, key);
    return e.present ? e.value : fallback;
}

std::string mergeIni(const std::string& next, const std::string& user, const std::string* base,
                     const std::vector<std::pair<std::string, std::string>>& forced,
                     MergeReport* report) {
    MergeReport local;
    MergeReport& rep = report ? *report : local;

    IniDoc nextDoc = iniParse(next);
    const std::map<std::string, MovedTarget> moved = movedKeys(nextDoc);
    const std::map<std::string, std::set<std::string>> retired =
        retiredDefaults(nextDoc);
    const IniDoc userDoc = iniParse(user);
    const IniDoc baseDoc = iniParse(base ? *base : next);
    rep.twoWay = (base == nullptr);

    const std::string eol = nextDoc.dominantEol();

    // Forced keys are the installer's own business (the chain target, above
    // all). They are applied last and are never treated as a user edit, so a
    // user value for one of them is not reported as kept when it is about to be
    // overwritten.
    std::set<std::string> forcedKeys;
    for (const auto& f : forced) forcedKeys.insert(lower(f.first));

    // ---- every setting the user's file has an opinion about -------------
    //
    // No file means no opinions. Not "every setting deleted": inferring
    // deletion from absence, with nothing to be absent FROM, commented out
    // every line of the shipped ini on a first install -- a settings file in
    // which nothing at all was set, which the game reads perfectly happily and
    // which looks, in an editor, almost right.
    std::vector<std::pair<std::string, std::string>> userKeys;  // section, key, in file order
    std::set<std::string> seen;
    bool haveUser = false;
    for (const IniLine& l : userDoc.lines) {
        if (l.kind != LineKind::Key && l.kind != LineKind::CommentedKey) continue;
        haveUser = true;
        const std::string id = lower(dottedName(l.section, l.key));
        if (seen.insert(id).second) userKeys.emplace_back(l.section, l.key);
    }
    // Keys the version they have shipped live, which their file no longer has
    // at all: a deleted line. Without this pass a deletion looks like "no
    // opinion" and the new file quietly restores the line.
    //
    // Only with a real base to compare against. In two-way mode the base IS the
    // new file, so every setting added since the version they hand-installed
    // would read as one they had deleted -- and arrive commented out, which is
    // the opposite of shipping a new default.
    if (haveUser && base != nullptr) {
        for (const IniLine& l : baseDoc.lines) {
            if (l.kind != LineKind::Key) continue;
            const std::string id = lower(dottedName(l.section, l.key));
            if (seen.insert(id).second) userKeys.emplace_back(l.section, l.key);
        }
    }

    // line index -> replacement, and section -> lines to append at its end.
    std::map<size_t, std::string> replacements;
    std::map<std::string, std::vector<std::string>> appended;  // lowercased section

    auto lastLineOfSection = [&](const std::string& section) -> size_t {
        size_t last = std::string::npos;
        for (size_t i = 0; i < nextDoc.lines.size(); ++i) {
            if (sameKey(nextDoc.lines[i].section, section)) last = i;
        }
        return last;
    };

    for (const auto& sk : userKeys) {
        const std::string& section = sk.first;
        const std::string& key = sk.second;
        const std::string dotted = dottedName(section, key);
        if (forcedKeys.count(lower(dotted))) continue;

        const Effective u = effectiveOf(userDoc, section, key);
        const Effective b = effectiveOf(baseDoc, section, key);
        const Effective n = effectiveOf(nextDoc, section, key);

        const bool changedByUser = (u.present != b.present) || (u.present && u.value != b.value);

        if (!n.known) {
            // The setting may have MOVED rather than gone: the new file says so
            // above its new key, and the value follows it there instead of
            // being stranded under a name nothing reads.
            const auto move = moved.find(lower(dotted));
            if (u.present && move != moved.end() &&
                !move->second.oldDefault.empty() &&
                u.value == move->second.oldDefault) {
                // Their line carries the OLD key's shipped default -- an
                // un-updated file, not a choice. The new line stands as
                // shipped and the stale line is consumed.
                rep.adopted.push_back(dotted + " = " + u.value +
                                      "  (retired default; " +
                                      dottedName(move->second.section,
                                                 move->second.key) +
                                      " ships its own)");
                continue;
            }
            if (u.present && move != moved.end()) {
                const std::string& newSection = move->second.section;
                const std::string& newKey = move->second.key;
                const Effective already = effectiveOf(userDoc, newSection, newKey);
                if (!already.present) {  // an explicit new-key value wins
                    size_t target = std::string::npos;
                    for (size_t i = 0; i < nextDoc.lines.size(); ++i) {
                        const IniLine& l = nextDoc.lines[i];
                        if ((l.kind == LineKind::Key || l.kind == LineKind::CommentedKey) &&
                            sameKey(l.section, newSection) && sameKey(l.key, newKey)) {
                            target = i;
                        }
                    }
                    if (target != std::string::npos) {
                        replacements[target] =
                            rewriteValue(nextDoc.lines[target].text, u.value,
                                         nextDoc.lines[target].kind == LineKind::CommentedKey);
                        rep.followed.push_back(dotted + " = " + u.value + "  ->  " +
                                               dottedName(newSection, newKey));
                        continue;
                    }
                }
            }

            // This version has no such setting. Their value is kept, in its
            // section, with a note -- silently dropping a line somebody put
            // there is how a support thread starts.
            if (u.present) {
                const std::string note = b.known
                                             ? "# carried over from your edvr.ini; this version no "
                                               "longer uses it"
                                             : "# carried over from your edvr.ini; not an EDVR "
                                               "setting this version knows";
                appended[lower(section)].push_back(note);
                appended[lower(section)].push_back(key + " = " + u.value);
                (b.known ? rep.retired : rep.carried).push_back(dotted + " = " + u.value);
            }
            continue;
        }

        // A value this key used to ship as its default is not a choice.
        //
        // It has to be tested HERE, in the known-key path, and before
        // changedByUser is honoured: every install of the old version has
        // the old default written out as a literal line, so it looks
        // exactly like somebody typing it on purpose. moved-from cannot
        // reach this case -- its retired-default arm lives under !n.known,
        // and this key never went anywhere.
        if (u.present) {
            const auto r = retired.find(lower(dotted));
            if (r != retired.end() && r->second.count(lower(u.value))) {
                rep.adopted.push_back(
                    dotted + " = " + u.value +
                    "  (a default this version retired; it now ships " +
                    n.value + " -- set it again if you meant it)");
                continue;
            }
        }

        if (changedByUser) {
            if (u.present) {
                // Their value goes into the new file's line for this key --
                // the last one, which is the one the reader would use.
                size_t target = std::string::npos;
                for (size_t i = 0; i < nextDoc.lines.size(); ++i) {
                    const IniLine& l = nextDoc.lines[i];
                    if ((l.kind == LineKind::Key || l.kind == LineKind::CommentedKey) &&
                        sameKey(l.section, section) && sameKey(l.key, key)) {
                        target = i;
                    }
                }
                if (target != std::string::npos) {
                    replacements[target] =
                        rewriteValue(nextDoc.lines[target].text, u.value,
                                     nextDoc.lines[target].kind == LineKind::CommentedKey);
                    rep.kept.push_back(dotted + " = " + u.value);
                }
            } else {
                // They deleted or commented out a line the version they have
                // ships live. Restoring it would undo a deliberate edit, so the
                // new file carries the line commented out -- documented, and
                // off, which is what they chose.
                size_t target = std::string::npos;
                for (size_t i = 0; i < nextDoc.lines.size(); ++i) {
                    const IniLine& l = nextDoc.lines[i];
                    if (l.kind == LineKind::Key && sameKey(l.section, section) &&
                        sameKey(l.key, key)) {
                        target = i;
                    }
                }
                if (target != std::string::npos) {
                    replacements[target] = commentOut(nextDoc.lines[target].text);
                    rep.removed.push_back(dotted);
                }
            }
            continue;
        }

        // Untouched by the user. The new file's line stands as shipped; say so
        // when that means a default actually moved.
        if (b.known && (n.present != b.present || n.value != b.value)) {
            rep.adopted.push_back(dotted + " = " + (n.present ? n.value : "(default)"));
        }
    }

    // ---- values the installer itself must set ---------------------------
    for (const auto& f : forced) {
        const std::string dotted = f.first;
        const size_t dot = dotted.find('.');
        const std::string section = dot == std::string::npos ? std::string() : dotted.substr(0, dot);
        const std::string key = dot == std::string::npos ? dotted : dotted.substr(dot + 1);

        size_t target = std::string::npos;
        for (size_t i = 0; i < nextDoc.lines.size(); ++i) {
            const IniLine& l = nextDoc.lines[i];
            if ((l.kind == LineKind::Key || l.kind == LineKind::CommentedKey) &&
                sameKey(l.section, section) && sameKey(l.key, key)) {
                target = i;
            }
        }
        if (target != std::string::npos) {
            replacements[target] = rewriteValue(nextDoc.lines[target].text, f.second,
                                                nextDoc.lines[target].kind == LineKind::CommentedKey);
        } else {
            appended[lower(section)].push_back(key + " = " + f.second);
        }
        rep.forced.push_back(dotted + " = " + f.second);
    }

    // ---- emit ------------------------------------------------------------
    std::vector<IniLine> out;
    out.reserve(nextDoc.lines.size() + 8);

    // Which line each section's appended block goes after.
    std::map<size_t, std::string> appendAfter;  // line index -> section (lowercased)
    std::vector<std::string> orphanSections;    // sections the new file does not have at all
    for (const auto& kv : appended) {
        const size_t last = lastLineOfSection(kv.first);
        if (last == std::string::npos) {
            orphanSections.push_back(kv.first);
        } else {
            appendAfter[last] = kv.first;
        }
    }

    for (size_t i = 0; i < nextDoc.lines.size(); ++i) {
        IniLine line = nextDoc.lines[i];
        const auto r = replacements.find(i);
        if (r != replacements.end()) {
            line.text = r->second;
            if (line.eol.empty()) line.eol = eol;
        }
        out.push_back(line);

        const auto a = appendAfter.find(i);
        if (a != appendAfter.end()) {
            // A section's last line is usually blank; put the block before that
            // blank line rather than after it, so it stays inside the section.
            std::vector<IniLine> block;
            for (const std::string& s : appended[a->second]) {
                IniLine l;
                l.kind = LineKind::Comment;
                l.text = s;
                l.eol = eol;
                block.push_back(l);
            }
            size_t insertAt = out.size();
            while (insertAt > 0 && out[insertAt - 1].kind == LineKind::Blank) --insertAt;
            out.insert(out.begin() + insertAt, block.begin(), block.end());
        }
    }

    for (const std::string& section : orphanSections) {
        IniLine header;
        header.kind = LineKind::Section;
        header.text = "[" + section + "]";
        header.eol = eol;
        IniLine blank;
        blank.kind = LineKind::Blank;
        blank.eol = eol;
        out.push_back(blank);
        out.push_back(header);
        for (const std::string& s : appended[section]) {
            IniLine l;
            l.kind = LineKind::Comment;
            l.text = s;
            l.eol = eol;
            out.push_back(l);
        }
    }

    IniDoc result;
    result.lines = out;
    return result.text();
}

// ---------------------------------------------------------------------------
// Writing a file back, whole. What each of these promises is in iniedit.h.

namespace {

void setError(std::wstring* error, const std::wstring& text) {
    if (error) *error = text;
}

std::wstring withCode(const wchar_t* what, DWORD code) {
    return std::wstring(what) + L" (Windows error " + std::to_wstring(code) + L")";
}

// A replace that failed for a reason that passes in milliseconds; see
// AtomicWriteOptions.
bool passesInMilliseconds(DWORD code) {
    return code == ERROR_SHARING_VIOLATION || code == ERROR_ACCESS_DENIED ||
           code == ERROR_LOCK_VIOLATION;
}

bool pathExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Beside the target, so the replace is a rename inside one volume; named for the
// process and the thread, so two writers -- the menu inside the game and the
// settings window in the installer, or two threads of one of them -- never
// share a temp file. A temp file that outlives a crash is one per writer, and
// only if the process died in the few milliseconds between the write and the
// replace.
std::wstring tempNameFor(const std::wstring& path) {
    return path + L".edvr-tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(GetCurrentThreadId());
}

// The bytes, complete and flushed, in a file of their own. Nothing that already
// exists is touched, and on failure the file is gone again.
bool stageFile(const std::wstring& path, const std::string& bytes, std::wstring* temp,
               std::wstring* error) {
    *temp = tempNameFor(path);
    HANDLE f = CreateFileW(temp->c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        setError(error, withCode(L"the temporary file could not be created beside it", code));
        return false;
    }

    const wchar_t* failed = nullptr;
    DWORD code = 0;
    for (size_t done = 0; done < bytes.size();) {
        const size_t left = bytes.size() - done;
        const DWORD chunk = static_cast<DWORD>(left > (1u << 20) ? (1u << 20) : left);
        DWORD wrote = 0;
        if (!WriteFile(f, bytes.data() + done, chunk, &wrote, nullptr) || wrote == 0) {
            code = GetLastError();
            if (code == ERROR_SUCCESS) code = ERROR_WRITE_FAULT;  // a short write is a failure
            failed = L"the temporary file could not be written";
            break;
        }
        done += wrote;
    }
    // Flushed before the replace, not after: the replace is what makes these
    // bytes THE file, and a power cut between the two must find either the old
    // file or a complete new one, not a new name over data still in the cache.
    if (!failed && !FlushFileBuffers(f)) {
        code = GetLastError();
        failed = L"the temporary file could not be flushed to disk";
    }
    CloseHandle(f);
    if (failed) {
        DeleteFileW(temp->c_str());
        setError(error, withCode(failed, code));
        return false;
    }
    return true;
}

// --- the POSIX-semantics replace ---------------------------------------------
//
// Nothing here comes from winbase.h. It gives FILE_RENAME_INFO's flags member and
// the FILE_RENAME_FLAG_* values only under SDK-version tests -- spelled with a
// macro, _WIN32_WINNT_WIN10_RS1, that the 10.0.26100.0 headers never define, so
// they read as true today by accident -- and FileRenameInfoEx only under
// NTDDI_VERSION. The layout and the values are the operating system's own and do
// not move, so they are written out here, and no SDK setting can take them away.

// FILE_RENAME_INFO as FileRenameInfoEx reads it: a DWORD of flags comes first.
struct RenameInfoEx {
    DWORD  flags;
    HANDLE rootDirectory;   // null: fileName is a full path
    DWORD  fileNameLength;  // in bytes, not counting a terminator
    WCHAR  fileName[1];
};
const DWORD kRenameReplaceIfExists = 0x00000001;  // FILE_RENAME_FLAG_REPLACE_IF_EXISTS
const DWORD kRenamePosixSemantics = 0x00000002;   // FILE_RENAME_FLAG_POSIX_SEMANTICS
// FILE_INFO_BY_HANDLE_CLASS::FileRenameInfoEx, counting from FileBasicInfo = 0
// (minwinbase.h, Windows SDK 10.0.26100.0).
const int kFileRenameInfoEx = 22;
#if defined(NTDDI_WIN10_RS1) && (NTDDI_VERSION >= NTDDI_WIN10_RS1)
// Where the SDK does name it (the same condition it declares it under), it has to
// agree with the number above.
static_assert(FileRenameInfoEx == 22, "FileRenameInfoEx is not the class number iniedit passes");
#endif

// What this process has learned about POSIX-semantics renames. A refusal as
// unsupported is kept for good: the operating system does not change under a
// running game, and a volume that cannot do it once will not the next time. The
// attempt counts and the hooks are the rig's (iniedit.h, replaceHooksForTest).
std::atomic<bool>        g_posixRefused{false};
std::atomic<int>         g_posixAttempts{0};
std::atomic<int>         g_classicAttempts{0};
std::atomic<ReplaceHook> g_posixHook{nullptr};
std::atomic<ReplaceHook> g_classicHook{nullptr};

// The operating system or the volume saying it does not do this, as against this
// one rename having been refused. An OS from before 1607 does not know the
// information class at all (ERROR_INVALID_PARAMETER); FAT, exFAT and some network
// shares answer not-supported or invalid-function.
bool refusedAsUnsupported(DWORD code) {
    return code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED ||
           code == ERROR_INVALID_FUNCTION || code == ERROR_CALL_NOT_IMPLEMENTED ||
           code == ERROR_INVALID_LEVEL;
}

bool isReadOnly(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0;
}

// The absolute form of `path`, or `path` itself when Windows will not give one.
std::wstring fullPathOf(const std::wstring& path) {
    const DWORD need = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (need == 0) return path;
    std::wstring out(need, L'\0');
    const DWORD got = GetFullPathNameW(path.c_str(), need, &out[0], nullptr);
    if (got == 0 || got >= need) return path;
    out.resize(got);
    return out;
}

enum class Posix {
    Done,         // the file is in place
    Unsupported,  // this operating system or volume does not do it
    Transient,    // refused for a reason that passes; the caller tries again
    Other,        // failed some other way; the classic rename decides this attempt
};

// A POSIX-semantics rename's Windows error (0: it worked), read as the writer
// needs it.
Posix classifyPosix(DWORD code) {
    if (code == ERROR_SUCCESS) return Posix::Done;
    if (refusedAsUnsupported(code)) return Posix::Unsupported;
    return passesInMilliseconds(code) ? Posix::Transient : Posix::Other;
}

// One POSIX-semantics rename of `from` over `to`. `code` is the Windows error of
// a failure.
Posix posixReplace(const std::wstring& from, const std::wstring& to, DWORD* code) {
    g_posixAttempts.fetch_add(1);
    if (ReplaceHook hook = g_posixHook.load()) {
        *code = hook(from.c_str(), to.c_str());
        return classifyPosix(*code);
    }

    // What is asked of the operating system, built before the temp file is opened
    // so that nothing between the open and the close can throw. A full path, not
    // whatever the caller passed: with no root directory the name is not relative
    // to anything useful. The buffer is zeroed, so the name ends in a terminator
    // as well as a length.
    const std::wstring name = fullPathOf(to);
    const size_t nameBytes = name.size() * sizeof(wchar_t);
    std::vector<unsigned char> buffer(sizeof(RenameInfoEx) + nameBytes, 0);
    RenameInfoEx* info = reinterpret_cast<RenameInfoEx*>(buffer.data());
    info->flags = kRenameReplaceIfExists | kRenamePosixSemantics;
    info->rootDirectory = nullptr;
    info->fileNameLength = static_cast<DWORD>(nameBytes);
    memcpy(buffer.data() + offsetof(RenameInfoEx, fileName), name.data(), nameBytes);

    // DELETE is the access a rename needs. The share modes leave everybody else
    // free to have the temp file open as well: an antivirus scanning it, most of
    // all, and its being in the way is a wait, not a failure. WRITE_THROUGH is
    // what the classic path's MOVEFILE_WRITE_THROUGH asks for: the data was
    // flushed before (stageFile), this asks that the rename be written through
    // too. OPEN_REPARSE_POINT so that, as with MoveFileExW, it is the named entry
    // that moves and not whatever it might point at.
    HANDLE h = CreateFileW(from.c_str(), DELETE | SYNCHRONIZE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH |
                               FILE_FLAG_OPEN_REPARSE_POINT,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *code = GetLastError();
        return passesInMilliseconds(*code) ? Posix::Transient : Posix::Other;
    }

    // The last error is cleared first, and a failure that left none is read as
    // "not implemented", because a Wine that refuses a class it does not know has
    // not always set one (LLVM's rename guards against the same thing).
    SetLastError(ERROR_SUCCESS);
    const BOOL renamed = SetFileInformationByHandle(
        h, static_cast<FILE_INFO_BY_HANDLE_CLASS>(kFileRenameInfoEx), buffer.data(),
        static_cast<DWORD>(buffer.size()));
    *code = renamed ? ERROR_SUCCESS : GetLastError();  // before CloseHandle can change it
    if (!renamed && *code == ERROR_SUCCESS) *code = ERROR_CALL_NOT_IMPLEMENTED;
    CloseHandle(h);
    return classifyPosix(*code);
}

// One attempt at putting `from` in place of `to`.
bool replaceOnce(const std::wstring& from, const std::wstring& to, DWORD* code) {
    // A read-only target goes straight to the classic rename, which refuses it
    // with ERROR_ACCESS_DENIED as it always has. Whether a POSIX-semantics rename
    // honours the attribute is not something to find out on somebody's settings.
    if (!g_posixRefused.load() && !isReadOnly(to)) {
        switch (posixReplace(from, to, code)) {
            case Posix::Done:
                return true;
            case Posix::Transient:
                // Held by something that will let go. *code says so, and the
                // caller tries again -- through this same call.
                return false;
            case Posix::Unsupported:
                // This operating system or volume does not do it, and asking
                // again on every write would only repeat the answer.
                g_posixRefused.store(true);
                break;
            case Posix::Other:
                // Not understood. The classic rename decides this attempt, and
                // the next attempt asks again.
                break;
        }
    }
    g_classicAttempts.fetch_add(1);
    if (ReplaceHook hook = g_classicHook.load()) {
        *code = hook(from.c_str(), to.c_str());
        return *code == ERROR_SUCCESS;
    }
    if (MoveFileExW(from.c_str(), to.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    *code = GetLastError();
    return false;
}

// replaceOnce, tried again while the failure is one that passes. `tries` (may be
// null) gets the number of attempts made.
bool replaceFile(const std::wstring& from, const std::wstring& to,
                 const AtomicWriteOptions& options, int* tries, DWORD* code) {
    for (int attempt = 1;; ++attempt) {
        if (tries) *tries = attempt;
        if (replaceOnce(from, to, code)) return true;
        if (attempt > options.retries || !passesInMilliseconds(*code)) return false;
        Sleep(options.backoffMs);
    }
}

std::wstring replaceFailure(DWORD code, int tries) {
    std::wstring what = L"it could not be replaced";
    if (tries > 1) what += L" after " + std::to_wstring(tries) + L" tries";
    if (passesInMilliseconds(code)) what += L" -- read-only, or held open by another program?";
    return withCode(what.c_str(), code);
}

}  // namespace

bool writeFileAtomic(const std::wstring& path, const std::string& bytes, std::wstring* error,
                     const AtomicWriteOptions& options, int* tries) {
    if (tries) *tries = 0;
    std::wstring temp;
    if (!stageFile(path, bytes, &temp, error)) return false;

    DWORD code = 0;
    int made = 0;
    const bool replaced = replaceFile(temp, path, options, &made, &code);
    if (tries) *tries = made;
    if (replaced) return true;
    DeleteFileW(temp.c_str());
    setError(error, replaceFailure(code, made));
    return false;
}

bool replaceFileAtomic(const std::wstring& from, const std::wstring& to,
                       const AtomicWriteOptions& options, int* tries, unsigned long* code) {
    DWORD last = ERROR_SUCCESS;
    int made = 0;
    const bool replaced = replaceFile(from, to, options, &made, &last);
    if (tries) *tries = made;
    if (code) *code = replaced ? ERROR_SUCCESS : last;
    return replaced;
}

void replaceHooksForTest(ReplaceHook posix, ReplaceHook classic) {
    g_posixHook.store(posix);
    g_classicHook.store(classic);
    g_posixRefused.store(false);
    g_posixAttempts.store(0);
    g_classicAttempts.store(0);
}

int posixReplaceAttempts() { return g_posixAttempts.load(); }

int classicReplaceAttempts() { return g_classicAttempts.load(); }

bool posixReplaceRefused() { return g_posixRefused.load(); }

bool readFileBytes(const std::wstring& path, std::string* bytes, size_t limit) {
    bytes->clear();
    // FILE_SHARE_DELETE: reading a file must not stop somebody replacing,
    // renaming or deleting it. (A POSIX-semantics replace goes through under a
    // reader that shares DELETE and a classic one does not; either way this handle
    // is closed again below, so the hold lasts as long as the read.)
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > limit) {
        CloseHandle(f);
        return false;
    }
    bytes->resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < bytes->size()) {
        const size_t left = bytes->size() - done;
        const DWORD chunk = static_cast<DWORD>(left > (1u << 20) ? (1u << 20) : left);
        DWORD got = 0;
        if (!ReadFile(f, &(*bytes)[done], chunk, &got, nullptr) || got == 0) break;
        done += got;
    }
    CloseHandle(f);
    // A short read is a failure, not a shorter file: config.cpp refuses one for
    // the same reason, and a truncated copy kept as a backup is worse than none.
    if (done != bytes->size()) {
        bytes->clear();
        return false;
    }
    return true;
}

std::wstring generationPath(const std::wstring& dir, const std::wstring& name, int generation) {
    std::wstring out = dir;
    if (!out.empty() && out.back() != L'\\' && out.back() != L'/') out += L'\\';
    out += name;
    if (generation > 0) out += L"." + std::to_wstring(generation);
    return out;
}

std::wstring newestGeneration(const std::wstring& dir, const std::wstring& name) {
    for (int g = 0; g < kMirrorGenerations; ++g) {
        const std::wstring path = generationPath(dir, name, g);
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (data.nFileSizeHigh == 0 && data.nFileSizeLow == 0) continue;
        return path;
    }
    return std::wstring();
}

bool writeGenerations(const std::wstring& dir, const std::wstring& name,
                      const std::string& bytes, bool rotate, std::wstring* error) {
    const std::wstring newest = generationPath(dir, name, 0);

    std::string old;
    const bool haveOld = readFileBytes(newest, &old);
    if (haveOld && old == bytes) return true;  // nothing new to keep; nothing ages

    // Nothing worth keeping (no newest copy, an empty one, or one that cannot be
    // read), or a change that is not a checkpoint: the newest is replaced in
    // place and no older generation is touched.
    if (!rotate || !haveOld || old.empty()) return writeFileAtomic(newest, bytes, error);

    // The new copy lands first, beside the others and not yet in their way. A
    // write that fails here has moved nothing.
    std::wstring staged;
    if (!stageFile(newest, bytes, &staged, error)) return false;

    const AtomicWriteOptions options{};
    DWORD code = 0;

    // Generation 1 already holds the copy being replaced: an earlier attempt at
    // this same write got that far and failed on its last step (the newest was
    // read-only, or held). Moving it up again would push a real older copy out
    // for a duplicate, and each retry would push out another.
    std::string behind;
    const bool alreadyKept =
        readFileBytes(generationPath(dir, name, 1), &behind) && behind == old;

    if (!alreadyKept) {
        for (int g = kMirrorGenerations - 1; g >= 2; --g) {
            const std::wstring from = generationPath(dir, name, g - 1);
            if (!pathExists(from)) continue;
            if (!replaceFile(from, generationPath(dir, name, g), options, nullptr, &code)) {
                DeleteFileW(staged.c_str());
                setError(error, withCode(L"an older copy could not be moved up", code));
                return false;
            }
        }

        // The copy being replaced becomes generation 1. Written from the bytes
        // just read rather than renamed, so the newest never goes missing: a
        // crash at any point leaves <name> in place, and a restore that reads it
        // finds a whole file.
        std::wstring why;
        if (!writeFileAtomic(generationPath(dir, name, 1), old, &why)) {
            DeleteFileW(staged.c_str());
            setError(error, L"the copy being replaced could not be kept: " + why);
            return false;
        }
    }

    int made = 0;
    if (!replaceFile(staged, newest, options, &made, &code)) {
        DeleteFileW(staged.c_str());
        setError(error, replaceFailure(code, made));
        return false;
    }
    return true;
}

}  // namespace edvr
