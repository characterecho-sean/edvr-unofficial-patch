#pragma once
// The pose-gap diagnostic (src/openxr/pose_gap.h; docs\terrain-culling.md): the quaternion angle, the window aggregation, the line format (with
// advanced.cull_pose's mode and the angle to the NEXT frame's drawn pose), what a caller was last handed and when it is measured, and
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

// tools\pose_gap_fixture.log: twelve windows, written by the aggregator the host uses, in two modes of advanced.cull_pose. Windows 1-5 are
// `display`, window 6 is `display` cut to 30 s by the switch to `next` (the mode change closes the window), windows 7-12 are `next`. Elite's own
// "now" request (thread 24212, return 0x4E3881) reads a gap of 0 under display and +11.1 ms under next, a small flat angle to the frame being
// drawn, with one fallback call in the second window and three failed ones in the fifth; its angle to the NEXT frame's drawn pose is head speed
// times one period (0.0111 degrees per deg/s) under display and a hundredth of that under next. Another caller (thread 7001, outside the image,
// a real prediction of 11 ms) is located at the wall clock and reads the true gap, an angle that grows with head speed (0.02 degrees per deg/s),
// and measures no next angle (it is not a game-thread caller).
inline std::string fixtureLog() {
  PoseGapStats stats;
  Lines lines;
  uint64_t now=1000;
  stats.flushIfDue(now,0,lines);   // the first call only opens a window
  for(int w=1;w<=12;++w) {
    const double speed=10.0*w;
    const uint32_t mode=w<=6?0u:2u;
    for(int i=0;i<120;++i) {
      auto s=sample(kGameThread,0x4E3881,0.0f,mode==2u?11.11:0.0,0.05+0.01*(w%2==0),speed);
      s.fallback=(w==2&&i==0);
      if(w==5&&i<3){s.located=false;s.gapKnown=false;s.angleKnown=false;}
      stats.note(s);
      if(!(w==5&&i<3))stats.noteNext(kGameThread,0x4E3881,mode==2u?0.0001*speed*(1.0+0.5*(i==7)):0.0111*speed*(1.0+0.5*(i==7)));
    }
    for(int i=0;i<60;++i)stats.note(sample(kOtherThread,kOutside,0.011f,9.0+0.5*(i%3),0.02*speed,speed));
    for(int k=0;k<5400;++k)stats.noteWait();
    if(w==6) {now+=30000;stats.flushIfDue(now,2,lines);continue;}   // the mode changed under the window: it is written at once, as display
    now+=60000;
    stats.flushIfDue(now,mode,lines);
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
    stats.flushIfDue(60999,0,lines);
    check(lines.v.empty(),"a window 59999 ms old writes nothing");
    stats.flushIfDue(61000,0,lines);
    check(lines.v.size()==1&&lines.v[0]=="pose gap: mode display tid 24212 calls 4 from exe+0x4E3881 prediction 0.0 ms target-minus-display mean 12.00 ms (min 10.00 max 14.00) angle-to-drawn mean 2.000 max 3.000 deg angle-to-next-drawn n/a head 30.0 deg/s waitgetposes 5 failed 0",
          "at 60 s the window writes one line per caller: calls, the return RVA, the prediction, the gap mean and range, the angle mean and max, the head speed, the waits, the failures (the exact line)");
    stats.flushIfDue(121000,0,lines);
    check(lines.v.size()==1&&stats.callers()==0&&stats.waits()==0,"...and the next window starts empty: no calls, no line, the wait count back to 0");
  }
  {
    PoseGapStats stats;
    Lines lines;
    stats.flushIfDue(1,0,lines);
    stats.note(sample(5,0x4E3881,-0.011f,-3.5,0.5,10.0));
    stats.note(sample(5,0x4E3881,-0.011f,-1.5,1.5,20.0));
    stats.flushIfDue(60001,0,lines);
    check(lines.v.size()==1&&lines.v[0]=="pose gap: mode display tid 5 calls 2 from exe+0x4E3881 prediction -11.0 ms target-minus-display mean -2.50 ms (min -3.50 max -1.50) angle-to-drawn mean 1.000 max 1.500 deg angle-to-next-drawn n/a head 15.0 deg/s waitgetposes 0 failed 0",
          "the prediction is a mean in milliseconds (-11 for -0.011 s) and a negative gap keeps its sign");
  }
  {
    PoseGapStats stats;
    Lines lines;
    PoseGapStats::Sample failed;failed.thread=9;failed.rva=0x4E3881;failed.predictionSec=0.0f;
    stats.note(failed);stats.note(failed);
    auto good=sample(9,0x4E3881,0.0f,1.0,1.0,1.0);good.fallback=true;
    stats.note(good);
    stats.flushNow(900,0,lines);
    check(lines.v.size()==1&&lines.v[0]=="pose gap: mode display tid 9 calls 3 from exe+0x4E3881 prediction 0.0 ms target-minus-display mean 1.00 ms (min 1.00 max 1.00) angle-to-drawn mean 1.000 max 1.000 deg angle-to-next-drawn n/a head 1.0 deg/s waitgetposes 0 failed 2 fallback 1",
          "calls that failed count in `failed` and are left out of every mean; a fallback adds ` fallback n` to the end of the line");
    PoseGapStats::Sample onlyFailed;onlyFailed.thread=3;onlyFailed.rva=0xFFFFFFFEu;
    stats.note(onlyFailed);
    stats.flushNow(1000,0,lines);
    check(lines.v.size()==2&&lines.v[1]=="pose gap: mode display tid 3 calls 1 from ? prediction 0.0 ms target-minus-display n/a angle-to-drawn n/a angle-to-next-drawn n/a head n/a waitgetposes 0 failed 1",
          "a caller with nothing measured says n/a for the gap, the angle and the head speed, and `from ?` when the return address was not captured");
    stats.note(sample(4,0xFFFFFFFFu,0.0f,2.0,0.1,5.0));
    stats.flushNow(1100,0,lines);
    check(lines.v.size()==3&&lines.v[2].find(" tid 4 calls 1 from outside prediction")!=std::string::npos,"a caller outside the game's image reads `from outside`");
  }
  // ---- the window ----------------------------------------------------------------------------------------------------------------------
  {
    PoseGapStats stats;
    Lines lines;
    stats.flushIfDue(10000,0,lines);
    stats.note(sample(1,0x4E3881,0.0f,1.0,1.0,1.0));
    stats.flushIfDue(69999,0,lines);
    check(lines.v.empty(),"a window is 60 s: 59999 ms is not enough");
    stats.flushIfDue(70000,0,lines);
    check(lines.v.size()==1&&lines.v[0].rfind("pose gap: mode display tid 1 calls 1 ",0)==0,"...and 60000 ms is");
    stats.note(sample(1,0x4E3881,0.0f,2.0,2.0,2.0));
    stats.flushIfDue(129999,0,lines);
    check(lines.v.size()==1,"...and the next window runs its own 60 s from that flush, not from the first one's opening");
    stats.flushIfDue(130000,0,lines);
    check(lines.v.size()==2,"(and writes its line then)");
    check(kPoseGapWindowMs==60000,"(the window is 60 seconds)");
  }
  {
    PoseGapStats stats;
    Lines lines;
    stats.flushNow(0,0,lines);
    for(uint32_t tid=1;tid<=20;++tid)stats.note(sample(tid,0x4E3881,0.0f,1.0,1.0,1.0));
    stats.noteWait();
    stats.flushNow(1,0,lines);
    check(lines.v.size()==17&&lines.count("pose gap: mode display tid ")==16&&lines.has("pose gap: mode display more than 16 callers in a window; 4 calls not counted"),
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
  // ---- the mode ---------------------------------------------------------------------------------------------------------------------------
  {
    PoseGapStats stats;
    Lines lines;
    stats.flushIfDue(1000,2,lines);   // the first call opens a window in the mode it is given
    stats.note(sample(5,0x4E3881,0.0f,11.11,0.1,30.0));
    stats.flushNow(1500,2,lines);
    check(lines.v.size()==1&&lines.v[0]=="pose gap: mode next tid 5 calls 1 from exe+0x4E3881 prediction 0.0 ms target-minus-display mean 11.11 ms (min 11.11 max 11.11) angle-to-drawn mean 0.100 max 0.100 deg angle-to-next-drawn n/a head 30.0 deg/s waitgetposes 0 failed 0",
          "a line names the mode its window was written under (the exact line, mode next)");
    const char* names[5]={"display","now","next","display_direct","next_direct"};
    bool all=true;
    for(uint32_t code=0;code<5;++code) {
      PoseGapStats one;Lines l;
      one.note(sample(1,0x4E3881,0.0f,0.0,0.0,0.0));
      one.flushNow(0,code,l);
      all=all&&l.v.size()==1&&l.v[0].rfind(std::string("pose gap: mode ")+names[code]+" tid 1 ",0)==0;
    }
    check(all,"every mode code 0..4 is written by its name: display, now, next, display_direct, next_direct");
    PoseGapStats beyond;Lines l;
    beyond.note(sample(1,0x4E3881,0.0f,0.0,0.0,0.0));
    beyond.flushNow(0,99,l);
    check(l.v.size()==1&&l.v[0].rfind("pose gap: mode display tid 1 ",0)==0,"...and a code past the last reads the default, display");
  }
  {
    // A window opened under a mode is written under it (the first flush only opens the window; no call has changed the mode since).
    PoseGapStats stats;
    Lines lines;
    stats.flushIfDue(1000,2,lines);
    stats.note(sample(5,0x4E3881,0.0f,11.11,0.1,30.0));
    stats.flushIfDue(1000+60000,2,lines);
    check(lines.v.size()==1&&lines.v[0].rfind("pose gap: mode next tid 5 calls 1 ",0)==0,"a window opened under next and still under next is written under next (the mode it opened with, not the default)");
    PoseGapStats overflow;
    Lines overflowLines;
    overflow.flushNow(0,3,overflowLines);
    for(uint32_t tid=1;tid<=18;++tid)overflow.note(sample(tid,0x4E3881,0.0f,1.0,1.0,1.0));
    overflow.flushNow(1,3,overflowLines);
    check(overflowLines.has("pose gap: mode display_direct more than 16 callers in a window; 2 calls not counted"),"the line that says callers were not counted carries the mode too");
  }
  {
    // The mode changing under a window closes it: its calls were located under the mode they were made in.
    PoseGapStats stats;
    Lines lines;
    stats.flushIfDue(1000,0,lines);
    stats.note(sample(5,0x4E3881,0.0f,0.0,0.1,30.0));
    stats.noteWait();
    stats.flushIfDue(2000,0,lines);
    check(lines.v.empty(),"a window 1 s old in the same mode writes nothing");
    stats.flushIfDue(2001,2,lines);
    check(lines.v.size()==1&&lines.v[0].rfind("pose gap: mode display tid 5 calls 1 ",0)==0&&lines.v[0].find(" waitgetposes 1 ")!=std::string::npos,
          "a change of mode under it closes it AT ONCE and writes it under the mode its calls were made in (display), not the new one");
    stats.note(sample(5,0x4E3881,0.0f,11.11,0.1,30.0));
    stats.note(sample(5,0x4E3881,0.0f,11.11,0.1,30.0));
    stats.flushIfDue(2001+59999,2,lines);
    check(lines.v.size()==1,"...the new window runs its own 60 s from the change");
    stats.flushIfDue(2001+60000,2,lines);
    check(lines.v.size()==2&&lines.v[1].rfind("pose gap: mode next tid 5 calls 2 ",0)==0,"...and is written under the new mode, with only its own calls");
    stats.flushIfDue(2001+60001,0,lines);
    check(lines.v.size()==2,"(a window with nothing in it writes nothing, even when the mode changes)");
    stats.note(sample(5,0x4E3881,0.0f,0.0,0.1,30.0));
    stats.flushIfDue(2001+60002,4,lines);
    check(lines.v.size()==3&&lines.v[2].rfind("pose gap: mode display tid 5 calls 1 ",0)==0,"(and the next change writes the window the last change opened, under display)");
  }
  // ---- the angle to the NEXT frame's drawn pose ------------------------------------------------------------------------------------------------
  {
    PoseGapStats stats;
    Lines lines;
    stats.flushIfDue(0,0,lines);
    stats.note(sample(24212,0x4E3881,0.0f,0.0,0.0,250.0));
    stats.note(sample(24212,0x4E3881,0.0f,0.0,0.0,250.0));
    stats.note(sample(24212,0x4E3881,0.0f,0.0,0.0,250.0));
    stats.noteNext(24212,0x4E3881,2.5);
    stats.noteNext(24212,0x4E3881,3.0);
    stats.noteNext(24212,0x4E3881,0.5);
    stats.noteNext(99,0x4E3881,50.0);       // a caller this window never noted
    stats.noteNext(24212,0x1234,50.0);      // ...and the same thread at another return address
    stats.flushNow(1,0,lines);
    check(lines.v.size()==1&&lines.v[0]=="pose gap: mode display tid 24212 calls 3 from exe+0x4E3881 prediction 0.0 ms target-minus-display mean 0.00 ms (min 0.00 max 0.00) angle-to-drawn mean 0.000 max 0.000 deg angle-to-next-drawn mean 2.000 max 3.000 deg head 250.0 deg/s waitgetposes 0 failed 0",
          "the angle to the next drawn pose is a mean and a max over the frames measured, beside the angle to the current one (the exact line)");
    stats.note(sample(1,0x4E3881,0.0f,0.0,0.0,1.0));
    stats.flushNow(2,0,lines);
    check(lines.v.size()==2&&lines.v[1].find("angle-to-next-drawn n/a head")!=std::string::npos&&lines.v.size()==2,
          "a caller with no frame measured says n/a for it, and a measure for a caller not in the window made no line of its own");
    stats.note(sample(1,0x4E3881,0.0f,0.0,0.0,1.0));
    stats.noteNext(1,0x4E3881,0.0);
    stats.flushNow(3,0,lines);
    check(lines.v.size()==3&&lines.v[2].find("angle-to-next-drawn mean 0.000 max 0.000 deg head")!=std::string::npos,
          "a measured 0 is 0.000, not n/a (an angle to the next frame of exactly nothing is the point of cull_pose next)");
    stats.note(sample(2,0x4E3881,0.0f,0.0,0.0,1.0));
    stats.flushNow(4,0,lines);
    check(lines.v.size()==4&&lines.v[3].find("angle-to-next-drawn n/a head")!=std::string::npos,"the next window starts with the next angle forgotten");
  }
  // ---- what each caller was last handed ----------------------------------------------------------------------------------------------------
  {
    const double h=3.14159265358979323846/180.0;
    const auto yaw=[&](double degrees,float q[4]){q[0]=0;q[1]=float(std::sin(degrees*0.5*h));q[2]=0;q[3]=float(std::cos(degrees*0.5*h));};
    struct Seen {uint32_t thread,rva;double deg;};
    std::vector<Seen> seen;
    const auto emit=[&](uint32_t t,uint32_t r,double d){seen.push_back({t,r,d});};
    float q10[4],q128[4],q0[4];
    yaw(10.0,q10);yaw(12.8,q128);yaw(0.0,q0);
    {
      HandedOutPoses table;
      table.note(24212,0x4E3881,10,q10);
      table.resolve(11,true,q128,emit);
      check(seen.size()==1&&seen[0].thread==24212&&seen[0].rva==0x4E3881&&close(seen[0].deg,2.8,0.001),
            "a pose handed out during frame 10 is measured against frame 11's render pose when that is published: 2.8 degrees (the lag H-onef predicts at 250 deg/s)");
      table.resolve(12,true,q0,emit);
      check(seen.size()==1,"...once: the next publish measures nothing (it was spent)");
    }
    seen.clear();
    {
      HandedOutPoses table;
      table.note(24212,0x4E3881,10,q0);
      table.note(24212,0x4E3881,10,q10);   // the same frame again: the LAST pose handed out is the one that is measured
      table.resolve(11,true,q10,emit);
      check(seen.size()==1&&close(seen[0].deg,0.0,1e-4)&&table.tracked()==1,"a caller that asks twice in a frame is measured on the last pose it was handed, once");
    }
    seen.clear();
    {
      HandedOutPoses table;
      table.note(24212,0x4E3881,10,q10);
      table.resolve(12,true,q128,emit);
      check(seen.empty(),"a pose handed out two frames ago (a frame was never published in between) is dropped, not measured against the wrong one");
      table.note(24212,0x4E3881,13,q10);
      table.resolve(13,true,q128,emit);
      check(seen.empty(),"a pose handed out during the frame being published is not measured against it (the next frame's pose is the one drawn)");
      table.resolve(14,true,q128,emit);
      check(seen.empty(),"...and it was spent by that publish");
    }
    seen.clear();
    {
      HandedOutPoses table;
      table.note(24212,0x4E3881,10,q10);
      table.resolve(11,false,q128,emit);
      table.resolve(12,true,q128,emit);
      check(seen.empty(),"a render pose that is not valid measures nothing, and spends the hand-out");
      table.note(24212,0x4E3881,UINT64_MAX,q10);
      table.resolve(0,true,q128,emit);
      table.resolve(UINT64_MAX,true,q128,emit);
      check(seen.empty(),"frame 0 has no frame before it: the sequence does not wrap");
    }
    seen.clear();
    {
      HandedOutPoses table;
      table.note(24212,0x4E3881,10,q10);
      table.note(24212,0x1234567,10,q0);
      table.note(7001,0x4E3881,10,q128);
      table.resolve(11,true,q128,emit);
      bool a=false,b=false,c=false;
      for(const auto& x:seen){
        if(x.thread==24212&&x.rva==0x4E3881)a=close(x.deg,2.8,0.001);
        if(x.thread==24212&&x.rva==0x1234567)b=close(x.deg,12.8,0.001);
        if(x.thread==7001&&x.rva==0x4E3881)c=close(x.deg,0.0,1e-4);
      }
      check(seen.size()==3&&a&&b&&c,"each caller is a thread AND a return address: three callers are three measures, each against its own pose");
      table.clear();
      table.note(24212,0x4E3881,11,q10);
      table.clear();
      seen.clear();
      table.resolve(12,true,q128,emit);
      check(seen.empty()&&table.tracked()==0,"clear forgets everything handed out (the session, the origin or the geometry was invalidated)");
    }
    seen.clear();
    {
      HandedOutPoses table;
      for(uint32_t t=1;t<=20;++t)table.note(t,0x4E3881,10,q10);
      table.resolve(11,true,q128,emit);
      bool first=seen.size()==16;
      for(const auto& x:seen)if(x.thread>16)first=false;
      check(table.tracked()==16&&first,"sixteen callers are tracked; a seventeenth is not, and the sixteen are still measured");
    }
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
    check(lineCount==24&&built.find("pose gap: mode display tid 24212 calls 120 from exe+0x4E3881 prediction 0.0 ms target-minus-display mean 0.00 ms (min 0.00 max 0.00)")!=std::string::npos&&
              built.find("pose gap: mode next tid 24212 calls 120 from exe+0x4E3881 prediction 0.0 ms target-minus-display mean 11.11 ms (min 11.11 max 11.11)")!=std::string::npos&&
              built.find("failed 3")!=std::string::npos&&built.find("fallback 1")!=std::string::npos&&built.find("from outside prediction 11.0 ms")!=std::string::npos,
          "...and it carries 24 lines: twelve windows of Elite's own request (gap 0 under display, 11.11 ms under next) and of another caller (gap 9.5 ms), the sixth cut short by the switch");
  }
}
}  // namespace pose_gap_cases
