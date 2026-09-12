#include "../../src/openxr/projection_math.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <initializer_list>

namespace {
bool near(float a, float b, float eps = 2e-5f) { return std::fabs(a - b) <= eps; }
bool check(bool value, const char* what) {
  if (!value) std::printf("FAIL: %s\n", what);
  return value;
}
bool same(const vr::HmdMatrix44_t& a, const vr::HmdMatrix44_t& b) {
  return std::memcmp(&a, &b, sizeof(a)) == 0;
}

bool selfTest() {
  bool ok = true;
  const XrFovf fov{-0.61f, 0.83f, 0.47f, -0.36f};
  edvr::openxr::RawFov raw{0, 0, 0, 0};
  ok &= check(edvr::openxr::fovToRaw(fov, raw), "asymmetric FOV conversion");
  ok &= check(near(raw.left, -0.69891886f) && near(raw.right, 1.09343292f) &&
              near(raw.top, -0.37640285f) && near(raw.bottom, 0.50796590f),
              "independent raw tangent expectations");

  vr::HmdMatrix44_t dx{};
  ok &= check(edvr::openxr::projectionMatrix(fov, 0.1f, 1000.0f,
                                              vr::API_DirectX, dx), "DX matrix");
  const vr::HmdMatrix44_t dxExpected{{
      {1.1158524f, 0.0f, 0.2201097f, 0.0f},
      {0.0f, 2.2615001f, 0.1487649f, 0.0f},
      {0.0f, 0.0f, -1.0001000f, -0.1000100f},
      {0.0f, 0.0f, -1.0f, 0.0f}}};
  for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c)
    ok &= check(near(dx.m[r][c], dxExpected.m[r][c]), "DX independent entries");

  vr::HmdMatrix44_t gl{};
  ok &= check(edvr::openxr::projectionMatrix(fov, 0.25f, 10.0f,
                                              vr::API_OpenGL, gl), "GL matrix");
  const vr::HmdMatrix44_t glExpected{{
      {1.1158524f, 0.0f, 0.2201097f, 0.0f},
      {0.0f, 2.2615001f, 0.1487649f, 0.0f},
      {0.0f, 0.0f, -1.05128205f, -0.5128205f},
      {0.0f, 0.0f, -1.0f, 0.0f}}};
  for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c)
    ok &= check(near(gl.m[r][c], glExpected.m[r][c]), "GL independent entries");
  ok &= check(near((-dx.m[2][2] * 0.1f + dx.m[2][3]) / 0.1f, 0.0f) &&
              near((-dx.m[2][2] * 1000.0f + dx.m[2][3]) / 1000.0f, 1.0f),
              "DX near/far depth endpoints");
  ok &= check(near((-gl.m[2][2] * 0.25f + gl.m[2][3]) / 0.25f, -1.0f) &&
              near((-gl.m[2][2] * 10.0f + gl.m[2][3]) / 10.0f, 1.0f),
              "GL near/far depth endpoints");

  const XrFovf capturedLeft{
      std::atan(-1.52926576f), std::atan(1.03239238f),
      std::atan(1.26479411f), std::atan(-1.26479411f)};
  const XrFovf capturedRight{
      std::atan(-1.03239238f), std::atan(1.52926576f),
      std::atan(1.26479411f), std::atan(-1.26479411f)};
  edvr::openxr::RawFov leftRaw{}, rightRaw{};
  ok &= check(edvr::openxr::fovToRaw(capturedLeft, leftRaw) &&
              near(leftRaw.left, -1.52926576f) && near(leftRaw.right, 1.03239238f) &&
              near(leftRaw.top, -1.26479411f) && near(leftRaw.bottom, 1.26479411f),
              "captured left eye regression");
  ok &= check(edvr::openxr::fovToRaw(capturedRight, rightRaw) &&
              near(rightRaw.left, -1.03239238f) && near(rightRaw.right, 1.52926576f) &&
              near(rightRaw.top, -1.26479411f) && near(rightRaw.bottom, 1.26479411f),
              "captured right eye regression");
  const XrFovf capturedEyes[] = {capturedLeft, capturedRight};
  const float planePairs[][2] = {{0.025f, 50000.0f}, {0.1f, 1000.0f},
                                 {1.0f, 50000.0f}};
  const float expectedDepth[][2] = {{-1.00000048f, -0.0250000115f},
                                    {-1.0000999f, -0.100009993f},
                                    {-1.00002003f, -1.00002003f}};
  for (unsigned eye=0;eye<2;++eye) for (unsigned i=0;i<3;++i) {
    vr::HmdMatrix44_t m{};
    ok &= check(edvr::openxr::projectionMatrix(capturedEyes[eye], planePairs[i][0],
                 planePairs[i][1], vr::API_DirectX, m), "both eyes/all captured plane pairs");
    const vr::HmdMatrix44_t expected{{
        {.780744314f,0,eye ? .193965539f : -.193965539f,0},
        {0,.7906425f,0,0}, {0,0,expectedDepth[i][0],expectedDepth[i][1]}, {0,0,-1,0}}};
    for(unsigned row=0;row<4;++row) for(unsigned col=0;col<4;++col)
      ok &= check(near(m.m[row][col],expected.m[row][col],2e-6f), "captured full matrix");
  }
  // View-space rays at each edge must reach the corresponding NDC edge.
  // Use independent tangent constants; this catches a vertical top/bottom swap.
  for(const auto& m : {dx, gl}) {
    ok &= check(near(m.m[0][0]*-.69891886f-m.m[0][2],-1) &&
                near(m.m[0][0]*1.09343292f-m.m[0][2],1) &&
                near(m.m[1][1]*-.37640285f-m.m[1][2],-1) &&
                near(m.m[1][1]*.50796590f-m.m[1][2],1), "asymmetric edge rays, negative forward Z");
  }

  const XrFovf invalids[] = {
      {0, 0, .5f, -.5f}, {-.5f, .5f, -.2f, .2f},
      {-1.5707963267948966f, .5f, .5f, -.5f},
      {NAN, .5f, .5f, -.5f}};
  for (const auto& bad : invalids) {
    edvr::openxr::RawFov untouched{7, 8, 9, 10};
    ok &= check(!edvr::openxr::fovToRaw(bad, untouched) && untouched.left == 7 &&
                untouched.right == 8 && untouched.top == 9 && untouched.bottom == 10,
                "invalid FOV unchanged");
  }
  vr::HmdMatrix44_t canary{};
  for(unsigned row=0;row<4;++row) for(unsigned col=0;col<4;++col) canary.m[row][col]=float(4*row+col+1);
  const float invalidPlanes[][2]={{0,10},{-1,10},{1,1},{2,1},{NAN,10},{1,NAN},{INFINITY,10},{1,INFINITY},{3e38f,3.3e38f}};
  for(const auto& planes:invalidPlanes) for(auto api:{vr::API_DirectX,vr::API_OpenGL}) {
    auto m=canary;
    ok &= check(!edvr::openxr::projectionMatrix(fov,planes[0],planes[1],api,m) && same(m,canary),
                "invalid/unrepresentable planes preserve entire output");
  }
  auto untouched=canary;
  ok &= check(!edvr::openxr::projectionMatrix(fov,.1f,10,static_cast<vr::EGraphicsAPIConvention>(9),untouched) && same(untouched,canary),
              "unsupported convention preserves output");
  ok &= check(edvr::openxr::projectionMatrix(fov,1e20f,2e20f,vr::API_DirectX,untouched) &&
              near(untouched.m[2][2],-2) && std::fabs(untouched.m[2][3]/-2e20f-1)<1e-6f, "large representable planes");
  const XrFovf tinyWidth{0,(std::numeric_limits<float>::denorm_min)(),.5f,-.5f};
  untouched=canary;
  ok &= check(!edvr::openxr::projectionMatrix(tinyWidth,.1f,10,vr::API_DirectX,untouched) && same(untouched,canary),
              "unrepresentable scale preserves output");
  return ok;
}
}

int main(int argc, char** argv) {
  if (argc==2 && std::strcmp(argv[1],"--dry-run")==0) {
    std::puts("Would test pure OpenXR projection conversions; no files or runtime calls."); return 0;
  }
  if(argc!=2 || std::strcmp(argv[1],"--self-test")!=0) {
    std::puts("usage: openxr_projection_test --self-test | --dry-run"); return 2;
  }
  const bool ok=selfTest();
  std::printf("openxr_projection_test: %s\n",ok?"PASS":"FAIL");return ok?0:1;
}
