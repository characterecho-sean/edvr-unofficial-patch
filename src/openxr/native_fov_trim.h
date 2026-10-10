#pragma once
#include "game_render_size.h"

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <cstring>

namespace edvr::openxr {

// The field-of-view trim's stage machine. It tells the game a narrower
// projection and a smaller render size, then waits for the game's own
// render-target rebuild to land before the narrower image is placed.
//
// This was the terrain guard's engine as well (a widened frustum, with a scene
// hold, a crop at submit and two projection channels); the guard was removed
// 2026-10-09 and the trim, which had always run through the same machine, kept
// it. The stage numbers are the trace's stage= and stay as they were.
enum class NativeTrimStage : uint32_t { Off = 0, Adopting = 2, Live = 3, Inert = 4 };

struct NativeTrimSettings {
  // Degrees taken off each eye's own edges before anything else happens, so
  // the game is told a narrower projection AND a smaller render size: every
  // pass it runs, upscaler included, is priced by that rectangle. Outer is
  // the temple side of each eye, nasal the nose side (which costs only the
  // stereo overlap), vertical both the top and the bottom edge. Zero leaves
  // the edge exactly where the runtime put it.
  float trimOuterDeg = 0.0f;
  float trimNasalDeg = 0.0f;
  float trimVerticalDeg = 0.0f;
};

struct NativeTrimFrustum { float left=0, right=0, down=0, up=0; };
struct NativeTrimDimensions { uint32_t width=0, height=0; };
struct NativeTrimBounds { float left=0, top=0, right=1, bottom=1; };

// The trim's own limits, in degrees. No edge is brought closer than
// kNativeTrimMinEdgeDeg to straight ahead and no axis is left narrower than
// kNativeTrimMinSpanDeg, whatever is asked for: a trim that closed an eye
// would look like a broken runtime rather than a setting.
constexpr float kNativeTrimMinEdgeDeg = 5.0f;
constexpr float kNativeTrimMinSpanDeg = 20.0f;
// Frames of adoption after which a target that has moved at all is taken as
// the rebuild although no ratio matched. The game's own quality multiplier
// can change underneath the ask (0.65 to 0.5 and back within one adoption in
// the flight of 2026-09-16), and then no ratio ever matches; the narrowed
// projection is correct at any target size. Three seconds at 90 Hz.
constexpr uint32_t kNativeTrimAdoptGraceFrames = 270;
// The nudge (NativeFovTrim::startNudge): rows taken off the lowest height
// the game has been told since its last rebuild, so that it rebuilds for a
// change it would otherwise ignore, and how many rows under the true ask
// the told height may fall before the ask is told as it is.
constexpr uint32_t kNativeTrimNudgePx = 2;
constexpr uint32_t kNativeTrimNudgeMaxPx = 40;
// What startNudge decided as a frame entered Adopting. First: no rebuild has
// been seen since the scene began, nothing to measure against. Same: the
// size did not move, only the angles, and the pair in hand already fits.
// Shrink: the height fell under the floor, the game rebuilds by itself.
// Taller: more rows than were told before, never nudged, the next apply
// builds them. Capped: the dip would cost more than kNativeTrimNudgeMaxPx
// rows. Started: the game is told the dipped height from this frame on.
enum class NativeTrimNudge : uint32_t { None, First, Same, Shrink, Taller, Capped, Started };
inline const char* nativeTrimNudgeName(NativeTrimNudge n) noexcept {
  switch (n) {
    case NativeTrimNudge::First: return "first";
    case NativeTrimNudge::Same: return "same";
    case NativeTrimNudge::Shrink: return "shrink";
    case NativeTrimNudge::Taller: return "taller";
    case NativeTrimNudge::Capped: return "capped";
    case NativeTrimNudge::Started: return "started";
    default: return "none";
  }
}
// How the adoption in hand, or the one that went live, was entered: the
// trace's route=. A trim tells the game exactly the frustum the headset shows,
// a function of the runtime's frustum and the settings only, so it is told
// from the first frame both are known. FirstAsk: told before the game had read
// any size, so every pair it renders is rendered for this ask and the first
// complete pair promotes, with no baseline. Rebuild: told after the game has
// read an earlier size: a baseline pair, then its rebuild, as a change while
// live has always been landed.
enum class NativeTrimRoute : uint32_t { None, FirstAsk, Rebuild };
inline const char* nativeTrimRouteName(NativeTrimRoute r) noexcept {
  switch (r) {
    case NativeTrimRoute::FirstAsk: return "first_ask";
    case NativeTrimRoute::Rebuild: return "rebuild";
    default: return "none";
  }
}
constexpr float kNativeTrimPi = 3.1415926535f;

inline float nativeTrimDegrees(float tangent) noexcept {
  return std::atan(tangent) * 180.0f / kNativeTrimPi;
}
inline float nativeTrimTangent(float degrees) noexcept {
  return std::tan(degrees * kNativeTrimPi / 180.0f);
}

// Elite applies its HMD quality multiplier with integer truncation. Keep the
// game-facing recommendation even on both axes so quality 0.5 cannot lose a
// pixel from an odd runtime or trimmed size. The XR swapchain dimensions stay
// in the host's raw `sizes` records; this helper is only for game geometry.
inline NativeTrimDimensions gameFacingDimensions(NativeTrimDimensions v) noexcept {
  return {gameFacingDimension(v.width), gameFacingDimension(v.height)};
}

// Owner-thread policy. It does no OpenXR, graphics, or configuration I/O.
class NativeFovTrim final {
 public:
  // sizeAsked: whether the game has read a recommended size, through either
  // channel, before this frame (NativeTrimRoute). The default is the
  // conservative answer: a game that has asked may hold targets for what it
  // was told, and a rebuild is waited for.
  NativeTrimStage beginFrame(const NativeTrimSettings& settings,
      const NativeTrimFrustum (&trueFrusta)[2],
      const NativeTrimDimensions (&runtimeDims)[2],
      uint64_t referenceGeneration, bool sizeAsked = true) noexcept {
    changed_ = false; nudge_ = NativeTrimNudge::None;
    const bool standDown=pendingStandDown_;pendingStandDown_=false;
    // Every exit records what the game is told from here on, so the next
    // adoption knows what size the pair that seeds its baseline was rendered
    // for, and the lowest height told since the last rebuild seen, which the
    // nudge dips under (startNudge).
    const auto finish=[this]{
      for(unsigned e=0;e<2;++e){told_[e]=recommended(e);trueTold_[e]=trueRecommended(e);}
      if(stage_==NativeTrimStage::Adopting||stage_==NativeTrimStage::Live){
        const uint32_t h=told_[0].height; if(h&&(!floor_||h<floor_)){floor_=h;floorAsk_=trueTold_[0].height;}
      }
      return stage_;};
    const bool valid = validSettings(settings) && validInputs(trueFrusta, runtimeDims);
    // Nothing to trim is nothing to run: the game is told the runtime's own
    // projection and size, as it always was.
    if (!valid || !anyTrim(settings)) {
      settings_=settings; referenceGeneration_=referenceGeneration;
      if (validInputs(trueFrusta,runtimeDims)) { copy(trueFrusta, true_); copy(runtimeDims, runtime_); copy(true_, target_); }
      if (stage_ != NativeTrimStage::Off) changed_ = true;
      clear();maxFactorW_=maxFactorH_=1;clearApplied();
      forcedInert_=false; stage_ = NativeTrimStage::Off; route_ = NativeTrimRoute::None; return finish();
    }
    const bool configChanged = !sameSettings(settings, settings_);
    // Recenter moves the reference space, not the optical projection or the
    // game's render targets. Re-arming adoption there would seed the already
    // resized targets as a new baseline and wait for a second resize.
    const bool geometryChanged = !sameFrusta(trueFrusta) || !sameDims(runtimeDims);
    referenceGeneration_ = referenceGeneration;
    if (configChanged || geometryChanged) {
      settings_ = settings; copy(trueFrusta, true_); copy(runtimeDims, runtime_); copy(true_, target_);
      referenceGeneration_ = referenceGeneration;
      // A change while a rebuild is in flight keeps the baseline: it is the
      // last pair known to have been rendered for a known ask, whether or not
      // the game has moved since. Any other re-arm seeds a fresh one from the
      // next pair, rendered for what the game was told until this frame.
      resetAdoption(stage_ == NativeTrimStage::Adopting && baselineReady_);
      clearApplied();
      stage_ = NativeTrimStage::Off; forcedInert_=false; route_ = NativeTrimRoute::None;
      changed_ = true;
    } else {
      settings_ = settings;
    }
    if(standDown){stage_=NativeTrimStage::Inert;forcedInert_=true;route_=NativeTrimRoute::None;resetAdoption();changed_=true;return finish();}
    if (stage_ == NativeTrimStage::Live || stage_ == NativeTrimStage::Adopting) {
      if (stage_ == NativeTrimStage::Adopting) ++adoptingFrames_;
      // Promotion is deliberately evaluated from the pair completed in the
      // preceding frame. A pair that is incomplete never becomes canonical.
      // Told before the game read any size, the first complete pair was
      // rendered for this ask, there being no other it could be for: it is
      // the evidence itself, with no baseline to measure it against.
      if (stage_ == NativeTrimStage::Adopting && route_ == NativeTrimRoute::FirstAsk && submittedReady_) {
        adopted_[0]=submitted_[0]; adopted_[1]=submitted_[1]; adoptedReady_=true;
        stage_ = NativeTrimStage::Live; changed_ = true; floor_ = 0;
      }
      if (stage_ == NativeTrimStage::Adopting && baselineReady_ && submittedReady_) {
        // The rebuild is measured against the ask the baseline was rendered
        // for, never against the runtime's size: after a change while live
        // the game sits at the previous ask, and 0.899 x that baseline is a
        // size it will never build. Flight 3 of 2026-09-16 spent two minutes
        // at this stage on exactly that, after outer 10 became 5.
        bool promote = true, moved = false;
        for (unsigned e=0;e<2;++e) {
          const auto ask = recommended(e);
          const float rw = askRatio(ask.width, baselineAsk_[e].width), rh = askRatio(ask.height, baselineAsk_[e].height);
          const bool askMoved = std::fabs(rw-1.0f) > .005f || std::fabs(rh-1.0f) > .005f;
          promote = promote && (!askMoved || changedSize(e)) &&
            reached(submitted_[e].width, baseline_[e].width, rw) &&
            reached(submitted_[e].height, baseline_[e].height, rh);
          moved = moved || changedSize(e);
        }
        if (!promote && moved && adoptingFrames_ > kNativeTrimAdoptGraceFrames) promote = true;
        // The pair reached the ask: the game holds targets for exactly what it
        // is told now, and the nudge's floor starts over from there.
        if (promote) { adopted_[0]=submitted_[0]; adopted_[1]=submitted_[1]; adoptedReady_=true; stage_ = NativeTrimStage::Live; changed_ = true; floor_ = 0; }
      }
      submittedReady_ = false; submittedMask_ = 0;
    }
    if (stage_ == NativeTrimStage::Inert && forcedInert_) return finish();
    if (stage_ == NativeTrimStage::Off || stage_ == NativeTrimStage::Inert) {
      // A trim tells the game exactly the frustum the headset shows, so it is
      // told from the first frame the runtime's frustum and the settings are
      // both known -- on 2026-09-23, 86 ms before the game's first size ask.
      // The floor outlives the trim going off: the game's targets are the
      // graphics settings', not the trim's.
      if (!computeFactors()) { stage_ = NativeTrimStage::Inert; forcedInert_=true; route_ = NativeTrimRoute::None; changed_ = true; return finish(); }
      stage_ = NativeTrimStage::Adopting; changed_ = true;
      submittedReady_ = false; submittedMask_ = 0; adoptingFrames_ = 0;
      route_ = (!sizeAsked && !baselineReady_) ? NativeTrimRoute::FirstAsk : NativeTrimRoute::Rebuild;
      // A kept baseline keeps its ask. A fresh one is rendered for what the
      // game was told during the previous frame or, before any frame has
      // run, for the runtime's own size. A first ask has no baseline and no
      // rebuild behind it for a nudge to measure from: the game will build
      // for what it is told now, and the temporal pass is sized by that.
      if (route_ == NativeTrimRoute::FirstAsk) floor_ = 0;
      else if (!baselineReady_) for (unsigned e=0;e<2;++e)
        pendingAsk_[e] = told_[e].width && told_[e].height ? told_[e] : gameFacingDimensions(runtime_[e]);
      startNudge();
      if (route_ == NativeTrimRoute::FirstAsk) for (unsigned e=0;e<2;++e) pendingAsk_[e] = recommended(e);
    }
    return finish();
  }

  void noteSubmittedSize(unsigned eye, uint32_t width, uint32_t height) noexcept {
    if (eye >= 2 || stage_ != NativeTrimStage::Adopting || !width || !height || width>16384 || height>16384 || (submittedMask_&(1u<<eye))) return;
    submitted_[eye] = {width,height}; submittedMask_ |= 1u << eye;
    if (submittedMask_ != 3) return;
    if (route_ == NativeTrimRoute::FirstAsk) { submittedReady_ = true; return; }
    if (!baselineReady_) {
      baseline_[0]=submitted_[0]; baseline_[1]=submitted_[1];
      baselineAsk_[0]=pendingAsk_[0]; baselineAsk_[1]=pendingAsk_[1];
      baselineReady_ = true; submittedReady_ = false;
    }
    else submittedReady_ = true;
  }

  void standDown() noexcept { pendingStandDown_=true; }
  NativeTrimStage stage() const noexcept { return stage_; }
  NativeTrimRoute route() const noexcept { return route_; }
  bool pending() const noexcept { return stage_ == NativeTrimStage::Adopting; }
  // The factor on the runtime's recommended size, per axis, the largest of
  // the two eyes': below 1, since a trim only narrows.
  float factorWidth() const noexcept { return maxFactorW_; }
  float factorHeight() const noexcept { return maxFactorH_; }
  bool changed() const noexcept { return changed_; }
  // The adoption in progress, for the trace: the pair the baseline was
  // seeded from, the ask that pair was rendered for, the last complete pair
  // seen, and how many frames the stage has waited.
  bool baselineReady() const noexcept { return baselineReady_; }
  NativeTrimDimensions baseline(unsigned eye) const noexcept { return eye<2&&baselineReady_?baseline_[eye]:NativeTrimDimensions{}; }
  NativeTrimDimensions baselineAsk(unsigned eye) const noexcept { return eye<2&&baselineReady_?baselineAsk_[eye]:NativeTrimDimensions{}; }
  NativeTrimDimensions lastSubmitted(unsigned eye) const noexcept { return eye<2?submitted_[eye]:NativeTrimDimensions{}; }
  uint32_t adoptingFrames() const noexcept { return adoptingFrames_; }
  // What the trim actually got, per eye, after the edge and span limits. It
  // can be less than was asked for; that is the only place a clamp shows.
  float appliedOuterDeg(unsigned eye) const noexcept { return eye<2?appliedOuter_[eye]:0.0f; }
  float appliedNasalDeg(unsigned eye) const noexcept { return eye<2?appliedNasal_[eye]:0.0f; }
  float appliedVerticalDeg(unsigned eye) const noexcept { return eye<2?appliedVertical_[eye]:0.0f; }
  bool trimmed() const noexcept {
    for(unsigned e=0;e<2;++e) if(appliedOuter_[e]>0||appliedNasal_[e]>0||appliedVertical_[e]>0) return true;
    return false;
  }
  NativeTrimFrustum runtimeFrustum(unsigned eye) const noexcept { return eye<2?true_[eye]:NativeTrimFrustum{}; }
  // The size the game is asked to render before any nudge: the runtime's own
  // outside an adoption, the trimmed one inside.
  NativeTrimDimensions trueRecommended(unsigned eye) const noexcept {
    if (eye >= 2) return {};
    if (stage_ != NativeTrimStage::Adopting && stage_ != NativeTrimStage::Live)
      return gameFacingDimensions(runtime_[eye]);
    return {roundRecommendedDim(float(runtime_[eye].width)*maxFactorW_),
            roundRecommendedDim(float(runtime_[eye].height)*maxFactorH_)};
  }
  // What the game is told: trueRecommended(), shorter by the nudge from the
  // change that started one until the next change (startNudge). It stays
  // shorter once live, so the temporal output and the target the game built
  // agree to the pixel.
  NativeTrimDimensions recommended(unsigned eye) const noexcept {
    auto d=trueRecommended(eye);
    if(nudgeHeight_&&d.height>nudgeHeight_&&(stage_==NativeTrimStage::Adopting||stage_==NativeTrimStage::Live))d.height=nudgeHeight_;
    return d;
  }
  // The nudge decision taken as this frame entered Adopting, for the trace
  // (None on any other frame), with the floor it was measured against: the
  // lowest height the game has been told since the rebuild last seen.
  NativeTrimNudge nudge() const noexcept { return nudge_; }
  uint32_t nudgeFloor() const noexcept { return nudgeFloor_; }
  // What the frame now in hand was rendered for. While a rebuild is awaited
  // that is the ask the baseline pair was built for (until that pair is
  // seen, what the game was told as the wait began), never the new ask: the
  // temporal pass sizes its output by it, and a frame the game rendered at
  // half of the previous ask, measured against the new one, falls outside
  // NGX's render range (flight 4, 2026-09-16, HMD quality 0.5: "outside
  // every DLSS mode's render range", own history for 14 s). The game itself
  // is still told recommended(). A first ask has only ever been told the
  // one ask, so that is what its frames are rendered for from frame 1.
  NativeTrimDimensions treatedFor(unsigned eye) const noexcept {
    if (eye >= 2) return {};
    if (stage_ == NativeTrimStage::Adopting) {
      const auto& ask = baselineReady_ ? baselineAsk_[eye] : pendingAsk_[eye];
      if (ask.width && ask.height) return ask;
    }
    return recommended(eye);
  }
  // What the game is told about the projection, on either of its two query
  // channels: the trimmed frustum once live, the runtime's own until then.
  NativeTrimFrustum gameFrustum(unsigned eye) const noexcept {
    if (eye >= 2) return {};
    if (stage_ != NativeTrimStage::Live) return true_[eye];
    return target_[eye];
  }
  // Where the trimmed image belongs inside the eye's own full field, which
  // is what the XR layer still advertises. Identity without a trim.
  NativeTrimBounds placementBounds(unsigned eye) const noexcept {
    if (eye >= 2 || stage_ != NativeTrimStage::Live) return {};
    return inside(target_[eye], true_[eye]);
  }
  // The projection the treated image holds.
  NativeTrimFrustum contentFrustum(unsigned eye) const noexcept {
    return eye < 2 && stage_ == NativeTrimStage::Live ? target_[eye] : (eye < 2 ? true_[eye] : NativeTrimFrustum{});
  }
  NativeTrimDimensions canonical(unsigned eye) const noexcept {
    if (eye >= 2 || stage_ != NativeTrimStage::Live || !adoptedReady_) return {};
    return {roundDim(float(adopted_[eye].width)/factorW_[eye]),roundDim(float(adopted_[eye].height)/factorH_[eye])};
  }

 private:
  // The inner frustum expressed as a sub-rectangle of the outer one. v runs
  // down from the outer frustum's up edge, as every texture bound here does.
  static NativeTrimBounds inside(const NativeTrimFrustum& in,const NativeTrimFrustum& out) noexcept {
    const float du=out.right-out.left, dv=out.up-out.down;
    if (!(du>1e-4f&&dv>1e-4f)) return {};
    return {(in.left-out.left)/du,(out.up-in.up)/dv,(in.right-out.left)/du,(out.up-in.down)/dv};
  }
  // Did the game move its render target as far as it was asked to, and in the
  // direction it was asked to? A trimmed ask is met by a target that actually
  // came down to it; a larger one (the game's own quality multiplier may
  // scale the ask far past) by anything at least that size.
  static bool reached(uint32_t submitted,uint32_t baseline,float factor) noexcept {
    const float want=float(baseline)*factor;
    return factor>=1.0f ? float(submitted)>=want*.97f : float(submitted)<=want*1.03f;
  }
  static bool anyTrim(const NativeTrimSettings& s) noexcept {
    return s.trimOuterDeg>0||s.trimNasalDeg>0||s.trimVerticalDeg>0;
  }
  static uint32_t roundDim(float v) noexcept { return v > 0 && std::isfinite(v) && v <= 16384.0f ? uint32_t(std::lround(v)) : 0; }
  // Elite applies HMDRenderTargetMultiplier with integer truncation. An odd
  // recommendation can therefore lose a pixel at quality 0.5 (4857 -> 2428),
  // falling below NGX's strict minimum (2429). This helper is used only for
  // the game-facing recommendation; XR targets and the canonical size retain
  // their existing dimensions and rounding.
  static uint32_t roundRecommendedDim(float v) noexcept {
    if (!(v > 0.0f) || !std::isfinite(v) || v > 16384.0f) return 0;
    return gameFacingDimension(static_cast<uint32_t>(std::lround(v)));
  }
  static bool finiteFrustum(const NativeTrimFrustum& f) noexcept {
    return std::isfinite(f.left)&&std::isfinite(f.right)&&std::isfinite(f.down)&&std::isfinite(f.up)&&
      f.left < f.right && f.down < f.up && f.left >= -20 && f.right <= 20 && f.down >= -20 && f.up <= 20;
  }
  static bool validSettings(const NativeTrimSettings& s) noexcept {
    for (float trim : {s.trimOuterDeg, s.trimNasalDeg, s.trimVerticalDeg})
      if (!std::isfinite(trim) || trim < 0 || trim > 30) return false;
    return true;
  }
  static bool validInputs(const NativeTrimFrustum (&f)[2], const NativeTrimDimensions (&d)[2]) noexcept {
    for (unsigned i=0;i<2;++i) if (!finiteFrustum(f[i])||!d[i].width||!d[i].height||d[i].width>16384||d[i].height>16384) return false; return true;
  }
  static bool sameSettings(const NativeTrimSettings& a,const NativeTrimSettings& b) noexcept {
    return a.trimOuterDeg==b.trimOuterDeg&&a.trimNasalDeg==b.trimNasalDeg&&a.trimVerticalDeg==b.trimVerticalDeg;
  }
  bool sameFrusta(const NativeTrimFrustum (&f)[2]) const noexcept { for(unsigned i=0;i<2;++i)if(std::memcmp(&f[i],&true_[i],sizeof f[i]))return false;return true; }
  bool changedSize(unsigned e) const noexcept { return e<2 && (submitted_[e].width!=baseline_[e].width || submitted_[e].height!=baseline_[e].height); }
  bool sameDims(const NativeTrimDimensions (&d)[2]) const noexcept { return d[0].width==runtime_[0].width&&d[0].height==runtime_[0].height&&d[1].width==runtime_[1].width&&d[1].height==runtime_[1].height; }
  static void copy(const NativeTrimFrustum (&a)[2],NativeTrimFrustum (&b)[2]) noexcept {b[0]=a[0];b[1]=a[1];}
  static void copy(const NativeTrimDimensions (&a)[2],NativeTrimDimensions (&b)[2]) noexcept {b[0]=a[0];b[1]=a[1];}
  // The floor survives the trim going off: the game keeps the targets it
  // has through a taller ask, so the next trim is measured against them.
  void clear() noexcept { resetAdoption(); baselineReady_=false; nudgeHeight_=0; }
  // Flights 4 and 5 (2026-09-16): Elite rebuilds its eye targets on its own
  // only when the height it reads is smaller than the one they were built
  // for (every such change landed in about two seconds); a change on the
  // width alone, or a taller ask, waited for a Graphics-settings apply, 52 s
  // and counting. So a change that leaves the height where it was, or does
  // not lower it under the floor (the lowest height told since the rebuild
  // last seen, re-based at each promotion), is told a height two pixels
  // under that floor, for good: the game rebuilds at once, both axes at the
  // current ask, and what it builds is what the temporal pass is sized by.
  // Each such change costs two more rows until a genuine shrink or an apply
  // re-bases the floor; past kNativeTrimNudgeMaxPx the ask is told as it is
  // and the trace says so. An ask taller than the one the floor was told
  // for is never nudged: the rows asked for are the point of it, a dip would
  // quietly drop them, and the game builds them at the next apply. Only the
  // left eye's height is measured; both eyes are told the dip.
  void startNudge() noexcept {
    nudgeHeight_=0; nudgeFloor_=floor_;
    const auto want=trueRecommended(0); const auto before=trueTold_[0];
    if(!floor_||!want.height||!before.height){nudge_=NativeTrimNudge::First;return;}
    if(want.width==before.width&&want.height==before.height){nudge_=NativeTrimNudge::Same;return;}
    if(want.height<floor_){nudge_=NativeTrimNudge::Shrink;return;}
    if(want.height>floorAsk_){nudge_=NativeTrimNudge::Taller;return;}
    const uint32_t dip=floor_>kNativeTrimNudgePx?floor_-kNativeTrimNudgePx:0;
    if(dip<16||want.height-dip>kNativeTrimNudgeMaxPx){nudge_=NativeTrimNudge::Capped;return;}
    nudgeHeight_=dip;nudge_=NativeTrimNudge::Started;
  }
  void clearApplied() noexcept { for(unsigned e=0;e<2;++e) appliedOuter_[e]=appliedNasal_[e]=appliedVertical_[e]=0; }
  void resetAdoption(bool keepBaseline=false) noexcept {
    if(!keepBaseline) baselineReady_=false;
    submittedReady_=false; adoptedReady_=false; submittedMask_=0; adoptingFrames_=0;
  }
  // How far the game is expected to move an axis: the ask it is given now
  // over the ask its baseline pair was rendered for. An unknown ask expects
  // nothing, and the first pair after the baseline then promotes.
  static float askRatio(uint32_t ask,uint32_t was) noexcept { return ask&&was?float(ask)/float(was):1.0f; }
  // One edge, trimmed inward by `applied` degrees with its sign kept. Zero is
  // the tangent untouched, bit for bit: the identity placement has to be
  // exact, not merely close.
  static float trimmedEdge(float tangent,float applied) noexcept {
    if(!(applied>0))return tangent;
    const float magnitude=nativeTrimDegrees(std::fabs(tangent));
    return (tangent<0?-1.0f:1.0f)*nativeTrimTangent(magnitude-applied);
  }
  static float edgeRoom(float tangent,float asked) noexcept {
    const float room=nativeTrimDegrees(std::fabs(tangent))-kNativeTrimMinEdgeDeg;
    return (!(asked>0)||room<=0)?0.0f:(asked<room?asked:room);
  }
  // Both edges of one axis at once, because the span limit is a property of
  // the pair: when the two asks together would leave too little to see, both
  // are scaled back in proportion rather than one being dropped.
  static void trimAxis(float low,float high,float lowAsk,float highAsk,
                       float& lowOut,float& highOut,float& lowApplied,float& highApplied) noexcept {
    float a=edgeRoom(low,lowAsk), b=edgeRoom(high,highAsk);
    const float span=nativeTrimDegrees(high)-nativeTrimDegrees(low);
    for(unsigned pass=0;pass<2&&(a>0||b>0);++pass) {
      const float lost=span-(nativeTrimDegrees(trimmedEdge(high,b))-nativeTrimDegrees(trimmedEdge(low,a)));
      if(lost<=0||span-lost>=kNativeTrimMinSpanDeg)break;
      const float room=span-kNativeTrimMinSpanDeg;
      const float k=room<=0?0.0f:room/lost;
      a*=k;b*=k;
    }
    lowOut=trimmedEdge(low,a);highOut=trimmedEdge(high,b);
    if(nativeTrimDegrees(highOut)-nativeTrimDegrees(lowOut)<kNativeTrimMinSpanDeg-.01f) {
      a=b=0;lowOut=low;highOut=high; // an axis the limits cannot serve keeps its edges
    }
    lowApplied=a;highApplied=b;
  }
  // The trimmed true frustum: what the headset will be shown, and what the
  // game is asked to render.
  bool computeTarget() noexcept {
    clearApplied();
    if(!anyTrim(settings_)) { copy(true_,target_); return true; }
    for(unsigned e=0;e<2;++e) {
      const auto& t=true_[e]; auto& g=target_[e]; g=t;
      // The outer edge is the temple side: the wider of the two horizontal
      // tangents on every headset seen here (eye 0's left, eye 1's right).
      const bool outerIsLeft=std::fabs(t.left)>=std::fabs(t.right);
      float lowApplied=0,highApplied=0,downApplied=0,upApplied=0;
      trimAxis(t.left,t.right,outerIsLeft?settings_.trimOuterDeg:settings_.trimNasalDeg,
               outerIsLeft?settings_.trimNasalDeg:settings_.trimOuterDeg,g.left,g.right,lowApplied,highApplied);
      trimAxis(t.down,t.up,settings_.trimVerticalDeg,settings_.trimVerticalDeg,g.down,g.up,downApplied,upApplied);
      appliedOuter_[e]=outerIsLeft?lowApplied:highApplied;
      appliedNasal_[e]=outerIsLeft?highApplied:lowApplied;
      // The two vertical edges can clamp differently on an asymmetric eye;
      // report the one that got least, so a clamp is never hidden.
      appliedVertical_[e]=(std::min)(downApplied,upApplied);
      if(!finiteFrustum(g))return false;
    }
    return true;
  }
  bool computeFactors() noexcept {
    if(!computeTarget())return false;
    for(unsigned e=0;e<2;++e) {
      const auto& t=target_[e]; const auto& raw=true_[e];
      // The factors are measured against the RUNTIME's frustum, because they
      // price the render target: a trim makes them smaller than 1.
      factorW_[e]=(t.right-t.left)/(raw.right-raw.left);factorH_[e]=(t.up-t.down)/(raw.up-raw.down);
      if(!std::isfinite(factorW_[e])||!std::isfinite(factorH_[e])||factorW_[e]>4||factorH_[e]>4||factorW_[e]<=0||factorH_[e]<=0)return false;
    }
    maxFactorW_=std::max(factorW_[0],factorW_[1]);maxFactorH_=std::max(factorH_[0],factorH_[1]);
    // A pixel count within one percent of what the runtime asked for, either
    // way, is not worth a render-target rebuild.
    return (std::fabs(maxFactorW_-1.0f)>=.01f||std::fabs(maxFactorH_-1.0f)>=.01f) &&
      roundRecommendedDim(float(runtime_[0].width)*maxFactorW_) && roundRecommendedDim(float(runtime_[0].height)*maxFactorH_) &&
      roundRecommendedDim(float(runtime_[1].width)*maxFactorW_) && roundRecommendedDim(float(runtime_[1].height)*maxFactorH_);
  }
  NativeTrimSettings settings_{}; NativeTrimFrustum true_[2]{},target_[2]{}; NativeTrimDimensions runtime_[2]{},baseline_[2]{},submitted_[2]{},adopted_[2]{};
  // baselineAsk_ is what the baseline pair was rendered for, pendingAsk_ the
  // same for the pair about to seed one, told_ what the game was told at the
  // end of the previous frame.
  NativeTrimDimensions baselineAsk_[2]{},pendingAsk_[2]{},told_[2]{},trueTold_[2]{}; uint32_t adoptingFrames_=0;
  // The nudge: the lowest height told since the rebuild last seen and the
  // height it stood for before any dip, the height told while a nudge is on
  // (0 = none), and the decision taken as the frame entered Adopting with
  // the floor it saw.
  uint32_t floor_=0,floorAsk_=0,nudgeHeight_=0,nudgeFloor_=0; NativeTrimNudge nudge_=NativeTrimNudge::None;
  float factorW_[2]{1,1},factorH_[2]{1,1},maxFactorW_=1,maxFactorH_=1;
  float appliedOuter_[2]{0,0},appliedNasal_[2]{0,0},appliedVertical_[2]{0,0};
  uint64_t referenceGeneration_=0; uint32_t submittedMask_=0; NativeTrimStage stage_=NativeTrimStage::Off; NativeTrimRoute route_=NativeTrimRoute::None; bool baselineReady_=false,submittedReady_=false,adoptedReady_=false,forcedInert_=false,pendingStandDown_=false,changed_=false;
};
}
