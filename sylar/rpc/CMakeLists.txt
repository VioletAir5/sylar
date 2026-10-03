find_package(Protobuf REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

set(PROTO_FILE "${PROJECT_SOURCE_DIR}/proto/catalog.proto")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")
set(PROTO_SOURCES
    "${GENERATED_DIR}/catalog.pb.cc"
    "${GENERATED_DIR}/catalog.grpc.pb.cc"
)
set(PROTO_HEADERS
    "${GENERATED_DIR}/catalog.pb.h"
    "${GENERATED_DIR}/catalog.grpc.pb.h"
)
add_custom_command(
    OUTPUT ${PROTO_SOURCES} ${PROTO_HEADERS}
    COMMAND "${Protobuf_PROTOC_EXECUTABLE}"
    ARGS "--proto_path=${PROJECT_SOURCE_DIR}/proto"
         "--cpp_out=${GENERATED_DIR}"
         "--grpc_out=${GENERATED_DIR}"
         "--plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}"
         "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}"
    VERBATIM
)
add_library(sylar_rpc_contract STATIC ${PROTO_SOURCES} ${PROTO_HEADERS})
target_include_directories(sylar_rpc_contract PUBLIC "${GENERATED_DIR}")
target_link_libraries(sylar_rpc_contract PUBLIC protobuf::libprotobuf PkgConfig::GRPCPP)

add_library(sylar_grpc STATIC
    ../src/rpc/GrpcRuntime.cpp
    ../src/rpc/Trace.cpp
    ../src/rpc/EndpointDirectory.cpp
    ../src/rpc/CatalogClient.cpp
    ../src/rpc/CatalogService.cpp
)
target_link_libraries(sylar_grpc PUBLIC sylar_core sylar_rpc_contract Threads::Threads)
target_compile_options(sylar_grpc PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

if(SYLAR_WITH_ZOOKEEPER)
    target_sources(sylar_grpc PRIVATE ../src/rpc/ZkDiscovery.cpp)
    target_compile_definitions(sylar_grpc PUBLIC SYLAR_RPC_WITH_ZOOKEEPER=1)
endif()
