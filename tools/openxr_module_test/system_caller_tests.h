#pragma once

#include "system_caller.h"
#include "system_queries.h"
#include <atomic>
#include <memory>
#include <limits>
#include <stdexcept>
#include <thread>

namespace edvr::openxr::module_test {

template<class Check>
void runSystemCallerTests(Check&& check) {
  OwnerService owner;
  const auto caller = std::this_thread::get_id();
  check(owner.start(), "persistent System owner starts");
  if (!owner.running()) return;

  std::thread::id firstOwner, secondOwner;
  const auto first = submitWithPump(owner, [] { return true; },
                                    [&] { firstOwner = std::this_thread::get_id(); });
  const auto second = submitWithPump(owner, [] { return true; },
                                     [&] { secondOwner = std::this_thread::get_id(); });
  check(first.submitted && first.completed && first.succeeded &&
        second.submitted && second.completed && second.succeeded,
        "persistent System jobs complete");
  check(firstOwner != caller && firstOwner == secondOwner,
        "persistent System jobs keep one owner identity");

  OwnerService stopped;
  const auto rejected = submitWithPump(stopped, [] { return true; }, [] {});
  check(!rejected.submitted && !rejected.completed, "submission rejects an unstarted owner");

  const auto throwing = submitWithPump(owner, [] { return true; }, [] {
    throw std::runtime_error("test callback failure");
  });
  check(throwing.submitted && throwing.completed && !throwing.succeeded,
        "throwing System callback reports failed completion");

  std::atomic<unsigned> pumpCalls{0};
  std::atomic<bool> release{false};
  std::atomic<bool> capturedDestroyed{false};
  struct CaptureGuard {
    std::atomic<bool>& destroyed;
    explicit CaptureGuard(std::atomic<bool>& value) : destroyed(value) {}
    ~CaptureGuard() { destroyed.store(true, std::memory_order_release); }
  };
  std::weak_ptr<CaptureGuard> weak;
  {
    auto guard = std::make_shared<CaptureGuard>(capturedDestroyed);
    weak = guard;
    const auto pumped = submitWithPump(
        owner,
        [&] {
          const unsigned calls = pumpCalls.fetch_add(1, std::memory_order_relaxed) + 1;
          if (calls >= 2) release.store(true, std::memory_order_release);
          return calls != 1;
        },
        [guard = std::move(guard), &release] {
          while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        });
    check(pumped.submitted && pumped.completed && pumped.succeeded &&
          pumped.pumpCalls >= 2 && !pumped.pumpSucceeded,
          "System completion keeps pumping after a pump failure");
  }
  check(weak.expired() && capturedDestroyed.load(std::memory_order_acquire),
        "System callback captures are released before return");

  release=false;
  unsigned throwingPumps=0;
  const auto pumpException=submitWithPump(owner,[&] {
    if (++throwingPumps < 3) throw std::runtime_error("test pump failure");
    release.store(true,std::memory_order_release);
    return true;
  },[&] {
    while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
  });
  check(pumpException.completed && pumpException.succeeded && !pumpException.pumpSucceeded &&
        throwingPumps>=3,"throwing render pump still drains accepted System work");

  std::thread::id finalizerOwner;
  const bool stoppedOk = owner.stop([&] { finalizerOwner = std::this_thread::get_id(); });
  check(stoppedOk && finalizerOwner == firstOwner && !owner.running(),
        "persistent System owner joins through its owner finalizer");
  bool ranAfterStop=false;
  const auto retired=submitWithPump(owner,[] { return true; },[&] { ranAfterStop=true; });
  check(!retired.submitted && !ranAfterStop,"retired System caller rejects later work");

  // These fixed vectors exercise the observer independently of production
  // projection construction. A 90-degree frustum with near=1 and far=2.
  vr::HmdMatrix44_t projection{};
  projection.m[0][0]=projection.m[1][1]=1;
  projection.m[2][2]=projection.m[2][3]=-2;projection.m[3][2]=-1;
  check(finiteProjection(projection,1,2),"projection observer accepts known DirectX clip mapping");
  check(!finiteProjection({},1,2),"projection observer rejects zero fallback");
  check(!finiteProjection(projection,.1f,2),"projection observer rejects wrong requested near plane");
  check(!finiteProjection(projection,1,20),"projection observer rejects wrong requested far plane");
  auto changed=projection;changed.m[3][3]=1;
  check(!finiteProjection(changed,1,2),"projection observer rejects invalid homogeneous divisor");
  changed=projection;changed.m[0][1]=.5f;
  check(!finiteProjection(changed,1,2),"projection observer rejects unexpected shear");
  changed=projection;changed.m[0][0]=std::numeric_limits<float>::quiet_NaN();
  check(!finiteProjection(changed,1,2),"projection observer rejects nonfinite matrix");

  vr::HmdMatrix34_t rigid{};
  rigid.m[0][1]=-1;rigid.m[1][0]=1;rigid.m[2][2]=1;rigid.m[0][3]=.032f;
  check(rigidMatrix(rigid),"rigid observer accepts rotated translated eye transform");
  check(!rigidMatrix({}),"rigid observer rejects zero geometry fallback");
  auto reflected=rigid;reflected.m[2][2]=-1;
  check(!rigidMatrix(reflected),"rigid observer rejects reflection");
  auto scaled=rigid;scaled.m[1][0]=2;
  check(!rigidMatrix(scaled),"rigid observer rejects scaled transform");
  vr::TrackedDevicePose_t pose{};pose.mDeviceToAbsoluteTracking=rigid;
  pose.bPoseIsValid=pose.bDeviceIsConnected=true;pose.eTrackingResult=vr::TrackingResult_Running_OK;
  check(validHeadPose(pose),"System tracking observer accepts valid rigid pose");
  pose.eTrackingResult=vr::TrackingResult_Uninitialized;
  check(!validHeadPose(pose),"System tracking observer rejects uninitialized result");
  pose.eTrackingResult=vr::TrackingResult_Running_OK;pose.vAngularVelocity.v[1]=std::numeric_limits<float>::infinity();
  check(!validHeadPose(pose),"System tracking observer rejects nonfinite velocity");
}

}  // namespace edvr::openxr::module_test
