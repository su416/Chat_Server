# 收集客户端源文件
aux_source_directory(. CLIENT_SRC)

# protobuf 生成文件
set(PROTO_SRC
    ${PROJECT_SOURCE_DIR}/build/generated/proto/chat.pb.cc
)

# 生成客户端可执行文件
add_executable(ChatClient ${CLIENT_SRC} ${PROTO_SRC})

# 链接依赖库
target_link_libraries(ChatClient pthread protobuf)
