FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends g++ make && rm -rf /var/lib/apt/lists/*
WORKDIR /app

# 源码位于仓库 minivectordb/ 子目录
COPY minivectordb/src/ src/
COPY minivectordb/Makefile minivectordb/CMakeLists.txt ./
RUN make

# 官方协议：JSONL stdin/stdout（见 STDIO_PROTOCOL.md），启动即输出 {"type":"ready"}，stderr 记日志
# 评测环境为 --read-only 根文件系统，程序只在 /data 写文件
ENTRYPOINT ["/app/minivectordb", "--data-dir", "/data"]
