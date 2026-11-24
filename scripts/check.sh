#!/bin/sh
# Runs the test suite with checks for undefined behavior and uninitialized memory switched on.
#
#   scripts/check.sh [poison|asan|tsan|all] [ctest arguments...]     (default: all = poison, then asan)
#
#   poison  the normal build (build/): GPU poison mode (REFORCE_GPU_POISON, src/render/GpuPoison.hpp), new heap
#           memory pre-filled with 0xAA and freed memory with 0x55 (MallocPreScribble / MallocScribble), Metal API
#           validation (MTL_DEBUG_LAYER) and shader validation (MTL_SHADER_VALIDATION: out-of-bounds buffer and
#           texture access in shaders)
#   asan    build-asan/: AddressSanitizer + UndefinedBehaviorSanitizer (out of bounds, use after free, overflow,
#           bad shifts, misaligned access, ...), uninitialized stack variables filled with a pattern, all new heap
#           memory filled with 0xBE, plus GPU poison mode
#   tsan    build-tsan/: ThreadSanitizer (data races between the render, audio, web and lights threads); slow.
#           Known false positives and third-party races: tests/tsan.supp
#
# Time budgets are reported, not enforced (REFORCE_NO_PERF_BUDGETS): every mode here runs slower than normal.
# Examples: scripts/check.sh asan -R render     scripts/check.sh poison --output-on-failure
set -eu

repo=$(cd "$(dirname "$0")/.." && pwd)
mode=${1:-all}
[ $# -gt 0 ] && shift
jobs=$(sysctl -n hw.ncpu)

build() {  # build DIR [cmake -D options...]
    dir=$1
    shift
    cmake -S "$repo" -B "$repo/$dir" -DCMAKE_BUILD_TYPE=RelWithDebInfo "$@" >/dev/null
    cmake --build "$repo/$dir" -j "$jobs"
}

poison() {
    echo "== poison: GPU poison, malloc scribble, Metal API + shader validation (build/)"
    build build
    REFORCE_GPU_POISON=1 MallocPreScribble=1 MallocScribble=1 MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
        REFORCE_NO_PERF_BUDGETS=1 ctest --test-dir "$repo/build" -j 3 "$@"
}

asan() {
    echo "== asan: AddressSanitizer + UndefinedBehaviorSanitizer + GPU poison (build-asan/)"
    build build-asan -DREFORCE_SANITIZE=address,undefined
    ASAN_OPTIONS=detect_stack_use_after_return=1:strict_string_checks=1:check_initialization_order=1:max_malloc_fill_size=2147483647 \
    UBSAN_OPTIONS=print_stacktrace=1 \
    REFORCE_GPU_POISON=1 REFORCE_NO_PERF_BUDGETS=1 ctest --test-dir "$repo/build-asan" -j 3 "$@"
}

tsan() {
    echo "== tsan: ThreadSanitizer (build-tsan/)"
    build build-tsan -DREFORCE_SANITIZE=thread
    TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:suppressions="$repo/tests/tsan.supp" \
    REFORCE_NO_PERF_BUDGETS=1 ctest --test-dir "$repo/build-tsan" -j 3 "$@"
}

case $mode in
    poison) poison "$@" ;;
    asan) asan "$@" ;;
    tsan) tsan "$@" ;;
    all) poison "$@" && asan "$@" ;;
    *) echo "usage: $0 [poison|asan|tsan|all] [ctest arguments...]" >&2; exit 2 ;;
esac
