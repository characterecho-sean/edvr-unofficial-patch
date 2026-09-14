#include "native_menu.h"
#include "menu_panel.h"
#include "input_gate.h"
#include "../common/frame_flag.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <wrl/client.h>

namespace {
struct State {
  ID3D11Device* device = nullptr; // NativeRenderBinding retains it through close
  DWORD thread = 0;
  uint64_t generation = 0, referenceGeneration = 0;
  float head[12]{}, eyes[2][12]{}, frusta[2][4]{};
  bool active = false, pose = false;
};
// Retain CPU contexts for the module's bounded 16 Init attempts. A stale table
// cannot address a new generation. One lock fences admission/callbacks/close.
State pool[16];
unsigned used = 0;
State* current = nullptr;
std::mutex mutex;
std::atomic<bool> available{false};
std::atomic<bool> active{false};
std::atomic<uint64_t> revision{0};
State* identify(void* context) {
  for (unsigned i = 0; i < used; ++i) if (context == &pool[i]) return &pool[i];
  return nullptr;
}
bool finite(const float* p, size_t n) {
  for (size_t i = 0; i < n; ++i) if (!std::isfinite(p[i])) return false;
  return true;
}
bool rigid(const float* p) {
  if (!finite(p, 12)) return false;
  for (int a = 0; a < 3; ++a) for (int b = a; b < 3; ++b) {
    float dot = 0;
    for (int c = 0; c < 3; ++c) dot += p[a*4+c]*p[b*4+c];
    if (std::fabs(dot - (a == b ? 1.f : 0.f)) > .001f) return false;
  }
  const float determinant = p[0]*(p[5]*p[10]-p[6]*p[9]) - p[1]*(p[4]*p[10]-p[6]*p[8]) + p[2]*(p[4]*p[9]-p[5]*p[8]);
  return std::fabs(determinant - 1.f) <= .002f;
}
bool frustum(const float* p) { return finite(p, 4) && p[0] < p[1] && p[2] < p[3]; }
void invalidate(State& s) {
  const bool wasValid = s.pose;
  s.pose = false;
  if (current == &s) {
    if (wasValid) revision.fetch_add(1, std::memory_order_release);
    available.store(false, std::memory_order_release);
    edvr::setMenuVisible(0.f); edvr::inputGateSetPrivate(false);
  }
}
bool sameDevice(ID3D11Device* expected, ID3D11Texture2D* source) {
  Microsoft::WRL::ComPtr<ID3D11Device> owner; source->GetDevice(&owner);
  Microsoft::WRL::ComPtr<IUnknown> a, b;
  return expected && owner && SUCCEEDED(expected->QueryInterface(IID_PPV_ARGS(&a))) &&
      SUCCEEDED(owner->QueryInterface(IID_PPV_ARGS(&b))) && a.Get() == b.Get();
}
HRESULT WINAPI publish(void* context, const float* head, const float (*eyes)[12],
    const float (*frusta)[4], uint64_t generation, uint64_t referenceGeneration) {
  std::lock_guard<std::mutex> lock(mutex);
  auto* s = identify(context);
  if (!s || !s->active || s != current || generation != s->generation) return E_INVALIDARG;
  if (!head && !eyes && !frusta && !referenceGeneration) { invalidate(*s); return S_OK; }
  if (GetCurrentThreadId() != s->thread) return E_INVALIDARG;
  if (!head || !eyes || !frusta || !referenceGeneration || !rigid(head) || !rigid(eyes[0]) ||
      !rigid(eyes[1]) || !frustum(frusta[0]) || !frustum(frusta[1])) {
    invalidate(*s); return E_INVALIDARG;
  }
  if (s->referenceGeneration && s->referenceGeneration != referenceGeneration) invalidate(*s);
  std::memcpy(s->head, head, sizeof(s->head)); std::memcpy(s->eyes, eyes, sizeof(s->eyes));
  std::memcpy(s->frusta, frusta, sizeof(s->frusta));
  s->referenceGeneration = referenceGeneration; s->pose = true;
  edvr::publishHeadPose(head); available.store(true, std::memory_order_release);
  return S_OK;
}
void headLockAnchor(const float head[12], float yaw, float pitch, float out[12]) {
  const float y = -yaw*.0174532925199433f, p = pitch*.0174532925199433f;
  const float cy = std::cos(y), sy = std::sin(y), cp = std::cos(p), sp = std::sin(p);
  const float r[9] = {cy, sy*sp, sy*cp, 0, cp, -sp, -sy, cy*sp, cy*cp};
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) out[row*4+col] = head[row*4]*r[col] + head[row*4+1]*r[3+col] + head[row*4+2]*r[6+col];
    out[row*4+3] = head[row*4+3];
  }
}
HRESULT WINAPI treat(void* context, uint32_t eye, ID3D11Texture2D* source,
    const float* box, ID3D11Texture2D** output, float* outBox) {
  if (output) *output = nullptr;
  if (outBox) std::memset(outBox, 0, sizeof(float)*4);
  std::lock_guard<std::mutex> lock(mutex);
  auto* s = identify(context);
  if (!s || !s->active || s != current || GetCurrentThreadId() != s->thread || eye > 1 ||
      !source || !output || !outBox || !s->pose) return E_INVALIDARG;
  if (box) {
    if (!finite(box, 4) || box[0] == box[2] || box[1] == box[3]) return E_INVALIDARG;
    for (unsigned i = 0; i < 4; ++i) if (box[i] < 0 || box[i] > 1) return E_INVALIDARG;
  }
  if (!sameDevice(s->device, source)) return E_INVALIDARG;
  D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
  if (!desc.Width || !desc.Height || desc.ArraySize != 1 || desc.MipLevels != 1 ||
      desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality || desc.Usage != D3D11_USAGE_DEFAULT ||
      desc.CPUAccessFlags) return E_INVALIDARG;
  float alpha = 0; uint32_t stamp = 0;
  if (!edvr::menuVisible(&alpha, &stamp) || alpha <= 0.f) return S_FALSE;
  float anchor[12]{}, yaw = 0, pitch = 0;
  if (edvr::menuHeadLock(&yaw, &pitch)) {
    if (!std::isfinite(yaw) || !std::isfinite(pitch)) return E_INVALIDARG;
    headLockAnchor(s->head, yaw, pitch, anchor);
  } else if (!edvr::menuAnchor(anchor, nullptr)) return S_FALSE;
  if (!rigid(anchor)) return E_INVALIDARG;
  const auto* eyePose = s->eyes[eye];
  float transform[12]{};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) transform[i*3+j] = anchor[i]*eyePose[j] + anchor[4+i]*eyePose[4+j] + anchor[8+i]*eyePose[8+j];
    transform[9+i] = anchor[i]*(eyePose[3]-anchor[3]) + anchor[4+i]*(eyePose[7]-anchor[7]) + anchor[8+i]*(eyePose[11]-anchor[11]);
  }
  // These signed physical magnitudes also represent an off-centre frustum.
  const float physical[4] = {-s->frusta[eye][0], s->frusta[eye][1], s->frusta[eye][3], -s->frusta[eye][2]};
  struct FrustumScope {
    explicit FrustumScope(const float* value) { edvr::menuPanelSetNativeFrustum(value); }
    ~FrustumScope() { edvr::menuPanelClearNativeFrustum(); }
  } frustumScope(physical);
  auto* treated = static_cast<ID3D11Texture2D*>(edvr::menuPanelCompositeNative(source, int(eye), box, transform));
  if (!treated) return S_FALSE;
  treated->AddRef(); *output = treated;
  const bool flipU = box && box[0] > box[2], flipV = box && box[1] > box[3];
  outBox[0] = flipU ? 1.f : 0.f; outBox[2] = flipU ? 0.f : 1.f;
  outBox[1] = flipV ? 1.f : 0.f; outBox[3] = flipV ? 0.f : 1.f;
  return S_OK;
}
HRESULT WINAPI close(void* context) {
  std::lock_guard<std::mutex> lock(mutex);
  auto* s = identify(context);
  if (!s) return E_INVALIDARG;
  if (!s->active) return S_FALSE;
  invalidate(*s); s->active = false; s->device = nullptr;
  active.store(false, std::memory_order_release);
  if (current == s) current = nullptr;
  return S_OK;
}
}

bool nativeMenuAvailable() { return available.load(std::memory_order_acquire); }
bool nativeMenuActive() { return active.load(std::memory_order_acquire); }
uint64_t nativeMenuRevision() { return revision.load(std::memory_order_acquire); }
extern "C" HRESULT WINAPI edvrAcquireNativeMenu(const EdvrNativeMenuRequest* request, EdvrNativeMenuTable* table) {
  if (!table || table->size != sizeof(*table) || table->version != EDVR_NATIVE_MENU_VERSION_1) return E_INVALIDARG;
  *table = {sizeof(*table), EDVR_NATIVE_MENU_VERSION_1};
  if (!request || request->size != sizeof(*request) || request->version != EDVR_NATIVE_MENU_VERSION_1 ||
      !request->gameDevice || !request->generation) return E_INVALIDARG;
  std::lock_guard<std::mutex> lock(mutex);
  if (current || used == _countof(pool)) return E_PENDING;
  auto& s = pool[used++]; s.device = request->gameDevice; s.thread = GetCurrentThreadId();
  s.generation = request->generation; s.active = true; current = &s;
  active.store(true, std::memory_order_release);
  table->context = &s; table->publishPose = publish; table->treatEye = treat; table->close = close;
  return S_OK;
}
