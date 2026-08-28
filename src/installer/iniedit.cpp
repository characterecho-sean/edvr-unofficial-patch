#include "iniedit.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace edvr::installer {
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
std::map<std::string, std::pair<std::string, std::string>> movedKeys(const IniDoc& doc) {
    std::map<std::string, std::pair<std::string, std::string>> out;
    std::vector<std::string> pending;
    for (const IniLine& line : doc.lines) {
        if (line.kind == LineKind::Comment) {
            const std::string body = trim(line.text);
            size_t at = 0;
            while (at < body.size() && (body[at] == '#' || body[at] == ';')) ++at;
            const std::string text = trim(body.substr(at));
            if (lower(text).rfind("moved-from:", 0) == 0) {
                pending.push_back(trim(text.substr(strlen("moved-from:"))));
            }
            continue;
        }
        if (line.kind == LineKind::Blank || line.kind == LineKind::Section) {
            pending.clear();
            continue;
        }
        for (const std::string& p : pending) {
            out[lower(p)] = {line.section, line.key};
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
        doc.hadBom = true;
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
    const std::map<std::string, std::pair<std::string, std::string>> moved = movedKeys(nextDoc);
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
            if (u.present && move != moved.end()) {
                const std::string& newSection = move->second.first;
                const std::string& newKey = move->second.second;
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

}  // namespace edvr::installer
