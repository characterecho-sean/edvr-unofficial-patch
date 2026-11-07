// F213: the on-foot flight recorder's core (src\common\onfoot_recorder_core.h), run with the same code the game runs. Checks the schema against offsetof/sizeof for every record type,
// the multi-producer ring (nothing lost under capacity, drops counted when full, a push never waits), the per-type sequence, segment rotation and retention over an injected sink,
// a truncated last record, and writes a deterministic fixture folder (argv[1], or a temp folder) that it reads back.
//
// The fixture: onfoot_20260102_030405_000.rec holding three records of every type (seq 0, 1, 2; qpc = 1000 * id + seq). Field i (0-based, in the order of the record's list) element e of
// the record with sequence n has the value base = 100 * id + 7 * i + e, stored as: u8 base % 200 (an enum field: base % the enum's value count); u16 base; u32 and u64 base + 1000000 * n;
// f32 and f64 base + 0.25 + n. Mark records carry number = 1 + n. A checker in another language reads the file by the header's schema and compares against that rule.
#include "../../src/common/onfoot_recorder_core.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <thread>

using namespace edvr::onfootrec;

namespace {

int g_fail = 0, g_pass = 0;
void check(bool ok, const char* what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("FAIL: %s\n", what);
    }
}
#define CHECKF(cond, ...)                      \
    do {                                       \
        char _m[256];                          \
        std::snprintf(_m, sizeof(_m), __VA_ARGS__); \
        check((cond), _m);                     \
    } while (0)

// ---- a sink over a map ------------------------------------------------------------------------------------------------------------------------------------------------------
class MemSink final : public FileSink {
 public:
    std::map<std::string, std::vector<uint8_t>> files;
    std::string current;
    int writes = 0, failAfter = -1;
    bool list(std::vector<DirEntry>* out) override {
        for (auto& f : files) out->push_back({f.first, f.second.size()});
        return true;
    }
    bool remove(const std::string& name) override { return files.erase(name) != 0; }
    bool create(const std::string& name) override {
        if (files.count(name)) return false;
        files[name];
        current = name;
        return true;
    }
    bool write(const void* data, size_t n) override {
        if (current.empty()) return false;
        if (failAfter >= 0 && writes++ >= failAfter) return false;
        const uint8_t* p = static_cast<const uint8_t*>(data);
        files[current].insert(files[current].end(), p, p + n);
        return true;
    }
    void close() override { current.clear(); }
};

// ---- the fixture sink: a folder ------------------------------------------------------------------------------------------------------------------------------------------
class FsSink final : public FileSink {
 public:
    explicit FsSink(std::filesystem::path dir) : dir_(std::move(dir)) { std::filesystem::create_directories(dir_); }
    bool list(std::vector<DirEntry>* out) override {
        for (auto& e : std::filesystem::directory_iterator(dir_))
            if (e.is_regular_file()) out->push_back({e.path().filename().string(), static_cast<uint64_t>(e.file_size())});
        return true;
    }
    bool remove(const std::string& name) override { return std::filesystem::remove(dir_ / name); }
    bool create(const std::string& name) override {
        close();
        if (std::filesystem::exists(dir_ / name)) return false;
        f_.open(dir_ / name, std::ios::binary);
        return f_.good();
    }
    bool write(const void* data, size_t n) override {
        f_.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
        return f_.good();
    }
    void close() override {
        if (f_.is_open()) f_.close();
    }

 private:
    std::filesystem::path dir_;
    std::ofstream f_;
};

HeaderInfo fixtureInfo(uint32_t segment) {
    HeaderInfo h;
    h.build = "v0.0.0-fixture";
    h.gameBuild = "334925";
    h.localTime = "2026-01-02 03:04:05.678";
    h.keysJson = "{\"fix.native_on_foot_eye_height_cm\":170.0000,\"fix.native_on_foot_look_to_aim\":\"off\"}";
    h.pid = 1234;
    h.qpcFreq = 10000000;
    h.qpcAtOpen = 5000;
    h.segment = segment;
    return h;
}
SegmentWriter::HeaderFn fixtureHeader() {
    return [](uint32_t segment, const std::string&) { return segmentHeader(fixtureInfo(segment)); };
}

// ---- reading a segment back -------------------------------------------------------------------------------------------------------------------------------------------------
struct Parsed {
    bool ok = false, truncated = false;
    std::string json;
    size_t body = 0;
    std::vector<size_t> offsets;   // of each complete record
};
Parsed parseSegment(const std::vector<uint8_t>& b) {
    Parsed p;
    if (b.size() < 12 || std::memcmp(b.data(), kMagic, 8) != 0) return p;
    uint32_t n = 0;
    for (int i = 0; i < 4; ++i) n |= static_cast<uint32_t>(b[8 + i]) << (8 * i);
    if (12u + n > b.size()) return p;
    p.json.assign(reinterpret_cast<const char*>(&b[12]), n);
    p.body = 12u + n;
    size_t at = p.body;
    while (at < b.size()) {
        if (b.size() - at < sizeof(Prefix)) {
            p.truncated = true;
            break;
        }
        Prefix h;
        std::memcpy(&h, &b[at], sizeof(h));
        const unsigned want = recordSize(h.id);
        if (!want || h.size != want) {
            p.truncated = true;   // not a record this schema knows: the reader stops
            break;
        }
        if (b.size() - at < h.size) {
            p.truncated = true;
            break;
        }
        p.offsets.push_back(at);
        at += h.size;
    }
    p.ok = true;
    return p;
}
// Braces and brackets balance outside strings: the header is JSON-shaped.
bool jsonBalanced(const std::string& s) {
    int depth = 0;
    bool inStr = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (inStr) {
            if (c == '\\') ++i;
            else if (c == '"') inStr = false;
        } else if (c == '"') {
            inStr = true;
        } else if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            if (--depth < 0) return false;
        }
    }
    return depth == 0 && !inStr;
}

// ---- the fixture rule ----------------------------------------------------------------------------------------------------------------------------------------------------------
double fixtureValue(const RecordDesc& d, unsigned fieldIndex, unsigned elem, unsigned n) {
    const FieldDesc& f = d.fields[fieldIndex];
    const unsigned base = 100u * d.id + 7u * fieldIndex + elem;
    const std::string t = f.type;
    if (t == "u8") {
        if (f.enumName) {
            for (const EnumDesc* e : kEnums)
                if (std::strcmp(e->name, f.enumName) == 0) return base % e->count;
        }
        return base % 200u;
    }
    if (t == "u16") return base;
    if (t == "u32" || t == "u64") return static_cast<double>(base) + 1000000.0 * n;
    return static_cast<double>(base) + 0.25 + n;
}
void storeValue(uint8_t* at, const char* type, double v) {
    const std::string t = type;
    if (t == "u8") { const uint8_t x = static_cast<uint8_t>(v); std::memcpy(at, &x, 1); }
    else if (t == "u16") { const uint16_t x = static_cast<uint16_t>(v); std::memcpy(at, &x, 2); }
    else if (t == "u32") { const uint32_t x = static_cast<uint32_t>(v); std::memcpy(at, &x, 4); }
    else if (t == "u64") { const uint64_t x = static_cast<uint64_t>(v); std::memcpy(at, &x, 8); }
    else if (t == "f32") { const float x = static_cast<float>(v); std::memcpy(at, &x, 4); }
    else { std::memcpy(at, &v, 8); }
}
double loadValue(const uint8_t* at, const char* type) {
    const std::string t = type;
    if (t == "u8") return at[0];
    if (t == "u16") { uint16_t x; std::memcpy(&x, at, 2); return x; }
    if (t == "u32") { uint32_t x; std::memcpy(&x, at, 4); return x; }
    if (t == "u64") { uint64_t x; std::memcpy(&x, at, 8); return static_cast<double>(x); }
    if (t == "f32") { float x; std::memcpy(&x, at, 4); return x; }
    double x;
    std::memcpy(&x, at, 8);
    return x;
}
// A fixture record of a type: zeroed, prefix set, every field filled by the rule.
std::vector<uint8_t> fixtureRecord(const RecordDesc& d, unsigned n) {
    std::vector<uint8_t> r(d.size, 0);
    Prefix h{};
    h.id = d.id;
    h.size = d.size;
    h.seq = n;
    h.qpc = 1000ull * d.id + n;
    std::memcpy(r.data(), &h, sizeof(h));
    for (unsigned i = 0; i < d.nFields; ++i) {
        const FieldDesc& f = d.fields[i];
        for (unsigned e = 0; e < f.count; ++e) storeValue(&r[f.offset + e * f.elemSize], f.type, fixtureValue(d, i, e, n));
    }
    if (d.id == kRecMark) {   // the mark's number is 1 + n (the fixture rule's field 0 is the same number plus 100 * id; the mark rule is the simpler one)
        uint32_t num = 1 + n;
        std::memcpy(&r[kMarkFields[0].offset], &num, 4);
    }
    return r;
}

// ---- schema checks ---------------------------------------------------------------------------------------------------------------------------------------------------------------
const FieldDesc* findField(const RecordDesc& d, const char* name) {
    for (unsigned i = 0; i < d.nFields; ++i)
        if (std::strcmp(d.fields[i].name, name) == 0) return &d.fields[i];
    return nullptr;
}
// Re-derives each field's offset and size from the struct itself (not from the table's own macro) and compares.
#define SCH_S(t, n)                                                                                                                       \
    {                                                                                                                                     \
        const FieldDesc* f = findField(*d, #n);                                                                                           \
        CHECKF(f && f->offset == offsetof(SCH_CUR, n) && f->count == 1 && f->elemSize == sizeof(ty::t) && sizeof(((SCH_CUR*)nullptr)->n) == sizeof(ty::t) && \
                   std::strcmp(f->type, #t) == 0,                                                                                         \
               "%s.%s: the table's offset, count and size are the struct's", d->name, #n);                                                \
    }
#define SCH_A(t, n, c)                                                                                                                    \
    {                                                                                                                                     \
        const FieldDesc* f = findField(*d, #n);                                                                                           \
        CHECKF(f && f->offset == offsetof(SCH_CUR, n) && f->count == (c) && f->elemSize == sizeof(ty::t) &&                              \
                   sizeof(((SCH_CUR*)nullptr)->n) == (c) * sizeof(ty::t) && std::strcmp(f->type, #t) == 0,                                \
               "%s.%s: the table's offset, count and size are the struct's", d->name, #n);                                                \
    }
#define SCH_E(en, n)                                                                                                                      \
    {                                                                                                                                     \
        const FieldDesc* f = findField(*d, #n);                                                                                           \
        CHECKF(f && f->offset == offsetof(SCH_CUR, n) && f->count == 1 && f->elemSize == 1 && f->enumName && std::strcmp(f->enumName, #en) == 0, \
               "%s.%s: the table's offset and enum are the struct's", d->name, #n);                                                       \
    }
#define SCH_RUN(Rec, LIST, ID)                                                                                                            \
    {                                                                                                                                     \
        using SCH_CUR = Rec;                                                                                                              \
        const RecordDesc* d = recordDesc(ID);                                                                                             \
        CHECKF(d && d->size == sizeof(Rec) && d->id == ID, "%s: the table's id and size are the struct's", #Rec);                        \
        if (d) {                                                                                                                          \
            unsigned counted = 0;                                                                                                         \
            LIST(SCH_COUNT_S, SCH_COUNT_A, SCH_COUNT_E)                                                                                   \
            CHECKF(counted == d->nFields, "%s: the table has exactly the list's %u fields (it has %u)", #Rec, counted, d->nFields);       \
            LIST(SCH_S, SCH_A, SCH_E)                                                                                                     \
        }                                                                                                                                 \
    }
#define SCH_COUNT_S(t, n) ++counted;
#define SCH_COUNT_A(t, n, c) ++counted;
#define SCH_COUNT_E(en, n) ++counted;

void testSchema() {
    SCH_RUN(AlignRec, ONF_LIST_ALIGN, kRecAlign)
    SCH_RUN(P1Rec, ONF_LIST_P1, kRecP1)
    SCH_RUN(PinRec, ONF_LIST_PIN, kRecPin)
    SCH_RUN(IkRec, ONF_LIST_IK, kRecIk)
    SCH_RUN(MarkRec, ONF_LIST_MARK, kRecMark)
    SCH_RUN(RtEyeRec, ONF_LIST_RT_EYE, kRecRtEye)
    SCH_RUN(RtRecentreRec, ONF_LIST_RT_RECENTRE, kRecRtRecentre)
    // The tables are well formed in themselves: in order, inside the record, no overlap, aligned, a multiple of 8, enums named and known.
    for (unsigned id = 1; id <= kRecTypes; ++id) {
        const RecordDesc* d = recordDesc(id);
        CHECKF(d->size % 8 == 0 && d->size >= sizeof(Prefix) && d->size <= kMaxRecordBytes, "%s: size %u is a multiple of 8 and within the largest", d->name, unsigned(d->size));
        unsigned end = sizeof(Prefix);
        for (unsigned i = 0; i < d->nFields; ++i) {
            const FieldDesc& f = d->fields[i];
            CHECKF(f.offset >= end, "%s.%s starts at or after the previous field's end (%u >= %u)", d->name, f.name, unsigned(f.offset), end);
            CHECKF(f.offset % f.elemSize == 0, "%s.%s is aligned to its element", d->name, f.name);
            end = f.offset + f.count * f.elemSize;
            CHECKF(end <= d->size, "%s.%s ends inside the record", d->name, f.name);
            if (f.enumName) {
                bool known = false;
                for (const EnumDesc* e : kEnums) known = known || std::strcmp(e->name, f.enumName) == 0;
                CHECKF(known, "%s.%s names an enum that exists (%s)", d->name, f.name, f.enumName);
            }
        }
        for (unsigned j = 0; j < kRecTypes; ++j)
            if (j + 1 != id) check(std::strcmp(kRecords[j].name, d->name) != 0, "record names are unique");
    }
    check(kSlotBytes >= kMaxRecordBytes && kSlotBytes % 64 == 0, "the ring slot holds the largest record");
    // the largest record is what the slot is sized for
    size_t most = 0;
    for (unsigned id = 1; id <= kRecTypes; ++id) most = std::max<size_t>(most, recordSize(id));
    check(most == kMaxRecordBytes, "kMaxRecordBytes is the largest record");
    // initRecord sets the prefix and zeroes the rest, padding included
    IkRec r;
    std::memset(&r, 0xAB, sizeof(r));
    initRecord(r);
    bool zero = true;
    for (size_t i = sizeof(Prefix); i < sizeof(r); ++i) zero = zero && reinterpret_cast<const uint8_t*>(&r)[i] == 0;
    check(zero && r.h.id == kRecIk && r.h.size == sizeof(IkRec), "initRecord zeroes every byte but the prefix's id and size");
}

// ---- the header --------------------------------------------------------------------------------------------------------------------------------------------------------------------
void testHeader() {
    const std::string h = segmentHeader(fixtureInfo(3));
    check(h.size() % 8 == 0, "the header is a multiple of 8 so the records start aligned");
    std::vector<uint8_t> bytes(h.begin(), h.end());
    const Parsed p = parseSegment(bytes);
    check(p.ok && p.body == h.size(), "the header parses and the body starts where it ends");
    check(jsonBalanced(p.json), "the header JSON is balanced");
    for (const char* needle : {"\"format\":1", "\"build\":\"v0.0.0-fixture\"", "\"game_build\":\"334925\"", "\"pid\":1234", "\"qpc_freq\":10000000", "\"qpc_at_open\":5000",
                               "\"local_time_at_open\":\"2026-01-02 03:04:05.678\"", "\"segment\":3", "\"keys\":{", "\"prefix\":[", "\"records\":{", "\"enums\":{"})
        CHECKF(p.json.find(needle) != std::string::npos, "the header carries %s", needle);
    for (unsigned id = 1; id <= kRecTypes; ++id) {
        char b[96];
        std::snprintf(b, sizeof(b), "\"%s\":{\"id\":%u,\"size\":%u,\"fields\":[", recName(id), id, recordSize(id));
        CHECKF(p.json.find(b) != std::string::npos, "the header describes %s", recName(id));
    }
    for (const EnumDesc* e : kEnums) {
        const std::string key = std::string("\"") + e->name + "\":[";
        CHECKF(p.json.find(key) != std::string::npos, "the header lists the enum %s", e->name);
    }
    // escaping: a quote and a backslash in a string
    HeaderInfo odd = fixtureInfo(0);
    odd.build = "a\"b\\c";
    check(headerJson(odd).find("\"build\":\"a\\\"b\\\\c\"") != std::string::npos, "strings in the header are escaped");
}

// ---- the ring ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
void testRing() {
    // nothing is lost under capacity: four producers, one consumer afterwards
    {
        Recorder rec;
        check(rec.init(8192, kSlotBytes), "a ring of 8192 slots allocates");
        constexpr int kThreads = 4, kEach = 1500;
        std::vector<std::thread> ts;
        for (int t = 0; t < kThreads; ++t)
            ts.emplace_back([&rec, t, kEach] {
                for (int i = 0; i < kEach; ++i) {
                    MarkRec m;
                    initRecord(m);
                    m.number = static_cast<uint32_t>(t * 100000 + i);
                    rec.push(m, 1);
                }
            });
        for (auto& t : ts) t.join();
        check(rec.counters(kRecMark).pushed.load() == kThreads * kEach && rec.counters(kRecMark).dropped.load() == 0, "four producers under capacity: every record accepted, none dropped");
        int next[kThreads] = {};
        std::set<uint32_t> seqs;
        uint32_t size = 0;
        alignas(8) uint8_t buf[kSlotBytes];
        int popped = 0;
        bool order = true;
        while (rec.ring().tryPop(buf, sizeof(buf), &size)) {
            MarkRec m;
            std::memcpy(&m, buf, sizeof(m));
            const int t = static_cast<int>(m.number / 100000), i = static_cast<int>(m.number % 100000);
            order = order && t >= 0 && t < kThreads && i == next[t]++;   // one producer's records come out in the order it pushed them
            seqs.insert(m.h.seq);
            ++popped;
        }
        check(popped == kThreads * kEach, "every accepted record is popped");
        check(order, "each producer's records keep their order");
        check(seqs.size() == static_cast<size_t>(kThreads * kEach) && *seqs.begin() == 0 && *seqs.rbegin() == kThreads * kEach - 1, "the per-type sequence is gap-free 0..N-1 with no drops");
    }
    // a full ring drops and counts, and the sequence shows the gap
    {
        Recorder rec;
        rec.init(64, kSlotBytes);
        int accepted = 0;
        for (int i = 0; i < 100; ++i) {
            MarkRec m;
            initRecord(m);
            m.number = i;
            accepted += rec.push(m, 1) ? 1 : 0;
        }
        check(accepted == 64, "a ring of 64 slots accepts 64 of 100 with no consumer");
        check(rec.counters(kRecMark).pushed.load() == 64 && rec.counters(kRecMark).dropped.load() == 36, "the accepted and dropped counts are 64 and 36");
        uint32_t size = 0;
        alignas(8) uint8_t buf[kSlotBytes];
        MarkRec first;
        check(rec.ring().tryPop(buf, sizeof(buf), &size), "a record pops");
        std::memcpy(&first, buf, sizeof(first));
        check(first.h.seq == 0, "the first record kept its sequence 0");
        MarkRec m;
        initRecord(m);
        check(rec.push(m, 1), "a slot freed by the consumer takes the next record");
        int n = 1;
        uint32_t last = 0;
        while (rec.ring().tryPop(buf, sizeof(buf), &size)) {
            MarkRec x;
            std::memcpy(&x, buf, sizeof(x));
            last = x.h.seq;
            ++n;
        }
        check(n == 65 && last == 100, "the record pushed after the 36 drops carries sequence 100: the drops are a gap of 36 in the reader's sequence");
        // a record of the wrong size or an unknown id is refused, and counts nothing
        MarkRec bad;
        initRecord(bad);
        check(!rec.pushRaw(kRecMark, &bad, static_cast<uint32_t>(sizeof(bad)) + 8, 0) && !rec.pushRaw(99, &bad, static_cast<uint32_t>(sizeof(bad)), 0), "a wrong size or an unknown id is refused");
    }
    // a push never waits: the consumer is stopped, producers keep pushing into a full ring
    {
        Recorder rec;
        rec.init(256, kSlotBytes);
        std::atomic<long> accepted{0}, refused{0};
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> ts;
        for (int t = 0; t < 4; ++t)
            ts.emplace_back([&] {
                for (int i = 0; i < 200000; ++i) {
                    AlignRec a;
                    initRecord(a);
                    if (rec.push(a, 1)) accepted.fetch_add(1);
                    else refused.fetch_add(1);
                }
            });
        for (auto& t : ts) t.join();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        check(accepted.load() == 256 && refused.load() == 800000 - 256, "with the consumer stopped, exactly the capacity is accepted and every other push returns false");
        CHECKF(ms < 5000.0, "800000 pushes into a full ring finish (%.0f ms): none blocks", ms);
        check(rec.counters(kRecAlign).dropped.load() == static_cast<uint64_t>(800000 - 256), "every refused push is counted as a drop");
    }
    // producers and a consumer together: accepted + dropped = pushed, and what the consumer reads is what was accepted
    {
        Recorder rec;
        rec.init(128, kSlotBytes);
        std::atomic<bool> done{false};
        std::atomic<long> read{0};
        std::thread consumer([&] {
            alignas(8) uint8_t buf[kSlotBytes];
            uint32_t size = 0;
            for (;;) {
                if (rec.ring().tryPop(buf, sizeof(buf), &size)) {
                    read.fetch_add(1);
                    continue;
                }
                if (done.load()) {   // the producers have finished: one last drain
                    while (rec.ring().tryPop(buf, sizeof(buf), &size)) read.fetch_add(1);
                    break;
                }
                std::this_thread::yield();
            }
        });
        std::vector<std::thread> ts;
        for (int t = 0; t < 3; ++t)
            ts.emplace_back([&] {
                for (int i = 0; i < 50000; ++i) {
                    P1Rec p;
                    initRecord(p);
                    rec.push(p, 1);
                }
            });
        for (auto& t : ts) t.join();
        done.store(true);
        consumer.join();
        const TypeCounters& c = rec.counters(kRecP1);
        CHECKF(c.pushed.load() + c.dropped.load() == 150000, "accepted + dropped = 150000 (%llu + %llu)", (unsigned long long)c.pushed.load(), (unsigned long long)c.dropped.load());
        CHECKF(static_cast<uint64_t>(read.load()) == c.pushed.load(), "the consumer read exactly what was accepted (%ld of %llu)", read.load(), (unsigned long long)c.pushed.load());
    }
    // the sequence is per type
    {
        Recorder rec;
        rec.init(64, kSlotBytes);
        AlignRec a;
        PinRec p;
        for (int i = 0; i < 5; ++i) {
            initRecord(a);
            rec.push(a, 1);
            if (i < 2) {
                initRecord(p);
                rec.push(p, 1);
            }
        }
        uint32_t size = 0;
        alignas(8) uint8_t buf[kSlotBytes];
        std::map<unsigned, std::vector<uint32_t>> seen;
        while (rec.ring().tryPop(buf, sizeof(buf), &size)) {
            Prefix h;
            std::memcpy(&h, buf, sizeof(h));
            seen[h.id].push_back(h.seq);
        }
        check((seen[kRecAlign] == std::vector<uint32_t>{0, 1, 2, 3, 4}) && (seen[kRecPin] == std::vector<uint32_t>{0, 1}), "each type counts its own sequence from 0");
    }
}

// ---- segments, rotation, retention ---------------------------------------------------------------------------------------------------------------------------------------------
void testRotation() {
    MemSink sink;
    Limits lim;
    lim.segmentBytes = segmentHeader(fixtureInfo(0)).size() + 4096;   // the schema is in every header: room for the header and a few records
    lim.keepSegments = 3;
    lim.folderBytes = ~0ull;
    SegmentWriter w(sink, lim, fixtureHeader());
    check(w.begin("20260102_030405"), "a session opens its first segment");
    check(sink.files.count("onfoot_20260102_030405_000.rec") == 1, "the first segment is named onfoot_<stamp>_000.rec");
    AlignRec a;
    for (int i = 0; i < 100; ++i) {
        initRecord(a);
        a.h.seq = i;
        a.serial = i;
        w.append(&a, sizeof(a));
    }
    w.end();
    const auto& st = w.stats();
    CHECKF(st.segments > 4, "100 records of %zu bytes with room for 4096 bytes of them in a segment make several segments (%u)", sizeof(a), st.segments);
    CHECKF(sink.files.size() == 3, "only the newest 3 segments of the session are kept (%zu files)", sink.files.size());
    check(st.rotationDeleted == st.segments - 3, "rotation deleted every older segment");
    uint32_t prevLast = 0;
    bool first = true, selfDescribing = true, withinLimit = true, contiguous = true;
    for (auto& f : sink.files) {
        const Parsed p = parseSegment(f.second);
        selfDescribing = selfDescribing && p.ok && !p.truncated && jsonBalanced(p.json) && p.body % 8 == 0 && p.json.find("\"records\":{") != std::string::npos;
        withinLimit = withinLimit && f.second.size() <= lim.segmentBytes;
        if (p.offsets.empty()) continue;
        AlignRec firstRec, lastRec;
        std::memcpy(&firstRec, &f.second[p.offsets.front()], sizeof(firstRec));
        std::memcpy(&lastRec, &f.second[p.offsets.back()], sizeof(lastRec));
        if (!first) contiguous = contiguous && firstRec.h.seq == prevLast + 1;   // consecutive segments continue the sequence: nothing between them was lost
        prevLast = lastRec.h.seq;
        first = false;
    }
    check(selfDescribing, "each kept segment is complete on its own: magic, JSON header with the schema, whole records");
    check(withinLimit, "no segment is larger than the limit");
    check(contiguous, "the kept segments run on from each other without a gap");
    check(prevLast == 99, "the last record written is the last one appended");
    // each segment's header says which segment it is
    bool numbered = true;
    for (auto& f : sink.files) {
        const std::string name = f.first;
        const unsigned n = static_cast<unsigned>(std::atoi(name.substr(23, 3).c_str()));
        char seg[32];
        std::snprintf(seg, sizeof(seg), "\"segment\":%u,", n);
        numbered = numbered && parseSegment(f.second).json.find(seg) != std::string::npos;
    }
    check(numbered, "each header's segment number is its file name's");
    // a failed write stops the session and says so (the record is lost, not the process)
    MemSink bad;
    bad.failAfter = 1;   // the header goes in; the next write fails
    SegmentWriter wb(bad, lim, fixtureHeader());
    wb.begin("20260102_030406");
    for (int i = 0; i < 5000; ++i) {
        initRecord(a);
        wb.append(&a, sizeof(a));
    }
    wb.flush();
    check(!wb.isOpen() && wb.stats().writeFailures >= 1, "a failed write marks the writer failed");
    // an existing file is never overwritten
    MemSink dup;
    dup.files["onfoot_20260102_030407_000.rec"] = {1, 2, 3};
    SegmentWriter wd(dup, lim, fixtureHeader());
    check(!wd.begin("20260102_030407") && dup.files["onfoot_20260102_030407_000.rec"].size() == 3, "a segment name that exists is not overwritten and the session does not start");
}

void testRetention() {
    check(matchesSegmentName("onfoot_20260102_030405_000.rec"), "the exact name matches");
    for (const char* n : {"onfoot_20260102_030405_000.rec.bak", "onfoot_20260102_030405_00.rec", "onfoot_2026-01-02_030405_000.rec", "Onfoot_20260102_030405_000.rec", "onfoot_20260102_030405_000.REC",
                          "xonfoot_20260102_030405_000.rec", "onfoot_20260102_030405_000.rec ", "onfoot_2026010a_030405_000.rec", "onfoot_20260102_030405_0000.rec", "notes.txt", ""})
        CHECKF(!matchesSegmentName(n), "'%s' is not a segment name", n);
    MemSink sink;
    const auto put = [&](const char* name, size_t bytes) { sink.files[name] = std::vector<uint8_t>(bytes, 1); };
    put("onfoot_20260101_100000_000.rec", 400);   // oldest
    put("onfoot_20260101_100000_001.rec", 400);
    put("onfoot_20260101_110000_000.rec", 400);
    put("onfoot_20260102_090000_000.rec", 400);   // newest
    put("onfoot_20260101_100000_000.rec.bak", 5000);
    put("notes.txt", 1000);
    put("Onfoot_20260101_100000_000.rec", 300);
    Limits lim;
    lim.folderBytes = 4000;   // the folder is 7900 bytes: over the limit
    SegmentWriter w(sink, lim, fixtureHeader());
    w.begin("20260103_000000");
    // the folder is 7900 bytes; deleting every matching file (1600 bytes) leaves 6300: still over the 4000, and nothing else may go
    check(!sink.files.count("onfoot_20260101_100000_000.rec") && !sink.files.count("onfoot_20260101_100000_001.rec") && !sink.files.count("onfoot_20260101_110000_000.rec") &&
              !sink.files.count("onfoot_20260102_090000_000.rec"),
          "with the folder still over the limit, every file of the exact shape goes (oldest first)");
    check(sink.files.count("onfoot_20260101_100000_000.rec.bak") && sink.files.count("notes.txt") && sink.files.count("Onfoot_20260101_100000_000.rec"),
          "a file that is not of the exact shape is never touched, even with the folder over the limit");
    check(w.stats().retentionDeleted == 4, "four files were deleted");
    // oldest first, and it stops when the folder is under the limit
    MemSink s2;
    for (int i = 0; i < 6; ++i) {
        char n[48];
        std::snprintf(n, sizeof(n), "onfoot_20260101_1000%02d_000.rec", i);
        s2.files[n] = std::vector<uint8_t>(1000, 1);
    }
    Limits l2;
    l2.folderBytes = 3500;   // 6000 bytes: two oldest go (4000 left, still >= 3500), a third goes (3000)
    SegmentWriter w2(s2, l2, fixtureHeader());
    w2.begin("20260103_000000");
    check(!s2.files.count("onfoot_20260101_100000_000.rec") && !s2.files.count("onfoot_20260101_100001_000.rec") && !s2.files.count("onfoot_20260101_100002_000.rec") &&
              s2.files.count("onfoot_20260101_100003_000.rec") && s2.files.count("onfoot_20260101_100004_000.rec") && s2.files.count("onfoot_20260101_100005_000.rec"),
          "retention deletes the oldest first and stops once the folder is under the limit");
    // a folder already under the limit loses nothing
    MemSink s3;
    s3.files["onfoot_20260101_100000_000.rec"] = std::vector<uint8_t>(100, 1);
    SegmentWriter w3(s3, Limits{}, fixtureHeader());
    w3.begin("20260103_000000");
    check(s3.files.count("onfoot_20260101_100000_000.rec") == 1 && w3.stats().retentionDeleted == 0, "a folder under the limit keeps everything");
}

void testTruncated() {
    MemSink sink;
    SegmentWriter w(sink, Limits{}, fixtureHeader());
    w.begin("20260102_030405");
    AlignRec a;
    PinRec p;
    for (int i = 0; i < 3; ++i) {
        initRecord(a);
        a.h.seq = i;
        w.append(&a, sizeof(a));
        initRecord(p);
        p.h.seq = i;
        w.append(&p, sizeof(p));
    }
    w.end();
    std::vector<uint8_t> bytes = sink.files.begin()->second;
    const Parsed whole = parseSegment(bytes);
    check(whole.ok && !whole.truncated && whole.offsets.size() == 6, "six whole records parse");
    bytes.resize(bytes.size() - 7);   // the last record is cut mid-field, as a crash would leave it
    const Parsed cut = parseSegment(bytes);
    check(cut.ok && cut.truncated && cut.offsets.size() == 5, "a truncated last record is tolerated: five whole records and the cut one dropped");
    bytes.resize(whole.offsets[5] + 4);   // cut inside the last record's prefix
    const Parsed cutPrefix = parseSegment(bytes);
    check(cutPrefix.ok && cutPrefix.truncated && cutPrefix.offsets.size() == 5, "a record cut inside its prefix is tolerated too");
    bytes.resize(20);   // inside the header
    check(!parseSegment(bytes).ok, "a file cut inside its header is not a segment");
}

// ---- the fixture ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
void testFixture(const std::filesystem::path& dir) {
    std::filesystem::remove_all(dir);
    {
        FsSink sink(dir);
        SegmentWriter w(sink, Limits{}, fixtureHeader());
        check(w.begin("20260102_030405"), "the fixture session opens");
        // the same writer logic as the game's: records are pushed through a Recorder's ring and drained into the writer
        Recorder rec;
        rec.init(256, kSlotBytes);
        for (unsigned n = 0; n < 3; ++n) {
            for (unsigned id = 1; id <= kRecTypes; ++id) {
                std::vector<uint8_t> r = fixtureRecord(*recordDesc(id), n);
                check(rec.pushRaw(id, r.data(), static_cast<uint32_t>(r.size()), 1000ull * id + n), "a fixture record is accepted");
            }
        }
        uint32_t size = 0;
        alignas(8) uint8_t buf[kSlotBytes];
        while (rec.ring().tryPop(buf, sizeof(buf), &size)) w.append(buf, size);
        w.end();
        check(w.stats().records == 3 * kRecTypes, "the fixture holds three records of every type");
    }
    // read it back
    std::vector<std::filesystem::path> files;
    for (auto& e : std::filesystem::directory_iterator(dir)) files.push_back(e.path());
    check(files.size() == 1 && files[0].filename() == "onfoot_20260102_030405_000.rec", "the fixture folder holds the one expected file");
    if (files.size() != 1) return;
    std::ifstream in(files[0], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const Parsed p = parseSegment(bytes);
    check(p.ok && !p.truncated && p.offsets.size() == 3 * kRecTypes, "the fixture file parses whole");
    check(jsonBalanced(p.json) && p.json.find("\"segment\":0,") != std::string::npos && p.json.find("\"pid\":1234") != std::string::npos, "the fixture header is intact");
    unsigned seen[kRecTypes + 1] = {};
    unsigned wrongValues = 0, wrongPrefix = 0;
    for (size_t off : p.offsets) {
        Prefix h;
        std::memcpy(&h, &bytes[off], sizeof(h));
        const RecordDesc* d = recordDesc(h.id);
        const unsigned n = seen[h.id]++;
        if (h.seq != n || h.qpc != 1000ull * h.id + n || h.size != d->size) ++wrongPrefix;
        for (unsigned i = 0; i < d->nFields; ++i) {
            const FieldDesc& f = d->fields[i];
            for (unsigned e = 0; e < f.count; ++e) {
                double want = fixtureValue(*d, i, e, n);
                if (h.id == kRecMark) want = 1 + n;
                const double got = loadValue(&bytes[off + f.offset + e * f.elemSize], f.type);
                if (got != want) {
                    if (wrongValues < 5) std::printf("  %s.%s[%u] of record %u: wanted %.2f, read %.2f\n", d->name, f.name, e, n, want, got);
                    ++wrongValues;
                }
            }
        }
    }
    CHECKF(wrongPrefix == 0, "every fixture record has the right id, size, sequence and QPC (%u wrong)", wrongPrefix);
    CHECKF(wrongValues == 0, "every fixture field reads back as written (%u wrong)", wrongValues);
    bool all = true;
    for (unsigned id = 1; id <= kRecTypes; ++id) all = all && seen[id] == 3;
    check(all, "three records of each of the seven types read back");
}

}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false;
    std::filesystem::path fixture;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
        else fixture = argv[i];
    }
    (void)selfTest;
    if (fixture.empty()) fixture = std::filesystem::temp_directory_path() / "edvr_onfoot_recorder_fixture";
    testSchema();
    testHeader();
    testRing();
    testRotation();
    testRetention();
    testTruncated();
    testFixture(fixture);
    std::printf("onfoot_recorder_test: %d checks passed, %d failed; fixture folder %s\n", g_pass, g_fail, fixture.string().c_str());
    return g_fail ? 1 : 0;
}
