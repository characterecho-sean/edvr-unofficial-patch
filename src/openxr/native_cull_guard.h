#pragma once
#include "game_render_size.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <cstring>

namespace edvr::openxr {

enum class NativeCullMode : uint32_t { Off = 0, Symmetric = 1, Percent = 2 };
enum class NativeCullStage : uint32_t { Off, WaitingScene, Adopting, Live, Inert };

struct NativeCullSignature { uint32_t horizontal = 0, vertical = 0; };

struct NativeCullSettings {
  NativeCullMode mode = NativeCullMode::Off;
  float percent = 8.0f;
  float horizontalFraction = 1.0f;
  float verticalFraction = 1.0f;
  uint32_t signatureCount = 0;
  std::array<NativeCullSignature, 8> signatures{};
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

struct NativeCullFrustum { float left=0, right=0, down=0, up=0; };
struct NativeCullDimensions { uint32_t width=0, height=0; };
struct NativeCullBounds { float left=0, top=0, right=1, bottom=1; };

// The trim's own limits, in degrees. No edge is brought closer than
// kNativeCullMinEdgeDeg to straight ahead and no axis is left narrower than
// kNativeCullMinSpanDeg, whatever is asked for: a trim that closed an eye
// would look like a broken runtime rather than a setting.
constexpr float kNativeCullMinEdgeDeg = 5.0f;
constexpr float kNativeCullMinSpanDeg = 20.0f;
// Frames of adoption after which a target that has moved at all is taken as
// the rebuild although no ratio matched. The game's own quality multiplier
// can change underneath the ask (0.65 to 0.5 and back within one adoption in
// the flight of 2026-09-16), and then no ratio ever matches; the lie is a
// projection, which any target size carries correctly. Three seconds at 90 Hz.
constexpr uint32_t kNativeCullAdoptGraceFrames = 270;
constexpr float kNativeCullPi = 3.1415926535f;

inline float nativeCullDegrees(float tangent) noexcept {
  return std::atan(tangent) * 180.0f / kNativeCullPi;
}
inline float nativeCullTangent(float degrees) noexcept {
  return std::tan(degrees * kNativeCullPi / 180.0f);
}

// Elite applies its HMD quality multiplier with integer truncation. Keep the
// game-facing recommendation even on both axes so quality 0.5 cannot lose a
// pixel from an odd runtime or widened size. The XR swapchain dimensions stay
// in the host's raw `sizes` records; this helper is only for game geometry.
inline NativeCullDimensions gameFacingDimensions(NativeCullDimensions v) noexcept {
  return {gameFacingDimension(v.width), gameFacingDimension(v.height)};
}

// Owner-thread policy. It does no OpenXR, graphics, or configuration I/O.
class NativeCullGuard final {
 public:
  NativeCullStage beginFrame(const NativeCullSettings& settings,
      const NativeCullFrustum (&trueFrusta)[2],
      const NativeCullDimensions (&runtimeDims)[2], bool sceneReady,
      uint64_t referenceGeneration) noexcept {
    changed_ = false;
    const bool standDown=pendingStandDown_;pendingStandDown_=false;
    // Every exit records what the game is told from here on, so the next
    // adoption knows what size the pair that seeds its baseline was rendered
    // for.
    const auto finish=[this]{told_[0]=recommended(0);told_[1]=recommended(1);return stage_;};
    const bool valid = validSettings(settings) && validInputs(trueFrusta, runtimeDims);
    // A trim alone is reason enough to run: it tells the game a different
    // projection and a different size, which is the whole of what the stage
    // machine exists to land safely.
    if (!valid || (settings.mode == NativeCullMode::Off && !anyTrim(settings))) {
      settings_=settings; referenceGeneration_=referenceGeneration;
      if (validInputs(trueFrusta,runtimeDims)) { copy(trueFrusta, true_); copy(runtimeDims, runtime_); copy(true_, target_); }
      if (stage_ != NativeCullStage::Off) changed_ = true;
      clear();maxFactorW_=maxFactorH_=1;maxWidenW_=maxWidenH_=1;clearApplied();
      forcedInert_=false; stage_ = NativeCullStage::Off; return finish();
    }
    const bool configChanged = !sameSettings(settings, settings_);
    // Recenter moves the reference space, not the optical projection or the
    // game's render targets. Re-arming adoption there would seed the already
    // enlarged targets as a new baseline and wait for a second enlargement.
    const bool geometryChanged = !sameFrusta(trueFrusta) || !sameDims(runtimeDims);
    referenceGeneration_ = referenceGeneration;
    if (configChanged || geometryChanged) {
      settings_ = settings; copy(trueFrusta, true_); copy(runtimeDims, runtime_); copy(true_, target_);
      referenceGeneration_ = referenceGeneration;
      // A change while a rebuild is in flight keeps the baseline: it is the
      // last pair known to have been rendered for a known ask, whether or not
      // the game has moved since. Any other re-arm seeds a fresh one from the
      // next pair, rendered for what the game was told until this frame.
      resetAdoption(stage_ == NativeCullStage::Adopting && baselineReady_);
      clearApplied();
      stage_ = NativeCullStage::Off; forcedInert_=false;
      changed_ = true;
    } else {
      settings_ = settings;
    }
    if(standDown){stage_=NativeCullStage::Inert;forcedInert_=true;resetAdoption();changed_=true;return finish();}
    if (stage_ == NativeCullStage::Live || stage_ == NativeCullStage::Adopting) {
      if (stage_ == NativeCullStage::Adopting) ++adoptingFrames_;
      // Promotion is deliberately evaluated from the pair completed in the
      // preceding frame. A pair that is incomplete never becomes canonical.
      if (stage_ == NativeCullStage::Adopting && baselineReady_ && submittedReady_) {
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
        if (!promote && moved && adoptingFrames_ > kNativeCullAdoptGraceFrames) promote = true;
        if (promote) { adopted_[0]=submitted_[0]; adopted_[1]=submitted_[1]; adoptedReady_=true; stage_ = NativeCullStage::Live; changed_ = true; }
      }
      submittedReady_ = false; submittedMask_ = 0;
    }
    if (stage_ == NativeCullStage::Inert && forcedInert_) return finish();
    if (stage_ == NativeCullStage::Off || stage_ == NativeCullStage::WaitingScene ||
        stage_ == NativeCullStage::Inert) {
      if (!sceneReady) { stage_ = NativeCullStage::WaitingScene; return finish(); }
      if (!computeWidened()) { stage_ = NativeCullStage::Inert; forcedInert_=true; changed_ = true; return finish(); }
      stage_ = NativeCullStage::Adopting; changed_ = true;
      submittedReady_ = false; submittedMask_ = 0; adoptingFrames_ = 0;
      // A kept baseline keeps its ask. A fresh one is rendered for what the
      // game was told during the previous frame or, before any frame has
      // run, for the runtime's own size.
      if (!baselineReady_) for (unsigned e=0;e<2;++e)
        pendingAsk_[e] = told_[e].width && told_[e].height ? told_[e] : gameFacingDimensions(runtime_[e]);
    }
    return finish();
  }

  void noteSubmittedSize(unsigned eye, uint32_t width, uint32_t height) noexcept {
    if (eye >= 2 || stage_ != NativeCullStage::Adopting || !width || !height || width>16384 || height>16384 || (submittedMask_&(1u<<eye))) return;
    submitted_[eye] = {width,height}; submittedMask_ |= 1u << eye;
    if (submittedMask_ != 3) return;
    if (!baselineReady_) {
      baseline_[0]=submitted_[0]; baseline_[1]=submitted_[1];
      baselineAsk_[0]=pendingAsk_[0]; baselineAsk_[1]=pendingAsk_[1];
      baselineReady_ = true; submittedReady_ = false;
    }
    else submittedReady_ = true;
  }

  void standDown() noexcept { pendingStandDown_=true; }
  NativeCullStage stage() const noexcept { return stage_; }
  bool pending() const noexcept { return stage_ == NativeCullStage::WaitingScene || stage_ == NativeCullStage::Adopting; }
  float factorWidth() const noexcept { return maxFactorW_; }
  float factorHeight() const noexcept { return maxFactorH_; }
  // The widening alone, target span -> lied span, never below 1. The channel
  // to the graphics half carries per-mille ABOVE 1.0 and cannot express a
  // shrink, so it is told what was added, not the net of add and trim.
  float widenFactorWidth() const noexcept { return maxWidenW_; }
  float widenFactorHeight() const noexcept { return maxWidenH_; }
  bool changed() const noexcept { return changed_; }
  // The adoption in progress, for the trace: the pair the baseline was
  // seeded from, the ask that pair was rendered for, the last complete pair
  // seen, and how many frames the stage has waited.
  bool baselineReady() const noexcept { return baselineReady_; }
  NativeCullDimensions baseline(unsigned eye) const noexcept { return eye<2&&baselineReady_?baseline_[eye]:NativeCullDimensions{}; }
  NativeCullDimensions baselineAsk(unsigned eye) const noexcept { return eye<2&&baselineReady_?baselineAsk_[eye]:NativeCullDimensions{}; }
  NativeCullDimensions lastSubmitted(unsigned eye) const noexcept { return eye<2?submitted_[eye]:NativeCullDimensions{}; }
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
  NativeCullFrustum runtimeFrustum(unsigned eye) const noexcept { return eye<2?true_[eye]:NativeCullFrustum{}; }
  NativeCullDimensions recommended(unsigned eye) const noexcept {
    if (eye >= 2) return {};
    if (stage_ != NativeCullStage::Adopting && stage_ != NativeCullStage::Live)
      return gameFacingDimensions(runtime_[eye]);
    return {roundRecommendedDim(float(runtime_[eye].width)*maxFactorW_),
            roundRecommendedDim(float(runtime_[eye].height)*maxFactorH_)};
  }
  // What the frame now in hand was rendered for. While a rebuild is awaited
  // that is the ask the baseline pair was built for (until that pair is
  // seen, what the game was told as the wait began), never the new ask: the
  // temporal pass sizes its output by it, and a frame the game rendered at
  // half of the previous ask, measured against the new one, falls outside
  // NGX's render range (flight 4, 2026-09-16, HMD quality 0.5: "outside
  // every DLSS mode's render range", own history for 14 s). The game itself
  // is still told recommended().
  NativeCullDimensions treatedFor(unsigned eye) const noexcept {
    if (eye >= 2) return {};
    if (stage_ == NativeCullStage::Adopting) {
      const auto& ask = baselineReady_ ? baselineAsk_[eye] : pendingAsk_[eye];
      if (ask.width && ask.height) return ask;
    }
    return recommended(eye);
  }
  NativeCullFrustum gameFrustum(unsigned eye) const noexcept {
    return eye < 2 && stage_ == NativeCullStage::Live ? lied_[eye] : (eye < 2 ? true_[eye] : NativeCullFrustum{});
  }
  // What the headset actually shows, inside what the game was asked to
  // render: the crop taken at submit. With a trim and no widening the two
  // are the same frustum and this is exactly the whole image.
  NativeCullBounds cropBounds(unsigned eye) const noexcept {
    if (eye >= 2 || stage_ != NativeCullStage::Live) return {};
    return inside(target_[eye], lied_[eye]);
  }
  // Where that cropped image belongs inside the eye's own full field, which
  // is what the XR layer still advertises. Identity without a trim.
  NativeCullBounds placementBounds(unsigned eye) const noexcept {
    if (eye >= 2 || stage_ != NativeCullStage::Live) return {};
    return inside(target_[eye], true_[eye]);
  }
  // The projection the treated image holds once the crop has been taken.
  NativeCullFrustum contentFrustum(unsigned eye) const noexcept {
    return eye < 2 && stage_ == NativeCullStage::Live ? target_[eye] : (eye < 2 ? true_[eye] : NativeCullFrustum{});
  }
  NativeCullDimensions canonical(unsigned eye) const noexcept {
    if (eye >= 2 || stage_ != NativeCullStage::Live || !adoptedReady_) return {};
    return {roundDim(float(adopted_[eye].width)/factorW_[eye]),roundDim(float(adopted_[eye].height)/factorH_[eye])};
  }

 private:
  // The inner frustum expressed as a sub-rectangle of the outer one. v runs
  // down from the outer frustum's up edge, as every texture bound here does.
  static NativeCullBounds inside(const NativeCullFrustum& in,const NativeCullFrustum& out) noexcept {
    const float du=out.right-out.left, dv=out.up-out.down;
    if (!(du>1e-4f&&dv>1e-4f)) return {};
    return {(in.left-out.left)/du,(out.up-in.up)/dv,(in.right-out.left)/du,(out.up-in.down)/dv};
  }
  // Did the game move its render target as far as it was asked to, and in the
  // direction it was asked to? A widened ask is met by anything at least the
  // widened size (the game's own quality multiplier may scale it far past);
  // a trimmed ask is met by a target that actually came down to it.
  static bool reached(uint32_t submitted,uint32_t baseline,float factor) noexcept {
    const float want=float(baseline)*factor;
    return factor>=1.0f ? float(submitted)>=want*.97f : float(submitted)<=want*1.03f;
  }
  static bool anyTrim(const NativeCullSettings& s) noexcept {
    return s.trimOuterDeg>0||s.trimNasalDeg>0||s.trimVerticalDeg>0;
  }
  static uint32_t roundDim(float v) noexcept { return v > 0 && std::isfinite(v) && v <= 16384.0f ? uint32_t(std::lround(v)) : 0; }
  // Elite applies HMDRenderTargetMultiplier with integer truncation. An odd
  // widened recommendation can therefore lose a pixel at quality 0.5
  // (4857 -> 2428), falling below NGX's strict minimum (2429). This helper
  // is used only for the game-facing recommendation; XR targets and the
  // canonical crop retain their existing dimensions and rounding.
  static uint32_t roundRecommendedDim(float v) noexcept {
    if (!(v > 0.0f) || !std::isfinite(v) || v > 16384.0f) return 0;
    return gameFacingDimension(static_cast<uint32_t>(std::lround(v)));
  }
  static bool finiteFrustum(const NativeCullFrustum& f) noexcept {
    return std::isfinite(f.left)&&std::isfinite(f.right)&&std::isfinite(f.down)&&std::isfinite(f.up)&&
      f.left < f.right && f.down < f.up && f.left >= -20 && f.right <= 20 && f.down >= -20 && f.up <= 20;
  }
  static bool validSettings(const NativeCullSettings& s) noexcept {
    for (float trim : {s.trimOuterDeg, s.trimNasalDeg, s.trimVerticalDeg})
      if (!std::isfinite(trim) || trim < 0 || trim > 30) return false;
    if (s.mode == NativeCullMode::Off) return true;
    if (s.mode != NativeCullMode::Symmetric && s.mode != NativeCullMode::Percent) return false;
    if (!std::isfinite(s.percent)||!std::isfinite(s.horizontalFraction)||!std::isfinite(s.verticalFraction)||
        s.percent < 0 || s.percent > 50 || s.horizontalFraction < 0 || s.horizontalFraction > 1 ||
        s.verticalFraction < 0 || s.verticalFraction > 1 || s.signatureCount > 8) return false;
    for (uint32_t i=0;i<s.signatureCount;++i) if (!s.signatures[i].horizontal||!s.signatures[i].vertical) return false;
    return true;
  }
  static bool validInputs(const NativeCullFrustum (&f)[2], const NativeCullDimensions (&d)[2]) noexcept {
    for (unsigned i=0;i<2;++i) if (!finiteFrustum(f[i])||!d[i].width||!d[i].height||d[i].width>16384||d[i].height>16384) return false; return true;
  }
  static bool sameSettings(const NativeCullSettings& a,const NativeCullSettings& b) noexcept {
    if (a.trimOuterDeg!=b.trimOuterDeg||a.trimNasalDeg!=b.trimNasalDeg||a.trimVerticalDeg!=b.trimVerticalDeg)return false;
    if (a.mode!=b.mode||a.percent!=b.percent||a.horizontalFraction!=b.horizontalFraction||a.verticalFraction!=b.verticalFraction||a.signatureCount!=b.signatureCount)return false;
    for(uint32_t i=0;i<a.signatureCount;++i)if(a.signatures[i].horizontal!=b.signatures[i].horizontal||a.signatures[i].vertical!=b.signatures[i].vertical)return false;return true;
  }
  bool sameFrusta(const NativeCullFrustum (&f)[2]) const noexcept { for(unsigned i=0;i<2;++i)if(std::memcmp(&f[i],&true_[i],sizeof f[i]))return false;return true; }
  bool changedSize(unsigned e) const noexcept { return e<2 && (submitted_[e].width!=baseline_[e].width || submitted_[e].height!=baseline_[e].height); }
  bool sameDims(const NativeCullDimensions (&d)[2]) const noexcept { return d[0].width==runtime_[0].width&&d[0].height==runtime_[0].height&&d[1].width==runtime_[1].width&&d[1].height==runtime_[1].height; }
  static void copy(const NativeCullFrustum (&a)[2],NativeCullFrustum (&b)[2]) noexcept {b[0]=a[0];b[1]=a[1];}
  static void copy(const NativeCullDimensions (&a)[2],NativeCullDimensions (&b)[2]) noexcept {b[0]=a[0];b[1]=a[1];}
  void clear() noexcept { resetAdoption(); baselineReady_=false; }
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
  // the tangent untouched, bit for bit: the identity crop and the identity
  // placement have to be exact, not merely close.
  static float trimmedEdge(float tangent,float applied) noexcept {
    if(!(applied>0))return tangent;
    const float magnitude=nativeCullDegrees(std::fabs(tangent));
    return (tangent<0?-1.0f:1.0f)*nativeCullTangent(magnitude-applied);
  }
  static float edgeRoom(float tangent,float asked) noexcept {
    const float room=nativeCullDegrees(std::fabs(tangent))-kNativeCullMinEdgeDeg;
    return (!(asked>0)||room<=0)?0.0f:(asked<room?asked:room);
  }
  // Both edges of one axis at once, because the span limit is a property of
  // the pair: when the two asks together would leave too little to see, both
  // are scaled back in proportion rather than one being dropped.
  static void trimAxis(float low,float high,float lowAsk,float highAsk,
                       float& lowOut,float& highOut,float& lowApplied,float& highApplied) noexcept {
    float a=edgeRoom(low,lowAsk), b=edgeRoom(high,highAsk);
    const float span=nativeCullDegrees(high)-nativeCullDegrees(low);
    for(unsigned pass=0;pass<2&&(a>0||b>0);++pass) {
      const float lost=span-(nativeCullDegrees(trimmedEdge(high,b))-nativeCullDegrees(trimmedEdge(low,a)));
      if(lost<=0||span-lost>=kNativeCullMinSpanDeg)break;
      const float room=span-kNativeCullMinSpanDeg;
      const float k=room<=0?0.0f:room/lost;
      a*=k;b*=k;
    }
    lowOut=trimmedEdge(low,a);highOut=trimmedEdge(high,b);
    if(nativeCullDegrees(highOut)-nativeCullDegrees(lowOut)<kNativeCullMinSpanDeg-.01f) {
      a=b=0;lowOut=low;highOut=high; // an axis the limits cannot serve keeps its edges
    }
    lowApplied=a;highApplied=b;
  }
  // The trimmed true frustum: what the headset will be shown, and what the
  // game is asked to render before any widening is added back on top.
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
  bool computeWidened() noexcept {
    if(!computeTarget())return false;
    for(unsigned e=0;e<2;++e) {
      const auto& t=target_[e]; auto& l=lied_[e]; const auto& raw=true_[e];
      // The signature names the headset, so it is read off the runtime's own
      // frustum, never the trimmed one -- and only where a mode is running.
      if(e==0 && settings_.signatureCount && settings_.mode!=NativeCullMode::Off) {
        const auto h=uint32_t(std::lround((std::atan(std::fabs(raw.left))*180.0f/3.1415926535f)+(std::atan(std::fabs(raw.right))*180.0f/3.1415926535f)));
        const auto v=uint32_t(std::lround((std::atan(std::fabs(raw.down))*180.0f/3.1415926535f)+(std::atan(std::fabs(raw.up))*180.0f/3.1415926535f)));
        bool match=false;for(uint32_t i=0;i<settings_.signatureCount;++i)match|=settings_.signatures[i].horizontal==h&&settings_.signatures[i].vertical==v;if(!match)return false;
      }
      auto extend=[](float n,float p,float f,float& on,float& op){const float m=std::max(std::fabs(n),std::fabs(p));on=n-(std::fabs(n)<m?f*(m-std::fabs(n)):0);op=p+(std::fabs(p)<m?f*(m-std::fabs(p)):0);};
      if(settings_.mode==NativeCullMode::Off)l=t; // a trim on its own asks for no margin at all
      else if(settings_.mode==NativeCullMode::Percent){const float f=1+settings_.percent/100;l={t.left*f,t.right*f,t.down*f,t.up*f};}
      else {extend(t.left,t.right,settings_.horizontalFraction,l.left,l.right);extend(t.down,t.up,settings_.verticalFraction,l.down,l.up);}
      if(!(l.left<=t.left&&l.right>=t.right&&l.down<=t.down&&l.up>=t.up&&finiteFrustum(l)))return false;
      // The factors are measured against the RUNTIME's frustum, because they
      // price the render target: a trim makes them smaller than 1.
      factorW_[e]=(l.right-l.left)/(raw.right-raw.left);factorH_[e]=(l.up-l.down)/(raw.up-raw.down);
      if(!std::isfinite(factorW_[e])||!std::isfinite(factorH_[e])||factorW_[e]>4||factorH_[e]>4||factorW_[e]<=0||factorH_[e]<=0)return false;
      widenW_[e]=std::max(1.0f,(l.right-l.left)/(t.right-t.left));widenH_[e]=std::max(1.0f,(l.up-l.down)/(t.up-t.down));
      if(!std::isfinite(widenW_[e])||!std::isfinite(widenH_[e]))return false;
    }
    maxFactorW_=std::max(factorW_[0],factorW_[1]);maxFactorH_=std::max(factorH_[0],factorH_[1]);
    maxWidenW_=std::max(widenW_[0],widenW_[1]);maxWidenH_=std::max(widenH_[0],widenH_[1]);
    // A pixel count within one percent of what the runtime asked for, either
    // way, is not worth a render-target rebuild.
    return (std::fabs(maxFactorW_-1.0f)>=.01f||std::fabs(maxFactorH_-1.0f)>=.01f) &&
      roundRecommendedDim(float(runtime_[0].width)*maxFactorW_) && roundRecommendedDim(float(runtime_[0].height)*maxFactorH_) &&
      roundRecommendedDim(float(runtime_[1].width)*maxFactorW_) && roundRecommendedDim(float(runtime_[1].height)*maxFactorH_);
  }
  NativeCullSettings settings_{}; NativeCullFrustum true_[2]{},target_[2]{},lied_[2]{}; NativeCullDimensions runtime_[2]{},baseline_[2]{},submitted_[2]{},adopted_[2]{};
  // baselineAsk_ is what the baseline pair was rendered for, pendingAsk_ the
  // same for the pair about to seed one, told_ what the game was told at the
  // end of the previous frame.
  NativeCullDimensions baselineAsk_[2]{},pendingAsk_[2]{},told_[2]{}; uint32_t adoptingFrames_=0;
  float factorW_[2]{1,1},factorH_[2]{1,1},widenW_[2]{1,1},widenH_[2]{1,1},maxFactorW_=1,maxFactorH_=1,maxWidenW_=1,maxWidenH_=1;
  float appliedOuter_[2]{0,0},appliedNasal_[2]{0,0},appliedVertical_[2]{0,0};
  uint64_t referenceGeneration_=0; uint32_t submittedMask_=0; NativeCullStage stage_=NativeCullStage::Off; bool baselineReady_=false,submittedReady_=false,adoptedReady_=false,forcedInert_=false,pendingStandDown_=false,changed_=false;
};
}
