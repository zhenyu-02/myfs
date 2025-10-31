#!/bin/bash
# 测试 MYFS 基本功能

MOUNTPOINT="$HOME/myfs_mount"

if [ ! -d "$MOUNTPOINT" ]; then
    echo "Error: Mount point $MOUNTPOINT does not exist"
    echo "Please mount MYFS first using mount_myfs.sh"
    exit 1
fi

echo "=== MYFS Basic Test ==="
echo ""

# 测试 1: 创建小文件
echo "Test 1: Creating small test file..."
echo "Hello MYFS! This is a test file." > "$MOUNTPOINT/test.txt"
if [ $? -eq 0 ]; then
    echo "✓ File created successfully"
else
    echo "✗ Failed to create file"
    exit 1
fi

# 测试 2: 读取文件
echo ""
echo "Test 2: Reading file..."
CONTENT=$(cat "$MOUNTPOINT/test.txt")
if [ "$CONTENT" == "Hello MYFS! This is a test file." ]; then
    echo "✓ File content matches"
else
    echo "✗ File content mismatch"
    echo "Expected: Hello MYFS! This is a test file."
    echo "Got: $CONTENT"
    exit 1
fi

# 测试 3: 创建 4 MB 文件
echo ""
echo "Test 3: Creating 4 MB test file..."
dd if=/dev/urandom of=/tmp/test_4mb.dat bs=1M count=4 2>/dev/null
cp /tmp/test_4mb.dat "$MOUNTPOINT/test_4mb.dat"
if [ $? -eq 0 ]; then
    echo "✓ 4 MB file created successfully"
else
    echo "✗ Failed to create 4 MB file"
    exit 1
fi

# 测试 4: 验证 4 MB 文件
echo ""
echo "Test 4: Verifying 4 MB file..."
cmp /tmp/test_4mb.dat "$MOUNTPOINT/test_4mb.dat"
if [ $? -eq 0 ]; then
    echo "✓ 4 MB file verification passed"
else
    echo "✗ 4 MB file verification failed"
    exit 1
fi

# 测试 5: 列出文件
echo ""
echo "Test 5: Listing files..."
ls -lh "$MOUNTPOINT"

echo ""
echo "=== All tests passed! ==="
echo ""
echo "You can check the storage nodes to see how files are distributed:"
echo "  ls -lh ~/storage_node1/"
echo "  ls -lh ~/storage_node2/"
echo "  ls -lh ~/storage_node3/"

# 清理
rm -f /tmp/test_4mb.dat

