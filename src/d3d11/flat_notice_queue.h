#pragma once
// The flat profile's one-slot notice queue (menuShowNotice, menu.cpp). The flat screen has no toasts: a notice is the F8 panel's raster at a
// toast's size, so it cannot be shown while the menu itself is open. A notice raised in that time (F7 AA comparison while F8 is up) stays
// queued and starts when the menu closes. The old menuTick cleared the due flag every tick but started the notice only when the menu was
// closed, so a notice raised with the menu open was lost (review of main 9fe5b257, finding 4).
//
// One slot: a newer notice replaces an older one still waiting, as the VR toast queue does for the "AA " notices (a quick second F7 press is
// the player's answer to the first). Pure logic, no clock and no device; tools\aa_compare_test covers it.
#include <string>

namespace edvr {

class FlatNoticeQueue {
 public:
  void show(const char* text) {
    text_ = text ? text : "";
    due_ = !text_.empty();
  }
  // One menuTick. Returns the notice to start now, or nullptr: none queued, or the menu is open (the notice is kept for when it closes).
  // The pointer is valid until the next show().
  const std::string* take(bool menuOpen) {
    if (!due_ || menuOpen) return nullptr;
    due_ = false;
    return &text_;
  }
  bool due() const { return due_; }

 private:
  std::string text_;
  bool due_ = false;
};

}  // namespace edvr
