#include "../../src/openxr/native_trace.h"
#include "../../src/openxr/native_cpu_trace.h"
#include <cstdio>
#include <cstdint>
#include <string>

using edvr::openxr::NativeTrace;
using edvr::openxr::NativeCpuTrace;
using edvr::openxr::NativeCpuTraceSpan;

struct Checks {
  unsigned count=0, failures=0;
  void check(bool value, const char* name) {
    ++count;
    if (!value) { ++failures; std::printf("FAIL: %s\n", name); }
  }
};

int cpuTraceSmoke() {
  constexpr unsigned phaseCycles=30,cycleMs=100,totalCycles=phaseCycles*2;
  auto& trace=NativeCpuTrace::get();
  if(!trace.start()) {
    const auto c=trace.counters();
    std::printf("openxr_trace_test: cpu trace registration failed error=%lu\n",(unsigned long)c.lastError);return 3;
  }
  for(unsigned i=0;i<100&&!trace.enabled();++i)Sleep(10);
  if(!trace.enabled()) {
    std::printf("openxr_trace_test: CPU provider not externally enabled pid=%lu guid={D3885FA1-0B70-44F1-AF88-63B2012B111E}\n",
      (unsigned long)GetCurrentProcessId());trace.stop();return 4;
  }
  const auto before=trace.counters();bool wrote=true;volatile uint64_t checksum=0;
  std::printf("openxr_trace_test: cpu trace smoke pid=%lu busy_ms=%u sleep_ms=%u cycles=%u\n",
    (unsigned long)GetCurrentProcessId(),phaseCycles*cycleMs,phaseCycles*cycleMs,totalCycles);
  for(unsigned i=0;i<totalCycles;++i) {
    const uint64_t frameBegin=edvrNativeTraceNowUs();
    {
      NativeCpuTraceSpan cycle(EdvrCpuFixtureCycle);
      if(i<phaseCycles) {
        NativeCpuTraceSpan phase(EdvrCpuFixtureBusy);const uint64_t until=frameBegin+uint64_t(cycleMs)*1000;
        do {checksum=checksum*uint64_t(6364136223846793005ull)+uint64_t(i+1);} while(edvrNativeTraceNowUs()<until);
        phase.finishVoid(int64_t(checksum&0x7fffffffffffffffull));
      } else {
        NativeCpuTraceSpan phase(EdvrCpuFixtureWait);Sleep(cycleMs);phase.finishVoid(cycleMs);
      }
      const uint64_t frameEnd=edvrNativeTraceNowUs();EdvrNativeCpuCompletedFramePayload event{};
      event.timestampUs=frameEnd;event.sequence=i+1;event.generation=1;event.featureEpoch=1;
      event.waitReturnUs=frameBegin;event.secondSubmitReturnUs=frameBegin;
      event.nextWaitEntryUs=frameEnd;event.nextWaitReturnUs=frameEnd;
      // Synthetic zero-duration Present keeps the fixture inside the same
      // postValid+singlePresent admission the production analyzer requires.
      event.presentBeginUs=frameBegin;event.presentEndUs=frameBegin;
      event.callerThread=GetCurrentThreadId();event.nextWaitThread=event.callerThread;
      event.status=EdvrCpuPostAvailable;event.flags=EdvrNativeCpuPostValid|EdvrNativeCpuSinglePresent;event.sceneReady=1;
      wrote=trace.emitFrame(event)&&wrote;cycle.finishVoid(i+1);
    }
  }
  trace.stop();const auto after=trace.counters();const uint64_t expected=uint64_t(totalCycles)*3;
  const uint64_t emitted=after.emitted-before.emitted;
  const bool success=wrote&&after.failures==before.failures&&emitted>=expected;
  std::printf("openxr_trace_test: cpu trace smoke emitted=%llu expected_data=%llu failures=%llu last_error=%lu checksum=%llu result=%s\n",
    (unsigned long long)emitted,(unsigned long long)expected,(unsigned long long)(after.failures-before.failures),
    (unsigned long)after.lastError,(unsigned long long)checksum,success?"ok":"failed");
  return success?0:5;
}

int wmain(int argc, wchar_t** argv) {
  SetErrorMode(3);
  if (argc == 2 && !wcscmp(argv[1], L"--dry-run")) {
    std::puts("openxr_trace_test: dry-run (no files)"); return 0;
  }
  if (argc == 2 && !wcscmp(argv[1], L"--cpu-trace-smoke")) return cpuTraceSmoke();
  if (argc != 2 || wcscmp(argv[1], L"--self-test")) return 2;
  Checks c;
  {
    auto& cpu=NativeCpuTrace::get();cpu.stop();const auto before=cpu.counters();
    {NativeCpuTraceSpan disabled(EdvrCpuFixtureCycle);disabled.finishVoid(1);}
    EdvrNativeCpuCompletedFramePayload event{};event.timestampUs=1;
    c.check(!cpu.emitFrame(event)&&cpu.counters().emitted==before.emitted,"disabled CPU provider emits nothing");
    c.check(cpu.start(),"CPU provider registration succeeds");cpu.stop();
  }
  SYSTEMTIME t{}; t.wYear=2026; t.wMonth=9; t.wDay=14;
  t.wHour=4; t.wMinute=23; t.wSecond=13; t.wMilliseconds=7;
  std::wstring path;
  c.check(NativeTrace::makeLogPathForExecutable(
      L"C:\\Games\\Elite Dangerous\\bin\\EliteDangerous64.exe", t, 1234, path),
      "typical executable path accepted");
  c.check(path == L"C:\\Games\\Elite Dangerous\\bin\\edvr_logs\\"
              L"edvr_openxr_20260914_042313_007_1234.log",
      "native path is beside executable in edvr_logs");
  c.check(NativeTrace::makeLogPathForExecutable(
      L"D:\\Games\\é\\EliteDangerous64.exe", t, 9, path) &&
      path.find(L"D:\\Games\\é\\edvr_logs\\") == 0,
      "spaces and non-ASCII path preserved");
  c.check(!NativeTrace::makeLogPathForExecutable(L"EliteDangerous64.exe", t, 9, path),
      "relative executable rejected");
  c.check(!NativeTrace::makeLogPathForExecutable(L"C:\\Games\\EliteDangerous64.exe", t, 0, path),
      "zero pid rejected");
  c.check(!NativeTrace::makeLogPathForExecutable(L"C:\\Games\\", t, 9, path),
      "directory without executable rejected");
  c.check(!NativeTrace::makeLogPathForExecutable(L"C:Games\\Elite.exe", t, 9, path),
      "drive-relative path rejected");
  c.check(NativeTrace::makeLogPathForExecutable(L"\\\\server\\share\\Elite.exe", t, 9, path) &&
      path.find(L"\\\\server\\share\\edvr_logs\\") == 0, "UNC executable preserved");
  c.check(NativeTrace::makeLogPathForExecutable(L"\\\\?\\C:\\Games\\Elite.exe", t, 9, path),
      "extended executable path supported");

  // Real begin must ignore the DLL address and the current directory. The
  // test executable lives in the workspace; only its edvr_logs is written.
  wchar_t executable[32768]{};
  c.check(GetModuleFileNameW(nullptr, executable, _countof(executable)) != 0, "process path");
  c.check(NativeTrace::get().begin(nullptr), "real trace opens next to process executable");
  NativeTrace::get().write("openxr_trace_test_marker");
  c.check(NativeTrace::get().begin(reinterpret_cast<void*>(1)), "begin is idempotent and ignores module address");
  const std::wstring exe(executable);
  const auto directory=exe.substr(0,exe.find_last_of(L"\\/")+1)+L"edvr_logs\\";
  const auto wildcard=directory+L"edvr_openxr_*_"+std::to_wstring(GetCurrentProcessId())+L".log";
  WIN32_FIND_DATAW found{};
  HANDLE search=FindFirstFileW(wildcard.c_str(),&found);
  bool marker=false;
  if(search!=INVALID_HANDLE_VALUE) {
    do {
      HANDLE f=CreateFileW((directory+found.cFileName).c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
          nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
      if(f!=INVALID_HANDLE_VALUE) {
        char data[1024]{}; DWORD count=0;
        if(ReadFile(f,data,sizeof(data)-1,&count,nullptr)) marker|=strstr(data,"openxr_trace_test_marker")!=nullptr;
        CloseHandle(f);
      }
    } while(FindNextFileW(search,&found));
    FindClose(search);
  }
  c.check(marker,"native marker readable in executable edvr_logs");
  std::printf("openxr_trace_test: %u checks, %u failures\n", c.count, c.failures);
  return c.failures ? 1 : 0;
}
