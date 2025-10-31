#!/bin/bash
# 启动 MYFS 存储节点服务器

if [ $# -ne 2 ]; then
    echo "Usage: $0 <port> <storage_dir>"
    echo "Example: $0 8001 ~/storage_node1"
    exit 1
fi

PORT=$1
STORAGE_DIR=$2

# 创建存储目录
mkdir -p "$STORAGE_DIR"

# 启动服务器
echo "Starting MYFS storage server on port $PORT, storage dir: $STORAGE_DIR"
cd "$(dirname "$0")/fuse-tutorial-2018-02-04"
./src/server "$PORT" "$STORAGE_DIR"

