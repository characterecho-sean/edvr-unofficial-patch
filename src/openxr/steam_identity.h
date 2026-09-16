#pragma once

#include <windows.h>

namespace edvr::openxr {

// SteamVR files a connecting process under whatever application it can match
// it to. A game Steam launched carries SteamAppId in its environment and is
// filed as steam.app.<id>: Steam's own name and artwork in the dashboard.
// Elite started from Frontier's launcher carries nothing, so SteamVR
// fabricates a "builtin" entry named after the OpenXR applicationName, with
// no artwork, filed as system.generated.openxr.<name>.elitedangerous64.exe.
//
// Lending the process Elite's Steam app id for the one connect is enough.
// vrclient reads the variable when xrCreateInstance connects, and vrserver
// keeps the key it assigned then through the OpenXRInstance -> OpenXRScene
// transition at session begin, the transition back at session end and the
// reconnect inside xrDestroyInstance (measured 2026-09-16 against
// SteamVR/OpenXR; docs/steamvr-app-identity-2026-09-16.md). SteamGameId
// alone does nothing. The variable is withdrawn as soon as the instance
// exists, so nothing else in the game ever sees it, and a launch that already
// carries a SteamAppId -- Steam's own -- is left exactly as it is.
//
// Other runtimes ignore the variable. A user without Elite in their Steam
// library still lands on steam.app.359320, named after applicationName and
// without artwork: no worse than the builtin entry.
struct SteamIdentityLoan {
  static constexpr const wchar_t* kVariable=L"SteamAppId";
  static constexpr const wchar_t* kEliteDangerous=L"359320";
  enum class Source { none, lent, inherited };

  SteamIdentityLoan()=default;
  SteamIdentityLoan(const SteamIdentityLoan&)=delete;
  SteamIdentityLoan& operator=(const SteamIdentityLoan&)=delete;
  ~SteamIdentityLoan() { end(); }

  // Lends the id unless the environment already carries one. Call before
  // the loader is loaded: the runtime may read its environment as early as
  // its DllMain.
  void begin(const wchar_t* appId=kEliteDangerous) {
    if(begun) return;
    begun=true;
    const DWORD n=GetEnvironmentVariableW(kVariable,value,kCapacity);
    if(n>0&&n<kCapacity) { source=Source::inherited; return; }
    if(n>=kCapacity) { source=Source::inherited; value[0]=L'?'; value[1]=0; return; }
    if(!SetEnvironmentVariableW(kVariable,appId)) { source=Source::none; value[0]=0; return; }
    source=Source::lent;
    for(unsigned i=0;i<kCapacity-1;++i) { value[i]=appId[i]; if(!appId[i]) break; }
    value[kCapacity-1]=0;
  }
  // Withdraws what begin lent; an inherited value is not touched. Idempotent,
  // and the destructor calls it for any early return.
  void end() {
    if(source!=Source::lent||withdrawn) return;
    withdrawn=SetEnvironmentVariableW(kVariable,nullptr)!=FALSE;
  }

  Source sourceKind() const { return source; }
  const char* sourceName() const {
    switch(source) { case Source::lent: return "lent"; case Source::inherited: return "inherited"; default: return "none"; }
  }
  // The id the runtime saw at connect: what was lent, or what Steam set.
  const wchar_t* appId() const { return value; }
  bool isWithdrawn() const { return withdrawn; }

 private:
  static constexpr unsigned kCapacity=32;
  wchar_t value[kCapacity]={};
  Source source=Source::none;
  bool begun=false, withdrawn=false;
};

} // namespace edvr::openxr
