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
// Pure: the clock and the sink are passed in, so tools\openxr_pose_test drives every case and holds tools\pose_gap_fixture.log to exactly
// what this writes (tools\edvr_log.py --tally pose reads it).
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
  // From the WaitGetPoses publish point: write the window's lines when it is 60 s old. The first call only opens a window. Lines are made under
  // the lock and written outside it.
  template<class Sink>
  void flushIfDue(uint64_t nowMs,Sink&& sink) {
    Out out;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if(!started_){started_=true;startMs_=nowMs;return;}
      if(nowMs-startMs_<kPoseGapWindowMs)return;
      collect(out);startMs_=nowMs;
    }
    for(unsigned i=0;i<out.count;++i)sink(out.lines[i]);
  }
  // The window as it stands, now (the end of a run, a rig).
  template<class Sink>
  void flushNow(uint64_t nowMs,Sink&& sink) {
    Out out;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      collect(out);startMs_=nowMs;started_=true;
    }
    for(unsigned i=0;i<out.count;++i)sink(out.lines[i]);
  }
  unsigned callers() const {std::lock_guard<std::mutex> lock(mutex_);return used_;}
  uint64_t waits() const {std::lock_guard<std::mutex> lock(mutex_);return waits_;}

  // The line for one caller. `from` reads exe+0xRVA, outside (the caller is not in the game''s image) or ? (not captured).
  static const char* line(char (&buffer)[352],uint32_t thread,uint32_t rva,uint64_t calls,double predictionMs,
                          bool gapKnown,double gapMean,double gapMin,double gapMax,bool angleKnown,double angleMean,double angleMax,
                          bool speedKnown,double speed,uint64_t waits,uint64_t failed,uint64_t fallbacks) {
    char from[32],gap[96],angle[80],head[40],tail[40]="";
    if(rva==0xFFFFFFFFu)std::snprintf(from,sizeof(from),"outside");
    else if(rva==0xFFFFFFFEu)std::snprintf(from,sizeof(from),"?");
    else std::snprintf(from,sizeof(from),"exe+0x%X",rva);
    if(gapKnown)std::snprintf(gap,sizeof(gap),"mean %.2f ms (min %.2f max %.2f)",gapMean,gapMin,gapMax);else std::snprintf(gap,sizeof(gap),"n/a");
    if(angleKnown)std::snprintf(angle,sizeof(angle),"mean %.3f max %.3f deg",angleMean,angleMax);else std::snprintf(angle,sizeof(angle),"n/a");
    if(speedKnown)std::snprintf(head,sizeof(head),"%.1f deg/s",speed);else std::snprintf(head,sizeof(head),"n/a");
    if(fallbacks)std::snprintf(tail,sizeof(tail)," fallback %llu",(unsigned long long)fallbacks);
    std::snprintf(buffer,sizeof(buffer),
      "pose gap: tid %lu calls %llu from %s prediction %.1f ms target-minus-display %s angle-to-drawn %s head %s waitgetposes %llu failed %llu%s",
      (unsigned long)thread,(unsigned long long)calls,from,predictionMs,gap,angle,head,(unsigned long long)waits,(unsigned long long)failed,tail);
    return buffer;
  }

 private:
  struct Key {
    uint32_t thread=0,rva=0;
    uint64_t calls=0,failed=0,fallbacks=0,gapN=0,angleN=0,speedN=0;
    double predictionSum=0,gapSum=0,gapMin=0,gapMax=0,angleSum=0,angleMax=0,speedSum=0;
  };
  struct Out {unsigned count=0;char lines[kPoseGapKeys+1][352]{};};
  void collect(Out& out) {
    for(unsigned i=0;i<used_;++i) {
      const Key& k=keys_[i];
      line(out.lines[out.count++],k.thread,k.rva,k.calls,k.calls?k.predictionSum/double(k.calls)*1000.0:0.0,
        k.gapN!=0,k.gapN?k.gapSum/double(k.gapN):0.0,k.gapMin,k.gapMax,k.angleN!=0,k.angleN?k.angleSum/double(k.angleN):0.0,k.angleMax,
        k.speedN!=0,k.speedN?k.speedSum/double(k.speedN):0.0,waits_,k.failed,k.fallbacks);
    }
    if(dropped_&&out.count<kPoseGapKeys+1)
      std::snprintf(out.lines[out.count++],sizeof(out.lines[0]),"pose gap: more than %u callers in a window; %llu calls not counted",
        kPoseGapKeys,(unsigned long long)dropped_);
    used_=0;dropped_=0;waits_=0;
  }
  mutable std::mutex mutex_;
  Key keys_[kPoseGapKeys]{};
  unsigned used_=0;
  uint64_t dropped_=0,waits_=0,startMs_=0;
  bool started_=false;
};
}
