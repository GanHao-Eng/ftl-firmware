# vfio-user PCIe 对接调试问题与解决方案

本文档记录 ftl-firmware 项目 vfio-user NVMe PCIe 后端从启动到完整读写打通过程中遇到的所有关键问题、根因分析和解决方案。

## 目录

1. [问题概览](#1-问题概览)
2. [协议层问题](#2-协议层问题)
3. [PCIe 配置空间问题](#3-pcie-配置空间问题)
4. [DMA 内存共享问题](#4-dma-内存共享问题)
5. [NVMe 队列与命令处理问题](#5-nvme-队列与命令处理问题)
6. [PRP 数据传输问题](#6-prp-数据传输问题)
7. [完成条目与中断问题](#7-完成条目与中断问题)
8. [调试方法论](#8-调试方法论)

---

## 1. 问题概览

vfio-user PCIe 对接是整个项目中调试周期最长、问题最密集的部分。从最初的"QEMU 无法连接"到最终的"/dev/nvme0n1 读写验证通过"，共经历了以下几个阶段：

| 阶段 | 现象 | 关键问题数 |
|------|------|-----------|
| 协议握手 | QEMU 连接后立即断开或无响应 | 3 |
| PCIe 枚举 | Guest 无法识别设备或识别后 hang | 4 |
| Admin 命令 | Identify Controller 超时或返回错误 | 5 |
| I/O 命令 | Read/Write 数据错误或系统 hang | 6 |

---

## 2. 协议层问题

### 2.1 消息头部大小不对齐

**现象：** QEMU 连接后立即发送 VFU_VERSION 请求，固件回复后 QEMU 断开连接。

**根因：** 固件实现的消息头部大小为 20 字节，而 QEMU 标准 user-protocol.h 定义为 16 字节。QEMU 按 16 字节读取头部，读到错误的 size 字段，导致后续 payload 解析错位。

**解决方案：** 严格对齐 QEMU 标准消息头部：

```c
typedef struct {
    uint16_t id;           // 请求/回复 ID
    uint16_t command;      // 消息类型
    uint32_t size;         // 整个消息大小（含头部）
    uint32_t flags;        // 消息标志
    uint32_t error_reply;  // 错误码
} vfu_msg_hdr_t;           // 总计 16 字节
```

**教训：** vfio-user 是 QEMU 内部协议，必须严格对齐 QEMU 源码中的定义，不能凭记忆或猜测实现。

### 2.2 粘包问题

**现象：** 固件读取 vfio-user 消息时，头部和 payload 数据错乱，导致命令解析失败。

**根因：** Unix domain socket 是流式协议，一次 read() 调用可能返回多条消息（粘包）或一条消息的部分数据（拆包）。固件最初只做一次 read()，没有循环读取直到收满整个消息。

**解决方案：** 分两次读取：
1. 先读 16 字节头部（同时通过 SCM_RIGHTS 接收 FD）
2. 解析头部中的 size 字段，循环读取 payload 直到收满

```c
// 第一步：读头部 + SCM_RIGHTS
recvmsg(fd, &msg_hdr_buf, sizeof(vfu_msg_hdr_t), &fds);

// 第二步：循环读 payload
uint32_t payload_size = hdr.size - sizeof(vfu_msg_hdr_t);
uint32_t received = 0;
while (received < payload_size) {
    ssize_t n = recv(fd, payload + received, payload_size - received, 0);
    received += n;
}
```

### 2.3 NO_REPLY 消息处理错误

**现象：** QEMU 发送 doorbell 写入（flags=0x10 NO_REPLY）后，固件尝试回复，导致 QEMU 等待回复而 hang。

**根因：** flags=0x10 的消息是 posted write，QEMU 不期望回复。固件无条件回复所有消息。

**解决方案：** 收到 NO_REPLY 消息后只处理不回复：

```c
if (hdr.flags & VFU_MSG_FLAG_NO_REPLY) {
    // 处理消息，不发送回复
    return;
}
```

---

## 3. PCIe 配置空间问题

### 3.1 BAR0 大小不对

**现象：** QEMU 启动时报错 `vfio-user-pci: bar 0 size is not power of 2` 或 Guest 无法映射 BAR0。

**根因：** BAR0 大小设置为非 2 的幂（如 4KB + 4KB + 4KB + 4KB = 16KB，但实际实现中大小计算错误）。

**解决方案：** BAR0 大小必须是 2 的幂。最终使用 64KB（0x10000），布局：
- 0x0000-0x0FFF：NVMe 控制器寄存器（4KB）
- 0x1000-0x1FFF：Doorbell 寄存器（4KB）
- 0x2000-0x2FFF：MSI-X 向量表（4KB）
- 0x3000-0x3FFF：MSI-X PBA（4KB）

### 3.2 CAP 寄存器值错误

**现象：** Guest NVMe 驱动初始化时立即失败，dmesg 显示 `nvme: probe failed`。

**根因：** CAP (Controller Capabilities) 寄存器值不正确。最初设置的值包含了 PMRS/CMBS/BPS 等不支持的位，导致驱动尝试访问不支持的寄存器区域。

**解决方案：** 使用保守的 CAP 值，只声明基本功能：

```c
#define NVME_CAP  0x0000000002F003FFULL
// MQES=0xFFF (队列深度4096)
// DSTRD=0 (doorbell stride=0)
// CSS=000 (NVM command set)
// MPSMIN=0 (最小内存页4KB)
// MPSMAX=0 (最大内存页4KB)
// 不支持 PMRS/CMBS/BPS
```

### 3.3 MSI-X 向量表位置

**现象：** Guest 触发中断时 hang 或丢失中断。

**根因：** MSI-X 向量表和 PBA 的 BAR0 偏移不正确，或者向量表大小与实际配置不匹配。

**解决方案：**
- MSI-X Capability 位于配置空间偏移 0x40
- 向量表位于 BAR0 偏移 0x2000
- PBA 位于 BAR0 偏移 0x3000
- 向量表大小 = 1（1 个中断向量）
- 每个向量 16 字节

---

## 4. DMA 内存共享问题

### 4.1 不使用共享内存导致 DMA_MAP 无 FD

**现象：** 固件收到 VFU_DMA_MAP 消息后，无法映射 Guest 内存，读写 PRP 地址时返回 NULL。

**根因：** QEMU 默认使用匿名内存后端（匿名 mmap），不通过 file descriptor 传递内存。固件无法通过 SCM_RIGHTS 接收内存 FD，导致 GPA→HVA 映射失败。

**解决方案：** QEMU 启动时必须使用共享内存后端：

```bash
-object memory-backend-file,id=mem,size=2G,mem-path=/dev/shm,share=on
-numa node,memdev=mem
```

这样 QEMU 会将 Guest 内存通过 `/dev/shm` 下的文件 backing，并在 VFU_DMA_MAP 消息中传递文件描述符，固件 mmap 后即可直接访问 Guest 物理内存。

### 4.2 GPA→HVA 映射查找

**现象：** 读写 PRP 地址时 `gpa_to_hva()` 返回 NULL。

**根因：** VFU_DMA_MAP 消息中可能包含多个内存区域，固件需要按区域逐一匹配 GPA 范围。

**解决方案：** 维护一个内存区域数组，每次查找时遍历所有区域：

```c
typedef struct {
    uint64_t iova;      // Guest 物理地址基址
    uint64_t size;      // 区域大小
    void    *host_addr; // mmap 后的主机虚拟地址
} dma_region_t;

void *gpa_to_hva(uint64_t gpa) {
    for (i = 0; i < dma_region_count; i++) {
        if (gpa >= regions[i].iova && 
            gpa < regions[i].iova + regions[i].size) {
            return regions[i].host_addr + (gpa - regions[i].iova);
        }
    }
    return NULL;
}
```

---

## 5. NVMe 队列与命令处理问题

### 5.1 CC.EN 复位逻辑错误

**现象：** Guest 驱动写 CC.EN=0 复位控制器后，再次写 CC.EN=1 启动时 hang。

**根因：** CC.EN=0 时固件清空了队列配置（ASQ/ACQ 基地址、队列大小等），导致驱动重新初始化时找不到队列。

**解决方案：** CC.EN=0 时只重置指针（sq_head/cq_tail/cq_phase），不清除队列配置：

```c
case NVME_CC_EN_RESET:
    // 只重置运行时状态，不清除队列配置
    g_queues[0].sq_head = 0;
    g_queues[0].cq_tail = 0;
    g_queues[0].cq_phase = 1;
    // 保留 ASQ/ACQ 基地址和队列大小
    break;
```

### 5.2 SGLS 字段配置错误

**现象：** Guest 驱动使用 SGL（Scatter-Gather List）而非 PRP 传输数据，导致固件读取 PRP 指针失败。

**根因：** Identify Controller 数据中 SGLS (SGL Support) 字段设为 0x03，表示支持 SGL。驱动检测到 SGL 支持后使用 SGL 格式传输。

**解决方案：** 将 SGLS 设为 0（不支持 SGL），强制驱动使用 PRP：

```c
// identify_ctrl 数据中
id_ctrl->sgls = 0;  // 不支持 SGL，使用 PRP
```

### 5.3 Identify Namespace 数据填充不完整

**现象：** 驱动创建命名空间后，Read 命令返回 `LBA Out of Range` 错误。

**根因：** Identify Namespace (CNS=0x00) 数据中 nsze (Namespace Size) 和 ncap (Namespace Capacity) 未正确填充，驱动认为命名空间大小为 0。

**解决方案：** 正确填充命名空间参数：
- nsze = 262143 (0x3FFFFF)，即 262144 个 LBA
- ncap = 262143
- nlbaf = 0（支持 1 种 LBAF）
- flbas = 0（使用 LBAF0）
- LBAF0：ds=12 (4096 bytes)，ms=0 (无元数据)

---

## 6. PRP 数据传输问题

### 6.1 PRP 列表遍历逻辑错误

**现象：** 多页 Read 命令完成后，Guest 内核 hang 或 panic。

**根因：** PRP 列表遍历逻辑错误。最初将所有页对齐的 PRP2 都当作 PRP 列表指针，但实际上：
- 正好 2 页时，PRP2 直接是第 2 页的地址
- 超过 2 页时，PRP2 才是 PRP 列表指针

**解决方案：** 正确判断 PRP2 是否为列表指针：

```c
// PRP2 是列表指针当且仅当传输超过2页
bool prp2_is_list = (len > first_page_chunk + page_size);
```

### 6.2 PRP2 非页对齐问题（关键修复）

**现象：** 大量多页 Read 命令完成后，Guest 系统 hang，无任何输出。

**根因：** QEMU 发送的部分 PRP2 地址不是页对齐的（如 0x251A100）。最初的代码要求 PRP2 必须页对齐才当作列表指针，非页对齐时只处理 2 页就 break。这导致多页传输只写入了前 2 页数据，剩余数据未写入 Guest 内存，内核读取到不完整的文件系统元数据后 hang。

**解决方案：** 只要传输超过 2 页，无论 PRP2 是否页对齐，都当作 PRP 列表指针处理：

```c
// 只要传输超过2页，PRP2就是列表指针
bool prp2_is_list = (len > first_page_chunk + page_size);
```

**为什么 PRP2 会非页对齐？** QEMU 的 vfio-user 实现在构建 PRP 列表时，列表本身可能分配在非页对齐的内存位置（如 4KB 页内偏移 0x100）。虽然 NVMe 规范要求 PRP 列表页对齐，但 QEMU 的实现并不严格遵守。后端需要容错处理。

### 6.3 未初始化的 PRP2 值

**现象：** 偶发 `PRP write GPA 转换失败, addr=0xCCCCCCCCCCCCCCCC` 错误。

**根因：** 少数多页 Read 命令的 PRP2 字段包含未初始化的栈内存值 0xCCCCCCCCCCCCCCCC。这是 QEMU 端的偶发时序问题。

**解决方案：** 提前检查 PRP2 有效性，返回 DATA_XFER_ERROR 让驱动降级为单页 Read 重试：

```c
if (transfer_len > PAGE_SIZE && gpa_to_hva(prp2) == NULL) {
    LOG_WARN("IO READ prp2 无效, 降级返回错误让驱动重试");
    cpl->status = NVME_SC_DATA_XFER_ERROR;
    break;
}
```

---

## 7. 完成条目与中断问题

### 7.1 完成条目 Phase Tag 位置（最耗时的调试）

**现象：**
- Phase Tag 放在 Status Field bit0 → I/O 队列创建成功，大量 Read 完成，但系统 hang
- Phase Tag 放在 DW2 bit16 → 系统能启动，但 Admin 命令超时

**根因：** Linux 内核 NVMe 驱动实际读取 Phase Tag 的位置与 NVMe 规范文档描述不完全一致。经过反复实验，确认内核从 Status Field (bytes 14-15) 的 bit0 读取 Phase Tag，而非 DW2 bit16。

**解决方案：** 完成条目布局：

```
偏移  大小  字段          说明
0-3   4    DW0          命令特定返回值
4-7   4    Reserved     保留
8-9   2    SQHD         SQ 头指针
10-11 2    Reserved     保留（全0）
12-13 2    CID          命令 ID
14-15 2    Status       bit0=Phase Tag, bits15:1=Status Code
```

代码实现：
```c
// Phase Tag 设置在 status 字段 bit0
if (q->cq_phase) {
    cpl.status |= cpu_to_le16(0x0001);
} else {
    cpl.status &= cpu_to_le16(~0x0001);
}
```

### 7.2 完成条目结构体中错误的 SQ ID 字段

**现象：** 完成条目写入后，驱动无法正确识别完成。

**根因：** 最初结构体中包含 `sqid` 字段（bytes 10-11），但 NVMe 完成条目中根本没有 SQ ID 字段。这个位置应该是 Reserved。设置 sqid 导致该区域非零，驱动读取时可能产生异常。

**解决方案：** 删除 sqid 赋值，bytes 10-11 保持全 0。

### 7.3 内存屏障

**现象：** 完成条目写入后，驱动有时看不到数据（读到旧值）。

**根因：** CPU 写缓冲区可能未刷新到内存，驱动读完成条目时拿到的是旧数据。

**解决方案：** 写入完成条目后加内存屏障：

```c
memcpy(cq_entry, &cpl, sizeof(nvme_completion_t));
__sync_synchronize();  // 确保写入对主机可见
```

### 7.4 MSI-X 中断触发

**现象：** 完成条目写入成功，但驱动没有收到中断，命令超时。

**根因：** 完成条目写入后没有触发 MSI-X 中断。

**解决方案：** 通过 eventfd 触发 MSI-X 中断：

```c
// 写入完成条目后触发中断
if (wrote_completion) {
    uint64_t val = 1;
    write(g_ctx.msix_evtfd, &val, sizeof(val));
}
```

---

## 8. 调试方法论

### 8.1 分层调试策略

vfio-user PCIe 对接涉及多层协议，建议从下到上逐层验证：

1. **协议握手层**：验证 VERSION、DEVICE_GET_INFO、DMA_MAP 等 vfio-user 消息是否正确交换
2. **PCIe 配置空间层**：验证 BAR0 读写、MSI-X 配置是否正确
3. **队列管理层**：验证 Admin 队列创建、I/O 队列创建是否成功
4. **命令处理层**：验证 Identify、Set Features 等 Admin 命令是否正确完成
5. **数据传输层**：验证 Read/Write 的 PRP 数据是否正确写入 Guest 内存

### 8.2 关键调试命令

```bash
# 固件日志：查看完成条目写入
grep "CPL write" /tmp/ftl.log

# 固件日志：查看 doorbell 写入
grep "doorbell" /tmp/ftl.log

# 固件日志：查看 IO 命令
grep "IO READ\|IO WRITE" /tmp/ftl.log

# 固件日志：查看错误
grep "ERROR" /tmp/ftl.log

# Guest 内核日志
dmesg | grep -i nvme

# 验证设备节点
ls -la /dev/nvme*
```

### 8.3 常见症状对照表

| 症状 | 可能原因 | 排查方向 |
|------|---------|---------|
| QEMU 连接后立即断开 | 消息头部大小不对齐 | 检查 vfu_msg_hdr_t 大小是否为 16 字节 |
| Guest 无法识别设备 | PCIe 配置空间错误 | 检查 Vendor ID、BAR0、Cap Pointer |
| Identify 超时 | 完成条目 Phase Tag 位置错误 | 检查 cpl.status 的 bit0 |
| I/O 队列创建后 hang | PRP 列表遍历不完整 | 检查多页 Read 的数据是否全部写入 |
| 数据验证失败 | DMA 映射错误 | 检查 gpa_to_hva() 返回值 |
| 偶发超时 | PRP2 未初始化 | 检查 prp2=0xCCCC... 的防护 |

### 8.4 验证清单

完成对接后，按以下清单逐项验证：

- [ ] QEMU 正常启动，Guest 内核识别 PCI 设备
- [ ] `lspci` 显示 NVMe 控制器
- [ ] `dmesg | grep nvme` 无错误
- [ ] `/dev/nvme0` 和 `/dev/nvme0n1` 出现
- [ ] `nvme id-ctrl /dev/nvme0` 正常返回
- [ ] Read 测试：`dd if=/dev/nvme0n1 of=/dev/null bs=4k count=100`
- [ ] Write 测试：`dd if=/dev/urandom of=/dev/nvme0n1 bs=4k count=100`
- [ ] 数据一致性：`cmp /tmp/test.bin /tmp/verify.bin`

---

## 总结

vfio-user PCIe 对接的核心挑战在于：

1. **协议栈深**：涉及 vfio-user 协议 → PCIe 配置空间 → NVMe 寄存器 → NVMe 命令 → PRP 数据传输 五层协议，任何一层出错都会导致上层失败
2. **规范偏差**：QEMU 的实现与 NVMe/vfio-user 规范文档存在细微偏差（如 PRP2 非页对齐、Phase Tag 位置），需要通过实验而非文档来确认
3. **错误现象不直观**：系统 hang、命令超时等症状往往不是直接原因，需要分层排查

关键经验：
- 严格对齐 QEMU 源码定义，不要凭记忆实现协议
- 每增加一个功能就做一次端到端验证，不要积累太多未验证的代码
- 多打日志，用日志数据指导调试方向
- PRP 遍历和完成条目布局是最容易出错的地方
