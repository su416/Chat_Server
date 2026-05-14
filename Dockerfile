# 第一阶段：构建 muduo 库
FROM ubuntu:24.04 AS muduo-builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 --branch master \
    https://github.com/chenshuo/muduo.git /tmp/muduo \
    && cd /tmp/muduo \
    && mkdir build && cd build \
    && cmake -DCMAKE_BUILD_TYPE=release -DCMAKE_INSTALL_PREFIX=/usr/local .. \
    && make -j$(nproc) \
    && make install \
    && rm -rf /tmp/muduo

# 第二阶段：构建 ChatServer
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake \
    libmysqlclient-dev libhiredis-dev \
    libprotobuf-dev protobuf-compiler \
    nlohmann-json3-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*

COPY --from=muduo-builder /usr/local/include /usr/local/include
COPY --from=muduo-builder /usr/local/lib /usr/local/lib

WORKDIR /app
COPY . .
RUN rm -rf build && mkdir build && cd build \
    && cmake -DCMAKE_BUILD_TYPE=release .. \
    && make -j$(nproc)

# 第三阶段：运行镜像
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libmysqlclient21 libhiredis0.14 libprotobuf32t64 libssl3t64 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && ldconfig

COPY --from=muduo-builder /usr/local/lib/libmuduo_*.a /usr/local/lib/
COPY --from=builder /app/bin/ChatServer /app/bin/ChatServer
COPY --from=builder /app/conf /app/conf

WORKDIR /app
EXPOSE 6000

CMD ["/app/bin/ChatServer"]
