/**
 * @file vfio_user_nvme.c
 * @brief vfio-user NVMe 后端完整实现
 * @details 实现 QEMU vfio-user 协议后端，使 ftl-firmware 可模拟一个
 *          完整的 PCIe NVMe 设备。QEMU 通过
 *          -device vfio-user-pci,socket=... 连接本后端。
 *
 *          实现内容：
 *          1. Unix domain socket 服务器（bind/listen/accept）
 *          2. vfio-user 消息接收/发送（支持 SCM_RIGHTS 传递 FD）
 *          3. 完整协议握手：GET_API_VERSION → DEVICE_GET_INFO →
 *             DEVICE_GET_REGION_INFO → DEVICE_GET_IRQ_INFO →
 *             DMA_MAP → DEVICE_SET_IRQS
 *          4. PCIe Type 0 配置空间模拟（256字节，含 MSI-X Capability）
 *          5. BAR0 模拟（64KB）：NVMe 寄存器(4KB) + 门铃(4KB) +
 *             MSI-X 向量表(4KB) + MSI-X PBA(4KB)
 *          6. NVMe 寄存器处理：CC.EN → CSTS.RDY，ASQ/ACQ/AQA
 *          7. 门铃触发命令处理：从 guest 内存 SQ 读命令，
 *             调用 nvme_ctrl_process_admin_cmd/io_cmd，PRP 数据传输，
 *             完成写入 CQ，MSI-X eventfd 发中断
 *          8. DMA 映射通过 VFU_DMA_MAP + SCM_RIGHTS 传递 FD
 *
 * @note 所有多字节字段按小端处理（vfio-user 和 NVMe 均为小端，
 *       x86 平台直接使用无需转换）。
 */

#define _GNU_SOURCE

#include "protocol/vfio_user_nvme.h"
#include "protocol/nvme_controller.h"
#include "ftl.h"
#include "log.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/select.h>

/* ============================================================
 *  内部常量
 * ============================================================ */

/** @brief 队列总数（Admin + 最大 I/O 队列） */
#define VFIO_MAX_QUEUES  (1U + NVME_MAX_IO_QUEUES)

/** @brief 页大小 */
#define VFIO_PAGE_SIZE   4096U

/* ============================================================
 *  内部数据结构
 * ============================================================ */

/**
 * @brief 单队列运行时状态
 * @details 跟踪 SQ/CQ 的基地址(GPA)、大小、头尾指针和相位位。
 *          Admin 队列(qid=0)从 ASQ/ACQ/AQA 寄存器获取信息；
 *          I/O 队列从 Create I/O SQ/CQ Admin 命令获取信息。
 */
typedef struct {
    uint64_t sq_base;    ///< SQ 基地址（GPA）
    uint64_t cq_base;    ///< CQ 基地址（GPA）
    uint16_t sq_size;    ///< SQ 大小（条目数，0=未创建）
    uint16_t cq_size;    ///< CQ 大小（条目数，0=未创建）
    uint16_t sq_head;    ///< SQ 头指针（控制器侧，下一个要取的命令）
    uint16_t sq_tail;    ///< SQ 尾指针（主机侧，门铃写入值）
    uint16_t cq_head;    ///< CQ 头指针（主机侧，门铃写入值）
    uint16_t cq_tail;    ///< CQ 尾指针（控制器侧，下一个要写的完成）
    bool     cq_phase;   ///< CQ 相位位（初始为1）
    bool     valid;      ///< 队列是否已创建/有效
} vfio_queue_state_t;

/* ============================================================
 *  内部数据
 * ============================================================ */

/** @brief 全局后端上下文 */
static vfio_user_nvme_ctx_t g_ctx;
static uint16_t g_current_req_id = 0;
static uint16_t g_current_command = 0;

/** @brief 队列状态数组（index 0 = Admin, 1+ = I/O） */
static vfio_queue_state_t g_queues[VFIO_MAX_QUEUES];

/** @brief 预分配 I/O 读缓冲区 */
static uint8_t g_io_read_buf[VFIO_USER_NVME_MAX_IO_SIZE];

/** @brief 预分配 I/O 写缓冲区 */
static uint8_t g_io_write_buf[VFIO_USER_NVME_MAX_IO_SIZE];

/** @brief 静态零缓冲区（用于 Write Zeroes） */
static uint8_t g_zero_buf[VFIO_PAGE_SIZE];

/* ============================================================
 *  小端字节序辅助（x86 为小端，直接返回）
 * ============================================================ */

static uint16_t vfu_le16_to_cpu(uint16_t val) { return val; }
static uint16_t vfu_cpu_to_le16(uint16_t val) { return val; }
static uint32_t vfu_le32_to_cpu(uint32_t val) { return val; }
static uint32_t vfu_cpu_to_le32(uint32_t val) { return val; }
static uint64_t vfu_le64_to_cpu(uint64_t val) { return val; }
static uint64_t vfu_cpu_to_le64(uint64_t val) { return val; }

/* ============================================================
 *  GPA → HVA 地址转换
 * ============================================================ */

/**
 * @brief 将 Guest 物理地址(GPA)转换为后端进程虚拟地址(HVA)
 * @details 遍历 VFU_DMA_MAP 建立的 DMA 区域映射，找到包含目标 GPA
 *          的区域，计算在 mmap 区域内的偏移并返回虚拟地址。
 * @param gpa Guest 物理地址
 * @return 虚拟地址指针，未找到返回 NULL
 */
static void *gpa_to_hva(uint64_t gpa)
{
    uint32_t i = 0;

    for (i = 0; i < g_ctx.dma_region_count; i++) {
        uint64_t base = g_ctx.dma_regions[i].iova;
        uint64_t size = g_ctx.dma_regions[i].size;

        if (gpa >= base && gpa < base + size) {
            uint64_t offset = gpa - base;
            return (uint8_t *)g_ctx.dma_mappings[i] + offset;
        }
    }
    return NULL;
}

/* ============================================================
 *  PRP 解析与数据传输
 * ============================================================ */

/**
 * @brief 从 PRP 列表读取数据到本地缓冲区
 * @details 解析 PRP1/PRP2，遍历所有物理页，将共享内存中的数据
 *          拷贝到本地缓冲区。支持跨页传输。
 * @param prp1  PRP Entry 1
 * @param prp2  PRP Entry 2
 * @param len   数据总长度（字节）
 * @param buf   输出缓冲区
 * @return RET_OK 成功，RET_ERR_PARAM 地址转换失败
 */
static ret_code_t prp_read_data(uint64_t prp1, uint64_t prp2,
                                uint32_t len, uint8_t *buf)
{
    uint32_t offset = 0;
    uint64_t page_size = VFIO_PAGE_SIZE;
    uint64_t current_addr = prp1;
    uint32_t first_page_chunk = (uint32_t)(page_size - (prp1 & (page_size - 1)));

    while (offset < len) {
        uint64_t page_offset = current_addr & (page_size - 1);
        uint32_t chunk = (uint32_t)(page_size - page_offset);
        void *hva = NULL;

        if (chunk > len - offset) {
            chunk = len - offset;
        }

        hva = gpa_to_hva(current_addr & ~(page_size - 1));
        if (hva == NULL) {
            LOG_ERROR("vfio-user: PRP read GPA 转换失败, addr=0x%llX",
                      (unsigned long long)current_addr);
            return RET_ERR_PARAM;
        }

        memcpy(buf + offset, (uint8_t *)hva + page_offset, chunk);
        offset += chunk;

        if (offset >= len) {
            break;
        }

        /* 判断下一页来源 */
        if ((prp2 & (page_size - 1)) == 0 && offset > first_page_chunk) {
            /* PRP2 是页对齐的 PRP 条目列表指针 */
            uint64_t *prp_list = (uint64_t *)gpa_to_hva(prp2);
            uint32_t list_idx = 0;

            if (prp_list == NULL) {
                LOG_ERROR("vfio-user: PRP 列表地址转换失败, prp2=0x%llX",
                          (unsigned long long)prp2);
                return RET_ERR_PARAM;
            }

            /* 计算列表索引：已传输的完整页数 - 1（第一页由 PRP1 覆盖） */
            list_idx = (offset - first_page_chunk) / (uint32_t)page_size;
            current_addr = vfu_le64_to_cpu(prp_list[list_idx]);
        } else if (offset == first_page_chunk) {
            /* 第一页结束，PRP2 直接是第二页地址（仅2页场景） */
            current_addr = prp2;
        } else {
            /* PRP2 带页内偏移（仅2页场景），第二页后无更多数据 */
            break;
        }
    }

    return RET_OK;
}

/**
 * @brief 将本地缓冲区数据写入 PRP 列表指向的共享内存
 * @param prp1  PRP Entry 1
 * @param prp2  PRP Entry 2
 * @param len   数据总长度（字节）
 * @param buf   输入缓冲区
 * @return RET_OK 成功，RET_ERR_PARAM 地址转换失败
 */
static ret_code_t prp_write_data(uint64_t prp1, uint64_t prp2,
                                 uint32_t len, const uint8_t *buf)
{
    uint32_t offset = 0;
    uint64_t page_size = VFIO_PAGE_SIZE;
    uint64_t current_addr = prp1;
    uint32_t first_page_chunk = (uint32_t)(page_size - (prp1 & (page_size - 1)));

    while (offset < len) {
        uint64_t page_offset = current_addr & (page_size - 1);
        uint32_t chunk = (uint32_t)(page_size - page_offset);
        void *hva = NULL;

        if (chunk > len - offset) {
            chunk = len - offset;
        }

        hva = gpa_to_hva(current_addr & ~(page_size - 1));
        if (hva == NULL) {
            LOG_ERROR("vfio-user: PRP write GPA 转换失败, addr=0x%llX",
                      (unsigned long long)current_addr);
            return RET_ERR_PARAM;
        }

        memcpy((uint8_t *)hva + page_offset, buf + offset, chunk);
        offset += chunk;

        if (offset >= len) {
            break;
        }

        if ((prp2 & (page_size - 1)) == 0 && offset > first_page_chunk) {
            uint64_t *prp_list = (uint64_t *)gpa_to_hva(prp2);
            uint32_t list_idx = 0;

            if (prp_list == NULL) {
                LOG_ERROR("vfio-user: PRP 列表地址转换失败, prp2=0x%llX",
                          (unsigned long long)prp2);
                return RET_ERR_PARAM;
            }

            list_idx = (offset - first_page_chunk) / (uint32_t)page_size;
            current_addr = vfu_le64_to_cpu(prp_list[list_idx]);
        } else if (offset == first_page_chunk) {
            current_addr = prp2;
        } else {
            break;
        }
    }

    return RET_OK;
}

/* ============================================================
 *  vfio-user 消息收发（支持 SCM_RIGHTS）
 * ============================================================ */

/**
 * @brief 接收 vfio-user 消息（含可能的文件描述符）
 * @details 使用 recvmsg 接收消息头部和 payload，通过 SCM_RIGHTS
 *          辅助数据接收文件描述符（如 DMA_MAP 的内存 FD、
 *          SET_IRQS 的 eventfd）。
 * @param fd      连接 socket
 * @param hdr     输出：消息头部
 * @param payload 输出：payload 缓冲区
 * @param fds     输出：接收到的文件描述符数组
 * @param max_fds fds 数组容量
 * @return 接收的 payload 字节数，-1 失败
 */
static ssize_t vfu_recv_msg(int fd, vfu_msg_hdr_t *hdr,
                            void *payload, int *fds, int max_fds)
{
    struct msghdr msg;
    struct iovec iov[2];
    char cmsgbuf[CMSG_SPACE(sizeof(int) * VFIO_USER_MAX_DMA_REGIONS)];
    struct cmsghdr *cmsg = NULL;
    ssize_t n = 0;
    int *fd_ptr = NULL;
    int fd_count = 0;
    int i = 0;

    memset(&msg, 0, sizeof(msg));
    memset(cmsgbuf, 0, sizeof(cmsgbuf));

    /* 先收头部（16字节） */
    iov[0].iov_base = hdr;
    iov[0].iov_len = sizeof(vfu_msg_hdr_t);
    /* 再收 payload */
    iov[1].iov_base = payload;
    iov[1].iov_len = VFIO_USER_MAX_PAYLOAD_SIZE;

    msg.msg_iov = iov;
    msg.msg_iovlen = 2;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    n = recvmsg(fd, &msg, 0);
    if (n <= 0) {
        return -1;
    }

    /* 提取文件描述符 */
    fd_count = 0;
    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            fd_ptr = (int *)CMSG_DATA(cmsg);
            int num = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (i = 0; i < num && fd_count < max_fds; i++) {
                fds[fd_count++] = fd_ptr[i];
            }
        }
    }

    return (ssize_t)hdr->size - (ssize_t)sizeof(vfu_msg_hdr_t);
}

/**
 * @brief 发送 vfio-user 消息
 * @param fd      连接 socket
 * @param msg_id  消息 ID
 * @param flags   消息标志
 * @param errno_val 错误码（回复时使用）
 * @param payload payload 数据
 * @param size    payload 字节数
 * @param fds     要传递的文件描述符数组（可为 NULL）
 * @param num_fds 文件描述符数量
 * @return 发送的总字节数，-1 失败
 */
static ssize_t vfu_send_msg(int fd, uint16_t msg_id, uint32_t flags,
                            uint32_t errno_val,
                            const void *payload, uint32_t size,
                            const int *fds, int num_fds)
{
    vfu_msg_hdr_t hdr;
    struct msghdr msg;
    struct iovec iov[2];
    char cmsgbuf[CMSG_SPACE(sizeof(int) * VFIO_USER_MAX_DMA_REGIONS)];
    struct cmsghdr *cmsg = NULL;
    ssize_t n = 0;

    memset(&hdr, 0, sizeof(hdr));
    hdr.id = g_current_req_id;
    hdr.command = g_current_command;
    hdr.size = (uint32_t)sizeof(hdr) + size;
    hdr.flags = flags;
    hdr.error_reply = errno_val;

    memset(&msg, 0, sizeof(msg));
    memset(cmsgbuf, 0, sizeof(cmsgbuf));

    iov[0].iov_base = &hdr;
    iov[0].iov_len = sizeof(hdr);
    if (payload != NULL && size > 0) {
        iov[1].iov_base = (void *)payload;
        iov[1].iov_len = size;
        msg.msg_iovlen = 2;
    } else {
        msg.msg_iovlen = 1;
    }
    msg.msg_iov = iov;

    if (fds != NULL && num_fds > 0) {
        msg.msg_control = cmsgbuf;
        msg.msg_controllen = CMSG_LEN(sizeof(int) * (size_t)num_fds);
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)num_fds);
        memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * (size_t)num_fds);
    }

    n = sendmsg(fd, &msg, MSG_NOSIGNAL);
    if (n < 0) {
        LOG_WARN("vfio-user: sendmsg failed cmd=%u size=%u errno=%d",
                 g_current_command, hdr.size, errno);
    }
    return n;
}

/**
 * @brief 发送 vfio-user 回复消息（带 REPLY 标志，errno=0）
 * @param fd      连接 socket
 * @param msg_id  回复的消息 ID（与请求相同）
 * @param payload 回复 payload
 * @param size    payload 字节数
 * @return 发送字节数，-1 失败
 */
static ssize_t vfu_send_reply(int fd, uint16_t msg_id,
                              const void *payload, uint32_t size)
{
    return vfu_send_msg(fd, msg_id, VFU_MSG_FLAG_REPLY, 0,
                        payload, size, NULL, 0);
}

/**
 * @brief 发送 vfio-user 错误回复（带 REPLY 标志和错误码）
 * @param fd        连接 socket
 * @param msg_id    消息 ID
 * @param errno_val 错误码
 */
static void vfu_send_error(int fd, uint16_t msg_id, uint32_t errno_val)
{
    vfu_send_msg(fd, msg_id, VFU_MSG_FLAG_REPLY, errno_val, NULL, 0, NULL, 0);
}

/* ============================================================
 *  PCIe 配置空间初始化
 * ============================================================ */

/**
 * @brief 初始化 PCIe Type 0 配置空间
 * @details 填充 Vendor ID、Device ID、Class Code、BAR0/1、
 *          Subsystem ID、MSI-X Capability 等字段。
 *          BAR0 为 64-bit 预取内存空间，大小 64KB。
 *          MSI-X Capability 位于偏移 0x40，1个向量。
 */
static void init_pci_config(void)
{
    uint8_t *cfg = g_ctx.pci_config;

    memset(cfg, 0, VFIO_PCI_CONFIG_SIZE);

    /* Offset 0x00: Vendor ID (2B) + Device ID (2B) */
    *(uint16_t *)(cfg + 0x00) = vfu_cpu_to_le16(VFIO_PCI_VENDOR_ID);
    *(uint16_t *)(cfg + 0x02) = vfu_cpu_to_le16(VFIO_PCI_DEVICE_ID);

    /* Offset 0x04: Command (2B) + Status (2B) */
    *(uint16_t *)(cfg + 0x04) = vfu_cpu_to_le16(VFIO_PCI_COMMAND);
    *(uint16_t *)(cfg + 0x06) = vfu_cpu_to_le16(VFIO_PCI_STATUS);

    /* Offset 0x08: Revision ID (1B) + Class Code (3B)
     * Class Code 0x010802: base=0x01(Mass Storage), subclass=0x08(NVMe),
     * prog-if=0x02. 小端存储: 0x08=0x02, 0x09=0x08, 0x0A=0x01 */
    cfg[0x08] = VFIO_PCI_REVISION_ID;
    cfg[0x09] = 0x02U;  /* Prog IF */
    cfg[0x0A] = 0x08U;  /* Subclass */
    cfg[0x0B] = 0x01U;  /* Base Class */

    /* Offset 0x0C: Cache Line Size (1B) + Latency Timer (1B) +
     *              Header Type (1B) + BIST (1B) */
    cfg[0x0C] = (uint8_t)VFIO_PCI_CACHE_LINE_SIZE;
    cfg[0x0D] = 0x00U;  /* Latency Timer */
    cfg[0x0E] = VFIO_PCI_HEADER_TYPE;
    cfg[0x0F] = 0x00U;  /* BIST */

    /* Offset 0x10: BAR0 低32位（64-bit 预取内存，64KB）
     * size mask = ~(0xFFFF) = 0xFFFF0000, flags = 0x0C (64-bit|prefetch) */
    *(uint32_t *)(cfg + 0x10) = vfu_cpu_to_le32(VFIO_PCI_BAR0_LOW);

    /* Offset 0x14: BAR1 = BAR0 高32位（0，32位地址空间足够） */
    *(uint32_t *)(cfg + 0x14) = vfu_cpu_to_le32(VFIO_PCI_BAR1);

    /* Offset 0x18-0x23: BAR2-BAR5 均为 0（未使用） */

    /* Offset 0x24: Cardbus CIS Pointer (4B) = 0 */

    /* Offset 0x28: Subsystem Vendor ID (2B) + Subsystem ID (2B) */
    *(uint16_t *)(cfg + 0x28) = vfu_cpu_to_le16(VFIO_PCI_SUBSYS_VENDOR_ID);
    *(uint16_t *)(cfg + 0x2A) = vfu_cpu_to_le16(VFIO_PCI_SUBSYS_ID);

    /* Offset 0x2C: Expansion ROM Base Address (4B) = 0 */

    /* Offset 0x34: Capability Pointer (1B) = 0x40 */
    cfg[0x34] = VFIO_PCI_CAP_POINTER;

    /* Offset 0x3C: Interrupt Line (1B) + Interrupt Pin (1B) +
     *              Min Grant (1B) + Max Latency (1B) */
    cfg[0x3C] = 0xFFU;  /* Interrupt Line: 无路由 */
    cfg[0x3D] = 0x01U;  /* Interrupt Pin: INTA# */
    cfg[0x3E] = 0x00U;  /* Min Grant */
    cfg[0x3F] = 0x00U;  /* Max Latency */

    /* ---- MSI-X Capability at Offset 0x40 ---- */
    cfg[0x40] = VFIO_PCI_MSIX_CAP_ID;       /* Capability ID = 0x11 */
    cfg[0x41] = 0x00U;                       /* Next Capability Pointer = 0 */
    /* Message Control (2B): Table Size=0(1向量), Function Mask=0, Enable=0
     * 主机驱动会通过写配置空间将 Enable 位置1 */
    *(uint16_t *)(cfg + 0x42) = vfu_cpu_to_le16(0x0000U);
    /* Table Offset/BIR (4B): BAR0 内偏移 0x2000, BIR=0 */
    *(uint32_t *)(cfg + 0x44) = vfu_cpu_to_le32(VFIO_PCI_MSIX_TABLE_OFFSET);
    /* PBA Offset/BIR (4B): BAR0 内偏移 0x3000, BIR=0 */
    *(uint32_t *)(cfg + 0x48) = vfu_cpu_to_le32(VFIO_PCI_MSIX_PBA_OFFSET);
}

/* ============================================================
 *  NVMe 寄存器初始化
 * ============================================================ */

/**
 * @brief 初始化 NVMe 寄存器镜像（BAR0 偏移 0x0000）
 * @details 设置 CAP、VS 等只读寄存器的初始值。CC.EN=0, CSTS.RDY=0。
 */
static void init_nvme_regs(void)
{
    uint32_t *regs32 = (uint32_t *)g_ctx.nvme_regs;
    uint64_t *regs64 = (uint64_t *)g_ctx.nvme_regs;

    memset(g_ctx.nvme_regs, 0, VFIO_BAR0_NVME_REGS_SIZE);

    /* CAP (0x00): Controller Capabilities
     * MQES=0x3FF(1023队列), DSTRD=0(4字节门铃), TO=0x0F, NSSRS=1 */
    regs64[0] = 0x0000000F000001FFULL;

    /* VS (0x08): NVMe 1.4 */
    regs32[2] = NVME_VERSION;

    /* CC (0x14): 0（控制器未使能） */
    /* CSTS (0x1C): 0（未就绪） */
}

/* ============================================================
 *  队列状态管理
 * ============================================================ */

/**
 * @brief 重置所有队列状态
 * @details 在连接建立或控制器复位时调用，清空所有队列的
 *          基地址、大小、头尾指针和相位位。
 */
static void reset_queue_state(void)
{
    memset(g_queues, 0, sizeof(g_queues));
}

/**
 * @brief 从 Admin 队列寄存器更新 Admin 队列信息
 * @details 当 ASQ/ACQ/AQA 寄存器被写入后，更新 qid=0 队列的
 *          基地址和大小，并初始化头尾指针和相位位。
 */
static void update_admin_queue_from_regs(void)
{
    nvme_ctrl_regs_t *ctrl_regs = nvme_ctrl_get_regs();
    vfio_queue_state_t *q = &g_queues[0];

    if (ctrl_regs == NULL) {
        return;
    }

    /* AQA: ASQS(低16位) = SQ大小-1, ACQS(高16位) = CQ大小-1 */
    q->sq_size = (uint16_t)((ctrl_regs->aqa & 0xFFFFU) + 1U);
    q->cq_size = (uint16_t)(((ctrl_regs->aqa >> 16) & 0xFFFFU) + 1U);

    /* ASQ/ACQ 低12位为属性位，实际地址需屏蔽低12位 */
    q->sq_base = ctrl_regs->asq & ~0xFFFULL;
    q->cq_base = ctrl_regs->acq & ~0xFFFULL;

    q->sq_head = 0;
    q->sq_tail = 0;
    q->cq_head = 0;
    q->cq_tail = 0;
    q->cq_phase = true;  /* 初始相位位为1 */
    q->valid = (q->sq_size > 0 && q->cq_size > 0 &&
                q->sq_base != 0 && q->cq_base != 0);

    if (q->valid) {
        LOG_INFO("vfio-user: Admin 队列配置: SQ@0x%llX(%u) CQ@0x%llX(%u)",
                 (unsigned long long)q->sq_base, q->sq_size,
                 (unsigned long long)q->cq_base, q->cq_size);
    }
}

/* ============================================================
 *  MSI-X 中断
 * ============================================================ */

/**
 * @brief 触发 MSI-X 中断
 * @details 通过写 MSI-X eventfd 通知 QEMU 有完成队列条目需要处理。
 *          若 eventfd 未设置（MSI-X 未使能），则回退到发送
 *          VFU_INTERRUPT 消息。
 */
static void trigger_msix_interrupt(void)
{
    if (g_ctx.msix_enabled && g_ctx.msix_evtfd >= 0) {
        uint64_t val = 1;
        /* 写 eventfd 触发中断（非阻塞，失败则忽略） */
        if (write(g_ctx.msix_evtfd, &val, sizeof(val)) < 0) {
            LOG_WARN("vfio-user: 写 MSI-X eventfd 失败, errno=%d", errno);
        }
    } else if (g_ctx.connected && g_ctx.conn_fd >= 0) {
        /* 回退：发送 VFU_INTERRUPT 消息 */
        /* IRQ via eventfd, no vfio-user message needed */
        /* vfu_interrupt_t irq; ... */
    }
}

/* ============================================================
 *  NVMe 命令处理
 * ============================================================ */

/**
 * @brief 处理单个 NVMe 命令
 * @details 根据操作码分发处理：
 *          - Create I/O SQ/CQ：记录队列基地址和大小
 *          - Identify/Get Log Page：调用 nvme_ctrl_fill_* 填充数据，
 *            通过 PRP 写入共享内存
 *          - Read：从 FTL 读取数据，通过 PRP 写入共享内存
 *          - Write：通过 PRP 从共享内存读取数据，写入 FTL
 *          - Write Zeroes：写零到 FTL
 *          - Dataset Mgmt：通过 PRP 读取 range 列表，调用 ftl_trim
 *          - 其他 Admin：nvme_ctrl_process_admin_cmd
 *          - 其他 I/O：nvme_ctrl_process_io_cmd
 * @param cmd NVMe 命令
 * @param cpl 输出：完成队列条目
 * @param qid 队列 ID（0=Admin，1+=I/O）
 */
static void process_nvme_command(const nvme_command_t *cmd,
                                 nvme_completion_t *cpl,
                                 uint16_t qid)
{
    uint16_t cid = vfu_le16_to_cpu(cmd->cid);
    uint64_t prp1 = vfu_le64_to_cpu(cmd->dptr_prp1);
    uint64_t prp2 = vfu_le64_to_cpu(cmd->dptr_prp2);

    memset(cpl, 0, sizeof(nvme_completion_t));
    cpl->cid = vfu_cpu_to_le16(cid);
    cpl->sqid = vfu_cpu_to_le16(qid);

    /* ---- Admin 命令（qid==0）---- */
    if (qid == 0) {
        switch (cmd->opcode) {
        case NVME_ADMIN_IDENTIFY: {
            uint8_t cns = (uint8_t)(vfu_le32_to_cpu(cmd->cdw10) & 0xFF);
            uint8_t id_data[4096];

            memset(id_data, 0, sizeof(id_data));
            if (cns == 0x01) {
                nvme_ctrl_fill_identify_controller(id_data, sizeof(id_data));
            } else if (cns == 0x00) {
                nvme_ctrl_fill_identify_namespace(id_data, sizeof(id_data),
                                                   vfu_le32_to_cpu(cmd->nsid));
            }
            prp_write_data(prp1, prp2, 4096, id_data);
            cpl->status = vfu_cpu_to_le16(0x0000);
            break;
        }
        case NVME_ADMIN_GET_LOG_PAGE: {
            uint8_t lid = (uint8_t)(vfu_le32_to_cpu(cmd->cdw10) & 0xFF);
            uint32_t numd = (vfu_le32_to_cpu(cmd->cdw10) >> 16) & 0xFFFF;
            uint32_t log_len = (numd + 1U) * 4U;
            uint8_t log_data[4096];

            memset(log_data, 0, sizeof(log_data));
            if (lid == NVME_LOG_SMART_HEALTH) {
                nvme_ctrl_fill_smart_log(log_data,
                                         log_len < 512U ? log_len : 512U);
            }
            if (log_len > sizeof(log_data)) {
                log_len = (uint32_t)sizeof(log_data);
            }
            prp_write_data(prp1, prp2, log_len, log_data);
            cpl->status = vfu_cpu_to_le16(0x0000);
            break;
        }
        case NVME_ADMIN_CREATE_IOSQ: {
            /* 记录 I/O SQ 信息：qid = cdw10 低16位，
             * 大小 = cdw10 高16位+1，地址 = PRP1 */
            uint16_t sqid = (uint16_t)(vfu_le32_to_cpu(cmd->cdw10) & 0xFFFF);
            uint16_t sqsize = (uint16_t)((vfu_le32_to_cpu(cmd->cdw10) >> 16) & 0xFFFF) + 1U;

            if (sqid > 0 && sqid < VFIO_MAX_QUEUES) {
                g_queues[sqid].sq_base = prp1 & ~0xFFFULL;
                g_queues[sqid].sq_size = sqsize;
                g_queues[sqid].sq_head = 0;
                g_queues[sqid].sq_tail = 0;
                g_queues[sqid].valid = (g_queues[sqid].cq_size > 0);
                LOG_INFO("vfio-user: Create IOSQ qid=%u, base=0x%llX, size=%u",
                         sqid, (unsigned long long)g_queues[sqid].sq_base, sqsize);
            }
            nvme_ctrl_process_admin_cmd(cmd, cpl);
            break;
        }
        case NVME_ADMIN_CREATE_IOCQ: {
            /* 记录 I/O CQ 信息：qid = cdw10 低16位，
             * 大小 = cdw10 高16位+1，地址 = PRP1 */
            uint16_t cqid = (uint16_t)(vfu_le32_to_cpu(cmd->cdw10) & 0xFFFF);
            uint16_t cqsize = (uint16_t)((vfu_le32_to_cpu(cmd->cdw10) >> 16) & 0xFFFF) + 1U;

            if (cqid > 0 && cqid < VFIO_MAX_QUEUES) {
                g_queues[cqid].cq_base = prp1 & ~0xFFFULL;
                g_queues[cqid].cq_size = cqsize;
                g_queues[cqid].cq_head = 0;
                g_queues[cqid].cq_tail = 0;
                g_queues[cqid].cq_phase = true;
                g_queues[cqid].valid = (g_queues[cqid].sq_size > 0);
                LOG_INFO("vfio-user: Create IOCQ qid=%u, base=0x%llX, size=%u",
                         cqid, (unsigned long long)g_queues[cqid].cq_base, cqsize);
            }
            nvme_ctrl_process_admin_cmd(cmd, cpl);
            break;
        }
        case NVME_ADMIN_DELETE_IOSQ: {
            uint16_t sqid = (uint16_t)(vfu_le32_to_cpu(cmd->cdw10) & 0xFFFF);
            if (sqid > 0 && sqid < VFIO_MAX_QUEUES) {
                g_queues[sqid].sq_size = 0;
                g_queues[sqid].valid = false;
            }
            nvme_ctrl_process_admin_cmd(cmd, cpl);
            break;
        }
        case NVME_ADMIN_DELETE_IOCQ: {
            uint16_t cqid = (uint16_t)(vfu_le32_to_cpu(cmd->cdw10) & 0xFFFF);
            if (cqid > 0 && cqid < VFIO_MAX_QUEUES) {
                g_queues[cqid].cq_size = 0;
                g_queues[cqid].valid = false;
            }
            nvme_ctrl_process_admin_cmd(cmd, cpl);
            break;
        }
        default:
            nvme_ctrl_process_admin_cmd(cmd, cpl);
            break;
        }
        return;
    }

    /* ---- I/O 命令（qid>=1）---- */
    switch (cmd->opcode) {
    case NVME_IO_READ: {
        uint64_t slba = ((uint64_t)vfu_le32_to_cpu(cmd->cdw11) << 32) |
                        vfu_le32_to_cpu(cmd->cdw10);
        uint16_t nlb = (uint16_t)(vfu_le16_to_cpu(cmd->cdw12) & 0xFFFF) + 1U;
        uint32_t transfer_len = nlb * VFIO_PAGE_SIZE;
        uint32_t i = 0;

        if (transfer_len > VFIO_USER_NVME_MAX_IO_SIZE) {
            cpl->status = vfu_cpu_to_le16(NVME_SC_INTERNAL_ERROR);
            break;
        }

        for (i = 0; i < nlb; i++) {
            if (ftl_read((uint32_t)(slba + i),
                         g_io_read_buf + i * VFIO_PAGE_SIZE) != RET_OK) {
                memset(g_io_read_buf + i * VFIO_PAGE_SIZE, 0, VFIO_PAGE_SIZE);
            }
        }
        prp_write_data(prp1, prp2, transfer_len, g_io_read_buf);
        cpl->status = vfu_cpu_to_le16(NVME_SC_SUCCESS);
        break;
    }
    case NVME_IO_WRITE: {
        uint64_t slba = ((uint64_t)vfu_le32_to_cpu(cmd->cdw11) << 32) |
                        vfu_le32_to_cpu(cmd->cdw10);
        uint16_t nlb = (uint16_t)(vfu_le16_to_cpu(cmd->cdw12) & 0xFFFF) + 1U;
        uint32_t transfer_len = nlb * VFIO_PAGE_SIZE;
        uint32_t i = 0;
        uint16_t status = NVME_SC_SUCCESS;

        if (transfer_len > VFIO_USER_NVME_MAX_IO_SIZE) {
            cpl->status = vfu_cpu_to_le16(NVME_SC_INTERNAL_ERROR);
            break;
        }

        if (prp_read_data(prp1, prp2, transfer_len, g_io_write_buf) != RET_OK) {
            cpl->status = vfu_cpu_to_le16(NVME_SC_DATA_XFER_ERROR);
            break;
        }

        for (i = 0; i < nlb; i++) {
            if (ftl_write((uint32_t)(slba + i),
                          g_io_write_buf + i * VFIO_PAGE_SIZE) != RET_OK) {
                status = NVME_SC_INTERNAL_ERROR;
                break;
            }
        }
        cpl->status = vfu_cpu_to_le16(status);
        break;
    }
    case NVME_IO_WRITE_ZEROES: {
        uint64_t slba = ((uint64_t)vfu_le32_to_cpu(cmd->cdw11) << 32) |
                        vfu_le32_to_cpu(cmd->cdw10);
        uint16_t nlb = (uint16_t)(vfu_le16_to_cpu(cmd->cdw12) & 0xFFFF) + 1U;
        uint32_t i = 0;
        uint16_t status = NVME_SC_SUCCESS;

        for (i = 0; i < nlb; i++) {
            if (ftl_write((uint32_t)(slba + i), g_zero_buf) != RET_OK) {
                status = NVME_SC_INTERNAL_ERROR;
                break;
            }
        }
        cpl->status = vfu_cpu_to_le16(status);
        break;
    }
    case NVME_IO_DATASET_MGMT: {
        uint8_t nr = (uint8_t)(vfu_le32_to_cpu(cmd->cdw10) & 0xFF) + 1U;
        uint32_t transfer_len = (uint32_t)nr * 16U;
        uint32_t r = 0;
        uint16_t status = NVME_SC_SUCCESS;

        if (transfer_len > VFIO_USER_NVME_MAX_IO_SIZE) {
            cpl->status = vfu_cpu_to_le16(NVME_SC_INTERNAL_ERROR);
            break;
        }

        if (prp_read_data(prp1, prp2, transfer_len, g_io_write_buf) != RET_OK) {
            cpl->status = vfu_cpu_to_le16(NVME_SC_DATA_XFER_ERROR);
            break;
        }

        for (r = 0; r < nr; r++) {
            uint8_t *range = g_io_write_buf + r * 16U;
            uint32_t length = vfu_le32_to_cpu(*(uint32_t *)(range + 4)) + 1U;
            uint64_t range_slba = vfu_le64_to_cpu(*(uint64_t *)(range + 8));
            if (ftl_trim((uint32_t)range_slba, length) != RET_OK) {
                status = NVME_SC_INTERNAL_ERROR;
            }
        }
        cpl->status = vfu_cpu_to_le16(status);
        break;
    }
    default:
        nvme_ctrl_process_io_cmd(cmd, cpl);
        break;
    }
}

/**
 * @brief 处理一个队列中所有待处理的 SQ 命令
 * @details 从 SQ 头指针到尾指针之间读取所有 nvme_command_t（64字节），
 *          逐个调用 process_nvme_command 处理，将完成写入 CQ，
 *          更新 CQ 尾指针和相位位，最后触发 MSI-X 中断。
 * @param qid 队列 ID
 */
static void process_queue_commands(uint16_t qid)
{
    vfio_queue_state_t *q = NULL;
    nvme_command_t *sq_entry = NULL;
    nvme_completion_t *cq_entry = NULL;
    nvme_completion_t cpl;
    bool wrote_completion = false;

    if (qid >= VFIO_MAX_QUEUES) {
        return;
    }

    q = &g_queues[qid];
    if (!q->valid || q->sq_size == 0 || q->cq_size == 0) {
        return;
    }

    /* 处理 SQ 中从 head 到 tail 的所有命令 */
    while (q->sq_head != q->sq_tail) {
        /* 从 guest 内存 SQ 读取命令（64字节） */
        sq_entry = (nvme_command_t *)gpa_to_hva(
            q->sq_base + (uint64_t)q->sq_head * sizeof(nvme_command_t));
        if (sq_entry == NULL) {
            LOG_ERROR("vfio-user: SQ 命令地址转换失败, qid=%u, head=%u",
                      qid, q->sq_head);
            break;
        }

        /* 处理命令 */
        process_nvme_command(sq_entry, &cpl, qid);

        /* 写入完成到 CQ（16字节） */
        cq_entry = (nvme_completion_t *)gpa_to_hva(
            q->cq_base + (uint64_t)q->cq_tail * sizeof(nvme_completion_t));
        if (cq_entry == NULL) {
            LOG_ERROR("vfio-user: CQ 完成地址转换失败, qid=%u, tail=%u",
                      qid, q->cq_tail);
            break;
        }

        /* 设置 SQ Head Pointer（当前命令的下一个位置） */
        cpl.sqhd = vfu_cpu_to_le16((uint16_t)((q->sq_head + 1U) % q->sq_size));

        /* 设置相位位：status 的 bit0 为相位位 */
        if (q->cq_phase) {
            cpl.status |= vfu_cpu_to_le16(0x0001U);
        } else {
            cpl.status &= vfu_cpu_to_le16((uint16_t)~0x0001U);
        }

        memcpy(cq_entry, &cpl, sizeof(nvme_completion_t));
        wrote_completion = true;

        /* 内存屏障：确保 CQ 写入对主机可见 */
        __sync_synchronize();

        /* 更新 CQ 尾指针，满时翻转相位位 */
        q->cq_tail++;
        if (q->cq_tail >= q->cq_size) {
            q->cq_tail = 0;
            q->cq_phase = !q->cq_phase;
        }

        /* 更新 SQ 头指针 */
        q->sq_head++;
        if (q->sq_head >= q->sq_size) {
            q->sq_head = 0;
        }
    }

    /* 有完成条目时触发中断 */
    if (wrote_completion) {
        trigger_msix_interrupt();
    }
}

/* ============================================================
 *  NVMe 寄存器写处理
 * ============================================================ */

/**
 * @brief 处理 NVMe 寄存器写操作
 * @details 更新寄存器镜像，对特殊寄存器做出响应：
 *          - CC.EN 变化：EN=1 时设置 CSTS.RDY=1
 *          - AQA/ASQ/ACQ：更新 Admin 队列配置
 *          - 门铃寄存器（偏移 >= 0x1000）：更新 SQ tail/CQ head，
 *            SQ 门铃触发命令处理
 * @param offset BAR0 内偏移（NVMe 寄存器区域）
 * @param size   写入长度（1/2/4字节）
 * @param value  写入值
 */
static void handle_nvme_reg_write(uint32_t offset, uint32_t size, uint32_t value)
{
    nvme_ctrl_regs_t *ctrl_regs = nvme_ctrl_get_regs();

    /* 写入寄存器镜像 */
    if (offset + size <= VFIO_BAR0_NVME_REGS_SIZE) {
        memcpy(g_ctx.nvme_regs + offset, &value, size);
    }

    /* 同步到 nvme_controller 寄存器并处理特殊逻辑 */
    switch (offset) {
    case NVME_REG_CC: {
        if (ctrl_regs != NULL) {
            ctrl_regs->cc = value;
            if (value & 0x01U) {
                /* CC.EN=1：使能控制器，设置 CSTS.RDY=1 */
                ctrl_regs->csts |= 0x01U;
                g_ctx.nvme_regs[NVME_REG_CSTS] = 0x01U;
                LOG_INFO("vfio-user: CC.EN=1, CSTS.RDY=1");
            } else {
                /* CC.EN=0：复位控制器 */
                ctrl_regs->csts &= ~0x01U;
                g_ctx.nvme_regs[NVME_REG_CSTS] = 0x00U;
                reset_queue_state();
                LOG_INFO("vfio-user: CC.EN=0, CSTS.RDY=0, 队列已复位");
            }
        }
        break;
    }
    case NVME_REG_AQA:
        if (ctrl_regs != NULL) {
            ctrl_regs->aqa = value;
            update_admin_queue_from_regs();
        }
        break;
    case NVME_REG_ASQ:
        if (ctrl_regs != NULL) {
            /* ASQ 是64位，分高低32位写（QEMU 通常按32位写） */
            ctrl_regs->asq = (ctrl_regs->asq & ~0xFFFFFFFFULL) | value;
            update_admin_queue_from_regs();
        }
        break;
    case NVME_REG_ASQ + 4:
        if (ctrl_regs != NULL) {
            ctrl_regs->asq = (ctrl_regs->asq & 0xFFFFFFFFULL) |
                             ((uint64_t)value << 32);
            /* ASQ 高32位写完后，Admin 队列地址完整，更新队列状态 */
            update_admin_queue_from_regs();
        }
        break;
    case NVME_REG_ACQ:
        if (ctrl_regs != NULL) {
            ctrl_regs->acq = (ctrl_regs->acq & ~0xFFFFFFFFULL) | value;
            update_admin_queue_from_regs();
        }
        break;
    case NVME_REG_ACQ + 4:
        if (ctrl_regs != NULL) {
            ctrl_regs->acq = (ctrl_regs->acq & 0xFFFFFFFFULL) |
                             ((uint64_t)value << 32);
            update_admin_queue_from_regs();
        }
        break;
    default:
        /* 其他寄存器直接写入镜像即可 */
        break;
    }
}

/**
 * @brief 处理门铃寄存器写
 * @param doorbell_offset 门铃区域内偏移（相对于 0x1000）
 * @param value           写入值（SQ tail 或 CQ head）
 */
static void handle_doorbell_write(uint32_t doorbell_offset, uint32_t value)
{
    uint16_t qid = (uint16_t)(doorbell_offset / 8U);
    bool is_sq = ((doorbell_offset % 8U) == 0U);

    if (qid >= VFIO_MAX_QUEUES) {
        return;
    }

    if (is_sq) {
        /* SQ Tail Doorbell：更新 SQ 尾指针，触发命令处理 */
        g_queues[qid].sq_tail = (uint16_t)(value & 0xFFFFU);
        nvme_ctrl_sq_doorbell(qid, value);
        process_queue_commands(qid);
    } else {
        /* CQ Head Doorbell：更新 CQ 头指针（主机已消费完成） */
        g_queues[qid].cq_head = (uint16_t)(value & 0xFFFFU);
        nvme_ctrl_cq_doorbell(qid, value);
    }
}

/* ============================================================
 *  BAR0 读写处理
 * ============================================================ */

/**
 * @brief 从 BAR0 读取数据
 * @details 根据偏移分发到不同子区域：
 *          - 0x0000-0x0FFF: NVMe 控制器寄存器
 *          - 0x1000-0x1FFF: 门铃寄存器（只读返回0）
 *          - 0x2000-0x2FFF: MSI-X 向量表
 *          - 0x3000-0x3FFF: MSI-X PBA
 * @param offset BAR0 内偏移
 * @param count  读取字节数
 * @param buf    输出缓冲区
 */
static void bar0_read(uint64_t offset, uint32_t count, uint8_t *buf)
{
    if (offset + count > VFIO_BAR0_SIZE) {
        if (offset >= VFIO_BAR0_SIZE) {
            return;
        }
        count = (uint32_t)(VFIO_BAR0_SIZE - offset);
    }

    if (offset < VFIO_BAR0_NVME_REGS_SIZE) {
        /* NVMe 寄存器区域 */
        uint32_t reg_end = (uint32_t)VFIO_BAR0_NVME_REGS_SIZE;
        uint32_t copy_len = count;
        if (offset + count > reg_end) {
            copy_len = reg_end - (uint32_t)offset;
        }
        /* 同步 CSTS 等只读寄存器 */
        {
            nvme_ctrl_regs_t *ctrl_regs = nvme_ctrl_get_regs();
            if (ctrl_regs != NULL) {
                g_ctx.nvme_regs[NVME_REG_CSTS] = (uint8_t)(ctrl_regs->csts & 0xFF);
            }
        }
        memcpy(buf, g_ctx.nvme_regs + offset, copy_len);
    } else if (offset >= VFIO_BAR0_MSIX_TABLE_OFFSET &&
               offset < VFIO_BAR0_MSIX_TABLE_OFFSET + 0x1000U) {
        /* MSI-X 向量表区域 */
        uint32_t tbl_off = (uint32_t)(offset - VFIO_BAR0_MSIX_TABLE_OFFSET);
        if (tbl_off < sizeof(g_ctx.msix_table)) {
            uint32_t copy_len = count;
            if (tbl_off + count > sizeof(g_ctx.msix_table)) {
                copy_len = (uint32_t)sizeof(g_ctx.msix_table) - tbl_off;
            }
            memcpy(buf, g_ctx.msix_table + tbl_off, copy_len);
        } else {
            memset(buf, 0, count);
        }
    } else if (offset >= VFIO_BAR0_MSIX_PBA_OFFSET &&
               offset < VFIO_BAR0_MSIX_PBA_OFFSET + 0x1000U) {
        /* MSI-X PBA 区域 */
        uint32_t pba_off = (uint32_t)(offset - VFIO_BAR0_MSIX_PBA_OFFSET);
        if (pba_off < sizeof(g_ctx.msix_pba)) {
            uint32_t copy_len = count;
            if (pba_off + count > sizeof(g_ctx.msix_pba)) {
                copy_len = (uint32_t)sizeof(g_ctx.msix_pba) - pba_off;
            }
            memcpy(buf, g_ctx.msix_pba + pba_off, copy_len);
        } else {
            memset(buf, 0, count);
        }
    } else {
        /* 门铃区域或保留区域：读返回0 */
        memset(buf, 0, count);
    }
}

/**
 * @brief 向 BAR0 写入数据
 * @details 根据偏移分发到不同子区域：
 *          - 0x0000-0x0FFF: NVMe 控制器寄存器（handle_nvme_reg_write）
 *          - 0x1000-0x1FFF: 门铃寄存器（handle_doorbell_write）
 *          - 0x2000-0x2FFF: MSI-X 向量表（更新向量配置）
 *          - 0x3000-0x3FFF: MSI-X PBA（只读，忽略写）
 * @param offset BAR0 内偏移
 * @param count  写入字节数
 * @param buf    输入数据
 */
static void bar0_write(uint64_t offset, uint32_t count, const uint8_t *buf)
{
    if (offset + count > VFIO_BAR0_SIZE) {
        return;
    }

    if (offset < VFIO_BAR0_NVME_REGS_SIZE) {
        /* NVMe 寄存器区域：按4字节粒度处理 */
        uint32_t i = 0;
        for (i = 0; i + 4 <= count; i += 4) {
            uint32_t value = 0;
            memcpy(&value, buf + i, 4);
            handle_nvme_reg_write((uint32_t)offset + i, 4, value);
        }
        /* 处理剩余的1-3字节 */
        if (i < count) {
            uint32_t value = 0;
            memcpy(&value, buf + i, count - i);
            handle_nvme_reg_write((uint32_t)offset + i, count - i, value);
        }
    } else if (offset >= VFIO_BAR0_DOORBELL_OFFSET &&
               offset < VFIO_BAR0_DOORBELL_OFFSET + 0x1000U) {
        /* 门铃寄存器区域：每个门铃4字节 */
        uint32_t db_off = (uint32_t)(offset - VFIO_BAR0_DOORBELL_OFFSET);
        uint32_t i = 0;
        for (i = 0; i + 4 <= count; i += 4) {
            uint32_t value = 0;
            memcpy(&value, buf + i, 4);
            handle_doorbell_write(db_off + i, value);
        }
    } else if (offset >= VFIO_BAR0_MSIX_TABLE_OFFSET &&
               offset < VFIO_BAR0_MSIX_TABLE_OFFSET + 0x1000U) {
        /* MSI-X 向量表：主机写入中断地址和数据 */
        uint32_t tbl_off = (uint32_t)(offset - VFIO_BAR0_MSIX_TABLE_OFFSET);
        if (tbl_off + count <= sizeof(g_ctx.msix_table)) {
            memcpy(g_ctx.msix_table + tbl_off, buf, count);
        }
    } else {
        /* MSI-X PBA 或保留区域：忽略写 */
    }
}

/* ============================================================
 *  PCI 配置空间写处理
 * ============================================================ */

/**
 * @brief 处理 PCI 配置空间写
 * @details 支持 Command 寄存器（Memory/IO Space enable）和
 *          MSI-X Message Control 寄存器（Enable 位）的写操作。
 *          BAR 寄存器写用于大小探测（写全1后读回），需正确处理。
 * @param offset 配置空间内偏移
 * @param count  写入字节数
 * @param buf    输入数据
 */
static void pci_config_write(uint32_t offset, uint32_t count, const uint8_t *buf)
{
    if (offset + count > VFIO_PCI_CONFIG_SIZE) {
        return;
    }

    /* MSI-X Message Control 寄存器（偏移 0x42，2字节） */
    if (offset <= 0x42U && offset + count >= 0x44U) {
        uint16_t msg_ctrl = 0;
        memcpy(&msg_ctrl, g_ctx.pci_config + 0x42, 2);

        /* 先写入原始数据 */
        memcpy(g_ctx.pci_config + offset, buf, count);

        /* 重新读取（可能被本次写更新） */
        memcpy(&msg_ctrl, g_ctx.pci_config + 0x42, 2);
        msg_ctrl = vfu_le16_to_cpu(msg_ctrl);

        /* Table Size 位是只读的，强制保持为0（1向量） */
        msg_ctrl &= 0xC000U;  /* 只保留 Enable 和 Function Mask */
        msg_ctrl |= 0x0000U;  /* Table Size = 0 */

        g_ctx.msix_enabled = ((msg_ctrl & 0x8000U) != 0);

        /* 写回（Table Size 硬编码为0） */
        *(uint16_t *)(g_ctx.pci_config + 0x42) = vfu_cpu_to_le16(msg_ctrl);

        LOG_INFO("vfio-user: MSI-X 控制字=0x%04X, enabled=%u",
                 msg_ctrl, g_ctx.msix_enabled ? 1U : 0U);
        return;
    }

    /* BAR0 寄存器（偏移 0x10，4字节）：处理大小探测
     * 主机写全1(0xFFFFFFFF)来探测 BAR 大小，读时应返回 size mask。
     * 正常写入时更新 BAR 地址（但我们的 BAR 是固定映射的，
     * 实际地址由 QEMU vfio-user-pci 管理，这里只做镜像） */
    if (offset == 0x10U && count == 4U) {
        uint32_t value = 0;
        memcpy(&value, buf, 4);
        if (value == 0xFFFFFFFFU) {
            /* 大小探测：返回 size mask | flags */
            *(uint32_t *)(g_ctx.pci_config + 0x10) =
                vfu_cpu_to_le32(VFIO_PCI_BAR0_LOW);
        } else {
            /* 正常写入：保留 flags，更新地址位 */
            uint32_t new_bar = (value & ~0xFFFFU) | (VFIO_PCI_BAR0_LOW & 0xFFFFU);
            *(uint32_t *)(g_ctx.pci_config + 0x10) = vfu_cpu_to_le32(new_bar);
        }
        return;
    }

    /* BAR1 寄存器（偏移 0x14，4字节）：BAR0 高32位，大小探测返回0 */
    if (offset == 0x14U && count == 4U) {
        uint32_t value = 0;
        memcpy(&value, buf, 4);
        if (value == 0xFFFFFFFFU) {
            *(uint32_t *)(g_ctx.pci_config + 0x14) = 0x00000000U;
        } else {
            *(uint32_t *)(g_ctx.pci_config + 0x14) = vfu_cpu_to_le32(value);
        }
        return;
    }

    /* 其他寄存器：直接写入 */
    memcpy(g_ctx.pci_config + offset, buf, count);
}

/* ============================================================
 *  vfio-user 消息处理函数
 * ============================================================ */

/**
 * @brief 处理 VFU_VERSION
 * @details 回复 vfio-user API 版本号（0）。
 */
static void handle_get_api_version(void)
{
    uint32_t version = 0;

    LOG_INFO("vfio-user: GET_API_VERSION, version=%u", version);
    vfu_send_reply(g_ctx.conn_fd, VFU_VERSION,
                   &version, sizeof(version));
}

/**
 * @brief 处理 VFU_DEVICE_RESET
 * @details 复位设备状态：重置队列、寄存器和 PCI 配置空间。
 */
static void handle_set_reset(void)
{
    LOG_INFO("vfio-user: SET_RESET");

    reset_queue_state();
    init_nvme_regs();
    init_pci_config();

    if (g_ctx.msix_evtfd >= 0) {
        close(g_ctx.msix_evtfd);
        g_ctx.msix_evtfd = -1;
    }
    g_ctx.msix_enabled = false;

    vfu_send_reply(g_ctx.conn_fd, VFU_DEVICE_RESET, NULL, 0);
}

/**
 * @brief 处理 VFU_DMA_MAP
 * @details 接收 DMA 区域描述符和对应的文件描述符（通过 SCM_RIGHTS），
 *          对每个区域进行 mmap 建立 GPA→HVA 映射。
 * @param payload vfu_dma_map_t 结构
 * @param fd      内存区域文件描述符
 */
static void handle_dma_map(const uint8_t *payload, int fd)
{
    const vfu_dma_map_t *map = (const vfu_dma_map_t *)payload;
    uint64_t iova = vfu_le64_to_cpu(map->iova);
    uint64_t size = vfu_le64_to_cpu(map->size);
    uint64_t offset = vfu_le64_to_cpu(map->offset);
    uint32_t idx = 0;
    void *addr = NULL;

    if (g_ctx.dma_region_count >= VFIO_USER_MAX_DMA_REGIONS) {
        LOG_ERROR("vfio-user: DMA 区域数已达上限");
        if (fd >= 0) close(fd);
        vfu_send_error(g_ctx.conn_fd, VFU_DMA_MAP, ENOMEM);
        return;
    }

    if (fd < 0) {
        LOG_WARN("vfio-user: DMA_MAP 无FD，使用匿名映射");
        addr = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    } else {
        addr = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fd, (off_t)offset);
    }
    if (addr == MAP_FAILED) {
        LOG_ERROR("vfio-user: DMA mmap 失败, iova=0x%llX, size=%llu, errno=%d",
                  (unsigned long long)iova, (unsigned long long)size, errno);
        if (fd >= 0) close(fd);
        vfu_send_error(g_ctx.conn_fd, VFU_DMA_MAP, errno);
        return;
    }

    idx = g_ctx.dma_region_count;
    g_ctx.dma_regions[idx].iova = iova;
    g_ctx.dma_regions[idx].size = size;
    g_ctx.dma_regions[idx].offset = offset;
    g_ctx.dma_mappings[idx] = addr;
    g_ctx.dma_fds[idx] = fd;
    g_ctx.dma_region_count++;

    LOG_INFO("vfio-user: DMA_MAP[%u]: GPA=0x%llX size=%llu -> HVA=%p",
             idx, (unsigned long long)iova, (unsigned long long)size, addr);

    vfu_send_reply(g_ctx.conn_fd, VFU_DMA_MAP, NULL, 0);
}

/**
 * @brief 处理 VFU_DMA_UNMAP
 * @details 解除指定 GPA 范围的 mmap 映射并关闭文件描述符。
 * @param payload vfu_dma_unmap_t 结构
 */
static void handle_dma_unmap(const uint8_t *payload)
{
    const vfu_dma_unmap_t *unmap = (const vfu_dma_unmap_t *)payload;
    uint64_t iova = vfu_le64_to_cpu(unmap->iova);
    uint64_t size = vfu_le64_to_cpu(unmap->size);
    uint32_t i = 0;

    for (i = 0; i < g_ctx.dma_region_count; i++) {
        if (g_ctx.dma_regions[i].iova == iova &&
            g_ctx.dma_regions[i].size == size) {
            if (g_ctx.dma_mappings[i] != NULL) {
                munmap(g_ctx.dma_mappings[i], (size_t)size);
                g_ctx.dma_mappings[i] = NULL;
            }
            if (g_ctx.dma_fds[i] >= 0) {
                close(g_ctx.dma_fds[i]);
                g_ctx.dma_fds[i] = -1;
            }
            /* 将最后一个区域移到当前位置以保持紧凑 */
            if (i < g_ctx.dma_region_count - 1) {
                uint32_t last = g_ctx.dma_region_count - 1;
                g_ctx.dma_regions[i] = g_ctx.dma_regions[last];
                g_ctx.dma_mappings[i] = g_ctx.dma_mappings[last];
                g_ctx.dma_fds[i] = g_ctx.dma_fds[last];
            }
            g_ctx.dma_region_count--;
            LOG_INFO("vfio-user: DMA_UNMAP: GPA=0x%llX size=%llu",
                     (unsigned long long)iova, (unsigned long long)size);
            break;
        }
    }

    vfu_send_reply(g_ctx.conn_fd, VFU_DMA_UNMAP, NULL, 0);
}

/**
 * @brief 处理 VFU_DEVICE_GET_INFO
 * @details 回复设备信息：num_regions=2（PCI config + BAR0），
 *          num_irqs=1（MSI-X）。
 */
static void handle_device_get_info(void)
{
    vfu_device_info_t info;

    memset(&info, 0, sizeof(info));
    info.argsz = (uint32_t)sizeof(info);
    info.flags = 0x2;  /* VFIO_DEVICE_FLAGS_PCI */
    info.num_regions = VFIO_REGION_COUNT;
    info.num_irqs = VFIO_IRQ_COUNT;
    info.cap_offset = 0;

    LOG_INFO("vfio-user: DEVICE_GET_INFO, regions=%u, irqs=%u",
             info.num_regions, info.num_irqs);

    vfu_send_reply(g_ctx.conn_fd, VFU_DEVICE_GET_INFO,
                   &info, sizeof(info));
}

/**
 * @brief 处理 VFU_DEVICE_GET_REGION_INFO
 * @details 根据 region index 回复对应 region 的信息：
 *          - index=0: PCI 配置空间（256字节，type=PCI_CONFIG）
 *          - index=1: BAR0（64KB，type=PCI_BAR，subtype=0）
 * @param payload vfu_region_info_t（含请求的 index）
 */
static void handle_device_get_region_info(const uint8_t *payload)
{
    const vfu_region_info_t *req = (const vfu_region_info_t *)payload;
    uint32_t index = vfu_le32_to_cpu(req->index);
    vfu_region_info_t info;

    memset(&info, 0, sizeof(info));
    info.argsz = (uint32_t)sizeof(info);
    info.index = index;

    if (index == VFIO_REGION_BAR0) {
        info.size = VFIO_BAR0_SIZE;
        info.offset = 0;
    } else if (index == VFIO_REGION_PCI_CONFIG) {
        info.size = VFIO_PCI_CONFIG_SIZE;
        info.offset = 0;
    } else {
        /* BAR1-BAR5, ROM, VGA: 未使用，size=0 */
        info.size = 0;
        info.offset = 0;
    }

    LOG_INFO("vfio-user: GET_REGION_INFO[%u]: size=%llu",
             index, (unsigned long long)info.size);

    vfu_send_reply(g_ctx.conn_fd, VFU_DEVICE_GET_REGION_INFO,
                   &info, sizeof(info));
}

/**
 * @brief 处理 VFU_DEVICE_GET_IRQ_INFO
 * @details 回复 IRQ 信息：MSI-X，1个向量。
 * @param payload vfu_irq_info_t（含请求的 index）
 */
static void handle_device_get_irq_info(const uint8_t *payload)
{
    const vfu_irq_info_t *req = (const vfu_irq_info_t *)payload;
    uint32_t index = vfu_le32_to_cpu(req->index);
    vfu_irq_info_t info;

    memset(&info, 0, sizeof(info));
    info.argsz = (uint32_t)sizeof(info);
    info.index = index;

    if (index == VFIO_IRQ_INTX) {
        info.count = 1;  /* INTx 支持 1 个向量 */
    } else if (index == VFIO_IRQ_MSI) {
        info.count = 0;  /* 不支持 MSI */
    } else if (index == VFIO_IRQ_MSIX) {
        info.count = VFIO_MSIX_VECTOR_COUNT;
    } else {
        info.count = 0;
    }

    LOG_INFO("vfio-user: GET_IRQ_INFO[%u]: count=%u", index, info.count);

    vfu_send_reply(g_ctx.conn_fd, VFU_DEVICE_GET_IRQ_INFO,
                   &info, sizeof(info));
}

/**
 * @brief 处理 VFU_DEVICE_SET_IRQS
 * @details 设置 MSI-X 中断的 eventfd。data_type=EVENTFD 时从
 *          SCM_RIGHTS 接收 eventfd 并保存；data_type=NONE 时
 *          禁用中断（关闭已有 eventfd）。
 * @param payload vfu_set_irqs_t
 * @param fd      eventfd（通过 SCM_RIGHTS 传递，可为 -1）
 */
static void handle_device_set_irqs(const uint8_t *payload, int fd)
{
    const vfu_set_irqs_t *irqs = (const vfu_set_irqs_t *)payload;
    uint32_t index = vfu_le32_to_cpu(irqs->index);
    uint32_t irq_flags = vfu_le32_to_cpu(irqs->flags);
    bool is_eventfd = (irq_flags & 0x4) != 0;

    if (index == VFIO_IRQ_INTX || index == VFIO_IRQ_MSI) {
        /* INTx/MSI 不支持，直接返回成功 */
        if (fd >= 0) close(fd);
        vfu_send_reply(g_ctx.conn_fd, VFU_DEVICE_SET_IRQS, NULL, 0);
        return;
    }

    if (index != VFIO_IRQ_MSIX) {
        LOG_WARN("vfio-user: SET_IRQS 未知 index=%u", index);
        if (fd >= 0) close(fd);
        vfu_send_error(g_ctx.conn_fd, VFU_DEVICE_SET_IRQS, EINVAL);
        return;
    }

    /* 关闭旧的 eventfd */
    if (g_ctx.msix_evtfd >= 0) {
        close(g_ctx.msix_evtfd);
        g_ctx.msix_evtfd = -1;
    }

    if (is_eventfd && fd >= 0) {
        g_ctx.msix_evtfd = fd;
        LOG_INFO("vfio-user: SET_IRQS MSI-X eventfd=%d", fd);
    } else {
        /* NONE 或无 FD：禁用中断 */
        g_ctx.msix_evtfd = -1;
        if (fd >= 0) close(fd);
        LOG_INFO("vfio-user: SET_IRQS MSI-X 禁用");
    }

    vfu_send_reply(g_ctx.conn_fd, VFU_DEVICE_SET_IRQS, NULL, 0);
}

/**
 * @brief 处理 VFU_REGION_READ
 * @details 从指定 region 读取数据：
 *          - region 0: PCI 配置空间
 *          - region 1: BAR0（NVMe 寄存器/门铃/MSI-X）
 *          回复中携带读取的数据。
 * @param payload vfu_region_rw_t（含 region_index, offset, count）
 */
static void handle_region_read(const uint8_t *payload)
{
    const vfu_region_rw_t *req = (const vfu_region_rw_t *)payload;
    uint32_t region_index = vfu_le32_to_cpu(req->region);
    uint64_t offset = vfu_le64_to_cpu(req->offset);
    uint32_t count = vfu_le32_to_cpu(req->count);
    static uint8_t reply_buf[sizeof(vfu_region_rw_t) + VFIO_BAR0_SIZE];
    vfu_region_rw_t *reply = (vfu_region_rw_t *)reply_buf;
    uint32_t reply_size = 0;

    if (count > VFIO_BAR0_SIZE) {
        count = VFIO_BAR0_SIZE;
    }

    memset(reply_buf, 0, sizeof(reply_buf));
    reply->offset = vfu_cpu_to_le64(offset);
    reply->region = vfu_cpu_to_le32(region_index);
    reply->count = vfu_cpu_to_le32(count);

    if (region_index == VFIO_REGION_PCI_CONFIG) {
        if (offset + count > VFIO_PCI_CONFIG_SIZE) {
            if (offset >= VFIO_PCI_CONFIG_SIZE) {
                count = 0;
            } else {
                count = VFIO_PCI_CONFIG_SIZE - (uint32_t)offset;
            }
        }
        reply->count = vfu_cpu_to_le32(count);
        if (count > 0) {
            memcpy(reply->data, g_ctx.pci_config + offset, count);
        }
    } else if (region_index == VFIO_REGION_BAR0) {
        bar0_read(offset, count, reply->data);
    } else {
        /* 其他 region：返回全 0 */
        LOG_INFO("vfio-user: REGION_READ region=%u offset=%llu count=%u (zero)",
                 region_index, (unsigned long long)offset, count);
    }

    /* 回复包含完整的 region_rw 结构（offset+region+count+data） */
    reply_size = 16U + count;  /* offsetof(vfu_region_rw_t, data) = 16 */
    vfu_send_reply(g_ctx.conn_fd, VFU_REGION_READ, reply_buf, reply_size);
}

/**
 * @brief 处理 VFU_REGION_WRITE
 * @details 向指定 region 写入数据：
 *          - region 0: PCI 配置空间
 *          - region 1: BAR0（NVMe 寄存器/门铃/MSI-X）
 * @param payload vfu_region_rw_t（含 region_index, offset, count, data）
 */
static void handle_region_write(const uint8_t *payload)
{
    const vfu_region_rw_t *req = (const vfu_region_rw_t *)payload;
    uint32_t region_index = vfu_le32_to_cpu(req->region);
    uint64_t offset = vfu_le64_to_cpu(req->offset);
    uint32_t count = vfu_le32_to_cpu(req->count);

    if (region_index == VFIO_REGION_PCI_CONFIG) {
        pci_config_write((uint32_t)offset, count, req->data);
    } else if (region_index == VFIO_REGION_BAR0) {
        bar0_write(offset, count, req->data);
    } else {
        /* 其他 region：忽略写入 */
        LOG_INFO("vfio-user: REGION_WRITE region=%u offset=%llu count=%u (ignored)",
                 region_index, (unsigned long long)offset, count);
    }

    vfu_send_reply(g_ctx.conn_fd, VFU_REGION_WRITE, NULL, 0);
}

/* ============================================================
 *  vfio-user 消息分发
 * ============================================================ */

/**
 * @brief 分发并处理一条 vfio-user 消息
 * @param hdr     消息头部
 * @param payload payload 数据
 * @param fds     伴随的文件描述符数组
 * @param num_fds 文件描述符数量
 */
static void dispatch_message(const vfu_msg_hdr_t *hdr,
                             const uint8_t *payload,
                             const int *fds, int num_fds)
{
    int fd = (num_fds > 0) ? fds[0] : -1;

    g_current_req_id = hdr->id;
    g_current_command = hdr->command;
    switch (hdr->command) {
    case VFU_VERSION:
        handle_get_api_version();
        break;
    case VFU_DEVICE_RESET:
        handle_set_reset();
        break;
    case VFU_DMA_MAP:
        handle_dma_map(payload, fd);
        break;
    case VFU_DMA_UNMAP:
        handle_dma_unmap(payload);
        break;
    case VFU_DEVICE_GET_INFO:
        handle_device_get_info();
        break;
    case VFU_DEVICE_GET_REGION_INFO:
        handle_device_get_region_info(payload);
        break;
    case VFU_DEVICE_GET_REGION_IO_FDS:
        /* 不支持 mmap，返回 count=0（无 FD），QEMU 将通过 REGION_READ/WRITE 访问 */
        {
            struct { uint32_t argsz; uint32_t flags; uint32_t index; uint32_t count; } reply = {0};
            reply.argsz = sizeof(reply);
            reply.count = 0;
            vfu_send_reply(g_ctx.conn_fd, VFU_DEVICE_GET_REGION_IO_FDS, &reply, sizeof(reply));
        }
        break;
    case VFU_DEVICE_GET_IRQ_INFO:
        handle_device_get_irq_info(payload);
        break;
    case VFU_DEVICE_SET_IRQS:
        handle_device_set_irqs(payload, fd);
        break;
    case VFU_REGION_READ:
        handle_region_read(payload);
        break;
    case VFU_REGION_WRITE:
        handle_region_write(payload);
        break;
    case VFU_DMA_READ:
    case VFU_DMA_WRITE:
        /* 后端主动访问 guest 内存时使用，当前通过 mmap 直接访问，
         * 无需处理这两个消息 */
        vfu_send_reply(g_ctx.conn_fd, hdr->command, NULL, 0);
        break;
    default:
        LOG_WARN("vfio-user: 未处理的消息 ID=%u", hdr->command);
        vfu_send_error(g_ctx.conn_fd, hdr->command, EINVAL);
        break;
    }
}

/**
 * @brief 处理连接 socket 上的 vfio-user 消息
 * @details 非阻塞读取消息，分发到对应处理函数。连接断开时
 *          清理资源并重置状态，支持重新 accept。
 * @return RET_OK 成功，RET_ERR_INTERNAL 连接断开或错误
 */
static ret_code_t handle_connection_messages(void)
{
    vfu_msg_hdr_t hdr;
    uint8_t payload[VFIO_USER_MAX_PAYLOAD_SIZE];
    int fds[VFIO_USER_MAX_DMA_REGIONS];
    int num_fds = 0;
    ssize_t payload_len = 0;
    uint32_t i = 0;

    memset(fds, -1, sizeof(fds));
    payload_len = vfu_recv_msg(g_ctx.conn_fd, &hdr, payload, fds,
                               VFIO_USER_MAX_DMA_REGIONS);

    if (payload_len < 0) {
        /* 连接断开或错误 */
        LOG_INFO("vfio-user: 连接断开, errno=%d", errno);
        /* 关闭未使用的 FD */
        for (i = 0; i < VFIO_USER_MAX_DMA_REGIONS; i++) {
            if (fds[i] >= 0) close(fds[i]);
        }
        return RET_ERR_INTERNAL;
    }

    /* 统计有效 FD 数 */
    num_fds = 0;
    for (i = 0; i < VFIO_USER_MAX_DMA_REGIONS; i++) {
        if (fds[i] >= 0) num_fds++;
    }

    dispatch_message(&hdr, payload, fds, num_fds);

    return RET_OK;
}

/* ============================================================
 *  连接清理
 * ============================================================ */

/**
 * @brief 关闭当前连接并清理资源
 * @details 关闭连接 socket，解除 DMA 映射，关闭 MSI-X eventfd，
 *          重置上下文状态，以便接受新连接。
 */
static void close_connection(void)
{
    uint32_t i = 0;

    if (g_ctx.conn_fd >= 0) {
        close(g_ctx.conn_fd);
        g_ctx.conn_fd = -1;
    }

    /* 解除 DMA 映射 */
    for (i = 0; i < g_ctx.dma_region_count; i++) {
        if (g_ctx.dma_mappings[i] != NULL) {
            munmap(g_ctx.dma_mappings[i], (size_t)g_ctx.dma_regions[i].size);
            g_ctx.dma_mappings[i] = NULL;
        }
        if (g_ctx.dma_fds[i] >= 0) {
            close(g_ctx.dma_fds[i]);
            g_ctx.dma_fds[i] = -1;
        }
    }
    g_ctx.dma_region_count = 0;

    /* 关闭 MSI-X eventfd */
    if (g_ctx.msix_evtfd >= 0) {
        close(g_ctx.msix_evtfd);
        g_ctx.msix_evtfd = -1;
    }
    g_ctx.msix_enabled = false;

    /* 重置队列状态 */
    reset_queue_state();

    g_ctx.connected = false;

    /* 重置寄存器和配置空间 */
    init_nvme_regs();
    init_pci_config();

    LOG_INFO("vfio-user: 连接已清理，等待新连接");
}

/* ============================================================
 *  对外接口实现
 * ============================================================ */

ret_code_t vfio_user_nvme_init(const vfio_user_nvme_config_t *config)
{
    struct sockaddr_un addr;
    uint32_t i = 0;

    if (config == NULL) {
        LOG_ERROR("vfio-user: 配置为空");
        return RET_ERR_PARAM;
    }

    memset(&g_ctx, 0, sizeof(g_ctx));
    memcpy(&g_ctx.config, config, sizeof(vfio_user_nvme_config_t));

    if (g_ctx.config.max_queues == 0) {
        g_ctx.config.max_queues = 2;  /* 默认 Admin + 1 I/O */
    }
    if (g_ctx.config.socket_path[0] == '\0') {
        strncpy(g_ctx.config.socket_path, VFIO_USER_NVME_DEFAULT_SOCKET,
                sizeof(g_ctx.config.socket_path) - 1);
    }

    /* 初始化 DMA 区域 FD */
    for (i = 0; i < VFIO_USER_MAX_DMA_REGIONS; i++) {
        g_ctx.dma_fds[i] = -1;
        g_ctx.dma_mappings[i] = NULL;
    }

    g_ctx.socket_fd = -1;
    g_ctx.conn_fd = -1;
    g_ctx.msix_evtfd = -1;
    g_ctx.msix_enabled = false;

    /* 初始化队列状态 */
    reset_queue_state();

    /* 初始化 PCIe 配置空间 */
    init_pci_config();

    /* 初始化 NVMe 寄存器镜像 */
    init_nvme_regs();

    /* 删除已存在的 socket 文件 */
    unlink(g_ctx.config.socket_path);

    /* 创建 Unix domain socket */
    g_ctx.socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_ctx.socket_fd < 0) {
        LOG_ERROR("vfio-user: 创建 socket 失败, errno=%d", errno);
        return RET_ERR_INTERNAL;
    }

    /* 设置非阻塞 */
    fcntl(g_ctx.socket_fd, F_SETFL, O_NONBLOCK);

    /* 绑定地址 */
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_ctx.config.socket_path, sizeof(addr.sun_path) - 1);

    if (bind(g_ctx.socket_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("vfio-user: 绑定 socket 失败, path=%s, errno=%d",
                  g_ctx.config.socket_path, errno);
        close(g_ctx.socket_fd);
        g_ctx.socket_fd = -1;
        return RET_ERR_INTERNAL;
    }

    /* 监听 */
    if (listen(g_ctx.socket_fd, 1) < 0) {
        LOG_ERROR("vfio-user: 监听 socket 失败, errno=%d", errno);
        close(g_ctx.socket_fd);
        g_ctx.socket_fd = -1;
        return RET_ERR_INTERNAL;
    }

    g_ctx.running = true;

    LOG_INFO("vfio-user NVMe 后端初始化完成, socket=%s, max_queues=%u",
             g_ctx.config.socket_path, g_ctx.config.max_queues);

    return RET_OK;
}

ret_code_t vfio_user_nvme_deinit(void)
{
    g_ctx.running = false;

    close_connection();

    if (g_ctx.socket_fd >= 0) {
        close(g_ctx.socket_fd);
        g_ctx.socket_fd = -1;
    }

    unlink(g_ctx.config.socket_path);

    LOG_INFO("vfio-user NVMe 后端已关闭");
    return RET_OK;
}

void vfio_user_nvme_process(void)
{
    fd_set readfds;
    int maxfd = 0;
    struct timeval tv;

    if (!g_ctx.running || g_ctx.socket_fd < 0) {
        return;
    }

    FD_ZERO(&readfds);
    FD_SET(g_ctx.socket_fd, &readfds);
    maxfd = g_ctx.socket_fd;

    if (g_ctx.connected && g_ctx.conn_fd >= 0) {
        FD_SET(g_ctx.conn_fd, &readfds);
        if (g_ctx.conn_fd > maxfd) {
            maxfd = g_ctx.conn_fd;
        }
    }

    /* 非阻塞 select（立即返回） */
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    if (select(maxfd + 1, &readfds, NULL, NULL, &tv) < 0) {
        if (errno == EINTR) return;
        return;
    }

    /* 1. 接受新连接 */
    if (FD_ISSET(g_ctx.socket_fd, &readfds)) {
        int client_fd = accept(g_ctx.socket_fd, NULL, NULL);
        if (client_fd >= 0) {
            if (g_ctx.connected) {
                /* 已有连接，拒绝新连接 */
                close(client_fd);
                LOG_WARN("vfio-user: 已有连接，拒绝新连接");
            } else {
                fcntl(client_fd, F_SETFL, O_NONBLOCK);
                g_ctx.conn_fd = client_fd;
                g_ctx.connected = true;
                LOG_INFO("vfio-user: QEMU 已连接");
            }
        }
    }

    /* 2. 处理 vfio-user 协议消息 */
    if (g_ctx.connected && g_ctx.conn_fd >= 0 &&
        FD_ISSET(g_ctx.conn_fd, &readfds)) {
        if (handle_connection_messages() != RET_OK) {
            close_connection();
        }
    }
}
