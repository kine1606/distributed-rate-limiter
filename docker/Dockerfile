# Multi-stage build of the limiter. -march=native is OFF: the image must run on any x86-64 host.
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config libgrpc++-dev protobuf-compiler-grpc \
    libprotobuf-dev libspdlog-dev libyaml-cpp-dev libgtest-dev libbenchmark-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DRL_NATIVE=OFF \
        -DRL_BUILD_TESTS=OFF -DRL_BUILD_BENCH=OFF \
    && cmake --build build --target rate_limiter_server

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgrpc++1.51t64 libprotobuf32t64 libspdlog1.12 libyaml-cpp0.8 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/rate_limiter_server /usr/local/bin/rate_limiter_server
COPY config /etc/ratelimiter
EXPOSE 50051
ENTRYPOINT ["rate_limiter_server"]
CMD ["0.0.0.0:50051", "/etc/ratelimiter/rules.yaml"]
