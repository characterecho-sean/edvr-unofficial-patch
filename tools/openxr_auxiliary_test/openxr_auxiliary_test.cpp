#include "../../src/openxr/openvr_auxiliary.h"
#include <cstdio>
#include <cstring>
#include <limits>
#include <initializer_list>
using namespace edvr::openxr;
extern "C" vr::IVRExtendedDisplay* auxiliaryDisplayBase(vr::IVRExtendedDisplay*);
extern "C" vr::IVRChaperone* auxiliaryChaperoneBase(vr::IVRChaperone*);
namespace {
unsigned checks=0,failures=0;
void check(bool value,const char* label){++checks;if(!value){++failures;std::printf("FAIL: %s\n",label);}}
struct Source:AuxiliarySource {
  AuxiliaryRead value{1,true,{100,200},{300,400}};unsigned diagnostics[3][8]{};
  AuxiliaryRead readAuxiliary()const override{return value;}
  void auxiliaryUnsupported(unsigned interfaceId,unsigned slot)noexcept override {
    if(interfaceId<3&&slot<8)++diagnostics[interfaceId][slot];
  }
};
void displayTests(Source& source,vr::IVRExtendedDisplay* display) {
  for(unsigned mode=0;mode<7;++mode) {
    source.value={1,true,{100,200},{300,400}};
    if(mode==1)source.value.generation=0;
    if(mode==2)source.value.connected=false;
    if(mode==3)source.value.width[0]=0;
    if(mode==4)source.value.height[1]=0;
    if(mode==5){source.value.width[0]=(std::numeric_limits<uint32_t>::max)();source.value.width[1]=1;}
    if(mode==6){source.value.width[0]=2;source.value.width[1]=3;source.value.height[0]=7;source.value.height[1]=5;}
    const bool usable=mode==0||mode==6;
    struct Window {int32_t x=9,y=9;uint32_t width=9,height=9,guard=0xabcdef12;} window;
    display->GetWindowBounds(&window.x,&window.y,&window.width,&window.height);
    check(window.x==0&&window.y==0,"window origin initialized");
    check(window.width==(usable?source.value.width[0]+source.value.width[1]:0),"window width matches policy");
    check(window.height==(usable?(mode==0?400u:7u):0),"window max height");
    check(window.guard==0xabcdef12,"window guard");
    for(auto eye:{vr::Eye_Left,vr::Eye_Right,static_cast<vr::EVREye>(2),static_cast<vr::EVREye>(-1)}) {
      uint32_t x=9,y=9,width=9,height=9;const bool valid=usable&&(eye==vr::Eye_Left||eye==vr::Eye_Right);
      display->GetEyeOutputViewport(eye,&x,&y,&width,&height);
      check(x==(valid&&eye==vr::Eye_Right?source.value.width[0]:0)&&y==0,"viewport offset and invalid eye");
      check(width==(valid?source.value.width[unsigned(eye)]:0),"viewport width");
      check(height==(valid?source.value.height[unsigned(eye)]:0),"viewport height");
    }
  }
  source.value={1,true,{100,200},{300,400}};
  for(unsigned mask=0;mask<16;++mask) {
    int32_t x=9,y=9;uint32_t width=9,height=9;
    display->GetWindowBounds(mask&1?&x:nullptr,mask&2?&y:nullptr,mask&4?&width:nullptr,mask&8?&height:nullptr);
    check(x==(mask&1?0:9)&&y==(mask&2?0:9)&&width==(mask&4?300u:9u)&&height==(mask&8?400u:9u),"nullable window outputs");
    uint32_t vx=9,vy=9; width=height=9;
    display->GetEyeOutputViewport(vr::Eye_Right,mask&1?&vx:nullptr,mask&2?&vy:nullptr,mask&4?&width:nullptr,mask&8?&height:nullptr);
    check(vx==(mask&1?100u:9u)&&vy==(mask&2?0u:9u)&&width==(mask&4?200u:9u)&&height==(mask&8?400u:9u),"nullable viewport outputs");
  }
  for(unsigned mask=0;mask<4;++mask) {
    int32_t adapter=7,output=7;display->GetDXGIOutputInfo(mask&1?&adapter:nullptr,mask&2?&output:nullptr);
    check(adapter==(mask&1?-1:7)&&output==(mask&2?-1:7),"no physical output invented");
  }
  check(source.diagnostics[1][2]==1,"display diagnostic once");
}
void zeroColor(const vr::HmdColor_t& color){check(color.r==0&&color.g==0&&color.b==0&&color.a==0,"zero unavailable color");}
void chaperoneTests(Source& source,vr::IVRChaperone* chaperone) {
  for(unsigned pass=0;pass<2;++pass) {
    check(chaperone->GetCalibrationState()==vr::ChaperoneCalibrationState_Error,"no fabricated calibration");
    float x=1,z=2;check(!chaperone->GetPlayAreaSize(&x,&z)&&x==0&&z==0,"size failure initializes");
    check(!chaperone->GetPlayAreaSize(nullptr,&z)&&!chaperone->GetPlayAreaSize(&x,nullptr)&&!chaperone->GetPlayAreaSize(nullptr,nullptr),"nullable size outputs");
    struct Rect {vr::HmdQuad_t value;uint32_t guard=0x12345678;} rect;
    std::memset(&rect.value,0xcd,sizeof(rect.value));
    check(!chaperone->GetPlayAreaRect(&rect.value)&&rect.guard==0x12345678,"rect failure bounded");
    for(const auto& corner:rect.value.vCorners)for(float value:corner.v)check(value==0,"zero unavailable rect component");
    check(!chaperone->GetPlayAreaRect(nullptr),"nullable rect");
    chaperone->ReloadInfo();chaperone->SetSceneColor({1,2,3,4});
    struct Colors {uint32_t before=0x99887766;vr::HmdColor_t values[3];uint32_t after=0x11223344;} colors;
    for(int count:{-1,0,1,3}) {
      std::memset(colors.values,0x7f,sizeof(colors.values));const auto original=colors;vr::HmdColor_t camera{1,2,3,4};
      chaperone->GetBoundsColor(colors.values,count,2,&camera);zeroColor(camera);
      check(colors.before==0x99887766&&colors.after==0x11223344,"color array guards");
      for(int i=0;i<3;++i) {
        if(i<count)zeroColor(colors.values[i]);
        else check(std::memcmp(&colors.values[i],&original.values[i],sizeof(vr::HmdColor_t))==0,"unrequested colors untouched");
      }
    }
    chaperone->GetBoundsColor(nullptr,3,0,nullptr);
    check(!chaperone->AreBoundsVisible(),"bounds not displayed");chaperone->ForceBoundsVisible(pass!=0);
  }
  for(unsigned slot=0;slot<8;++slot)check(source.diagnostics[2][slot]==1,"chaperone diagnostic once per slot");
  OpenVRChaperone another(source);auto* base=auxiliaryChaperoneBase(&another);base->GetCalibrationState();
  check(source.diagnostics[2][0]==2,"new object has its own diagnostic budget");
}
}
int main(int argc,char** argv) {
  if(argc!=2)return 2;
  if(std::strcmp(argv[1],"--dry-run")==0){std::puts("openxr_auxiliary_test: dry-run (no runtime or writes)");return 0;}
  if(std::strcmp(argv[1],"--self-test")!=0)return 2;
  Source source;OpenVRExtendedDisplay display(source);OpenVRChaperone chaperone(source);
  displayTests(source,auxiliaryDisplayBase(&display));chaperoneTests(source,auxiliaryChaperoneBase(&chaperone));
  std::printf("openxr_auxiliary_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
