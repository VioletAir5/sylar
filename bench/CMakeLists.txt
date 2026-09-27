find_package(Threads REQUIRED)

add_executable(sylar_log_bench bench_log.cpp)
target_link_libraries(sylar_log_bench PRIVATE sylar_core Threads::Threads)
target_compile_options(sylar_log_bench PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)
