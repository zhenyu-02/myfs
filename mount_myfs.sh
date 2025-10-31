#!/bin/bash
# 挂载 MYFS 文件系统

if [ $# -lt 3 ]; then
    echo "Usage: $0 <rootdir> <mountpoint> <node1:port1> [node2:port2] [node3:port3] ..."
    echo "Example: $0 ~/myfs_root ~/myfs_mount 10.0.1.4:8001 10.0.1.5:8002 10.0.1.6:8003"
    exit 1
fi

ROOTDIR=$1
MOUNTPOINT=$2
shift 2
NODES="$@"

# 创建目录
mkdir -p "$ROOTDIR"
mkdir -p "$MOUNTPOINT"

# 检查挂载点是否已被使用
if mountpoint -q "$MOUNTPOINT"; then
    echo "Error: $MOUNTPOINT is already mounted"
    exit 1
fi

# 挂载文件系统
echo "Mounting MYFS..."
echo "Root dir: $ROOTDIR"
echo "Mount point: $MOUNTPOINT"
echo "Nodes: $NODES"

cd "$(dirname "$0")/fuse-tutorial-2018-02-04"
./src/bbfs "$ROOTDIR" "$MOUNTPOINT" $NODES

echo "MYFS mounted successfully!"
echo "You can now access files at: $MOUNTPOINT"
echo ""
echo "To unmount, run: fusermount -u $MOUNTPOINT"

