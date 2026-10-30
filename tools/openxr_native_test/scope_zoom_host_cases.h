#pragma once
// B3, the scope zoom's window at the host (src/openxr/native_runtime_host.h; the pure parts are held in tools\openxr_system_test): the game's reads of eye 0's projection are counted only while
// a press of the window stands and only for eye 0's reads that the zoom is held back from; each getter's last answered span is kept (a read that answered nothing does not erase it); the raw
// getter's reads are counted by caller (return address as an RVA in the game's executable) into a fixed table with the rest as other; and the press's close puts all of it in the press the
// d3d11 half is told. The reads are made by the host's own noteProjectionRead, the press opened by its own run and closed by its own close.
#include "launch_centre_cases.h"
#include "../../src/openxr/scope_zoom_state.h"
#include <cmath>

namespace edvr::openxr::test {
template<class Check> void runScopeZoomHostCases(Check check) {
  using namespace launch_fixture;
  Fixture f;auto& h=f.host;
  ScopeZoomInput in;in.answerCarries=true;in.share=1.f;in.shareDrawn=true;in.base={-0.6f,0.7f,0.5f,-0.4f};
  in.request.zoom=true;in.request.m=4.f;in.request.strip=1.f;
  const ScopeZoomOutput applied=decideScopeZoom(in);
  check(applied.why==ScopeZoomWhy::kApplied,"scope zoom host: a decision that applies, to open a press with");
  constexpr uint32_t kA=0x4E3C93u,kB=0x2842AE3u,kOutside=0xFFFFFFFFu;
  h.noteProjectionRead(0,true,true,1.28f,kA);
  check(h.zoomRawReads.load()==0u&&!h.zoomReadsOn.load(),"scope zoom host: a read before any press is not counted");
  const auto opened=h.zoomRun.frame(applied,1000000);
  h.beginScopeZoomReads();
  check(opened.opened&&h.zoomRun.press.open&&h.zoomReadsOn.load(),"scope zoom host: the press opens and the reads are counted from there");
  // reads that are not the zoom's: another eye, and one the zoom is not held back from
  h.noteProjectionRead(1,true,true,1.5f,kA);h.noteProjectionRead(1,false,true,0.4f,kA);h.noteProjectionRead(0,true,false,1.9f,kA);h.noteProjectionRead(0,false,false,1.9f,kA);
  check(h.zoomRawReads.load()==0u&&h.zoomMatrixReads.load()==0u,"scope zoom host: another eye's reads, and reads the zoom is not held back from, are not counted");
  // eye 0 raw reads: five from A, two from B, one from outside the game's image, then six callers once each (nine distinct callers for a table of eight)
  for(int i=0;i<5;++i)h.noteProjectionRead(0,true,true,1.28f,kA);
  for(int i=0;i<2;++i)h.noteProjectionRead(0,true,true,1.28f,kB);
  h.noteProjectionRead(0,true,true,1.28f,kOutside);
  for(uint32_t i=0;i<6;++i)h.noteProjectionRead(0,true,true,1.28f,0x100u+i);
  h.noteProjectionRead(0,true,true,0.f,0x100u);   // answered nothing: counted as a read, the last span stays
  for(int i=0;i<3;++i)h.noteProjectionRead(0,false,true,0.32f,kA);   // the matrix getter's reads (its callers are not tallied)
  check(h.zoomRawReads.load()==15u&&h.zoomMatrixReads.load()==3u,"scope zoom host: eye 0's reads are counted per getter");
  check(h.zoomRawSpan.load()==1.28f&&h.zoomMatrixSpan.load()==0.32f,"scope zoom host: each getter's last answered span is kept, and a read that answered nothing does not erase it");
  // whether the compositor drew the right eye's mirror of the strip over the run, as compose() notes it, goes into the press at its close
  h.zoomMirror.note(0.6f,false,true,0.06f);
  h.zoomMirror.note(1.f,true,true,0.11f);
  h.zoomRun.frame(applied,2000000);
  h.closeScopeZoomPress("test");
  const ScopeZoomPress& p=h.zoomRun.press;
  check(p.mirrorFrames==2u&&p.mirrorWidthMax==0.11f,"scope zoom host: the press takes the mirror strip's tally at its close");
  check(!p.open&&!h.zoomReadsOn.load()&&p.rawReads==15u&&p.matrixReads==3u&&p.rawSpan==1.28f&&p.matrixSpan==0.32f&&std::fabs(p.rawSpan/p.matrixSpan-4.0f)<1e-4f,
        "scope zoom host: the press takes the counts and the spans, which are in the ratio of the narrowing, and the reads stop counting");
  check(p.callerRva[0]==kA&&p.callerReads[0]==5u&&p.callerRva[1]==kB&&p.callerReads[1]==2u&&p.callerRva[2]==0x100u&&p.callerReads[2]==2u&&p.callerRva[3]==kOutside&&p.callerReads[3]==1u,
        "scope zoom host: the press names the four busiest callers of the raw getter, most reads first, ties in the order first seen (the one outside the image as its own)");
  check(p.callerReads[0]+p.callerReads[1]+p.callerReads[2]+p.callerReads[3]+p.callerOther==15u&&p.callerOther==5u,
        "scope zoom host: and counts every other caller as other, so the callers add up to the raw reads");
  EdvrNativeScopeZoomReport report{};p.fill(report);
  check(report.rawCallerRva[0]==kA&&report.rawCallerReads[2]==2u&&report.rawCallerOther==5u&&report.rawSpan==1.28f&&report.matrixSpan==0.32f&&edvr::scopewin::reportReadable(&report),
        "scope zoom host: the report the d3d11 half is told carries them, and is readable");
  h.noteProjectionRead(0,true,true,1.28f,kA);
  check(h.zoomRawReads.load()==15u,"scope zoom host: a read after the press ended is not counted");
  // the next press starts from nothing
  h.zoomRun.frame(ScopeZoomOutput{},3000000);
  h.zoomRun.frame(applied,4000000);
  h.beginScopeZoomReads();
  h.noteProjectionRead(0,true,true,1.1f,kB);
  h.zoomRun.frame(applied,5000000);
  h.closeScopeZoomPress("test");
  check(h.zoomRun.press.rawReads==1u&&h.zoomRun.press.callerRva[0]==kB&&h.zoomRun.press.callerReads[1]==0u&&h.zoomRun.press.callerOther==0u&&h.zoomRun.press.matrixReads==0u&&h.zoomRun.press.matrixSpan==0.f,
        "scope zoom host: the next press counts from nothing: its own reads, callers and spans only");
}
} // namespace edvr::openxr::test
