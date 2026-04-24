# 收集当前目录及子目录源文件
aux_source_directory(. SERVER_SRC)
aux_source_directory(./db DB_SRC)
aux_source_directory(./model MODEL_SRC)
aux_source_directory(./redis REDIS_SRC)

# protobuf 生成文件
set(PROTO_SRC
    ${PROJECT_SOURCE_DIR}/build/generated/proto/chat.pb.cc
)

# 生成服务端可执行文件
add_executable(ChatServer
    ${SERVER_SRC}
    ${DB_SRC}
    ${MODEL_SRC}
    ${REDIS_SRC}
    ${PROTO_SRC}
)

# 链接依赖库
target_link_libraries(ChatServer
    muduo_net
    muduo_base
    mysqlclient
    hiredis
    pthread
    ${Protobuf_LIBRARIES}
)