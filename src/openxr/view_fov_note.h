#pragma once
// The views' field of view, said in the runtime log (F194). No line named it before: the pilot's Quest 3 and the tester's both put look to aim's aim about 13 deg off where they looked, and the frustum
// that explains it (up 0.9657, down 1.4281 as tangents: the middle of the picture 13.0 deg below the pose's forward axis) was only in the graphics half's channel. One line at the first located views with a
// valid fov, and again when a view's fov changes by more than 0.01 deg; never per frame. The angles are OpenXR's as located (angleLeft and angleDown negative), before EDVR's fov trim or the scope zoom's
// window; when the frustum the game is told differs from them by more than that (a trim), the line says that too.
// F194b: F194 part 1, which aimed look to aim at this picture centre, was removed (refuted on the pilot's Quest: his gaze offset is his own, about 5 deg against the picture's 12.2); the line stays as a measurement with no key,
// for headset reports.
//
// The picture centre: the middle of a view's image on the tangent plane is the centre of its tangent span, so its angle from the view's forward axis is atan((tan(up) + tan(down)) / 2) vertically (angleDown is
// negative) and atan((tan(left) + tan(right)) / 2) horizontally, signed as the angles are (up and right positive). Both eyes share the vertical; the horizontals mirror, so the two eyes' mean is 0.
#include <openxr/openxr.h>
#include <cmath>
#include <cstddef>
#include <cstdio>

namespace edvr::openxr {

namespace viewfov {

constexpr double kPi = 3.14159265358979323846;
constexpr double kChangeDeg = 0.01;   // a view's angle moved by more than this is a change
inline double deg(double radians) { return radians * 180.0 / kPi; }
inline bool valid(const XrFovf& f) {
  const double a[4] = {f.angleLeft, f.angleRight, f.angleUp, f.angleDown};
  for(double v:a) if(!std::isfinite(v)||std::fabs(v)>=kPi*0.5) return false;
  return f.angleLeft<f.angleRight&&f.angleDown<f.angleUp;
}
// The middle of the image from the view's forward axis, degrees: vertical (up +) and horizontal (right +).
inline double centreVerticalDeg(const XrFovf& f) {return deg(std::atan((std::tan(double(f.angleUp))+std::tan(double(f.angleDown)))*0.5));}
inline double centreHorizontalDeg(const XrFovf& f) {return deg(std::atan((std::tan(double(f.angleLeft))+std::tan(double(f.angleRight)))*0.5));}
inline const char* side(double verticalDeg) {return verticalDeg<-0.05?"below":verticalDeg>0.05?"above":"from";}   // "13.0 deg below the forward axis", "0.0 deg from it"
inline bool moved(const XrFovf& a, const XrFovf& b) {
  const double d[4] = {double(a.angleLeft)-b.angleLeft, double(a.angleRight)-b.angleRight, double(a.angleUp)-b.angleUp, double(a.angleDown)-b.angleDown};
  for(double v:d) if(std::fabs(deg(v))>kChangeDeg) return true;
  return false;
}
inline bool movedPair(const XrFovf (&a)[2], const XrFovf (&b)[2]) {return moved(a[0],b[0])||moved(a[1],b[1]);}

}  // namespace viewfov

struct ViewFovNote {
  bool printed=false, printedGameDiffers=false;
  XrFovf last[2]{}, lastGame[2]{};
  // The line to write for this frame's located views (and the ones the game is told, when the caller has them), or false when there is nothing new to say. A view whose fov is not valid says nothing
  // and changes nothing: the next frame that locates decides.
  bool note(const XrFovf (&located)[2], const XrFovf (*game)[2], char* line, size_t size) {
    if(!viewfov::valid(located[0])||!viewfov::valid(located[1])) return false;
    const bool gameOk=game&&viewfov::valid((*game)[0])&&viewfov::valid((*game)[1]);
    const bool differs=gameOk&&viewfov::movedPair(located,*game);
    const bool say=!printed||viewfov::movedPair(located,last)||(gameOk&&(differs!=printedGameDiffers||(differs&&viewfov::movedPair(*game,lastGame))));
    if(!say) return false;
    printed=true;last[0]=located[0];last[1]=located[1];
    printedGameDiffers=differs;
    if(differs){lastGame[0]=(*game)[0];lastGame[1]=(*game)[1];}
    format(line,size,located,differs?game:nullptr);
    return true;
  }
  static int format(char* line, size_t size, const XrFovf (&located)[2], const XrFovf (*game)[2]) {
    const double vertical=viewfov::centreVerticalDeg(located[0]);
    const XrFovf (&l)[2]=located;
    char tail[200]{};
    if(game) {
      const XrFovf (&g)[2]=*game;
      std::snprintf(tail,sizeof(tail),"; the game is told (a trim or the scope zoom) eye 0 left %+.2f right %+.2f up %+.2f down %+.2f deg: its picture centre %.1f deg %s the forward axis",
        viewfov::deg(g[0].angleLeft),viewfov::deg(g[0].angleRight),viewfov::deg(g[0].angleUp),viewfov::deg(g[0].angleDown),
        std::fabs(viewfov::centreVerticalDeg(g[0])),viewfov::side(viewfov::centreVerticalDeg(g[0])));
    }
    return std::snprintf(line,size,"view fov: eye 0 left %+.2f right %+.2f up %+.2f down %+.2f deg, eye 1 left %+.2f right %+.2f up %+.2f down %+.2f deg; picture centre %.1f deg %s the forward axis "
      "(eye 0 horizontal %+.1f, eye 1 %+.1f)%s",
      viewfov::deg(l[0].angleLeft),viewfov::deg(l[0].angleRight),viewfov::deg(l[0].angleUp),viewfov::deg(l[0].angleDown),
      viewfov::deg(l[1].angleLeft),viewfov::deg(l[1].angleRight),viewfov::deg(l[1].angleUp),viewfov::deg(l[1].angleDown),
      std::fabs(vertical),viewfov::side(vertical),viewfov::centreHorizontalDeg(l[0]),viewfov::centreHorizontalDeg(l[1]),tail);
  }
};

}  // namespace edvr::openxr
