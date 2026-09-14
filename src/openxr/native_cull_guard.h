#pragma once

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
};

struct NativeCullFrustum { float left=0, right=0, down=0, up=0; };
struct NativeCullDimensions { uint32_t width=0, height=0; };
struct NativeCullBounds { float left=0, top=0, right=1, bottom=1; };

// Owner-thread policy. It does no OpenXR, graphics, or configuration I/O.
class NativeCullGuard final {
 public:
  NativeCullStage beginFrame(const NativeCullSettings& settings,
      const NativeCullFrustum (&trueFrusta)[2],
      const NativeCullDimensions (&runtimeDims)[2], bool sceneReady,
      uint64_t referenceGeneration) noexcept {
    changed_ = false;
    const bool standDown=pendingStandDown_;pendingStandDown_=false;
    const bool valid = validSettings(settings) && validInputs(trueFrusta, runtimeDims);
    if (!valid || settings.mode == NativeCullMode::Off) {
      settings_=settings; referenceGeneration_=referenceGeneration;
      if (validInputs(trueFrusta,runtimeDims)) { copy(trueFrusta, true_); copy(runtimeDims, runtime_); }
      if (stage_ != NativeCullStage::Off) changed_ = true;
      clear();maxFactorW_=maxFactorH_=1; forcedInert_=false; stage_ = NativeCullStage::Off; return stage_;
    }
    const bool configChanged = !sameSettings(settings, settings_);
    // Recenter moves the reference space, not the optical projection or the
    // game's render targets. Re-arming adoption there would seed the already
    // enlarged targets as a new baseline and wait for a second enlargement.
    const bool geometryChanged = !sameFrusta(trueFrusta) || !sameDims(runtimeDims);
    referenceGeneration_ = referenceGeneration;
    if (configChanged || geometryChanged) {
      settings_ = settings; copy(trueFrusta, true_); copy(runtimeDims, runtime_);
      referenceGeneration_ = referenceGeneration; resetAdoption(); stage_ = NativeCullStage::Off; forcedInert_=false;
      changed_ = true;
    } else {
      settings_ = settings;
    }
    if(standDown){stage_=NativeCullStage::Inert;forcedInert_=true;resetAdoption();changed_=true;return stage_;}
    if (stage_ == NativeCullStage::Live || stage_ == NativeCullStage::Adopting) {
      // Promotion is deliberately evaluated from the pair completed in the
      // preceding frame. A pair that is incomplete never becomes canonical.
      if (stage_ == NativeCullStage::Adopting && baselineReady_ && submittedReady_) {
        bool promote = true;
        for (unsigned e=0;e<2;++e) {
          promote = promote && changedSize(e) &&
            float(submitted_[e].width) >= float(baseline_[e].width)*maxFactorW_*.97f &&
            float(submitted_[e].height) >= float(baseline_[e].height)*maxFactorH_*.97f;
        }
        if (promote) { adopted_[0]=submitted_[0]; adopted_[1]=submitted_[1]; adoptedReady_=true; stage_ = NativeCullStage::Live; changed_ = true; }
      }
      submittedReady_ = false; submittedMask_ = 0;
    }
    if (stage_ == NativeCullStage::Inert && forcedInert_) return stage_;
    if (stage_ == NativeCullStage::Off || stage_ == NativeCullStage::WaitingScene ||
        stage_ == NativeCullStage::Inert) {
      if (!sceneReady) { stage_ = NativeCullStage::WaitingScene; return stage_; }
      if (!computeWidened()) { stage_ = NativeCullStage::Inert; forcedInert_=true; changed_ = true; return stage_; }
      stage_ = NativeCullStage::Adopting; changed_ = true;
      baselineReady_ = false; submittedReady_ = false; submittedMask_ = 0;
    }
    return stage_;
  }

  void noteSubmittedSize(unsigned eye, uint32_t width, uint32_t height) noexcept {
    if (eye >= 2 || stage_ != NativeCullStage::Adopting || !width || !height || width>16384 || height>16384 || (submittedMask_&(1u<<eye))) return;
    submitted_[eye] = {width,height}; submittedMask_ |= 1u << eye;
    if (submittedMask_ != 3) return;
    if (!baselineReady_) { baseline_[0]=submitted_[0]; baseline_[1]=submitted_[1]; baselineReady_ = true; submittedReady_ = false; }
    else submittedReady_ = true;
  }

  void standDown() noexcept { pendingStandDown_=true; }
  NativeCullStage stage() const noexcept { return stage_; }
  bool pending() const noexcept { return stage_ == NativeCullStage::WaitingScene || stage_ == NativeCullStage::Adopting; }
  float factorWidth() const noexcept { return maxFactorW_; }
  float factorHeight() const noexcept { return maxFactorH_; }
  bool changed() const noexcept { return changed_; }
  NativeCullDimensions recommended(unsigned eye) const noexcept {
    if (eye >= 2) return {};
    if (stage_ != NativeCullStage::Adopting && stage_ != NativeCullStage::Live) return runtime_[eye];
    return {roundDim(float(runtime_[eye].width)*maxFactorW_), roundDim(float(runtime_[eye].height)*maxFactorH_)};
  }
  NativeCullFrustum gameFrustum(unsigned eye) const noexcept {
    return eye < 2 && stage_ == NativeCullStage::Live ? lied_[eye] : (eye < 2 ? true_[eye] : NativeCullFrustum{});
  }
  NativeCullBounds cropBounds(unsigned eye) const noexcept {
    if (eye >= 2 || stage_ != NativeCullStage::Live) return {};
    const auto& t=true_[eye]; const auto& l=lied_[eye];
    const float du=l.right-l.left, dv=l.up-l.down;
    if (!(du>1e-4f&&dv>1e-4f)) return {};
    return {(t.left-l.left)/du,(l.up-t.up)/dv,(t.right-l.left)/du,(l.up-t.down)/dv};
  }
  NativeCullDimensions canonical(unsigned eye) const noexcept {
    if (eye >= 2 || stage_ != NativeCullStage::Live || !adoptedReady_) return {};
    return {roundDim(float(adopted_[eye].width)/factorW_[eye]),roundDim(float(adopted_[eye].height)/factorH_[eye])};
  }

 private:
  static uint32_t roundDim(float v) noexcept { return v > 0 && std::isfinite(v) && v <= 16384.0f ? uint32_t(std::lround(v)) : 0; }
  static bool finiteFrustum(const NativeCullFrustum& f) noexcept {
    return std::isfinite(f.left)&&std::isfinite(f.right)&&std::isfinite(f.down)&&std::isfinite(f.up)&&
      f.left < f.right && f.down < f.up && f.left >= -20 && f.right <= 20 && f.down >= -20 && f.up <= 20;
  }
  static bool validSettings(const NativeCullSettings& s) noexcept {
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
    if (a.mode!=b.mode||a.percent!=b.percent||a.horizontalFraction!=b.horizontalFraction||a.verticalFraction!=b.verticalFraction||a.signatureCount!=b.signatureCount)return false;
    for(uint32_t i=0;i<a.signatureCount;++i)if(a.signatures[i].horizontal!=b.signatures[i].horizontal||a.signatures[i].vertical!=b.signatures[i].vertical)return false;return true;
  }
  bool sameFrusta(const NativeCullFrustum (&f)[2]) const noexcept { for(unsigned i=0;i<2;++i)if(std::memcmp(&f[i],&true_[i],sizeof f[i]))return false;return true; }
  bool changedSize(unsigned e) const noexcept { return e<2 && (submitted_[e].width!=baseline_[e].width || submitted_[e].height!=baseline_[e].height); }
  bool sameDims(const NativeCullDimensions (&d)[2]) const noexcept { return d[0].width==runtime_[0].width&&d[0].height==runtime_[0].height&&d[1].width==runtime_[1].width&&d[1].height==runtime_[1].height; }
  static void copy(const NativeCullFrustum (&a)[2],NativeCullFrustum (&b)[2]) noexcept {b[0]=a[0];b[1]=a[1];}
  static void copy(const NativeCullDimensions (&a)[2],NativeCullDimensions (&b)[2]) noexcept {b[0]=a[0];b[1]=a[1];}
  void clear() noexcept { resetAdoption(); baselineReady_=false; }
  void resetAdoption() noexcept { baselineReady_=false; submittedReady_=false; adoptedReady_=false; submittedMask_=0; }
  bool computeWidened() noexcept {
    for(unsigned e=0;e<2;++e) {
      const auto& t=true_[e]; auto& l=lied_[e];
      if(e==0 && settings_.signatureCount) {
        const auto h=uint32_t(std::lround((std::atan(std::fabs(t.left))*180.0f/3.1415926535f)+(std::atan(std::fabs(t.right))*180.0f/3.1415926535f)));
        const auto v=uint32_t(std::lround((std::atan(std::fabs(t.down))*180.0f/3.1415926535f)+(std::atan(std::fabs(t.up))*180.0f/3.1415926535f)));
        bool match=false;for(uint32_t i=0;i<settings_.signatureCount;++i)match|=settings_.signatures[i].horizontal==h&&settings_.signatures[i].vertical==v;if(!match)return false;
      }
      auto extend=[](float n,float p,float f,float& on,float& op){const float m=std::max(std::fabs(n),std::fabs(p));on=n-(std::fabs(n)<m?f*(m-std::fabs(n)):0);op=p+(std::fabs(p)<m?f*(m-std::fabs(p)):0);};
      if(settings_.mode==NativeCullMode::Percent){const float f=1+settings_.percent/100;l={t.left*f,t.right*f,t.down*f,t.up*f};}
      else {extend(t.left,t.right,settings_.horizontalFraction,l.left,l.right);extend(t.down,t.up,settings_.verticalFraction,l.down,l.up);}
      if(!(l.left<=t.left&&l.right>=t.right&&l.down<=t.down&&l.up>=t.up&&finiteFrustum(l)))return false;
      factorW_[e]=(l.right-l.left)/(t.right-t.left);factorH_[e]=(l.up-l.down)/(t.up-t.down);
      if(!std::isfinite(factorW_[e])||!std::isfinite(factorH_[e])||factorW_[e]>4||factorH_[e]>4)return false;
    }
    maxFactorW_=std::max(factorW_[0],factorW_[1]);maxFactorH_=std::max(factorH_[0],factorH_[1]);
    return (maxFactorW_>=1.01f||maxFactorH_>=1.01f) &&
      roundDim(float(runtime_[0].width)*maxFactorW_) && roundDim(float(runtime_[0].height)*maxFactorH_) &&
      roundDim(float(runtime_[1].width)*maxFactorW_) && roundDim(float(runtime_[1].height)*maxFactorH_);
  }
  NativeCullSettings settings_{}; NativeCullFrustum true_[2]{},lied_[2]{}; NativeCullDimensions runtime_[2]{},baseline_[2]{},submitted_[2]{},adopted_[2]{};
  float factorW_[2]{1,1},factorH_[2]{1,1},maxFactorW_=1,maxFactorH_=1; uint64_t referenceGeneration_=0; uint32_t submittedMask_=0; NativeCullStage stage_=NativeCullStage::Off; bool baselineReady_=false,submittedReady_=false,adoptedReady_=false,forcedInert_=false,pendingStandDown_=false,changed_=false;
};
}
