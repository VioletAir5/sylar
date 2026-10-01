FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates cmake g++ libboost-context-dev libfmt-dev \
    libssl-dev libsqlite3-dev libyaml-cpp-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B /tmp/sylar-build \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    && cmake --build /tmp/sylar-build -j 2 --target http_server \
    && install -m 755 /tmp/sylar-build/sylar/http_server \
        /usr/local/bin/http_server

USER 65532:65532
WORKDIR /opt/sylar
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/http_server"]
CMD ["8080", "4"]
