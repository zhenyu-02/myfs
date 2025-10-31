# MYFS 快速开始指南

## 一、前置准备

假设你有 3 台虚拟机：
- VM1 (10.0.1.4) - 客户端 + 存储节点 1
- VM2 (10.0.1.5) - 存储节点 2  
- VM3 (10.0.1.6) - 存储节点 3

## 二、上传代码到虚拟机

在你的本地机器上：

```bash
# 上传整个项目到 VM1
cd /Users/ts-zhenyu.b.wang/cuhk-myfs
scp -P 140xx -r fuse-tutorial-2018-02-04 csci5550@projgw.cse.cuhk.edu.hk:~/myfs-zy/

# 或者如果已经有 fuse-tutorial，只上传修改的文件
cd fuse-tutorial-2018-02-04/src
scp -P 140xx protocol.h server.c params.h bbfs.c Makefile.am csci5550@projgw.cse.cuhk.edu.hk:~/myfs-zy/fuse-tutorial-2018-02-04/src/
```

## 三、在虚拟机上编译

在 VM1 上：

```bash
cd ~/myfs-zy/fuse-tutorial-2018-02-04

# 重新配置和编译
./configure
make

# 验证编译结果
ls -l src/bbfs src/server
```

## 四、启动存储节点服务器

### 在 VM1 上启动 Node 1：
```bash
cd ~/myfs-zy/fuse-tutorial-2018-02-04
mkdir -p ~/storage_node1
./src/server 8001 ~/storage_node1 &
```

### 在 VM2 上启动 Node 2：
```bash
# 首先从 VM1 复制程序到 VM2
# 在 VM1 上执行：
scp ~/myfs-zy/fuse-tutorial-2018-02-04/src/server csci5550@vm2:~/

# 然后在 VM2 上执行：
mkdir -p ~/storage_node2
./server 8002 ~/storage_node2 &
```

### 在 VM3 上启动 Node 3：
```bash
# 首先从 VM1 复制程序到 VM3
# 在 VM1 上执行：
scp ~/myfs-zy/fuse-tutorial-2018-02-04/src/server csci5550@vm3:~/

# 然后在 VM3 上执行：
mkdir -p ~/storage_node3
./server 8003 ~/storage_node3 &
```

## 五、挂载 MYFS（在 VM1 上）

```bash
cd ~/myfs-zy/fuse-tutorial-2018-02-04

# 创建挂载点
mkdir -p ~/myfs_root ~/myfs_mount

# 挂载文件系统
./src/bbfs ~/myfs_root ~/myfs_mount 10.0.1.4:8001 10.0.1.5:8002 10.0.1.6:8003 &

# 或者前台运行查看调试信息：
./src/bbfs -f ~/myfs_root ~/myfs_mount 10.0.1.4:8001 10.0.1.5:8002 10.0.1.6:8003
```

## 六、快速测试

```bash
# 进入挂载点
cd ~/myfs_mount

# 测试 1: 小文件
echo "Hello MYFS!" > test.txt
cat test.txt

# 测试 2: 4 MB 文件
dd if=/dev/urandom of=/tmp/4mb.dat bs=1M count=4
cp /tmp/4mb.dat ./
cat 4mb.dat > /dev/null
echo "4 MB file test passed"

# 测试 3: 查看文件分布
echo "Files on Node 1:"
ls -lh ~/storage_node1/

echo "Files on Node 2:"
ssh vm2 "ls -lh ~/storage_node2/"

echo "Files on Node 3:"
ssh vm3 "ls -lh ~/storage_node3/"
```

## 七、容错测试

```bash
# 1. 创建测试文件
echo "Fault tolerance test" > ~/myfs_mount/fault_test.txt

# 2. 停止一个节点（在 VM2 上）
ssh vm2 "pkill -f 'server 8002'"

# 3. 尝试读取文件（应该成功）
cat ~/myfs_mount/fault_test.txt

# 4. 查看日志确认 XOR 恢复
tail ~/myfs-zy/fuse-tutorial-2018-02-04/example/bbfs.log
```

## 八、清理

```bash
# 卸载文件系统
fusermount -u ~/myfs_mount

# 停止服务器
pkill -f "server 800"
ssh vm2 "pkill -f 'server 800'"
ssh vm3 "pkill -f 'server 800'"

# 清理测试文件
rm -rf ~/myfs_root/* ~/storage_node1/*
ssh vm2 "rm -rf ~/storage_node2/*"
ssh vm3 "rm -rf ~/storage_node3/*"
```

## 九、TA 测试准备

按照作业要求的测试用例：

### Test Case 1: 写入 10 个 4 MiB 文件
```bash
cd ~/myfs_mount
for i in {1..10}; do
    dd if=/dev/urandom of=file_4mb_$i.dat bs=1M count=4
done
ls -lh
```

### Test Case 2: 写入 1 个 40 MiB 文件
```bash
dd if=/dev/urandom of=file_40mb.dat bs=1M count=40
```

### Test Case 3: 写入 1 个 400 MiB 文件
```bash
dd if=/dev/urandom of=file_400mb.dat bs=1M count=400
```

### Test Case 4-6: 读取文件
```bash
# 使用 cp 命令测试
cp ~/myfs_mount/file_4mb_1.dat /tmp/test_read.dat
cp ~/myfs_mount/file_40mb.dat /tmp/test_read.dat
cp ~/myfs_mount/file_400mb.dat /tmp/test_read.dat
```

### Test Case 7-8: 容错读取
```bash
# 停止一个节点（由 TA 指定）
ssh vm2 "pkill -f 'server 8002'"

# 读取文件
cp ~/myfs_mount/file_4mb_1.dat /tmp/
cp ~/myfs_mount/file_400mb.dat /tmp/
```

## 十、常见问题

1. **编译错误**：确保安装了 libfuse-dev
2. **连接失败**：检查服务器是否启动，防火墙是否开放
3. **权限错误**：确保用户在 fuse 组中
4. **写入失败**：确保所有节点都在线

详细问题排查请参考 `README_MYFS.md`。

---

祝测试顺利！🎉

