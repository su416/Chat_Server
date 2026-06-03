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
foreach(test_name test_rate_limiter test_jsoncodec test_protocodec test_password_utils test_friendlistmodel)
    if(TARGET ${test_name})
        add_test(NAME ${test_name} COMMAND $<TARGET_FILE:${test_name}>)
    endif()
endforeach()
