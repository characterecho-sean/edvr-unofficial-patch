// F106: the left eye's share of the right eye's image (renderCaptured's leftEyeShare, src\openxr\eye_share.h), included by openxr_stereo_test.cpp after its fixture,
// readback and work-count helpers (it uses them as they are).
//
// While the sights are up the LEFT eye shows the RIGHT eye's image, turned into the left view by rotation only. The pixels are read back from WARP and held to this
// rig's own projection arithmetic (quaternion products, not the renderer's matrices): (a) parallel symmetric eyes: the left pixel looking straight ahead samples the
// right image's straight-ahead texel; (b) displays canted 10.75 deg each way with asymmetric fovs: a direction lands on the right image's texel the rig computes for it,
// in both eyes; (c) a left pixel outside the right fov keeps the left eye's own image, with a band of about 3 deg between; (d) share 0 (and anything that is not a share)
// is bit for bit the render made with no share, and draws nothing more; (e) share 1 inside the coverage is exactly the right eye's own pixel; (f) no texel of the right
// eye's hidden area reaches the left eye, and nothing turns black. Also: the right eye and the layer's views are never changed, a trimmed placement's border stays
// black, the coverage figure the renderer computes on the CPU matches what the GPU drew, and the pure parts (the relative rotation, the mask) on their own.
#pragma once

double decodeSrgb(double e){return e<=.04045?e/12.92:std::pow((e+.055)/1.055,2.4);}
ComPtr<ID3D11Texture2D> shareTexture(Fixture& f,const std::function<uint32_t(unsigned,unsigned)>& rgba) {
  D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=size;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;
  d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  std::vector<uint32_t> pixels(size_t(size)*size);
  for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x)pixels[size_t(y)*size+x]=rgba(x,y);
  D3D11_SUBRESOURCE_DATA data{pixels.data(),size*4,0};ComPtr<ID3D11Texture2D> texture;
  check(SUCCEEDED(f.runtime.device->CreateTexture2D(&d,&data,&texture)),"share texture");return texture;
}
uint32_t rgbaOf(unsigned r,unsigned g,unsigned b){return 0xff000000u|(b<<16)|(g<<8)|r;}
constexpr double kSharePi=3.14159265358979323846;
XrQuaternionf yawQuat(double deg){const double h=deg*kSharePi/360.0;return {0.f,float(std::sin(h)),0.f,float(std::cos(h))};}
XrFovf fovDeg(double l,double r,double u,double d){const double k=kSharePi/180.0;return {float(l*k),float(r*k),float(u*k),float(d*k)};}
// The rig's own arithmetic: the left pixel centre (x, y) as a direction, through the world, into the right view; false behind it. su, sv: the right swapchain's u, v.
// edgeDeg: the direction's distance inside the right fov's nearest edge, per axis angle (negative outside).
bool rigMap(const XrView (&v)[2],double x,double y,double& su,double& sv,double& edgeDeg) {
  const auto t=[](float a){return std::tan(double(a));};
  const XrFovf& lf=v[0].fov;const XrFovf& rf=v[1].fov;
  const double u=(x+.5)/size,w=(y+.5)/size;
  const SkyRay dl{t(lf.angleLeft)+(t(lf.angleRight)-t(lf.angleLeft))*u,t(lf.angleUp)+(t(lf.angleDown)-t(lf.angleUp))*w,-1};
  const SkyRay world=rotateSky(v[0].pose.orientation,dl);
  const XrQuaternionf inv{-v[1].pose.orientation.x,-v[1].pose.orientation.y,-v[1].pose.orientation.z,v[1].pose.orientation.w};
  const SkyRay dr=rotateSky(inv,world);
  if(!(dr.z<-1e-6))return false;
  const double ax=std::atan2(dr.x,-dr.z),ay=std::atan2(dr.y,-dr.z);
  edgeDeg=(std::min)((std::min)(ax-double(rf.angleLeft),double(rf.angleRight)-ax),(std::min)(ay-double(rf.angleDown),double(rf.angleUp)-ay))*180.0/kSharePi;
  su=(dr.x/-dr.z-t(rf.angleLeft))/(t(rf.angleRight)-t(rf.angleLeft));sv=(t(rf.angleUp)-dr.y/-dr.z)/(t(rf.angleUp)-t(rf.angleDown));
  return true;
}
void sharePureSelfTest() {
  using namespace edvr::openxr::shareview;
  double m[3][3],ypr[3];
  relativeRotation(yawQuat(10.75),yawQuat(-10.75),m);yawPitchRollDeg(m,ypr);
  check(std::fabs(ypr[0]-21.5)<1e-4&&std::fabs(ypr[1])<1e-4&&std::fabs(ypr[2])<1e-4,
        "F106 pure: displays canted 10.75 deg outward each give the left view a yaw of +21.5 deg against the right, no pitch or roll");
  relativeRotation(yawQuat(0),yawQuat(0),m);yawPitchRollDeg(m,ypr);
  check(std::fabs(ypr[0])<1e-9&&std::fabs(ypr[1])<1e-9&&std::fabs(ypr[2])<1e-9,"F106 pure: parallel views turn by nothing");
  const double place[4]={0,0,1,1};
  const Mapping par=makeMapping(fovDeg(-45,45,45,-45),yawQuat(0),fovDeg(-45,45,45,-45),yawQuat(0),place);
  const Lookup ahead=lookup(par,.5,.5),corner=lookup(par,.001,.001);
  check(par.valid&&ahead.ahead&&std::fabs(ahead.su-.5)<1e-12&&std::fabs(ahead.sv-.5)<1e-12&&ahead.fov==1.0&&corner.fov==0.0,
        "F106 pure: parallel symmetric views map the centre to the centre with full coverage, and the very corner (inside the edge margin) to none");
  const double empty[4]={.5,0,.5,1};
  check(!makeMapping(fovDeg(-45,45,45,-45),yawQuat(0),fovDeg(-45,45,45,-45),yawQuat(0),empty).valid&&
        !makeMapping(fovDeg(45,-45,45,-45),yawQuat(0),fovDeg(-45,45,45,-45),yawQuat(0),place).valid,"F106 pure: an empty placement or a fov whose edges cross is refused");
  // the mask: one triangle over the top-left corner of a +-45 deg view, (-1, 1), (-0.2, 1), (-1, 0.2) on the tangent plane
  const XrVector2f vtx[3]={{-1.f,1.f},{-.2f,1.f},{-1.f,.2f}};const uint32_t idx[3]={0,1,2};
  HiddenMask mask;
  check(buildHiddenMask(vtx,3,idx,3,tangentsOf(fovDeg(-45,45,45,-45)),size,size,mask)&&mask.size==kMaskSize&&mask.triangles==1&&mask.hiddenTexels>0,"F106 pure: the mask builds");
  bool inside=true;
  for(int i=0;i<200;++i)for(int j=0;j<200;++j) {
    const double tx=-1+.8*(i+.5)/200,ty=1-.8*(j+.5)/200;   // the triangle's bounding square
    if((tx+1)+(1-ty)>.8)continue;                           // beyond its hypotenuse
    inside=inside&&mask.sample((tx+1)/2,(1-ty)/2)==0.0;
  }
  check(inside,"F106 pure: every point of the hidden triangle samples exactly 0 from the mask (texels touched conservatively, then a zero band)");
  check(mask.sample(.75,.75)==1.0&&mask.sample(.5,.5)==1.0,"F106 pure: far from the hidden area the mask is 1");
  HiddenMask none;
  check(buildHiddenMask(nullptr,0,nullptr,0,tangentsOf(fovDeg(-45,45,45,-45)),size,size,none)&&none.hiddenTexels==0&&none.sample(.01,.01)==1.0,
        "F106 pure: an empty mesh hides nothing");
  check(!buildHiddenMask(vtx,3,idx,2,tangentsOf(fovDeg(-45,45,45,-45)),size,size,none),"F106 pure: an index count that is not whole triangles is refused");
}
void shareSelfTest(bool owned) {
  if(!owned)sharePureSelfTest();
  const char* mode=owned?"owned immediate":"deferred";
  char what[480];
  for(bool unorm:{false,true}) {
    const char* kind=unorm?"unorm":"srgb";
    Fixture f;f.runtime.unorm=unorm;check(f.init(owned)==XR_SUCCESS,"share initialize");
    const DXGI_FORMAT format=DXGI_FORMAT(f.renderer.format());
    const auto channels=[&](uint32_t p,unsigned& r,unsigned& g,unsigned& b){rgbOf(format,p,r,g,b);};
    struct Out {std::vector<uint32_t> eye[2];XrCompositionLayerProjectionView views[2]{};uint32_t viewCount=0;};
    const auto render=[&](EyeCapture& capture,float share,const StereoHiddenArea* hidden,const StereoPlacement* placement,bool withShare) {
      Out out;XrCompositionLayerProjection layer{};const unsigned image=f.runtime.eyes[0].calls%2;
      const XrResult r=withShare?f.renderer.renderCaptured(f.views,space,capture,layer,nullptr,nullptr,placement,0.f,nullptr,share,hidden)
                                :f.renderer.renderCaptured(f.views,space,capture,layer,nullptr,nullptr,placement);
      check(r==XR_SUCCESS,"share render");
      out.eye[0]=readWhole(f,0,image);out.eye[1]=readWhole(f,1,image);
      out.viewCount=layer.viewCount;for(unsigned i=0;i<2&&layer.views;++i)out.views[i]=layer.views[i];
      return out;
    };
    const auto at=[](const std::vector<uint32_t>& img,unsigned x,unsigned y){return img[size_t(y)*size+x];};
    // The right eye: a gradient that names its texel (red 2x, green 2y) with blue full; the left eye: plain black. Captured as linear: no decode before filtering.
    auto right=shareTexture(f,[](unsigned x,unsigned y){return rgbaOf(2*x,2*y,255);});
    auto left=shareTexture(f,[](unsigned,unsigned){return rgbaOf(0,0,0);});
    EyeCapture capture;check(SUCCEEDED(capture.initialize(f.runtime.device.Get())),"share capture owner");
    const vr::Texture_t lt{left.Get(),vr::API_DirectX,vr::ColorSpace_Linear},rt{right.Get(),vr::API_DirectX,vr::ColorSpace_Linear};
    check(capture.capture(vr::Eye_Left,&lt)==vr::VRCompositorError_None&&capture.capture(vr::Eye_Right,&rt)==vr::VRCompositorError_None,"share capture");

    // ---- parallel symmetric eyes (the fixture's: identity, +-45 deg) ----
    const Out base=render(capture,0.f,nullptr,nullptr,false);
    {
      bool same=true;
      for(float s:{0.0f,-0.0f,-1.0f,std::numeric_limits<float>::quiet_NaN()}){const Out o=render(capture,s,nullptr,nullptr,true);same=same&&o.eye[0]==base.eye[0]&&o.eye[1]==base.eye[1];}
      std::snprintf(what,sizeof(what),"F106 (d) (%s, %s) SHARE 0 IS TODAY'S IMAGE, bit for bit: share 0, -0, -1 and NaN give both eyes exactly the render made with no share",mode,kind);
      check(same,what);
    }
    const Out full=render(capture,1.f,nullptr,nullptr,true);
    { // B3: while the scope zoom's window stands the left eye's own picture is the zoomed view, so the part of it the share does not cover (the corner here, inside the edge margin) goes
      // toward black by the strip's weight; the covered interior is the right eye's pixel whatever the strip is, no strip is the share exactly as it was, and no share draws no strip.
      auto gray=shareTexture(f,[](unsigned,unsigned){return rgbaOf(180,180,180);});
      EyeCapture grayCapture;check(SUCCEEDED(grayCapture.initialize(f.runtime.device.Get())),"strip capture owner");
      const vr::Texture_t gl{gray.Get(),vr::API_DirectX,vr::ColorSpace_Linear},rt2{right.Get(),vr::API_DirectX,vr::ColorSpace_Linear};
      check(grayCapture.capture(vr::Eye_Left,&gl)==vr::VRCompositorError_None&&grayCapture.capture(vr::Eye_Right,&rt2)==vr::VRCompositorError_None,"strip capture");
      const auto strip=[&](float share,float black) {
        XrCompositionLayerProjection layer{};const unsigned image=f.runtime.eyes[0].calls%2;
        check(f.renderer.renderCaptured(f.views,space,grayCapture,layer,nullptr,nullptr,nullptr,0.f,nullptr,share,nullptr,black)==XR_SUCCESS,"strip render");
        return readWhole(f,0,image);
      };
      const auto sum=[&](uint32_t p){unsigned r=0,g=0,b=0;channels(p,r,g,b);return r+g+b;};
      const std::vector<uint32_t> s0=strip(1.f,0.f),sHalf=strip(1.f,.5f),s1=strip(1.f,1.f),noShare=strip(0.f,1.f),noShare0=strip(0.f,0.f),nan=strip(1.f,std::numeric_limits<float>::quiet_NaN());
      std::snprintf(what,sizeof(what),"B3 strip (%s, %s): the corner the share does not cover is the left eye's own picture with no strip, half toward black with half a strip, and black with a whole one",mode,kind);
      check(sum(at(s0,0,0))>3*150&&sum(at(sHalf,0,0))<sum(at(s0,0,0))&&sum(at(sHalf,0,0))>sum(at(s1,0,0))&&sum(at(s1,0,0))<=3&&sum(at(s1,0,0))<sum(at(sHalf,0,0)),what);
      bool covered=true;
      for(unsigned y=16;y<112;++y)for(unsigned x=16;x<112;++x)covered=covered&&at(s1,x,y)==at(base.eye[1],x,y)&&at(sHalf,x,y)==at(base.eye[1],x,y);
      std::snprintf(what,sizeof(what),"B3 strip (%s, %s): inside the coverage the pixel is the right eye's own, bit for bit, whatever the strip is",mode,kind);
      check(covered,what);
      std::snprintf(what,sizeof(what),"B3 strip (%s, %s): no strip (and a strip that is no number) is the share exactly as it was, and with no share the strip draws nothing",mode,kind);
      check(s0==nan&&noShare==noShare0,what);
    }
    {
      bool interior=true;
      for(unsigned y=16;y<112;++y)for(unsigned x=16;x<112;++x)interior=interior&&at(full.eye[0],x,y)==at(base.eye[1],x,y);
      std::snprintf(what,sizeof(what),"F106 (e) (%s, %s) SHARE 1 INSIDE THE COVERAGE IS THE RIGHT EYE'S OWN PIXEL, bit for bit (parallel eyes: every left pixel 16..111 equals the right eye's there)",
                    mode,kind);
      check(interior,what);
      unsigned r=0,g=0,b=0;channels(at(full.eye[0],64,64),r,g,b);
      const int want=int(encode(128.0/255));
      const bool texel=at(full.eye[0],64,64)==at(base.eye[1],64,64)&&std::abs(int(r)-want)<=1&&std::abs(int(g)-want)<=1&&b==255;
      std::snprintf(what,sizeof(what),"F106 (a) (%s, %s) parallel symmetric eyes: the left pixel looking straight ahead (64, 64) samples the right image's straight-ahead texel (64, 64): "
                    "red and green 128, blue full",mode,kind);
      check(texel&&at(full.eye[0],63,63)==at(base.eye[1],63,63),what);
      bool views=full.viewCount==2;
      for(unsigned i=0;i<2;++i)views=views&&std::memcmp(&full.views[i].pose,&f.views[i].pose,sizeof(XrPosef))==0&&std::memcmp(&full.views[i].fov,&f.views[i].fov,sizeof(XrFovf))==0;
      std::snprintf(what,sizeof(what),"F106 (%s, %s) the right eye is never touched, and the layer's views stay each eye's own pose and fov",mode,kind);
      check(full.eye[1]==base.eye[1]&&views,what);
      const auto& facts=f.renderer.shareFacts();
      check(facts.drawn&&!facts.refused&&std::fabs(facts.rotationDeg[0])<1e-3f&&facts.hiddenTriangles==0&&facts.coverage>.8f&&facts.coverage<=1.f,
            "F106: the facts of a parallel share: drawn, no rotation, no mask, most of the left eye taken");
    }
    {   // the work: share 0 draws what a plain frame draws; a share above 0 one more triangle, on the left eye only
      const PassWork plain=measurePass(f,[&]{XrCompositionLayerProjection l{};check(f.renderer.renderCaptured(f.views,space,capture,l)==XR_SUCCESS,"share work render");});
      const PassWork zero=measurePass(f,[&]{XrCompositionLayerProjection l{};
        check(f.renderer.renderCaptured(f.views,space,capture,l,nullptr,nullptr,nullptr,0.f,nullptr,0.f)==XR_SUCCESS,"share work render");});
      const PassWork half=measurePass(f,[&]{XrCompositionLayerProjection l{};
        check(f.renderer.renderCaptured(f.views,space,capture,l,nullptr,nullptr,nullptr,0.f,nullptr,.5f)==XR_SUCCESS,"share work render");});
      std::snprintf(what,sizeof(what),"F106 (d) (%s, %s) share 0 runs no extra pass (the same vertex and pixel work as a plain frame); a share above 0 draws one more triangle, one eye's worth",
                    mode,kind);
      check(plain.ok&&zero.ok&&half.ok&&zero.vs==plain.vs&&zero.ps==plain.ps&&half.vs==plain.vs+plain.vs/2,what);
    }
    {   // a trimmed placement: the left border stays black, the share draws inside the left image only
      const StereoPlacement place[2]={{.25f,.25f,.75f,.75f},{.25f,.25f,.75f,.75f}};
      const Out placedBase=render(capture,0.f,nullptr,place,false),placed=render(capture,1.f,nullptr,place,true);
      bool border=true;
      for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x){const bool in=x>=32&&x<96&&y>=32&&y<96;if(!in)border=border&&at(placed.eye[0],x,y)==0xff000000u;}
      std::snprintf(what,sizeof(what),"F106 (%s, %s) A TRIMMED PLACEMENT: the left border stays black, the left image shows the right's inside it (64, 64 as the right eye's), the right eye is today's",
                    mode,kind);
      check(border&&at(placed.eye[0],64,64)==at(placedBase.eye[1],64,64)&&placed.eye[1]==placedBase.eye[1],what);
    }
    const Out fullAgain=render(capture,1.f,nullptr,nullptr,true);
    {   // the coverage figure (CPU, the same mapping on a grid) against what the GPU drew: blue is the share's weight there
      const auto weight=[&](uint32_t p){unsigned r=0,g=0,b=0;channels(p,r,g,b);return unorm?b/255.0:decodeSrgb(b/255.0);};
      double sum=0;unsigned n=0;
      for(unsigned j=0;j<48;++j)for(unsigned i=0;i<48;++i){const unsigned x=unsigned((i+.5)*size/48),y=unsigned((j+.5)*size/48);sum+=weight(at(fullAgain.eye[0],x,y));++n;}
      const double gpu=sum/n,cpu=f.renderer.shareFacts().coverage;
      std::snprintf(what,sizeof(what),"F106 (%s, %s) the coverage figure the renderer computes on the CPU (%.3f) matches the share the GPU drew (%.3f) within 0.03",mode,kind,cpu,gpu);
      check(std::fabs(gpu-cpu)<.03,what);
    }

    // ---- canted displays (+-10.75 deg) with asymmetric fovs ----
    f.views[0].pose.orientation=yawQuat(10.75);f.views[1].pose.orientation=yawQuat(-10.75);
    f.views[0].fov=fovDeg(-55,40,42,-48);f.views[1].fov=fovDeg(-40,55,42,-48);
    const Out cBase=render(capture,0.f,nullptr,nullptr,false),canted=render(capture,1.f,nullptr,nullptr,true);
    {
      unsigned tested=0,good=0;
      for(unsigned y=2;y<size;y+=5)for(unsigned x=2;x<size;x+=5) {
        double su=0,sv=0,edge=0;
        if(!rigMap(f.views,x,y,su,sv,edge)||edge<3.6)continue;
        const double tx=su*size-.5,ty=sv*size-.5;   // in the right capture's texels, where the gradient is linear in each axis
        if(tx<0||ty<0||tx>size-1.0||ty>size-1.0)continue;
        ++tested;
        unsigned r=0,g=0,b=0;channels(at(canted.eye[0],x,y),r,g,b);
        const unsigned er=encode(2*tx/255),eg=encode(2*ty/255);
        if(std::abs(int(r)-int(er))<=2&&std::abs(int(g)-int(eg))<=2&&b==255)++good;
      }
      std::snprintf(what,sizeof(what),"F106 (b) (%s, %s) CANTED +-10.75 DEG, ASYMMETRIC FOVS: each of the %u left pixels inside the coverage shows the right capture's texel the rig's own "
                    "quaternion arithmetic puts its direction on (red and green within 2); %u did",mode,kind,tested,good);
      check(tested>100&&good==tested,what);
      // the world's straight ahead, at its own pixel in each eye, is the same texel
      const double tl=std::tan(-55*kSharePi/180),tr=std::tan(40*kSharePi/180),tu=std::tan(42*kSharePi/180),td=std::tan(-48*kSharePi/180);
      const unsigned lx=unsigned((std::tan(10.75*kSharePi/180)-tl)/(tr-tl)*size),ly=unsigned(tu/(tu-td)*size);
      double su=0,sv=0,edge=0;
      const bool mapped=rigMap(f.views,lx,ly,su,sv,edge)&&edge>3.6;
      const unsigned rx=unsigned(su*size),ry=unsigned(sv*size);
      unsigned r0=0,g0=0,b0=0,r1=0,g1=0,b1=0;channels(at(canted.eye[0],lx,ly),r0,g0,b0);channels(at(cBase.eye[1],rx<size?rx:0,ry<size?ry:0),r1,g1,b1);
      std::snprintf(what,sizeof(what),"F106 (b) (%s, %s) the world's straight ahead, at the left eye's pixel (%u, %u) and the right eye's (%u, %u), is the same texel in both (within a pixel's step)",
                    mode,kind,lx,ly,rx,ry);
      check(mapped&&rx<size&&ry<size&&std::abs(int(r0)-int(r1))<=4&&std::abs(int(g0)-int(g1))<=4&&b0==255,what);
      const auto& facts=f.renderer.shareFacts();
      std::snprintf(what,sizeof(what),"F106 (%s, %s) the facts of the canted share: yaw %+.3f (21.5 wanted), pitch %+.3f, roll %+.3f, coverage %.3f",mode,kind,
                    double(facts.rotationDeg[0]),double(facts.rotationDeg[1]),double(facts.rotationDeg[2]),double(facts.coverage));
      check(facts.drawn&&std::fabs(facts.rotationDeg[0]-21.5f)<1e-3f&&std::fabs(facts.rotationDeg[1])<1e-3f&&std::fabs(facts.rotationDeg[2])<1e-3f&&
            facts.coverage>.3f&&facts.coverage<.9f,what);
    }
    {   // (c) outside the right fov the left eye keeps its own image; a band of about 3 deg between
      bool own=true;unsigned outside=0;
      for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x) {
        double su=0,sv=0,edge=0;
        if(rigMap(f.views,x,y,su,sv,edge)&&edge>-.05)continue;
        ++outside;own=own&&at(canted.eye[0],x,y)==at(cBase.eye[0],x,y);
      }
      std::snprintf(what,sizeof(what),"F106 (c) (%s, %s) every left pixel whose direction is outside the right fov (%u of them) keeps the left eye's own image, bit for bit",mode,kind,outside);
      check(outside>500&&own,what);
      // along the middle row the blue (the share's weight) goes 0, a ramp, full, monotonically; the ramp about 3 deg wide in the left view
      const unsigned row=size/2;int first=-1,last=-1;bool monotone=true;unsigned prev=0;
      for(unsigned x=0;x<size;++x){unsigned r=0,g=0,b=0;channels(at(canted.eye[0],x,row),r,g,b);if(b<prev)monotone=false;prev=b;if(b>0&&first<0)first=int(x);if(b==255&&last<0)last=int(x);}
      const auto angle=[&](int x){const double l=std::tan(double(f.views[0].fov.angleLeft)),r=std::tan(double(f.views[0].fov.angleRight));return std::atan(l+(r-l)*(x+.5)/size)*180/kSharePi;};
      const double width=first>=0&&last>first?angle(last)-angle(first):-1;
      std::snprintf(what,sizeof(what),"F106 (c) (%s, %s) the band: along the middle row the share rises from 0 (column %d) to full (column %d) monotonically, over %.2f deg of the left view "
                    "(3 wanted, within a pixel)",mode,kind,first,last,width);
      check(monotone&&first>0&&last>first+1&&width>1.8&&width<4.6,what);
    }

    // ---- (f) the right eye's hidden area: parallel eyes again; the right capture green with red where the game masked it, the left blue ----
    f.views[0].pose.orientation=f.views[1].pose.orientation=yawQuat(0);
    f.views[0].fov=f.views[1].fov=fovDeg(-45,45,45,-45);
    const XrVector2f vtx[6]={{-1.f,1.f},{-.2f,1.f},{-1.f,.2f},{1.f,-1.f},{.3f,-1.f},{1.f,-.3f}};const uint32_t idx[6]={0,1,2,3,4,5};
    const auto maskedTexel=[](unsigned x,unsigned y){const double tx=-1+2*(x+.5)/size,ty=1-2*(y+.5)/size;return (tx+1)+(1-ty)<=.8||(1-tx)+(ty+1)<=.7;};
    auto green=shareTexture(f,[&](unsigned x,unsigned y){return maskedTexel(x,y)?rgbaOf(255,0,0):rgbaOf(0,255,0);});
    auto blue=shareTexture(f,[](unsigned,unsigned){return rgbaOf(0,0,255);});
    EyeCapture hiddenCapture;check(SUCCEEDED(hiddenCapture.initialize(f.runtime.device.Get())),"hidden capture owner");
    const vr::Texture_t bt{blue.Get(),vr::API_DirectX,vr::ColorSpace_Auto},gt{green.Get(),vr::API_DirectX,vr::ColorSpace_Auto};
    check(hiddenCapture.capture(vr::Eye_Left,&bt)==vr::VRCompositorError_None&&hiddenCapture.capture(vr::Eye_Right,&gt)==vr::VRCompositorError_None,"hidden capture");
    const StereoHiddenArea area{vtx,6,idx,6,7};
    const Out hBase=render(hiddenCapture,0.f,nullptr,nullptr,false),hidden=render(hiddenCapture,1.f,&area,nullptr,true);
    {
      bool noRed=true,noBlack=true;unsigned leftOwn=0,taken=0;
      for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x) {
        unsigned r=0,g=0,b=0;channels(at(hidden.eye[0],x,y),r,g,b);
        noRed=noRed&&r==0;noBlack=noBlack&&g+b>=250;   // blue and green blended: their sum stays about 255 or more, never near black
        if(at(hidden.eye[0],x,y)==at(hBase.eye[0],x,y))++leftOwn;
        if(at(hidden.eye[0],x,y)==at(hBase.eye[1],x,y))++taken;
      }
      unsigned rr=0,rg=0,rb=0;channels(at(hBase.eye[1],2,2),rr,rg,rb);
      std::snprintf(what,sizeof(what),"F106 (f) (%s, %s) NO TEXEL OF THE RIGHT EYE'S HIDDEN AREA REACHES THE LEFT EYE: no left pixel carries any red (the right eye's own masked corners are red), "
                    "and none is black; %u left pixels kept their own image, %u took the right's",mode,kind,leftOwn,taken);
      check(rr==255&&noRed&&noBlack&&leftOwn>0&&taken>0,what);
      check(at(hidden.eye[0],12,12)==at(hBase.eye[0],12,12)&&at(hidden.eye[0],123,123)==at(hBase.eye[0],123,123)&&at(hidden.eye[0],64,64)==at(hBase.eye[1],64,64),
            "F106 (f) inside the hidden corners the left eye is its own; at the centre it is the right eye's");
      const auto& facts=f.renderer.shareFacts();
      check(facts.drawn&&facts.hiddenTriangles==2&&facts.coverage>.5f&&facts.coverage<.85f,"F106 (f) the facts: the mask's two triangles, and less of the left eye taken than with no mask");
      // the control: with no mesh given, the same corner pixel, well inside the right fov, does show the right eye's red; the mask is what keeps it out
      const Out bare=render(hiddenCapture,1.f,nullptr,nullptr,true);
      unsigned r=0,g=0,b=0;channels(at(bare.eye[0],12,12),r,g,b);
      check(f.renderer.shareFacts().hiddenTriangles==0&&r==255,"F106 (f) the control: with no mesh the red of the right eye's masked corner reaches the left eye at (12, 12)");
      const Out zero=render(hiddenCapture,0.f,&area,nullptr,true);
      check(zero.eye[0]==hBase.eye[0]&&zero.eye[1]==hBase.eye[1],"F106 (d) share 0 with a hidden area given is still today's image, bit for bit");
    }
    std::snprintf(what,sizeof(what),"F106 (%s, %s) the renderer shuts down cleanly after the share, its mask included",mode,kind);
    check(f.renderer.shutdown()==XR_SUCCESS&&f.runtime.destroys==2,what);
  }
}
