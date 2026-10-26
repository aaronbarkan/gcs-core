FROM debian:12-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config \
    libgrpc++-dev protobuf-compiler-grpc \
    libprotobuf-dev protobuf-compiler libboost-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j2 \
 && ctest --test-dir build --output-on-failure

FROM debian:12-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgrpc++1.51 libprotobuf32 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/gcs-core /usr/local/bin/gcs-core
EXPOSE 14550/udp
EXPOSE 50051/tcp
CMD ["/usr/local/bin/gcs-core"]
