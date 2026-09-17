cmake_minimum_required(VERSION 3.16)
set(CMAKE_AUTOMOC ON)

# Google Test — 
find_package(GTest REQUIRED)

include_directories(
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/include/server
    ${PROJECT_SOURCE_DIR}/include/server/db
    ${PROJECT_SOURCE_DIR}/include/server/model
    ${PROJECT_SOURCE_DIR}/include/server/redis
    ${PROJECT_SOURCE_DIR}/include/server/utils
    ${PROJECT_SOURCE_DIR}/src/server
    ${PROJECT_SOURCE_DIR}/thirdparty
)

set(COMMON_LIBS GTest::gtest GTest::gtest_main ssl crypto)

# --- 纯逻辑测试（无外部依赖） ---

add_executable(test_rate_limiter
    test_rate_limiter.cpp
    ${PROJECT_SOURCE_DIR}/src/server/utils/rate_limiter.cpp
)
target_link_libraries(test_rate_limiter ${COMMON_LIBS})

add_executable(test_jsoncodec
    test_jsoncodec.cpp
    ${PROJECT_SOURCE_DIR}/src/server/jsoncodec.cpp
)
target_link_libraries(test_jsoncodec ${COMMON_LIBS} muduo_net muduo_base pthread)

add_executable(test_protocodec
    test_protocodec.cpp
    ${PROJECT_SOURCE_DIR}/src/server/protocodec.cpp
)
target_link_libraries(test_protocodec ${COMMON_LIBS} muduo_net muduo_base pthread)

add_executable(test_password_utils
    test_password_utils.cpp
    ${PROJECT_SOURCE_DIR}/src/server/utils/password_utils.cpp
)
target_link_libraries(test_password_utils ${COMMON_LIBS})

add_executable(test_session_manager
    test_session_manager.cpp
    ${PROJECT_SOURCE_DIR}/src/server/gateway/session_manager.cpp
)
target_link_libraries(test_session_manager ${COMMON_LIBS} pthread)

add_executable(test_message_decoder
    test_message_decoder.cpp
    ${PROJECT_SOURCE_DIR}/src/server/protocol/message_decoder.cpp
    ${PROJECT_SOURCE_DIR}/src/server/protocol/message_encoder.cpp
    ${PROJECT_SOURCE_DIR}/src/server/jsoncodec.cpp
    ${PROJECT_SOURCE_DIR}/src/server/protocodec.cpp
    ${PROJECT_SOURCE_DIR}/src/server/binary_codec.cpp
    ${PROJECT_SOURCE_DIR}/build/generated/proto/chat.pb.cc
)
target_link_libraries(test_message_decoder
    ${COMMON_LIBS}
    muduo_net
    muduo_base
    pthread
    ${Protobuf_LIBRARIES}
)

add_executable(test_message_router
    test_message_router.cpp
    ${PROJECT_SOURCE_DIR}/src/server/gateway/message_router.cpp
    ${PROJECT_SOURCE_DIR}/src/server/protocol/message_encoder.cpp
    ${PROJECT_SOURCE_DIR}/src/server/jsoncodec.cpp
    ${PROJECT_SOURCE_DIR}/src/server/protocodec.cpp
    ${PROJECT_SOURCE_DIR}/build/generated/proto/chat.pb.cc
)

add_executable(test_pending_delivery_manager
    test_pending_delivery_manager.cpp
    ${PROJECT_SOURCE_DIR}/src/server/gateway/pending_delivery_manager.cpp
)
target_link_libraries(test_pending_delivery_manager
    ${COMMON_LIBS} muduo_net muduo_base pthread)
target_link_libraries(test_message_router
    ${COMMON_LIBS}
    muduo_net
    muduo_base
    pthread
    ${Protobuf_LIBRARIES}
)

add_executable(test_cluster
    test_cluster.cpp
    ${PROJECT_SOURCE_DIR}/src/server/cluster/presence_manager.cpp
    ${PROJECT_SOURCE_DIR}/src/server/cluster/route_transport.cpp
    ${PROJECT_SOURCE_DIR}/src/server/redis/redis.cpp
    ${PROJECT_SOURCE_DIR}/src/server/config.cpp
)
target_compile_definitions(test_cluster PRIVATE
    CHAT_TEST_CONFIG="${PROJECT_SOURCE_DIR}/conf/chatserver.json")
target_link_libraries(test_cluster ${COMMON_LIBS} hiredis muduo_base pthread)

# --- Qt Model 测试（需要 Qt6::Core） ---

find_package(Qt6 QUIET COMPONENTS Core)
if(Qt6_FOUND)
    add_executable(test_friendlistmodel
        test_friendlistmodel.cpp
        ${PROJECT_SOURCE_DIR}/qt_client/src/model/friendlistmodel.cpp
    )
    target_link_libraries(test_friendlistmodel ${COMMON_LIBS} Qt6::Core)
    target_include_directories(test_friendlistmodel PRIVATE
        ${PROJECT_SOURCE_DIR}/qt_client/include
        ${PROJECT_SOURCE_DIR}/qt_client/include/model
    )
else()
    message(STATUS "Qt6 not found — skipping test_friendlistmodel")
endif()

# --- 注册到 CTest ---
enable_testing()
foreach(test_name test_rate_limiter test_jsoncodec test_protocodec test_password_utils test_session_manager test_message_decoder test_message_router test_pending_delivery_manager test_friendlistmodel test_cluster)
    if(TARGET ${test_name})
        add_test(NAME ${test_name} COMMAND $<TARGET_FILE:${test_name}>)
    endif()
endforeach()

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_reliable_client
        COMMAND ${Python3_EXECUTABLE} -m unittest discover -s tests -p test_reliable_client.py)
    set_tests_properties(test_reliable_client PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
endif()
