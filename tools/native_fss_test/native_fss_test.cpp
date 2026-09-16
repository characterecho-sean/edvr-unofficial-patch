#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <string>
#include <cmath>
#include <stdexcept>
#include "../../src/common/native_fss.h"
#include "../../src/common/config.h"
#include "../../src/common/log.h"
#include "../../src/common/system_d3d11.h"

using Microsoft::WRL::ComPtr;
static unsigned checks=0, failures=0, healCalls=0;
static void* seenLeft=nullptr; static void* seenRight=nullptr; static float seenRect[4]{}; static float seenOuter=0,seenInner=0; static int seenMode=0;
static LONG arrival=0, chrome=0; static float panel[16]{}; static bool panelReady=false;
static void* published[2]{};
static std::string mode="on";
void check(bool ok,const char* msg){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",msg);}}
void check(bool ok){check(ok,"unnamed check");}
void require(bool ok,const char* msg){check(ok,msg);if(!ok)throw std::runtime_error(msg);}

namespace edvr {
Config& Config::get(){static Config c;return c;}
std::string Config::getString(const char* key,const char* def) const {return std::strcmp(key,"fix.fss_eye_sync")==0?mode:(def?def:"");}
Log& Log::get(){static Log l;return l;}
Log::~Log(){}
void Log::note(const char*,...){ }
void publishSubmitTexture(int eye,void* texture){if(eye>=0&&eye<2)published[eye]=texture;}
void fssHealRelease(){}
LONG fssArrivalStampValue(){return arrival;}
LONG fssChromeStampValue(){return chrome;}
bool readFssPanelRect(float* out){if(!panelReady||!out)return false;std::memcpy(out,panel,sizeof(panel));return true;}
}
extern "C" void* edvrFssHealLeft(void* left,void* right,float outer,float inner,int m,const float* rect){
  ++healCalls;seenLeft=left;seenRight=right;seenOuter=outer;seenInner=inner;seenMode=m;if(rect)std::memcpy(seenRect,rect,sizeof(seenRect));return left;
}

struct Device {
  ComPtr<ID3D11Device> d; ComPtr<ID3D11DeviceContext> c;
  Device(){auto fn=edvr::systemD3D11CreateDevice();require(fn!=nullptr,"System32 D3D11CreateDevice");D3D_FEATURE_LEVEL fl{};require(fn&&SUCCEEDED(fn(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,&fl,&c)),"WARP device");}
  ComPtr<ID3D11Texture2D> tex(unsigned w=32,unsigned h=24){D3D11_TEXTURE2D_DESC x{};x.Width=w;x.Height=h;x.MipLevels=x.ArraySize=x.SampleDesc.Count=1;x.Format=DXGI_FORMAT_R8G8B8A8_UNORM;x.Usage=D3D11_USAGE_DEFAULT;x.BindFlags=D3D11_BIND_SHADER_RESOURCE;ComPtr<ID3D11Texture2D> t;require(SUCCEEDED(d->CreateTexture2D(&x,nullptr,&t)),"texture");return t;}
  ComPtr<ID3D11Texture2D> staging(unsigned w=32,unsigned h=24){D3D11_TEXTURE2D_DESC x{};x.Width=w;x.Height=h;x.MipLevels=x.ArraySize=x.SampleDesc.Count=1;x.Format=DXGI_FORMAT_R8G8B8A8_UNORM;x.Usage=D3D11_USAGE_STAGING;x.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> t;require(SUCCEEDED(d->CreateTexture2D(&x,nullptr,&t)),"staging");return t;}
};
EdvrNativeFssFrame frame(uint64_t seq,uint64_t ref=1){EdvrNativeFssFrame f{};f.size=sizeof(f);f.version=2;f.generation=7;f.referenceGeneration=ref;f.sequence=seq;f.frusta[0][0]=-1.2f;f.frusta[0][1]=.8f;f.frusta[0][2]=-.9f;f.frusta[0][3]=1.1f;f.frusta[1][0]=-.8f;f.frusta[1][1]=1.2f;f.frusta[1][2]=-.9f;f.frusta[1][3]=1.1f;for(unsigned e=0;e<2;++e){f.eyeToHead[e][0]=f.eyeToHead[e][5]=f.eyeToHead[e][10]=1.f;f.eyeToHead[e][3]=e?-.032f:.032f;}return f;}
EdvrNativeFssTable acquire(Device& d){EdvrNativeFssRequest r{sizeof(r),2,d.d.Get(),7};EdvrNativeFssTable t{sizeof(t),2};require(edvrAcquireNativeFss(&r,&t)==S_OK,"acquire");return t;}
HRESULT treat(EdvrNativeFssTable& t,uint64_t seq,unsigned eye,ID3D11Texture2D* src,const float* b,ID3D11Texture2D** returned=nullptr){ID3D11Texture2D* out=nullptr;HRESULT h=t.treatEye(t.context,seq,eye,src,b,&out);if(returned)*returned=out;else if(out)out->Release();return h;}
HRESULT healPair(EdvrNativeFssTable& t,uint64_t seq,uint32_t* eye,ID3D11Texture2D** returned=nullptr){ID3D11Texture2D* out=nullptr;HRESULT h=t.healPair(t.context,seq,eye,&out);if(returned)*returned=out;else if(out)out->Release();return h;}
void fill(Device& d,ID3D11Texture2D* t,unsigned char value){unsigned char p[32*24*4]{};for(unsigned i=0;i<sizeof(p);i+=4){p[i]=value;p[i+3]=255;}d.c->UpdateSubresource(t,0,nullptr,p,32*4,0);}
unsigned char firstPixel(Device& d,ID3D11Texture2D* t){D3D11_TEXTURE2D_DESC td{};t->GetDesc(&td);auto st=d.staging(td.Width,td.Height);d.c->CopyResource(st.Get(),t);d.c->Flush();D3D11_MAPPED_SUBRESOURCE m{};require(SUCCEEDED(d.c->Map(st.Get(),0,D3D11_MAP_READ,0,&m)),"map donor");unsigned char v=*static_cast<unsigned char*>(m.pData);d.c->Unmap(st.Get(),0);return v;}

void run(){
  Device d,foreign;auto left=d.tex(),right=d.tex(),bad=foreign.tex();const float full[4]={0,0,1,1};
  panelReady=true;panel[0]=.10f;panel[1]=.20f;panel[2]=.90f;panel[3]=.20f;panel[4]=.90f;panel[5]=.80f;panel[6]=.10f;panel[7]=.80f;
  // seq 1: mode heal (target left/eye0). Left treated first only snapshots;
  // the heal is pending until the right is treated the same frame.
  mode="heal";arrival=0;chrome=0;auto t=acquire(d);auto f1=frame(1);check(t.beginFrame(t.context,&f1)==S_OK,"begin first");arrival=1;chrome=1;uint32_t eye=99;check(treat(t,1,0,left.Get(),full)==S_FALSE,"left first only snapshots");check(healPair(t,1,&eye)==E_PENDING,"left alone waits on the right donor");fill(d,right.Get(),42);check(treat(t,1,1,right.Get(),full)==S_FALSE,"right also only snapshots");ID3D11Texture2D* healed=nullptr;require(healPair(t,1,&eye,&healed)==S_OK&&eye==0&&healed,"both eyes present heals the same frame");check(healCalls==1&&seenMode==1,"mode one healer");check(std::fabs(seenRect[0]-(.1f+.8f*.5f*(1-131.176f/164.700f)))<.001f&&std::fabs(seenOuter-1.2f)<.001f&&std::fabs(seenInner-.8f)<.001f,"rectangle and tangent order");D3D11_TEXTURE2D_DESC hd{};healed->GetDesc(&hd);check(hd.Width==32&&hd.Height==24,"full ROI output size");check(firstPixel(d,static_cast<ID3D11Texture2D*>(seenRight))==42,"owned donor survives source overwrite");fill(d,right.Get(),99);check(firstPixel(d,static_cast<ID3D11Texture2D*>(seenRight))==42,"donor remains immutable");healed->Release();check(healPair(t,1,&eye)==S_FALSE,"heal delivered once per frame");
  // seq 2: same mode, right treated first this time.
  unsigned before=healCalls;auto f2=frame(2);check(t.beginFrame(t.context,&f2)==S_OK,"begin right first");arrival=2;chrome=2;check(treat(t,2,1,right.Get(),full)==S_FALSE,"right first only snapshots");check(healPair(t,2,&eye)==S_FALSE,"target not yet snapshotted");check(treat(t,2,0,left.Get(),full)==S_FALSE,"left completes the pair");check(healPair(t,2,&eye)==S_OK&&eye==0,"right-first heals once the target arrives");check(healCalls==before+1,"right-first still heals once");
  // seq 3: mode mirror (target right/eye1); left treated first is donor-only.
  mode="mirror";before=healCalls;auto f3=frame(3);check(t.beginFrame(t.context,&f3)==S_OK,"begin mirror");arrival=3;chrome=3;check(treat(t,3,0,left.Get(),full)==S_FALSE,"mirror donor snapshot only");check(healPair(t,3,&eye)==S_FALSE,"mirror target not yet snapshotted");check(treat(t,3,1,right.Get(),full)==S_FALSE,"mirror target snapshot only");check(healPair(t,3,&eye)==S_OK&&eye==1&&seenMode==2&&seenLeft!=seenRight,"mirror heals the right eye");check(healCalls==before+1,"mirror heals once");
  // Gate age: healGate closes three frames after the last stamp change.
  mode="heal";before=healCalls;auto f4=frame(4);check(t.beginFrame(t.context,&f4)==S_OK,"begin gate age");check(treat(t,4,0,left.Get(),full)==S_FALSE,"gate age left snapshot");check(treat(t,4,1,right.Get(),full)==S_FALSE,"gate age right snapshot");check(healPair(t,4,&eye)==S_OK,"unchanged stamps remain recent");before=healCalls;for(uint64_t n=5;n<8;++n){auto fx=frame(n);check(t.beginFrame(t.context,&fx)==S_OK,"begin gate age sequence");check(treat(t,n,0,left.Get(),full)==S_FALSE,"gate age sequence left snapshot");check(treat(t,n,1,right.Get(),full)==S_FALSE,"gate age sequence right snapshot");check(healPair(t,n,&eye)==(n<=6?S_OK:S_FALSE),"arrival gate expires after three frames");}check(healCalls==before+2,"gate age healed only while recent");before=healCalls;auto f8=frame(8);check(t.beginFrame(t.context,&f8)==S_OK,"begin expired gate");check(treat(t,8,0,left.Get(),full)==S_FALSE&&treat(t,8,1,right.Get(),full)==S_FALSE,"expired gate still snapshots");check(healPair(t,8,&eye)==S_FALSE&&healCalls==before,"expired gate stays off");
  // Canted projection: guarded off regardless of stamps.
  mode="heal";arrival=4;chrome=4;auto canted=frame(10);canted.eyeToHead[1][0]=.98f;canted.eyeToHead[1][2]=.2f;canted.eyeToHead[1][8]=-.2f;canted.eyeToHead[1][10]=.98f;check(t.beginFrame(t.context,&canted)==S_OK,"begin canted");check(treat(t,10,0,left.Get(),full)==S_FALSE&&treat(t,10,1,right.Get(),full)==S_FALSE,"canted projection still snapshots");before=healCalls;check(healPair(t,10,&eye)==S_FALSE&&healCalls==before,"canted projection guarded");
  // Invalid arguments, including the three new healPair failure modes.
  auto f11=frame(11);check(t.beginFrame(t.context,&f11)==S_OK,"begin invalid tests");check(treat(t,11,0,bad.Get(),full)==E_INVALIDARG,"wrong device");const float invalid[4]={0,0,0,1};check(treat(t,11,0,left.Get(),invalid)==E_INVALIDARG,"invalid bounds");const float flipped[4]={1,1,0,0};check(treat(t,11,0,left.Get(),flipped)==S_FALSE,"flipped bounds passthrough consumes");const float degFlip[4]={1,0,1,1};check(treat(t,11,1,right.Get(),degFlip)==E_INVALIDARG,"degenerate flipped bounds rejected");check(treat(t,11,0,left.Get(),full)==E_INVALIDARG,"flipped consumed");HRESULT wrong=E_FAIL;std::thread th([&]{wrong=treat(t,11,1,right.Get(),full);});th.join();check(wrong==E_INVALIDARG,"wrong thread");check(healPair(t,999,&eye)==E_INVALIDARG,"healPair wrong sequence");check(t.healPair(t.context,11,&eye,nullptr)==E_INVALIDARG,"healPair null out");HRESULT wrongHeal=E_FAIL;std::thread th2([&]{wrongHeal=healPair(t,11,&eye);});th2.join();check(wrongHeal==E_INVALIDARG,"healPair wrong thread");
  check(t.invalidate(t.context)==S_OK,"invalidate");auto f12=frame(12);check(t.beginFrame(t.context,&f12)==S_OK,"sequence recovers after invalidate");const float partial[4]={.25f,.25f,.75f,.75f};auto ref=frame(13,2);check(t.beginFrame(t.context,&ref)==S_OK,"reference change clears donors and stamps");arrival=5;chrome=5;check(treat(t,13,1,right.Get(),partial)==S_FALSE,"new reference right donor");arrival=5;chrome=5;auto crop=frame(14,2);check(t.beginFrame(t.context,&crop)==S_OK);check(treat(t,14,1,right.Get(),partial)==S_FALSE,"crop right snapshot");check(treat(t,14,0,left.Get(),partial)==S_FALSE,"crop left snapshot");ID3D11Texture2D* cropped=nullptr;require(healPair(t,14,&eye,&cropped)==S_OK&&cropped,"ROI heal");D3D11_TEXTURE2D_DESC cd{};cropped->GetDesc(&cd);check(cd.Width==16&&cd.Height==12,"healed output is cropped ROI");cropped->Release();
  // Equal logical eyes can occupy different regions of the game's atlas.
  const float atlasLeft[4]={0,0,.5f,1},atlasRight[4]={.5f,0,1,1};
  auto atlas=frame(15,2);check(t.beginFrame(t.context,&atlas)==S_OK,"begin atlas eyes");
  arrival=6;chrome=6;
  check(treat(t,15,1,right.Get(),atlasRight)==S_FALSE,"atlas right snapshot");
  check(treat(t,15,0,left.Get(),atlasLeft)==S_FALSE,"atlas left snapshot");
  check(healPair(t,15,&eye)==S_OK,"equal ROI donors allow different atlas offsets");
  // A donor never crosses frames: begin() clears both eyes' snapshots every
  // frame, so a pending target cannot be completed by a later frame's donor.
  before=healCalls;auto f16=frame(16,2);check(t.beginFrame(t.context,&f16)==S_OK,"begin donor frame");
  check(treat(t,16,0,left.Get(),full)==S_FALSE,"seq16 left snapshot only");
  check(healPair(t,16,&eye)==E_PENDING,"seq16 waits on its own right");
  auto f17=frame(17,2);check(t.beginFrame(t.context,&f17)==S_OK,"begin next frame with no right in 16");
  check(treat(t,17,1,right.Get(),full)==S_FALSE,"seq17 right snapshot only");
  check(healPair(t,17,&eye)==S_FALSE&&healCalls==before,"a stale pending left is not a donor pair");
  check(treat(t,17,0,left.Get(),full)==S_FALSE,"seq17 left snapshot completes the pair");
  check(healPair(t,17,&eye)==S_OK,"seq17 heals from its own pair");
  // Mirror, right-first: the target (right) waits on the left donor.
  mode="mirror";before=healCalls;auto f18=frame(18,2);check(t.beginFrame(t.context,&f18)==S_OK,"begin mirror right-first");
  check(treat(t,18,1,right.Get(),full)==S_FALSE,"mirror right-first target snapshot only");
  check(healPair(t,18,&eye)==E_PENDING,"mirror target waits for the left");
  check(treat(t,18,0,left.Get(),full)==S_FALSE,"mirror right-first donor snapshot");
  check(healPair(t,18,&eye)==S_OK&&eye==1,"mirror right-first still heals");
  check(healCalls==before+1,"mirror right-first heals once");
  // Unequal ROI sizes: both eyes snapshot but the mismatched pair refuses.
  mode="heal";before=healCalls;const float atlasRightNarrow[4]={.5f,0,.75f,1};
  auto unequal=frame(19,2);check(t.beginFrame(t.context,&unequal)==S_OK,"begin unequal atlas eyes");
  arrival=7;chrome=7;
  check(treat(t,19,1,right.Get(),atlasRightNarrow)==S_FALSE,"unequal atlas right snapshot");
  check(treat(t,19,0,left.Get(),atlasLeft)==S_FALSE,"unequal atlas left snapshot");
  check(healPair(t,19,&eye)==S_FALSE&&healCalls==before,"unequal ROI sizes refuse to heal");
  mode="heal";auto freshReference=frame(20,3);
  check(t.beginFrame(t.context,&freshReference)==S_OK,"begin reference with old nonzero stamps");
  check(treat(t,20,1,right.Get(),full)==S_FALSE&&!published[1],"old stamps do not trigger snapshots after recenter");
  check(treat(t,20,0,left.Get(),full)==S_FALSE&&!published[0],"old stamps do not trigger snapshots either eye");
  before=healCalls;check(healPair(t,20,&eye)==S_FALSE&&healCalls==before,"old stamps cannot heal new reference");
  mode="off";auto disabled=frame(21,3);
  check(t.beginFrame(t.context,&disabled)==S_OK,"begin disabled repair");arrival=8;chrome=8;
  check(treat(t,21,1,right.Get(),full)==S_FALSE&&!published[1],"disabled repair does not publish a donor");
  check(treat(t,21,0,left.Get(),full)==S_FALSE&&!published[0],"disabled repair preserves inputs despite fresh stamps");
  before=healCalls;check(healPair(t,21,&eye)==S_FALSE&&healCalls==before,"disabled repair never heals");
  check(t.close(t.context)==S_OK,"close");check(t.close(t.context)==S_FALSE,"close idempotent");
}
int wmain(int argc,wchar_t** argv){SetErrorMode(3);if(argc==2&&!wcscmp(argv[1],L"--dry-run")){std::puts("native_fss_test: dry-run");return 0;}if(argc!=2||wcscmp(argv[1],L"--self-test"))return 2;try{run();}catch(const std::exception& e){std::printf("FAIL: setup aborted: %s\n",e.what());return 1;}std::printf("native_fss_test: %u checks, %u failures\n",checks,failures);return failures?1:0;}
