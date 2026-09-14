#include "openvr_auxiliary.h"
#include <limits>
#include <cstring>
namespace edvr::openxr {
namespace {
bool usable(const AuxiliaryRead& value) {
  return value.generation&&value.connected&&value.width[0]&&value.width[1]&&
    value.height[0]&&value.height[1]&&
    value.width[0]<=(std::numeric_limits<uint32_t>::max)()-value.width[1];
}
}
void OpenVRExtendedDisplay::GetWindowBounds(int32_t* x,int32_t* y,uint32_t* width,uint32_t* height) {
  if(x)*x=0;
  if(y)*y=0;
  if(width)*width=0;
  if(height)*height=0;
  const auto value=source_.readAuxiliary();
  if(!usable(value))return;
  // A virtual side-by-side framebuffer, not an HMD desktop monitor location.
  if(width)*width=value.width[0]+value.width[1];
  if(height)*height=value.height[0]>value.height[1]?value.height[0]:value.height[1];
}
void OpenVRExtendedDisplay::GetEyeOutputViewport(vr::EVREye eye,uint32_t* x,uint32_t* y,uint32_t* width,uint32_t* height) {
  if(x)*x=0;
  if(y)*y=0;
  if(width)*width=0;
  if(height)*height=0;
  if(eye!=vr::Eye_Left&&eye!=vr::Eye_Right)return;
  const auto value=source_.readAuxiliary();
  if(!usable(value))return;
  const unsigned index=eye==vr::Eye_Right?1:0;
  if(x)*x=index?value.width[0]:0;
  if(width)*width=value.width[index];
  if(height)*height=value.height[index];
}
void OpenVRExtendedDisplay::GetDXGIOutputInfo(int32_t* adapter,int32_t* output) {
  if(adapter)*adapter=-1;
  if(output)*output=-1;
  report(2);
}
vr::ChaperoneCalibrationState OpenVRChaperone::GetCalibrationState() {
  report(0);return vr::ChaperoneCalibrationState_Error;
}
bool OpenVRChaperone::GetPlayAreaSize(float* x,float* z) {
  if(x)*x=0;
  if(z)*z=0;
  report(1);return false;
}
bool OpenVRChaperone::GetPlayAreaRect(vr::HmdQuad_t* rect) {
  if(rect)*rect={};
  report(2);return false;
}
void OpenVRChaperone::ReloadInfo(){report(3);}
void OpenVRChaperone::SetSceneColor(vr::HmdColor_t){report(4);}
void OpenVRChaperone::GetBoundsColor(vr::HmdColor_t* colors,int count,float,vr::HmdColor_t* camera) {
  // The historical API supplies the caller's capacity in count. No boundary
  // renderer or calibrated STAGE bounds are claimed by this diagnostic.
  if(colors&&count>0)std::memset(colors,0,sizeof(vr::HmdColor_t)*static_cast<size_t>(count));
  if(camera)*camera={};
  report(5);
}
bool OpenVRChaperone::AreBoundsVisible(){report(6);return false;}
void OpenVRChaperone::ForceBoundsVisible(bool){report(7);}
} // namespace edvr::openxr
