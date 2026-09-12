#pragma once
#include "../openvr/compat/openvr_v0_9_20.h"
#include <atomic>
#include <cstdint>

namespace edvr::openxr {
struct AuxiliaryRead {
  uint64_t generation=0;
  bool connected=false;
  uint32_t width[2]{},height[2]{};
};
class AuxiliarySource {
 public:
  virtual ~AuxiliarySource()=default;
  // One coherent cached snapshot; must not open a runtime or wait for a frame.
  virtual AuxiliaryRead readAuxiliary() const=0;
  virtual void auxiliaryUnsupported(unsigned interfaceId,unsigned slot) noexcept=0;
};
class OpenVRExtendedDisplay final : public vr::IVRExtendedDisplay {
  AuxiliarySource& source_;
  std::atomic<unsigned> reported_{0};
  void report(unsigned slot) noexcept {
    const unsigned bit=1u<<slot;
    if(!(reported_.fetch_or(bit)&bit))source_.auxiliaryUnsupported(1,slot);
  }
 public:
  explicit OpenVRExtendedDisplay(AuxiliarySource& source):source_(source){}
  void GetWindowBounds(int32_t*,int32_t*,uint32_t*,uint32_t*) override;
  void GetEyeOutputViewport(vr::EVREye,uint32_t*,uint32_t*,uint32_t*,uint32_t*) override;
  void GetDXGIOutputInfo(int32_t*,int32_t*) override;
};
class OpenVRChaperone final : public vr::IVRChaperone {
  AuxiliarySource& source_;
  std::atomic<unsigned> reported_{0};
  void report(unsigned slot) noexcept {
    const unsigned bit=1u<<slot;
    if(!(reported_.fetch_or(bit)&bit))source_.auxiliaryUnsupported(2,slot);
  }
 public:
  explicit OpenVRChaperone(AuxiliarySource& source):source_(source){}
  vr::ChaperoneCalibrationState GetCalibrationState() override;
  bool GetPlayAreaSize(float*,float*) override;
  bool GetPlayAreaRect(vr::HmdQuad_t*) override;
  void ReloadInfo() override;
  void SetSceneColor(vr::HmdColor_t) override;
  void GetBoundsColor(vr::HmdColor_t*,int,float,vr::HmdColor_t*) override;
  bool AreBoundsVisible() override;
  void ForceBoundsVisible(bool) override;
};
}
