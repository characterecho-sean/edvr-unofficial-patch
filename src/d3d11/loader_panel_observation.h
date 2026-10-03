#pragma once

#include <cstdint>
#include <type_traits>

namespace edvr {

template <class T>
struct LoaderPanelRead final {
    bool reached = false;
    bool known = false;
    T value{};
};

struct LoaderPanelSequenceObservation final {
    LoaderPanelRead<std::uint32_t> position;
    LoaderPanelRead<std::uint32_t> hashBeforeCount;
    LoaderPanelRead<std::uint32_t> hashAfterCount;
    LoaderPanelRead<std::uint32_t> hashBeforeWidth;
    LoaderPanelRead<std::uint32_t> hashAfterWidth;
    LoaderPanelRead<std::uint32_t> hashBeforeHeight;
    LoaderPanelRead<std::uint32_t> hashAfterHeight;
    LoaderPanelRead<std::uint32_t> slotCount;
    LoaderPanelRead<std::uint32_t> slotWidth;
    LoaderPanelRead<std::uint32_t> slotHeight;
    LoaderPanelRead<std::uint32_t> lenBefore;
    LoaderPanelRead<std::uint32_t> lenAfter;
};

struct LoaderPanelPanelObservation final {
    LoaderPanelRead<bool> frameAnyBefore;
    LoaderPanelRead<bool> frameAnyAfter;
    LoaderPanelRead<bool> frameFirstPanelDoneBefore;
    LoaderPanelRead<bool> frameFirstPanelDoneAfter;
    LoaderPanelRead<bool> chainOnBeforeCollection;
    LoaderPanelRead<std::uint32_t> chainWidth;
    LoaderPanelRead<std::uint32_t> chainHeight;
    LoaderPanelRead<bool> frameChainPanelBefore;
    LoaderPanelRead<bool> frameChainPanelAfter;
    LoaderPanelRead<std::uint32_t> panelOrdinalBefore;
    LoaderPanelRead<std::uint32_t> panelOrdinalAfter;
    // Consistency check for the value later consumed by the chain ordinal scan.
    LoaderPanelRead<std::uint32_t> localOrdinal;
};

struct LoaderPanelCollectionObservation final {
    LoaderPanelRead<bool> collecting;
    LoaderPanelRead<std::uint32_t> capCountGate;
    LoaderPanelRead<bool> guardEntered;
    LoaderPanelRead<bool> guardReturned;
    LoaderPanelRead<std::uint32_t> capCountBeforeWrite;
    LoaderPanelRead<std::uint32_t> capCountAfterWrite;
    LoaderPanelRead<std::uint32_t> ibFillBeforeWrite;
    LoaderPanelRead<std::uint32_t> ibFillAfterWrite;
    LoaderPanelRead<std::uint32_t> capDroppedBeforeWrite;
    LoaderPanelRead<std::uint32_t> capDroppedAfterWrite;
    // True means GPU staging/copy work entered; its full ledger is not recorded.
    bool collectionMutationUnobserved = false;
};

struct LoaderPanelChainRead final {
    LoaderPanelRead<std::uint32_t> loopCount;
    LoaderPanelRead<std::uint32_t> chainOrd;
};

struct LoaderPanelWithholdObservation final {
    LoaderPanelRead<bool> subArmBeforeClear;
    LoaderPanelRead<bool> subArmAfterClear;
    LoaderPanelRead<bool> subArmBeforeWithhold;
    LoaderPanelRead<bool> subArmAfterWithhold;
    LoaderPanelRead<std::uint32_t> chainOrdCountGate;
    LoaderPanelChainRead chainScan[4]{};
    LoaderPanelRead<std::uint32_t> terminalChainOrdCount;
    LoaderPanelRead<bool> specDoneGate;
    LoaderPanelRead<bool> chainOnSpecGate;
    LoaderPanelRead<bool> retiredGate;
    LoaderPanelRead<bool> frameWithheldBefore;
    LoaderPanelRead<bool> frameWithheldAfter;
    LoaderPanelRead<bool> dimLiveBefore;
    LoaderPanelRead<bool> dimLiveAfter;
};

struct LoaderPanelOuterObservation final {
    LoaderPanelRead<bool> wants;
    LoaderPanelRead<std::uint32_t> eyeDrawsLastFrame;
    LoaderPanelRead<bool> rtvPresent;
    LoaderPanelRead<bool> resolved;
    LoaderPanelRead<bool> isTexture2D;
    LoaderPanelRead<std::uint32_t> targetWidth;
    LoaderPanelRead<std::uint32_t> targetHeight;
    LoaderPanelRead<std::uint32_t> qsStartIndex;
    LoaderPanelRead<std::int32_t> qsBaseVertex;
    LoaderPanelRead<bool> textured;
};

struct LoaderPanelHelperObservation final {
    LoaderPanelRead<bool> wants;
    LoaderPanelRead<bool> contextNonNull;
    LoaderPanelSequenceObservation sequence;
    LoaderPanelPanelObservation panel;
    LoaderPanelCollectionObservation collection;
    LoaderPanelWithholdObservation withhold;
};

struct LoaderPanelObservation final {
    std::uint16_t siteId = 25;
    std::uint8_t kind = 19;
    LoaderPanelOuterObservation outer;
    LoaderPanelHelperObservation helper;
};

static_assert(std::is_standard_layout<LoaderPanelObservation>::value &&
                  std::is_trivially_copyable<LoaderPanelObservation>::value,
              "LoaderPanel observations must remain pointer-free PODs");

} // namespace edvr
