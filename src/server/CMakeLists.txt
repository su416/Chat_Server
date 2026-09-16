# 收集当前目录及子目录源文件
aux_source_directory(. SERVER_SRC)
aux_source_directory(./db DB_SRC)
aux_source_directory(./gateway GATEWAY_SRC)
aux_source_directory(./protocol PROTOCOL_SRC)
aux_source_directory(./model MODEL_SRC)
aux_source_directory(./redis REDIS_SRC)
aux_source_directory(./utils UTILS_SRC)

# protobuf 生成文件
set(PROTO_SRC
    ${PROJECT_SOURCE_DIR}/build/generated/proto/chat.pb.cc
)

# 生成服务端可执行文件
add_executable(ChatServer
    ${SERVER_SRC}
    ${DB_SRC}
    ${GATEWAY_SRC}
    ${PROTOCOL_SRC}
    ${MODEL_SRC}
    ${REDIS_SRC}
    ${UTILS_SRC}
    ${PROTO_SRC}
)

# 链接依赖库
target_link_libraries(ChatServer
    muduo_net
    muduo_base
    mysqlclient
    hiredis
    pthread
    ssl
    crypto
    ${Protobuf_LIBRARIES}
)
