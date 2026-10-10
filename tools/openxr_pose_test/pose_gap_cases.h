#pragma once
// The pose-gap diagnostic (src/openxr/pose_gap.h; docs\terrain-culling.md): the quaternion angle, the window aggregation, the line format, and
// tools\pose_gap_fixture.log, which the log tool's --self-test reads, held here to exactly what the writer writes.
#include "../../src/openxr/pose_gap.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace pose_gap_cases {
using namespace edvr::openxr;

struct Lines {
  std::vector<std::string> v;
  void operator()(const char* s) {v.emplace_back(s);}
  size_t count(const std::string& prefix) const {size_t n=0;for(const auto& l:v)if(l.rfind(prefix,0)==0)++n;return n;}
  bool has(const std::string& text) const {for(const auto& l:v)if(l==text)return true;return false;}
};

constexpr uint32_t kGameThread=24212,kOtherThread=7001,kOutside=0xFFFFFFFFu;

inline PoseGapStats::Sample sample(uint32_t tid,uint32_t rva,float prediction,double gapMs,double angleDeg,double speed) {
  PoseGapStats::Sample s;
  s.thread=tid;s.rva=rva;s.predictionSec=prediction;s.located=true;
  s.gapKnown=true;s.gapMs=gapMs;s.angleKnown=true;s.angleDeg=angleDeg;s.speedKnown=true;s.speedDegPerSec=speed;
  return s;
}

// tools\pose_gap_fixture.log: twelve 60-second windows, written by the aggregator the host uses. Elite's own "now" request (thread 24212, return
// 0x4E3881, answered one display period after the display time) reads a gap of one period (+11.11 ms) and an angle to the drawn pose of head
// speed times that period (0.0111 degrees per deg/s), with one fallback call in the second window and three failed ones in the fifth; another
// caller (thread 7001, outside the image, a real prediction of 11 ms) is located at the wall clock and reads the true gap, an angle that grows
// with head speed (0.02 degrees per deg/s).
inline std::string fixtureLog() {
  PoseGapStats stats;
  Lines lines;
  uint64_t now=1000;
  stats.flushIfDue(now,lines);   // the first call only opens a window
  for(int w=1;w<=12;++w) {
    const double speed=10.0*w;
    for(int i=0;i<120;++i) {
      auto s=sample(kGameThread,0x4E3881,0.0f,11.11,0.0111*speed,speed);
      s.fallback=(w==2&&i==0);
      if(w==5&&i<3){s.located=false;s.gapKnown=false;s.angleKnown=false;}
      stats.note(s);
    }
    for(int i=0;i<60;++i)stats.note(sample(kOtherThread,kOutside,0.011f,9.0+0.5*(i%3),0.02*speed,speed));
    for(int k=0;k<5400;++k)stats.noteWait();
    now+=60000;
    stats.flushIfDue(now,lines);
  }
  std::string out;
  for(const auto& l:lines.v)out+="[00:00:00.000] "+l+"\n";
  return out;
}

template<class Check>
void runPoseGapCases(Check&& check) {
  const auto close=[](double a,double b,double eps){return std::fabs(a-b)<=eps;};
  // ---- the angle between two orientations -------------------------------------------------------------------------------------------
  {
    const float id[4]={0,0,0,1};
    const double h=3.14159265358979323846/180.0;
    const auto about=[&](double degrees,float q[4]){q[0]=0;q[1]=float(std::sin(degrees*0.5*h));q[2]=0;q[3]=float(std::cos(degrees*0.5*h));};
    float q90[4],q03[4],q180[4]={0,1,0,0};
    about(90.0,q90);about(0.03,q03);
    const float flipped[4]={-q90[0],-q90[1],-q90[2],-q90[3]};
    check(close(quaternionAngleDegrees(id,id),0.0,1e-12)&&close(quaternionAngleDegrees(id,q90),90.0,1e-4)&&close(quaternionAngleDegrees(q90,id),90.0,1e-4),
          "the angle between two orientations: 0 for the same, 90 for a quarter turn, whichever way round");
    check(close(quaternionAngleDegrees(id,flipped),90.0,1e-4)&&close(quaternionAngleDegrees(q90,flipped),0.0,1e-6),
          "...q and -q are the same rotation (the sign of w does not matter)");
    check(close(quaternionAngleDegrees(id,q180),180.0,1e-4),"...a half turn is 180");
    check(close(quaternionAngleDegrees(id,q03),0.03,0.001),"...and 0.03 degrees, a frame's worth of head turn, survives float quaternions");
    check(close(angularSpeedDegrees(1.0f,0.0f,0.0f),57.29578,1e-4)&&close(angularSpeedDegrees(0.0f,3.0f,4.0f),5.0*57.29578,1e-3)&&angularSpeedDegrees(0,0,0)==0.0,
          "the head speed is the length of the angular velocity, radians a second to degrees");
  }
  // ---- the line, and what goes in it ----------------------------------------------------------------------------------------------------
  {
    PoseGapStats stats;
    Lines lines;
    stats.flushIfDue(1000,lines);   // opens the window
    const double gaps[4]={12.0,14.0,10.0,12.0},angles[4]={2.0,3.0,1.0,2.0};   // (the max is not the last and the min is not the first)
    for(int i=0;i<4;++i)stats.note(sample(24212,0x4E3881,0.0f,gaps[i],angles[i],30.0));
    stats.noteWait();stats.noteWait();stats.noteWait();stats.noteWait();stats.noteWait();
    stats.flushIfDue(60999,lines);
    check(lines.v.empty(),"a window 59999 ms old writes nothing");
    stats.flushIfDue(61000,lines);
    check(lines.v.size()==1&&lines.v[0]=="pose gap: tid 24212 calls 4 from exe+0x4E3881 prediction 0.0 ms target-minus-display mean 12.00 ms (min 10.00 max 14.00) angle-to-drawn mean 2.000 max 3.000 deg head 30.0 deg/s waitgetposes 5 failed 0",
          "at 60 s the window writes one line per caller: calls, the return RVA, the prediction, the gap mean and range, the angle mean and max, the head speed, the waits, the failures (the exact line)");
    stats.flushIfDue(121000,lines);
    check(lines.v.size()==1&&stats.callers()==0&&stats.waits()==0,"...and the next window starts empty: no calls, no line, the wait count back to 0");
  }
  {
    PoseGapStats stats;
    Lines lines;
    stats.flushIfDue(1,lines);
    stats.note(sample(5,0x4E3881,-0.011f,-3.5,0.5,10.0));
    stats.note(sample(5,0x4E3881,-0.011f,-1.5,1.5,20.0));
    stats.flushIfDue(60001,lines);
    check(lines.v.size()==1&&lines.v[0]=="pose gap: tid 5 calls 2 from exe+0x4E3881 prediction -11.0 ms target-minus-display mean -2.50 ms (min -3.50 max -1.50) angle-to-drawn mean 1.000 max 1.500 deg head 15.0 deg/s waitgetposes 0 failed 0",
          "the prediction is a mean in milliseconds (-11 for -0.011 s) and a negative gap keeps its sign");
  }
  {
    PoseGapStats stats;
    Lines lines;
    PoseGapStats::Sample failed;failed.thread=9;failed.rva=0x4E3881;failed.predictionSec=0.0f;
    stats.note(failed);stats.note(failed);
    auto good=sample(9,0x4E3881,0.0f,1.0,1.0,1.0);good.fallback=true;
    stats.note(good);
    stats.flushNow(900,lines);
    check(lines.v.size()==1&&lines.v[0]=="pose gap: tid 9 calls 3 from exe+0x4E3881 prediction 0.0 ms target-minus-display mean 1.00 ms (min 1.00 max 1.00) angle-to-drawn mean 1.000 max 1.000 deg head 1.0 deg/s waitgetposes 0 failed 2 fallback 1",
          "calls that failed count in `failed` and are left out of every mean; a fallback adds ` fallback n` to the end of the line");
    PoseGapStats::Sample onlyFailed;onlyFailed.thread=3;onlyFailed.rva=0xFFFFFFFEu;
    stats.note(onlyFailed);
    stats.flushNow(1000,lines);
    check(lines.v.size()==2&&lines.v[1]=="pose gap: tid 3 calls 1 from ? prediction 0.0 ms target-minus-display n/a angle-to-drawn n/a head n/a waitgetposes 0 failed 1",
          "a caller with nothing measured says n/a for the gap, the angle and the head speed, and `from ?` when the return address was not captured");
    stats.note(sample(4,0xFFFFFFFFu,0.0f,2.0,0.1,5.0));
    stats.flushNow(1100,lines);
    check(lines.v.size()==3&&lines.v[2].find(" tid 4 calls 1 from outside prediction")!=std::string::npos,"a caller outside the game's image reads `from outside`");
  }
  // ---- the window ----------------------------------------------------------------------------------------------------------------------
  {
    PoseGapStats stats;
    Lines lines;
    stats.flushIfDue(10000,lines);
    stats.note(sample(1,0x4E3881,0.0f,1.0,1.0,1.0));
    stats.flushIfDue(69999,lines);
    check(lines.v.empty(),"a window is 60 s: 59999 ms is not enough");
    stats.flushIfDue(70000,lines);
    check(lines.v.size()==1&&lines.v[0].rfind("pose gap: tid 1 calls 1 ",0)==0,"...and 60000 ms is");
    stats.note(sample(1,0x4E3881,0.0f,2.0,2.0,2.0));
    stats.flushIfDue(129999,lines);
    check(lines.v.size()==1,"...and the next window runs its own 60 s from that flush, not from the first one's opening");
    stats.flushIfDue(130000,lines);
    check(lines.v.size()==2,"(and writes its line then)");
    check(kPoseGapWindowMs==60000,"(the window is 60 seconds)");
  }
  {
    PoseGapStats stats;
    Lines lines;
    stats.flushNow(0,lines);
    for(uint32_t tid=1;tid<=20;++tid)stats.note(sample(tid,0x4E3881,0.0f,1.0,1.0,1.0));
    stats.noteWait();
    stats.flushNow(1,lines);
    check(lines.v.size()==17&&lines.count("pose gap: tid ")==16&&lines.has("pose gap: more than 16 callers in a window; 4 calls not counted"),
          "sixteen callers a window are tabled; calls from more are counted and said in a line of their own");
    stats.note(sample(1,0x4E3881,0.0f,1.0,1.0,1.0));
    stats.note(sample(1,0x4E3882,0.0f,1.0,1.0,1.0));
    stats.flushNow(2,lines);
    check(lines.v.size()==19,"(a caller is a thread AND a return address: the same thread from another address is another line)");
  }
  {
    PoseGapStats stats;
    std::vector<std::thread> threads;
    for(int t=0;t<4;++t)threads.emplace_back([&]{for(int i=0;i<1000;++i){stats.note(sample(77,0x4E3881,0.0f,1.0,1.0,1.0));if(i%10==0)stats.noteWait();}});
    for(auto& t:threads)t.join();
    Lines lines;
    stats.flushNow(1,lines);
    check(lines.v.size()==1&&lines.v[0].find(" calls 4000 ")!=std::string::npos&&lines.v[0].find(" waitgetposes 400 ")!=std::string::npos,"four threads noting 1000 calls each lose none");
  }
  // ---- the fixture the log tool reads -----------------------------------------------------------------------------------------------------
  {
    const std::string built=fixtureLog();
    std::string fixture;
    FILE* f=nullptr;
    if(fopen_s(&f,"tools/pose_gap_fixture.log","rb")==0&&f) {
      char buffer[4096];size_t n;
      while((n=std::fread(buffer,1,sizeof(buffer),f))>0)fixture.append(buffer,n);
      std::fclose(f);
    }
    std::string normalised;
    for(char ch:fixture)if(ch!='\r')normalised+=ch;
    check(!fixture.empty(),"tools/pose_gap_fixture.log is readable from the repo root");
    check(normalised==built,"the log tool's fixture file is what the aggregator writes for the scripted flight, byte for byte (python tools\\edvr_log.py --tally pose reads it; regenerate with --print-pose-fixture)");
    size_t lineCount=0;for(char ch:built)if(ch=='\n')++lineCount;
    check(lineCount==24&&built.find("pose gap: tid 24212 calls 120 from exe+0x4E3881 prediction 0.0 ms target-minus-display mean 11.11 ms (min 11.11 max 11.11)")!=std::string::npos&&
              built.find("failed 3")!=std::string::npos&&built.find("fallback 1")!=std::string::npos&&built.find("from outside prediction 11.0 ms")!=std::string::npos,
          "...and it carries 24 lines: twelve windows of Elite's own request (gap one period, +11.11 ms) and of another caller (gap 9.5 ms)");
  }
}
}  // namespace pose_gap_cases
