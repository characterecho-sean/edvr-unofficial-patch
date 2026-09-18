#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <memory>

namespace edvr::openxr {

// Fixed-storage, wall-clock accounting for the caller/owner handoffs around one
// complete OpenVR frame. Ticks are supplied by the caller so the state machine
// is deterministic in the native fixture and adds no clocks, waits or GPU work.
class FrameCycleStats final {
 public:
  static constexpr unsigned capacity=4096;
  enum Missing : unsigned { BadClock, Reentrant, WrongThread, BadSequence,
    PartialStereo, DuplicateEye, DirectOwner, ShapeChange, Overflow, MissingCount };
  struct Shape { uint32_t width[2]{},height[2]{},outputWidth[2]{},outputHeight[2]{},pacing=0,shouldRender=0,sceneReady=0; uint64_t generation=0,featureEpoch=0; };
  struct Sample {
    uint64_t sequence=0;
    double cycle=0,beforeFirst=0,firstSubmit=0,betweenEyes=0,secondSubmit=0,
      afterSecond=0,nextWait=0,residual=0;
    double waitOwner=0,submitOwner[2]{},renderPark[2]{};
    uint32_t eyeOrder[2]{},callerThread=0,waitThread=0;
  };
  struct Dist { double mean=0,p50=0,p95=0; };
  struct Report {
    uint64_t window=0,firstSequence=0,lastSequence=0,elapsedMs=0,admitted=0;
    unsigned valid=0; Shape shape{}; std::array<uint64_t,MissingCount> missing{};
    Dist cycle,beforeFirst,firstSubmit,betweenEyes,secondSubmit,afterSecond,nextWait,residual,waitOwner;
    Dist submitOwner[2],renderPark[2]; uint32_t callerThread=0,waitThread=0; bool threadConsistent=false;
  };
  FrameCycleStats():samples_(std::make_unique<std::array<Sample,capacity>>()){}

  void enabled() noexcept { std::lock_guard<std::mutex> l(m_); enabled_=true; }
  bool firstComplete() const noexcept { std::lock_guard<std::mutex> l(m_); return everComplete_; }
  void reset(Missing why=ShapeChange) noexcept { std::lock_guard<std::mutex> l(m_); fail(why); current_={}; wait_={}; }

  uint64_t waitCallerBegin(uint64_t tick,uint32_t thread) noexcept {
    std::lock_guard<std::mutex> l(m_); enabled_=true;
    observeThread(observedWaitThread_,thread);if(!tick||wait_.open){++missing_[Reentrant];return 0;}
    wait_={};wait_.open=true;wait_.token=++nextToken_;wait_.begin=tick;wait_.thread=thread;
    if(current_.active&&current_.eyes==2){current_.afterSecond=tick-current_.submitReturn[1];current_.afterSecondReady=true;}
    return wait_.token;
  }
  void waitOwnerBegin(uint64_t token,uint64_t tick) noexcept { std::lock_guard<std::mutex> l(m_); if(token&&wait_.token==token&&!wait_.ownerBegin)wait_.ownerBegin=tick;else ++missing_[Reentrant]; }
  void waitOwnerEnd(uint64_t token,uint64_t tick) noexcept { std::lock_guard<std::mutex> l(m_); if(token&&wait_.token==token&&wait_.ownerBegin&&tick>=wait_.ownerBegin)wait_.ownerEnd=tick;else ++missing_[BadClock]; }
  void waitCallerEnd(uint64_t token,uint64_t sequence,uint64_t tick,uint64_t nowMs,uint32_t thread,const Shape& shape,bool ok) noexcept {
    std::lock_guard<std::mutex> l(m_);
    if(!ok||!token||wait_.token!=token||!wait_.open||!ordered(wait_.begin,wait_.ownerBegin,wait_.ownerEnd,tick)||thread!=wait_.thread){advanceWindow(nowMs,shape,sequence);bad(!ok?PartialStereo:BadClock);if(wait_.token==token)wait_={};return;}
    if(current_.active&& !sameShape(current_.shape,shape)){++missing_[ShapeChange];if(windowStartMs_)makeReport(lastAttemptedSequence_,nowMs);current_={};}
    else if(current_.active) finishCurrent(tick,nowMs,thread);
    advanceWindow(nowMs,shape,sequence);
    current_={};current_.active=true;current_.sequence=sequence;current_.waitReturn=tick;
    current_.waitRound=tick-wait_.begin;current_.waitOwner=wait_.ownerEnd-wait_.ownerBegin;
    current_.callerThread=thread;current_.waitThread=thread;current_.shape=shape;current_.atMs=nowMs;wait_={};
  }
  uint64_t submitCallerBegin(unsigned eye,uint64_t tick,uint32_t thread) noexcept {
    std::lock_guard<std::mutex> l(m_);
    observeThread(observedSubmitThread_,thread);if(!current_.active||eye>1||current_.submitOpen||current_.eyes>=2){++missing_[Reentrant];return 0;}
    if(current_.seenEyes&(1u<<eye)){bad(DuplicateEye);return 0;}
    if(!tick||tick<current_.waitReturn||(current_.eyes&&tick<current_.submitReturn[0])){bad(BadClock);return 0;}
    current_.submitOpen=true;current_.submitToken=++nextToken_;current_.submitBegin=tick;current_.submitThread=thread;
    current_.order[current_.eyes]=eye;
    if(!current_.eyes)current_.beforeFirst=tick-current_.waitReturn;
    else current_.between=tick-current_.submitReturn[0];return current_.submitToken;
  }
  void submitOwnerBegin(uint64_t token,uint64_t tick) noexcept { std::lock_guard<std::mutex> l(m_); if(token&&current_.submitToken==token&&current_.submitOpen&&!current_.ownerBegin)current_.ownerBegin=tick;else ++missing_[Reentrant]; }
  void submitOwnerEnd(uint64_t token,uint64_t tick) noexcept { std::lock_guard<std::mutex> l(m_); if(token&&current_.submitToken==token&&current_.ownerBegin&&tick>=current_.ownerBegin)current_.ownerEnd=tick;else ++missing_[BadClock]; }
  void submitCallerEnd(uint64_t token,unsigned eye,uint64_t sequence,uint64_t tick,uint32_t thread,double renderParkMs,bool ok) noexcept {
    std::lock_guard<std::mutex> l(m_);
    if(current_.submitOpen&&thread!=current_.submitThread){bad(WrongThread);return;}
    if(!ok||!token||current_.submitToken!=token||!current_.submitOpen||eye!=current_.order[current_.eyes]||sequence!=current_.sequence||
       !ordered(current_.submitBegin,current_.ownerBegin,current_.ownerEnd,tick)||
       !std::isfinite(renderParkMs)||renderParkMs<0){bad(!ok?PartialStereo:BadSequence);return;}
    if(current_.callerThread!=thread||current_.submitThread!=thread){bad(WrongThread);return;}
    const unsigned n=current_.eyes;current_.submitRound[n]=tick-current_.submitBegin;
    current_.owner[n]=current_.ownerEnd-current_.ownerBegin;current_.park[n]=renderParkMs;
    current_.submitReturn[n]=tick;current_.seenEyes|=1u<<eye;++current_.eyes;
    current_.submitOpen=false;current_.submitToken=0;current_.ownerBegin=current_.ownerEnd=0;
  }
  void noteDirectOwner() noexcept {std::lock_guard<std::mutex>l(m_);++missing_[DirectOwner];failCurrent();}
  bool takeReport(Report& out) noexcept { std::lock_guard<std::mutex> l(m_); if(!ready_)return false;out=report_;ready_=false;return true; }
  const std::array<uint64_t,MissingCount>& missingForTest() const noexcept{return missing_;}

 private:
  struct Wait {bool open=false;uint64_t token=0,begin=0,ownerBegin=0,ownerEnd=0;uint32_t thread=0;};
  struct Current {bool active=false,submitOpen=false;uint64_t sequence=0,waitReturn=0,waitRound=0,waitOwner=0,
    beforeFirst=0,between=0,afterSecond=0,submitToken=0,submitBegin=0,ownerBegin=0,ownerEnd=0,submitReturn[2]{},submitRound[2]{},owner[2]{},atMs=0;bool afterSecondReady=false;
    double park[2]{};uint32_t callerThread=0,waitThread=0,submitThread=0,order[2]{},seenEyes=0,eyes=0;Shape shape{};};
  static bool ordered(uint64_t a,uint64_t b,uint64_t c,uint64_t d){return a&&a<=b&&b<=c&&c<=d;}
  static bool sameShape(const Shape&a,const Shape&b){return a.pacing==b.pacing&&a.shouldRender==b.shouldRender&&a.sceneReady==b.sceneReady&&a.generation==b.generation&&a.featureEpoch==b.featureEpoch&&a.width[0]==b.width[0]&&a.width[1]==b.width[1]&&a.height[0]==b.height[0]&&a.height[1]==b.height[1]&&a.outputWidth[0]==b.outputWidth[0]&&a.outputWidth[1]==b.outputWidth[1]&&a.outputHeight[0]==b.outputHeight[0]&&a.outputHeight[1]==b.outputHeight[1];}
  void observeThread(uint32_t& first,uint32_t value){if(!first)first=value;else if(first!=value)observedThreadMismatch_=true;}
  void bad(Missing m){++missing_[m];failCurrent();}
  void fail(Missing m){++missing_[m];failCurrent();}
  void failCurrent(){current_={};}
  void finishCurrent(uint64_t nextWaitReturn,uint64_t nowMs,uint32_t waitThread){
    if(current_.eyes!=2){bad(PartialStereo);return;}
    if(nextWaitReturn<current_.waitReturn||!current_.afterSecondReady){bad(BadClock);return;}
    constexpr double toMs=0.001; Sample s{};s.sequence=current_.sequence;s.cycle=double(nextWaitReturn-current_.waitReturn)*toMs;
    s.beforeFirst=double(current_.beforeFirst)*toMs;s.firstSubmit=double(current_.submitRound[0])*toMs;s.betweenEyes=double(current_.between)*toMs;
    s.secondSubmit=double(current_.submitRound[1])*toMs;s.afterSecond=double(current_.afterSecond)*toMs;s.nextWait=double(nextWaitReturn-wait_.begin)*toMs;
    const double parts=s.beforeFirst+s.firstSubmit+s.betweenEyes+s.secondSubmit+s.afterSecond+s.nextWait;
    s.residual=s.cycle-parts;s.waitOwner=double(wait_.ownerEnd-wait_.ownerBegin)*toMs;
    for(unsigned i=0;i<2;++i){s.submitOwner[i]=double(current_.owner[i])*toMs;s.renderPark[i]=current_.park[i];s.eyeOrder[i]=current_.order[i];}
    s.callerThread=current_.callerThread;s.waitThread=waitThread;
    if(std::fabs(s.residual)>0.01||!current_.sequence){bad(BadClock);return;}
    admit(s,current_.shape,nowMs);
  }
  void admit(const Sample&s,const Shape& shape,uint64_t nowMs){
    if(!count_){windowStart_=s.sequence;shape_=shape;firstThread_=s.callerThread;waitThread_=s.waitThread;}
    if(count_>=capacity){++missing_[Overflow];makeReport(s.sequence,nowMs);return;} (*samples_)[count_++]=s;everComplete_=true;
  }
  void advanceWindow(uint64_t nowMs,const Shape& shape,uint64_t sequence){
    if(!windowStartMs_){windowStartMs_=nowMs;shape_=shape;windowStart_=sequence;}
    else if(nowMs<windowStartMs_){++missing_[BadClock];makeReport(lastAttemptedSequence_,nowMs);windowStartMs_=nowMs;shape_=shape;windowStart_=sequence;}
    else if(nowMs-windowStartMs_>=30000){makeReport(lastAttemptedSequence_,nowMs);windowStartMs_=nowMs;shape_=shape;windowStart_=sequence;}
    ++attempted_;lastAttemptedSequence_=sequence;
  }
  Dist dist(double Sample::*field)const{std::array<double,capacity>v{};double total=0;for(unsigned i=0;i<count_;++i){v[i]=(*samples_)[i].*field;total+=v[i];}std::sort(v.begin(),v.begin()+count_);auto p=[&](unsigned x){return count_?v[(count_*x+99)/100-1]:0;};return{count_?total/count_:0,p(50),p(95)};}
  void makeReport(uint64_t last,uint64_t nowMs){report_={};report_.window=++window_;report_.firstSequence=count_?(*samples_)[0].sequence:windowStart_;report_.lastSequence=last;report_.elapsedMs=nowMs>=windowStartMs_?nowMs-windowStartMs_:0;report_.admitted=attempted_;report_.valid=count_;report_.shape=shape_;report_.missing=missing_;report_.callerThread=observedSubmitThread_;report_.waitThread=observedWaitThread_;report_.threadConsistent=!observedThreadMismatch_;
    report_.cycle=dist(&Sample::cycle);report_.beforeFirst=dist(&Sample::beforeFirst);report_.firstSubmit=dist(&Sample::firstSubmit);report_.betweenEyes=dist(&Sample::betweenEyes);report_.secondSubmit=dist(&Sample::secondSubmit);report_.afterSecond=dist(&Sample::afterSecond);report_.nextWait=dist(&Sample::nextWait);report_.residual=dist(&Sample::residual);report_.waitOwner=dist(&Sample::waitOwner);for(unsigned i=0;i<2;++i){std::array<double,capacity>v{};for(unsigned j=0;j<count_;++j)v[j]=(*samples_)[j].submitOwner[i];report_.submitOwner[i]=manual(v);for(unsigned j=0;j<count_;++j)v[j]=(*samples_)[j].renderPark[i];report_.renderPark[i]=manual(v);}for(unsigned j=0;j<count_;++j)if((*samples_)[j].callerThread!=observedSubmitThread_||(*samples_)[j].waitThread!=observedWaitThread_)report_.threadConsistent=false;ready_=true;count_=0;attempted_=0;missing_={};windowStart_=windowStartMs_=lastAttemptedSequence_=0;observedWaitThread_=observedSubmitThread_=0;observedThreadMismatch_=false;}
  Dist manual(std::array<double,capacity>&v)const{double t=0;for(unsigned i=0;i<count_;++i)t+=v[i];std::sort(v.begin(),v.begin()+count_);auto p=[&](unsigned x){return count_?v[(count_*x+99)/100-1]:0;};return{count_?t/count_:0,p(50),p(95)};}
  mutable std::mutex m_;bool enabled_=false,everComplete_=false,ready_=false;Wait wait_{};Current current_{};
  std::unique_ptr<std::array<Sample,capacity>> samples_;std::array<uint64_t,MissingCount> missing_{};unsigned count_=0;uint64_t attempted_=0,window_=0,windowStart_=0,windowStartMs_=0,lastAttemptedSequence_=0,nextToken_=0;Shape shape_{};uint32_t firstThread_=0,waitThread_=0,observedWaitThread_=0,observedSubmitThread_=0;bool observedThreadMismatch_=false;Report report_{};
};
} // namespace edvr::openxr
