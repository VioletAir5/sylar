find_package(Threads REQUIRED)

add_executable(sylar_log_bench bench_log.cpp)
target_link_libraries(sylar_log_bench PRIVATE sylar_core Threads::Threads)
target_compile_options(sylar_log_bench PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

option(SYLAR_BENCH_WITH_SPDLOG "Build comparison benchmark against installed spdlog" OFF)
if(SYLAR_BENCH_WITH_SPDLOG)
    find_package(spdlog CONFIG REQUIRED)
    add_executable(spdlog_log_bench bench_spdlog.cpp)
    target_link_libraries(spdlog_log_bench PRIVATE
        spdlog::spdlog Threads::Threads
    )
    target_compile_options(spdlog_log_bench PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
    )
endif()

add_executable(sylar_echo_bench bench_echo.cpp)
target_compile_options(sylar_echo_bench PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

add_executable(sylar_http_bench bench_http.cpp)
target_compile_options(sylar_http_bench PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

if(SYLAR_WITH_GRPC)
    add_executable(sylar_trace_export_bench bench_trace_export.cpp)
    target_link_libraries(sylar_trace_export_bench PRIVATE sylar_grpc)
    add_executable(sylar_trace_bench bench_trace.cpp)
    target_link_libraries(sylar_trace_bench PRIVATE sylar_grpc)
    add_executable(sylar_rpc_bench bench_rpc.cpp)
    target_link_libraries(sylar_rpc_bench PRIVATE sylar_grpc Threads::Threads)
    target_compile_options(sylar_rpc_bench PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)
endif()

if(SYLAR_WITH_GRPC AND BUILD_TESTING)
    add_test(NAME GrpcTrace.AllocationBudget
        COMMAND sylar_trace_bench 10000 1)
    set_tests_properties(GrpcTrace.AllocationBudget PROPERTIES TIMEOUT 10)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME GrpcBenchmark.Smoke
        COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/bench/run_rpc_benchmark.py"
            --build "${CMAKE_BINARY_DIR}" --requests 200 --concurrency 4 --rounds 1
            --output "${CMAKE_BINARY_DIR}/rpc_smoke.json")
    set_tests_properties(GrpcBenchmark.Smoke PROPERTIES TIMEOUT 30)
endif()
