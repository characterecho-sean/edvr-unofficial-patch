#include "../../src/openxr/system_geometry.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <thread>
#include <atomic>
using namespace edvr::openxr;
namespace {
unsigned checks=0,failures=0;
void check(bool value,const char* message){++checks;if(!value){++failures;std::printf("FAIL: %s\n",message);}}
bool near(float a,float b){return std::fabs(a-b)<.0001f;}
XrQuaternionf yaw(float degrees){const double half=degrees*3.14159265358979323846/360;return {0,float(std::sin(half)),0,float(std::cos(half))};}
GeometryInput input(uint64_t generation=1,uint64_t sequence=1){
  GeometryInput v{};v.generation=generation;v.sequence=sequence;v.displayTime=sequence*10;
  v.viewFlags=XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT;
  v.headFlags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_VALID_BIT;
  v.headPose={yaw(90),{1,2,3}};
  for(unsigned eye=0;eye<2;++eye){v.views[eye].pose={yaw(eye?60.f:120.f),{1,2,eye?3.03f:2.97f}};v.views[eye].fov={-.6f,.7f,.5f,-.4f};v.width[eye]=640+eye*10;v.height[eye]=480+eye*20;}
  return v;
}
void checkRelative(const GeometrySnapshot& s){
  for(unsigned eye=0;eye<2;++eye){const float sign=eye?-1.f:1.f;const float expected[3][4]={{.8660254f,0,sign*.5f,sign*.03f},{0,1,0,0},{-sign*.5f,0,.8660254f,0}};
    for(unsigned row=0;row<3;++row)for(unsigned col=0;col<4;++col)check(near(s.eyeToHead[eye].m[row][col],expected[row][col]),"independent relative yaw/translation matrix");}
}
int selfTest(){
  auto v=input();GeometrySnapshot snapshot{};check(makeGeometrySnapshot(v,snapshot),"valid complete native geometry");checkRelative(snapshot);
  const float head[3][4]={{0,0,1,1},{0,1,0,2},{-1,0,0,3}};
  for(unsigned row=0;row<3;++row)for(unsigned col=0;col<4;++col)check(near(snapshot.headToLocal.m[row][col],head[row][col]),"independent head local matrix");
  check(std::memcmp(snapshot.native.views,v.views,sizeof(v.views))==0&&snapshot.native.displayTime==10,"native views and time preserved");
  v.headPose.position={10,20,30};for(unsigned eye=0;eye<2;++eye){v.views[eye].pose.position={10,20,eye?30.03f:29.97f};}
  check(makeGeometrySnapshot(v,snapshot),"translated frame");checkRelative(snapshot);
  // Apply global yaw -90 to the original frame: HMD is identity and eyes
  // retain their independent +/-30 cant. Relative geometry must not change.
  v=input();v.headPose={yaw(0),{-3,2,1}};v.views[0].pose={yaw(30),{-2.97f,2,1}};v.views[1].pose={yaw(-30),{-3.03f,2,1}};
  check(makeGeometrySnapshot(v,snapshot),"global rotation");checkRelative(snapshot);
  const auto canary=snapshot;
  for(unsigned variant=0;variant<16;++variant){
    v=input();switch(variant){
      case 0:v.generation=0;break;case 1:v.sequence=0;break;case 2:v.viewFlags=0;break;case 3:v.headFlags=0;break;
      case 4:v.headPose.position.x=NAN;break;case 5:v.views[0].pose.position.z=INFINITY;break;
      case 6:v.headPose.orientation.w=2;break;case 7:v.views[1].pose.orientation.x=INFINITY;break;
      case 8:v.width[0]=0;break;case 9:v.height[1]=UINT32_MAX;break;case 10:v.views[0].type=XR_TYPE_UNKNOWN;break;
      case 11:v.views[1].next=reinterpret_cast<void*>(1);break;case 12:v.views[0].fov.angleUp=NAN;break;
      case 13:v.views[1].fov.angleLeft=v.views[1].fov.angleRight;break;
      case 14:v.headPose={yaw(0),{-(std::numeric_limits<float>::max)(),0,0}};v.views[0].pose={yaw(0),{(std::numeric_limits<float>::max)(),0,0}};break;
      case 15:v.height[0]=0;break;
    }
    check(!makeGeometrySnapshot(v,snapshot),"invalid input rejected");check(std::memcmp(&snapshot,&canary,sizeof(snapshot))==0,"failure leaves all output unchanged");
  }
  GeometryStore store;check(!store.read(snapshot),"uninitialized store unreadable");
  const auto generation=store.beginGeneration();v=input(generation,1);check(store.publish(v),"first publication");
  SystemGeometry read(store);check(read.valid()&&read.generation()==generation&&read.sequence()==1,"read transaction identity");
  uint32_t w=0,h=0;check(read.recommendedSize(w,h)&&w==650&&h==500,"common maximum eye size");
  vr::HmdMatrix34_t eye{};check(read.eyeToHead(vr::Eye_Left,eye)&&near(eye.m[0][3],.03f),"game-facing eye transform");
  RawFov raw{};check(read.raw(vr::Eye_Left,raw)&&near(raw.top,std::tan(-.4f)),"historical vertical tangent convention");
  vr::HmdMatrix44_t matrix{};check(read.projection(vr::Eye_Right,.025f,50000,vr::API_DirectX,matrix)&&near(matrix.m[3][2],-1),"game-facing arbitrary planes");
  const auto matrixCanary=matrix;check(!read.projection(vr::Eye_Right,0,50000,vr::API_DirectX,matrix)&&std::memcmp(&matrix,&matrixCanary,sizeof(matrix))==0,"invalid caller planes unchanged");
  check(!read.raw(static_cast<vr::EVREye>(2),raw),"invalid eye rejected");
  v.sequence=2;v.headPose.position.x=2;check(store.publish(v),"next frame publication");
  check(read.sequence()==1&&read.headToLocal(eye)&&near(eye.m[0][3],1),"existing read transaction immutable");
  check(!store.publish(input(generation,1))&&SystemGeometry(store).sequence()==2,"stale frame cannot replace current");
  check(!store.invalidate(generation,1)&&SystemGeometry(store).valid(),"stale invalidation ignored");
  check(store.invalidate(generation,2)&&!SystemGeometry(store).valid(),"same-frame failure clears current");
  check(!store.publish(input(generation,2)),"invalidated sample cannot be replayed");
  check(store.publish(input(generation,3)),"later valid sample recovers");
  auto invalid=input(generation,4);invalid.headFlags=0;check(!store.publish(invalid)&&!SystemGeometry(store).valid(),"invalid tracking clears cached geometry");
  store.retire(generation);check(!store.publish(input(generation,5)),"retirement blocks all later samples");
  const auto next=store.beginGeneration();check(next>generation&&!SystemGeometry(store).valid(),"reinit clears old snapshots");
  check(!store.publish(input(generation,999))&&store.publish(input(next,1)),"old generation cannot publish");
  store.retire(generation);check(SystemGeometry(store).valid(),"old shutdown cannot retire current generation");
  SystemGeometry empty(GeometryStore{});w=7;h=9;check(!empty.recommendedSize(w,h)&&w==7&&h==9,"unavailable getters don't fabricate geometry");
  GeometryStore threaded;const auto gen=threaded.beginGeneration();
  std::atomic<bool> torn{false};std::atomic<unsigned> reads{0};
  auto sample=[&](uint64_t seq){auto x=input(gen,seq);x.headPose.position.x=float(seq);x.width[0]=unsigned(seq+100);return x;};
  check(threaded.publish(sample(1)),"concurrency initial sample");
  std::thread writer([&]{for(uint64_t seq=2;seq<5000;++seq)threaded.publish(sample(seq));});
  auto reader=[&]{for(unsigned n=0;n<10000;++n){GeometrySnapshot x{};if(threaded.read(x)){
    ++reads;const auto seq=x.native.sequence;
    if(x.native.headPose.position.x!=float(seq)||x.native.width[0]!=seq+100||x.native.displayTime!=XrTime(seq*10)||x.native.generation!=gen)torn=true;
  }}};
  std::thread one(reader),two(reader);writer.join();one.join();two.join();
  check(reads==20000&&!torn,"concurrent readers never see mixed snapshots");
  std::printf("openxr_geometry_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
}
int main(int argc,char**argv){
  if(argc!=2)return 2;if(!std::strcmp(argv[1],"--dry-run")){std::puts("Would test native geometry and coherent readers; no runtime/device/files.");return 0;}
  return !std::strcmp(argv[1],"--self-test")?selfTest():2;
}
