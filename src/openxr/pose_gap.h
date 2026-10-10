#pragma once

// The pose-gap diagnostic (docs\terrain-culling.md). Always on, cheap: one mutex-held update per GetDeviceToAbsoluteTrackingPose call, one
// line per caller (thread, return RVA) every 60 s, written from the WaitGetPoses publish point.
//
// WHAT IT MEASURES. Elite's game thread asks IVRSystem::GetDeviceToAbsoluteTrackingPose for "now"; the render thread draws the pose
// WaitGetPoses gave it, located at the frame's predictedDisplayTime. The gap between the two instants is the lag Elite's terrain culling was
// working with (41-44 ms before the fix, flight 3); the fix (head_pose_time.h) answers Elite's own "now" at the display time, so under it the
// gap reads 0 for Elite and shows the true lag for any other caller. Per call this records who asked (thread, return RVA), the prediction
// passed, the located instant minus the latest frame's display time, the angle between the pose handed back and the pose that frame was
// drawn with, and how fast the head was turning. It is a diagnostic that stays: a caller that appears after a game update, or a fix that
// stops acting, shows here.
//
// THE SECOND ANGLE (advanced.cull_pose, TEMPORARY; common/cull_pose.h). Elite's game thread prepares frame N+1 while the render thread
// draws N, so the pose a game-thread call is handed during N is drawn one frame later. `angle-to-next-drawn` is the angle between the
// orientation each such caller (thread, return RVA) was last handed during frame N and the render orientation WaitGetPoses publishes for
// N+1: the culling lag that is actually drawn. About 0 when the culling pose is located one period ahead (cull_pose next); head speed
// times one period (2.8 degrees at 250 deg/s, 11 ms) when it is located at the display time (cull_pose display).
//
// Pure: the clock and the sink are passed in, so tools\openxr_pose_test drives every case and holds tools\pose_gap_fixture.log to exactly
// what this writes (tools\edvr_log.py --tally pose reads it).
#include "../common/cull_pose.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace edvr::openxr {

// The angle, degrees, between two unit quaternions given as (x, y, z, w): the relative rotation conj(a) * b turns by
// 2 * atan2(|v|, |w|). Not 2 * acos(w), whose slope is flat at zero: a frame's gap is a few hundredths of a degree.
inline double quaternionAngleDegrees(const float a[4], const float b[4]) {
  const double ax=a[0],ay=a[1],az=a[2],aw=a[3],bx=b[0],by=b[1],bz=b[2],bw=b[3];
  const double w=aw*bw+ax*bx+ay*by+az*bz;
  const double x=aw*bx-ax*bw-ay*bz+az*by;
  const double y=aw*by+ax*bz-ay*bw-az*bx;
  const double z=aw*bz-ax*by+ay*bx-az*bw;
  return 2.0*std::atan2(std::sqrt(x*x+y*y+z*z),std::fabs(w))*(180.0/3.14159265358979323846);
}
inline double angularSpeedDegrees(float x,float y,float z) {
  return std::sqrt(double(x)*x+double(y)*y+double(z)*z)*(180.0/3.14159265358979323846);
}

// How many callers (thread, return RVA) one window keeps; more are counted and said.
constexpr unsigned kPoseGapKeys=16;
constexpr uint64_t kPoseGapWindowMs=60000;

class PoseGapStats {
 public:
  struct Sample {
    uint32_t thread=0,rva=0;
    float predictionSec=0;
    bool located=false;       // a valid pose came back
    bool fallback=false;      // a Display/Next call that could not be formed and was located at now + prediction
    bool gapKnown=false,angleKnown=false,speedKnown=false;
    double gapMs=0,angleDeg=0,speedDegPerSec=0;
  };
  // The angle from what a caller was handed during one frame to the render pose of the next (HandedOutPoses). A caller this window did
  // not note is not added: the call that was handed the pose is in the same window (the window only closes at the publish that measures).
  void noteNext(uint32_t thread,uint32_t rva,double angleDeg) {
    std::lock_guard<std::mutex> lock(mutex_);
    for(unsigned i=0;i<used_;++i)if(keys_[i].thread==thread&&keys_[i].rva==rva) {
      ++keys_[i].nextN;keys_[i].nextSum+=angleDeg;keys_[i].nextMax=(std::max)(keys_[i].nextMax,angleDeg);return;
    }
  }
  void note(const Sample& s) {
    std::lock_guard<std::mutex> lock(mutex_);
    Key* key=nullptr;
    for(unsigned i=0;i<used_;++i)if(keys_[i].thread==s.thread&&keys_[i].rva==s.rva){key=&keys_[i];break;}
    if(!key) {
      if(used_>=kPoseGapKeys){++dropped_;return;}
      key=&keys_[used_++];*key=Key{};key->thread=s.thread;key->rva=s.rva;
    }
    ++key->calls;key->predictionSum+=double(s.predictionSec);
    if(!s.located)++key->failed;
    if(s.fallback)++key->fallbacks;
    if(s.gapKnown){
      if(!key->gapN){key->gapMin=key->gapMax=s.gapMs;} else {key->gapMin=(std::min)(key->gapMin,s.gapMs);key->gapMax=(std::max)(key->gapMax,s.gapMs);}
      ++key->gapN;key->gapSum+=s.gapMs;
    }
    if(s.angleKnown){++key->angleN;key->angleSum+=s.angleDeg;key->angleMax=(std::max)(key->angleMax,s.angleDeg);}
    if(s.speedKnown){++key->speedN;key->speedSum+=s.speedDegPerSec;}
  }
  // One WaitGetPoses completed.
  void noteWait() {std::lock_guard<std::mutex> lock(mutex_);++waits_;}
  // From the WaitGetPoses publish point: write the window's lines when it is 60 s old, or when the cull_pose mode changed under it (so a
  // line's mode is the mode its calls were located under; `mode` is a cullpose::Mode code). The first call only opens a window. Lines are
  // made under the lock and written outside it.
  template<class Sink>
  void flushIfDue(uint64_t nowMs,uint32_t mode,Sink&& sink) {
    Out out;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if(!started_){started_=true;startMs_=nowMs;windowMode_=mode;return;}
      if(nowMs-startMs_<kPoseGapWindowMs&&mode==windowMode_)return;
      collect(out,windowMode_);startMs_=nowMs;windowMode_=mode;
    }
    for(unsigned i=0;i<out.count;++i)sink(out.lines[i]);
  }
  // The window as it stands, now, written under `mode` (the end of a run, a rig).
  template<class Sink>
  void flushNow(uint64_t nowMs,uint32_t mode,Sink&& sink) {
    Out out;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      collect(out,mode);startMs_=nowMs;windowMode_=mode;started_=true;
    }
    for(unsigned i=0;i<out.count;++i)sink(out.lines[i]);
  }
  unsigned callers() const {std::lock_guard<std::mutex> lock(mutex_);return used_;}
  uint64_t waits() const {std::lock_guard<std::mutex> lock(mutex_);return waits_;}

  // The line for one caller. `from` reads exe+0xRVA, outside (the caller is not in the game''s image) or ? (not captured).
  static const char* line(char (&buffer)[400],const char* mode,uint32_t thread,uint32_t rva,uint64_t calls,double predictionMs,
                          bool gapKnown,double gapMean,double gapMin,double gapMax,bool angleKnown,double angleMean,double angleMax,
                          bool nextKnown,double nextMean,double nextMax,
                          bool speedKnown,double speed,uint64_t waits,uint64_t failed,uint64_t fallbacks) {
    char from[32],gap[96],angle[80],next[80],head[40],tail[40]="";
    if(rva==0xFFFFFFFFu)std::snprintf(from,sizeof(from),"outside");
    else if(rva==0xFFFFFFFEu)std::snprintf(from,sizeof(from),"?");
    else std::snprintf(from,sizeof(from),"exe+0x%X",rva);
    if(gapKnown)std::snprintf(gap,sizeof(gap),"mean %.2f ms (min %.2f max %.2f)",gapMean,gapMin,gapMax);else std::snprintf(gap,sizeof(gap),"n/a");
    if(angleKnown)std::snprintf(angle,sizeof(angle),"mean %.3f max %.3f deg",angleMean,angleMax);else std::snprintf(angle,sizeof(angle),"n/a");
    if(nextKnown)std::snprintf(next,sizeof(next),"mean %.3f max %.3f deg",nextMean,nextMax);else std::snprintf(next,sizeof(next),"n/a");
    if(speedKnown)std::snprintf(head,sizeof(head),"%.1f deg/s",speed);else std::snprintf(head,sizeof(head),"n/a");
    if(fallbacks)std::snprintf(tail,sizeof(tail)," fallback %llu",(unsigned long long)fallbacks);
    std::snprintf(buffer,sizeof(buffer),
      "pose gap: mode %s tid %lu calls %llu from %s prediction %.1f ms target-minus-display %s angle-to-drawn %s angle-to-next-drawn %s head %s waitgetposes %llu failed %llu%s",
      mode,(unsigned long)thread,(unsigned long long)calls,from,predictionMs,gap,angle,next,head,(unsigned long long)waits,(unsigned long long)failed,tail);
    return buffer;
  }

 private:
  struct Key {
    uint32_t thread=0,rva=0;
    uint64_t calls=0,failed=0,fallbacks=0,gapN=0,angleN=0,nextN=0,speedN=0;
    double predictionSum=0,gapSum=0,gapMin=0,gapMax=0,angleSum=0,angleMax=0,nextSum=0,nextMax=0,speedSum=0;
  };
  struct Out {unsigned count=0;char lines[kPoseGapKeys+1][400]{};};
  void collect(Out& out,uint32_t label) {
    const char* mode=cullpose::modeName(cullpose::modeFromCode(label));
    for(unsigned i=0;i<used_;++i) {
      const Key& k=keys_[i];
      line(out.lines[out.count++],mode,k.thread,k.rva,k.calls,k.calls?k.predictionSum/double(k.calls)*1000.0:0.0,
        k.gapN!=0,k.gapN?k.gapSum/double(k.gapN):0.0,k.gapMin,k.gapMax,k.angleN!=0,k.angleN?k.angleSum/double(k.angleN):0.0,k.angleMax,
        k.nextN!=0,k.nextN?k.nextSum/double(k.nextN):0.0,k.nextMax,
        k.speedN!=0,k.speedN?k.speedSum/double(k.speedN):0.0,waits_,k.failed,k.fallbacks);
    }
    if(dropped_&&out.count<kPoseGapKeys+1)
      std::snprintf(out.lines[out.count++],sizeof(out.lines[0]),"pose gap: mode %s more than %u callers in a window; %llu calls not counted",
        mode,kPoseGapKeys,(unsigned long long)dropped_);
    used_=0;dropped_=0;waits_=0;
  }
  mutable std::mutex mutex_;
  Key keys_[kPoseGapKeys]{};
  unsigned used_=0;
  uint64_t dropped_=0,waits_=0,startMs_=0;
  uint32_t windowMode_=0;
  bool started_=false;
};

// What each game-thread caller (thread, return RVA) was last handed, and during which frame: one orientation per caller, no allocation. Owner
// thread only (every pose call is located there), so no lock. `note` records a hand-out during the latest published frame `frameSequence`;
// `resolve` is called when the render pose of frame `sequence` is published and measures every caller last handed a pose during
// `sequence - 1` against it, once; a hand-out from any other frame (a frame in between was not published, the session restarted) is dropped
// unmeasured, and so is one measured against a render pose that is not valid. A caller beyond kPoseGapKeys is not tracked.
class HandedOutPoses {
 public:
  void note(uint32_t thread,uint32_t rva,uint64_t frameSequence,const float orientation[4]) {
    Entry* e=nullptr;
    for(unsigned i=0;i<used_;++i)if(entries_[i].thread==thread&&entries_[i].rva==rva){e=&entries_[i];break;}
    if(!e) {
      if(used_>=kPoseGapKeys)return;
      e=&entries_[used_++];e->thread=thread;e->rva=rva;
    }
    e->sequence=frameSequence;e->live=true;
    for(unsigned k=0;k<4;++k)e->orientation[k]=orientation[k];
  }
  // emit(thread, rva, angleDegrees) once per measured caller.
  template<class Emit>
  void resolve(uint64_t sequence,bool renderValid,const float render[4],Emit&& emit) {
    for(unsigned i=0;i<used_;++i) {
      Entry& e=entries_[i];
      if(!e.live)continue;
      e.live=false;
      if(renderValid&&sequence>0&&e.sequence==sequence-1)emit(e.thread,e.rva,quaternionAngleDegrees(e.orientation,render));
    }
  }
  void clear() {used_=0;}
  unsigned tracked() const {return used_;}
 private:
  struct Entry {
    uint32_t thread=0,rva=0;
    uint64_t sequence=0;
    float orientation[4]{0,0,0,1};
    bool live=false;
  };
  Entry entries_[kPoseGapKeys]{};
  unsigned used_=0;
};
}
