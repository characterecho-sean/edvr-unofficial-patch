// The orbit lines' vertex shader with its half-width scaled (orbital_width.h says what and why): the DXBC edit, the
// cache that keeps the patched copy per device, the binding that puts it in for the game's own draw and out again,
// and the one-shot read of the instance stream's half-widths. Header-only like ui_holo_remap.h, whose pixel-shader
// edit this follows, so tools/orbital_width_test links the production code itself.
//
// THE EDIT. The game's program (vs C7FA0C0F5DD49180, build 332841, 2412 bytes, vs_5_0, one constant buffer CB1[333])
// ends its clip-space offset with
//     mul [precise(xy)] r0.xy, r0.xyxx, v1.wwww                      the unit normal times the instance's half-width
//     mul [precise(zw)] r0.zw, cb1[332].zzzw, l(0, 0, 2.0, 2.0)      2 x (1/W, 1/H): two pixels' clip size
//     mul [precise(zw)] r0.zw, r2.wwww, r0.zzzw                      times clip w
//     mul [precise(xy)] r0.xy, r0.zwzz, r0.xyxx                      the offset
// and the edit is the second instruction alone: `l(0,0,2,2)` becomes `cb13[0].xxxx`, and `dcl_constantbuffer
// cb13[1], immediateIndexed` is declared after CB1's. The edit lands only when the lead instruction, the target and
// the two that follow are exactly these tokens, the shader declares exactly one constant buffer (CB1[333]) and has
// no b13 of its own; anything else is refused, said once, and the game's shader stays.
#pragma once

#include "dxbc_container.h"
#include "orbital_width.h"
#include "../common/guard.h"

#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace edvr {
namespace orbital_width {

// Why a copy was not made or not used. Each reason is said once.
enum class Why : uint8_t {
    kNone = 0,
    kNotOurs,   // another shader: no reason to say
    kBytes,     // the bytes are not the 2412 the hash names (size, hash, container or checksum)
    kSlot,      // the program already declares constant buffer 13
    kPatch,     // the instruction stream is not the one the edit was written against
    kLinked,    // created with class linkage: dynamic linkage is not edited
    kIdentity,  // the creation could not be tagged
    kCreate,    // the device refused the patched program
    kConstants, // the factor's buffer could not be made or written
    kBind,      // a getter or setter faulted around the draw, or the game's binding was not what was expected
    kRestore,   // the game's shader or its slot 13 could not be put back
};

inline const char* whyName(Why w) {
    switch (w) {
        case Why::kNone: return "none";
        case Why::kNotOurs: return "not this shader";
        case Why::kBytes: return "bytecode mismatch";
        case Why::kSlot: return "constant buffer slot 13 in use";
        case Why::kPatch: return "patch failed";
        case Why::kLinked: return "class linkage";
        case Why::kIdentity: return "creation not tagged";
        case Why::kCreate: return "shader creation refused";
        case Why::kConstants: return "factor buffer";
        case Why::kBind: return "binding";
        case Why::kRestore: return "restore failed";
    }
    return "?";
}

struct Result {
    Why why = Why::kNone;
    std::string detail;
    bool ok() const { return why == Why::kNone; }
};

struct Refusal : std::runtime_error {
    Why why;
    Refusal(Why w, const char* what) : std::runtime_error(what), why(w) {}
};

inline constexpr uint32_t kBytes = 2412;
inline constexpr uint32_t kProgramType = 0x00010050;  // the SHEX chunk's first token: vs_5_0

// mul [precise(xy)] r0.xy, r0.xyxx, v1.wwww
inline constexpr uint32_t kLead[] = {0x07180038, 0x00100032, 0, 0x00100046, 0, 0x00101ff6, 1};
// mul [precise(zw)] r0.zw, cb1[332].zzzw, l(0.000000, 0.000000, 2.000000, 2.000000)
inline constexpr uint32_t kTarget[] = {0x0b600038, 0x001000c2, 0,          0x00208ea6, 1,         332,
                                       0x00004002, 0,          0,          0x40000000, 0x40000000};
// mul [precise(zw)] r0.zw, r2.wwww, r0.zzzw ; mul [precise(xy)] r0.xy, r0.zwzz, r0.xyxx
inline constexpr uint32_t kFollow[] = {0x07600038, 0x001000c2, 0, 0x00100ff6, 2, 0x00100ea6, 0,
                                       0x07180038, 0x00100032, 0, 0x00100ae6, 0, 0x00100046, 0};
// dcl_constantbuffer CB1[333], immediateIndexed (what must be the one declaration) and the one added after it
inline constexpr uint32_t kCb1Declaration[] = {0x04000059, 0x00208e46, 1, 333};
inline constexpr uint32_t kCb13Declaration[] = {0x04000059, 0x00208e46, kSlot, 1};
// mul [precise(zw)] r0.zw, cb1[332].zzzw, cb13[0].xxxx
inline constexpr uint32_t kReplacement[] = {0x09600038, 0x001000c2, 0, 0x00208ea6, 1, 332, 0x00208006, kSlot, 0};

inline uint64_t hashOf(const void* data, size_t n) {
    const auto* p = static_cast<const BYTE*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// The tokens of the program with the edit made. Throws Refusal when the stream is not the one the edit was written for.
inline std::vector<uint32_t> patchProgram(const std::vector<uint32_t>& t) {
    using namespace dxbc_container;
    constexpr size_t kLeadN = sizeof(kLead) / 4, kTargetN = sizeof(kTarget) / 4, kFollowN = sizeof(kFollow) / 4;
    constexpr size_t kCbN = sizeof(kCb1Declaration) / 4, kReplacementN = sizeof(kReplacement) / 4;
    if (!(t.size() > 2 && t[0] == kProgramType && t[1] == t.size()))
        throw Refusal(Why::kPatch, "not a vs_5_0 program of the length it names");
    size_t declaration = 0, target = 0, before = 0, last = 0;  // `last`: where the instruction walked just before this one starts
    unsigned declarations = 0, found = 0;
    for (size_t a = 2; a < t.size();) {
        const uint32_t n = instructionLength(t, a);
        if ((t[a] & 2047) == 89) {  // dcl_constantbuffer
            if (n != kCbN) throw Refusal(Why::kPatch, "a constant buffer declaration of another shape");
            if (t[a + 2] == kSlot) throw Refusal(Why::kSlot, "the program already declares constant buffer 13");
            ++declarations;
            declaration = a;
        }
        if (n == kTargetN && std::equal(kTarget, kTarget + n, t.begin() + a)) {
            target = a;
            before = last;
            ++found;
        }
        last = a;
        a += n;
    }
    if (found != 1)
        throw Refusal(Why::kPatch, found ? "the target instruction occurs more than once" : "the target instruction is not in the program");
    if (declarations != 1 || !std::equal(kCb1Declaration, kCb1Declaration + kCbN, t.begin() + declaration))
        throw Refusal(Why::kPatch, "the program's constant buffers are not exactly CB1[333]");
    // The lead is the INSTRUCTION before the target (the walk's own boundary), not a run of tokens that reads like one.
    if (target - before != kLeadN || !std::equal(kLead, kLead + kLeadN, t.begin() + before))
        throw Refusal(Why::kPatch, "the instruction before the target is not the half-width multiply");
    if (target + kTargetN + kFollowN > t.size() || !std::equal(kFollow, kFollow + kFollowN, t.begin() + target + kTargetN))
        throw Refusal(Why::kPatch, "the instructions after the target are not the clip-w multiply and the offset");
    const size_t declarationEnd = declaration + kCbN;
    std::vector<uint32_t> out;
    out.reserve(t.size() + kCbN);
    out.insert(out.end(), t.begin(), t.begin() + declarationEnd);
    out.insert(out.end(), kCb13Declaration, kCb13Declaration + kCbN);
    out.insert(out.end(), t.begin() + declarationEnd, t.begin() + target);
    out.insert(out.end(), kReplacement, kReplacement + kReplacementN);
    out.insert(out.end(), t.begin() + target + kTargetN, t.end());
    out[1] = static_cast<uint32_t>(out.size());
    // The edit only: every other token is the game's own (the length word aside), and the edited stream still walks
    // to its end on instruction boundaries with exactly two constant buffer declarations.
    const size_t replaced = target + kCbN;
    bool same = out.size() == t.size() + kCbN + kReplacementN - kTargetN;
    same = same && std::equal(t.begin() + 2, t.begin() + declarationEnd, out.begin() + 2);
    same = same && std::equal(kCb13Declaration, kCb13Declaration + kCbN, out.begin() + declarationEnd);
    same = same && std::equal(t.begin() + declarationEnd, t.begin() + target, out.begin() + declarationEnd + kCbN);
    same = same && std::equal(kReplacement, kReplacement + kReplacementN, out.begin() + replaced);
    same = same && std::equal(t.begin() + target + kTargetN, t.end(), out.begin() + replaced + kReplacementN);
    if (!same || out[0] != t[0]) throw Refusal(Why::kPatch, "an untouched token changed");
    unsigned cbs = 0;
    size_t a = 2;
    while (a < out.size()) {
        if ((out[a] & 2047) == 89) ++cbs;
        a += instructionLength(out, a);
    }
    if (a != out.size() || cbs != 2) throw Refusal(Why::kPatch, "the edited stream does not walk");
    return out;
}

// The container the game created the shader with, patched. The input's retail checksum is verified and the output's
// made fresh (dxbc_container.h). `hash` is what the creation hook computed over the same bytes.
inline Result patch(const void* data, size_t n, uint64_t hash, std::vector<BYTE>& output) {
    output.clear();
    try {
        if (!data || n != kBytes || hash != kVs || hashOf(data, n) != kVs)
            return {Why::kBytes, "size " + std::to_string(n) + " (the game's program is 2412 bytes) or the hash is not C7FA0C0F5DD49180"};
        auto chunks = dxbc_container::parseContainer(data, n, kProgramType);
        unsigned changed = 0;
        for (auto& c : chunks) {
            if (c.tag != 0x58454853u && c.tag != 0x52444853u) continue;  // SHEX / SHDR
            std::vector<uint32_t> tokens(c.bytes.size() / 4);
            std::memcpy(tokens.data(), c.bytes.data(), c.bytes.size());
            const auto edited = patchProgram(tokens);
            c.bytes.resize(edited.size() * 4);
            std::memcpy(c.bytes.data(), edited.data(), c.bytes.size());
            ++changed;
        }
        if (changed != 1) return {Why::kPatch, "no single program chunk"};
        output = dxbc_container::makeContainer(chunks);
        return {};
    } catch (const Refusal& r) {
        return {r.why, r.what()};
    } catch (const std::exception& e) {
        return {Why::kBytes, e.what()};
    }
}

inline const GUID& identityGuid() {
    static const GUID guid = {0x6c1f0b52, 0x3a9e, 0x4d70, {0x8b, 0x14, 0x92, 0x5e, 0x07, 0xc3, 0xa1, 0x6d}};
    return guid;
}
struct Identity {
    uint64_t hash;
    uintptr_t device;
};

using SetVs = void (*)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, uint32_t);
inline void directSetVs(ID3D11DeviceContext* c, ID3D11VertexShader* v, ID3D11ClassInstance* const* a, uint32_t n) {
    c->VSSetShader(v, a, n);
}

// One patched shader per device, from bytes the creation hook verified. The creation threads call remember(); the
// render thread calls prepare() and shader(). Creation of the patched shader is a device call and is made on the
// render thread only (the frame boundary).
class Cache {
    std::mutex mutex_;
    std::vector<BYTE> bytes_;
    std::atomic<bool> remembered_{false};
    ID3D11Device* device_ = nullptr;  // identity only (the shader below holds its device alive)
    ID3D11VertexShader* shader_ = nullptr;
    bool attempted_ = false;

public:
    Cache() = default;
    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;
    ~Cache() { reset(); }

    bool remembered() const { return remembered_.load(std::memory_order_acquire); }
    bool attempted() const { return attempted_; }
    ID3D11VertexShader* shader() const { return shader_; }

    // A shader the game created. Any hash but kVs is "not ours" and costs one compare. The exact program is patched
    // off to the side and kept; the creation is tagged so the draw can tell this shader from any other bound to the
    // same slot.
    Result remember(ID3D11VertexShader* shader, uint64_t hash, const void* data, size_t n, bool linked) {
        if (hash != kVs) return {Why::kNotOurs, ""};
        if (!shader || linked) return {Why::kLinked, linked ? "created with class linkage" : "no shader"};
        std::vector<BYTE> edited;
        Result r = patch(data, n, hash, edited);
        if (!r.ok()) return r;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!bytes_.empty() && bytes_ != edited) return {Why::kPatch, "a second program with this hash differs"};
            if (bytes_.empty()) bytes_ = std::move(edited);
        }
        ID3D11Device* device = nullptr;
        HRESULT hr = E_FAIL;
        const bool ran = guarded("orbital.width.identity", [&] {
            shader->GetDevice(&device);
            if (device) {
                const Identity identity{kVs, reinterpret_cast<uintptr_t>(device)};
                hr = shader->SetPrivateData(identityGuid(), sizeof(identity), &identity);
            }
        });
        release(device);
        if (!ran || FAILED(hr)) return {Why::kIdentity, "SetPrivateData on the game's shader failed"};
        remembered_.store(true, std::memory_order_release);
        return {};
    }

    // Is `bound` a shader remember() tagged, on this device? The draw's own check that the shader bound is the exact
    // program, whatever the binding shadow said.
    static bool isOriginal(ID3D11VertexShader* bound, ID3D11DeviceContext* ctx) {
        if (!bound || !ctx) return false;
        Identity identity{};
        UINT length = sizeof(identity);
        HRESULT hr = E_FAIL;
        ID3D11Device* device = nullptr;
        const bool ran = guarded("orbital.width.identity.read", [&] {
            hr = bound->GetPrivateData(identityGuid(), &length, &identity);
            ctx->GetDevice(&device);
        });
        const bool ok = ran && SUCCEEDED(hr) && length == sizeof(identity) && identity.hash == kVs && device &&
                        identity.device == reinterpret_cast<uintptr_t>(device);
        release(device);
        return ok;
    }

    // Render thread, frame boundary: the patched shader on ctx's device, created the first time and never retried
    // after a failure (the result says why). Null when it is not there.
    ID3D11VertexShader* prepare(ID3D11DeviceContext* ctx, Result* result) {
        if (!ctx || !remembered()) return nullptr;
        ID3D11Device* d = nullptr;
        if (!guarded("orbital.width.prepare.device", [&] { ctx->GetDevice(&d); }) || !d) {
            release(d);
            return nullptr;
        }
        if (d != device_) {
            release(shader_);
            device_ = d;
            attempted_ = false;
        }
        if (!attempted_) {
            attempted_ = true;
            std::vector<BYTE> bytes;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                bytes = bytes_;
            }
            ID3D11VertexShader* made = nullptr;
            HRESULT hr = E_FAIL;
            const bool ran = !bytes.empty() &&
                guarded("orbital.width.prepare.create", [&] { hr = d->CreateVertexShader(bytes.data(), bytes.size(), nullptr, &made); });
            if (!ran || FAILED(hr) || !made) {
                release(made);
                if (result) *result = {Why::kCreate, "CreateVertexShader on the patched program failed (HRESULT " + std::to_string(static_cast<unsigned>(hr)) + ")"};
            } else {
                shader_ = made;
            }
        }
        release(d);
        return shader_;
    }

    // The patched bytes, for the rig.
    std::vector<BYTE> bytes() {
        std::lock_guard<std::mutex> lock(mutex_);
        return bytes_;
    }

    void reset() {
        release(shader_);
        device_ = nullptr;
        attempted_ = false;
    }
};

// The game's vertex shader and its slot-13 buffer put aside, the patched ones in for one draw, and put back. The
// shape and the failure rules are ui_holo_remap::Binding's: a restore that failed twice leaves the saved references
// held and needsRestore() true, and settle() -- run at the next frame boundary -- puts them back only into a slot
// that still holds EDVR's own object (a slot the game rebound holds its own state already).
class Binding {
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11Buffer* cb_ = nullptr;
    const void* patched_ = nullptr;
    const void* constants_ = nullptr;
    ID3D11ClassInstance* classes_[D3D11_SHADER_MAX_INTERFACES] = {};
    UINT count_ = D3D11_SHADER_MAX_INTERFACES;
    bool shaderSaved_ = false, bufferSaved_ = false, modified_ = false;

    void drop() {
        release(vs_);
        release(cb_);
        for (auto& p : classes_) release(p);
        count_ = D3D11_SHADER_MAX_INTERFACES;
        shaderSaved_ = bufferSaved_ = modified_ = false;
        patched_ = constants_ = nullptr;
    }
    bool settleShader(ID3D11DeviceContext* c, SetVs setVs) {
        ID3D11VertexShader* bound = nullptr;
        const bool read = guarded("orbital.width.settle.get.vs", [&] { c->VSGetShader(&bound, nullptr, nullptr); });
        const bool ours = bound && static_cast<const void*>(bound) == patched_;
        release(bound);
        if (!read) return false;
        if (ours && !guarded("orbital.width.settle.vs", [&] { setVs(c, vs_, classes_, count_); })) return false;
        release(vs_);
        for (auto& p : classes_) release(p);
        count_ = D3D11_SHADER_MAX_INTERFACES;
        shaderSaved_ = false;
        return true;
    }
    bool settleBuffer(ID3D11DeviceContext* c) {
        ID3D11Buffer* bound = nullptr;
        const bool read = guarded("orbital.width.settle.get.cb", [&] { c->VSGetConstantBuffers(kSlot, 1, &bound); });
        const bool ours = bound && static_cast<const void*>(bound) == constants_;
        release(bound);
        if (!read) return false;
        if (ours && !guarded("orbital.width.settle.cb", [&] { c->VSSetConstantBuffers(kSlot, 1, &cb_); })) return false;
        release(cb_);
        bufferSaved_ = false;
        return true;
    }

public:
    Binding() = default;
    Binding(const Binding&) = delete;
    Binding& operator=(const Binding&) = delete;
    ~Binding() { drop(); }

    bool needsRestore() const { return modified_; }
    void clear() { drop(); }

    // `accept` is asked about the shader that is bound now (exactly the program? the cache's isOriginal): a draw whose
    // bound shader it does not accept, or that carries class instances, is left alone. True: the patched shader and
    // the constants are bound and restore() owes the game its own back.
    template <class Accept>
    bool begin(ID3D11DeviceContext* c, ID3D11VertexShader* patched, ID3D11Buffer* constants, Accept&& accept,
               SetVs setVs = directSetVs) {
        if (!c || !patched || !constants || modified_ || shaderSaved_ || bufferSaved_) return false;
        bool ok = false;
        const bool ran = guarded("orbital.width.bind", [&] {
            c->VSGetShader(&vs_, classes_, &count_);
            shaderSaved_ = true;
            c->VSGetConstantBuffers(kSlot, 1, &cb_);
            bufferSaved_ = true;
            if (count_ != 0 || !vs_ || !accept(vs_)) return;
            patched_ = patched;
            constants_ = constants;
            modified_ = true;  // either setter can partially publish before a fault
            c->VSSetConstantBuffers(kSlot, 1, &constants);
            setVs(c, patched, nullptr, 0);
            ok = true;
        });
        if (!ran || !ok) {
            if (!modified_) drop();  // nothing of ours is bound: let go of what the getters took
            return false;
        }
        return true;
    }

    bool restore(ID3D11DeviceContext* c, SetVs setVs = directSetVs) {
        if (!modified_) return true;
        bool ok = true;
        if (shaderSaved_) ok = guarded("orbital.width.restore.vs", [&] { setVs(c, vs_, classes_, count_); }) && ok;
        if (bufferSaved_) ok = guarded("orbital.width.restore.cb", [&] { c->VSSetConstantBuffers(kSlot, 1, &cb_); }) && ok;
        if (ok) drop();
        return ok;
    }
    struct RestoreResult {
        bool restored, retried;
    };
    RestoreResult finish(ID3D11DeviceContext* c, SetVs setVs = directSetVs) {
        if (restore(c, setVs)) return {true, false};
        return {restore(c, setVs), true};  // one bounded attempt, the saved references retained
    }
    bool settle(ID3D11DeviceContext* c, SetVs setVs = directSetVs) {
        if (!modified_) return true;
        if (!c) return false;
        bool ok = true;
        if (shaderSaved_) ok = settleShader(c, setVs) && ok;
        if (bufferSaved_) ok = settleBuffer(c) && ok;
        if (ok) modified_ = false;
        return ok;
    }
};

// The first scaled draw's instance stream, read once: the half-widths the game asked for (the fourth float of each
// 60 byte record in vertex buffer 1), so the log can say what the factor was applied to. begin() copies the records
// the draw reads into a staging buffer before the draw is issued and never waits; poll(), once a frame, maps it with
// DO_NOT_WAIT and answers kPending until the GPU has got there. A buffer of another stride, a range past the buffer or
// any failure ends the read with kFailed: it is an instrument, and the draw never depends on it.
class InstanceProbe {
    ID3D11Buffer* stage_ = nullptr;
    uint32_t instances_ = 0, issued_ = 0;
    bool pending_ = false;

public:
    static constexpr uint32_t kMaxInstances = 64;
    static constexpr uint32_t kFirstMapFrames = 2;   // the copy is queued behind the frame's work: not before the frame after next
    static constexpr uint32_t kGiveUpFrames = 240;   // four seconds at 60 Hz
    enum class Poll { kIdle, kPending, kDone, kFailed };

    InstanceProbe() = default;
    InstanceProbe(const InstanceProbe&) = delete;
    InstanceProbe& operator=(const InstanceProbe&) = delete;
    ~InstanceProbe() { reset(); }

    bool pending() const { return pending_; }

    bool begin(ID3D11DeviceContext* c, uint32_t instances, uint32_t startInstance, uint32_t frame) {
        if (pending_ || !c || !instances || instances > kMaxInstances) return false;
        ID3D11Buffer* vb = nullptr;
        ID3D11Device* d = nullptr;
        ID3D11Buffer* made = nullptr;
        bool ok = false;
        const bool ran = guarded("orbital.width.probe.begin", [&] {
            UINT stride = 0, offset = 0;
            c->IAGetVertexBuffers(1, 1, &vb, &stride, &offset);
            if (!vb || stride != kInstanceStride) return;
            D3D11_BUFFER_DESC bd{};
            vb->GetDesc(&bd);
            const uint64_t first = uint64_t(offset) + uint64_t(startInstance) * stride;
            const uint64_t bytes = uint64_t(instances) * stride;
            if (first + bytes > bd.ByteWidth) return;
            c->GetDevice(&d);
            if (!d) return;
            D3D11_BUFFER_DESC sd{};
            sd.ByteWidth = static_cast<UINT>(bytes);
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(d->CreateBuffer(&sd, nullptr, &made)) || !made) return;
            const D3D11_BOX box{static_cast<UINT>(first), 0, 0, static_cast<UINT>(first + bytes), 1, 1};
            c->CopySubresourceRegion(made, 0, 0, 0, 0, vb, 0, &box);
            ok = true;
        });
        release(vb);
        release(d);
        if (!ran || !ok) {
            release(made);
            return false;
        }
        stage_ = made;
        instances_ = instances;
        issued_ = frame;
        pending_ = true;
        return true;
    }

    // Fills halfWidths[0..*count) when it answers kDone.
    Poll poll(ID3D11DeviceContext* c, uint32_t frame, float* halfWidths, uint32_t cap, uint32_t* count) {
        if (!pending_) return Poll::kIdle;
        if (!c || frame < issued_ + kFirstMapFrames) return Poll::kPending;
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT hr = E_FAIL;
        const bool ran = guarded("orbital.width.probe.map", [&] { hr = c->Map(stage_, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m); });
        if (ran && hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            if (frame - issued_ <= kGiveUpFrames) return Poll::kPending;
            reset();
            return Poll::kFailed;
        }
        if (!ran || FAILED(hr) || !m.pData) {
            reset();
            return Poll::kFailed;
        }
        const uint32_t n = instances_ < cap ? instances_ : cap;
        for (uint32_t i = 0; i < n; ++i) {
            float w = 0.0f;
            std::memcpy(&w, static_cast<const uint8_t*>(m.pData) + size_t(i) * kInstanceStride + 3 * sizeof(float), sizeof(w));
            halfWidths[i] = w;
        }
        if (count) *count = n;
        guarded("orbital.width.probe.unmap", [&] { c->Unmap(stage_, 0); });
        reset();
        return Poll::kDone;
    }

    void reset() {
        release(stage_);
        pending_ = false;
        instances_ = issued_ = 0;
    }
};

}  // namespace orbital_width
}  // namespace edvr
