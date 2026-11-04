// The Scorpion latch (issue 140): whether the player's SRV is the Scorpion, read from the journal as its bytes stream past. Pure (no game, no Windows), so
// tools/srv_turret_hud_test runs it directly; journal_watch.cpp feeds it every byte the tail reads, exactly once, on the worker thread.
//
// WHY. Status.json's turret-view bit (Flags bit 13) is set in the Scarab (SRV type testbuggy) too, and the Scarab's turret view was fine; the camera and the HUD pass are for the
// Scorpion (SRV type combat_multicrew_srv_01) only. Status.json does not say which SRV it is, the journal does:
//   LoadGame   "Ship":"Combat_Multicrew_SRV_01"   the game loaded into the Scorpion (the latch is set when the value is the Scorpion's and cleared when it is anything else)
//   LaunchSRV  "SRVType":"combat_multicrew_srv_01"   a Scorpion was launched (testbuggy, the Scarab, or anything else clears it)
//   Music      "MusicTrack":"MainMenu"            the main menu: clears it
//   DockSRV, SRVDestroyed, Shutdown               clear it
// The values are compared case-insensitively, in place, byte by byte. UNKNOWN IS "NOT THE SCORPION": the latch starts false, and a training scenario writes no journal at all, so
// it stays false there and the Scarab (or the Scorpion) in a training scenario gets the game's own camera and HUD.
//
// PRIVACY. The module's posture is names only. Of a journal line this reads the event NAME (kept: at most 15 bytes, to compare with the six names above), and for LoadGame,
// LaunchSRV and Music one field's value, which it compares with a fixed string as the bytes pass and never stores. The latch keeps one bool and a handful of counters. Nothing else
// of any line is looked at or kept: a line is skipped byte by byte until its next newline.
//
// STREAMING. The state survives a chunk boundary anywhere (the tail reads what the game appended since the last poll, which can end inside a line, an event name or a value), so
// feed() may be given the bytes in any pieces. A newline ends the line whatever state it finds: a LoadGame or LaunchSRV line that ends without its field clears the latch (a value
// that cannot be read is unknown). The field must follow the event name within kLookahead bytes of the same line.
//
// STALE JOURNALS. journal_watch.cpp reads a journal from the top only when it can prove the file is this process's own (journal_watch.h, journalPickNewest: a previous run's
// LoadGame must not announce a Scorpion), and tails any other from its end; a new file resets the latch (reset()).
#pragma once

#include <cstdint>

namespace edvr {
namespace journal_vehicle {

inline constexpr char kScorpionName[] = "combat_multicrew_srv_01";   // LoadGame's Ship and LaunchSRV's SRVType for the Scorpion, lower case
inline constexpr char kMainMenuTrack[] = "mainmenu";                 // Music's MusicTrack for the main menu, lower case
inline constexpr uint32_t kLookahead = 512;                           // bytes after an event's name in which its field must appear

// The event that last moved the latch, for the log line: what the journal said, or the file change.
enum class Event : uint8_t { kNone = 0, kLoadGame, kLaunchSrv, kMusic, kDockSrv, kSrvDestroyed, kShutdown, kNewJournal };
inline const char* eventName(Event e) {
    switch (e) {
        case Event::kLoadGame: return "LoadGame";
        case Event::kLaunchSrv: return "LaunchSRV";
        case Event::kMusic: return "Music";
        case Event::kDockSrv: return "DockSRV";
        case Event::kSrvDestroyed: return "SRVDestroyed";
        case Event::kShutdown: return "Shutdown";
        case Event::kNewJournal: return "NewJournal";
        default: return "none";
    }
}

class Latch {
public:
    bool scorpion() const { return scorpion_; }

    // Forget the stream's position and the latch (a different file). True when the latch was set, so the caller can say it was cleared.
    bool reset() {
        const bool was = scorpion_;
        *this = Latch();
        return was;
    }

    // The next bytes of the journal. onChange(bool scorpion, Event cause) is called each time the latch changes value, in stream order, once.
    template <class OnChange>
    void feed(const char* p, uint32_t n, OnChange&& onChange) {
        if (!p) return;
        for (uint32_t i = 0; i < n; ++i) step(p[i], onChange);
    }

private:
    enum class Phase : uint8_t { kSeek, kName, kKey, kValue };

    static constexpr const char* eventKey() { return "\"event\":\""; }
    static constexpr uint8_t kEventKeyLen = 9;
    static const char* fieldKey(Event e) {
        switch (e) {
            case Event::kLoadGame: return "\"Ship\":\"";
            case Event::kLaunchSrv: return "\"SRVType\":\"";
            default: return "\"MusicTrack\":\"";
        }
    }
    static char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
    static bool same(const char* a, uint8_t an, const char* b) {
        uint8_t i = 0;
        for (; i < an; ++i)
            if (b[i] == '\0' || a[i] != b[i]) return false;
        return b[i] == '\0';
    }

    Phase phase_ = Phase::kSeek;
    uint8_t keyPos_ = 0;        // kSeek: the bytes of "event":" matched; kKey: the bytes of the field's key matched
    char name_[16] = {};        // the event's name, while it is read
    uint8_t nameLen_ = 0;
    bool nameOver_ = false;
    Event event_ = Event::kNone;
    uint16_t scanned_ = 0;      // kKey: the bytes since the event's name ended
    uint8_t valuePos_ = 0;      // kValue: the bytes of the value read (saturating)
    bool valueMatches_ = true;  // ... and whether they are the target's so far
    bool escape_ = false;
    bool scorpion_ = false;

    template <class OnChange>
    void set(bool v, Event cause, OnChange& onChange) {
        if (v == scorpion_) return;
        scorpion_ = v;
        onChange(v, cause);
    }
    void toSeek() {
        phase_ = Phase::kSeek;
        keyPos_ = 0;
    }

    template <class OnChange>
    void step(char c, OnChange& onChange) {
        if (c == '\n') {
            // The line ends. An event that wanted a field and never got it is unknown: the vehicle is not the Scorpion. A Music line without its track changes nothing.
            if ((phase_ == Phase::kKey || phase_ == Phase::kValue) && (event_ == Event::kLoadGame || event_ == Event::kLaunchSrv)) set(false, event_, onChange);
            toSeek();
            return;
        }
        switch (phase_) {
            case Phase::kSeek:
                if (c == eventKey()[keyPos_]) {
                    if (++keyPos_ == kEventKeyLen) {
                        phase_ = Phase::kName;
                        nameLen_ = 0;
                        nameOver_ = false;
                        keyPos_ = 0;
                    }
                } else {
                    keyPos_ = (c == '"') ? 1 : 0;
                }
                break;
            case Phase::kName:
                if (c != '"') {
                    if (nameLen_ < sizeof(name_) - 1) name_[nameLen_++] = c;
                    else nameOver_ = true;
                    break;
                }
                eventNamed(onChange);
                break;
            case Phase::kKey: {
                if (++scanned_ > kLookahead) {   // the field is not within reach: unknown
                    if (event_ == Event::kLoadGame || event_ == Event::kLaunchSrv) set(false, event_, onChange);
                    toSeek();
                    break;
                }
                const char* key = fieldKey(event_);
                if (c == key[keyPos_]) {
                    if (key[++keyPos_] == '\0') {
                        phase_ = Phase::kValue;
                        valuePos_ = 0;
                        valueMatches_ = true;
                        escape_ = false;
                    }
                } else {
                    keyPos_ = (c == '"') ? 1 : 0;
                }
                break;
            }
            case Phase::kValue: {
                if (escape_) {   // the byte after a backslash: the target has none, so the value cannot match
                    escape_ = false;
                    break;
                }
                if (c == '\\') {
                    escape_ = true;
                    valueMatches_ = false;
                    break;
                }
                const char* target = event_ == Event::kMusic ? kMainMenuTrack : kScorpionName;
                if (c == '"') {
                    const bool equal = valueMatches_ && target[valuePos_] == '\0';
                    if (event_ == Event::kMusic) {
                        if (equal) set(false, event_, onChange);   // the main menu clears; any other track says nothing of the vehicle
                    } else {
                        set(equal, event_, onChange);
                    }
                    toSeek();
                    break;
                }
                if (valueMatches_) {
                    if (valuePos_ >= 255 || target[valuePos_] == '\0' || lower(c) != target[valuePos_]) valueMatches_ = false;
                    else ++valuePos_;
                }
                break;
            }
        }
    }

    // The event's name is complete: the five it acts on (nothing else of the line is looked at).
    template <class OnChange>
    void eventNamed(OnChange& onChange) {
        event_ = Event::kNone;
        if (!nameOver_) {
            if (same(name_, nameLen_, "LoadGame")) event_ = Event::kLoadGame;
            else if (same(name_, nameLen_, "LaunchSRV")) event_ = Event::kLaunchSrv;
            else if (same(name_, nameLen_, "Music")) event_ = Event::kMusic;
            else if (same(name_, nameLen_, "DockSRV")) event_ = Event::kDockSrv;
            else if (same(name_, nameLen_, "SRVDestroyed")) event_ = Event::kSrvDestroyed;
            else if (same(name_, nameLen_, "Shutdown")) event_ = Event::kShutdown;
        }
        if (event_ == Event::kLoadGame || event_ == Event::kLaunchSrv || event_ == Event::kMusic) {
            phase_ = Phase::kKey;
            keyPos_ = 0;
            scanned_ = 0;
            return;
        }
        if (event_ == Event::kDockSrv || event_ == Event::kSrvDestroyed || event_ == Event::kShutdown) set(false, event_, onChange);
        toSeek();
    }
};

}  // namespace journal_vehicle
}  // namespace edvr
