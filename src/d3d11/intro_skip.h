// The launch movie, not played: fix.intro_video = skip.
//
// WHAT THE MOVIE IS (docs/intro-video.md). Elite's launch idents are WebM
// files under Products\...\Movies\ -- Ident_Frontier_*.webm, about twenty
// seconds each, plus intro_temp.webm -- decoded through the DirectShow
// filters shipped beside the executable and uploaded to the GPU as YUV
// planes. The game builds that graph by hand: the executable names
// webmsplit64 and vp8decoder64 itself and carries DllGetClassObject, and the
// splitter beside it is a splitter, not a file source. So somebody has to
// OPEN the file, and the one thing every route to that has in common is a
// path with `Movies\` in it.
//
// THE MECHANISM, chosen for being the same class of hook the settings menu
// already uses on the same table (iat_hook.h): the executable's own imports
// of CreateFileW / CreateFileA and the GetFileAttributes family are watched,
// and an open of an ident is answered ERROR_FILE_NOT_FOUND. That is
// precisely the answer the game gets when a player renames the file --
// the long-standing way to skip these idents -- so the game's own missing-
// movie path is what runs, and nothing on disk is touched, so nothing has
// to be put back after a crash, a game update or an uninstall.
//
// WHAT IS NOT KNOWN YET, stated so the first flight is read for it and not
// against it. Which module makes the open is unmeasured: the executable
// through its import table (this hook sees it), its C runtime (this hook
// does not), or a DirectShow file source of Windows' own (nor that). And
// whether the game's missing-file path is a clean skip is the field's
// word, not this project's measurement. The log therefore says which of
// three things happened:
//
//   `intro skip: WORKED` -- refusals counted, the movie's fill never drew,
//   the scene arrived; the seconds-since-arming on that line against the
//   ~28 s the movie costs is the size of the win.
//
//   `intro skip: the movie is DRAWING anyway` -- with zero refusals the open
//   went by a route this table does not carry, and a process-wide hook
//   (code_hook.h on KernelBase!CreateFileW, whose prologue was read as
//   relocatable on 2026-09-13) is the next step; with refusals, the game
//   got the file another way after being told no.
//
//   `intro skip: no ident was asked for` -- neither refused nor drawn; the
//   count of OTHER Movies\ opens on the line says whether this table sees
//   the game's movie opens at all.
//
// A build in which none of those lines appears with skip set is a build in
// which introSkipTick was never called, which is its own finding.
#pragma once

namespace edvr {

class Config;

// Reads fix.intro_video. `skip` arms the refusal and installs the import
// hooks the first time it is seen; any other value disarms (the hooks stay,
// forwarding everything). Install and reload; the movie is decided at
// launch, so a change mid-session matters at the next one.
void introSkipConfigure(Config& cfg);

// The movie's YUV-to-RGB fill drew this frame -- introPanelNoteFill's
// moment. With the skip armed this is the fact that says it did not take.
void introSkipNoteMovieDrew();

// Frame edge with the caller's scene boundary. The first rendered scene
// closes the verdict, said once.
void introSkipTick(bool sceneFrame);

// Put the import slots back where they still hold ours.
void introSkipShutdown();

}  // namespace edvr
