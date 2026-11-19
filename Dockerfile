# MiniVectorDB 评测镜像
# 固定 Ubuntu 24.04 基础镜像；编译依赖只在 build 阶段安装，运行镜像不联网。
# 最终程序通过 stdin/stdout 交互，不使用 -d / -t，也不 EXPOSE 任何端口。

FROM ubuntu:24.04 AS build

ARG DEBIAN_FRONTEND=noninteractive

# 编译工具链在构建阶段一次性安装并清理 apt 索引。
# build-essential 提供 g++/make 及标准 C++ 开发头文件；显式列出 g++、make，
# 保证工具链入口与 Makefile 的 CXX=g++ 约定一致。
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        g++ \
        make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY Makefile ./
# 只复制源码和头文件，避免把宿主机上已编译的 .o 带入 Linux 构建阶段。
COPY include/*.h ./include/
COPY src/*.cpp ./src/

# Makefile 规则：g++ -std=c++17 -O3 -Iinclude src/*.cpp -> ./minivectordb
RUN make -j"$(nproc)" CXX=g++ \
    && test -x ./minivectordb

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

# 仅保留运行 C++ 程序所需的 libstdc++6；不安装编译器，运行容器不联网。
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /data

WORKDIR /app
COPY --from=build /build/minivectordb /usr/local/bin/minivectordb

# 数据、WAL、索引等所有持久化文件统一写入 /data。
VOLUME ["/data"]

# stdout 必须只承载评测结果，日志由程序写入 stderr。
# 前台运行，保持 stdin 打开，交由测试驱动逐条发送 SQL。
CMD ["minivectordb", "--data-dir", "/data"]
