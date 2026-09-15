# vfio-user NVMe PCIe 后端实现细节与调试笔记

## 1. 概述

本文档记录 ftl-firmware 项目中 vfio-user NVMe PCIe 后端的协议实现细节、QEMU 交互流程、关键数据结构和调试技巧。

**相关文件：**
- `include/protocol/vfio_user_nvme.h` — 协议头文件（消息 ID、数据结构、常量定义）
- `src/protocol/nvme/vfio_user_nvme.c` — 后端实现（约1900行）
- `docs/详细设计文档.md` 第 3.6 节 — 协议层设计

**当前状态（2026-09-16）：**
- QEMU vfio-user-pci 设备已成功识别，SeaBIOS/iPXE 正常启动
- PCIe 枚举流程完整走通（VERSION → DMA_MAP → GET_INFO → GET_REGION_INFO → GET_IRQ_INFO → SET_IRQS → REGION_READ/WRITE）
- 设备在 Guest 中显示为 PCI 00:03.0
- 待解决：DMA_UNMAP 超时、Guest NVMe 驱动初始化验证

---

## 2. vfio-user 协议消息格式（对齐 QEMU 标准）

### 2.1 消息通用头部（16 字节）

```c
typedef struct {
    uint16_t id;           // 请求/回复 ID（匹配请求与回复）
    uint16_t command;      // 消息类型（vfu_msg_id_t）
    uint32_t size;         // 整个消息大小（含头部）
    uint32_t flags;        // 消息标志
    uint32_t error_reply;  // 错误码（回复时使用，0=成功）
} vfu_msg_hdr_t;
```

**flags 定义：**
| 值 | 名称 | 说明 |
|----|------|------|
| 0x0 | REQUEST | 请求消息 |
| 0x1 | REPLY | 回复消息 |
| 0x10 | NO_REPLY | 无需回复 |
| 0x20 | ERROR | 错误回复 |

**关键点：** `size` 字段是**整个消息大小（含头部）**，不是 payload 大小。

### 2.2 消息 ID 列表（对齐 QEMU user-protocol.h）

| ID | 名称 | 方向 | 功能 |
|----|------|------|------|
| 1 | VFU_VERSION | C→S | 协议版本协商 |
| 2 | VFU_DMA_MAP | C→S | 映射 DMA 内存区域 |
| 3 | VFU_DMA_UNMAP | C→S | 解除 DMA 内存映射 |
| 4 | VFU_DEVICE_GET_INFO | C→S | 获取设备信息 |
| 5 | VFU_DEVICE_GET_REGION_INFO | C→S | 获取 region 信息 |
| 6 | VFU_DEVICE_GET_REGION_IO_FDS | C→S | 获取 region mmap FD |
| 7 | VFU_DEVICE_GET_IRQ_INFO | C→S | 获取 IRQ 信息 |
| 8 | VFU_DEVICE_SET_IRQS | C→S | 设置 IRQ（eventfd） |
| 9 | VFU_REGION_READ | C→S | 读 region |
| 10 | VFU_REGION_WRITE | C→S | 写 region |
| 11 | VFU_DMA_READ | C→S | 后端读 guest 内存 |
| 12 | VFU_DMA_WRITE | C→S | 后端写 guest 内存 |
| 13 | VFU_DEVICE_RESET | C→S | 设备复位 |
| 14 | VFU_DIRTY_PAGES | C→S | 脏页追踪（迁移） |
| 15 | VFU_REGION_WRITE_MULTI | C→S | 批量写 |

### 2.3 关键数据结构

#### VFU_VERSION 回复
```c
struct { uint16_t major; uint16_t minor; }  // major=0, minor=0
```

#### VFU_DEVICE_GET_INFO 回复（20 字节）
```c
typedef struct {
    uint32_t argsz;        // = 20
    uint32_t flags;        // 0x2 = VFIO_DEVICE_FLAGS_PCI
    uint32_t num_regions;  // = 9（标准 PCI 设备）
    uint32_t num_irqs;     // = 3（INTx/MSI/MSI-X）
    uint32_t cap_offset;   // = 0
} vfu_device_info_t;
```

#### VFU_DEVICE_GET_REGION_INFO（32 字节）
```c
typedef struct {
    uint32_t argsz;        // = 32
    uint32_t flags;
    uint32_t index;        // 请求时输入，回复时回显
    uint32_t cap_offset;
    uint64_t size;         // region 大小
    uint64_t offset;       // mmap 偏移
} vfu_region_info_t;
```

**Region 索引（本版本 Linux vfio.h 枚举顺序）：**
| index | 用途 | size |
|-------|------|------|
| 0 | BAR0 | 65536 (64KB) |
| 1-5 | BAR1-BAR5 | 0（未使用） |
| 6 | ROM | 0（未使用） |
| 7 | PCI Config | 256 |
| 8 | VGA | 0（未使用） |

> **重要**：本版本 `/usr/include/linux/vfio.h` 的枚举顺序是 BAR0=0, ROM=6, CONFIG=7，与传统顺序（CONFIG=0, BAR0=1）相反。必须以系统头文件为准。

#### VFU_DEVICE_GET_IRQ_INFO（16 字节）
```c
typedef struct {
    uint32_t argsz;        // = 16
    uint32_t flags;
    uint32_t index;        // 0=INTx, 1=MSI, 2=MSI-X
    uint32_t count;        // 向量数
} vfu_irq_info_t;
```

#### VFU_DEVICE_SET_IRQS（变长）
```c
typedef struct {
    uint32_t argsz;
    uint32_t flags;        // bit2=EVENTFD 数据类型
    uint32_t index;
    uint32_t start;
    uint32_t count;
    uint8_t  data[1];      // eventfd 通过 SCM_RIGHTS 传递
} vfu_set_irqs_t;
```

#### VFU_REGION_READ/WRITE（变长）
```c
typedef struct {
    uint64_t offset;       // region 内偏移
    uint32_t region;       // region 索引
    uint32_t count;        // 字节数
    uint8_t  data[1];      // 数据（变长）
} vfu_region_rw_t;
```

> **重要**：REGION_READ 的**回复**必须包含完整的 `vfu_region_rw_t` 结构（offset+region+count+data），不是只返回数据。QEMU 会把回复解析为 `VFIOUserRegionRW`，从 `data` 字段取数据。

#### VFU_DMA_MAP（32 字节）
```c
typedef struct {
    uint32_t argsz;        // = 32
    uint32_t flags;
    uint64_t offset;       // FD 内偏移
    uint64_t iova;         // guest 物理地址
    uint64_t size;         // 区域大小
} vfu_dma_map_t;
```

#### VFU_DMA_UNMAP（24 字节）
```c
typedef struct {
    uint32_t argsz;        // = 24
    uint32_t flags;
    uint64_t iova;
    uint64_t size;
} vfu_dma_unmap_t;
```

---

## 3. QEMU 编译与使用

### 3.1 支持 vfio-user-pci 的 QEMU 编译

系统 QEMU 8.2.2 不支持 `vfio-user-pci` 设备，需要编译 Oracle vfio-user 分支（基于 QEMU 7.1.5）。

**安装路径**：`/opt/qemu-vfio-user/`
**源码路径**：`/tmp/qemu-vfio-user/`（VM 中）

**编译配置**：
```bash
./configure --target-list=x86_64-softmmu \
  --prefix=/opt/qemu-vfio-user \
  --disable-werror \
  --disable-slirp --disable-capstone --disable-guest-agent \
  --disable-tools --disable-mpath --disable-linux-aio \
  --disable-linux-io-uring --disable-rbd --disable-libiscsi \
  --disable-libnfs --disable-vte --disable-vnc --disable-spice \
  --disable-usb-redir --disable-curl --disable-numa \
  --disable-gtk --disable-sdl --disable-opengl \
  --disable-virglrenderer --disable-curses
make -j$(nproc)
sudo make install
```

**已知编译问题及修复**：
1. GCC 13 枚举索引错误 → 切换 GCC 12，删除 `ui/input-keymap-*.c.inc` 中 F13-F24 行
2. 缺失子模块 → `git init` + 初始提交绕过，手动下载 keycodemapdb
3. `vfio_pci_config_setup` 段错误（config_size=0）→ 在 pci.c:2718 后添加兜底：`if (vdev->config_size < PCI_CONFIG_SPACE_SIZE) vdev->config_size = PCI_CONFIG_SPACE_SIZE;`

### 3.2 启动命令

**固件（终端1）**：
```bash
/tmp/ftl-firmware-build/ftl_firmware \
  --vfio-user \
  --vfio-socket=/tmp/ftl-vfio-user.sock
```

**QEMU（终端2）**：
```bash
/opt/qemu-vfio-user/bin/qemu-system-x86_64 \
  -m 2G -smp 2 \
  -cdrom alpine-virt-3.19.0-x86_64.iso \
  -boot d \
  -device vfio-user-pci,socket=/tmp/ftl-vfio-user.sock \
  -nographic
```

> 注意：设备参数是 `socket=<path>`，不是 `chardev=`。

---

## 4. 完整交互流程

```
阶段1：连接建立
  QEMU connect → 后端 accept

阶段2：版本协商
  QEMU → VERSION (id=0)
  后端 → 回复 {major=0, minor=0}

阶段3：设备信息
  QEMU → DEVICE_GET_INFO (id=6)
  后端 → 回复 {flags=PCI, num_regions=9, num_irqs=3}

  QEMU → GET_REGION_INFO ×9 (id=7~15, index=0~8)
  后端 → 逐个回复 size/offset

  QEMU → GET_IRQ_INFO (id=16, index=2=MSI-X)
  后端 → 回复 count=1

阶段4：PCI 配置空间读取
  QEMU → REGION_READ (region=7, offset=0, count=256)
  后端 → 回复完整 PCIe 配置空间

  QEMU → REGION_READ/WRITE 多次（枚举 BAR、设置 Command）

阶段5：中断设置
  QEMU → SET_IRQS (INTx/MSI/MSI-X)
  后端 → 保存 eventfd（MSI-X 时）

阶段6：DMA 映射
  QEMU → DMA_MAP (iova, size) [本版本无 FD，使用匿名映射]
  后端 → mmap 匿名内存

阶段7：SeaBIOS 枚举
  SeaBIOS → 大量 REGION_READ/WRITE（读取 Vendor ID、Class Code、BAR 大小）
  设备显示为 PCI 00:03.0

阶段8：Guest NVMe 驱动初始化（待验证）
  Guest → 读 CAP 寄存器（BAR0 offset=0）
  Guest → 写 CC.EN=1 → 后端置 CSTS.RDY=1
  Guest → 设置 Admin 队列 → 门铃触发 → 处理 Identify 命令
```

---

## 5. PCIe 配置空间

### 5.1 关键字段

| 偏移 | 字段 | 取值 | 含义 |
|------|------|------|------|
| 0x00 | VID | 0x1B36 | Red Hat Vendor ID |
| 0x02 | DID | 0x0010 | QEMU 标准 NVMe Device ID |
| 0x04 | Command | 0x0006 | Memory Space + Bus Master |
| 0x08-0x0B | Class Code | 0x010802 | Mass Storage / NVMe |
| 0x10 | BAR0 | 0xFFFF000C | 64KB, 64-bit, prefetchable |
| 0x34 | Cap Pointer | 0x40 | MSI-X Capability |
| 0x40 | MSI-X Cap | 0x11 | Cap ID |

### 5.2 BAR0 布局（64KB）

| 偏移 | 大小 | 用途 |
|------|------|------|
| 0x0000 | 4KB | NVMe 寄存器（CAP/CC/CSTS/AQA/ASQ/ACQ...） |
| 0x1000 | 4KB | Doorbell 寄存器（Admin SQ/CQ + IO SQ/CQ） |
| 0x2000 | 4KB | MSI-X 向量表（16 字节/向量） |
| 0x3000 | 4KB | MSI-X PBA（Pending Bit Array） |
| 0x4000 | 48KB | 保留 |

---

## 6. 调试记录（2026-09-15 ~ 09-16）

### 6.1 协议不兼容问题（已解决）

**问题**：固件最初使用自定义消息头 `{msg_id, msg_version, msg_size, msg_flags, msg_errno}`，与 QEMU 标准 `{id, command, size, flags, error_reply}` 不兼容。

**修复**：
- 消息头改为 QEMU 标准格式
- `size` 字段改为整个消息大小（含头部）
- 消息 ID 枚举对齐 QEMU 标准（VERSION=1, DMA_MAP=2, ...）
- 所有数据结构移除 `flags_ext`/`reserved` 等多余字段，对齐 QEMU 标准

### 6.2 "this isn't a PCI device"（已解决）

**原因**：`DEVICE_GET_INFO` 回复的 `flags=0`，缺少 `VFIO_DEVICE_FLAGS_PCI (0x2)`。

**修复**：`info.flags = 0x2`

### 6.3 "unexpected number of io regions 2"（已解决）

**原因**：`num_regions=2`，QEMU 期望标准 PCI 设备有 9 个 region。

**修复**：`num_regions=9`，对未使用的 BAR1-BAR5/ROM/VGA 返回 size=0。

### 6.4 段错误 `emulated_config_bits`（已解决）

**原因**：QEMU `vfio_pci_config_setup` 中 `g_malloc0(vdev->config_size)`，config_size=0 时返回 NULL。

**修复**：QEMU 源码 pci.c 添加兜底，config_size 至少 256。

### 6.5 Region 索引顺序（已解决）

**原因**：固件假设 CONFIG=0, BAR0=1（传统顺序），但本版本 Linux vfio.h 枚举是 BAR0=0, CONFIG=7。

**修复**：调整 region 索引定义，CONFIG=7, BAR0=0。

### 6.6 "reply larger than recv buffer"（已解决）

**原因**：REGION_READ 回复只发送数据，QEMU 期望完整的 `VFIOUserRegionRW` 结构（offset+region+count+data）。

**修复**：回复包含完整的 `vfu_region_rw_t` 结构，`reply_size = 16 + count`。

### 6.7 DMA_MAP 无 FD（待优化）

**现象**：QEMU 这个版本的 DMA_MAP 消息不携带 FD（SCM_RIGHTS），固件使用匿名内存映射降级。

**影响**：guest 内存和固件内存不共享，后续 PRP/DMA 数据传输可能无法正常工作。

**待研究**：QEMU 这个版本的 DMA 映射机制，是否需要通过其他方式获取内存 FD。

### 6.8 "vfio_wait_reqs - timed out"（待解决）

**现象**：SeaBIOS 启动后出现两次超时，可能是 DMA_UNMAP 或其他消息的回复问题。

**影响**：不影响设备基本枚举，但可能影响运行时稳定性。

---

## 7. 常见问题排查

### 7.1 查看消息流

在 `dispatch_message` 中添加日志，打印每条收到的消息：
```c
LOG_WARN("RECV cmd=%u id=%u size=%u", hdr->command, hdr->id, hdr->size);
```

### 7.2 查看原始 payload

对于 REGION_READ 等消息，打印原始 payload 字节确认字段解析：
```c
LOG_WARN("raw=%02x%02x...", payload[0], payload[1], ...);
```

### 7.3 QEMU 段错误定位

```bash
gdb -batch -ex run -ex "bt 20" --args \
  /opt/qemu-vfio-user/bin/qemu-system-x86_64 ...
```

---

## 8. 已知限制

| 限制项 | 说明 |
|--------|------|
| DMA_MAP 无 FD | 使用匿名映射，guest 内存不共享，影响数据传输 |
| 单 MSI-X 向量 | 仅 1 个中断向量 |
| 单 I/O 队列 | Admin + 1 个 I/O 队列 |
| 无 PCIe 扩展配置空间 | 仅 256 字节基础配置 |
| vfio_wait_reqs 超时 | SeaBIOS 启动后有超时警告 |

---

## 9. 参考资料

- QEMU vfio-user 协议：`hw/vfio/user-protocol.h`
- QEMU vfio-user-pci 实现：`hw/vfio/pci.c`, `hw/vfio/user.c`
- Linux VFIO 头文件：`/usr/include/linux/vfio.h`
- NVMe 规范 1.4

---

*文档版本：v2.0*
*最后更新：2026-09-16*
