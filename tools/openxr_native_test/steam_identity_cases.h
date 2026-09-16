#pragma once
#include "../../src/openxr/steam_identity.h"
#include <cwchar>

namespace edvr::openxr::test {
namespace steam_identity_fixture {
// What the process environment holds right now: the value, or "" when absent.
inline bool read(wchar_t* out,unsigned n) {
  const DWORD got=GetEnvironmentVariableW(SteamIdentityLoan::kVariable,out,n);
  if(got==0) { out[0]=0; return false; }
  return got<n;
}
inline bool holds(const wchar_t* expected) {
  wchar_t v[32]={}; return read(v,32)&&std::wcscmp(v,expected)==0;
}
inline bool absent() { wchar_t v[32]={}; return !read(v,32); }
}
template<class Check> void runSteamIdentityCases(Check&& check) {
  using namespace steam_identity_fixture;
  using Source=SteamIdentityLoan::Source;
  // The rig is never a Steam launch; start from the state the game has when
  // Frontier's launcher started it, and leave the process as it was found.
  wchar_t original[32]={}; const bool hadOriginal=read(original,32);
  SetEnvironmentVariableW(SteamIdentityLoan::kVariable,nullptr);
  {
    SteamIdentityLoan loan; loan.begin();
    check(loan.sourceKind()==Source::lent&&holds(L"359320")&&std::wcscmp(loan.appId(),L"359320")==0,
      "an absent SteamAppId is lent Elite's id for the connect");
    loan.end();
    check(loan.isWithdrawn()&&absent(),"the loan is withdrawn once the instance exists");
    loan.end(); loan.begin();
    check(loan.isWithdrawn()&&absent()&&loan.sourceKind()==Source::lent,"end and begin are idempotent after the loan");
  }
  check(absent(),"a withdrawn loan is not withdrawn twice by the destructor");
  { SteamIdentityLoan loan; loan.begin(); check(holds(L"359320"),"the loan stands until end"); }
  check(absent(),"an early return still withdraws the loan");
  SetEnvironmentVariableW(SteamIdentityLoan::kVariable,L"12345");
  {
    SteamIdentityLoan loan; loan.begin();
    check(loan.sourceKind()==Source::inherited&&holds(L"12345")&&std::wcscmp(loan.appId(),L"12345")==0,
      "a Steam launch keeps the id Steam gave it");
    loan.end();
    check(!loan.isWithdrawn()&&holds(L"12345"),"end never touches an inherited id");
  }
  check(holds(L"12345"),"the destructor never touches an inherited id");
  {
    SteamIdentityLoan loan; loan.begin(L"777");
    check(loan.sourceKind()==Source::inherited&&holds(L"12345"),"an inherited id wins over any id offered");
  }
  SetEnvironmentVariableW(SteamIdentityLoan::kVariable,nullptr);
  {
    SteamIdentityLoan loan; loan.begin(L"777");
    check(loan.sourceKind()==Source::lent&&holds(L"777")&&std::wcscmp(loan.appId(),L"777")==0,"the id lent is the id offered");
  }
  check(absent(),"a lent id other than the default is withdrawn too");
  SetEnvironmentVariableW(SteamIdentityLoan::kVariable,hadOriginal?original:nullptr);
}
} // namespace edvr::openxr::test
