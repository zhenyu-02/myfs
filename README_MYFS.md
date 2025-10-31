# MYFS 分布式文件系统 - 使用说明

## 项目概述

MYFS 是基于 FUSE 的分布式文件系统，支持：
- 将文件分片存储到多个远程节点
- 使用 XOR 实现数据冗余和容错
- 支持单节点故障时的数据恢复

## 文件说明

### 源代码文件
- `src/protocol.h` - 客户端和服务器通信协议定义
- `src/server.c` - 存储节点服务器程序
- `src/bbfs.c` - MYFS 客户端（基于 BBFS 修改）
- `src/params.h` - 配置参数和数据结构
- `src/log.c/log.h` - 日志功能

## 编译步骤

在虚拟机上执行：

```bash
cd ~/myfs-zy/fuse-tutorial-2018-02-04

# 重新生成 Makefile（如果修改了 Makefile.am）
./configure

# 编译
make

# 检查编译结果
ls -l src/bbfs src/server
```

编译成功后会生成两个可执行文件：
- `src/bbfs` - MYFS 客户端
- `src/server` - 存储节点服务器

## 部署步骤

假设你有 3 个虚拟机：VM1 (10.0.1.4), VM2 (10.0.1.5), VM3 (10.0.1.6)

### 1. 在每个存储节点（VM1, VM2, VM3）启动服务器

```bash
# 在 VM1 上
mkdir -p ~/storage_node1
cd ~/myfs-zy/fuse-tutorial-2018-02-04
./src/server 8001 ~/storage_node1 &

# 在 VM2 上
mkdir -p ~/storage_node2
cd ~/myfs-zy/fuse-tutorial-2018-02-04
./src/server 8002 ~/storage_node2 &

# 在 VM3 上
mkdir -p ~/storage_node3
cd ~/myfs-zy/fuse-tutorial-2018-02-04
./src/server 8003 ~/storage_node3 &
```

### 2. 在客户端（VM1）挂载 MYFS

```bash
# 创建目录
mkdir -p ~/myfs_root
mkdir -p ~/myfs_mount

# 挂载文件系统
cd ~/myfs-zy/fuse-tutorial-2018-02-04
./src/bbfs ~/myfs_root ~/myfs_mount 10.0.1.4:8001 10.0.1.5:8002 10.0.1.6:8003

# 或者使用前台模式查看调试信息
./src/bbfs -f ~/myfs_root ~/myfs_mount 10.0.1.4:8001 10.0.1.5:8002 10.0.1.6:8003
```

## 测试步骤

### 基本测试

```bash
# 进入挂载点
cd ~/myfs_mount

# 创建测试文件
echo "Hello MYFS!" > test.txt

# 读取文件
cat test.txt

# 复制文件
cp /path/to/source_file.txt ./
cat source_file.txt

# 检查各节点的片段
ls -lh ~/storage_node1/
ls -lh ~/storage_node2/
ls -lh ~/storage_node3/
```

### 容错测试

```bash
# 1. 先写入一个文件
echo "Test fault tolerance" > fault_test.txt

# 2. 模拟一个节点故障（停止服务器）
# 在 VM2 上执行：
pkill -f "server 8002"

# 3. 尝试读取文件（应该仍然成功）
cat fault_test.txt

# 4. 检查日志确认使用了 XOR 恢复
tail -f ~/myfs-zy/fuse-tutorial-2018-02-04/example/bbfs.log
```

### 大文件测试

```bash
# 创建测试文件
dd if=/dev/urandom of=4mb.dat bs=1M count=4
dd if=/dev/urandom of=40mb.dat bs=1M count=40
dd if=/dev/urandom of=400mb.dat bs=1M count=400

# 复制到 MYFS
cp 4mb.dat ~/myfs_mount/
cp 40mb.dat ~/myfs_mount/
cp 400mb.dat ~/myfs_mount/

# 读取并验证
cmp 4mb.dat ~/myfs_mount/4mb.dat
cmp 40mb.dat ~/myfs_mount/40mb.dat
cmp 400mb.dat ~/myfs_mount/400mb.dat
```

## 卸载文件系统

```bash
# 卸载
fusermount -u ~/myfs_mount

# 或者如果用 -f 前台运行，按 Ctrl+C
```

## 停止服务器

```bash
# 在每个节点上
pkill -f "server 800"
```

## 查看日志

```bash
# 客户端日志
tail -f ~/myfs-zy/fuse-tutorial-2018-02-04/example/bbfs.log

# 服务器日志（在服务器终端查看）
```

## 常见问题

### 1. 编译错误：找不到 fuse.h
```bash
sudo apt-get install libfuse-dev fuse
```

### 2. 挂载失败：Permission denied
```bash
# 确保用户在 fuse 组中
sudo usermod -a -G fuse $USER
# 重新登录
```

### 3. 连接失败：Connection refused
- 检查服务器是否启动：`ps aux | grep server`
- 检查端口是否监听：`netstat -tln | grep 800`
- 检查防火墙设置

### 4. 写入失败
- 确保所有节点都在线（写入时需要所有节点）
- 检查服务器存储目录权限

### 5. 读取失败
- 至少需要 n-1 个节点在线
- 检查 bbfs.log 查看详细错误信息

## 架构说明

### 数据分片

对于 n 个节点：
- 将数据分成 n-1 个数据片段
- 计算 1 个 XOR 校验片段
- 校验片段存储在第 n 个节点

例如，3 个节点，1 MB 数据：
- Node 1: 前 512 KB 数据
- Node 2: 后 512 KB 数据  
- Node 3: XOR(Node1, Node2) = 校验片段

### XOR 恢复

如果 Node 2 失效：
- 从 Node 1 读取数据片段 1
- 从 Node 3 读取校验片段
- 计算：Node2_data = XOR(Node1_data, Parity)
- 恢复原始数据

## 性能提示

1. **使用 SSD 存储**：提高 I/O 性能
2. **千兆网络**：减少网络传输时间
3. **多线程优化**（未实现）：并行读写多个节点
4. **内存映射**（未实现）：处理超大文件

## 调试技巧

```bash
# 1. 使用前台模式查看实时日志
./src/bbfs -d -f ~/myfs_root ~/myfs_mount 10.0.1.4:8001 10.0.1.5:8002 10.0.1.6:8003

# 2. 监控网络连接
watch -n 1 'netstat -tn | grep 800'

# 3. 监控文件操作
tail -f bbfs.log

# 4. 使用 strace 调试
strace -f ./src/bbfs ~/myfs_root ~/myfs_mount 10.0.1.4:8001 10.0.1.5:8002 10.0.1.6:8003
```

---

祝测试顺利！如有问题请查看日志文件或联系开发者。

