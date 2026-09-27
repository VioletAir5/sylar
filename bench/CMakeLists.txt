find_package(Threads REQUIRED)

add_executable(sylar_log_bench bench_log.cpp)
target_link_libraries(sylar_log_bench PRIVATE sylar_core Threads::Threads)
target_compile_options(sylar_log_bench PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

set(SYLAR_SPDLOG_SOURCE_DIR "" CACHE PATH "Path to a separate spdlog source checkout")
if(SYLAR_SPDLOG_SOURCE_DIR)
    if(NOT EXISTS "${SYLAR_SPDLOG_SOURCE_DIR}/include/spdlog/spdlog.h")
        message(FATAL_ERROR "SYLAR_SPDLOG_SOURCE_DIR must contain include/spdlog/spdlog.h")
    endif()

    add_executable(spdlog_log_bench bench_spdlog.cpp)
    target_include_directories(spdlog_log_bench PRIVATE
        "${SYLAR_SPDLOG_SOURCE_DIR}/include"
    )
    target_link_libraries(spdlog_log_bench PRIVATE Threads::Threads)
    target_compile_options(spdlog_log_bench PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
    )
endif()
