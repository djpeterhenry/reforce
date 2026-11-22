#pragma once
// Time budgets in tests (GPU / CPU milliseconds). On a real Mac they are checked. With REFORCE_NO_PERF_BUDGETS set
// (CI: a virtual GPU whose timings mean nothing) a missed budget is only reported.
#include <cstdlib>

#include <doctest/doctest.h>

#define PERF_BUDGET(expr)                                                         \
    do {                                                                          \
        if (!std::getenv("REFORCE_NO_PERF_BUDGETS")) {                            \
            CHECK(expr);                                                          \
        } else if (!(expr)) {                                                     \
            MESSAGE("time budget missed (not enforced here): " #expr);            \
        }                                                                         \
    } while (0)
