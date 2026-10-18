# Multi-stage build of the limiter. -march=native is OFF: the image must run on any x86-64 host.
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config git ca-certificates libgrpc++-dev protobuf-compiler-grpc \
    libprotobuf-dev libspdlog-dev libyaml-cpp-dev libhiredis-dev libuv1-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*
# redis-plus-plus is not packaged by Ubuntu. Static, so the runtime image needs only hiredis and libuv.
RUN git clone -q --depth 1 -b 1.3.13 https://github.com/sewenew/redis-plus-plus.git /tmp/rpp \
    && cmake -S /tmp/rpp -B /tmp/rpp/b -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
         -DREDIS_PLUS_PLUS_BUILD_ASYNC=libuv -DREDIS_PLUS_PLUS_CXX_STANDARD=20 -DREDIS_PLUS_PLUS_BUILD_TEST=OFF \
         -DREDIS_PLUS_PLUS_BUILD_SHARED=OFF \
    && cmake --build /tmp/rpp/b --target install && rm -rf /tmp/rpp
WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DRL_NATIVE=OFF \
        -DRL_BUILD_TESTS=OFF -DRL_BUILD_BENCH=OFF \
    && cmake --build build --target rate_limiter_server

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgrpc++1.51t64 libprotobuf32t64 libspdlog1.12 libyaml-cpp0.8 libhiredis1.1.0 libuv1t64 libssl3t64 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/rate_limiter_server /usr/local/bin/rate_limiter_server
COPY config /etc/ratelimiter
COPY lua /usr/share/ratelimiter/lua
EXPOSE 50051
ENTRYPOINT ["rate_limiter_server"]
CMD ["0.0.0.0:50051", "/etc/ratelimiter/rules.yaml", "redis-1:6379", "/usr/share/ratelimiter/lua"]
