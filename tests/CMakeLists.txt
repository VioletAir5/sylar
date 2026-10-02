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
        if(test_name STREQUAL "test_grpc" AND NOT SYLAR_WITH_GRPC)
            continue()
        endif()
        add_executable("${test_name}" "${test_source}")
        target_link_libraries("${test_name}" PRIVATE sylar_core GTest::gtest_main)
        if(test_name STREQUAL "test_grpc")
            target_link_libraries("${test_name}" PRIVATE sylar_grpc)
        endif()
        if(test_name STREQUAL "test_grpc")
            gtest_discover_tests("${test_name}" PROPERTIES TIMEOUT 10)
        else()
            gtest_discover_tests("${test_name}")
        endif()
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

add_test(
    NAME HttpCluster.IndependentReplicas
    COMMAND Python3::Interpreter
        "${CMAKE_CURRENT_SOURCE_DIR}/test_http_cluster.py"
        $<TARGET_FILE:http_server>
)
set_tests_properties(HttpCluster.IndependentReplicas PROPERTIES TIMEOUT 12)

add_test(
    NAME HttpApp.AdmissionAndDrain
    COMMAND Python3::Interpreter
        "${CMAKE_CURRENT_SOURCE_DIR}/test_http_drain_app.py"
        $<TARGET_FILE:http_server>
)
set_tests_properties(HttpApp.AdmissionAndDrain PROPERTIES TIMEOUT 12)

add_test(
    NAME HttpApp.DeterministicFaultMatrix
    COMMAND Python3::Interpreter
        "${CMAKE_CURRENT_SOURCE_DIR}/test_http_fault_matrix.py"
        $<TARGET_FILE:http_server>
)
set_tests_properties(HttpApp.DeterministicFaultMatrix PROPERTIES TIMEOUT 12)

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

if(OPENSSL_EXECUTABLE)
    add_test(
        NAME WebSocketApp.ProtocolAndShutdown
        COMMAND Python3::Interpreter
            "${CMAKE_CURRENT_SOURCE_DIR}/test_websocket_app.py"
            $<TARGET_FILE:ws_echo_server>
    )
    set_tests_properties(WebSocketApp.ProtocolAndShutdown PROPERTIES TIMEOUT 20)
endif()

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

if(OPENSSL_EXECUTABLE)
    add_test(
        NAME SmtpApp.PlainTlsStartTlsAndReject
        COMMAND Python3::Interpreter
            "${CMAKE_CURRENT_SOURCE_DIR}/test_smtp_app.py"
            $<TARGET_FILE:smtp_demo>
    )
    set_tests_properties(SmtpApp.PlainTlsStartTlsAndReject PROPERTIES TIMEOUT 20)
endif()

find_program(REDIS_SERVER_EXECUTABLE redis-server)
if(SYLAR_WITH_REDIS AND REDIS_SERVER_EXECUTABLE)
    add_test(
        NAME RedisApp.TypedRepliesAndShutdown
        COMMAND Python3::Interpreter
            "${CMAKE_CURRENT_SOURCE_DIR}/test_redis_app.py"
            $<TARGET_FILE:redis_demo>
    )
    set_tests_properties(RedisApp.TypedRepliesAndShutdown PROPERTIES TIMEOUT 20)
    add_test(
        NAME RedisCluster.BroadcastReplayAndGap
        COMMAND Python3::Interpreter
            "${CMAKE_CURRENT_SOURCE_DIR}/test_redis_stream_cluster.py"
            $<TARGET_FILE:redis_ws_server>
    )
    set_tests_properties(RedisCluster.BroadcastReplayAndGap PROPERTIES TIMEOUT 25)
endif()

find_program(MYSQLD_EXECUTABLE mysqld)
find_program(MYSQLADMIN_EXECUTABLE mysqladmin)
if(SYLAR_WITH_MYSQL AND MYSQLD_EXECUTABLE AND MYSQLADMIN_EXECUTABLE)
    add_test(
        NAME MySqlApp.PreparedQueryAndShutdown
        COMMAND Python3::Interpreter
            "${CMAKE_CURRENT_SOURCE_DIR}/test_mysql_app.py"
            $<TARGET_FILE:mysql_demo>
    )
    set_tests_properties(MySqlApp.PreparedQueryAndShutdown PROPERTIES TIMEOUT 60)
endif()

find_program(JAVA_EXECUTABLE java)
if(SYLAR_WITH_ZOOKEEPER AND JAVA_EXECUTABLE AND
   EXISTS "/usr/share/zookeeper/bin/zkServer.sh")
    add_test(
        NAME ZooKeeperApp.NodesAndShutdown
        COMMAND Python3::Interpreter
            "${CMAKE_CURRENT_SOURCE_DIR}/test_zk_app.py"
            $<TARGET_FILE:zk_demo>
    )
    set_tests_properties(ZooKeeperApp.NodesAndShutdown PROPERTIES TIMEOUT 30)
endif()

if(SYLAR_WITH_GRPC)
    add_test(NAME GrpcCluster.GatewayAndFailures
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/test_grpc_cluster.py"
            $<TARGET_FILE:catalog_server> $<TARGET_FILE:rpc_gateway>)
    set_tests_properties(GrpcCluster.GatewayAndFailures PROPERTIES TIMEOUT 30)
endif()
