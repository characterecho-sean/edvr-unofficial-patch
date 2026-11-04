#pragma once
// F194: the views' field of view in the runtime log (src/openxr/view_fov_note.h). The headset's frustum was only in the graphics half's channel; a Quest 3's is not symmetric vertically (up 0.9657,
// down 1.4281 as tangents: the middle of the picture 13.0 deg below the pose's forward axis, which is where look to aim's aim sat 13 deg above), a Pimax Crystal Super's is. The line is said at the first
// located views with a valid fov and again when a view's fov moves by more than 0.01 deg, never per frame; the host calls it at the two places it locates the views and, for a trim, before the
// scope zoom's window. The numbers are checked against the tangents written out here, not against the header's own formulas.
#include "parallel_projection_notice_cases.h"
#include "../../src/openxr/view_fov_note.h"
#include <cmath>
#include <cstring>
#include <string>

namespace edvr::openxr::test {
namespace view_fov_fixture {
constexpr double kPi=3.14159265358979323846;
inline XrFovf fovOfTangents(double left,double right,double up,double down){
  return XrFovf{float(std::atan(left)),float(std::atan(right)),float(std::atan(up)),float(std::atan(down))};
}
// The Quest 3 as Virtual Desktop reports it: vertical 0.9657 up, 1.4281 down; horizontal outer 1.19, nasal 0.95 (the eyes mirror).
inline void quest(XrFovf (&out)[2]){
  out[0]=fovOfTangents(-1.19,0.95,0.9657,-1.4281);
  out[1]=fovOfTangents(-0.95,1.19,0.9657,-1.4281);
}
inline void pimax(XrFovf (&out)[2]){
  out[0]=fovOfTangents(-1.30,1.05,1.2648,-1.2648);
  out[1]=fovOfTangents(-1.05,1.30,1.2648,-1.2648);
}
}  // namespace view_fov_fixture
template<class Check> void runViewFovNoteCases(Check&& check) {
  using namespace view_fov_fixture;
  using parallel_fixture::readSource;
  char line[768]{};
  {
    XrFovf q[2];quest(q);
    check(std::fabs(viewfov::centreVerticalDeg(q[0])-(-13.0))<0.05&&std::fabs(viewfov::centreVerticalDeg(q[1])-(-13.0))<0.05,
      "view fov: a Quest 3's picture centre is 13.0 deg below the forward axis (atan((0.9657 - 1.4281) / 2) = -13.0), the same in both eyes");
    check(std::fabs(viewfov::centreVerticalDeg(q[0])-std::atan((0.9657-1.4281)/2.0)*180.0/kPi)<0.01,"view fov: ... which is the tangents' centre, written out here");
    check(std::fabs(viewfov::centreHorizontalDeg(q[0])+viewfov::centreHorizontalDeg(q[1]))<0.01&&viewfov::centreHorizontalDeg(q[0])<-3.0&&viewfov::centreHorizontalDeg(q[1])>3.0,
      "view fov: the eyes' horizontal centres mirror (the left eye's to the left, the right eye's to the right), so their mean is 0");
    XrFovf p[2];pimax(p);
    check(std::fabs(viewfov::centreVerticalDeg(p[0]))<1e-4&&std::fabs(viewfov::centreVerticalDeg(p[1]))<1e-4,"view fov: a Pimax Crystal Super's picture centre is on the forward axis (1.2648 up and down)");
  }
  {
    ViewFovNote note;XrFovf q[2];quest(q);
    check(note.note(q,nullptr,line,sizeof(line)),"view fov: the first located views with a valid fov are said");
    check(std::strstr(line,"view fov: eye 0 left ")==line&&std::strstr(line,"deg, eye 1 left ")!=nullptr&&std::strstr(line,"picture centre 13.0 deg below the forward axis")!=nullptr&&
          std::strstr(line,"(eye 0 horizontal -")!=nullptr&&std::strstr(line,", eye 1 +")!=nullptr&&std::strstr(line,"the game is told")==nullptr&&std::strlen(line)<400,
      "view fov: the line gives both eyes' four angles, the picture centre 13.0 deg below the forward axis and the eyes' horizontal centres, and nothing about the game when it is not given");
    check(std::strstr(line,"up +44.0")!=nullptr||std::strstr(line,"up +43.9")!=nullptr||std::strstr(line,"up +44.")!=nullptr,"view fov: the angles are degrees, up positive (the Quest's 0.9657 is about 44 deg)");
    check(!note.note(q,nullptr,line,sizeof(line)),"view fov: the same frame again says nothing (not per frame)");
    XrFovf jitter[2]={q[0],q[1]};jitter[0].angleUp+=float(0.005*kPi/180.0);
    check(!note.note(jitter,nullptr,line,sizeof(line)),"view fov: a change of 0.005 deg is under the 0.01 deg and says nothing");
    XrFovf moved[2]={q[0],q[1]};moved[1].angleLeft-=float(0.02*kPi/180.0);
    check(note.note(moved,nullptr,line,sizeof(line)),"view fov: one view's angle moved by 0.02 deg is said again, from either eye");
    check(!note.note(moved,nullptr,line,sizeof(line)),"view fov: ... once");
    XrFovf drift[2]={moved[0],moved[1]};drift[0].angleDown+=float(0.007*kPi/180.0);
    check(!note.note(drift,nullptr,line,sizeof(line)),"view fov: the change is measured from what was last said, so a second small step of 0.007 deg is still under 0.01 deg");
    XrFovf bad[2]={q[0],q[1]};bad[0].angleUp=std::nanf("");
    check(!note.note(bad,nullptr,line,sizeof(line))&&!note.note(drift,nullptr,line,sizeof(line)),"view fov: a view whose fov is not numbers says nothing and changes nothing");
    XrFovf inverted[2]={q[0],q[1]};inverted[1].angleRight=inverted[1].angleLeft-0.1f;
    check(!note.note(inverted,nullptr,line,sizeof(line)),"view fov: a view whose left is not left of its right says nothing");
  }
  {
    ViewFovNote note;XrFovf p[2];pimax(p);
    note.note(p,nullptr,line,sizeof(line));
    check(std::strstr(line,"picture centre 0.0 deg from the forward axis")!=nullptr,"view fov: a Pimax's line says 0.0 deg from the forward axis");
  }
  {
    ViewFovNote note;XrFovf q[2];quest(q);
    note.note(q,nullptr,line,sizeof(line));
    XrFovf same[2]={q[0],q[1]};
    check(!note.note(q,&same,line,sizeof(line)),"view fov: the frustum the game is told equal to the located one adds nothing");
    XrFovf trimmed[2]={q[0],q[1]};
    for(int e=0;e<2;++e){trimmed[e].angleUp-=float(10.0*kPi/180.0);trimmed[e].angleDown+=float(10.0*kPi/180.0);}
    check(note.note(q,&trimmed,line,sizeof(line))&&std::strstr(line,"the game is told (a trim or the scope zoom) eye 0 left ")!=nullptr&&std::strstr(line,"its picture centre ")!=nullptr&&
          std::strstr(line,"picture centre 13.0 deg below the forward axis (eye 0")!=nullptr,"view fov: a trim is said beside the located frustum: the located picture centre first, the game's after");
    check(!note.note(q,&trimmed,line,sizeof(line)),"view fov: ... once");
    XrFovf trimmed2[2]={trimmed[0],trimmed[1]};trimmed2[0].angleUp-=float(1.0*kPi/180.0);
    check(note.note(q,&trimmed2,line,sizeof(line)),"view fov: the trim changing is said again");
    check(note.note(q,&same,line,sizeof(line))&&std::strstr(line,"the game is told")==nullptr,"view fov: the trim ending is said (the line without the game's part)");
  }
  // ---- the wiring, as source pins ----
  const std::string host=parallel_fixture::readSource("src\\openxr\\native_runtime_host.h");
  check(!host.empty(),"view fov wiring: the host source is readable from the repo root");
  if(host.empty())return;
  const size_t loadingAt=host.find("if(valid)noteViewFov(located,nullptr);");
  const size_t gameAt=host.find("if(locatedValid)noteViewFov(located,nullptr);");
  const size_t toldAt=host.find("noteViewFov(located,&gameGeometry);");
  const size_t zoomAt=host.find("zoomApplied=applyScopeZoom(gameGeometry,zoomPreRaw,zoomToldRaw);");
  check(loadingAt!=std::string::npos&&gameAt!=std::string::npos&&toldAt!=std::string::npos&&zoomAt!=std::string::npos&&loadingAt<gameAt&&gameAt<toldAt&&toldAt<zoomAt&&zoomAt-toldAt<400,
    "view fov wiring: the loading frames and the game frames say the located views, and the frustum the game is told is said after the trim and before the scope zoom's window moves it");
  check(host.find("#include \"view_fov_note.h\"")!=std::string::npos&&host.find("ViewFovNote viewFovNote;")!=std::string::npos&&host.find("if(viewFovNote.note(seen,told?&toldFov:nullptr,line,sizeof(line)))nativeTracePuts(line);")!=std::string::npos,
    "view fov wiring: the host holds one note, and writes the line it returns to the runtime log");
}
}  // namespace edvr::openxr::test
