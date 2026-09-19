FROM ubuntu:24.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake curl ca-certificates libboost-dev \
    default-libmysqlclient-dev libhiredis-dev libgtest-dev \
    libprotobuf-dev protobuf-compiler nlohmann-json3-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*

ARG MUDUO_VERSION=v2.0.2
ARG MUDUO_SHA256=184d76c003b6b0d3081c35bd683feb708ac4fd48d2f93a23944817bb4d83ca60
RUN curl -fL --retry 5 --retry-all-errors --connect-timeout 15 --max-time 180 \
      -o /tmp/muduo.tar.gz \
      "https://codeload.github.com/chenshuo/muduo/tar.gz/refs/tags/${MUDUO_VERSION}" \
    && echo "${MUDUO_SHA256}  /tmp/muduo.tar.gz" | sha256sum -c - \
    && mkdir /tmp/muduo \
    && tar -xzf /tmp/muduo.tar.gz --strip-components=1 -C /tmp/muduo \
    && cmake -S /tmp/muduo -B /tmp/muduo/build \
       -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
       -DMUDUO_BUILD_EXAMPLES=OFF \
    && cmake --build /tmp/muduo/build -j$(nproc) \
    && cmake --install /tmp/muduo/build

WORKDIR /app
COPY . .
RUN ./script/build.sh

FROM ubuntu:24.04 AS runtime

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates default-libmysqlclient-dev libhiredis-dev \
    libprotobuf-dev libssl-dev netcat-openbsd \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /app/bin/ChatServer /app/bin/ChatServer
COPY --from=builder /app/conf/chatserver.example.json /app/conf/chatserver.json

WORKDIR /app
EXPOSE 6000
HEALTHCHECK --interval=5s --timeout=3s --retries=12 \
    CMD nc -z 127.0.0.1 "${CHAT_SERVER_PORT:-6000}" || exit 1

CMD ["/app/bin/ChatServer", "/app/conf/chatserver.json"]
