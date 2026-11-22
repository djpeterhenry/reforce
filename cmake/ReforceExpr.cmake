# reforce_expr: expression engine (lexer, Pratt parser, resolver, bytecode
# compiler, VM, built-ins). Pure C++20, no Apple dependencies. Also adds the
# scene loader / tier evaluation sources to reforce_scene, the expr unit tests
# and the (not ctest-run) micro-benchmark.
# Included from the top-level CMakeLists.txt (paths are relative to it).

add_library(reforce_expr STATIC
    src/expr/Builtins.cpp
    src/expr/BuiltinsMatrix.cpp
    src/expr/Compiler.cpp
    src/expr/Lexer.cpp
    src/expr/Operators.cpp
    src/expr/Parser.cpp
    src/expr/Value.cpp
    src/expr/Vars.cpp
    src/expr/Vm.cpp
)
target_include_directories(reforce_expr PUBLIC src)
target_compile_options(reforce_expr PRIVATE ${REFORCE_WARNINGS})

target_sources(reforce_scene PRIVATE
    src/scene/SceneFile.cpp
    src/scene/SceneConfig.cpp
    src/scene/Evaluate.cpp
    src/scene/WorkerPool.cpp
)
target_link_libraries(reforce_scene PUBLIC reforce_expr)

if(REFORCE_BUILD_TESTS)
    enable_testing()
    add_executable(reforce_expr_tests
        tests/test_main.cpp
        tests/test_expr_parser.cpp
        tests/test_expr_builtins.cpp
        tests/test_expr_vm.cpp
        tests/test_scene_file.cpp
        tests/test_scene_eval.cpp
        tests/test_corpus.cpp
    )
    target_compile_options(reforce_expr_tests PRIVATE ${REFORCE_WARNINGS})
    target_compile_definitions(reforce_expr_tests PRIVATE
        REFORCE_SCENES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scenes")
    target_link_libraries(reforce_expr_tests PRIVATE reforce_scene doctest::doctest)
    add_test(NAME reforce_expr_tests COMMAND reforce_expr_tests)

    # Micro-benchmark: build only, run by hand (not registered with ctest).
    add_executable(reforce_expr_bench tests/bench_expr.cpp)
    target_compile_options(reforce_expr_bench PRIVATE ${REFORCE_WARNINGS})
    target_compile_definitions(reforce_expr_bench PRIVATE
        REFORCE_SCENES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scenes")
    target_link_libraries(reforce_expr_bench PRIVATE reforce_scene)
endif()
