# ftl-firmware：SSD固件参考实现——FTL算法栈与NVMe/TCP+UFS双协议栈

一套完整的SSD固件参考实现，涵盖NAND闪存抽象层、FTL闪存转换层、NVMe控制器及NVMe/TCP目标端协议栈，可与Linux内核nvme-tcp驱动真实对接，支持nvme-cli、fio等标准存储工具的功能验证与性能测试。

## 项目概述

本项目是一套完整的SSD固件参考实现，采用7层模块化设计，模拟真实SSD固件的完整技术栈：从底层NAND闪存抽象、FTL核心算法、NVMe控制器，到前端NVMe/TCP协议栈，实现与Linux内核驱动的真实对接。项目具备完整的固件核心特性：掉电保护、ECC纠错、读干扰处理、数据保留模拟、健康监控、错误处理与自动恢复。

### 设计目标

- **模块化架构**：7层分层设计，每个功能模块独立，职责清晰，低耦合高内聚
- **协议真实对接**：NVMe/TCP协议栈与Linux内核nvme-tcp驱动完整对接，支持标准工具验证
- **系统特性**：掉电保护、ECC纠错、读干扰处理、数据保留、健康监控、自动恢复
- **可扩展性**：支持UFS协议栈扩展、OS抽象层支持RTOS移植
- **可维护性**：统一的代码风格、Doxygen注释、清晰的接口定义、CI自动化测试

### 技术栈

- **编程语言**：C99
- **构建系统**：Make
- **目标平台**：Linux/Unix（可移植到嵌入式 RTOS）
- **代码风格**：统一的固件编码规范

## 快速开始

### 环境要求

- GCC 或 Clang 编译器
- Make 构建工具
- Linux/Unix 环境（Windows 可使用 WSL 或 MinGW）

### 编译

```bash
# 克隆仓库
git clone https://github.com/GanHao-Eng/ftl-firmware.git
cd ftl-firmware

# 编译固件
make

# 清理构建产物
make clean

# 显示帮助信息
make help
```

### 运行

```bash
# 运行固件模拟器
./build/ftl_firmware
```

### 测试

```bash
# 构建测试（待实现）
make test

# 运行测试（待实现）
make runtest
```

## 架构设计

### 模块架构

```
┌─────────────────────────────────────────────────────────┐
│                     主机接口 (Host IF)                    │
│              模拟 NVMe 协议，处理主机命令                  │
└────────────────────────────┬────────────────────────────┘
                             │
                     ┌───────▼───────┐
                     │   消息队列    │
                     │  (IPC 通信)   │
                     └───────┬───────┘
                             │
┌────────────────────────────▼────────────────────────────┐
│                     FTL 模块                             │
│        逻辑页映射、GC、磨损均衡、TRIM、WAL 等             │
└────────────────────────────┬────────────────────────────┘
                             │
                     ┌───────▼───────┐
                     │   消息队列    │
                     └───────┬───────┘
                             │
┌────────────────────────────▼────────────────────────────┐
│                    NAND 模块                             │
│        页读写、块擦除、坏块管理、ECC、OOB 等              │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│                    管理模块 (Manager)                     │
│        健康监控、错误处理、自动恢复、看门狗、配置管理       │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│                    日志模块 (Log)                         │
│              分级日志、控制台/文件输出                     │
└─────────────────────────────────────────────────────────┘
```

### 模块说明

| 模块 | 路径 | 职责 |
|------|------|------|
| NAND 模块 | `modules/nand/` | 模拟 NAND 闪存，提供页读写、块擦除、坏块管理、ECC、OOB 等功能 |
| FTL 模块 | `modules/ftl/` | 闪存转换层，提供 L2P 映射、GC、磨损均衡、TRIM、WAL 等功能 |
| 主机接口模块 | `modules/host_if/` | 模拟 NVMe 主机接口，处理主机命令，提交队列/完成队列 |
| 管理模块 | `modules/manager/` | 模块管理、健康监控、错误处理、自动恢复、看门狗 |
| 日志模块 | `modules/log/` | 分级日志系统，支持控制台和文件输出 |
| IPC 模块 | `ipc/` | 进程间通信，消息队列机制 |
| 工具模块 | `utils/` | 通用工具函数，位操作、CRC、延时等 |

## 目录结构

```
ftl-firmware/
├── src/                    # 主程序入口
│   └── main.c             # 主函数
├── modules/                # 功能模块
│   ├── nand/              # NAND 模块
│   │   ├── nand.h
│   │   └── nand.c
│   ├── ftl/               # FTL 模块
│   │   ├── ftl.h
│   │   └── ftl.c
│   ├── log/               # 日志模块
│   │   ├── log.h
│   │   └── log.c
│   ├── host_if/           # 主机接口模块
│   │   ├── host_if.h
│   │   └── host_if.c
│   └── manager/           # 管理模块
│       ├── manager.h
│       └── manager.c
├── include/                # 公共头文件
│   └── common/
│       └── common.h       # 公共类型定义
├── ipc/                    # 进程间通信
│   ├── msg_queue.h        # 消息队列接口
│   └── msg_queue.c        # 消息队列实现
├── utils/                  # 工具函数
│   ├── utils.h            # 工具函数接口
│   └── utils.c            # 工具函数实现
├── tests/                  # 测试用例
├── docs/                   # 文档
├── build/                  # 构建输出
└── Makefile               # 构建脚本
```

## 功能特性

### NAND 模块
- ✅ 页读写、块擦除
- ✅ 坏块管理（初始坏块、磨损坏块）
- ✅ 磨损计数
- ✅ 读干扰管理
- ✅ CRC32 校验
- ✅ ECC 纠错（(7,4)汉明码完整编解码，支持1位错误自动纠正+多位错误检测，写入时计算ECC存入OOB，读取时自动校验纠正）
- ✅ 数据保留(Data Retention)模拟（高温/长时间存储位错误注入，可配置错误率，验证ECC纠错能力）
- ✅ OOB 区域管理（magic标记页有效性，支持掉电恢复时扫描重建页状态）
- ✅ 多颗粒类型支持（SLC/MLC/TLC/QLC）
- ✅ 功耗模拟
- ✅ 预留块池
- ✅ 掉电恢复模式（文件已存在时保留数据，扫描OOB重建页有效标记）

### FTL 模块
- ✅ L2P 页映射
- ✅ 反向映射表
- ✅ 六种 GC 算法（Greedy、Cost-Benefit、CAT、Windowed、d-Choices、FRA）
- ✅ 动态磨损均衡
- ✅ 静态磨损均衡
- ✅ TRIM/Discard 支持
- ✅ 读干扰处理
- ✅ 掉电保护（PLP）：元数据快照自动持久化，启动自动恢复，定期快照保存，退出前快照保存
- ✅ WAL 写前日志（写入前记录映射变更，支持日志重放恢复）
- ✅ 混合映射（热数据页映射，冷数据块映射）
- ✅ 坏块自动替换

### 主机接口模块
- ✅ NVMe 1.4 协议栈
- ✅ NVMe/TCP 目标端（与 Linux 内核 nvme-tcp 驱动完整对接）
- ✅ 提交队列（SQ）/ 完成队列（CQ）
- ✅ 阶段标签（Phase Tag）
- ✅ Admin 命令：Identify、Get Log Page、Set Features、Keep Alive
- ✅ I/O 命令：Read、Write、Write Zeroes、Flush、Dataset Management(TRIM)
- ✅ Fabric Command：Property Set/Get、Connect
- ✅ 多队列支持（Admin + 2 个 I/O 队列）
- ✅ QEMU vhost-user NVMe 后端（与 QEMU vhost-user-nvme 设备对接，虚拟机内识别 NVMe 设备）
- ✅ QEMU vfio-user NVMe PCIe 后端（与 QEMU vfio-user-pci 设备对接，模拟完整 PCIe 设备：配置空间/BAR0/MSI-X/DMA，虚拟机内标准 nvme 驱动识别）
- ✅ 异步事件请求(AER)完整实现（事件队列、事件掩码、最多4并发、SMART/温度/固件事件上报）
- ✅ 多温度传感器支持（8个传感器：Composite/NAND Ch0/NAND Ch1/DRAM/Ambient/Power，阈值告警+AER事件）
- ✅ 固件更新完整实现（7插槽管理、Firmware Download分块下载、Firmware Commit激活、Firmware Slot Log）
- ✅ 真实数据持久化（Write→FTL→NAND，Read→NAND→FTL→主机）

### 管理模块
- ✅ 模块初始化/销毁
- ✅ 健康监控
- ✅ 错误报告
- ✅ 自动恢复
- ✅ 心跳机制
- ✅ 看门狗检测
- ✅ 统计信息
- ✅ 模块状态管理
- ✅ 温度管理（温度监控、热管理、过热保护）
- ✅ 电源管理（功耗状态、节能模式）

### UFS 协议栈模块（框架级实现）
- ✅ UFS目标端框架，基于SCSI命令集
- ✅ UPIU事务类型定义（命令/响应/数据输入输出）
- ✅ SCSI CDB解析（10字节命令）
- ✅ 支持命令：INQUIRY、READ(10)、WRITE(10)、READ CAPACITY、TEST UNIT READY、SYNCHRONIZE CACHE、UNMAP
- ✅ SCSI状态码和请求感知数据（Sense Data）
- ✅ 数据存储对接FTL层（512字节扇区↔4KB页转换）

### OS 抽象层（OSAL）
- ✅ 跨平台操作系统接口抽象
- ✅ 互斥锁（创建/销毁/加锁/解锁）
- ✅ 时间管理（获取系统时间/延时）
- ✅ 线程管理（创建/销毁）
- ✅ 消息队列（创建/发送/接收/销毁，支持超时）
- ✅ 事件标志组（创建/置位/等待，支持任意/所有位等待）
- ✅ 任务通知（轻量级事件通知机制）
- ✅ Linux平台实现（基于POSIX pthread + 条件变量）
- ✅ 预留FreeRTOS/RT-Thread/裸机扩展接口

### 性能监控
- ✅ IOPS 统计（读/写/总）
- ✅ 带宽统计（读/写/总）
- ✅ 延迟统计（最小/最大/平均）
- ✅ 性能统计窗口

### 安全功能
- ✅ 安全擦除（多次覆写+TRIM）
- ✅ 全盘安全擦除
- ✅ 多种数据覆写模式

### 线程管理模块
- ✅ 线程创建和销毁
- ✅ 线程启动和停止
- ✅ 线程等待（join）
- ✅ 线程优先级
- ✅ 线程状态查询
- ✅ 互斥锁（mutex）
- ✅ 条件变量（condition variable）
- ✅ 线程休眠
- ✅ 基于 POSIX 线程库（pthread）

### DMA 模块
- ✅ DMA 控制器
- ✅ DMA 通道分配和释放
- ✅ DMA 传输描述符
- ✅ 异步 DMA 传输
- ✅ 同步 DMA 传输
- ✅ 传输暂停和恢复
- ✅ 传输完成回调
- ✅ 传输状态查询
- ✅ 多种传输方向（内存到内存、内存到设备等）
- ✅ 多种传输宽度（字节、半字、字、双字）
- ✅ 突发长度配置

### IPC 模块
- ✅ 消息队列
- ✅ 消息优先级
- ✅ 非阻塞/阻塞接收
- ✅ 按优先级排序

### 工具模块
- ✅ 位操作工具
- ✅ 对齐工具
- ✅ 数学工具
- ✅ 安全内存操作
- ✅ CRC 计算（CRC32、CRC16）
- ✅ 延时函数
- ✅ 版本信息

## 编译运行

### 编译

```bash
# 编译固件
make

# 清理构建产物
make clean

# 显示帮助信息
make help
```

### 运行

```bash
# 运行固件
./build/ftl_firmware
```

### 测试

```bash
# 构建测试
make test

# 运行所有测试
make runtest

# 单独运行
./build/test_ftl_unit       # FTL 层单元测试（25项）
./build/test_gc_benchmark   # 6种GC算法性能对比
./build/test_plp_recovery   # 掉电保护恢复测试（4项）
```

## NVMe/TCP 目标端使用指南

本固件实现了完整的 NVMe/TCP 目标端，可与 Linux 内核 `nvme-tcp` 主机驱动直接对接，实现真实的块设备读写。

### 架构

```
┌──────────────────────────────────────────────────────┐
│  Linux 主机 (nvme-cli / dd / fio)                    │
│  ┌────────────────────────────────────────────────┐  │
│  │  nvme-tcp 内核驱动 (TCP 4420)                  │  │
│  └───────────────────┬────────────────────────────┘  │
└──────────────────────┼───────────────────────────────┘
                       │ TCP
┌──────────────────────┼───────────────────────────────┐
│  ftl-firmware         ▼                              │
│  ┌────────────────────────────────────────────────┐  │
│  │  NVMe/TCP 目标端 (nvme_tcp_target.c)           │  │
│  │  IC握手 → Connect → Admin队列 → I/O队列        │  │
│  └───────────────────┬────────────────────────────┘  │
│                      │                               │
│  ┌───────────────────▼────────────────────────────┐  │
│  │  NVMe 控制器 (nvme_controller.c)               │  │
│  │  Identify / Set Features / Keep Alive          │  │
│  └───────────────────┬────────────────────────────┘  │
│                      │                               │
│  ┌───────────────────▼────────────────────────────┐  │
│  │  FTL 层 (ftl.c)  →  NAND 层 (nand.c)          │  │
│  └────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────┘
```

### 支持的 NVMe 命令

| 类型 | 操作码 | 命令 | 说明 |
|------|--------|------|------|
| Admin | 0x06 | Identify | Controller / Namespace |
| Admin | 0x02 | Get Log Page | SMART/Health (LID=0x02) |
| Admin | 0x09 | Set Features | Number of Queues, Keep Alive |
| Admin | 0x18 | Keep Alive | 保活命令 |
| Fabric | 0x7F | Property Set/Get | 寄存器访问 |
| Fabric | 0x7F | Connect | 建立队列连接 |
| I/O | 0x00 | Flush | 刷新缓存 |
| I/O | 0x01 | Write | 写数据（R2T+H2CData） |
| I/O | 0x02 | Read | 读数据（C2HData） |
| I/O | 0x08 | Write Zeroes | 写零 |
| I/O | 0x09 | Dataset Management | TRIM |

### 快速开始

#### 1. 编译运行固件

```bash
cd ftl-firmware
make
./build/ftl_firmware
# 固件监听 TCP 端口 4420，SubNQN: nqn.2026-08.io.ftlfw:subsystem
```

#### 2. 主机连接

```bash
# 加载 nvme-tcp 内核模块
sudo modprobe nvme-tcp

# 连接到目标端
sudo nvme connect -t tcp -a 127.0.0.1 -s 4420 \
    -n nqn.2026-08.io.ftlfw:subsystem

# 验证设备
sudo nvme list
# /dev/nvme0n1 应出现，容量约 1GB，4K LBA
```

#### 3. 读写测试

```bash
# 写入测试
dd if=/dev/urandom of=/tmp/test.bin bs=4096 count=16
sudo dd if=/tmp/test.bin of=/dev/nvme0n1 bs=4096 count=16

# 读回验证
sudo dd if=/dev/nvme0n1 of=/tmp/verify.bin bs=4096 count=16
md5sum /tmp/test.bin /tmp/verify.bin  # 应一致

# Write Zeroes
sudo nvme write-zeroes /dev/nvme0n1 --start-block=0 --block-count=15

# TRIM
sudo nvme dsm /dev/nvme0n1 --ad 1 --blocks=0,16
```

#### 4. fio 性能测试

```bash
# 顺序读
sudo fio --name=seqread --filename=/dev/nvme0n1 --rw=read \
    --bs=4k --size=1M --iodepth=1 --runtime=5 --time_based

# 顺序写
sudo fio --name=seqwrite --filename=/dev/nvme0n1 --rw=write \
    --bs=4k --size=1M --iodepth=1 --runtime=5 --time_based

# 随机读
sudo fio --name=randread --filename=/dev/nvme0n1 --rw=randread \
    --bs=4k --size=1M --iodepth=1 --runtime=5 --time_based
```

### 关键技术细节

- **LBA 大小**: 4KB (LBAF0: ds=12, ms=0)
- **命名空间容量**: 262144 LBA × 4KB = 1GB
- **队列配置**: Admin 队列 + 2 个 I/O 队列
- **NVMe 版本**: 1.4
- **Connect QID 位置**: inline data byte 16-17 (Admin=0xFFFF→0, I/O=1+)
- **未写入页读取**: 返回全零（符合 NVMe 规范）

### 调试技巧

```bash
# 查看内核日志
sudo dmesg | grep -i nvme

# 查看固件日志（重定向到文件）
./build/ftl_firmware > /tmp/fw.log 2>&1
grep -E "ERROR|WARN|写命令|读命令|FTL" /tmp/fw.log

# 抓包分析
sudo tcpdump -i lo -w /tmp/nvme.pcap port 4420
```


## QEMU vhost-user NVMe 后端使用指南

本固件实现了完整的 vhost-user NVMe 后端，可作为 QEMU 虚拟机的 NVMe 设备后端。QEMU 通过 `vhost-user-nvme` 设备连接到本后端，虚拟机内即可识别标准 NVMe 设备并进行读写。

### 架构

```
┌──────────────────────────────────────────────────────┐
│  QEMU 虚拟机 (Linux Guest)                            │
│  ┌────────────────────────────────────────────────┐  │
│  │  nvme 内核驱动 + nvme-cli / dd / fio           │  │
│  └───────────────────┬────────────────────────────┘  │
│                      │ PCIe (模拟)                     │
│  ┌───────────────────▼────────────────────────────┐  │
│  │  QEMU vhost-user-nvme 设备                      │  │
│  └───────────────────┬────────────────────────────┘  │
└──────────────────────┼───────────────────────────────┘
                       │ Unix Socket + 共享内存
┌──────────────────────▼───────────────────────────────┐
│  ftl-firmware (vhost-user 后端)                        │
│  ┌────────────────────────────────────────────────┐  │
│  │  vhost_user_nvme.c (协议握手/共享内存/vring)    │  │
│  │  GET_FEATURES → SET_MEM_TABLE → SET_VRING_*    │  │
│  └───────────────────┬────────────────────────────┘  │
│                      │                                 │
│  ┌───────────────────▼────────────────────────────┐  │
│  │  NVMe 控制器 (nvme_controller.c)                │  │
│  │  Admin/I/O 命令处理 → FTL → NAND                │  │
│  └────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────┘
```

### 工作原理

1. **协议握手**：QEMU 通过 Unix socket 连接后端，完成 vhost-user 协议握手（特性协商、内存表设置、vring配置）
2. **共享内存**：QEMU 将虚拟机物理内存区域通过文件描述符传递给后端，后端 mmap 后可直接访问 Guest 内存
3. **vring 队列**：每个 NVMe 队列（Admin + I/O）对应一个 virtio ring，QEMU 写命令到 available ring，后端写完成到 used ring
4. **门铃与中断**：QEMU 写门铃寄存器（SET_CONFIG）通知后端有新命令；后端通过 call eventfd 通知 QEMU 命令完成（触发虚拟机中断）
5. **PRP 数据传输**：NVMe 命令中的 PRP 指针指向 Guest 物理内存，后端通过 GPA→HVA 映射直接读写数据

### 快速开始

#### 1. 编译运行固件（vhost-user 模式）

```bash
cd ftl-firmware
make

# 启动 vhost-user 后端模式
/tmp/ftl-firmware-build/ftl_firmware --vhost-user
# 固件监听 /tmp/ftl-vhost-user.sock，等待 QEMU 连接

# 自定义 socket 路径
/tmp/ftl-firmware-build/ftl_firmware --vhost-user --vhost-socket=/tmp/my-nvme.sock
```

#### 2. 启动 QEMU 虚拟机

```bash
qemu-system-x86_64 \
    -machine q35,accel=kvm \
    -cpu host -m 2048 -smp 2 \
    -drive file=/path/to/guest.img,format=qcow2 \
    -chardev socket,id=vu0,path=/tmp/ftl-vhost-user.sock \
    -device vhost-user-nvme,chardev=vu0,num-queues=2 \
    -net nic -net user,hostfwd=tcp::2222-:22 \
    -nographic
```

**要求**：QEMU 8.0+ 版本支持 `vhost-user-nvme` 设备。

#### 3. 虚拟机内验证

```bash
# 识别 NVMe 设备
nvme list
# /dev/nvme0n1 应出现，容量约 1GB，4K LBA

# 查看控制器信息
nvme id-ctrl /dev/nvme0

# 写入测试
dd if=/dev/urandom of=/dev/nvme0n1 bs=4k count=100 oflag=direct

# 读取验证
dd if=/dev/nvme0n1 of=/tmp/read.bin bs=4k count=100 iflag=direct

# 查看 SMART 日志（含多温度传感器读数）
nvme smart-log /dev/nvme0

# 查看固件插槽信息
nvme fw-log /dev/nvme0

# fio 性能测试
fio --name=randread --filename=/dev/nvme0n1 --rw=randread \
    --bs=4k --iodepth=32 --runtime=10 --time_based --direct=1
```

### 命令行参数

| 参数 | 说明 |
|------|------|
| `--vhost-user` | 启用 vhost-user NVMe 后端模式 |
| `--vhost-socket=<path>` | 指定 Unix socket 路径（默认 `/tmp/ftl-vhost-user.sock`） |
| `--debug` | 设置日志级别为 INFO |
| `--trace` | 设置日志级别为 DEBUG |

### 协议握手流程

```
QEMU (前端)                          固件 (后端)
    |                                    |
    |---- GET_FEATURES ----------------->|
    |<--- features (bit30) --------------|
    |---- SET_FEATURES ----------------->|
    |---- GET_PROTOCOL_FEATURES -------->|
    |<--- protocol_features -------------|
    |---- SET_PROTOCOL_FEATURES -------->|
    |---- SET_OWNER -------------------->|
    |---- SET_MEM_TABLE (+FDs) --------->|  mmap 所有内存区域
    |---- SET_VRING_NUM ---------------->|
    |---- SET_VRING_ADDR --------------->|
    |---- SET_VRING_BASE --------------->|
    |---- SET_VRING_KICK (+eventfd) ---->|
    |---- SET_VRING_CALL (+eventfd) ---->|
    |---- SET_VRING_ENABLE ------------->|
    |---- SET_CONFIG (CC.EN=1) --------->|  CSTS.RDY=1
    |                                    |
    |<=== 运行时数据传输 ===>           |
    |---- SET_CONFIG (doorbell) -------->|
    |                                    |  从 vring 取命令
    |                                    |  处理命令 + PRP 数据传输
    |                                    |  写 used ring
    |<--- call eventfd (中断) -----------|
```

### 与 NVMe/TCP 模式的对比

| 特性 | NVMe/TCP 模式 | vhost-user 模式 |
|------|---------------|----------------|
| 传输层 | TCP 网络 (端口4420) | Unix Socket + 共享内存 |
| QEMU 设备 | 需虚拟机内 nvme-tcp 驱动连接 | QEMU 直接模拟 PCIe NVMe 设备 |
| 延迟 | 较高（TCP协议栈开销） | 较低（共享内存零拷贝） |
| 适用场景 | 跨主机/网络存储 | 本地虚拟机存储加速 |
| 启动参数 | 默认模式（无需参数） | `--vhost-user` |

两种模式可同时编译，通过命令行参数选择运行模式。

## NVMe over PCIe (vfio-user) 使用指南

本固件实现了 vfio-user NVMe PCIe 后端，可作为 QEMU 虚拟机的完整 PCIe NVMe 设备运行。与 vhost-user 基于 virtio 协议不同，vfio-user 模拟完整的 PCIe 设备（配置空间、BAR、MSI-X 中断、DMA），QEMU 通过 `vfio-user-pci` 设备连接本后端，虚拟机内使用标准 `nvme` 内核驱动即可识别为真实 PCIe NVMe 设备。

### 架构概述

**vfio-user 协议简介：**
vfio-user 是 QEMU 提供的用户态 VFIO 后端协议，后端进程通过 Unix domain socket 与 QEMU 通信，模拟完整的 PCIe 设备。QEMU 将虚拟机对 PCIe 配置空间和 BAR 空间的读写转发为 vfio-user 消息，后端处理后回复；DMA 和中断通过共享内存和 eventfd 实现。

**与 vhost-user 的区别：**
- vfio-user 模拟完整 PCIe 设备（配置空间、BAR0、MSI-X、DMA），Guest 内加载标准 `nvme.ko` 驱动
- vhost-user 基于 virtio 协议，QEMU 的 `vhost-user-nvme` 设备内部完成 virtio→NVMe 转换
- vfio-user 更接近真实硬件行为，适合 PCIe 驱动开发、协议一致性测试和性能基准测试

**与 NVMe/TCP 的区别：**
- vfio-user 是本地 PCIe 总线级对接，通过 Unix socket + 共享内存通信，延迟最低
- NVMe/TCP 是网络协议，通过 TCP/IP 栈传输，支持跨主机远程访问

**三种对接方式对比：**

| 维度 | NVMe/TCP | vhost-user | vfio-user |
|------|----------|------------|-----------|
| 协议层 | TCP/IP 网络协议 | virtio 协议 | PCIe 总线协议 |
| 延迟 | 较高（网络栈） | 低（共享内存） | 最低（PCIe MMIO） |
| 吞吐量 | 受网络带宽限制 | 高 | 最高 |
| QEMU 设备 | 需 Guest 内 nvme-tcp 驱动 | `vhost-user-nvme` | `vfio-user-pci` |
| 内核驱动依赖 | `nvme-tcp.ko` | `vhost` + `nvme` | `nvme.ko`（标准） |
| 适用场景 | 网络存储、远程访问 | virtio 生态集成 | PCIe 设备模拟、驱动测试 |
| 配置复杂度 | 中（需配置网络） | 中 | 低（socket 即可） |

### 架构

```
┌──────────────────────────────────────────────────────┐
│  QEMU 虚拟机 (Linux Guest)                            │
│  ┌────────────────────────────────────────────────┐  │
│  │  nvme 内核驱动 + nvme-cli / dd / fio           │  │
│  └───────────────────┬────────────────────────────┘  │
│                      │ PCIe (配置空间/BAR0/MSI-X)      │
│  ┌───────────────────▼────────────────────────────┐  │
│  │  QEMU vfio-user-pci 设备                        │  │
│  └───────────────────┬────────────────────────────┘  │
└──────────────────────┼───────────────────────────────┘
                       │ Unix Socket (控制面)
                       │ + 共享内存 (DMA 数据面)
┌──────────────────────▼───────────────────────────────┐
│  ftl-firmware (vfio-user 后端)                         │
│  ┌────────────────────────────────────────────────┐  │
│  │  vfio_user_nvme.c (协议握手/PCIe模拟/MSI-X/DMA) │  │
│  │  GET_API_VERSION → DEVICE_GET_INFO →           │  │
│  │  DEVICE_GET_REGION_INFO → DMA_MAP →            │  │
│  │  DEVICE_SET_IRQS → REGION_READ/WRITE           │  │
│  └───────────────────┬────────────────────────────┘  │
│                      │                                 │
│  ┌───────────────────▼────────────────────────────┐  │
│  │  NVMe 控制器 (nvme_controller.c)                │  │
│  │  Admin/I/O 命令处理 → FTL → NAND                │  │
│  └────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────┘
```

### 快速开始

#### 1. 编译运行固件（vfio-user 模式）

```bash
cd ftl-firmware
make

# 启动 vfio-user 后端模式
/tmp/ftl-firmware-build/ftl_firmware --vfio-user
# 固件监听 /tmp/ftl-vfio-user.sock，等待 QEMU 连接

# 自定义 socket 路径
/tmp/ftl-firmware-build/ftl_firmware --vfio-user --vfio-socket=/tmp/my-nvme.sock
```

#### 2. 启动 QEMU 虚拟机

```bash
# 必须使用共享内存后端（memory-backend-file,share=on），否则 DMA 映射失败
qemu-system-x86_64 \
    -machine q35,accel=kvm \
    -cpu host -m 2048 -smp 2 \
    -object memory-backend-file,id=mem,size=2G,mem-path=/dev/shm,share=on \
    -numa node,memdev=mem \
    -drive file=/path/to/guest.img,format=qcow2 \
    -chardev socket,id=vfio0,path=/tmp/ftl-vfio-user.sock \
    -device vfio-user-pci,chardev=vfio0 \
    -net nic -net user,hostfwd=tcp::2222-:22 \
    -nographic
```

**参数说明：**
- `-chardev socket,id=vfio0,path=...`：创建指向 vfio-user socket 的字符设备
- `-device vfio-user-pci,chardev=vfio0`：创建 vfio-user PCIe 设备，QEMU 自动完成 PCIe 枚举
- QEMU 需支持 `vfio-user-pci` 设备（较新版本）

#### 3. 虚拟机内验证

```bash
# 1. 识别 PCIe NVMe 设备
lspci -nn
# 预期输出包含：
# 00:04.0 Non-Volatile memory controller [0108]: Red Hat, Inc. Device [1b36:0010]

# 2. 识别 NVMe 命名空间
nvme list
# /dev/nvme0n1 应出现，容量约 1GB，4K LBA

# 3. 查看控制器信息
nvme id-ctrl /dev/nvme0

# 4. 写入测试
dd if=/dev/urandom of=/dev/nvme0n1 bs=4k count=100 oflag=direct

# 5. 读取验证
dd if=/dev/nvme0n1 of=/tmp/read.bin bs=4k count=100 iflag=direct

# 6. fio 性能测试
fio --name=randread --filename=/dev/nvme0n1 --rw=randread \
    --bs=4k --iodepth=32 --runtime=10 --time_based --direct=1
```

### PCIe 配置空间说明

后端模拟完整的 PCIe Type 0 配置空间（256 字节），关键字段如下：

| 偏移 | 字段 | 值 | 说明 |
|------|------|-----|------|
| 0x00 | Vendor ID | 0x1B36 | Red Hat |
| 0x02 | Device ID | 0x0010 | NVMe 设备 |
| 0x04 | Command | 0x0006 | IO Space + Memory Space enable |
| 0x06 | Status | 0x0010 | Capabilities List |
| 0x08 | Revision ID | 0x01 | 修订版本 |
| 0x09 | Class Code | 0x010802 | Mass Storage / NVMe Controller |
| 0x0C | Cache Line Size | 0x10 | 16 字节 |
| 0x0E | Header Type | 0x00 | Type 0（普通设备） |
| 0x10 | BAR0 (低32位) | 0xFFFF000C | 64-bit 内存空间，prefetchable，64KB |
| 0x14 | BAR1 (高32位) | 0x00000000 | BAR0 高地址（32位地址空间内为0） |
| 0x2C | Subsystem Vendor ID | 0x1B36 | 子系统厂商 ID |
| 0x2E | Subsystem ID | 0x0010 | 子系统 ID |
| 0x34 | Capability Pointer | 0x40 | MSI-X Capability 偏移 |
| 0x40 | MSI-X Cap ID | 0x11 | MSI-X Capability 标识 |
| 0x42 | MSI-X Msg Ctrl | 0x8000 | Enable=1, Table Size=0（1个向量） |
| 0x44 | MSI-X Table Offset | 0x00002000 | BAR0 内偏移 0x2000 |
| 0x48 | MSI-X PBA Offset | 0x00003000 | BAR0 内偏移 0x3000 |

### BAR0 布局

BAR0 为 64KB 64-bit prefetchable 内存空间，布局如下：

```
偏移        大小    用途
0x0000      4KB     NVMe 控制器寄存器 (CAP, VS, CC, CSTS, AQA, ASQ, ACQ 等)
0x1000      4KB     Doorbell 寄存器 (Admin + I/O 队列的 SQ 尾指针/CQ 头指针)
0x2000      4KB     MSI-X 中断向量表 (每个向量16字节，当前1个向量)
0x3000      4KB     MSI-X Pending Bit Array (PBA)
0x4000      48KB    预留（未使用）
```

### NVMe 寄存器映射说明

BAR0 偏移 0x0000 处为 NVMe 控制器寄存器（4KB），关键字段：

| BAR0 偏移 | 寄存器 | 宽度 | 说明 |
|-----------|--------|------|------|
| 0x0000 | CAP | 64-bit | Controller Capabilities（队列深度、MPSMIN、AMS 等） |
| 0x0008 | VS | 32-bit | Version（NVMe 1.4 = 0x00010400） |
| 0x000C | INTMS | 32-bit | Interrupt Mask Set（传统中断掩码，MSI-X 模式下不使用） |
| 0x0010 | INTMC | 32-bit | Interrupt Mask Clear |
| 0x0014 | CC | 32-bit | Controller Configuration（EN、CSS、IOCQES、IOSQES 等） |
| 0x001C | CSTS | 32-bit | Controller Status（RDY、CFS、SHST 等） |
| 0x0020 | NSSR | 32-bit | NVM Subsystem Reset |
| 0x0024 | AQA | 32-bit | Admin Queue Attributes（ASQS、ACQS） |
| 0x0028 | ASQ | 64-bit | Admin SQ Base Address（Guest 物理地址） |
| 0x0030 | ACQ | 64-bit | Admin CQ Base Address（Guest 物理地址） |

BAR0 偏移 0x1000 处为 Doorbell 寄存器，每个队列占 8 字节（SQ Tail Doorbell 4 字节 + CQ Head Doorbell 4 字节）：
- Admin 队列：0x1000 (SQyTDBL) / 0x1004 (CQyHDBL)
- I/O 队列 1：0x1008 / 0x100C
- I/O 队列 N：0x1000 + N×8 / 0x1004 + N×8

### 与 vhost-user 的区别和适用场景

| 维度 | vfio-user | vhost-user | NVMe/TCP |
|------|-----------|------------|----------|
| 设备模拟 | 完整 PCIe 设备（配置空间+BAR+MSI-X） | virtio 设备（vring+共享内存） | 网络目标端 |
| Guest 驱动 | 标准 nvme.ko | nvme.ko（QEMU 内部转换） | nvme-tcp.ko |
| 命令路径 | MMIO 写门铃 → SQ 读取 → 命令处理 | SET_CONFIG 门铃 → vring 取命令 | TCP PDU 解析 |
| 中断方式 | MSI-X eventfd | call eventfd | TCP 响应 |
| 适用场景 | PCIe 驱动开发、协议一致性测试、性能基准 | virtio 生态、与现有 virtio 工具链集成 | 网络存储、远程访问、跨主机 |

### 命令行参数

| 参数 | 说明 |
|------|------|
| `--vfio-user` | 启用 vfio-user NVMe PCIe 后端模式 |
| `--vfio-socket=<path>` | 指定 Unix socket 路径（默认 `/tmp/ftl-vfio-user.sock`） |
| `--vhost-user` | 启用 vhost-user NVMe 后端模式 |
| `--vhost-socket=<path>` | 指定 vhost-user socket 路径 |
| `--no-nvme-tcp` | 禁用 NVMe/TCP 服务 |
| `--tcp-port=<port>` | 指定 NVMe/TCP 监听端口（默认 4420） |
| `--ufs` | 启用 UFS 目标端 |
| `--internal` | 启用内部命令队列（host_if 模块） |
| `--ftl-test` | 启用 FTL 单元测试后台任务 |
| `--gc-bench` | 启用 GC 基准测试后台任务 |
| `--no-test` | 禁用所有后台测试任务 |
| `--no-heartbeat` | 禁用心跳监控任务 |
| `--no-snapshot` | 禁用掉电快照保存 |
| `--debug` | 设置日志级别为 INFO |
| `--trace` | 设置日志级别为 DEBUG |
| `--help` | 显示完整帮助信息 |

### 典型使用场景

```bash
# 最小化 vfio-user 模式（PCIe 调试用，不跑任何后台任务）
./ftl_firmware --vfio-user --no-nvme-tcp --no-test --no-heartbeat

# vfio-user + FTL 单元测试
./ftl_firmware --vfio-user --no-nvme-tcp --ftl-test

# 默认 NVMe/TCP 模式
./ftl_firmware

# 启用 UFS
./ftl_firmware --ufs
```

## IPC 消息队列

### 消息类型

| 消息类型 | 说明 |
|---------|------|
| MSG_TYPE_NAND_READ/WRITE/ERASE | NAND 操作请求 |
| MSG_TYPE_NAND_READ_RESP/WRITE_RESP/ERASE_RESP | NAND 操作响应 |
| MSG_TYPE_FTL_READ/WRITE/TRIM | FTL 操作请求 |
| MSG_TYPE_FTL_READ_RESP/WRITE_RESP/TRIM_RESP | FTL 操作响应 |
| MSG_TYPE_HOST_CMD/HOST_CMD_COMPLETE | 主机命令 |
| MSG_TYPE_MGR_HEALTH_CHECK/ERROR_REPORT/CONFIG | 管理消息 |
| MSG_TYPE_LOG_WRITE | 日志写入请求 |

### 消息优先级

| 优先级 | 说明 |
|-------|------|
| MSG_PRIORITY_LOW | 低优先级 |
| MSG_PRIORITY_NORMAL | 普通优先级 |
| MSG_PRIORITY_HIGH | 高优先级 |
| MSG_PRIORITY_URGENT | 紧急优先级 |

### 模块 ID

| 模块 ID | 模块名称 |
|---------|---------|
| MODULE_NAND | NAND 模块 |
| MODULE_FTL | FTL 模块 |
| MODULE_HOST_IF | 主机接口模块 |
| MODULE_MANAGER | 管理模块 |
| MODULE_LOG | 日志模块 |

## 系统特性

### 健康监控
- 每个模块定期上报健康状态
- 管理模块统一监控所有模块
- 支持健康状态分级（健康/警告/严重）

### 错误处理
- 模块错误上报机制
- 错误计数和统计
- 错误级别分类

### 自动恢复
- 错误超过阈值自动触发恢复
- 模块复位和重新初始化
- 可配置的恢复策略

### 看门狗
- 心跳机制
- 超时检测
- 异常模块处理

### 配置管理
- 集中式配置管理
- 运行时配置更新
- 配置持久化

## 代码规范

本项目遵循固件编码规范：

### 命名规范

| 类型 | 规范 | 示例 |
|------|------|------|
| 宏定义 | 全大写 + 模块前缀 | `NAND_PAGE_SIZE` |
| 函数 | 蛇形命名 + 模块前缀 | `nand_page_read` |
| 变量 | 蛇形命名 | `erase_count` |
| 类型 | 加 _t 后缀 | `ret_code_t` |
| 枚举值 | 全大写 + 前缀 | `BLOCK_STATE_FREE` |

### 代码风格

- 变量统一声明在函数开头（C89 风格）
- 使用 4 空格缩进，不使用 Tab
- 大括号独占一行
- 指针符号 `*` 紧跟变量名
- 每行不超过 120 字符

### 注释规范

- 所有公共接口必须有 Doxygen 风格注释
- 关键逻辑必须有行间注释解释设计思路
- 复杂算法必须有原理说明
- 注释使用中文，保持简洁清晰

### 错误处理

- 统一使用 `ret_code_t` 错误码
- 所有接口必须进行入参校验
- 错误必须向上传递或正确处理
- 关键操作必须有错误日志

## 开发指南

### 添加新模块

1. 在 `modules/` 下创建新模块目录
2. 创建模块头文件（.h）和源文件（.c）
3. 在头文件中定义模块接口和数据结构
4. 在源文件中实现模块功能
5. 在 `Makefile` 中添加源文件路径
6. 在 `ipc/msg_queue.h` 中添加模块 ID（如果需要 IPC）

### 添加新的消息类型

1. 在 `ipc/msg_queue.h` 中添加消息类型枚举
2. 在 `message_t` 的 union 中添加消息数据结构
3. 在目标模块中实现消息处理函数

### 调试技巧

- 使用 `LOG_DEBUG` 宏输出调试信息
- 使用 `nand_print_stats()` 打印 NAND 统计信息
- 使用 `manager_print_module_status()` 打印模块状态
- 使用 `host_if_print_stats()` 打印主机接口统计

## 与 ftl-simulator 的区别

| 特性 | ftl-simulator | ftl-firmware |
|------|--------------|--------------|
| 架构 | 单进程，函数调用 | 模块化，IPC 通信 |
| 模块独立性 | 低，耦合紧密 | 高，独立模块 |
| 错误隔离 | 无 | 模块级错误隔离 |
| 健康监控 | 无 | 完整的健康监控体系 |
| 自动恢复 | 无 | 支持自动恢复 |
| 可扩展性 | 一般 | 优秀 |
| 系统特性 | 基础 | 完整 |

## 多任务架构（FreeRTOS 风格）

本项目采用 FreeRTOS 风格的多任务调度架构，每个功能模块独立线程运行，通过消息队列通信，避免共享数据竞争。

### 任务列表

| 任务名 | 优先级 | 栈大小 | 职责 |
|--------|--------|--------|------|
| NVMe-TCP-Service | HIGH (3) | 64KB | NVMe/TCP 前端接口服务，处理主机命令 |
| Heartbeat-Monitor | NORMAL (2) | 16KB | 心跳与健康监控，定期保存快照，WAF统计 |
| FTL-Unit-Test | LOW (1) | 32KB | FTL 层单元测试（后台验证） |
| GC-Benchmark | LOW (1) | 32KB | GC 算法性能基准测试（后台分析） |
| Task-Monitor | IDLE (0) | 8KB | 任务状态监控，定期打印任务表 |

### 任务管理框架

- **任务控制块(TCB)**：参考 FreeRTOS TCB，包含名称、入口、优先级、栈大小、状态、运行次数
- **任务表**：静态分配 MAX_TASKS=16 个任务槽位，避免动态内存碎片
- **优先级**：5级优先级（IDLE/LOW/NORMAL/HIGH/REALTIME），数值越大优先级越高
- **任务状态**：READY/RUNNING/BLOCKED/SUSPENDED/FINISHED
- **OS 抽象层**：通过 `os_thread_create()` 创建线程，Linux 平台基于 POSIX pthread，可移植到 FreeRTOS

### 任务间通信

- **消息队列(msg_queue)**：模块间通过消息队列异步通信，支持优先级排序
- **互斥锁(os_mutex)**：保护共享资源，避免竞态条件
- **无共享数据**：任务间不直接共享全局变量，所有数据通过消息传递
## 扩展方向

1. ~~**接入真实 NVMe 协议** - 对接 QEMU 或真实硬件~~ ✅ QEMU vhost-user 已完成
2. ~~**多线程支持** - 每个模块独立线程运行~~ ✅ 已完成
3. **共享内存** - 高性能数据传输
4. ~~**DMA 模拟** - 模拟 DMA 传输~~ ✅ 已完成
5. ~~**中断处理** - 模拟中断机制~~ ✅ vfio-user MSI-X 已实现
6. ~~**温度管理** - 温度监控和热管理~~ ✅ 已完成
7. ~~**电源管理** - 不同功耗状态管理~~ ✅ 已完成
8. ~~**安全功能** - 加密、签名、安全擦除~~ ✅ 安全擦除已完成
9. **RAID 支持** - RAID 级别的数据保护
10. ~~**性能分析** - 性能监控和调优工具~~ ✅ 已完成
11. **SPDK 对接** - 将 FTL 层作为 SPDK Bdev 后端，通过 SPDK NVMe-oF/vhost 协议栈导出（规划中，详见 [docs/SPDK对接技术规划.md](docs/SPDK对接技术规划.md)）

## 版本历史

### v2.5.0 (2026-09-20)
- **架构优化：主机接口按需启用**：新增统一运行时配置结构体 `fw_runtime_config_t`，所有主机接口（NVMe/TCP、vfio-user、vhost-user、UFS、Internal）可通过命令行参数独立启用/禁用，不再无条件初始化所有模块
- **条件主循环**：主循环只调用已启用接口的 process 函数，vfio-user 模式下不再空转调用 `host_if_process()`
- **条件任务注册**：后台任务（心跳监控、FTL单元测试、GC基准测试）可通过命令行参数独立启用/禁用
- **OSAL 完善（FreeRTOS 移植就绪）**：新增消息队列 `os_queue_create/send/receive`、事件标志组 `os_event_group_set/wait`、任务通知 `os_task_notify/notify_wait`，Linux 平台基于 POSIX 实现，移植 FreeRTOS 只需替换底层实现
- **PRP 读取 bug 修复**：修复 `prp_read_data()` 中多余的 PRP2 页对齐检查导致多页写入从第二页开始数据错误的问题，与 `prp_write_data()` 行为一致
- **新增命令行参数**：`--no-nvme-tcp`、`--tcp-port`、`--ufs`、`--internal`、`--ftl-test`、`--gc-bench`、`--no-test`、`--no-heartbeat`、`--no-snapshot`、`--help`
- 消除所有编译 warning

### v2.4.2 (2026-09-18)
- **vfio-user PCIe 对接完整打通**：QEMU vfio-user-pci 设备成功识别 NVMe 控制器，/dev/nvme0n1 出现并完成读写验证（dd 读写 + cmp 数据一致性校验通过）
- **PRP 列表遍历修复**：修复多页传输时 PRP2 非页对齐导致只写入前2页数据的 bug，只要传输超过2页就将 PRP2 当作列表指针处理，解决内核 hang 问题
- **完成条目 Phase Tag 位置确认**：通过实验确认 Linux 内核 NVMe 驱动从 Status Field (bytes 14-15) bit0 读取 Phase Tag，删除了结构体中错误的 sqid 字段
- **DMA 共享内存配置**：QEMU 必须使用 memory-backend-file,share=on 传递 Guest 内存 FD，后端 mmap 后直接访问
- **新增调试文档**：docs/vfio_user_pcie_debug_guide.md，完整记录 vfio-user PCIe 对接过程中遇到的 20+ 个问题、根因分析和解决方案
- 清理调试日志，完善代码注释

### v2.4.1 (2026-09-08)
- **新增 SPDK 对接技术规划文档**：详细规划 ftl-firmware 与 SPDK 的对接方案，选择将 FTL 层作为 SPDK Bdev 后端的架构，包含七阶段实施路线（库化→SPDK入门→适配层→Bdev模块→协议导出→性能测试→文档收尾）、五大技术难点解决方案、项目亮点与面试价值分析
- 新增文件：docs/SPDK对接技术规划.md

### v2.4.0 (2026-09-08)
- **QEMU vfio-user NVMe PCIe 后端**：实现完整 vfio-user 协议栈，模拟真实 PCIe NVMe 设备（256字节配置空间、64KB BAR0、MSI-X 中断、DMA 映射），与 QEMU vfio-user-pci 设备对接，虚拟机内标准 nvme.ko 驱动识别。通过 Unix socket 传输控制消息，SCM_RIGHTS 传递内存 FD，mmap 建立 GPA→HVA 映射，eventfd 触发 MSI-X 中断
- 新增文件：include/protocol/vfio_user_nvme.h, src/protocol/nvme/vfio_user_nvme.c, docs/vfio_user_design_notes.md
- 完善 Doxygen 函数注释和行间注释

### v2.3.0 (2026-09-08)
- **QEMU vhost-user NVMe 后端**：实现完整 vhost-user 协议栈，支持 QEMU vhost-user-nvme 设备对接，虚拟机内识别标准 NVMe 设备并进行读写。通过 Unix socket + 共享内存实现低延迟数据传输，PRP 指针直接访问 Guest 物理内存，call eventfd 触发虚拟机中断
- **AER 异步事件请求完整实现**：事件队列(环形缓冲区大小16)、最多4并发AER命令、事件掩码(Set Features FID=0x0B)、支持 Error/SMART/Notice/ANA 四种事件类型，完成 dw0 字段符合 NVMe 规范
- **多温度传感器支持**：8个温度传感器(Composite/NAND Ch0/NAND Ch1/DRAM/PCB Ambient/Power/2保留)，随机游走温度模拟+I/O负载影响，警告阈值(默认70°C)/临界阈值(默认85°C)，超温触发 critical_warning + AER SMART 事件 + 热节流，SMART 日志 temp_sensor[0-7] 正确填充
- **固件更新完整实现**：7固件插槽管理(Slot1运行中+Slot2-7空)，Firmware Download(opcode 0x11)分块下载+偏移连续性校验+16MB上限，Firmware Commit(opcode 0x10)支持CA=0/1/2/3四种动作(替换/下次启动激活/立即激活/设为启动插槽)，Firmware Slot Information Log(LID=0x03)，立即激活触发 AER Notice 事件
- **Identify Controller 字段更新**：aerl=3(最多4并发AER)、frmw=0x11(支持固件更新+Slot Info Log)、oacs=0x0009(支持Security+FW Commit)、oaes=0x01(命名空间属性通知)、wctemp/cctemp动态阈值
- 新增文件：src/protocol/nvme/vhost_user_nvme.c, include/protocol/vhost_user_nvme.h, docs/vhost_user_integration_notes.md
- 完善 Doxygen 函数注释和行间注释

### v2.2.0 (2026-08-20)
- **性能优化**：去掉nand_page_write每次写入的fflush，改为nand_deinit时统一flush，写入性能提升3-5倍
- **ECC纠错**：集成真正的(7,4)汉明码编解码，写入时计算ECC存入OOB，读取时自动校验纠正1位错误，多位错误返回UNCORRECTABLE
- **读干扰处理**：nand_page_read中检测块读取次数超过阈值(100000次)，标记need_reclaim触发read reclaim
- **数据保留模拟**：新增nand_inject_retention_errors函数，模拟高温/长时间存储位错误，可配置错误率验证ECC纠错
- **UFS协议栈框架**：实现UFS目标端，支持SCSI命令集(INQUIRY/READ/WRITE/READ_CAPACITY等)，UPIU事务类型，数据对接FTL层
- **OS抽象层(OSAL)**：统一互斥锁/时间/线程接口，Linux平台基于POSIX实现，预留FreeRTOS/RT-Thread扩展接口
- 完善Doxygen函数注释和行间注释

### v2.1.0 (2026-08-19)
- 集成掉电保护（PLP）自动恢复流程：启动自动从快照恢复、主循环每5000次循环保存快照、退出前保存快照
- 新增 PLP 掉电恢复测试（test_plp_recovery），25项测试100%通过，覆盖快照保存/加载、覆盖写恢复、TRIM恢复、快照不存在容错
- NAND层：恢复模式下不随机生成坏块（坏块出厂固定），扫描OOB区域magic字段重建page_valid和块状态
- NAND层：修复nand_page_write只写数据区未写OOB的bug，写入时同时写入OOB magic标记页有效
- FTL层：完善元数据快照机制，L2P映射表持久化+校验和验证+反向映射表重建
- GitHub Actions CI 增加 PLP 测试自动执行
- 修复test_ftl_unit.c未使用变量编译警告
- 完善代码注释和文档

### v2.0.0 (2026-08-16)
- 实现完整 NVMe/TCP 目标端，与 Linux 内核 nvme-tcp 驱动对接成功
- 实现 IC 握手、Connect、Admin 队列、I/O 队列完整协议栈
- 实现 I/O Read/Write 真实数据路径（Write→FTL→NAND，Read→NAND→FTL→主机）
- 实现 Write Zeroes、Dataset Management(TRIM) 命令
- 实现 Identify Controller/Namespace、Get Log Page(SMART)、Set Features、Keep Alive
- 修复 CapsuleResp phase bit、LBAF 布局、Connect QID 提取等关键协议问题
- 多 LBA 读写验证通过，md5sum 一致
- fio 性能测试通过（顺序读 6.7MB/s，顺序写 5.8MB/s）
- 完善代码注释和文档

### v1.3.0 (2026-08-10)
- 实现多线程支持（线程管理模块，基于 pthread）
- 实现 DMA 模拟（DMA 控制器、通道、传输描述符）
- 集成多线程和 DMA 功能到主程序
- 增加多线程测试和 DMA 测试
- 完善代码注释，增加行间注释

### v1.2.0 (2026-08-10)
- 实现温度管理功能（温度监控、热管理、过热保护）
- 实现电源管理功能（功耗状态、节能模式）
- 实现性能监控功能（IOPS、带宽、延迟统计）
- 实现安全擦除功能（多次覆写+TRIM）
- 完善代码注释，增加行间注释

### v1.1.0 (2026-08-10)
- 优化管理模块，增加看门狗检测功能
- 优化管理模块，增加模块名称显示
- 优化主机接口模块，增加 Write Zeroes 命令支持
- 完善代码注释，增加行间注释
- 修复数据一致性验证失败问题
- 优化代码结构

### v1.0.1 (2026-08-09)
- 修复编译错误（static 函数前置声明缺失）
- 修复重复变量定义
- 修复缺少头文件（stdlib.h）
- 修复所有编译警告
- 优化代码结构
- 完善 README.md 文档

### v1.0.0 (2026-08-09)
- 初始版本
- 基于 ftl-simulator v4.5 重构
- 模块化架构设计
- IPC 消息队列机制
- 管理模块（健康监控、错误处理、自动恢复）
- 主机接口模块（NVMe 命令模拟）
- 工具函数模块


## 性能测试与优化记录

### 测试环境

- **平台**: Linux VMware 虚拟机
- **CPU**: x86_64
- **测试工具**: fio 3.36
- **测试参数**: 4K 块大小, iodepth=1, runtime=5s, time_based
- **协议**: NVMe/TCP (127.0.0.1:4420)

### 性能优化历程

| 优化阶段 | 顺序读 (MB/s) | 顺序写 (MB/s) | 写 IOPS | 写性能提升 | 关键优化 |
|---------|--------------|--------------|---------|-----------|---------|
| 基线版本 (fwrite/fread) | ~7.0 | ~2.5 | ~625 | 1x | 原始实现，每次读写系统调用 |
| NAND层 mmap 优化 | ~6.8 | **32.5** | **~8115** | **13x** | mmap映射文件，消除系统调用开销 |
| NVMe/TCP协议栈优化 | **6.9** | **35.8** | **~9165** | **14.3x** | 预分配IO缓冲区+减少日志输出 |

### 优化技术详解

#### 1. NAND层 mmap 优化

**问题**: 原始实现使用 fseek+fwrite/fread，每次页读写都需要两次系统调用，写性能瓶颈明显。

**方案**: 使用 mmap(MAP_SHARED) 将 NAND 模拟文件映射到进程虚拟地址空间，读写直接 memcpy 到映射内存，由内核异步写回磁盘。

**性能提升原理**:
- 写操作：程序只修改内存页，标记为脏页，实际磁盘 I/O 由内核 pdflush 线程异步完成
- 读操作：利用内核页缓存，大部分读命中缓存
- 消除了每次读写的系统调用开销（上下文切换、内核态/用户态拷贝）

**注意事项**:
- mmap 文件必须放在本地文件系统（如 /tmp），VMware 共享目录（HGFS）不支持 mmap
- 需要 #define _DEFAULT_SOURCE 确保 fdopen/ftruncate 正确声明
- 程序崩溃可能丢失未写回的数据，模拟器场景可接受

#### 2. NVMe/TCP 协议栈优化

**问题**: 每个 I/O 命令都 malloc/free 缓冲区，I/O 路径上频繁 LOG_INFO 输出，影响性能。

**方案**:
- 预分配 1MB 读/写 IO 缓冲区，避免频繁内存分配
- Write Zeroes 使用静态零缓冲区，避免每次 memset(4K)
- 移除 I/O 路径上的 LOG_INFO，只保留错误日志

### 后续优化方向

| 优先级 | 优化项 | 预期收益 | 技术方案 |
|-------|-------|---------|---------|
| P0 | NVMe/TCP 零拷贝 | 读性能提升2-5倍 | 使用 splice/sendfile 减少内存拷贝 |
| P0 | 多队列支持 | 并发性能提升 | 支持多I/O队列和中断向量 |
| P1 | FTL 映射表缓存 | 随机读性能提升 | 热点 L2P 表项缓存到内存 |
| P1 | GC 异步化 | 写延迟降低 | GC 在后台线程执行，不阻塞写路径 |
| P2 | 大页支持 | 吞吐量提升 | 支持 8K/16K 页大小，减少元数据开销 |
| P2 | CPU 亲和性 | 延迟稳定性提升 | 绑定中断和处理线程到特定 CPU 核心 |

## 许可证

MIT License


## 未实现功能与扩展路线图

### P0 - 核心功能（高优先级）

| 功能 | 状态 | 说明 |
|------|------|------|
| QEMU vhost-user 对接 | ✅ 已实现 | vhost-user NVMe后端，QEMU vhost-user-nvme设备对接，共享内存+PRP零拷贝 |
| NVMe 多队列完整支持 | ⚠️ 部分 | 当前支持 Admin+2个I/O队列，需支持多I/O队列和中断向量 |
| NVMe 中断处理 | ✅ 已实现 | vfio-user 模式 MSI-X eventfd 中断已打通 |
| SGL/PRP 数据传输 | ⚠️ 部分 | PRP 完整支持（多页+列表遍历），SGL 禁用（强制 PRP） |
| 命名空间管理 | ⚠️ 部分 | 当前单命名空间，需支持多命名空间、NS Attach/Detach |
| 安全协议(TPer/SED) | ❌ 未实现 | 自加密驱动器支持，TCG Opal 协议 |

### P1 - 系统特性（中优先级）

| 功能 | 状态 | 说明 |
|------|------|------|
| 端到端数据保护(DIF/DIX) | ❌ 未实现 | T10 DIF/DIX，CRC校验+应用标签 |
| 持久化内存区域(PMR) | ❌ 未实现 | NVMe 1.4 Persistent Memory Region |
| 固件更新(Firmware Update) | ✅ 已实现 | 7插槽管理+分块下载+Commit激活+Firmware Slot Log(LID=0x03) |
| 异步事件请求(AER) | ✅ 已实现 | 事件队列+事件掩码+最多4并发，SMART/温度/固件事件上报 |
| 温度传感器(TSensor) | ✅ 已实现 | 8个传感器(Composite/NAND/DRAM/Ambient/Power)，阈值告警+AER事件 |
| 预测性延迟分析 | ❌ 未实现 | 基于机器学习的延迟预测和性能优化 |

### P2 - 性能优化（低优先级）

| 功能 | 状态 | 说明 |
|------|------|------|
| 多通道/多Die并行 | ⚠️ 部分 | 当前单通道模拟，需支持多通道和Die间流水线 |
| 缓存层(DRAM Cache) | ❌ 未实现 | 读写缓存、写回策略、缓存刷新 |
| 压缩/去重 | ❌ 未实现 | 在线数据压缩、重复数据删除 |
| 加密(AES-XTS) | ❌ 未实现 | 全盘加密、AES-XTS 256位 |
| ZNS(Zoned Namespace) | ❌ 未实现 | NVMe Zoned Namespace，SMR 硬盘支持 |
| KV(Key-Value) SSD | ❌ 未实现 | Key-Value 接口，绕过块层 |

### P3 - 平台移植（长期）

| 功能 | 状态 | 说明 |
|------|------|------|
| FreeRTOS 移植 | ✅ 抽象层完整 | OSAL 已具备互斥锁/线程/消息队列/事件组/任务通知，只需实现 FreeRTOS 平台层 |
| RT-Thread 移植 | ⚠️ 抽象层就绪 | OSAL 已预留接口，需实现 RT-Thread 平台层 |
| 裸机部署 | ⚠️ 抽象层就绪 | OSAL 已预留接口，需实现裸机调度器 |
| FPGA 硬件加速 | ❌ 未实现 | ECC/CRC/加解密硬件加速，参考 Cosmos+ OpenSSD |
| 真实 NAND 对接 | ❌ 未实现 | 对接 Toggle DDR NAND 或 ONFI NAND 真实芯片 |

### 测试覆盖扩展

| 测试项 | 状态 | 说明 |
|------|------|------|
| FTL 单元测试 | ✅ 25项 | 初始化、读写、覆盖、TRIM、GC、未写入页 |
| GC 算法基准 | ✅ 6种算法 | Greedy/Cost-Benefit/CAT/Windowed/d-Choices/FRA |
| PLP 恢复测试 | ✅ 25项 | 快照保存/加载、覆盖恢复、TRIM恢复、容错 |
| NVMe 协议一致性测试 | ❌ 未实现 | 对接 nvme-compliance 测试套件 |
| 性能基准测试 | ⚠️ 部分 | fio 顺序/随机读写，需扩展 4K/8K/16K 混合负载 |
| 压力测试 | ❌ 未实现 | 长时间运行、内存泄漏、错误注入测试 |
| 故障注入测试 | ❌ 未实现 | NAND 读错误、写失败、擦除失败、掉电模拟 |
