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
