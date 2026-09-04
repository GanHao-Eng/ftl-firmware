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
- ✅ Linux平台实现（基于POSIX pthread）
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

1. **接入真实 NVMe 协议** - 对接 QEMU 或真实硬件
2. ~~**多线程支持** - 每个模块独立线程运行~~ ✅ 已完成
3. **共享内存** - 高性能数据传输
4. ~~**DMA 模拟** - 模拟 DMA 传输~~ ✅ 已完成
5. **中断处理** - 模拟中断机制
6. ~~**温度管理** - 温度监控和热管理~~ ✅ 已完成
7. ~~**电源管理** - 不同功耗状态管理~~ ✅ 已完成
8. ~~**安全功能** - 加密、签名、安全擦除~~ ✅ 安全擦除已完成
9. **RAID 支持** - RAID 级别的数据保护
10. ~~**性能分析** - 性能监控和调优工具~~ ✅ 已完成

## 版本历史

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
| NVMe 多队列完整支持 | ⚠️ 部分 | 当前支持 Admin+1个I/O队列，需支持多I/O队列和中断向量 |
| NVMe 中断处理 | ❌ 未实现 | 当前轮询模式，需实现 MSI-X 中断和中断处理线程 |
| SGL/PRP 数据传输 | ⚠️ 部分 | 当前简化实现，需完整支持 SGL(Scatter Gather List) |
| 命名空间管理 | ⚠️ 部分 | 当前单命名空间，需支持多命名空间、NS Attach/Detach |
| 安全协议(TPer/SED) | ❌ 未实现 | 自加密驱动器支持，TCG Opal 协议 |

### P1 - 系统特性（中优先级）

| 功能 | 状态 | 说明 |
|------|------|------|
| 端到端数据保护(DIF/DIX) | ❌ 未实现 | T10 DIF/DIX，CRC校验+应用标签 |
| 持久化内存区域(PMR) | ❌ 未实现 | NVMe 1.4 Persistent Memory Region |
| 固件更新( Firmware Update) | ⚠️ 框架 | 命令已占位，需实现固件下载/激活/回滚 |
| 异步事件请求(AER) | ⚠️ 框架 | 命令已占位，需实现事件上报机制 |
| 温度传感器(TSensor) | ⚠️ 部分 | 需实现多温度传感器和温度阈值告警 |
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
| FreeRTOS 移植 | ⚠️ 抽象层就绪 | OSAL 已预留接口，需实现 FreeRTOS 平台层 |
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
