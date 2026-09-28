file(GLOB SYLAR_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp"
)

if(SYLAR_TEST_SOURCES)
    find_package(GTest CONFIG REQUIRED)
    include(GoogleTest)

    foreach(test_source IN LISTS SYLAR_TEST_SOURCES)
        get_filename_component(test_name "${test_source}" NAME_WE)
        add_executable("${test_name}" "${test_source}")
        target_link_libraries("${test_name}" PRIVATE sylar_core GTest::gtest_main)
        gtest_discover_tests("${test_name}")
    endforeach()
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(
    NAME EchoApp.ConfigIdleAndShutdown
    COMMAND Python3::Interpreter
        "${CMAKE_CURRENT_SOURCE_DIR}/test_echo_app.py"
        $<TARGET_FILE:echo_server>
)
set_tests_properties(EchoApp.ConfigIdleAndShutdown PROPERTIES TIMEOUT 5)

add_test(
    NAME HttpApp.PipeliningAndShutdown
    COMMAND Python3::Interpreter
        "${CMAKE_CURRENT_SOURCE_DIR}/test_http_app.py"
        $<TARGET_FILE:http_server>
)
set_tests_properties(HttpApp.PipeliningAndShutdown PROPERTIES TIMEOUT 5)
