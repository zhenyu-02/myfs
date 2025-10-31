#!/bin/bash
# MYFS 单机测试脚本

set -e  # Exit on error

echo "=========================================="
echo "MYFS Single VM Test Script"
echo "=========================================="

# 配置
MYFS_DIR=~/myfs-zy/fuse-tutorial-2018-02-04
SRC_DIR=$MYFS_DIR/src

# 清理之前的运行
echo -e "\n[1] Cleaning up previous runs..."
fusermount -u ~/myfs_mount 2>/dev/null || true
pkill -f "server 800" 2>/dev/null || true
sleep 1

# 清理并重建目录
rm -rf ~/storage_node{1,2,3} ~/myfs_root ~/myfs_mount
mkdir -p ~/storage_node{1,2,3} ~/myfs_root ~/myfs_mount

echo "✓ Cleanup complete"

# 编译
echo -e "\n[2] Compiling..."
cd $SRC_DIR
gcc -o bbfs bbfs.c log.c -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=26 \
    $(pkg-config fuse --cflags --libs) -lpthread
gcc -o server server.c -lpthread
echo "✓ Compilation complete"

# 启动服务器
echo -e "\n[3] Starting storage servers..."
cd $MYFS_DIR
./src/server 8001 ~/storage_node1 &
./src/server 8002 ~/storage_node2 &
./src/server 8003 ~/storage_node3 &
sleep 1

# 检查服务器
echo -e "\n[4] Checking servers..."
ps aux | grep "[s]erver 800"
netstat -tln | grep 800
echo "✓ All servers running"

# 挂载文件系统（后台模式）
echo -e "\n[5] Mounting MYFS..."
./src/bbfs ~/myfs_root ~/myfs_mount 127.0.0.1:8001 127.0.0.1:8002 127.0.0.1:8003 &
sleep 2

# 检查挂载
if mount | grep -q myfs_mount; then
    echo "✓ MYFS mounted successfully"
else
    echo "✗ MYFS mount failed"
    exit 1
fi

# 运行测试
echo -e "\n[6] Running tests..."

# Test 1: 小文件写入
echo "Test 1: Small file write"
echo "Hello MYFS!" > ~/myfs_mount/test.txt
cat ~/myfs_mount/test.txt
echo "✓ Small file test passed"

# Test 2: 检查分片
echo -e "\nTest 2: Check fragments"
echo "Node 1 files:"
ls -lh ~/storage_node1/
echo "Node 2 files:"
ls -lh ~/storage_node2/
echo "Node 3 files (parity):"
ls -lh ~/storage_node3/

# Test 3: 容错测试
echo -e "\nTest 3: Fault tolerance"
echo "Stopping node 2..."
pkill -f "server 8002"
sleep 1
echo "Trying to read test.txt (should succeed with XOR recovery)..."
cat ~/myfs_mount/test.txt && echo "✓ Fault tolerance test passed"

# Test 4: 4 MB 文件（重启node 2后）
echo -e "\nTest 4: 4 MB file"
echo "Restarting node 2 (needed for write operations)..."
cd $MYFS_DIR
./src/server 8002 ~/storage_node2 &
SERVER_PID=$!
echo "Server 2 restarted, PID=$SERVER_PID"
echo "Waiting for server to fully start..."
sleep 3
echo "Creating 4 MB file..."
dd if=/dev/urandom of=/tmp/4mb.dat bs=1M count=4 2>/dev/null
echo "Copying to MYFS..."
time cp /tmp/4mb.dat ~/myfs_mount/
echo "Reading back..."
time cat ~/myfs_mount/4mb.dat > /dev/null && echo "✓ 4 MB file test passed"

# 再次检查分片
echo -e "\nFragments after 4MB file:"
echo "Node 1:"
ls -lh ~/storage_node1/
echo "Node 2:"
ls -lh ~/storage_node2/
echo "Node 3 (parity):"
ls -lh ~/storage_node3/

# Test 5: 40 MB 文件
echo -e "\nTest 5: 40 MB file"
echo "Creating 40 MB file..."
dd if=/dev/urandom of=/tmp/40mb.dat bs=1M count=40 2>/dev/null
echo "Copying to MYFS..."
time cp /tmp/40mb.dat ~/myfs_mount/
echo "Reading back..."
time cat ~/myfs_mount/40mb.dat > /dev/null && echo "✓ 40 MB file test passed"

# Test 6: 400 MB 文件
read -p $'\nTest 6: Run 400 MB file test? (y/n) ' -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "Creating 400 MB file (this may take a minute)..."
    dd if=/dev/urandom of=/tmp/400mb.dat bs=1M count=400 2>/dev/null
    echo "✓ Test file created"
    
    echo "Writing to MYFS (this will take some time)..."
    echo "Start time: $(date '+%H:%M:%S')"
    time cp /tmp/400mb.dat ~/myfs_mount/
    echo "✓ 400 MB file written"
    
    echo "Reading back from MYFS..."
    echo "Start time: $(date '+%H:%M:%S')"
    time cat ~/myfs_mount/400mb.dat > /dev/null
    echo "✓ 400 MB file test passed"
    
    echo -e "\nFinal fragments:"
    echo "Node 1:"
    ls -lh ~/storage_node1/ | tail -3
    echo "Node 2:"
    ls -lh ~/storage_node2/ | tail -3
    echo "Node 3 (parity):"
    ls -lh ~/storage_node3/ | tail -3
    
    # Test 7: 容错测试 - 关闭一个节点后读取 400MB
    read -p $'\nTest 7: Test fault tolerance with 400 MB file? (y/n) ' -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        echo "Stopping node 2 (simulating failure)..."
        pkill -f "server 8002"
        sleep 1
        echo "Trying to read 400mb.dat with node 2 down (using XOR recovery)..."
        echo "Start time: $(date '+%H:%M:%S')"
        time cat ~/myfs_mount/400mb.dat > /dev/null && echo "✓ Fault tolerance with 400 MB file passed"
    fi
fi

echo -e "\n=========================================="
echo "All tests completed!"
echo "=========================================="
echo ""
echo "To view logs: tail -f $MYFS_DIR/example/bbfs.log"
echo "To unmount: fusermount -u ~/myfs_mount"
echo "To stop servers: pkill -f 'server 800'"

