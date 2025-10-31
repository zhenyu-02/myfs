#!/bin/bash
# 演示 MYFS 容错功能

MOUNTPOINT="$HOME/myfs_mount"

if [ ! -d "$MOUNTPOINT" ]; then
    echo "Error: Mount point $MOUNTPOINT does not exist"
    echo "Please mount MYFS first"
    exit 1
fi

echo "=== MYFS Fault Tolerance Demo ==="
echo ""
echo "This script demonstrates MYFS's ability to recover data"
echo "even when one storage node is offline."
echo ""

# 步骤 1: 创建测试文件
echo "Step 1: Creating test file with all nodes online..."
TEST_FILE="$MOUNTPOINT/fault_test.txt"
echo "This file will survive a node failure!" > "$TEST_FILE"
echo "Test data: $(date)" >> "$TEST_FILE"
echo "Random data: $(head -c 100 /dev/urandom | base64)" >> "$TEST_FILE"

ORIGINAL_MD5=$(md5sum "$TEST_FILE" | cut -d' ' -f1)
echo "✓ File created, MD5: $ORIGINAL_MD5"
echo ""

# 步骤 2: 读取文件（所有节点在线）
echo "Step 2: Reading file with all nodes online..."
cat "$TEST_FILE" > /dev/null
if [ $? -eq 0 ]; then
    echo "✓ File read successfully"
else
    echo "✗ Failed to read file"
    exit 1
fi
echo ""

# 步骤 3: 提示关闭一个节点
echo "Step 3: Simulating node failure..."
echo ""
echo "Please open another terminal and stop ONE storage server:"
echo "  For node 1: pkill -f 'server 8001'"
echo "  For node 2: pkill -f 'server 8002'"
echo "  For node 3: pkill -f 'server 8003'"
echo ""
read -p "Press Enter after stopping one server..."

# 步骤 4: 尝试读取文件（一个节点离线）
echo ""
echo "Step 4: Attempting to read file with one node offline..."
cat "$TEST_FILE" > /tmp/fault_test_recovered.txt
if [ $? -eq 0 ]; then
    echo "✓ File read successfully (with one node down!)"
else
    echo "✗ Failed to read file"
    exit 1
fi

# 验证数据完整性
RECOVERED_MD5=$(md5sum /tmp/fault_test_recovered.txt | cut -d' ' -f1)
echo ""
echo "Verifying data integrity..."
echo "Original MD5:  $ORIGINAL_MD5"
echo "Recovered MD5: $RECOVERED_MD5"

if [ "$ORIGINAL_MD5" == "$RECOVERED_MD5" ]; then
    echo "✓ Data recovered successfully using XOR parity!"
else
    echo "✗ Data corruption detected"
    exit 1
fi

echo ""
echo "Step 5: Content comparison..."
diff "$TEST_FILE" /tmp/fault_test_recovered.txt
if [ $? -eq 0 ]; then
    echo "✓ File contents are identical!"
else
    echo "✗ File contents differ"
fi

echo ""
echo "=== Fault Tolerance Demo Complete ==="
echo ""
echo "Summary:"
echo "- MYFS successfully recovered data from n-1 nodes"
echo "- XOR parity was used to reconstruct missing fragment"
echo "- Data integrity verified with MD5 checksum"
echo ""
echo "You can check the bbfs.log for details:"
echo "  tail -f ~/myfs-zy/fuse-tutorial-2018-02-04/example/bbfs.log"

# 清理
rm -f /tmp/fault_test_recovered.txt

