#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include "../common/freeze_book.h"
#include "frame_cycle_stats.h"

namespace edvr::openxr {

// Names for FrameCycleStats::PostUnavailable, in the enum's order: the words
// native_post_submit_unavailable prints per window, and what a long cycle's
// present_split says when its Present split is missing.
inline const char* postUnavailableName(unsigned reason) noexcept {
  static const char* const names[]={"available","provider_missing","provider_version","provider_size","provider_generation",
    "not_yet_observable","lost_or_inflight_present_history","partial_present","other_present_thread",
    "failed_present","test_present","malformed_or_overlapping_present"};
  static_assert(sizeof(names)/sizeof(*names)==FrameCycleStats::PostUnavailableCount,"post-submit rejection names");
  return reason<FrameCycleStats::PostUnavailableCount?names[reason]:"unknown";
}

// Why a long cycle has no Present split: the trace's own rejection, or a valid
// trace that held no Present or more than one (the split is defined for one).
inline const char* presentSplitReason(const FrameCycleStats::Completed& c) noexcept {
  if(c.presentSplit)return "ok";
  if(!c.postValid)return postUnavailableName(c.postUnavailable);
  return c.presentCount==0?"no_present":"multiple_present";
}

inline void longCycleAppend(char* buf,size_t cap,size_t& len,const char* format,...) noexcept {
  if(len+1>=cap)return;
  va_list arguments;va_start(arguments,format);
  const int n=vsnprintf(buf+len,cap-len,format,arguments);
  va_end(arguments);
  if(n<0)return;
  len=static_cast<size_t>(n)<cap-len?len+static_cast<size_t>(n):cap-1;
}

// One line per completed cycle longer than twice the runtime's predicted period
// (native_runtime_host.h noteLongCycle), without the trailing newline. Every
// field the line has always carried keeps its name and its meaning; the Present
// split is new, and sits beside post_second_submit_to_next_wait, which it cuts:
//
//   pre_present     the second Submit's return to the Present hook's entry
//   present_hook    the hook's entry to its exit: EDVR's work plus the real Present
//   post_present    the hook's exit to the next WaitGetPoses's entry
//
// so pre_present+present_hook+post_present == post_second_submit_to_next_wait.
// The hook is cut once more where the graphics half marks it: hook_before_real
// (EDVR, before the real call), hook_real_present (the driver's own Present),
// hook_after_real (EDVR again: the frame boundary's ticks and the rest),
// hook_render_callback (the call into this runtime). A cycle without a valid
// single-Present trace prints present_split=<reason> and none of the numbers.
// The fields of one long cycle, "sequence=..." to "units=wall_ms", appended at `len` after whatever head
// the caller wrote: native_long_cycle's own, or native_long_cycle_worst's (formatWorstCycleLine).
inline void formatLongCycleFields(char* buf,size_t cap,size_t& len,unsigned long long sequence,double periodMs,
                                  const FrameCycleStats::Completed& c) noexcept {
  longCycleAppend(buf,cap,len,"sequence=%llu,cycle_ms=%.4f,period_ms=%.4f,"
    "game_before_first_submit=%.4f,first_submit_roundtrip=%.4f,first_submit_owner_body=%.4f,"
    "between_eye_calls=%.4f,second_submit_roundtrip=%.4f,second_submit_owner_body=%.4f,"
    "first_submit_render_park=%.4f,second_submit_render_park=%.4f,"
    "post_second_submit_to_next_wait=%.4f,present_split=%s",
    sequence,c.cycleMs,periodMs,
    c.beforeFirstMs,c.firstSubmitMs,c.submitOwnerMs[0],
    c.betweenEyesMs,c.secondSubmitMs,c.submitOwnerMs[1],
    c.renderParkMs[0],c.renderParkMs[1],
    c.afterSecondMs,presentSplitReason(c));
  if(c.presentSplit)
    longCycleAppend(buf,cap,len,",pre_present=%.4f,present_hook=%.4f,hook_before_real=%.4f,"
      "hook_real_present=%.4f,hook_after_real=%.4f,hook_render_callback=%.4f,post_present=%.4f",
      c.prePresentMs,c.presentHookMs,c.hookBeforeRealMs,c.hookRealMs,c.hookAfterRealMs,
      c.hookCallbackMs,c.postPresentMs);
  longCycleAppend(buf,cap,len,",next_wait_roundtrip=%.4f,next_wait_owner_body=%.4f,units=wall_ms",
    c.nextWaitMs,c.waitOwnerMs);
}

inline size_t formatLongCycleLine(char* buf,size_t cap,unsigned long long sequence,double periodMs,
                                  const FrameCycleStats::Completed& c) noexcept {
  size_t len=0;
  if(!buf||!cap)return 0;
  buf[0]=0;
  longCycleAppend(buf,cap,len,"native_long_cycle,");
  formatLongCycleFields(buf,cap,len,sequence,periodMs,c);
  return len;
}

// The same cycle again, for the end of the session: one line per rank of the worst few cycles of the whole
// session, written when the runtime closes (and while it runs, whenever the list changed, every few
// minutes). `utc` is when the cycle ended, as YYYY-MM-DDTHH:MM:SS.mmmZ -- the runtime log's own clock,
// whose line prefix is UTC. `fields` is formatLongCycleFields's text, formatted when the cycle was logged
// and kept with its entry, so the line is the cycle's as it was: one reader takes a native_long_cycle line
// and a native_long_cycle_worst line apart the same way.
inline size_t formatWorstCycleLine(char* buf,size_t cap,unsigned rank,unsigned of,const char* utc,
                                   const char* fields) noexcept {
  size_t len=0;
  if(!buf||!cap)return 0;
  buf[0]=0;
  longCycleAppend(buf,cap,len,"native_long_cycle_worst,rank=%u,of=%u,utc=%s,%s",rank,of,utc&&*utc?utc:"unknown",
    fields?fields:"");
  return len;
}

// The counts of the session's long cycles: the periodic line and the part of the session summary that
// follows the three fields that line has always carried. `book` is the runtime's FreezeBook; its key=value
// text is comma-joined here, the runtime log's own style.
inline size_t formatLongCycleCountsLine(char* buf,size_t cap,const char* head,unsigned long long count,
                                        unsigned long long logged,const char* reason,const FreezeBook& book) noexcept {
  size_t len=0;
  if(!buf||!cap)return 0;
  buf[0]=0;
  char kv[640];
  book.formatCounts(kv,sizeof(kv),',',false);
  if(reason&&*reason)
    longCycleAppend(buf,cap,len,"%s,reason=%s,count=%llu,logged=%llu,threshold=2x_period,%s",head,reason,count,logged,kv);
  else
    longCycleAppend(buf,cap,len,"%s,count=%llu,logged=%llu,threshold=2x_period,%s",head,count,logged,kv);
  return len;
}

} // namespace edvr::openxr
