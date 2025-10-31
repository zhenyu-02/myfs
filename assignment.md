# CSCI5550 编程项目：基于 FUSE 的分布式文件系统（MYFS）

## 1. 项目概述

本项目要求你使用 **FUSE（Filesystem in Userspace）** 在 C/C++ 中实现一个名为 **MYFS** 的分布式文件系统。MYFS 的核心目标是：

- 提供标准的文件系统接口（通过 FUSE）；
- 将文件数据 **分布式地存储在多个远程存储节点（服务器）上**；
- 支持 **容错读写**：即使一个节点失效，仍能从其余节点恢复文件；
- 基于 **客户端-服务器模型**，使用 **Socket 编程** 实现通信；
- 扩展自 **BBFS（Big Brother File System）**，由 CMU 的 Joseph Pfeiffer 教授开发。

### 1.1 系统架构

```
          Node 1     Node 2     ...     Node n
             |          |                  |
             +----------+------------------+
                        |
                     MYFS (Client)
                        |
                   /mnt/myfs (挂载点)
```

- MYFS 作为 **FUSE 客户端**，挂载于本地（如 `/mnt/myfs`）；
- 实际数据存储在 **n 个远程存储节点** 上，每个节点运行一个 **服务器进程**；
- 文件元数据（如文件名、大小等）**本地存储在运行 MYFS 的机器上**。

---

## 2. 功能要求

### 2.1 写入文件（Write）

- 输入：任意二进制文件；
- 处理流程：
  1. 将文件 **切分为最多 1 MiB 的数据块**；
  2. 若有 `n` 个节点，则生成 `n−1` 个 **数据片段（data fragments）**；
  3. 通过 **XOR 计算生成 1 个奇偶校验片段（parity fragment）**；
  4. 将 `n` 个片段（`n−1` 数据 + 1 校验）**分别写入 n 个节点**；
  5. **校验片段必须存储在第 n 个节点（Node n）**；
- 前提假设：
  - **写入时所有节点必须在线**；
  - **不实现节点故障恢复机制**（即写入失败不重试）。

### 2.2 读取文件（Read）

- 支持从 **任意 n−1 个节点** 读取数据并重建原始文件；
- 若某节点失效（如网络断开），`recv()` 返回 0，系统应：
  - 自动跳过该节点；
  - 利用其余 `n−1` 个片段（可能包含 parity）通过 XOR 重建缺失数据；
- 支持大文件读取（如 400 MiB），建议使用 **内存映射 I/O（mmap）** 避免内存溢出。

### 2.3 列出文件（List）

- 实现 `list` 功能，显示当前 MYFS 中存储的所有文件名；
- 可通过 `ls /mnt/myfs` 或自定义命令实现；
- 需在演示中展示 **每个节点上存储的片段分布情况**（如目录内容）。

---

## 3. 技术实现细节

### 3.1 基础框架

- 基于 **BBFS**（https://www.cs.nmsu.edu/~pfeiffer/fuse-tutorial/）进行扩展；
- 使用 **FUSE 2.9.2**（注意：**不兼容 FUSE 3.x**）；
- 编译环境：**GCC/G++ on Linux**；
- 通信方式：**TCP Socket 编程**。

### 3.2 客户端（MYFS）

- 修改 `bbfs.c` 中的 `bb_write()` 和 `bb_read()` 函数；
- 在 `main()` 中初始化与 n 个服务器的连接；
- 保存 socket 描述符供后续读写使用；
- 示例结构：
  ```c
  void init_myfs(int argc, char** argv) {
      // 连接 n 个服务器，保存 sockets
  }
  int myfs_write(const char* path, const char* buf, size_t size, off_t offset) {
      // 分片、计算 parity、发送到各节点
  }
  int bb_write(...) {
      return myfs_write(...);
  }
  ```

### 3.3 服务器端

- 每个节点运行一个 **独立的服务器进程**（如 `./server <port> <dir>`）；
- 服务器功能：
  - 监听指定端口；
  - 接收客户端的读/写请求；
  - 执行本地文件操作（`pwrite`, `pread` 或 `mmap`）；
  - 返回操作结果；
- 示例主循环：
  ```c
  while (1) {
      recv(request);
      if (request.type == WRITE) pwrite(...);
      else if (request.type == READ) pread(...);
      send(response);
  }
  ```

### 3.4 大文件优化建议（可选但推荐）

- **内存映射 I/O（mmap）**：
  - 避免将整个大文件加载到内存；
  - 使用 `mmap()` 映射文件到虚拟内存，按需访问；
  - 相关函数：`mmap`, `munmap`, `lseek`, `posix_fadvise`；
- **多线程并行通信**：
  - 同时向 n 个节点发送读/写请求，减少等待时间；
- **合并小写请求**：
  - 将多个 4 KiB 写请求合并为更大的块，减少网络开销；
- **性能测量**：
  - 使用 `gettimeofday()` 记录操作耗时（单位：微秒）；
  - 用于优化和争取 **性能 bonus**。

---

## 4. 测试与评分标准（共 100% + 5% Bonus）

所有测试通过 `cp` 命令执行（如 `cp local_file /mnt/myfs/`）。

| 测试项 | 描述 | 分值 |
|--------|------|------|
| **Startup** | 设置 `n = 3`，所有节点正常运行 | - |
| Test Case 1 | 写入 10 个 4 MiB 文件，列出文件并展示各节点片段分布 | 10% |
| Test Case 2 | 写入 1 个 40 MiB 文件，列出文件并展示分布 | 10% |
| Test Case 3 | 写入 1 个 400 MiB 文件，列出文件并展示分布 | 20% |
| Test Case 4 | 读取一个 4 MiB 文件 | 10% |
| Test Case 5 | 读取 40 MiB 文件 | 10% |
| Test Case 6 | 读取 400 MiB 文件 | 10% |
| Test Case 7 | 关闭一个节点（由 TA 指定），读取 4 MiB 文件 | 10% |
| Test Case 8 | 在同一节点仍关闭的情况下，读取 400 MiB 文件 | 20% |
| **Bonus** | **读写大文件（Test 3 & 6）性能最快小组** | +5% |

> ⚠️ **注意**：测试按顺序执行，依赖前序结果；**迟交不予接受，得 0 分**。

---

## 5. 开发与部署环境（VM 配置）

### 5.1 虚拟机（VM）设置

- 每组分配 **3 台 Ubuntu 22.04 LTS 虚拟机**：`VM[a]`, `VM[b]`, `VM[c]`；
- 网络结构：
  - `VM[a]`：唯一可访问外网，可通过 SSH 从 CSE 网络连接；
  - `VM[b]`, `VM[c]`：仅可通过 `VM[a]` 内网访问（IP：`10.0.xx.5`, `10.0.xx.6`）；
- 默认账号：`csci5550`，密码：`csci5550@2025`（**请立即修改！**）

### 5.2 网络访问方式

- **在校内或使用 CSE VPN**：
  - 直接 SSH：`ssh -p 140xx csci5550@projgw.cse.cuhk.edu.hk`
- **通过 CSE 网关**：
  - `ssh <username>@gw.cse.cuhk.edu.hk`

### 5.3 VM 配置建议

#### (1) 修改主机名
```bash
sudo vim /etc/hostname   # 改为 vm1, vm2, vm3
sudo vim /etc/hosts      # 将 127.0.0.1 行同步修改
sudo reboot
```

#### (2) 配置 hosts 映射
在每台 VM 的 `/etc/hosts` 中添加：
```
10.0.xx.4 vm1
10.0.xx.5 vm2
10.0.xx.6 vm3
```
→ 可用 `ssh vm2` 代替 IP。

#### (3) 配置 SSH 免密登录
```bash
ssh-keygen               # 一路回车
ssh-copy-id vm1
ssh-copy-id vm2
ssh-copy-id vm3
```

#### (4) 文件传输
- **外部 ↔ VM[a]**：
  ```bash
  scp -P 140xx file csci5550@projgw.cse.cuhk.edu.hk:~/
  scp -P 140xx -r dir csci5550@projgw.cse.cuhk.edu.hk:~/
  ```
- **VM 之间**：
  ```bash
  scp file csci5550@vm2:~/
  scp -r dir csci5550@vm3:~/
  ```

### 5.4 离线安装软件包（如需）

- 在联网机器下载：
  ```bash
  apt-get install --download-only <package>
  # 包位于 /var/cache/apt/archives/
  ```
- 复制到离线 VM 后安装：
  ```bash
  dpkg -i *.deb
  ```

---

## 6. 项目交付物

- **源代码**：提交至 Blackboard；
- **可编译性**：必须能在 Linux 上用 GCC/G++ 成功编译；
- **演示（Demo）**：TA 将按上述测试用例现场验证；
- **严禁迟交**：截止时间 **2025年12月11日 23:59:59**。

---

## 7. 其他注意事项

- **元数据本地存储**：文件名、大小、分片信息等保存在 MYFS 客户端（如 VM[a]）；
- **无需实现节点恢复**：写入时假设全在线，读取时容忍单点故障；
- **鼓励创新优化**：如多线程、异步 I/O、请求批处理等；
- **参考 BBFS 教程**：理解 FUSE 回调函数（如 `getattr`, `open`, `read`, `write`）。

---

> ✅ **提示**：建议先在小文件（如 1 MiB）上验证分片与 XOR 逻辑，再扩展至大文件和容错场景。

--- 
