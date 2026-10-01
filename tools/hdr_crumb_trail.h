// Reading the HDR route's breadcrumb trail back in a rig (src\d3d11\flat_hdr_crumbs.h). Both rigs that pin the crumbs
// (flat_temporal_test for the gate, the budget and the wiring, flat_mono_resolve_test for the real resolver on WARP) stub
// edvr::breadcrumb() to collect the lines it is handed, and read them with this: the line grammar, whether every begin
// has its end, and where a step sits in the trail.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace hdr_crumb_trail {

struct Crumb {
    unsigned slot = 0;        // the K of "K/3"; 0 for the budget line and the armed line, which have none
    std::string step;         // "capture-state", "admitted", ...
    std::string edge;         // "begin", "end", or empty for a line that stands alone
    std::string detail;       // everything after
    bool budgetLine = false;  // "gfx: hdr-treat budget of N crumbs spent; none follow"
};

// "gfx: hdr-treat K/3 <step> [begin|end] [detail]". False for a line that is not one of the route's.
inline bool parse(const std::string& line, Crumb* out) {
    static const char kPrefix[] = "gfx: hdr-treat ";
    *out = Crumb{};
    if (line.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) return false;
    std::string rest = line.substr(sizeof(kPrefix) - 1);
    if (rest.compare(0, 10, "budget of ") == 0) { out->budgetLine = true; out->detail = rest; return true; }
    // "gfx: hdr-treat armed key=auto ...": written once when the route is switched on; it has no frame number.
    if (rest.compare(0, 6, "armed ") == 0) { out->step = "armed"; out->detail = rest.substr(6); return true; }
    char* end = nullptr;
    const unsigned long slot = std::strtoul(rest.c_str(), &end, 10);
    if (!end || end == rest.c_str() || std::strncmp(end, "/3 ", 3) != 0) return false;
    out->slot = static_cast<unsigned>(slot);
    rest = std::string(end + 3);
    const size_t sp = rest.find(' ');
    out->step = rest.substr(0, sp);
    std::string tail = sp == std::string::npos ? std::string() : rest.substr(sp + 1);
    if (!tail.empty() || sp != std::string::npos) {
        const size_t sp2 = tail.find(' ');
        const std::string word = tail.substr(0, sp2);
        if (word == "begin" || word == "end") {
            out->edge = word;
            tail = sp2 == std::string::npos ? std::string() : tail.substr(sp2 + 1);
        }
        out->detail = tail;
    }
    return !out->step.empty();
}

// The route's crumbs out of a captured stream of breadcrumb lines (anything else in it is ignored).
inline std::vector<Crumb> trail(const std::vector<std::string>& lines) {
    std::vector<Crumb> out;
    for (const auto& line : lines) {
        Crumb c;
        if (parse(line, &c)) out.push_back(c);
    }
    return out;
}

// Every begin has its end, nested like scopes: the end that follows a begin is that step's, and nothing is left open. On
// a normal frame this is what "the last crumb in the file names the step" rests on.
inline bool balanced(const std::vector<Crumb>& t, std::string* why = nullptr) {
    std::vector<std::string> open;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i].edge == "begin") open.push_back(t[i].step);
        else if (t[i].edge == "end") {
            if (open.empty() || open.back() != t[i].step) {
                if (why) *why = "end of '" + t[i].step + "' at " + std::to_string(i) + (open.empty() ? " with nothing open" : " while '" + open.back() + "' is open");
                return false;
            }
            open.pop_back();
        }
    }
    if (!open.empty()) { if (why) *why = "'" + open.back() + "' is never ended"; return false; }
    return true;
}

// The index of the first crumb at or after `from` with this step and edge (empty edge: a line that stands alone), or -1.
inline int find(const std::vector<Crumb>& t, const char* step, const char* edge, size_t from = 0) {
    for (size_t i = from; i < t.size(); ++i)
        if (t[i].step == step && t[i].edge == edge) return static_cast<int>(i);
    return -1;
}

// How many crumbs carry this step and edge.
inline size_t count(const std::vector<Crumb>& t, const char* step, const char* edge) {
    size_t n = 0;
    for (const auto& c : t) n += c.step == step && c.edge == edge;
    return n;
}

inline bool has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

// The steps, in order, of one frame's trail from the first index on, each begin or stand-alone line named once, for a
// failure message that shows what was written.
inline std::string outline(const std::vector<Crumb>& t, size_t from = 0, size_t to = static_cast<size_t>(-1)) {
    std::string out;
    for (size_t i = from; i < t.size() && i < to; ++i) {
        if (!out.empty()) out += ' ';
        out += t[i].step;
        if (!t[i].edge.empty()) out += t[i].edge == "begin" ? "[" : "]";
    }
    return out;
}

}  // namespace hdr_crumb_trail
