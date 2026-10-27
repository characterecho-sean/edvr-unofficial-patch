#pragma once
// World scale on the host (docs/world-scale.md; src/common/world_scale.h): the latch that moves only on a frame that located its eyes, and the wiring as source pins.
// The pins hold what the unit rigs cannot drive end to end: the value is applied to the geometry Elite and the temporal pass are given and to nothing else (the
// layer's views, the menu's and the plugins' stay the located ones), it is taken from this frame's provider answer before either is built, and the readers of
// eyeToHead are the ones that were classified. A new reader fails the last pin by name: decide whether it reconstructs or checks Elite's camera (it must see the
// scaled pair) or draws EDVR's own content (it must see the real eyes), then add it to the list.
#include "parallel_projection_notice_cases.h"
#include "../../src/common/world_scale.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace edvr::openxr::test {
namespace world_scale_fixture {
inline GeometryInput located() {
  GeometryInput in{};in.generation=1;in.sequence=1;
  in.viewFlags=XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT;
  in.headFlags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_VALID_BIT;
  in.headPose.orientation.w=1;
  for(unsigned eye=0;eye<2;++eye) {
    in.views[eye].pose.orientation.w=1;in.views[eye].pose.position={eye?0.0315f:-0.0315f,0.0f,0.0f};
    in.views[eye].fov={-0.6f,0.7f,0.5f,-0.4f};in.width[eye]=1000;in.height[eye]=900;
  }
  return in;
}
// The files under src that name eyeToHead, as repo-relative paths with backslashes, sorted.
inline std::vector<std::string> readersOfEyeToHead() {
  std::vector<std::string> found;
  for(const char* directory:{"src\\openxr","src\\common","src\\d3d11"}) {
    WIN32_FIND_DATAA data{};
    const std::string base=std::string(directory)+"\\";
    HANDLE handle=FindFirstFileA((base+"*").c_str(),&data);
    if(handle==INVALID_HANDLE_VALUE)continue;
    do {
      const std::string name=data.cFileName;
      auto ends=[&](const char* suffix){const size_t n=std::strlen(suffix);return name.size()>n&&name.compare(name.size()-n,n,suffix)==0;};
      if(!(ends(".h")||ends(".cpp"))||(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))continue;
      if(parallel_fixture::has(parallel_fixture::readSource((base+name).c_str()),"eyeToHead"))found.push_back(base+name);
    } while(FindNextFileA(handle,&data));
    FindClose(handle);
  }
  std::sort(found.begin(),found.end());
  return found;
}
}
template<class Check> void runWorldScaleCases(Check&& check) {
  using namespace parallel_fixture;
  using namespace world_scale_fixture;
  // ---- the host's latch: a value is latched by the frame that located its eyes, and only by one ----
  {
    launch_fixture::Fixture f;auto& h=f.host;
    check(h.worldScaleLatched==100,"world scale: nothing is latched, and nothing said, before a frame says otherwise");
    GeometryInput real=located(),told=real;
    told.worldScalePercent=130;
    h.noteWorldScale(130,real,told);
    check(h.worldScaleLatched==130,"world scale: a frame that located its eyes latches the value it was asked for");
    GeometryInput lost=real;lost.headFlags=0;
    GeometryInput told80=lost;told80.worldScalePercent=80;
    h.noteWorldScale(80,lost,told80);
    check(h.worldScaleLatched==130,"world scale: a frame whose eyes did not locate leaves the latched value where it was (the change is said by the next frame that locates)");
    told.worldScalePercent=80;
    h.noteWorldScale(80,real,told);
    check(h.worldScaleLatched==80,"world scale: the next frame that locates latches 80");
    told.worldScalePercent=100;
    h.noteWorldScale(100,real,told);
    check(h.worldScaleLatched==100,"world scale: and the way back to 100 is latched too");
  }
  // ---- the wiring, as source pins ----
  const std::string host=readSource("src\\openxr\\native_runtime_host.h");
  const std::string temporalClient=readSource("src\\openxr\\native_temporal_client.h");
  const std::string menuClient=readSource("src\\openxr\\native_menu_client.h");
  const std::string frameClient=readSource("src\\openxr\\native_frame_client.h");
  const std::string stereo=readSource("src\\openxr\\d3d11_stereo.cpp");
  check(!host.empty()&&!temporalClient.empty()&&!menuClient.empty()&&!frameClient.empty()&&!stereo.empty(),"world scale wiring: the sources the pins read are readable from the repo root");
  if(host.empty()||temporalClient.empty()||menuClient.empty()||frameClient.empty()||stereo.empty())return;
  const size_t fromAnswer=host.find("worldScale=edvr::worldScaleClamp(featureFrame.worldScalePercent);");
  const size_t answered=host.find("featureFrame=next;featureFrameKnown=true;");
  const size_t applied=host.find("gameGeometry.worldScalePercent=worldScale;");
  const size_t treated=host.find("auto treatedGeometry=gameGeometry;");
  const size_t temporalBegin=host.find("temporal.begin(treatedGeometry,");
  const size_t published=host.find("geometry.publish(gameGeometry,");
  check(answered!=std::string::npos&&fromAnswer!=std::string::npos&&applied!=std::string::npos&&treated!=std::string::npos&&temporalBegin!=std::string::npos&&
            published!=std::string::npos&&answered<fromAnswer&&fromAnswer<applied&&applied<treated&&treated<temporalBegin&&temporalBegin<published&&
            count(host,"worldScalePercent=")==1,
        "world scale wiring: this frame's provider answer is latched once, before the geometry the temporal pass and the system publication are built from, and both are built from it");
  check(has(host,"frameGeometry=located;frameGeometryAvailable=true;")&&has(host,"frameViews[0]=located.views[0];frameViews[1]=located.views[1];\n    frameContentViews[0]=frameViews[0];")&&
            has(host,"features.begin(located,")&&has(host,"makeGeometrySnapshot(located,temporalGeometry)")&&!has(host,"frameGeometry=gameGeometry")&&!has(host,"frameViews[0]=gameGeometry")&&
            has(host,"auto menuGeometry=frameGeometry;"),
        "world scale wiring: the layer's views, the menu's, the plugins' and the frame input stay the located ones; only the game's and the temporal pass's geometry is scaled");
  check(has(stereo,"v.pose=views[i].pose;")&&!has(stereo,"eyeToHead")&&!has(menuClient,"eyeToHead")&&!has(frameClient,"eyeToHead")&&has(menuClient,"detail::rigid(geometry.views[eye].pose, matrix);"),
        "world scale wiring: the projection layer's pose is the located view, and the menu client takes the located views, not the snapshot's eye transforms");
  check(has(temporalClient,"makeGeometrySnapshot(geometry, snapshot)")&&has(temporalClient,"std::memcpy(frame.eyeToHead[eye], snapshot.eyeToHead[eye].m, sizeof(frame.eyeToHead[eye]));"),
        "world scale wiring: the temporal frame's eye transforms are the snapshot's, made from the geometry it is handed (the scaled one)");
  // ---- the readers of eyeToHead are the classified ones ----
  const std::vector<std::string> want={
    "src\\common\\native_temporal.h",          // (a) the temporal frame struct: the eye transforms as given to Elite
    "src\\d3d11\\native_temporal.cpp",         // (a) the temporal pass's eyes: motion, the ADS eye offsets, the history reset on a moved separation
    "src\\openxr\\canted_display.h",           // (a) gameHandedness: Elite's S*E*S of the snapshot's pair
    "src\\openxr\\geometry_snapshot.h",        // the one place the scale is applied
    "src\\openxr\\native_runtime_host.h",      // the change line's two separations
    "src\\openxr\\native_temporal_client.h",   // (a) the temporal frame
    "src\\openxr\\openvr_system.cpp",          // (a) GetEyeToHeadTransform and the IPD property
    "src\\openxr\\system_geometry.h",          // the future IVRSystem_012 read transaction (a rig reads it; default scale)
    "src\\openxr\\system_publication.h",       // (a) the publication Elite's answers are read from
    "src\\openxr\\system_source.h",            // (a) the cached optics
  };
  const std::vector<std::string> got=readersOfEyeToHead();
  std::string extra;
  for(const auto& name:got)if(std::find(want.begin(),want.end(),name)==want.end())extra+=" "+name;
  for(const auto& name:want)if(std::find(got.begin(),got.end(),name)==got.end())extra+=" (gone: "+name+")";
  if(!extra.empty())std::printf("world scale: the files that name eyeToHead changed:%s\n",extra.c_str());
  check(extra.empty(),"world scale: the files that name eyeToHead are the classified readers (a new one must be classified: it sees the scaled pair if it checks Elite's camera, the located eyes if it draws EDVR's own)");
}
} // namespace edvr::openxr::test
