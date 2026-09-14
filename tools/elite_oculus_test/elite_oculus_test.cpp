#include "../../src/common/elite_oculus_profile.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;
unsigned checks = 0;
unsigned failures = 0;

void check(bool value, const char* what) {
  ++checks;
  if (!value) {
    ++failures;
    std::printf("FAIL: %s\n", what);
  }
}

std::uint16_t u16(const Bytes& d, std::size_t at) {
  if (at + 2 > d.size()) return 0;
  return static_cast<std::uint16_t>(d[at]) |
         static_cast<std::uint16_t>(d[at + 1]) << 8;
}

std::uint32_t u32(const Bytes& d, std::size_t at) {
  if (at + 4 > d.size()) return 0;
  return static_cast<std::uint32_t>(d[at]) |
         static_cast<std::uint32_t>(d[at + 1]) << 8 |
         static_cast<std::uint32_t>(d[at + 2]) << 16 |
         static_cast<std::uint32_t>(d[at + 3]) << 24;
}

std::uint64_t u64(const Bytes& d, std::size_t at) {
  return static_cast<std::uint64_t>(u32(d, at)) |
         static_cast<std::uint64_t>(u32(d, at + 4)) << 32;
}

void put32(Bytes& d, std::size_t at, std::uint32_t value) {
  if (at + 4 > d.size()) return;
  for (unsigned i = 0; i != 4; ++i) d[at + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

void put16(Bytes& d, std::size_t at, std::uint16_t value) {
  if (at + 2 > d.size()) return;
  d[at] = static_cast<std::uint8_t>(value);
  d[at + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put64(Bytes& d, std::size_t at, std::uint64_t value) {
  if (at + 8 > d.size()) return;
  for (unsigned i = 0; i != 8; ++i) d[at + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

bool parseHeaders(const Bytes& raw, std::uint32_t* imageSize,
                  std::uint32_t* headersSize, std::uint32_t* nt,
                  std::uint16_t* sections, std::uint16_t* optionalSize) {
  if (raw.size() < 64 || raw[0] != 'M' || raw[1] != 'Z') return false;
  const std::uint32_t n = u32(raw, 0x3C);
  if (n > raw.size() || n + 24 > raw.size() || u32(raw, n) != 0x4550) return false;
  const std::uint16_t count = u16(raw, n + 6);
  const std::uint16_t opt = u16(raw, n + 20);
  if (!count || count > 96 || opt < 112 || n + 24ull + opt > raw.size()) return false;
  const std::size_t o = n + 24;
  if (u16(raw, o) != 0x20B) return false;
  *imageSize = u32(raw, o + 56);
  *headersSize = u32(raw, o + 60);
  *nt = n;
  *sections = count;
  *optionalSize = opt;
  return *imageSize != 0;
}

bool applyRelocations(Bytes& mapped, const Bytes& raw, std::uint32_t nt,
                      std::uint64_t mappedBase) {
  const std::size_t optional = nt + 24;
  const std::uint64_t preferred = u64(raw, optional + 24);
  const std::uint32_t relocRva = u32(raw, optional + 112 + 5 * 8);
  const std::uint32_t relocSize = u32(raw, optional + 112 + 5 * 8 + 4);
  if (!relocRva || !relocSize) return true;
  const std::uint64_t delta = mappedBase - preferred;
  std::uint32_t cursor = 0;
  while (cursor + 8 <= relocSize) {
    const std::uint32_t block = relocRva + cursor;
    if (block + 8 > mapped.size()) return false;
    const std::uint32_t page = u32(mapped, block);
    const std::uint32_t blockSize = u32(mapped, block + 4);
    if (blockSize < 8 || blockSize > relocSize - cursor || ((blockSize - 8) & 1)) return false;
    const std::uint32_t count = (blockSize - 8) / 2;
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::uint16_t entry = u16(mapped, block + 8 + i * 2);
      const std::uint16_t type = static_cast<std::uint16_t>(entry >> 12);
      const std::uint32_t offset = page + (entry & 0x0FFF);
      if (type == 0) continue;
      if (type != 10 || static_cast<std::uint64_t>(offset) + 8 > mapped.size()) return false;
      put64(mapped, offset, u64(mapped, offset) + delta);
    }
    cursor += blockSize;
  }
  return cursor == relocSize || cursor + 8 > relocSize;
}

bool mapImage(const Bytes& raw, Bytes* mapped) {
  std::uint32_t imageSize = 0, headersSize = 0, nt = 0;
  std::uint16_t sections = 0, optSize = 0;
  if (!parseHeaders(raw, &imageSize, &headersSize, &nt, &sections, &optSize) ||
      imageSize > 0x40000000u || headersSize > imageSize || headersSize > raw.size()) return false;
  mapped->assign(imageSize, 0);
  std::copy(raw.begin(), raw.begin() + headersSize, mapped->begin());
  const std::size_t table = nt + 24ull + optSize;
  if (table + static_cast<std::size_t>(sections) * 40 > raw.size()) return false;
  for (std::uint16_t i = 0; i < sections; ++i) {
    const std::size_t at = table + i * 40ull;
    const std::uint32_t virtualSize = u32(raw, at + 8);
    const std::uint32_t rva = u32(raw, at + 12);
    const std::uint32_t rawSize = u32(raw, at + 16);
    const std::uint32_t rawAt = u32(raw, at + 20);
    const std::uint32_t span = virtualSize > rawSize ? virtualSize : rawSize;
    if (!span || static_cast<std::uint64_t>(rva) + span > imageSize ||
        static_cast<std::uint64_t>(rawAt) + rawSize > raw.size()) return false;
    if (rawSize) std::copy(raw.begin() + rawAt, raw.begin() + rawAt + rawSize,
                           mapped->begin() + rva);
  }
  // A vector is deliberately used instead of SEC_IMAGE.  It keeps the test
  // read-only with respect to the game and avoids resolving or executing any
  // import.  Apply DIR64 relocations so ASLR-adjusted vtable pointers match.
  return applyRelocations(*mapped, raw, nt,
                          reinterpret_cast<std::uintptr_t>(mapped->data()));
}

Bytes readAll(const std::wstring& path) {
  std::ifstream file(path, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

Bytes identityFixture(bool withImport) {
  Bytes d(0x6409000, 0);
  d[0] = 'M'; d[1] = 'Z'; put32(d, 0x3C, 0x80);
  d[0x80] = 'P'; d[0x81] = 'E';
  put16(d, 0x84, 0x8664); put16(d, 0x86, 7); put16(d, 0x94, 240);
  put16(d, 0x98, 0x20B); put64(d, 0x98 + 24, 0x140000000ull);
  put32(d, 0x98 + 56, 0x6409000); put32(d, 0x98 + 60, 0x400);
  put32(d, 0x98 + 108, 16); put32(d, 0x98 + 112 + 8, 0x4DB6000);
  put32(d, 0x98 + 112 + 12, 40);
  const std::size_t table = 0x98 + 240;
  const std::uint32_t starts[] = {0x1000, 0x4DB3000, 0x4DB5000, 0x4DB6000,
                                  0x4DB8000, 0x4DBA000, 0x4DBE000};
  const std::uint32_t spans[] = {0x4DB2000, 0x1000, 0x1000, 0x2000,
                                 0x1000, 0x1000, 0x1000};
  for (unsigned i = 0; i != 7; ++i) {
    put32(d, table + i * 40 + 8, spans[i]);
    put32(d, table + i * 40 + 12, starts[i]);
    put32(d, table + i * 40 + 36, i == 0 ? 0x60000020 : 0xC0000040);
  }
  put32(d, 0x80 + 8, 0x6A989634);
  if (withImport) {
    const std::uint32_t base = 0x4DB6000;
    put32(d, base, base + 0x100); put32(d, base + 12, base + 0x200);
    put32(d, base + 16, 0x4DB32E0); put32(d, base + 4, 0);
    put64(d, base + 0x100, base + 0x400);
    const char dll[] = "KERNEL32.dll";
    for (unsigned i = 0; i < sizeof(dll); ++i) d[base + 0x200 + i] = dll[i];
    const char fn[] = "LoadLibraryW";
    for (unsigned i = 0; i < sizeof(fn); ++i) d[base + 0x402 + i] = fn[i];
    put64(d, 0x4DB32E0, 1);
  }
  return d;
}

void selfTest() {
  edvr::OculusProfileMatch out{reinterpret_cast<void**>(1), 1};
  check(!edvr::eliteOculusProfileValidateMapped(nullptr, 0, &out), "null image rejected");
  check(!out.loadLibrarySlot && !out.callerReturnRva, "null image clears output");
  Bytes malformed(64, 0);
  malformed[0] = 'M'; malformed[1] = 'Z';
  out = {reinterpret_cast<void**>(1), 1};
  check(!edvr::eliteOculusProfileValidateMapped(malformed.data(), malformed.size(), &out),
        "truncated DOS image rejected");
  check(!out.loadLibrarySlot && !out.callerReturnRva, "malformed image clears output");

  // A compact identity-shaped image exercises the rejection gates without
  // embedding proprietary executable blocks in the tracked test fixture.
  Bytes fixture(0x5000, 0);
  fixture[0] = 'M'; fixture[1] = 'Z'; put32(fixture, 0x3C, 0x80);
  fixture[0x80] = 'P'; fixture[0x81] = 'E';
  put32(fixture, 0x84, 0x8664); // deliberately malformed machine/header shape
  out = {reinterpret_cast<void**>(1), 1};
  check(!edvr::eliteOculusProfileValidateMapped(fixture.data(), fixture.size(), &out),
        "bad machine/header rejected");
  check(!out.loadLibrarySlot && !out.callerReturnRva, "bad header clears output");

  // This short allocation cannot satisfy the qualified SizeOfImage.
  put32(fixture, 0x84 + 4, 0x8664);
  put32(fixture, 0x86, 7);
  put32(fixture, 0x94, 240);
  put32(fixture, 0x98, 0x20B);
  put32(fixture, 0x98 + 56, 0x6409000);
  put32(fixture, 0x98 + 60, 0x400);
  check(!edvr::eliteOculusProfileValidateMapped(fixture.data(), fixture.size(), &out),
        "short mapped span rejected");
  put32(fixture, 0x3C, 0xFFFFFFF8u);
  check(!edvr::eliteOculusProfileValidateMapped(fixture.data(), fixture.size(), &out),
        "out of range NT header rejected");

  Bytes importFixture = identityFixture(true);
  namespace detail = edvr::elite_oculus_profile_detail;
  detail::View view{};
  check(detail::image_view(importFixture.data(), importFixture.size(), &view),
        "synthetic header and seven nonoverlapping sections pass structural validation");
  check(detail::mapped_rva(view, 0x4DB32E0, 8) &&
        !detail::mapped_rva(view, 0x4DB4FFC, 8), "mapped range cannot cross a section gap");
  importFixture[0x1000] = 'a'; importFixture[0x1001] = 'b'; importFixture[0x1002] = 'c';
  check(detail::fingerprint(view, 0x1000, 3, 0xE71FA2190541574Bull), "FNV known vector matches");
  importFixture[0x1002] = 'd';
  check(!detail::fingerprint(view, 0x1000, 3, 0xE71FA2190541574Bull), "changed code fingerprint rejected");
  const auto address = reinterpret_cast<std::uintptr_t>(importFixture.data());
  put64(importFixture, 0x4DB32E0, address + 0x1000);
  check(detail::relocated_pointer(view, 0x4DB32E0, 0x1000), "relocated pointer uses actual mapped base");
  put64(importFixture, 0x4DB32E0, address + 0x1001);
  check(!detail::relocated_pointer(view, 0x4DB32E0, 0x1000), "changed relocated pointer rejected");
  check(!edvr::eliteOculusProfileValidateMapped(importFixture.data(), importFixture.size(), &out),
        "synthetic executable with valid imports cannot satisfy audited code fingerprints");

  // The current test executable is not Elite; the guarded convenience API
  // must refuse it and clear a previously populated result.
  out = {reinterpret_cast<void**>(1), 1};
  check(!edvr::eliteOculusProfileValidate(GetModuleHandleW(nullptr), &out),
        "non-Elite convenience image rejected");
  check(!out.loadLibrarySlot && !out.callerReturnRva, "convenience rejection clears output");
}

int gameTest(const wchar_t* path) {
  const Bytes raw = readAll(path);
  if (raw.empty()) {
    std::printf("elite_oculus_test: cannot read game executable\n");
    return 1;
  }
  Bytes mapped;
  if (!mapImage(raw, &mapped)) {
    std::printf("elite_oculus_test: manual mapping failed\n");
    return 1;
  }
  edvr::OculusProfileMatch out{};
  check(edvr::eliteOculusProfileValidateMapped(mapped.data(), mapped.size(), &out),
        "mapped known Elite profile accepted");
  check(out.loadLibrarySlot != nullptr && out.callerReturnRva == 0x4E70BC,
        "mapped profile returns slot and exact caller RVA");
  if (failures) return 1;

  const std::size_t slotOffset = reinterpret_cast<std::uint8_t*>(out.loadLibrarySlot) - mapped.data();
  auto rejectsMutation = [&](std::size_t offset, const char* label) {
    // Preserve the allocation so the unchanged vtables remain correctly relocated.
    mapped[offset] ^= 1;
    check(!edvr::eliteOculusProfileValidateMapped(mapped.data(), mapped.size(), &out), label);
    check(!out.loadLibrarySlot && !out.callerReturnRva, "mutation rejection clears output");
    mapped[offset] ^= 1;
  };
  for (const auto rva : {0x8D533Cu, 0x4E4AA0u, 0x4E4630u, 0x4E4870u,
                         0x4E8530u, 0x4E70D0u, 0x4E6D40u, 0x4E0E60u,
                         0x4E8230u, 0x4E5CA0u, 0x4E86B0u})
    rejectsMutation(rva, "audited code-block mutation rejected");
  for (const auto rva : {0x4E243F8u, 0x4E24400u, 0x4E245C0u})
    rejectsMutation(rva, "vtable mutation rejected");
  const auto nt = u32(mapped, 0x3C);
  for (const auto offset : {nt + 4, nt + 8, nt + 24 + 56})
    rejectsMutation(offset, "qualified PE identity mutation rejected");
  const auto importRva = u32(mapped, nt + 24 + 112 + 8);
  const auto importSize = u32(mapped, nt + 24 + 112 + 12);
  rejectsMutation(importRva + importSize - 20 + 4, "partial import terminator rejected");
  check(edvr::eliteOculusProfileValidateMapped(mapped.data(), mapped.size(), &out),
        "restored image still passes after all independent mutations");
  check(slotOffset == 0x4DB32E0, "validated slot has qualified RVA");

  Bytes relocated = mapped;
  // Re-map to a different vector address and adjust only DIR64 entries by
  // replaying the raw file map; acceptance demonstrates ASLR-safe validation.
  check(mapImage(raw, &relocated), "second ASLR mapping created");
  check(relocated.data() != mapped.data(), "ASLR test allocations are distinct");
  edvr::OculusProfileMatch rebased{};
  check(edvr::eliteOculusProfileValidateMapped(relocated.data(), relocated.size(), &rebased),
        "different mapped base accepted");
  return failures ? 1 : 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
               SEM_NOOPENFILEERRORBOX);
  if (argc == 2 && !std::wcscmp(argv[1], L"--dry-run")) {
    std::puts("elite_oculus_test: dry-run (no files, mappings, imports or game access)");
    return 0;
  }
  if (argc == 2 && !std::wcscmp(argv[1], L"--self-test")) {
    selfTest();
    std::printf("elite_oculus_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
  }
  if (argc == 3 && !std::wcscmp(argv[1], L"--game")) {
    const int result = gameTest(argv[2]);
    std::printf("elite_oculus_test: %u checks, %u failures\n", checks, failures);
    return result;
  }
  std::fputs("usage: --self-test | --dry-run | --game EliteDangerous64.exe\n", stderr);
  return 2;
}
