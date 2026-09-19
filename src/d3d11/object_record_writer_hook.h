#pragma once
#include <cstdint>

namespace edvr {
class ObjectRecordWriterProbe;
const char* attachObjectRecordWriterHook(ObjectRecordWriterProbe* probe) noexcept;
void detachObjectRecordWriterHook(ObjectRecordWriterProbe* probe) noexcept;
bool objectRecordWriterHookMatches(uintptr_t target) noexcept;

#ifdef EDVR_RECORD_WRITER_TEST
// Real executable relay/original-forward checks, without a game executable.
unsigned objectRecordWriterHookSelfTest() noexcept;
#endif
}
