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

find_program(OPENSSL_EXECUTABLE openssl)
if(OPENSSL_EXECUTABLE)
    add_test(
        NAME HttpsApp.VerificationAndShutdown
        COMMAND Python3::Interpreter
            "${CMAKE_CURRENT_SOURCE_DIR}/test_https_app.py"
            $<TARGET_FILE:http_server>
            $<TARGET_FILE:http_client>
    )
    set_tests_properties(HttpsApp.VerificationAndShutdown PROPERTIES TIMEOUT 20)
endif()

add_test(
    NAME WebSocketApp.ProtocolAndShutdown
    COMMAND Python3::Interpreter
        "${CMAKE_CURRENT_SOURCE_DIR}/test_websocket_app.py"
        $<TARGET_FILE:ws_echo_server>
)
set_tests_properties(WebSocketApp.ProtocolAndShutdown PROPERTIES TIMEOUT 20)

if(OPENSSL_EXECUTABLE)
    add_test(
        NAME WebSocketClientApp.WsAndWss
        COMMAND Python3::Interpreter
            "${CMAKE_CURRENT_SOURCE_DIR}/test_websocket_client_app.py"
            $<TARGET_FILE:ws_client>
            $<TARGET_FILE:ws_echo_server>
    )
    set_tests_properties(WebSocketClientApp.WsAndWss PROPERTIES TIMEOUT 25)
endif()

add_test(
    NAME SqliteApp.PersistsAcrossProcesses
    COMMAND Python3::Interpreter
        "${CMAKE_CURRENT_SOURCE_DIR}/test_sqlite_app.py"
        $<TARGET_FILE:sqlite_demo>
)
set_tests_properties(SqliteApp.PersistsAcrossProcesses PROPERTIES TIMEOUT 10)

add_test(
    NAME SmtpApp.PlainTlsAndReject
    COMMAND Python3::Interpreter
        "${CMAKE_CURRENT_SOURCE_DIR}/test_smtp_app.py"
        $<TARGET_FILE:smtp_demo>
)
set_tests_properties(SmtpApp.PlainTlsAndReject PROPERTIES TIMEOUT 20)

add_test(
    NAME RedisApp.TypedRepliesAndShutdown
    COMMAND Python3::Interpreter
        "${CMAKE_CURRENT_SOURCE_DIR}/test_redis_app.py"
        $<TARGET_FILE:redis_demo>
)
set_tests_properties(RedisApp.TypedRepliesAndShutdown PROPERTIES TIMEOUT 20)

add_test(
    NAME MySqlApp.PreparedQueryAndShutdown
    COMMAND Python3::Interpreter
        "${CMAKE_CURRENT_SOURCE_DIR}/test_mysql_app.py"
        $<TARGET_FILE:mysql_demo>
)
set_tests_properties(MySqlApp.PreparedQueryAndShutdown PROPERTIES TIMEOUT 60)

add_test(
    NAME ZooKeeperApp.NodesAndShutdown
    COMMAND Python3::Interpreter
        "${CMAKE_CURRENT_SOURCE_DIR}/test_zk_app.py"
        $<TARGET_FILE:zk_demo>
)
set_tests_properties(ZooKeeperApp.NodesAndShutdown PROPERTIES TIMEOUT 30)
