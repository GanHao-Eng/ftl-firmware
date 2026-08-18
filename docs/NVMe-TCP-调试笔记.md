# NVMe/TCP 目标端调试笔记

> 本文记录了从零实现 NVMe/TCP 目标端并与 Linux 内核 7.0 `nvme-tcp` 驱动对接过程中遇到的所有关键问题及解决方案。
>
> 调试环境：Ubuntu (Linux 7.0.0-28-generic), nvme-cli 2.8, libnvme 1.8
>
> 调试工具：`dmesg`、`tcpdump`、`nvme-cli`、固件日志、`hexdump`

---

## 目录

1. [CapsuleResp PDU 格式问题](#1-capsuleresp-pdu-格式问题)
2. [Identify Controller 字段偏移错误](#2-identify-controller-字段偏移错误)
3. [LBAF 布局错误](#3-lbaf-布局错误)
4. [Set Features 队列数过大](#4-set-features-队列数过大)
5. [Get Log Page 与 I/O Read opcode 冲突](#5-get-log-page-与-io-read-opcode-冲突)
6. [Connect QID 提取位置错误（最关键）](#6-connect-qid-提取位置错误最关键)
7. [sqid 硬编码导致 I/O 命令走 Admin 路径](#7-sqid-硬编码导致-io-命令走-admin-路径)
8. [60 秒空闲超时断连](#8-60-秒空闲超时断连)
9. [Write 命令 inline data 卡死](#9-write-命令-inline-data-卡死)
10. [FTL 读取未写入页返回错误](#10-ftl-读取未写入页返回错误)
11. [Write Zeroes 小端转换缺失](#11-write-zeroes-小端转换缺失)

---

## 1. CapsuleResp PDU 格式问题

### 现象

主机 `dmesg` 报错：
```
nvme nvme0: request 0x0 genctr mismatch (got 0x0 expected 0x1)
nvme nvme0: got bad cqe.command_id 0x0 on queue 0
nvme nvme0: receive failed: -22
```

后续尝试设置 phase bit 后：
```
nvme nvme0: Connect command failed, error wo/DNR bit: 0
nvme nvme0: failed to connect queue: 0 ret=16384
```

### 根因

CapsuleResp PDU 的结构和 phase bit 处理两次踩坑：

**第一次：自定义布局**
- 错误做法：自己设计了一个非标准的响应结构
- 结果：主机无法解析完成队列条目，报 genctr mismatch

**第二次：设置了 phase bit**
- 错误做法：在 `cpl.status` 中设置了 phase bit（`status = 0x8000`）
- 结果：主机将 phase bit 误判为状态码，Connect 命令返回错误 16384

### 解决方案

CapsuleResp PDU 必须使用标准结构：
```
公共头部(8B) + NVMe 完成队列条目(16B) = 24B
```

NVMe 完成队列条目字段顺序：
```c
typedef struct {
    uint32_t dw0;       // 命令特定返回值
    uint32_t rsvd1;     // 保留
    uint16_t sqhd;      // SQ 头指针
    uint16_t sqid;      // SQ 标识符
    uint16_t cid;       // 命令 ID
    uint16_t status;    // 状态字段
} nvme_completion_t;
```

**关键：NVMe/TCP 传输时禁止设置 phase bit**
```c
// 错误：status |= 0x8000;  // phase bit
// 正确：status &= 0x7FFF;   // 清除 phase bit
cpl.status = cpu_to_le16(0x0000);  // 成功状态
```

> **原理**：在 PCIe NVMe 中，phase bit 由控制器硬件在写入完成队列时翻转。
> 但在 NVMe/TCP 中，完成队列条目封装在 CapsuleResp PDU 中传输，
> phase bit 的语义由 TCP PDU 序号替代，因此不应在 status 字段中设置。

---

## 2. Identify Controller 字段偏移错误

### 现象

主机 `dmesg` 报错：
```
nvme nvme0: missing SUBNQN
nvme nvme0: Mismatching cntlid
```

`nvme list` 显示控制器信息异常。

### 根因

使用了旧版 NVMe 规范的字段偏移，与 Linux 内核 7.0 驱动使用的 `struct nvme_id_ctrl` 不匹配。

**错误的旧偏移：**
- CNTLID @ 94-95
- SUBNQN @ 256-511

**正确的偏移（NVMe 1.4 / Linux 内核 struct nvme_id_ctrl）：**

| 字段 | 偏移 | 值 | 说明 |
|------|------|-----|------|
| VID | 0-1 | 0x1234 | Vendor ID |
| SSVID | 2-3 | 0x1234 | Subsystem Vendor ID |
| SN | 4-23 | "FTLFW00000000000001" | Serial Number (20B) |
| MN | 24-63 | "FTL-Firmware NVMe Controller" | Model Number (40B) |
| FR | 64-71 | "1.0" | Firmware Revision (8B) |
| CNTLID | 78-79 | 0x0001 | Controller ID |
| KAS | 320-321 | 0x0001 | Keep Alive Support |
| SQES | 512 | 0x06 | SQ Entry Size (64B = 2^6) |
| CQES | 513 | 0x04 | CQ Entry Size (16B = 2^4) |
| NN | 516-519 | 1 | Number of Namespaces |
| SGLS | 536-539 | 0x03 | SGL Support |
| SUBNQN | 768-1023 | "nqn.2026-08.io.ftlfw:subsystem" | Subsystem NQN (256B) |
| IOCCSZ | 1792 | 4 | I/O Queue Command Capsule Size |
| IORCSZ | 1796 | 1 | I/O Queue Response Capsule Size |

### 解决方案

通过阅读 Linux 内核源码 `include/linux/nvme.h` 中的 `struct nvme_id_ctrl` 定义，逐一修正字段偏移。

**调试方法：**
1. 用 `tcpdump` 抓取 C2HData PDU
2. 用 `hexdump` 查看返回的 4096 字节数据
3. 对比 `struct nvme_id_ctrl` 的偏移，确认每个字段位置

> **关键教训**：NVMe 规范的字段偏移在不同版本间有变化，不能凭记忆或旧文档填写，
> 必须以实际对接的内核驱动头文件为准。

---

## 3. LBAF 布局错误

### 现象

`nvme list` 显示：
```
Format
512 B + 12 B
```

预期应为 `4 KiB + 0 B`。

### 根因

错误理解了 `struct nvme_lbaf` 的布局。

**错误布局（想当然）：**
```c
buf[128] = 0x0C;  // 误以为 byte0 = LBADS (LBA Data Size)
buf[129] = 0x00;  // 误以为 byte1 = MS (Metadata Size)
```

**正确布局（NVMe 规范）：**
```c
struct nvme_lbaf {
    __le16 ms;    // byte 0-1: Metadata Size (以字节为单位)
    __u8   ds;    // byte 2:   LBA Data Size (LBA = 2^ds 字节)
    __u8   rp;    // byte 3:   Relative Performance
};
```

因此 4KB LBA + 无元数据应为：
```c
buf[128] = 0x00;  // ms 低字节 = 0
buf[129] = 0x00;  // ms 高字节 = 0
buf[130] = 0x0C;  // ds = 12 → LBA = 2^12 = 4096 字节
buf[131] = 0x00;  // rp = 0
```

### 验证

修复后 `nvme id-ns` 输出：
```
nlbaf   : 0
lbaf  0 : ms:0   lbads:12 rp:0 (in use)
```

`nvme list` 显示：
```
Format
4 KiB +  0 B
```

---

## 4. Set Features 队列数过大

### 现象

```
Failed to write to /dev/nvme-fabrics: Cannot allocate memory
could not add new controller: failed to write to nvme-fabrics device
```

`dmesg`：
```
nvme nvme0: Could not set queue count (16384)
nvme nvme0: unable to set any I/O queues
```

### 根因

Set Features (Number of Queues, FID=0x07) 处理中，直接回显了主机请求的队列数。

主机请求 16384 个 I/O 队列，固件原样返回，导致主机尝试分配 16384 个队列，内存不足。

### 解决方案

限制 I/O 队列数为合理值（本实现限制为 2 个）：
```c
case 0x07: {  // Number of Queues
    uint16_t nsq = le16_to_cpu(cmd->cdw11) + 1;  // 主机请求的 SQ 数
    uint16_t ncq = le16_to_cpu(cmd->cdw12) + 1;  // 主机请求的 CQ 数
    uint16_t max_q = 2;  // 限制最大队列数
    nsq = nsq < max_q ? nsq : max_q;
    ncq = ncq < max_q ? ncq : max_q;
    cpl->dw0 = cpu_to_le32(((ncq - 1) << 16) | (nsq - 1));
    break;
}
```

> **注意**：返回值格式为 `(NCQA << 16) | NSQA`，其中 NCQA/NSQA 是 0-based 的。

---

## 5. Get Log Page 与 I/O Read opcode 冲突

### 现象

I/O Read 命令返回全零数据，`nvme read` 读取到的不是预期数据。

### 根因

Get Log Page（Admin 命令）和 I/O Read 的 opcode 都是 `0x02`。

最初的处理逻辑没有检查 `sqid`：
```c
// 错误：所有 opcode=0x02 都当 Get Log Page 处理
if (cmd->opcode == 0x02) {
    // Get Log Page 处理，返回4字节日志数据
}
```

导致 I/O Read（sqid=1）被误判为 Get Log Page，返回 4 字节全零。

### 解决方案

添加 `sqid == 0` 条件：
```c
// 正确：只有 Admin 队列(sqid=0)的 opcode=0x02 才是 Get Log Page
if (cmd->opcode == NVME_ADMIN_GET_LOG_PAGE && sqid == 0) {
    // Get Log Page 处理
}
// I/O 队列(sqid!=0)的 opcode=0x02 是 Read 命令
else if (cmd->opcode == NVME_IO_READ && sqid != 0) {
    // Read 处理
}
```

### 附加问题：NUMD 字段解析

Get Log Page 的 `cdw10` 中 NUMD（Number of Dwords）的位置：
- 正确：bits 31:16（`(cdw10 >> 16) & 0xFFFF`）
- 错误：bits 31:20（会导致 `no space in request` 连接失败）

数据长度 = `(NUMD + 1) * 4` 字节。

---

## 6. Connect QID 提取位置错误（最关键）

### 现象

I/O Read 返回全零或模拟数据，`nvme read` 无法获取真实数据。

固件日志显示所有命令的 `sqid=0`，都走了 Admin 处理路径。

### 根因

Connect 命令中 QID（Queue Identifier）的提取位置多次尝试错误：

**尝试1：从 cdw11 提取**
```c
qid = le32_to_cpu(cmd->cdw11);  // 结果：全 0
```

**尝试2：从命令 byte 24-25 提取**
```c
qid = le16_to_cpu(*(uint16_t*)((uint8_t*)cmd + 24));  // 结果：全 0
```
打印 3 个连接（Admin + 2个I/O队列）的原始字节，发现 byte 24-25 完全相同，都是 `00 00`。

**最终：从 inline data byte 16-17 提取**

通过打印 Connect 命令 inline data 的原始字节：
```
Admin 队列连接: inline[16-17] = FF FF  → 映射为 QID=0
I/O 队列连接:   inline[16-17] = 01 00  → QID=1
I/O 队列连接:   inline[16-17] = 01 00  → QID=1
```

### 解决方案

```c
// Connect 命令的 QID 在 inline data 的 byte 16-17
uint16_t qid_raw = le16_to_cpu(*(uint16_t*)(data + 16));
uint16_t qid = (qid_raw == 0xFFFF) ? 0 : qid_raw;
conn->qid = qid;
```

> **原理**：NVMe/TCP Connect 命令的 inline data 是 `nvme_fabrics_connect_data` 结构，
> 其中 `qid` 字段在偏移 16 处。Admin 队列使用 `qid=0xFFFF` 作为特殊标记。

> **调试关键**：当协议字段位置不确定时，打印原始字节十六进制是最可靠的方法，
> 不要假设字段位置，用抓包/日志实证。

---

## 7. sqid 硬编码导致 I/O 命令走 Admin 路径

### 现象

即使 QID 正确提取后，I/O 命令仍然走 Admin 处理路径。

### 根因

`nvme_tcp_handle_capsule_cmd` 中 `sqid` 被硬编码为 0：
```c
// 错误
uint16_t sqid = 0;
```

### 解决方案

从连接上下文获取：
```c
// 正确
uint16_t sqid = conn->qid;
```

这样 Admin 队列连接（qid=0）的命令走 Admin 路径，I/O 队列连接（qid=1+）的命令走 I/O 路径。

---

## 8. 60 秒空闲超时断连

### 现象

`dmesg` 显示控制器每 60 秒触发错误恢复并重连：
```
nvme nvme0: starting error recovery in 2 seconds
nvme nvme0: Reconnecting in 10 seconds...
nvme nvme0: Successfully reconnected (attempt 1/60)
```

### 根因

`nvme_tcp_target.c` 中有一个 60 秒无活动关闭连接的检查逻辑：
```c
// 错误：空闲超时关闭连接
if (now - conn->last_activity_ms > 60000) {
    close_connection(conn);
}
```

NVMe/TCP 是长连接协议，主机在空闲时不会发送数据，但保持连接打开。
固件主动关闭连接导致主机认为控制器故障，触发重连。

### 解决方案

移除空闲超时关闭逻辑。NVMe/TCP 连接应保持打开，直到主机主动断开或发送 H2CTerm。

> **补充**：Keep Alive 命令（opcode=0x18）用于保活，KAS 寄存器设置为 1 秒，
> 主机会定期发送 Keep Alive，固件正常响应即可。

---

## 9. Write 命令 inline data 卡死

### 现象

`dd of=/dev/nvme0n1` 写入命令卡死，无响应。

### 根因

Write 命令的数据可以通过两种方式传输：
1. **Inline data**：数据随 CapsuleCmd PDU 一起发送
2. **R2T + H2CData**：固件发送 R2T 请求，主机用 H2CData 发送数据

最初的逻辑：
- 如果有 inline data 但不完整 → 发送 R2T 请求剩余数据 ✅
- 如果 inline data 已包含全部数据 → **既不发 R2T 也不发 CapsuleResp** ❌

导致主机等待 CapsuleResp，固件等待 H2CData，死锁。

### 解决方案

```c
if (conn->data_len < conn->data_total) {
    // 数据不完整，发送 R2T 请求剩余数据
    nvme_tcp_send_r2t(conn, cid, 0, conn->data_len,
                      conn->data_total - conn->data_len);
} else {
    // inline data 已包含全部数据，直接处理并发送 CapsuleResp
    // ... 写入 FTL ...
    nvme_tcp_send_capsule_resp(conn, &cpl);
}
```

---

## 10. FTL 读取未写入页返回错误

### 现象

`dd if=/dev/nvme0n1` 读取未写入过的 LBA 时报错：
```
dd: 读取 '/dev/nvme0n1' 时出错: 输入/输出错误
```

固件日志：
```
FTL 读取失败, LPN=xxx, ret=-5
```

### 根因

FTL 层 `ftl_read()` 对未写入的逻辑页返回错误（`RET_ERR_NOT_FOUND`）。

但 NVMe 规范要求：读取未写入的 LBA 应成功，返回全零（或未定义数据），不应返回错误。

此外，Linux 内核有预读机制，`dd count=16` 可能实际读取超过 16 个 LBA，触发未写入页读取。

### 解决方案

在 NVMe/TCP Read 处理中，FTL 读取失败时填充全零：
```c
for (i = 0; i < nlb; i++) {
    ret_code_t ret = ftl_read(slba + i, read_data + i * 4096);
    if (ret != RET_OK) {
        // 未写入的页返回全零（NVMe 规范要求）
        memset(read_data + i * 4096, 0, 4096);
    }
}
```

---

## 11. Write Zeroes 小端转换缺失

### 现象

`nvme write-zeroes` 命令返回成功，但读回数据未被清零。

### 根因

Write Zeroes 命令走 `nvme_ctrl_process_io_cmd()` 路径，该函数为 PCIe 模式设计，
命令字段已是主机字节序。但在 NVMe/TCP 模式下，命令从网络接收，是小端格式，
需要转换。

`nvme_ctrl_process_io_cmd()` 中：
```c
uint64_t slba = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;  // 无小端转换
uint16_t nlb = (cmd->cdw12 & 0xFFFF) + 1;                    // 无小端转换
```

导致 SLBA 和 NLB 解析错误，写入了错误的 LBA 位置。

### 解决方案

在 `nvme_tcp_target.c` 中对 Write Zeroes 做特殊处理（类似 Read 命令），
使用正确的小端转换：
```c
if (cmd->opcode == NVME_IO_WRITE_ZEROES && sqid != 0) {
    uint16_t nlb = (le16_to_cpu(cmd->cdw12) & 0xFFFF) + 1;
    uint64_t slba = ((uint64_t)le32_to_cpu(cmd->cdw11) << 32) |
                    le32_to_cpu(cmd->cdw10);
    // ... 写零到 FTL ...
}
```

---

## 调试方法论总结

1. **先看 dmesg**：内核报错通常能定位问题大类（内存不足/协议错误/超时）
2. **抓包验证**：`tcpdump -i lo -w /tmp/nvme.pcap port 4420`，用 Wireshark 分析 PDU
3. **打印原始字节**：协议字段位置不确定时，打印 hex dump 实证，不要假设
4. **对比内核源码**：字段偏移以 `include/linux/nvme.h` 中的结构体定义为准
5. **分阶段验证**：IC 握手 → Connect → Admin → I/O 队列 → Read → Write，逐步打通
6. **最小化复现**：用 `nvme-cli` 单条命令测试，不要一开始就用 fio/dd

---

## 最终验证结果

| 测试项 | 结果 |
|--------|------|
| 连接建立 | ✅ `nvme connect` 成功，2 个 I/O 队列 |
| Identify Controller | ✅ `nvme id-ctrl` 字段正确 |
| Identify Namespace | ✅ `nvme id-ns` LBAF=4K+0B |
| 单 LBA 读写 | ✅ md5sum 一致 |
| 多 LBA 读写 (64KB) | ✅ md5sum 一致 |
| Write Zeroes | ✅ 指定范围清零 |
| TRIM (DSM) | ✅ `nvme dsm` success |
| fio 顺序读 | ✅ 6.7 MB/s |
| fio 顺序写 | ✅ 5.8 MB/s |
