find_package(fmt CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS context)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)

set(SYLAR_LINK_LIBRARIES
    fmt::fmt yaml-cpp Boost::context
    OpenSSL::SSL OpenSSL::Crypto SQLite::SQLite3
)

if(SYLAR_WITH_REDIS OR SYLAR_WITH_MYSQL)
    find_package(PkgConfig REQUIRED)
endif()
if(SYLAR_WITH_REDIS)
    pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)
    list(APPEND SYLAR_LINK_LIBRARIES PkgConfig::HIREDIS)
endif()
if(SYLAR_WITH_MYSQL)
    pkg_check_modules(MYSQLCLIENT REQUIRED IMPORTED_TARGET mysqlclient)
    list(APPEND SYLAR_LINK_LIBRARIES PkgConfig::MYSQLCLIENT)
endif()
if(SYLAR_WITH_ZOOKEEPER)
    find_library(ZOOKEEPER_MT_LIBRARY NAMES zookeeper_mt REQUIRED)
    list(APPEND SYLAR_LINK_LIBRARIES ${ZOOKEEPER_MT_LIBRARY})
endif()

file(GLOB_RECURSE SYLAR_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)
if(NOT SYLAR_WITH_REDIS)
    list(FILTER SYLAR_SOURCES EXCLUDE REGEX "/src/redis/")
endif()
if(NOT SYLAR_WITH_MYSQL)
    list(FILTER SYLAR_SOURCES EXCLUDE REGEX "/src/db/MySql\\.cpp$")
endif()
if(NOT SYLAR_WITH_ZOOKEEPER)
    list(FILTER SYLAR_SOURCES EXCLUDE REGEX "/src/zk/")
endif()

add_library(sylar_core STATIC ${SYLAR_SOURCES})
target_include_directories(sylar_core PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(sylar_core PUBLIC ${SYLAR_LINK_LIBRARIES})
target_compile_options(sylar_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

file(GLOB SYLAR_APPS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/app/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/*.cxx"
)
foreach(app_source IN LISTS SYLAR_APPS)
    get_filename_component(app_name "${app_source}" NAME_WE)
    if((app_name STREQUAL "redis_demo" AND NOT SYLAR_WITH_REDIS) OR
       (app_name STREQUAL "mysql_demo" AND NOT SYLAR_WITH_MYSQL) OR
       (app_name STREQUAL "zk_demo" AND NOT SYLAR_WITH_ZOOKEEPER))
        continue()
    endif()
    add_executable("${app_name}" "${app_source}")
    target_link_libraries("${app_name}" PRIVATE sylar_core)
endforeach()
