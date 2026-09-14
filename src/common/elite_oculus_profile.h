#pragma once

// Strict executable profile proof for Elite's legacy Oculus selector.
//
// This header is intentionally allocation-free and does not use the CRT.  The
// mapped entry point is suitable for a process-attach caller after that
// caller has obtained and guarded a complete MEM_IMAGE span.  The two-argument
// convenience entry point obtains that span with VirtualQuery; callers that
// run under loader lock should put their call in their existing OS exception
// guard.  No profile is enabled when any PE, import, section or code check
// fails.

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace edvr {

struct OculusProfileMatch {
  void** loadLibrarySlot = nullptr;
  std::uintptr_t callerReturnRva = 0;
};

namespace elite_oculus_profile_detail {

constexpr char kProfileId[] = "elite-odyssey-e6be8bbe-libovr-r1";
constexpr char kGameSha256[] =
    "e6be8bbe04e6a7ae226d4318945af7f367de13dc5a007a261964d9ba8144e988";
constexpr std::uint32_t kLoadLibraryIatRva = 0x4DB32E0u;
constexpr std::uint32_t kLoaderCallRva = 0x4E70B6u;
constexpr std::uint32_t kLoaderReturnRva = 0x4E70BCu;

struct Section {
  std::uint32_t rva;
  std::uint32_t span;
  std::uint32_t characteristics;
};

struct View {
  const std::uint8_t* base;
  std::size_t span;
  std::uint32_t imageSize;
  std::uint32_t headersSize;
  std::uint32_t sectionTable;
  std::uint16_t sectionCount;
  std::uint32_t optional;
  std::uint16_t optionalSize;
  std::uint32_t directoryCount;
};

inline bool range(const View& v, std::uint64_t offset,
                  std::uint64_t size) noexcept {
  return offset <= v.span && size <= v.span - offset &&
         offset + size <= v.imageSize;
}

inline bool span_range(const View& v, std::uint64_t offset,
                       std::uint64_t size) noexcept {
  return offset <= v.span && size <= v.span - offset;
}

inline std::uint16_t u16(const View& v, std::uint64_t offset) noexcept {
  return static_cast<std::uint16_t>(v.base[offset]) |
         static_cast<std::uint16_t>(v.base[offset + 1]) << 8;
}

inline std::uint32_t u32(const View& v, std::uint64_t offset) noexcept {
  return static_cast<std::uint32_t>(v.base[offset]) |
         static_cast<std::uint32_t>(v.base[offset + 1]) << 8 |
         static_cast<std::uint32_t>(v.base[offset + 2]) << 16 |
         static_cast<std::uint32_t>(v.base[offset + 3]) << 24;
}

inline std::uint64_t u64(const View& v, std::uint64_t offset) noexcept {
  return static_cast<std::uint64_t>(u32(v, offset)) |
         static_cast<std::uint64_t>(u32(v, offset + 4)) << 32;
}

inline bool section(const View& v, std::uint32_t index,
                    Section* out) noexcept {
  const std::uint64_t off = static_cast<std::uint64_t>(v.sectionTable) +
                            static_cast<std::uint64_t>(index) * 40u;
  if (!range(v, off, 40)) return false;
  const std::uint32_t virtualSize = u32(v, off + 8);
  const std::uint32_t rva = u32(v, off + 12);
  const std::uint32_t rawSize = u32(v, off + 16);
  const std::uint32_t span = virtualSize > rawSize ? virtualSize : rawSize;
  if (!span || static_cast<std::uint64_t>(rva) + span > v.imageSize)
    return false;
  out->rva = rva;
  out->span = span;
  out->characteristics = u32(v, off + 36);
  return true;
}

inline bool mapped_rva(const View& v, std::uint32_t rva,
                       std::uint32_t size) noexcept {
  if (!range(v, rva, size)) return false;
  if (static_cast<std::uint64_t>(rva) + size <= v.headersSize) return true;
  for (std::uint32_t i = 0; i < v.sectionCount; ++i) {
    Section s{};
    if (!section(v, i, &s)) return false;
    if (rva >= s.rva && static_cast<std::uint64_t>(rva) + size <=
                           static_cast<std::uint64_t>(s.rva) + s.span)
      return true;
  }
  return false;
}

inline bool executable_rva(const View& v, std::uint32_t rva,
                           std::uint32_t size) noexcept {
  if (!range(v, rva, size)) return false;
  for (std::uint32_t i = 0; i < v.sectionCount; ++i) {
    Section s{};
    if (!section(v, i, &s)) return false;
    if ((s.characteristics & 0x20000000u) && rva >= s.rva &&
        static_cast<std::uint64_t>(rva) + size <=
            static_cast<std::uint64_t>(s.rva) + s.span)
      return true;
  }
  return false;
}

inline bool cstring_equals(const View& v, std::uint32_t rva,
                           const char* expected) noexcept {
  if (!mapped_rva(v, rva, 1)) return false;
  for (std::uint32_t n = 0; n < 4096; ++n) {
    if (!mapped_rva(v, rva + n, 1)) return false;
    const std::uint8_t actual = v.base[rva + n];
    const std::uint8_t wanted = static_cast<std::uint8_t>(expected[n]);
    if (!actual || !wanted) return actual == wanted;
    const std::uint8_t ac = actual >= 'A' && actual <= 'Z'
                                ? static_cast<std::uint8_t>(actual + 32)
                                : actual;
    const std::uint8_t wc = wanted >= 'A' && wanted <= 'Z'
                                ? static_cast<std::uint8_t>(wanted + 32)
                                : wanted;
    if (ac != wc) return false;
  }
  return false;
}

inline bool cstring_contains_libovr(const View& v, std::uint32_t rva) noexcept {
  if (!mapped_rva(v, rva, 1)) return false;
  std::uint8_t recent[6]{};
  for (unsigned n = 0; n < 64; ++n) {
    if (!mapped_rva(v, rva + n, 1)) return false;
    const std::uint8_t raw = v.base[rva + n];
    if (!raw) return false;
    for (unsigned j = 0; j < 5; ++j) recent[j] = recent[j + 1];
    recent[5] = raw >= 'A' && raw <= 'Z'
                    ? static_cast<std::uint8_t>(raw + 32)
                    : raw;
    if (recent[0] == 'l' && recent[1] == 'i' && recent[2] == 'b' &&
        recent[3] == 'o' && recent[4] == 'v' && recent[5] == 'r') return true;
  }
  return false;
}

inline bool image_view(const void* image, std::size_t span, View* out) noexcept {
  if (!image || !out || span < 64) return false;
  View v{static_cast<const std::uint8_t*>(image), span, 0, 0, 0, 0, 0, 0, 0};
  if (v.base[0] != 'M' || v.base[1] != 'Z' || !span_range(v, 0x3C, 4)) return false;
  const std::uint32_t nt = u32(v, 0x3C);
  if (nt > span - 4 || nt > 0xFFFFFFFFu - 24u ||
      !span_range(v, nt, 24) || u32(v, nt) != 0x00004550u)
    return false;
  v.sectionCount = u16(v, nt + 6);
  v.optionalSize = u16(v, nt + 20);
  v.optional = nt + 24;
  if (v.optional > 0xFFFFFFFFu - v.optionalSize) return false;
  if (!v.sectionCount || v.sectionCount > 96 || v.optionalSize < 112 ||
      !span_range(v, v.optional, v.optionalSize) ||
      u16(v, v.optional) != 0x20Bu || u16(v, nt + 4) != 0x8664u ||
      u32(v, nt + 8) != 0x6A989634u) return false;
  v.directoryCount = u32(v, v.optional + 108);
  if (v.directoryCount > (v.optionalSize - 112u) / 8u) return false;
  v.imageSize = u32(v, v.optional + 56);
  v.headersSize = u32(v, v.optional + 60);
  if (v.sectionCount != 7 || v.imageSize != 0x6409000u ||
      v.headersSize != 0x400u || v.imageSize > span || v.headersSize > v.imageSize ||
      v.headersSize < v.optional + v.optionalSize) return false;
  // PE32+ ImageBase must be 0x140000000 for this revision.  The high dword
  // check avoids a 64-bit load on an unaligned optional-header field.
  if (u32(v, v.optional + 24) != 0x40000000u ||
      u32(v, v.optional + 28) != 1u) return false;
  v.sectionTable = v.optional + v.optionalSize;
  if (!range(v, v.sectionTable, static_cast<std::uint64_t>(v.sectionCount) * 40u))
    return false;
  for (std::uint32_t i = 0; i < v.sectionCount; ++i) {
    Section a{};
    if (!section(v, i, &a)) return false;
    for (std::uint32_t j = 0; j < i; ++j) {
      Section b{};
      if (!section(v, j, &b)) return false;
      if (a.rva < b.rva + b.span && b.rva < a.rva + a.span) return false;
    }
  }
  // The qualified Frontier image has a single .text section beginning at
  // RVA 0x1000 and ending after the highest checked call-chain proof point.
  // This is a compact identity check; code bytes themselves are compared
  // below with ASLR-safe relative-call and fixed instruction signatures.
  Section text{};
  if (!section(v, 0, &text) || text.rva != 0x1000u ||
      text.span < 0x4DB2000u || !(text.characteristics & 0x20000000u))
    return false;
  *out = v;
  return true;
}

inline bool bytes(const View& v, std::uint32_t rva, const std::uint8_t* want,
                  std::uint32_t size) noexcept {
  if (!executable_rva(v, rva, size)) return false;
  for (std::uint32_t i = 0; i < size; ++i)
    if (v.base[rva + i] != want[i]) return false;
  return true;
}

// These FNV-1a blocks were calculated over the raw file bytes after auditing
// the base-relocation table: none of the eleven ranges intersects a DIR64
// relocation.  They therefore remain stable when the image is mapped at a
// different ASLR base; the three vtable checks below explicitly add that base
// to their expected function RVAs.
inline std::uint64_t fnv1a(const View& v, std::uint32_t rva,
                           std::uint32_t size) noexcept {
  if (!executable_rva(v, rva, size)) return 0;
  std::uint64_t hash = 0xCBF29CE484222325ull;
  for (std::uint32_t i = 0; i < size; ++i) {
    hash ^= v.base[rva + i];
    hash *= 0x100000001B3ull;
  }
  return hash;
}

inline bool fingerprint(const View& v, std::uint32_t rva,
                        std::uint32_t size, std::uint64_t expected) noexcept {
  return fnv1a(v, rva, size) == expected;
}

inline bool direct_call(const View& v, std::uint32_t rva,
                        std::uint32_t target) noexcept {
  if (!executable_rva(v, rva, 5) || v.base[rva] != 0xE8) return false;
  const std::int32_t displacement = static_cast<std::int32_t>(u32(v, rva + 1));
  const std::int64_t actual = static_cast<std::int64_t>(rva) + 5 + displacement;
  return actual == target;
}

inline bool loader_call(const View& v, std::uint32_t rva,
                        std::uint32_t iat) noexcept {
  if (!executable_rva(v, rva, 6) || v.base[rva] != 0xFF ||
      v.base[rva + 1] != 0x15) return false;
  const std::int32_t displacement = static_cast<std::int32_t>(u32(v, rva + 2));
  const std::int64_t actual = static_cast<std::int64_t>(rva) + 6 + displacement;
  return actual == iat;
}

inline bool relocated_pointer(const View& v, std::uint32_t rva,
                              std::uint32_t targetRva) noexcept {
  if (!mapped_rva(v, rva, 8)) return false;
  const std::uint64_t expected =
      static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(v.base)) +
      targetRva;
  return u64(v, rva) == expected;
}

} // namespace elite_oculus_profile_detail

inline bool eliteOculusProfileValidateMapped(const void* exeBase,
                                             std::size_t mappedSize,
                                             OculusProfileMatch* out) noexcept {
  using namespace elite_oculus_profile_detail;
  if (!out) return false;
  *out = OculusProfileMatch{};
  View v{};
  if (!image_view(exeBase, mappedSize, &v)) return false;
  if (v.directoryCount <= 1) return false;
  const std::uint32_t importRva = u32(v, v.optional + 112 + 8);
  const std::uint32_t importSize = u32(v, v.optional + 112 + 12);
  if (!importRva || !importSize || !mapped_rva(v, importRva, importSize) ||
      importSize < 20) return false;

  // Delay imports use RVA attributes in this mapped image.  They cannot
  // supply the qualified regular slot, but a LibOVR delay import would make
  // the executable identity ambiguous, so reject it before scanning thunks.
  if (v.directoryCount > 13) {
    const std::uint32_t delayRva = u32(v, v.optional + 112 + 13 * 8);
    const std::uint32_t delaySize = u32(v, v.optional + 112 + 13 * 8 + 4);
    if (delayRva || delaySize) {
      if (!delayRva || !delaySize || delaySize < 32 ||
          static_cast<std::uint64_t>(delayRva) + delaySize > v.imageSize ||
          !mapped_rva(v, delayRva, delaySize)) return false;
      bool delayTerminated = false;
      for (std::uint32_t d = 0; d < delaySize / 32; ++d) {
        const std::uint32_t at = delayRva + d * 32;
        if (!mapped_rva(v, at, 32)) return false;
        const std::uint32_t attrs = u32(v, at);
        const std::uint32_t nameRva = u32(v, at + 4);
        const std::uint32_t first = u32(v, at + 12);
        const std::uint32_t original = u32(v, at + 16);
        if (!attrs && !nameRva && !u32(v, at + 8) && !first && !original &&
            !u32(v, at + 20) && !u32(v, at + 24) && !u32(v, at + 28)) {
          delayTerminated = true;
          break;
        }
        if (attrs != 1 || !nameRva || !first || !original ||
            !mapped_rva(v, nameRva, 1) || !mapped_rva(v, first, 8) ||
            !mapped_rva(v, original, 8)) return false;
        if (cstring_contains_libovr(v, nameRva)) return false;
      }
      if (!delayTerminated) return false;
    }
  }

  void** loadSlot = nullptr;
  unsigned loadCount = 0;
  bool descriptorTerminated = false;
  const std::uint32_t descriptors = importSize / 20;
  for (std::uint32_t d = 0; d < descriptors; ++d) {
    const std::uint32_t at = importRva + d * 20;
    if (!mapped_rva(v, at, 20)) return false;
    const std::uint32_t original = u32(v, at);
    const std::uint32_t nameRva = u32(v, at + 12);
    const std::uint32_t first = u32(v, at + 16);
    if (!original && !u32(v, at + 4) && !u32(v, at + 8) && !nameRva && !first) {
      descriptorTerminated = true;
      break;
    }
    if (!original || !nameRva || !first || !mapped_rva(v, original, 8) ||
        !mapped_rva(v, first, 8) || !mapped_rva(v, nameRva, 1)) return false;
    const bool kernel = cstring_equals(v, nameRva, "KERNEL32.dll");
    if (cstring_contains_libovr(v, nameRva)) return false;
    bool thunkTerminated = false;
    for (std::uint32_t i = 0; i < 65536; ++i) {
      const std::uint64_t entry = static_cast<std::uint64_t>(original) +
                                  static_cast<std::uint64_t>(i) * 8;
      const std::uint64_t slotRva = static_cast<std::uint64_t>(first) +
                                    static_cast<std::uint64_t>(i) * 8;
      if (entry > 0xFFFFFFFFull || slotRva > 0xFFFFFFFFull ||
          !mapped_rva(v, static_cast<std::uint32_t>(entry), 8) ||
          !mapped_rva(v, static_cast<std::uint32_t>(slotRva), 8)) return false;
      const std::uint64_t thunk = u64(v, entry);
      if (!thunk) {
        thunkTerminated = true;
        break;
      }
      if (thunk & (1ull << 63)) {
        if (thunk & ~((1ull << 63) | 0xFFFFull)) return false;
        continue;
      }
      if (thunk > 0xFFFFFFFFull || !mapped_rva(v, static_cast<std::uint32_t>(thunk), 3))
        return false;
      const std::uint32_t name = static_cast<std::uint32_t>(thunk) + 2;
      if (kernel && cstring_equals(v, name, "LoadLibraryW")) {
        if (slotRva != kLoadLibraryIatRva) return false;
        loadSlot = reinterpret_cast<void**>(const_cast<std::uint8_t*>(v.base) + slotRva);
        ++loadCount;
      }
    }
    if (!thunkTerminated) return false;
  }
  if (!descriptorTerminated || loadCount != 1 || !loadSlot) return false;

  if (!fingerprint(v, 0x8D52A0, 0xF3, 0xD7AF874FE886692Full) ||
      !fingerprint(v, 0x4E4AA0, 0x2C7, 0x004862E1F4FCB501ull) ||
      !fingerprint(v, 0x4E4630, 0x23C, 0x6A3EB060D842540Aull) ||
      !fingerprint(v, 0x4E4870, 0x230, 0x8ECA07CE9341C07Eull) ||
      !fingerprint(v, 0x4E8530, 0x155, 0x7ADD324C355953ABull) ||
      !fingerprint(v, 0x4E70D0, 0x984, 0x94C8ABF435366D27ull) ||
      !fingerprint(v, 0x4E6D40, 0x390, 0x586738379777C71Full) ||
      !fingerprint(v, 0x4E0E60, 0xC7, 0xB21D5E8AF10A5286ull) ||
      !fingerprint(v, 0x4E8230, 0x3A, 0x0193764C73F7D26Cull) ||
      !fingerprint(v, 0x4E5CA0, 0x65, 0xFCBA4A2866DCD117ull) ||
      !fingerprint(v, 0x4E86B0, 0x20, 0xF250867D8F233B0Eull)) return false;

  static constexpr std::uint8_t selectorKinds[] = {
      0xC7, 0x44, 0x24, 0x28, 1, 0, 0, 0,
  };
  static constexpr std::uint8_t selectorKind2[] = {
      0xC7, 0x44, 0x24, 0x2C, 2, 0, 0, 0,
  };
  static constexpr std::uint8_t selectorKind3[] = {
      0xC7, 0x44, 0x24, 0x30, 3, 0, 0, 0,
  };
  static constexpr std::uint8_t exitsOnSuccess[] = {0x84, 0xC0, 0x75, 0x45};
  static constexpr std::uint8_t advances[] = {
      0xFF, 0xC7, 0x48, 0x83, 0xC6, 0x04, 0x83, 0xFF, 0x03, 0x72, 0xE1,
  };
  static constexpr std::uint8_t virtualInit[] = {0xFF, 0x50, 0x08};
  static constexpr std::uint8_t initFailure[] = {0x84, 0xC0, 0x74, 0x31};
  static constexpr std::uint8_t falseReturn[] = {0x32, 0xC0};
  static constexpr std::uint8_t nullModule[] = {
      0x48, 0x85, 0xC0, 0x75, 0x1D, 0xB8, 0x47, 0xF4, 0xFF, 0xFF,
  };
  if (!bytes(v, 0x8D530B, selectorKinds, sizeof(selectorKinds)) ||
      !bytes(v, 0x8D5318, selectorKind2, sizeof(selectorKind2)) ||
      !bytes(v, 0x8D5320, selectorKind3, sizeof(selectorKind3)) ||
      !bytes(v, 0x8D5328, exitsOnSuccess, sizeof(exitsOnSuccess)) ||
      !bytes(v, 0x8D533C, advances, sizeof(advances)) ||
      !bytes(v, 0x4E4D50, virtualInit, sizeof(virtualInit)) ||
      !bytes(v, 0x4E467C, initFailure, sizeof(initFailure)) ||
      !bytes(v, 0x4E46B1, falseReturn, sizeof(falseReturn)) ||
      !bytes(v, 0x4E710E, nullModule, sizeof(nullModule)) ||
      !direct_call(v, 0x8D5337, 0x4E4AA0) ||
      !direct_call(v, 0x4E4666, 0x4E8530) ||
      !direct_call(v, 0x4E860F, 0x4E70D0) ||
      !direct_call(v, 0x4E7102, 0x4E6D40) ||
      !direct_call(v, 0x4E5CF3, 0x4E86B0) ||
      !relocated_pointer(v, 0x4E243F8, 0x4E4630) ||
      !relocated_pointer(v, 0x4E24400, 0x4E5CA0) ||
      !relocated_pointer(v, 0x4E245C0, 0x4E4870) ||
      !loader_call(v, kLoaderCallRva, kLoadLibraryIatRva)) return false;
  static constexpr std::uint8_t continuation[] = {
      0x48, 0x8B, 0xCB, 0x48, 0x8B, 0xF0,
  };
  if (!bytes(v, kLoaderReturnRva, continuation, sizeof(continuation))) return false;
  OculusProfileMatch match{};
  match.loadLibrarySlot = loadSlot;
  match.callerReturnRva = kLoaderReturnRva;
  *out = match;
  return true;
}

// This convenience wrapper is for callers which already use an OS fault guard
// around process-attach inspection.  The mapped-size overload above is the
// primitive to use when the caller has its own complete image span.
inline bool eliteOculusProfileValidateUnguarded(void* exeBase,
                                                OculusProfileMatch* out) noexcept {
#if defined(_WIN32)
  if (out) *out = OculusProfileMatch{};
  if (!exeBase || !out) return false;
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(exeBase, &mbi, sizeof(mbi)) != sizeof(mbi) ||
      mbi.AllocationBase != exeBase || mbi.State != MEM_COMMIT ||
      mbi.Type != MEM_IMAGE) return false;
  // A PE image's first mapped region contains the headers.  Read only the
  // bounded optional-header field needed to discover SizeOfImage.  The
  // public wrapper below catches a stale/unmapped page while the pure parser
  // checks every range it touches; requiring every image page to be committed
  // here would incorrectly reject images with discarded sections.
  const auto* p = static_cast<const std::uint8_t*>(exeBase);
  if (mbi.RegionSize < 64 || p[0] != 'M' || p[1] != 'Z') return false;
  const std::uint32_t nt = static_cast<std::uint32_t>(p[0x3C]) |
                           static_cast<std::uint32_t>(p[0x3D]) << 8 |
                           static_cast<std::uint32_t>(p[0x3E]) << 16 |
                           static_cast<std::uint32_t>(p[0x3F]) << 24;
  if (nt > mbi.RegionSize - 4 || nt > 0x10000000u ||
      mbi.RegionSize < static_cast<SIZE_T>(nt) + 24 + 60) return false;
  const auto* optional = p + nt + 24;
  const std::uint32_t imageSize = static_cast<std::uint32_t>(optional[56]) |
                                   static_cast<std::uint32_t>(optional[57]) << 8 |
                                   static_cast<std::uint32_t>(optional[58]) << 16 |
                                   static_cast<std::uint32_t>(optional[59]) << 24;
  if (!imageSize || imageSize > 0x40000000u) return false;
  return eliteOculusProfileValidateMapped(exeBase, imageSize, out);
#else
  (void)exeBase;
  (void)out;
  return false;
#endif
}

inline bool eliteOculusProfileValidate(void* exeBase,
                                       OculusProfileMatch* out) noexcept {
#if defined(_WIN32) && defined(_MSC_VER)
  __try {
    return eliteOculusProfileValidateUnguarded(exeBase, out);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    if (out) *out = OculusProfileMatch{};
    return false;
  }
#else
  return eliteOculusProfileValidateUnguarded(exeBase, out);
#endif
}

} // namespace edvr
