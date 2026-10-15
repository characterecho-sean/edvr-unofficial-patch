// The AA comparison key's module (hotkey.compare_aa; the state machine and the words are src\common\aa_compare.h). Binds the key,
// checks it against the player's Elite bindings, turns EDVR's temporal AA off and on as a runtime override of fix.temporal_aa
// (Config::setRuntimeOverride: nothing is written to any ini) and says so in a short notice.
//
// What to read in a flight log, in order:
//   `hotkey: AA comparison key bound: F7 (vk 0x76, mods 0x0).`     the key is live (empty value: `... cleared`)
//   `hotkey: AA comparison key F7 is free in your Elite bindings`  or `... CLASHES with your Elite bindings in <file>: ...`
//   `aa compare: AA OFF for comparison ...`                        a press went off (and the configure sweep's own lines follow)
//   `aa compare: AA RESTORED ...`                                  the next press
//   `aa compare: ended ...`                                        the player changed AA, or a profile switch, under it
#pragma once

namespace edvr {

// Every owned Present, both profiles, after the diagnostic hotkeys' tick. Cheap while nothing is pressed: one key poll, and a
// config-generation compare.
void aaCompareTick();

// The F8 menu queued a write of fix.temporal_aa. Ends a comparison that is showing AA off, once the write has landed (aa_compare.h).
void aaCompareNoteMenuEdit();

// The binding's current text, or empty while unbound (for tests and the Status page).
const char* aaCompareKeyText();

}  // namespace edvr
