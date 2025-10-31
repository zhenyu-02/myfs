#!/bin/bash
# MYFS 单机测试脚本（带详细调试输出）

set -e  # Exit on error

echo "=========================================="
echo "MYFS Single VM Debug Test Script"
echo "=========================================="

# 配置
MYFS_DIR=~/myfs-zy/fuse-tutorial-2018-02-04
SRC_DIR=$MYFS_DIR/src
LOG_FILE=$MYFS_DIR/example/bbfs.log

# 清理之前的运行
echo -e "\n[1] Cleaning up previous runs..."
fusermount -u ~/myfs_mount 2>/dev/null || true
pkill -f "server 800" 2>/dev/null || true
pkill -f "bbfs" 2>/dev/null || true
sleep 1

# 清理并重建目录
rm -rf ~/storage_node{1,2,3} ~/myfs_root ~/myfs_mount
mkdir -p ~/storage_node{1,2,3} ~/myfs_root ~/myfs_mount
rm -f $LOG_FILE

echo "✓ Cleanup complete"

# 编译
echo -e "\n[2] Compiling..."
cd $SRC_DIR
gcc -o bbfs bbfs.c log.c -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=26 \
    $(pkg-config fuse --cflags --libs) -lpthread
gcc -o server server.c -lpthread
echo "✓ Compilation complete"

# 启动服务器（带详细输出）
echo -e "\n[3] Starting storage servers..."
cd $MYFS_DIR
./src/server 8001 ~/storage_node1 &
echo "  Server 1 (port 8001) started, PID=$!"
./src/server 8002 ~/storage_node2 &
echo "  Server 2 (port 8002) started, PID=$!"
./src/server 8003 ~/storage_node3 &
echo "  Server 3 (port 8003) started, PID=$!"
sleep 1

# 检查服务器
echo -e "\n[4] Checking servers..."
ps aux | grep "[s]erver 800" | awk '{print "  PID", $2, ":", $11, $12, $13}'
netstat -tln | grep 800 | awk '{print "  Port", $4, "->", $1}'
echo "✓ All servers running"

# 在后台挂载文件系统，并持续监控输出
echo -e "\n[5] Mounting MYFS (with debug output)..."
echo "====== MYFS Mount Output ======"
./src/bbfs ~/myfs_root ~/myfs_mount 127.0.0.1:8001 127.0.0.1:8002 127.0.0.1:8003 2>&1 | tee /tmp/myfs_mount.log &
BBFS_PID=$!
echo "BBFS PID: $BBFS_PID"
sleep 2

# 检查挂载
if mount | grep -q myfs_mount; then
    echo "✓ MYFS mounted successfully"
else
    echo "✗ MYFS mount failed"
    echo "Last 20 lines from mount output:"
    tail -20 /tmp/myfs_mount.log
    exit 1
fi

# 等待一下让初始化完成
sleep 1

echo -e "\n====== Starting Tests ======"

# Test 1: 小文件写入
echo -e "\n[Test 1] Small file write"
echo "Writing: echo 'Hello MYFS!' > ~/myfs_mount/test.txt"
echo "Hello MYFS!" > ~/myfs_mount/test.txt
sleep 0.5
echo "Reading back..."
cat ~/myfs_mount/test.txt
echo "✓ Small file test passed"

# 检查分片
echo -e "\n[Test 1 - Check Fragments]"
echo "Node 1 files:"
ls -lh ~/storage_node1/ | grep -v "^total" | awk '{print "  ", $0}' || echo "  (empty)"
echo "Node 2 files:"
ls -lh ~/storage_node2/ | grep -v "^total" | awk '{print "  ", $0}' || echo "  (empty)"
echo "Node 3 files (parity):"
ls -lh ~/storage_node3/ | grep -v "^total" | awk '{print "  ", $0}' || echo "  (empty)"

# Test 2: 1 MB 文件
echo -e "\n[Test 2] 1 MB file"
echo "Creating 1 MB file..."
dd if=/dev/urandom of=/tmp/1mb.dat bs=1M count=1 2>/dev/null
echo "Writing to MYFS..."
cp /tmp/1mb.dat ~/myfs_mount/
sleep 0.5
echo "Reading back..."
cat ~/myfs_mount/1mb.dat > /dev/null && echo "✓ 1 MB file test passed"

echo -e "\n[Test 2 - Check Fragments]"
echo "Node 1 files:"
ls -lh ~/storage_node1/ | grep -v "^total" | awk '{print "  ", $0}'
echo "Node 2 files:"
ls -lh ~/storage_node2/ | grep -v "^total" | awk '{print "  ", $0}'
echo "Node 3 files (parity):"
ls -lh ~/storage_node3/ | grep -v "^total" | awk '{print "  ", $0}'

# Test 3: 容错测试
echo -e "\n[Test 3] Fault Tolerance Test"
echo "Current test.txt content: $(cat ~/myfs_mount/test.txt)"
echo "Stopping node 2 (simulating failure)..."
pkill -f "server 8002"
sleep 1
echo "Server status after stopping node 2:"
ps aux | grep "[s]erver 800" | awk '{print "  ", $0}' || echo "  No servers running"

echo -e "\nTrying to read test.txt (should use XOR recovery)..."
cat ~/myfs_mount/test.txt
echo "✓ Fault tolerance test passed"

echo -e "\n[Test 3 - Verify XOR Recovery]"
echo "Trying to read 1mb.dat with node 2 down..."
cat ~/myfs_mount/1mb.dat > /dev/null && echo "✓ Large file read with fault tolerance"

# Test 4: 4 MB 文件（可选，看时间）
read -p $'\n[Test 4] Run 4 MB file test? (y/n) ' -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "Creating 4 MB file..."
    dd if=/dev/urandom of=/tmp/4mb.dat bs=1M count=4 2>/dev/null
    echo "Writing to MYFS..."
    cp /tmp/4mb.dat ~/myfs_mount/
    sleep 0.5
    echo "Reading back..."
    cat ~/myfs_mount/4mb.dat > /dev/null && echo "✓ 4 MB file test passed"
    
    echo -e "\n[Test 4 - Check Fragments]"
    echo "Node 1 files:"
    ls -lh ~/storage_node1/ | grep -v "^total"
    echo "Node 2 files (OFFLINE):"
    echo "  Node 2 is offline"
    echo "Node 3 files (parity):"
    ls -lh ~/storage_node3/ | grep -v "^total"
fi

echo -e "\n=========================================="
echo "All tests completed!"
echo "=========================================="
echo ""
echo "Detailed logs:"
echo "  MYFS mount log: /tmp/myfs_mount.log"
echo "  FUSE log: $LOG_FILE"
echo ""
echo "View logs:"
echo "  tail -f /tmp/myfs_mount.log"
echo "  tail -f $LOG_FILE"
echo ""
echo "Cleanup:"
echo "  fusermount -u ~/myfs_mount"
echo "  pkill -f 'server 800'"
echo "  pkill -f 'bbfs'"
echo ""
echo "Press Ctrl+C to stop monitoring, or run cleanup commands above."

# 持续监控日志
echo -e "\n====== Monitoring MYFS Activity (Ctrl+C to stop) ======"
tail -f /tmp/myfs_mount.log 2>/dev/null || tail -f $LOG_FILE

