#pragma once
#include <cstdint>
struct ExportFixtureStats {
    uint32_t calls[5]{}; // init, shutdown, generic, validity, token
    uint32_t nullErrors = 0, lastApplication = 0;
    uint32_t reentryCalls = 0, reentryResult = 99;
};
