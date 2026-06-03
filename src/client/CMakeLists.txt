# 收集客户端源文件
aux_source_directory(. CLIENT_SRC)

# 生成客户端可执行文件
add_executable(ChatClient ${CLIENT_SRC})

# 链接依赖库
target_link_libraries(ChatClient pthread protobuf)
