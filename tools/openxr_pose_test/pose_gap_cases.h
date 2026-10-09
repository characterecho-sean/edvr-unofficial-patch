#pragma once
// The pose-gap instrument (src/openxr/pose_gap.h; docs\terrain-culling.md round 6): the quaternion angle, the window aggregation, the
// line format, and tools\pose_gap_fixture.log, which the log tool's --self-test reads, held here to exactly what the writer writes.
#include "../../src/openxr/pose_gap.h"
#include "../../src/common/cull_pose.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace pose_gap_cases {
using namespace edvr::openxr;
namespace cp = edvr::cullpose;

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

// tools\pose_gap_fixture.log: 20 two-second windows, written by the aggregator the host uses. The game thread (24212, Elite's one direct
// pose call at 0x4E3881) and another caller (7001, outside the image) run through eight windows with the key off, the head turning
// 10..80 deg/s and the drawn pose a hundredth of a degree behind per deg/s; then six windows of `display` (the gap 0, the angle
// small and flat, one fallback call) and six of `next` (the gap one period, three failed calls in one window).
inline std::string fixtureLog() {
  PoseGapStats stats;
  Lines lines;
  uint64_t now=1000;
  const auto windowOf=[&](uint32_t mode,const std::function<void()>& calls) {
    stats.flushIfDue(now,mode,lines);    // a mode change closes the window before it; the first call only opens one
    calls();
    for(int w=0;w<180;++w)stats.noteWait();
    now+=2000;
    stats.flushIfDue(now,mode,lines);
  };
  for(int w=1;w<=8;++w) {
    const double speed=10.0*w;
    windowOf(0,[&]{
      for(int i=0;i<120;++i)stats.note(sample(kGameThread,0x4E3881,0.0f,9.0+0.5*(i%3),0.01*speed,speed));
      for(int i=0;i<60;++i)stats.note(sample(kOtherThread,kOutside,0.011f,9.0,0.02*speed,speed));
    });
  }
  for(int w=1;w<=6;++w) {
    const double speed=15.0*w;
    windowOf(1,[&]{
      for(int i=0;i<120;++i) {
        auto s=sample(kGameThread,0x4E3881,0.0f,0.0,0.05+0.01*(w%2==0),speed);
        s.fallback=(w==2&&i==0);
        stats.note(s);
      }
    });
  }
  for(int w=1;w<=6;++w) {
    const double speed=15.0*w;
    windowOf(2,[&]{
      for(int i=0;i<120;++i) {
        auto s=sample(kGameThread,0x4E3881,0.0f,11.0+0.1*(i%3),0.1,speed);
        if(w==3&&i<3){s.located=false;s.gapKnown=false;s.angleKnown=false;}
        stats.note(s);
      }
    });
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
    stats.flushIfDue(1000,0,lines);   // opens the window
    const double gaps[4]={12.0,14.0,10.0,12.0},angles[4]={2.0,3.0,1.0,2.0};   // (the max is not the last and the min is not the first)
    for(int i=0;i<4;++i)stats.note(sample(24212,0x4E3881,0.0f,gaps[i],angles[i],30.0));
    stats.noteWait();stats.noteWait();stats.noteWait();stats.noteWait();stats.noteWait();
    stats.flushIfDue(2999,0,lines);
    check(lines.v.empty(),"a window 1999 ms old writes nothing");
    stats.flushIfDue(3000,0,lines);
    check(lines.v.size()==1&&lines.v[0]=="pose gap: mode off tid 24212 calls 4 from exe+0x4E3881 prediction 0.0 ms target-minus-display mean 12.00 ms (min 10.00 max 14.00) angle-to-drawn mean 2.000 max 3.000 deg head 30.0 deg/s waitgetposes 5 failed 0",
          "at 2000 ms the window writes one line per caller: calls, the return RVA, the prediction, the gap mean and range, the angle mean and max, the head speed, the waits, the failures (the exact line)");
    stats.flushIfDue(5000,0,lines);
    check(lines.v.size()==1&&stats.callers()==0&&stats.waits()==0,"...and the next window starts empty: no calls, no line, the wait count back to 0");
  }
  {
    PoseGapStats stats;
    Lines lines;
    stats.flushIfDue(0+1,2,lines);
    stats.note(sample(5,0x4E3881,-0.011f,-3.5,0.5,10.0));
    stats.note(sample(5,0x4E3881,-0.011f,-1.5,1.5,20.0));
    stats.flushIfDue(2001,2,lines);
    check(lines.v.size()==1&&lines.v[0]=="pose gap: mode next tid 5 calls 2 from exe+0x4E3881 prediction -11.0 ms target-minus-display mean -2.50 ms (min -3.50 max -1.50) angle-to-drawn mean 1.000 max 1.500 deg head 15.0 deg/s waitgetposes 0 failed 0",
          "the prediction is a mean in milliseconds (-11 for -0.011 s) and a negative gap keeps its sign, in the mode the window was opened under");
  }
  {
    PoseGapStats stats;
    Lines lines;
    stats.flushIfDue(100,1,lines);
    PoseGapStats::Sample failed;failed.thread=9;failed.rva=0x4E3881;failed.predictionSec=0.0f;
    stats.note(failed);stats.note(failed);
    auto good=sample(9,0x4E3881,0.0f,1.0,1.0,1.0);good.fallback=true;
    stats.note(good);
    stats.flushNow(900,1,lines);
    check(lines.v.size()==1&&lines.v[0]=="pose gap: mode display tid 9 calls 3 from exe+0x4E3881 prediction 0.0 ms target-minus-display mean 1.00 ms (min 1.00 max 1.00) angle-to-drawn mean 1.000 max 1.000 deg head 1.0 deg/s waitgetposes 0 failed 2 fallback 1",
          "calls that failed count in `failed` and are left out of every mean; a fallback adds ` fallback n` to the end of the line");
    PoseGapStats::Sample onlyFailed;onlyFailed.thread=3;onlyFailed.rva=0xFFFFFFFEu;
    stats.note(onlyFailed);
    stats.flushNow(1000,0,lines);
    check(lines.v.size()==2&&lines.v[1]=="pose gap: mode off tid 3 calls 1 from ? prediction 0.0 ms target-minus-display n/a angle-to-drawn n/a head n/a waitgetposes 0 failed 1",
          "a caller with nothing measured says n/a for the gap, the angle and the head speed, and `from ?` when the return address was not captured; flushNow writes under the mode it is given");
    stats.note(sample(4,0xFFFFFFFFu,0.0f,2.0,0.1,5.0));
    stats.flushNow(1100,0,lines);
    check(lines.v.size()==3&&lines.v[2].find(" tid 4 calls 1 from outside prediction")!=std::string::npos,"a caller outside the game's image reads `from outside`");
  }
  // ---- the window: when it closes, what a mode change does -----------------------------------------------------------------------------------
  {
    PoseGapStats stats;
    Lines lines;
    stats.flushIfDue(10000,0,lines);
    stats.note(sample(1,0x4E3881,0.0f,1.0,1.0,1.0));
    stats.flushIfDue(10500,3,lines);
    check(lines.v.size()==1&&lines.v[0].rfind("pose gap: mode off tid 1 calls 1",0)==0,"a mode change closes the window at once, under the mode its calls were located under (off), not the new one");
    stats.note(sample(1,0x4E3881,0.0f,2.0,2.0,2.0));
    stats.flushIfDue(12499,3,lines);
    check(lines.v.size()==1,"...and the new window runs its own 2.0 s from the change");
    stats.flushIfDue(12500,3,lines);
    check(lines.v.size()==2&&lines.v[1].rfind("pose gap: mode display_direct tid 1 calls 1",0)==0,"...and is labelled with the new mode (display_direct)");
    bool names=true;
    for(uint32_t code=0;code<5;++code) {
      PoseGapStats one;Lines l;
      one.flushIfDue(0,code,l);one.note(sample(1,1,0.0f,1.0,1.0,1.0));one.flushNow(1,code,l);
      names=names&&l.v.size()==1&&l.v[0].rfind(std::string("pose gap: mode ")+cp::modeName(cp::modeFromCode(code))+" tid",0)==0;
    }
    check(names,"every mode code is written under its own name");
  }
  {
    PoseGapStats stats;
    Lines lines;
    stats.flushIfDue(0,0,lines);
    for(uint32_t tid=1;tid<=20;++tid)stats.note(sample(tid,0x4E3881,0.0f,1.0,1.0,1.0));
    stats.noteWait();
    stats.flushNow(1,0,lines);
    check(lines.v.size()==17&&lines.count("pose gap: mode off tid ")==16&&lines.has("pose gap: mode off more than 16 callers in a window; 4 calls not counted"),
          "sixteen callers a window are tabled; calls from more are counted and said in a line of their own");
    stats.note(sample(1,0x4E3881,0.0f,1.0,1.0,1.0));
    stats.note(sample(1,0x4E3882,0.0f,1.0,1.0,1.0));
    stats.flushNow(2,0,lines);
    check(lines.v.size()==19,"(a caller is a thread AND a return address: the same thread from another address is another line)");
  }
  {
    PoseGapStats stats;
    std::vector<std::thread> threads;
    for(int t=0;t<4;++t)threads.emplace_back([&]{for(int i=0;i<1000;++i){stats.note(sample(77,0x4E3881,0.0f,1.0,1.0,1.0));if(i%10==0)stats.noteWait();}});
    for(auto& t:threads)t.join();
    Lines lines;
    stats.flushNow(1,0,lines);
    check(lines.v.size()==1&&lines.v[0].find(" calls 4000 ")!=std::string::npos&&lines.v[0].find(" waitgetposes 400 ")!=std::string::npos,"four threads noting 1000 calls each lose none");
  }
  // ---- the target arithmetic agrees with the one the game-pose array already uses --------------------------------------------------------
  {
    bool agree=true;
    const int64_t displays[]={1,200,5000000000,INT64_MAX-12,INT64_MAX-11,INT64_MAX-1,INT64_MAX};
    const int64_t periods[]={-1,0,1,11,11111111,INT64_MAX};
    for(int64_t display:displays)for(int64_t period:periods) {
      int64_t ours=-7;
      const bool a=cp::targetTime(cp::Time::Next,display,period,&ours);
      const bool b=period>0&&display<=INT64_MAX-period;   // nextPredictionTime (space_pose.h): a positive period, no overflow
      agree=agree&&a==b&&(!a||ours==display+period);
    }
    check(agree,"next forms exactly what nextPredictionTime does (a positive period, no overflow), for every display time and period tried");
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
    check(lineCount==28&&built.find("pose gap: mode display tid 24212 calls 120 from exe+0x4E3881 prediction 0.0 ms target-minus-display mean 0.00 ms (min 0.00 max 0.00)")!=std::string::npos&&
              built.find("failed 3")!=std::string::npos&&built.find("fallback 1")!=std::string::npos&&built.find("from outside")!=std::string::npos,
          "...and it carries 28 lines: eight windows of two callers with the key off, six of one with display and six with next");
  }
}
}  // namespace pose_gap_cases
