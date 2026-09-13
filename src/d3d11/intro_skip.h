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
// The executable's own imports AND quartz.dll's CreateFileW import are
// watched. Flight 09:43 drew 902 movie frames without a single executable
// request. The DirectShow AsyncReader named in Elite's executable opens
// through quartz instead; a real IFileSourceFilter::Load reproduces the
// miss and verifies the reader hook. Only import pointers change, never
// process-wide function code. An ident gets ERROR_FILE_NOT_FOUND without
// changing the movie on disk; menu loops and other videos pass through.
// The system reader is loaded before graph setup and retained until its
// import has been restored. The log distinguishes these outcomes:
//
//   `intro skip: WORKED` -- refusals counted, the movie's fill never drew,
//   the scene arrived; the seconds-since-arming on that line against the
//   ~28 s the movie costs is the size of the win.
//
//   `intro skip: the movie is DRAWING anyway` -- no observed refusal, or
//   playback continued despite refusal. Do not infer success from arming.
//
//   `intro skip: no ident was asked for` -- neither refused nor drawn; the
//   count of OTHER Movies\ opens on the line says whether these hooks see
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

// Put the import slots back where they still hold ours, then release the
// explicit system-reader module reference.
void introSkipShutdown();

}  // namespace edvr
