# Dockerfile — reproduce Linux CI test_icmp hang
FROM ubuntu:22.04

RUN apt-get update -qq && apt-get install -y -qq \
    cmake \
    gcc \
    git \
    strace \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN git submodule update --init --recursive

RUN cmake -B build -DCMAKE_BUILD_TYPE=Debug \
    && cmake --build build -j$(nproc)

# Run test_icmp with strace on timer/IO syscalls
CMD ["sh", "-c", "\
    echo '=== test_icmp ===' && \
    timeout 6 strace -f -e trace=epoll_ctl,epoll_wait,clock_nanosleep,clock_gettime,sendto,recvmsg \
        ./build/tests/test_icmp 2>&1 | head -80; \
    echo 'exit='$?; echo '---'; \
    timeout 6 strace -c ./build/tests/test_icmp 2>&1 \
"]
