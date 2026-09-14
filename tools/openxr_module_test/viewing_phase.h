#pragma once

#include <cstdint>

namespace edvr::openxr {

// Diagnostic viewing time, driven by the runtime's CanRenderScene signal.
// Application focus is not independent proof of physical headset visibility.
// Credit only intervals whose two endpoint samples were focused. Losing focus
// before Scene restarts the warmup/grid; in Scene it pauses the viewing budget.
class ViewingPhase final {
 public:
  enum class State { Warmup, Grid, Scene, Complete, Expired };
  static constexpr uint64_t kWarmupMs=500, kStartupMs=30000;
  ViewingPhase(uint64_t now,uint32_t seconds)
      :started_(now),last_(now),totalMs_(uint64_t(seconds)*1000),
       gridMs_(seconds>=12?3000:uint64_t(seconds)*250) {
    if(!seconds||seconds>60)state_=State::Expired;
  }
  State tick(uint64_t now,bool focused) {
    if(state_==State::Complete||state_==State::Expired)return state_;
    if(now<last_||now-started_>=totalMs_+kStartupMs||
       (state_!=State::Scene&&now-started_>=kStartupMs))return state_=State::Expired;
    const uint64_t delta=focused&&previousFocused_?now-last_:0;
    last_=now;previousFocused_=focused;
    if(!focused) {
      if(state_!=State::Scene){state_=State::Warmup;elapsed_=0;}
      return state_;
    }
    elapsed_+=delta;
    if(state_==State::Warmup&&elapsed_>=kWarmupMs){state_=State::Grid;elapsed_=0;}
    else if(state_==State::Grid&&elapsed_>=gridMs_){state_=State::Scene;elapsed_=0;}
    else if(state_==State::Scene&&elapsed_>=totalMs_-gridMs_)state_=State::Complete;
    return state_;
  }
  State state()const{return state_;}
  uint64_t phaseElapsedMs()const{return elapsed_;}
  uint64_t gridBudgetMs()const{return gridMs_;}
  uint64_t sceneBudgetMs()const{return totalMs_-gridMs_;}
 private:
  uint64_t started_,last_,totalMs_,gridMs_,elapsed_=0;
  bool previousFocused_=false;
  State state_=State::Warmup;
};

}  // namespace edvr::openxr
