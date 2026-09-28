find_package(fmt CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS context)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)

# Reconfigure when a source file is added, so new modules need no CMake edit.
file(GLOB_RECURSE SYLAR_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)

if(SYLAR_SOURCES)
    add_library(sylar_core STATIC ${SYLAR_SOURCES})
    target_include_directories(sylar_core PUBLIC "${PROJECT_SOURCE_DIR}/include")
    target_link_libraries(sylar_core PUBLIC fmt::fmt yaml-cpp Boost::context OpenSSL::SSL OpenSSL::Crypto SQLite::SQLite3)
    target_compile_options(sylar_core PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
    )
else()
    # The repository can configure before the first implementation file exists.
    add_library(sylar_core INTERFACE)
    target_include_directories(sylar_core INTERFACE "${PROJECT_SOURCE_DIR}/include")
    target_link_libraries(sylar_core INTERFACE fmt::fmt yaml-cpp Boost::context OpenSSL::SSL OpenSSL::Crypto SQLite::SQLite3)
endif()

file(GLOB SYLAR_APPS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/app/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/*.cxx"
)

foreach(app_source IN LISTS SYLAR_APPS)
    get_filename_component(app_name "${app_source}" NAME_WE)
    add_executable("${app_name}" "${app_source}")
    target_link_libraries("${app_name}" PRIVATE sylar_core)
endforeach()
