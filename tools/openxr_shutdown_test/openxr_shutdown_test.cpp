#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "../openxr_native_test/present_device.h"
#include "../../src/openxr/native_render_binding.h"
#include "../../src/openxr/render_route.h"
#include "../../src/openxr/render_shutdown_coordinator.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <thread>

using namespace edvr::openxr;

namespace {

std::atomic<unsigned> checks{0};
std::atomic<unsigned> failures{0};

void check(bool value, const char* description) {
  ++checks;
  if (!value) {
    ++failures;
    std::printf("FAIL: %s\n", description);
  }
}

bool absolutePath(const std::wstring& path) {
  return path.size() > 3 && path[1] == L':' &&
      (path[2] == L'\\' || path[2] == L'/');
}

class Event final {
 public:
  Event() : handle_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
  ~Event() { if (handle_) CloseHandle(handle_); }
  Event(const Event&) = delete;
  Event& operator=(const Event&) = delete;
  void signal() { if (handle_) SetEvent(handle_); }
  bool wait(DWORD milliseconds) const {
    return handle_ && WaitForSingleObject(handle_, milliseconds) == WAIT_OBJECT_0;
  }
 private:
  HANDLE handle_ = nullptr;
};

struct CaptureLifetime final {
  explicit CaptureLifetime(std::atomic<unsigned>& retired) : retired_(&retired) {}
  ~CaptureLifetime() { ++*retired_; }
  std::atomic<unsigned>* retired_;
};

// This fixture must never leave a caller waiting on a graphics callback if a
// broken proxy prevents progress. The parent runs it in a child process.
struct Watchdog final {
  Event finished;
  std::thread thread{[this] {
    if (!finished.wait(20000)) std::_Exit(3);
  }};
  ~Watchdog() { finished.signal(); thread.join(); }
};

bool reached(const std::function<bool()>& predicate, DWORD milliseconds = 3000) {
  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(milliseconds);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    SwitchToThread();
  }
  return true;
}

struct FakeResources final {
  std::atomic<bool> destroyed{false};
};

struct FakeSteps final {
  std::shared_ptr<FakeResources> resources;
  std::atomic<unsigned> drains{0};
  std::atomic<unsigned> finalizers{0};
  std::atomic<unsigned> capturedRetired{0};
  std::atomic<DWORD> drainThread{0};
  std::atomic<DWORD> finalizerThread{0};
  std::atomic<DWORD> immediateThread{0};
  CountedGraphics* immediate = nullptr;
  Event* finalizerEntered = nullptr;
  Event* finalizerRelease = nullptr;
  bool drainResult = true;
  bool finalizerResult = true;
  bool drainThrows = false;
  bool finalizerThrows = false;

  bool drain() {
    drainThread.store(GetCurrentThreadId(), std::memory_order_release);
    ++drains;
    if (immediate) {
      if (!immediate->invoke([this] {
            immediateThread.store(GetCurrentThreadId(), std::memory_order_release);
          })) return false;
    }
    if (drainThrows) throw 7;
    return drainResult;
  }

  bool finalize() {
    finalizerThread.store(GetCurrentThreadId(), std::memory_order_release);
    ++finalizers;
    if (finalizerThrows) throw 8;
    if (finalizerEntered) {
      finalizerEntered->signal();
      if (!finalizerRelease || !finalizerRelease->wait(5000)) return false;
    }
    if (finalizerResult && resources) resources->destroyed.store(true,
        std::memory_order_release);
    return finalizerResult;
  }
};

// One actual graphics lease and one actual bound Present caller are reused by
// each case. All owner and render objects are stopped before this object dies.
class BoundFixture final {
 public:
  BoundFixture(HMODULE proxy, const std::wstring& path, PresentDevice& present)
      : proxy_(proxy), path_(path), present_(present), render_(owner_),
        route_{render_, &binding_.work()} {}
  BoundFixture(const BoundFixture&) = delete;
  BoundFixture& operator=(const BoundFixture&) = delete;
  ~BoundFixture() { if (!close()) std::terminate(); }

  bool start() {
    std::atomic<HRESULT> acquired{E_FAIL};
    std::thread init([&] {
      initThread_ = GetCurrentThreadId();
      acquired.store(binding_.acquire(path_), std::memory_order_release);
    });
    init.join();
    if (acquired.load(std::memory_order_acquire) != S_OK) return false;
    acquired_ = true;

    presentThread_ = GetCurrentThreadId();
    if (FAILED(present_.present())) return false;
    if (!binding_.renderThread() || binding_.renderThread() != presentThread_)
      return false;
    if (!owner_.start()) return false;
    started_ = true;

    std::atomic<bool> bound{false};
    std::thread bind([&] {
      systemSetupThread_ = GetCurrentThreadId();
      bound.store(route_.bind(), std::memory_order_release);
    });
    const bool pumped = reached([&] {
      if (bound.load(std::memory_order_acquire)) return true;
      present_.present();
      return bound.load(std::memory_order_acquire);
    });
    bind.join();
    return pumped && bound.load(std::memory_order_acquire) &&
        render_.isRenderThread();
  }

  bool close() {
    if (!acquired_ && !started_) return true;
    return retain() && disposeRetained();
  }

  // Mirrors ModuleBackend::retain: close render admission, close the boundary,
  // join CPU owner work, and keep the graphics lease/device references held.
  bool retain() {
    if (!acquired_ && !started_) return true;
    render_.close();
    const HRESULT closed = binding_.close();
    bool stopped = true;
    if (started_) {
      const bool outcome = owner_.stop();
      // OwnerService reports the finalizer outcome separately from worker
      // retirement. A throwing fake finalizer therefore returns false even
      // after this explicit foreign stop call has joined the worker.
      stopped = outcome || (!owner_.running() && owner_.pending() == 0);
    }
    started_ = false;
    return closed == S_OK && stopped;
  }

  bool disposeRetained() {
    if (!acquired_) return true;
    const HRESULT released = binding_.release();
    if (released == S_OK) acquired_ = false;
    return released == S_OK;
  }

  RenderRoute& route() { return route_; }
  PresentWorkQueue& queue() { return binding_.work(); }
  RenderThreadDispatcher& render() { return render_; }
  OwnerService& owner() { return owner_; }
  NativeRenderBinding& binding() { return binding_; }
  PresentDevice& present() { return present_; }
  DWORD presentThread() const { return presentThread_; }
  DWORD initThread() const { return initThread_; }
  DWORD setupThread() const { return systemSetupThread_; }

 private:
  HMODULE proxy_ = nullptr; // documents the paired module kept by binding
  std::wstring path_;
  PresentDevice& present_;
  NativeRenderBinding binding_;
  OwnerService owner_;
  RenderThreadDispatcher render_;
  RenderRoute route_;
  bool acquired_ = false;
  bool started_ = false;
  DWORD initThread_ = 0;
  DWORD presentThread_ = 0;
  DWORD systemSetupThread_ = 0;
};

struct ShutdownRun final {
  RenderShutdownCoordinatorResult result{};
  std::atomic<bool> done{false};
  DWORD systemThread = 0;
  std::weak_ptr<CaptureLifetime> captured;
  std::thread caller;
};

void startShutdown(BoundFixture& fixture, FakeSteps& steps, ShutdownRun& run,
                   std::chrono::milliseconds boundaryTimeout =
                       std::chrono::milliseconds(5000)) {
  auto capture = std::make_shared<CaptureLifetime>(steps.capturedRetired);
  run.captured = capture;
  run.caller = std::thread([&, capture = std::move(capture), boundaryTimeout]() mutable {
    run.systemThread = GetCurrentThreadId();
    run.result = runRenderShutdown(
        fixture.route(), fixture.queue(), fixture.render(), fixture.owner(),
        [&, capture] { (void)capture; return steps.drain(); },
        [&, capture] { (void)capture; return steps.finalize(); }, boundaryTimeout);
    // Drop the caller's strong reference only after the coordinator has
    // returned; the weak probe then measures callback capture retirement at
    // the operation boundary rather than at thread-object destruction.
    capture.reset();
    run.done.store(true, std::memory_order_release);
  });
}

bool pumpUntilDone(BoundFixture& fixture, ShutdownRun& run, DWORD milliseconds = 3000) {
  return reached([&] {
    if (run.done.load(std::memory_order_acquire)) return true;
    fixture.present().present();
    return run.done.load(std::memory_order_acquire);
  }, milliseconds);
}

void joinRun(ShutdownRun& run) {
  if (run.caller.joinable()) run.caller.join();
}

bool ownerThreadContract(const BoundFixture& fixture, const FakeSteps& steps,
                         const ShutdownRun& run) {
  const DWORD drainThread = steps.drainThread.load(std::memory_order_acquire);
  const DWORD finalizerThread = steps.finalizerThread.load(std::memory_order_acquire);
  return drainThread != 0 && finalizerThread != 0 &&
      drainThread == finalizerThread && drainThread != fixture.presentThread() &&
      drainThread != run.systemThread && run.systemThread != fixture.presentThread();
}

void noPresentCancelsDrain(HMODULE proxy, const std::wstring& path,
                           PresentDevice& present) {
  BoundFixture fixture(proxy, path, present);
  const bool started = fixture.start();
  check(started, "no-Present fixture binds the real WARP Present caller");
  if (!started) return;
  FakeSteps steps{std::make_shared<FakeResources>()};
  ShutdownRun run;
  startShutdown(fixture, steps, run);
  check(reached([&] { return fixture.queue().pending() == 1; }),
      "first drain request is pending before the idle shutdown case");
  check(reached([&] { return run.done.load(std::memory_order_acquire); }, 7000),
      "idle first drain reaches its bounded pending deadline");
  joinRun(run);
  check(!run.result.drainInvoked && !run.result.drainSucceeded &&
        steps.drains == 0 && steps.finalizers == 0,
      "idle first drain cancels without invoking drain or finalizer");
  check(run.captured.expired() && steps.capturedRetired == 1,
      "cancelled drain releases its captured callback before return");
  check(!steps.resources->destroyed.load(std::memory_order_acquire),
      "idle first drain retains fake resources");
  const uint64_t callbacks = fixture.binding().callbacks();
  check(fixture.retain(), "retained shutdown closes admission and joins the owner");
  check(fixture.binding().provider() == proxy &&
        fixture.binding().device() == present.device(),
      "retained shutdown keeps paired graphics references alive");
  check(SUCCEEDED(present.present()) && fixture.binding().callbacks() == callbacks &&
        !steps.resources->destroyed.load(std::memory_order_acquire),
      "late Present after retained close cannot run cancelled work or destroy resources");
  check(fixture.disposeRetained(), "fixture disposes retained graphics after observation");
}

void noPresentCancelsBoundary(HMODULE proxy, const std::wstring& path,
                              PresentDevice& present) {
  BoundFixture fixture(proxy, path, present);
  const bool started = fixture.start();
  check(started, "boundary idle fixture binds the real WARP Present caller");
  if (!started) return;
  FakeSteps steps{std::make_shared<FakeResources>()};
  ShutdownRun run;
  startShutdown(fixture, steps, run);
  check(reached([&] { return fixture.queue().pending() == 1; }),
      "drain request is pending before one Present resumes it");
  check(SUCCEEDED(present.present()), "one Present services the GPU drain");
  check(reached([&] { return steps.drains.load(std::memory_order_acquire) == 1; }),
      "GPU drain executes on the owner service caller");
  check(reached([&] { return run.done.load(std::memory_order_acquire); }, 7000),
      "second boundary request reaches its bounded pending deadline");
  joinRun(run);
  check(run.result.drainInvoked && run.result.drainSucceeded &&
        !run.result.boundary.entered && !run.result.finalizerSucceeded &&
        steps.finalizers == 0,
      "missing follow-up Present cancels the final boundary before finalizer");
  check(run.captured.expired() && steps.capturedRetired == 1,
      "cancelled boundary releases its captured callback before return");
  check(!steps.resources->destroyed.load(std::memory_order_acquire),
      "missing follow-up Present retains fake resources");
  check(fixture.close(), "boundary idle fixture disposes cleanly");
}

void resumedPresentCompletes(HMODULE proxy, const std::wstring& path,
                             PresentDevice& present) {
  BoundFixture fixture(proxy, path, present);
  const bool started = fixture.start();
  check(started, "resumed fixture binds the real WARP Present caller");
  if (!started) return;
  check(fixture.initThread() != fixture.presentThread() &&
        fixture.setupThread() != fixture.presentThread(),
      "Init and render-dispatch setup callers remain distinct from Present");
  FakeSteps steps{std::make_shared<FakeResources>()};
  CountedGraphics immediate(fixture.render());
  steps.immediate = &immediate;
  ShutdownRun run;
  startShutdown(fixture, steps, run);
  check(pumpUntilDone(fixture, run), "resumed Present services both shutdown stages");
  joinRun(run);
  check(run.result.drainInvoked && run.result.drainSucceeded &&
        run.result.boundary.entered && run.result.boundary.joined &&
        run.result.finalizerSucceeded && steps.drains == 1 && steps.finalizers == 1,
      "resumed Present completes drain and finalizer exactly once");
  check(run.captured.expired() && steps.capturedRetired == 1,
      "successful shutdown releases callback captures before return");
  check(steps.resources->destroyed.load(std::memory_order_acquire),
      "successful finalizer releases fake resources");
  check(ownerThreadContract(fixture, steps, run),
      "both stages run on one owner thread distinct from System and Present");
  check(immediate.calls == 1 && immediate.wrongThread == 0 && immediate.rejected == 0 &&
        immediate.thread == fixture.presentThread() &&
        steps.immediateThread == fixture.presentThread() &&
        steps.immediateThread != run.systemThread,
      "successful drain uses the immediate executor on the actual Present caller");
  check(fixture.close(), "resumed fixture disposes cleanly");
}

void heldFinalizerRetiresBeforeRelease(HMODULE proxy, const std::wstring& path,
                                       PresentDevice& present) {
  BoundFixture fixture(proxy, path, present);
  const bool started = fixture.start();
  check(started, "held-finalizer fixture binds the real WARP Present caller");
  if (!started) return;
  FakeSteps steps{std::make_shared<FakeResources>()};
  Event entered;
  Event release;
  steps.finalizerEntered = &entered;
  steps.finalizerRelease = &release;
  ShutdownRun run;
  startShutdown(fixture, steps, run, std::chrono::milliseconds(500));
  check(reached([&] { return fixture.queue().pending() == 1; }),
      "held-finalizer drain request is pending");
  check(SUCCEEDED(present.present()), "held-finalizer case services the drain");
  check(reached([&] { return steps.drains.load(std::memory_order_acquire) == 1; }),
      "held-finalizer drain executes before boundary request");
  check(reached([&] { return fixture.queue().pending() == 1; }),
      "held-finalizer boundary request is queued before the second Present");

  // The Present caller must stay inside the real callback while the observer
  // proves that lease release is pending. The observer then releases the fake
  // finalizer, allowing the callback and System caller to retire normally.
  std::atomic<bool> observerChecked{false};
  std::thread observer([&] {
    const bool seen = entered.wait(3000);
    Sleep(600);
    check(seen && !run.done.load(std::memory_order_acquire) &&
          !run.captured.expired(),
        "System remains blocked while the finalizer holds the Present callback");
    const HRESULT closeResult = fixture.binding().close();
    const HRESULT releaseResult = fixture.binding().release();
    check(closeResult == E_PENDING && releaseResult == E_PENDING,
        "boundary close and release remain pending during the active callback");
    observerChecked.store(true, std::memory_order_release);
    release.signal();
  });
  // The callback is held beyond the 500 ms fixture deadline. Once it has
  // entered it must finish with its borrowed captures intact. Production keeps
  // the existing five-second pending deadline.
  check(SUCCEEDED(present.present()), "held finalizer Present completes after release");
  observer.join();
  joinRun(run);
  check(observerChecked.load(std::memory_order_acquire) &&
        run.result.boundary.entered && run.result.boundary.joined &&
        run.result.finalizerSucceeded && steps.finalizers == 1 &&
        steps.resources->destroyed.load(std::memory_order_acquire),
      "held finalizer retires callback and resources after explicit release");
  check(run.captured.expired() && steps.capturedRetired == 1,
      "held callback captures retire before the coordinator returns");
  check(fixture.binding().release() == S_OK,
      "held-finalizer lease releases after callback retirement");
  check(fixture.close(), "held-finalizer fixture disposes cleanly");
}

void failurePreservesResources(HMODULE proxy, const std::wstring& path,
                               PresentDevice& present) {
  BoundFixture fixture(proxy, path, present);
  const bool started = fixture.start();
  check(started, "failure fixture binds the real WARP Present caller");
  if (!started) return;
  FakeSteps steps{std::make_shared<FakeResources>()};
  steps.finalizerResult = false;
  ShutdownRun run;
  startShutdown(fixture, steps, run);
  check(pumpUntilDone(fixture, run), "finalizer failure case services both stages");
  joinRun(run);
  check(run.result.drainSucceeded && run.result.boundary.entered &&
        run.result.boundary.joined && !run.result.finalizerSucceeded &&
        steps.finalizers == 1 && !steps.resources->destroyed.load(),
      "false finalizer reports failure and preserves resources");
  check(run.captured.expired() && steps.capturedRetired == 1,
      "false finalizer releases callback captures before return");
  check(fixture.close(), "false-finalizer fixture disposes cleanly");
}

void throwingDrainSkipsFinalizer(HMODULE proxy, const std::wstring& path,
                                 PresentDevice& present) {
  BoundFixture fixture(proxy, path, present);
  const bool started = fixture.start();
  check(started, "throw fixture binds the real WARP Present caller");
  if (!started) return;
  FakeSteps steps{std::make_shared<FakeResources>()};
  steps.drainThrows = true;
  ShutdownRun run;
  startShutdown(fixture, steps, run);
  check(pumpUntilDone(fixture, run), "throwing drain case services the queued callback");
  joinRun(run);
  check(!run.result.drainSucceeded && steps.drains == 1 && steps.finalizers == 0 &&
        !steps.resources->destroyed.load(),
      "throwing drain skips finalizer and preserves resources");
  check(run.captured.expired() && steps.capturedRetired == 1,
      "throwing drain releases callback captures before return");
  check(fixture.close(), "throwing-drain fixture disposes cleanly");
}

void falseDrainSkipsFinalizer(HMODULE proxy, const std::wstring& path,
                              PresentDevice& present) {
  BoundFixture fixture(proxy, path, present);
  const bool started = fixture.start();
  check(started, "false-drain fixture binds the real WARP Present caller");
  if (!started) return;
  FakeSteps steps{std::make_shared<FakeResources>()};
  steps.drainResult = false;
  ShutdownRun run;
  startShutdown(fixture, steps, run);
  check(pumpUntilDone(fixture, run), "false drain case services the queued callback");
  joinRun(run);
  check(!run.result.drainSucceeded && steps.drains == 1 && steps.finalizers == 0 &&
        !steps.resources->destroyed.load(),
      "false drain skips finalizer and preserves resources");
  check(run.captured.expired() && steps.capturedRetired == 1,
      "false drain releases callback captures before return");
  check(fixture.close(), "false-drain fixture disposes cleanly");
}

void throwingFinalizerPreservesResources(HMODULE proxy, const std::wstring& path,
                                         PresentDevice& present) {
  BoundFixture fixture(proxy, path, present);
  const bool started = fixture.start();
  check(started, "throwing-finalizer fixture binds the real WARP Present caller");
  if (!started) return;
  FakeSteps steps{std::make_shared<FakeResources>()};
  steps.finalizerThrows = true;
  ShutdownRun run;
  startShutdown(fixture, steps, run);
  check(pumpUntilDone(fixture, run), "throwing finalizer case services both stages");
  joinRun(run);
  check(run.result.drainSucceeded && run.result.boundary.entered &&
        !run.result.boundary.joined && !run.result.finalizerSucceeded &&
        steps.finalizers == 1 && !steps.resources->destroyed.load(),
      "throwing finalizer reports failure and preserves resources");
  check(run.captured.expired() && steps.capturedRetired == 1,
      "throwing finalizer releases callback captures before return");
  check(fixture.close(), "throwing-finalizer fixture disposes cleanly");
}

int selfTest(const std::wstring& suppliedPath) {
  Watchdog watchdog;
  wchar_t executable[MAX_PATH]{};
  GetModuleFileNameW(nullptr, executable, MAX_PATH);
  std::wstring path = suppliedPath;
  if (path.empty()) {
    path = executable;
    const auto slash = path.find_last_of(L"\\/");
    path = path.substr(0, slash + 1) + L"d3d11.dll";
  }
  check(absolutePath(path), "graphics proxy path is drive-absolute");
  HMODULE proxy = LoadLibraryExW(path.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  check(proxy != nullptr, "load the actual graphics proxy");
  if (!proxy) return 1;
  {
    PresentDevice present;
    check(SUCCEEDED(present.initialize(proxy, D3D_DRIVER_TYPE_WARP)),
        "initialize a real WARP device through the graphics proxy");
    if (present.swapchain()) {
      noPresentCancelsDrain(proxy, path, present);
      noPresentCancelsBoundary(proxy, path, present);
      resumedPresentCompletes(proxy, path, present);
      heldFinalizerRetiresBeforeRelease(proxy, path, present);
      failurePreservesResources(proxy, path, present);
      throwingDrainSkipsFinalizer(proxy, path, present);
      falseDrainSkipsFinalizer(proxy, path, present);
      throwingFinalizerPreservesResources(proxy, path, present);
    }
  }
  // The proxy owns process-lifetime hook state; keep its module loaded until
  // this isolated diagnostic exits, matching the existing graphics fixtures.
  return failures.load(std::memory_order_acquire) ? 1 : 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 2 && !std::wcscmp(argv[1], L"--dry-run")) {
    std::puts("Would reproduce the two-step native render shutdown ordering with a real WARP Present; no windows, device, threads or writes created.");
    return 0;
  }
  if (argc == 2 && !std::wcscmp(argv[1], L"--self-test")) {
    const int result = selfTest(L"");
    std::printf("openxr_shutdown_test: %u checks, %u failures\n",
        checks.load(), failures.load());
    return result;
  }
  if (argc == 3 && !std::wcscmp(argv[1], L"--graphics-proxy") &&
      absolutePath(argv[2])) {
    const int result = selfTest(argv[2]);
    std::printf("openxr_shutdown_test: %u checks, %u failures\n",
        checks.load(), failures.load());
    return result;
  }
  std::fputs("usage: openxr_shutdown_test --dry-run|--self-test|--graphics-proxy ABSOLUTE_DLL\n", stderr);
  return 2;
}
