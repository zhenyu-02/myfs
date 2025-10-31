#!/bin/bash
# MYFS 单机测试脚本（正确的验证逻辑）
# 关键改进：验证数据真的分布到了远程节点，而不是只验证FUSE能读写

set -e

echo "=========================================="
echo "MYFS 正确验证逻辑测试"
echo "=========================================="

# 配置
MYFS_DIR=~/myfs-zy/fuse-tutorial-2018-02-04
SRC_DIR=$MYFS_DIR/src
LOG_FILE=$MYFS_DIR/example/bbfs.log

# 清理
echo -e "\n[1] 清理环境..."
fusermount -u ~/myfs_mount 2>/dev/null || true
pkill -f "server 800" 2>/dev/null || true
sleep 1

rm -rf ~/storage_node{1,2,3} ~/myfs_root ~/myfs_mount
mkdir -p ~/storage_node{1,2,3} ~/myfs_root ~/myfs_mount
rm -f $LOG_FILE

echo "✓ 清理完成"

# 编译
echo -e "\n[2] 构建项目（autogen -> configure -> make）..."
cd $MYFS_DIR

# 2.1 运行 autogen.sh（生成 configure 脚本）
if [ ! -f "configure" ] || [ "configure.ac" -nt "configure" ]; then
    echo "  运行 autogen.sh..."
    ./autogen.sh > /tmp/autogen.log 2>&1
    if [ $? -ne 0 ]; then
        echo "✗ autogen.sh 失败，查看日志："
        tail -20 /tmp/autogen.log
        exit 1
    fi
    echo "  ✓ autogen.sh 完成"
fi

# 2.2 运行 configure（生成 Makefile）
if [ ! -f "Makefile" ] || [ "configure" -nt "Makefile" ]; then
    echo "  运行 configure..."
    ./configure > /tmp/configure.log 2>&1
    if [ $? -ne 0 ]; then
        echo "✗ configure 失败，查看日志："
        tail -20 /tmp/configure.log
        exit 1
    fi
    echo "  ✓ configure 完成"
fi

# 2.3 运行 make（编译源代码）
echo "  运行 make..."
make > /tmp/make.log 2>&1
if [ $? -ne 0 ]; then
    echo "✗ make 失败，查看日志："
    tail -30 /tmp/make.log
    exit 1
fi
echo "  ✓ make 完成"

# 2.4 验证可执行文件
if [ ! -f "$SRC_DIR/bbfs" ]; then
    echo "✗ bbfs 可执行文件不存在"
    exit 1
fi
if [ ! -f "$SRC_DIR/server" ]; then
    echo "✗ server 可执行文件不存在"
    exit 1
fi

echo "✓ 编译完成 (bbfs, server)"

# 启动服务器
echo -e "\n[3] 启动存储节点..."
cd $MYFS_DIR
./src/server 8001 ~/storage_node1 &
SERVER1_PID=$!
./src/server 8002 ~/storage_node2 &
SERVER2_PID=$!
./src/server 8003 ~/storage_node3 &
SERVER3_PID=$!
sleep 2

# 验证3个服务器都在运行
SERVER_COUNT=$(ps aux | grep "[s]erver 800" | wc -l)
if [ $SERVER_COUNT -ne 3 ]; then
    echo "✗ 期望3个服务器，实际运行 $SERVER_COUNT 个"
    exit 1
fi
echo "✓ 3个存储节点都在运行"
echo "  Node 1: PID $SERVER1_PID (端口 8001)"
echo "  Node 2: PID $SERVER2_PID (端口 8002)"
echo "  Node 3: PID $SERVER3_PID (端口 8003)"

# 挂载MYFS
echo -e "\n[4] 挂载MYFS..."
./src/bbfs ~/myfs_root ~/myfs_mount 127.0.0.1:8001 127.0.0.1:8002 127.0.0.1:8003 > /tmp/myfs_mount.log 2>&1 &
BBFS_PID=$!
sleep 2

if ! mount | grep -q myfs_mount; then
    echo "✗ MYFS挂载失败"
    tail -20 /tmp/myfs_mount.log
    exit 1
fi
echo "✓ MYFS挂载成功 (PID: $BBFS_PID)"

echo -e "\n=========================================="
echo "开始验证测试"
echo "=========================================="

# ============================================================
# 测试1：小文件写入 - 验证片段分布
# ============================================================
echo -e "\n[测试1] 小文件写入与片段验证"
echo "----------------------------------------"

# 1.1 记录写入前的节点状态
echo "写入前，各节点目录内容："
NODE1_BEFORE=$(ls -A ~/storage_node1/ | wc -l)
NODE2_BEFORE=$(ls -A ~/storage_node2/ | wc -l)
NODE3_BEFORE=$(ls -A ~/storage_node3/ | wc -l)
echo "  Node 1: $NODE1_BEFORE 个文件"
echo "  Node 2: $NODE2_BEFORE 个文件"
echo "  Node 3: $NODE3_BEFORE 个文件"

# 1.2 写入测试文件
TEST_CONTENT="Hello MYFS! This is a distributed file system test."
echo "写入内容: $TEST_CONTENT"
echo "$TEST_CONTENT" > ~/myfs_mount/test.txt
sync  # 强制刷新缓冲区
sleep 1

# 1.3 验证节点上产生了新文件
echo -e "\n写入后，各节点目录内容："
NODE1_AFTER=$(ls -A ~/storage_node1/ | wc -l)
NODE2_AFTER=$(ls -A ~/storage_node2/ | wc -l)
NODE3_AFTER=$(ls -A ~/storage_node3/ | wc -l)
echo "  Node 1: $NODE1_AFTER 个文件"
echo "  Node 2: $NODE2_AFTER 个文件"
echo "  Node 3: $NODE3_AFTER 个文件"

# 验证每个节点都产生了新文件
if [ $NODE1_AFTER -le $NODE1_BEFORE ]; then
    echo "✗ Node 1 没有产生新文件！"
    exit 1
fi
if [ $NODE2_AFTER -le $NODE2_BEFORE ]; then
    echo "✗ Node 2 没有产生新文件！"
    exit 1
fi
if [ $NODE3_AFTER -le $NODE3_BEFORE ]; then
    echo "✗ Node 3 没有产生新文件！"
    exit 1
fi

echo "✓ 所有节点都产生了新文件"

# 1.4 显示片段详细信息
echo -e "\n片段详细信息："
echo "Node 1 (数据片段):"
ls -lh ~/storage_node1/ | tail -n +2 | awk '{printf "  %s %s %s\n", $5, $9, $6" "$7" "$8}'

echo "Node 2 (数据片段):"
ls -lh ~/storage_node2/ | tail -n +2 | awk '{printf "  %s %s %s\n", $5, $9, $6" "$7" "$8}'

echo "Node 3 (校验片段 - Parity):"
ls -lh ~/storage_node3/ | tail -n +2 | awk '{printf "  %s %s %s\n", $5, $9, $6" "$7" "$8}'

# 1.5 验证能从挂载点读回（这个是次要的）
READ_CONTENT=$(cat ~/myfs_mount/test.txt)
if [ "$READ_CONTENT" = "$TEST_CONTENT" ]; then
    echo -e "\n✓ 从挂载点读回内容正确"
else
    echo -e "\n✗ 读回内容不匹配！"
    echo "  期望: $TEST_CONTENT"
    echo "  实际: $READ_CONTENT"
    echo ""
    echo "调试信息："
    echo "  查看节点1数据: cat ~/storage_node1/test.txt.frag0"
    echo "  查看节点2数据: cat ~/storage_node2/test.txt.frag1"
    echo "  查看节点3数据: cat ~/storage_node3/test.txt.frag2"
    echo "  查看MYFS日志: tail -50 ~/myfs-zy/fuse-tutorial-2018-02-04/bbfs.log"
    echo "  查看挂载日志: tail -50 /tmp/myfs_mount.log"
    exit 1
fi

echo "✓ 测试1通过：文件确实分布到了3个节点"

# ============================================================
# 测试2：1MB文件 - 验证分片与数据完整性
# ============================================================
echo -e "\n[测试2] 1MB文件分片验证"
echo "----------------------------------------"

# 2.1 创建测试文件
echo "创建1MB随机数据..."
dd if=/dev/urandom of=/tmp/1mb.dat bs=1M count=1 2>/dev/null
ORIGINAL_MD5=$(md5sum /tmp/1mb.dat | awk '{print $1}')
ORIGINAL_SIZE=$(stat -f%z /tmp/1mb.dat 2>/dev/null || stat -c%s /tmp/1mb.dat)
echo "原始文件: 大小=$ORIGINAL_SIZE bytes, MD5=$ORIGINAL_MD5"

# 2.2 记录节点状态
NODE1_FILES_BEFORE=$(ls ~/storage_node1/ | wc -l)
NODE2_FILES_BEFORE=$(ls ~/storage_node2/ | wc -l)
NODE3_FILES_BEFORE=$(ls ~/storage_node3/ | wc -l)

# 2.3 写入MYFS
echo "写入到MYFS..."
cp /tmp/1mb.dat ~/myfs_mount/
sync
sleep 1

# 2.4 验证新文件产生
NODE1_FILES_AFTER=$(ls ~/storage_node1/ | wc -l)
NODE2_FILES_AFTER=$(ls ~/storage_node2/ | wc -l)
NODE3_FILES_AFTER=$(ls ~/storage_node3/ | wc -l)

echo "新增文件数："
echo "  Node 1: +$(($NODE1_FILES_AFTER - $NODE1_FILES_BEFORE))"
echo "  Node 2: +$(($NODE2_FILES_AFTER - $NODE2_FILES_BEFORE))"
echo "  Node 3: +$(($NODE3_FILES_AFTER - $NODE3_FILES_BEFORE))"

# 2.5 分析片段大小
echo -e "\n片段大小分析："
echo "Node 1 最新文件："
ls -lh ~/storage_node1/ | tail -1 | awk '{print "  大小:", $5, "文件:", $9}'

echo "Node 2 最新文件："
ls -lh ~/storage_node2/ | tail -1 | awk '{print "  大小:", $5, "文件:", $9}'

echo "Node 3 (Parity) 最新文件："
ls -lh ~/storage_node3/ | tail -1 | awk '{print "  大小:", $5, "文件:", $9}'

# 2.6 读回并验证MD5
echo -e "\n从MYFS读回并验证..."
READ_MD5=$(md5sum ~/myfs_mount/1mb.dat | awk '{print $1}')

if [ "$ORIGINAL_MD5" = "$READ_MD5" ]; then
    echo "✓ MD5校验通过: $READ_MD5"
else
    echo "✗ MD5校验失败！"
    echo "  原始: $ORIGINAL_MD5"
    echo "  读回: $READ_MD5"
    echo ""
    echo "调试信息："
    echo "  查看节点文件列表: ls -lh ~/storage_node{1,2,3}/"
    echo "  查看最新片段: ls -lht ~/storage_node1/ | head -5"
    echo "  查看MYFS日志: tail -100 ~/myfs-zy/fuse-tutorial-2018-02-04/bbfs.log"
    echo "  查看挂载日志: tail -100 /tmp/myfs_mount.log"
    echo "  对比原始文件: md5sum /tmp/1mb.dat ~/myfs_mount/1mb.dat"
    exit 1
fi

echo "✓ 测试2通过：1MB文件正确分片并能完整读回"

# ============================================================
# 测试3：容错读取 - 真实验证XOR恢复
# ============================================================
echo -e "\n[测试3] 容错读取验证（XOR恢复）"
echo "----------------------------------------"

echo "当前test.txt的MD5："
ORIGINAL_TEST_MD5=$(md5sum ~/myfs_mount/test.txt | awk '{print $1}')
echo "  $ORIGINAL_TEST_MD5"

echo "当前1mb.dat的MD5："
ORIGINAL_1MB_MD5=$(md5sum ~/myfs_mount/1mb.dat | awk '{print $1}')
echo "  $ORIGINAL_1MB_MD5"

# 3.1 关闭Node 2
echo -e "\n关闭Node 2 (模拟节点失效)..."
kill $SERVER2_PID
sleep 2

# 验证Node 2确实关闭
if ps -p $SERVER2_PID > /dev/null 2>&1; then
    echo "✗ Node 2 仍在运行"
    exit 1
fi
echo "✓ Node 2 已停止"

# 3.2 卸载并重新挂载（强制从节点读取）
echo -e "\n卸载并重新挂载MYFS（强制重新从节点读取）..."
fusermount -u ~/myfs_mount
sleep 1

./src/bbfs ~/myfs_root ~/myfs_mount 127.0.0.1:8001 127.0.0.1:8002 127.0.0.1:8003 > /tmp/myfs_mount.log 2>&1 &
BBFS_PID=$!
sleep 2

if ! mount | grep -q myfs_mount; then
    echo "✗ 重新挂载失败"
    exit 1
fi
echo "✓ 重新挂载成功"

# 3.3 尝试读取小文件
echo -e "\n读取test.txt（应通过Node 1 + Node 3恢复Node 2的数据）..."
RECOVERED_TEST_MD5=$(md5sum ~/myfs_mount/test.txt 2>/dev/null | awk '{print $1}')

if [ -z "$RECOVERED_TEST_MD5" ]; then
    echo "✗ 读取失败！容错机制未生效"
    echo ""
    echo "调试信息："
    echo "  查看MYFS日志: tail -50 ~/myfs-zy/fuse-tutorial-2018-02-04/bbfs.log"
    echo "  查看挂载日志: tail -50 /tmp/myfs_mount.log"
    echo "  查看存活节点: ps aux | grep '[s]erver 800'"
    echo "  查看Node 1片段: cat ~/storage_node1/test.txt.frag0"
    echo "  查看Node 3片段: xxd ~/storage_node3/test.txt.frag2 | head -5"
    tail -30 /tmp/myfs_mount.log
    exit 1
fi

if [ "$RECOVERED_TEST_MD5" = "$ORIGINAL_TEST_MD5" ]; then
    echo "✓ XOR恢复成功！MD5: $RECOVERED_TEST_MD5"
else
    echo "✗ XOR恢复的数据不正确！"
    echo "  原始: $ORIGINAL_TEST_MD5"
    echo "  恢复: $RECOVERED_TEST_MD5"
    echo ""
    echo "调试信息："
    echo "  查看恢复的内容: cat ~/myfs_mount/test.txt | hexdump -C | head -5"
    echo "  查看Node 1片段: cat ~/storage_node1/test.txt.frag0"
    echo "  查看Node 3片段: xxd ~/storage_node3/test.txt.frag2 | head -5"
    echo "  查看MYFS日志: tail -50 ~/myfs-zy/fuse-tutorial-2018-02-04/bbfs.log"
    exit 1
fi

# 3.4 尝试读取大文件
echo -e "\n读取1mb.dat（Node 2失效情况下）..."
RECOVERED_1MB_MD5=$(md5sum ~/myfs_mount/1mb.dat 2>/dev/null | awk '{print $1}')

if [ "$RECOVERED_1MB_MD5" = "$ORIGINAL_1MB_MD5" ]; then
    echo "✓ 大文件XOR恢复成功！MD5: $RECOVERED_1MB_MD5"
else
    echo "✗ 大文件XOR恢复失败！"
    echo "  原始: $ORIGINAL_1MB_MD5"
    echo "  恢复: $RECOVERED_1MB_MD5"
    echo ""
    echo "调试信息："
    echo "  查看片段大小: ls -lh ~/storage_node{1,3}/1mb.dat.frag*"
    echo "  验证原始文件: md5sum /tmp/1mb.dat"
    echo "  验证恢复文件: md5sum ~/myfs_mount/1mb.dat"
    echo "  查看MYFS日志: tail -100 ~/myfs-zy/fuse-tutorial-2018-02-04/bbfs.log | grep -A 5 'MYFS READ'"
    exit 1
fi

echo "✓ 测试3通过：容错机制正常工作"

# ============================================================
# 测试4：4MB文件（需要重启Node 2用于写入）
# ============================================================
read -p $'\n[测试4] 运行4MB文件测试？(y/n) ' -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "重启Node 2..."
    cd $MYFS_DIR
    ./src/server 8002 ~/storage_node2 &
    SERVER2_PID=$!
    sleep 3
    
    if ! ps -p $SERVER2_PID > /dev/null; then
        echo "✗ Node 2 启动失败"
        exit 1
    fi
    echo "✓ Node 2 已重启 (PID: $SERVER2_PID)"
    
    # 卸载并重新挂载
    fusermount -u ~/myfs_mount
    sleep 1
    ./src/bbfs ~/myfs_root ~/myfs_mount 127.0.0.1:8001 127.0.0.1:8002 127.0.0.1:8003 > /tmp/myfs_mount.log 2>&1 &
    sleep 2
    
    echo "创建4MB文件..."
    dd if=/dev/urandom of=/tmp/4mb.dat bs=1M count=4 2>/dev/null
    ORIGINAL_4MB_MD5=$(md5sum /tmp/4mb.dat | awk '{print $1}')
    
    echo "写入MYFS..."
    time cp /tmp/4mb.dat ~/myfs_mount/
    sync
    sleep 1
    
    echo "读回并验证..."
    READ_4MB_MD5=$(md5sum ~/myfs_mount/4mb.dat | awk '{print $1}')
    
    if [ "$ORIGINAL_4MB_MD5" = "$READ_4MB_MD5" ]; then
        echo "✓ 4MB文件测试通过！MD5: $READ_4MB_MD5"
    else
        echo "✗ 4MB文件数据损坏"
        echo "  原始: $ORIGINAL_4MB_MD5"
        echo "  读回: $READ_4MB_MD5"
        exit 1
    fi
    
    # 显示片段分布
    echo -e "\n片段数统计："
    echo "  Node 1: $(ls ~/storage_node1/ | wc -l) 个文件"
    echo "  Node 2: $(ls ~/storage_node2/ | wc -l) 个文件"
    echo "  Node 3: $(ls ~/storage_node3/ | wc -l) 个文件"
fi

echo -e "\n=========================================="
echo "所有测试通过！"
echo "=========================================="
echo ""
echo "关键验证点："
echo "  ✓ 文件确实分布到了3个独立的存储节点"
echo "  ✓ 每个节点都收到了数据片段"
echo "  ✓ 数据完整性通过MD5验证"
echo "  ✓ 单节点失效时能通过XOR恢复数据"
echo "  ✓ 恢复的数据与原始数据完全一致"
echo ""
echo "清理命令："
echo "  fusermount -u ~/myfs_mount"
echo "  pkill -f 'server 800'"
echo ""

