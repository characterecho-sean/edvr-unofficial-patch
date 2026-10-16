// A standalone driver for flat_offscreen_tests.h: the same checks flat_temporal_test runs, in a binary that compiles in a fraction of the
// time, so offscreen_mutants.py can build one per mutation of src\d3d11\flat_offscreen_fast.h. Run from the repository root (the corpus and
// the sources are read by relative path).
#include <cstdio>

#include "flat_offscreen_tests.h"

int main(int argc, char**) {
    const int failures = flatOffscreenTests();
    std::printf("flat_offscreen_test: %d failure(s)\n", failures);
    (void)argc;
    return failures ? 1 : 0;
}
