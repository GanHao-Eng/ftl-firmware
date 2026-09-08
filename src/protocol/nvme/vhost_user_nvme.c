/**
 * @file vhost_user_nvme.c
 * @brief vhost-user NVMe 后端完整实现
 * @details 实现 QEMU vhost-user 协议后端，使 ftl-firmware 可作为
 *          QEMU 的 NVMe 设备后端运行。QEMU 通过
 *          -device vhost-user-nvme,chardev=... 连接本后端。
 *
 *          实现内容：
 *          1. Unix domain socket 服务器（bind/listen/accept）
 *          2. vhost-user 消息接收/发送（支持 SCM_RIGHTS 传递 FD）
 *          3. 完整协议握手：GET_FEATURES → SET_FEATURES →
 *             GET_PROTOCOL_FEATURES → SET_PROTOCOL_FEATURES →
 *             SET_OWNER → SET_MEM_TABLE → SET_VRING_* → SET_VRING_ENABLE
 *          4. NVMe 寄存器镜像（BAR0 4KB），支持 GET_CONFIG/SET_CONFIG
 *          5. vring 命令处理：从 available ring 取 NVMe 命令，
 *             调用 nvme_ctrl_process_admin_cmd/io_cmd，PRP 数据传输，
 *             完成写入 used ring，call eventfd 发中断
 *          6. PRP 解析：支持跨页数据传输，GPA→HVA 地址转换
 *
 *          vring 缓冲区布局假设（与 QEMU vhost-user-nvme 对接）：
 *          available ring 中每个描述符指向一块缓冲区，布局为：
 *            [nvme_command_t 64字节][nvme_completion_t 16字节]
 *          后端读取前64字节作为命令，处理后将完成写入后16字节，
 *          然后将描述符放入 used ring（len=16）。
 *          数据传输通过命令中的 PRP1/PRP2 指针访问共享内存。
 *
 * @note 所有多字节字段按小端处理（NVMe 和 vhost-user 均为小端，
 *       x86 平台直接使用无需转换）。
 */

#define _GNU_SOURCE

#include "protocol/vhost_user_nvme.h"
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
 *  内部数据
 * ============================================================ */

/** @brief 全局后端上下文 */
static vhost_user_nvme_ctx_t g_ctx;

/** @brief 预分配 I/O 读缓冲区（避免频繁 malloc） */
static uint8_t g_io_read_buf[VHOST_USER_NVME_MAX_IO_SIZE];

/** @brief 预分配 I/O 写缓冲区 */
static uint8_t g_io_write_buf[VHOST_USER_NVME_MAX_IO_SIZE];

/** @brief 静态零缓冲区（用于 Write Zeroes） */
static uint8_t g_zero_buf[4096];

/* ============================================================
 *  小端字节序辅助（x86 为小端，直接返回）
 * ============================================================ */

static uint16_t vu_le16_to_cpu(uint16_t val) { return val; }
static uint16_t vu_cpu_to_le16(uint16_t val) { return val; }
static uint32_t vu_le32_to_cpu(uint32_t val) { return val; }
static uint32_t vu_cpu_to_le32(uint32_t val) { return val; }
static uint64_t vu_le64_to_cpu(uint64_t val) { return val; }

/* ============================================================
 *  GPA → HVA 地址转换
 * ============================================================ */

/**
 * @brief 将 Guest 物理地址(GPA)转换为后端进程虚拟地址(HVA)
 * @details 遍历 SET_MEM_TABLE 建立的内存区域映射，找到包含目标 GPA
 *          的区域，计算在 mmap 区域内的偏移并返回虚拟地址。
 * @param gpa Guest 物理地址
 * @return 虚拟地址指针，未找到返回 NULL
 */
static void *gpa_to_hva(uint64_t gpa)
{
    uint32_t i = 0;

    for (i = 0; i < g_ctx.mem_region_count; i++) {
        uint64_t base = g_ctx.mem_regions[i].guest_phys_addr;
        uint64_t size = g_ctx.mem_regions[i].memory_size;

        if (gpa >= base && gpa < base + size) {
            uint64_t offset = gpa - base;
            return (uint8_t *)g_ctx.mem_mappings[i] + offset;
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
 *
 *          PRP 规则：
 *          - PRP1：第一个数据页的地址（含页内偏移）
 *          - PRP2：若页对齐则指向下一个 PRP 条目列表；否则为
 *            第二个数据页地址（仅当数据不超过2页时使用）
 *
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
    uint64_t page_size = 4096;
    uint64_t current_addr = prp1;

    while (offset < len) {
        uint64_t page_offset = current_addr & (page_size - 1);
        uint32_t chunk = (uint32_t)(page_size - page_offset);
        void *hva = NULL;

        if (chunk > len - offset) {
            chunk = len - offset;
        }

        hva = gpa_to_hva(current_addr & ~(page_size - 1));
        if (hva == NULL) {
            LOG_ERROR("vhost-user: PRP read GPA 转换失败, addr=0x%llX",
                      (unsigned long long)current_addr);
            return RET_ERR_PARAM;
        }

        memcpy(buf + offset, (uint8_t *)hva + page_offset, chunk);
        offset += chunk;

        if (offset >= len) {
            break;
        }

        /* 下一页：若 PRP2 是页对齐的 PRP 列表指针，从列表取下一个 */
        if ((prp2 & (page_size - 1)) == 0 && offset > (uint32_t)(page_size - (prp1 & (page_size - 1)))) {
            /* PRP2 指向 PRP 条目列表 */
            uint64_t *prp_list = (uint64_t *)gpa_to_hva(prp2);
            uint32_t list_idx = 0;

            if (prp_list == NULL) {
                LOG_ERROR("vhost-user: PRP 列表地址转换失败, prp2=0x%llX",
                          (unsigned long long)prp2);
                return RET_ERR_PARAM;
            }

            /* 计算当前在列表中的索引：已传输的完整页数 - 1 */
            list_idx = (offset - (uint32_t)(page_size - (prp1 & (page_size - 1)))) / (uint32_t)page_size;
            current_addr = vu_le64_to_cpu(prp_list[list_idx]);
        } else if (offset == (uint32_t)(page_size - (prp1 & (page_size - 1)))) {
            /* 第一页结束，PRP2 直接是第二页地址 */
            current_addr = prp2;
        } else {
            /* PRP2 是页内偏移（仅2页场景），第二页后无更多数据 */
            break;
        }
    }

    return RET_OK;
}

/**
 * @brief 将本地缓冲区数据写入 PRP 列表指向的共享内存
 * @details 解析 PRP1/PRP2，遍历所有物理页，将本地数据拷贝到
 *          共享内存中。支持跨页传输。
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
    uint64_t page_size = 4096;
    uint64_t current_addr = prp1;

    while (offset < len) {
        uint64_t page_offset = current_addr & (page_size - 1);
        uint32_t chunk = (uint32_t)(page_size - page_offset);
        void *hva = NULL;

        if (chunk > len - offset) {
            chunk = len - offset;
        }

        hva = gpa_to_hva(current_addr & ~(page_size - 1));
        if (hva == NULL) {
            LOG_ERROR("vhost-user: PRP write GPA 转换失败, addr=0x%llX",
                      (unsigned long long)current_addr);
            return RET_ERR_PARAM;
        }

        memcpy((uint8_t *)hva + page_offset, buf + offset, chunk);
        offset += chunk;

        if (offset >= len) {
            break;
        }

        if ((prp2 & (page_size - 1)) == 0 && offset > (uint32_t)(page_size - (prp1 & (page_size - 1)))) {
            uint64_t *prp_list = (uint64_t *)gpa_to_hva(prp2);
            uint32_t list_idx = 0;

            if (prp_list == NULL) {
                LOG_ERROR("vhost-user: PRP 列表地址转换失败, prp2=0x%llX",
                          (unsigned long long)prp2);
                return RET_ERR_PARAM;
            }

            list_idx = (offset - (uint32_t)(page_size - (prp1 & (page_size - 1)))) / (uint32_t)page_size;
            current_addr = vu_le64_to_cpu(prp_list[list_idx]);
        } else if (offset == (uint32_t)(page_size - (prp1 & (page_size - 1)))) {
            current_addr = prp2;
        } else {
            break;
        }
    }

    return RET_OK;
}

/* ============================================================
 *  vhost-user 消息收发（支持 SCM_RIGHTS）
 * ============================================================ */

/**
 * @brief 接收 vhost-user 消息（含可能的文件描述符）
 * @details 使用 recvmsg 接收消息头部和 payload，通过 SCM_RIGHTS
 *          辅助数据接收文件描述符（如 SET_MEM_TABLE 的内存区域 FD、
 *          SET_VRING_KICK/CALL 的 eventfd）。
 * @param fd      连接 socket
 * @param hdr     输出：消息头部
 * @param payload 输出：payload 缓冲区
 * @param fds     输出：接收到的文件描述符数组
 * @param max_fds fds 数组容量
 * @return 接收的 payload 字节数，-1 失败
 */
static ssize_t vu_recv_msg(int fd, vhost_user_msg_hdr_t *hdr,
                           void *payload, int *fds, int max_fds)
{
    struct msghdr msg;
    struct iovec iov[2];
    char cmsgbuf[CMSG_SPACE(sizeof(int) * VHOST_USER_MAX_MEM_REGIONS)];
    struct cmsghdr *cmsg = NULL;
    ssize_t n = 0;
    int *fd_ptr = NULL;
    int fd_count = 0;
    int i = 0;

    memset(&msg, 0, sizeof(msg));
    memset(cmsgbuf, 0, sizeof(cmsgbuf));

    /* 先收头部 */
    iov[0].iov_base = hdr;
    iov[0].iov_len = sizeof(vhost_user_msg_hdr_t);
    /* 再收 payload */
    iov[1].iov_base = payload;
    iov[1].iov_len = VHOST_USER_MAX_PAYLOAD_SIZE;

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
            int num = (cmsg->cmsg_len - CMSG_LEN(0)) / (int)sizeof(int);
            for (i = 0; i < num && fd_count < max_fds; i++) {
                fds[fd_count++] = fd_ptr[i];
            }
        }
    }

    return n - (ssize_t)sizeof(vhost_user_msg_hdr_t);
}

/**
 * @brief 发送 vhost-user 消息（含可能的文件描述符）
 * @param fd      连接 socket
 * @param request 消息 ID
 * @param flags   消息标志
 * @param payload payload 数据
 * @param size    payload 字节数
 * @param fds     要传递的文件描述符数组（可为 NULL）
 * @param num_fds 文件描述符数量
 * @return 发送的总字节数，-1 失败
 */
static ssize_t vu_send_msg(int fd, uint32_t request, uint32_t flags,
                           const void *payload, uint32_t size,
                           const int *fds, int num_fds)
{
    vhost_user_msg_hdr_t hdr;
    struct msghdr msg;
    struct iovec iov[2];
    char cmsgbuf[CMSG_SPACE(sizeof(int) * VHOST_USER_MAX_MEM_REGIONS)];
    struct cmsghdr *cmsg = NULL;
    ssize_t n = 0;

    memset(&hdr, 0, sizeof(hdr));
    hdr.request = request;
    hdr.flags = flags;
    hdr.size = size;

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
    return n;
}

/**
 * @brief 发送 vhost-user 回复消息（带 REPLY 标志）
 * @param fd      连接 socket
 * @param request 回复的消息 ID（与请求相同）
 * @param payload 回复 payload
 * @param size    payload 字节数
 * @return 发送字节数，-1 失败
 */
static ssize_t vu_send_reply(int fd, uint32_t request,
                             const void *payload, uint32_t size)
{
    return vu_send_msg(fd, request, VHOST_USER_FLAG_REPLY, payload, size, NULL, 0);
}

/* ============================================================
 *  NVMe 寄存器初始化
 * ============================================================ */

/**
 * @brief 初始化 NVMe 寄存器镜像（BAR0）
 * @details 设置 CAP、VS 等只读寄存器的初始值，与 nvme_controller
 *          保持一致。CC.EN=0, CSTS.RDY=0。
 */
static void init_nvme_regs(void)
{
    uint32_t *regs32 = (uint32_t *)g_ctx.regs;
    uint64_t *regs64 = (uint64_t *)g_ctx.regs;

    memset(g_ctx.regs, 0, VHOST_USER_NVME_REG_SIZE);

    /* CAP (0x00): Controller Capabilities
     * MQES=0x3FF(1023队列), DSTRD=0(4字节门铃), TO=0x0F, NSSRS=1 */
    regs64[0] = 0x0000000F000001FFULL;

    /* VS (0x08): NVMe 1.4 */
    regs32[2] = NVME_VERSION;

    /* CC (0x14): 0 (控制器未使能) */
    /* CSTS (0x1C): 0 (未就绪) */
}

/**
 * @brief 处理 NVMe 寄存器写操作
 * @details 解析 SET_CONFIG 的偏移和值，更新寄存器镜像。
 *          对 CC 寄存器的 EN 位变化做出响应：EN=1 时设置 CSTS.RDY=1。
 *          对门铃寄存器更新 SQ tail / CQ head（触发命令处理）。
 * @param offset BAR0 内偏移
 * @param size   写入长度（1/2/4字节）
 * @param value  写入值
 */
static void handle_reg_write(uint32_t offset, uint32_t size, uint32_t value)
{
    nvme_ctrl_regs_t *ctrl_regs = nvme_ctrl_get_regs();

    /* 写入寄存器镜像 */
    if (offset + size <= VHOST_USER_NVME_REG_SIZE) {
        memcpy(g_ctx.regs + offset, &value, size);
    }

    /* 同步到 nvme_controller 寄存器并处理特殊逻辑 */
    switch (offset) {
    case NVME_REG_CC: {
        /* CC.EN (bit0) 变化 */
        if (ctrl_regs != NULL) {
            ctrl_regs->cc = value;
            if (value & 0x01U) {
                /* 使能控制器，设置 CSTS.RDY */
                ctrl_regs->csts |= 0x01U;
                g_ctx.regs[NVME_REG_CSTS] = 0x01U;
                LOG_INFO("vhost-user: CC.EN=1, CSTS.RDY=1");
            } else {
                ctrl_regs->csts &= ~0x01U;
                g_ctx.regs[NVME_REG_CSTS] = 0x00U;
                LOG_INFO("vhost-user: CC.EN=0, CSTS.RDY=0");
            }
        }
        break;
    }
    case NVME_REG_AQA:
        if (ctrl_regs != NULL) ctrl_regs->aqa = value;
        break;
    case NVME_REG_ASQ:
        if (ctrl_regs != NULL) {
            /* ASQ 是64位，分高低32位写 */
            if (size == 4) {
                /* 假设写低32位（QEMU 通常按32位写） */
                ctrl_regs->asq = (ctrl_regs->asq & ~0xFFFFFFFFULL) | value;
            }
        }
        break;
    case NVME_REG_ASQ + 4:
        if (ctrl_regs != NULL) {
            ctrl_regs->asq = (ctrl_regs->asq & 0xFFFFFFFFULL) | ((uint64_t)value << 32);
        }
        break;
    case NVME_REG_ACQ:
        if (ctrl_regs != NULL) {
            if (size == 4) {
                ctrl_regs->acq = (ctrl_regs->acq & ~0xFFFFFFFFULL) | value;
            }
        }
        break;
    case NVME_REG_ACQ + 4:
        if (ctrl_regs != NULL) {
            ctrl_regs->acq = (ctrl_regs->acq & 0xFFFFFFFFULL) | ((uint64_t)value << 32);
        }
        break;
    default:
        /* 门铃寄存器区域 0x1000+ */
        if (offset >= 0x1000U) {
            uint32_t doorbell_offset = offset - 0x1000U;
            uint16_t qid = (uint16_t)(doorbell_offset / 8U);
            bool is_sq = ((doorbell_offset % 8U) == 0U);

            if (is_sq) {
                /* SQ tail doorbell */
                nvme_ctrl_sq_doorbell(qid, value);
            } else {
                /* CQ head doorbell */
                nvme_ctrl_cq_doorbell(qid, value);
            }
        }
        break;
    }
}

/* ============================================================
 *  vhost-user 消息处理函数
 * ============================================================ */

/**
 * @brief 处理 VHOST_USER_GET_FEATURES
 * @details 返回后端支持的 vhost 特性位。必须包含
 *          VHOST_USER_F_PROTOCOL_FEATURES 以支持协议特性协商。
 */
static void handle_get_features(void)
{
    uint64_t features = VHOST_USER_F_PROTOCOL_FEATURES;

    LOG_INFO("vhost-user: GET_FEATURES, features=0x%llX",
             (unsigned long long)features);

    vu_send_reply(g_ctx.conn_fd, VHOST_USER_GET_FEATURES,
                  &features, sizeof(features));
}

/**
 * @brief 处理 VHOST_USER_SET_FEATURES
 * @param payload 包含协商后的特性位（8字节）
 */
static void handle_set_features(const uint8_t *payload)
{
    uint64_t features = 0;

    memcpy(&features, payload, sizeof(features));
    g_ctx.features = features;

    LOG_INFO("vhost-user: SET_FEATURES, features=0x%llX",
             (unsigned long long)features);
}

/**
 * @brief 处理 VHOST_USER_SET_OWNER
 * @details QEMU 声明自己为 vhost 后端的所有者。
 */
static void handle_set_owner(void)
{
    g_ctx.owner_set = true;
    LOG_INFO("vhost-user: SET_OWNER");
}

/**
 * @brief 处理 VHOST_USER_RESET_OWNER
 * @details QEMU 释放所有权，后端应重置状态。
 */
static void handle_reset_owner(void)
{
    g_ctx.owner_set = false;
    LOG_INFO("vhost-user: RESET_OWNER");
}

/**
 * @brief 处理 VHOST_USER_SET_MEM_TABLE
 * @details 接收内存区域描述符和对应的文件描述符，对每个区域
 *          进行 mmap 建立 GPA→HVA 映射。
 * @param payload VhostUserMemory 结构
 * @param fds     内存区域文件描述符数组
 * @param num_fds 文件描述符数量
 */
static void handle_set_mem_table(const uint8_t *payload,
                                 const int *fds, int num_fds)
{
    const vhost_user_memory_t *mem = (const vhost_user_memory_t *)payload;
    uint32_t nregions = vu_le32_to_cpu(mem->nregions);
    uint32_t i = 0;

    LOG_INFO("vhost-user: SET_MEM_TABLE, nregions=%u, num_fds=%d",
             nregions, num_fds);

    /* 先解除旧映射 */
    for (i = 0; i < g_ctx.mem_region_count; i++) {
        if (g_ctx.mem_mappings[i] != NULL) {
            munmap(g_ctx.mem_mappings[i], g_ctx.mem_regions[i].memory_size);
            g_ctx.mem_mappings[i] = NULL;
        }
        if (g_ctx.mem_fds[i] >= 0) {
            close(g_ctx.mem_fds[i]);
            g_ctx.mem_fds[i] = -1;
        }
    }
    g_ctx.mem_region_count = 0;

    /* 映射新区域 */
    for (i = 0; i < nregions && i < VHOST_USER_MAX_MEM_REGIONS; i++) {
        void *addr = NULL;
        int fd = (i < (uint32_t)num_fds) ? fds[i] : -1;

        g_ctx.mem_regions[i].guest_phys_addr =
            vu_le64_to_cpu(mem->regions[i].guest_phys_addr);
        g_ctx.mem_regions[i].memory_size =
            vu_le64_to_cpu(mem->regions[i].memory_size);
        g_ctx.mem_regions[i].mmap_offset =
            vu_le64_to_cpu(mem->regions[i].mmap_offset);

        if (fd < 0) {
            LOG_ERROR("vhost-user: 内存区域 %u 缺少 FD", i);
            continue;
        }

        addr = mmap(NULL, g_ctx.mem_regions[i].memory_size,
                    PROT_READ | PROT_WRITE, MAP_SHARED,
                    fd, (off_t)g_ctx.mem_regions[i].mmap_offset);

        if (addr == MAP_FAILED) {
            LOG_ERROR("vhost-user: mmap 区域 %u 失败, errno=%d", i, errno);
            close(fd);
            continue;
        }

        g_ctx.mem_mappings[i] = addr;
        g_ctx.mem_fds[i] = fd;
        g_ctx.mem_region_count++;

        LOG_INFO("vhost-user: 映射区域 %u: GPA=0x%llX size=%llu -> HVA=%p",
                 i,
                 (unsigned long long)g_ctx.mem_regions[i].guest_phys_addr,
                 (unsigned long long)g_ctx.mem_regions[i].memory_size,
                 addr);
    }
}

/**
 * @brief 处理 VHOST_USER_SET_VRING_NUM
 * @param payload VhostUserVringState（index + num）
 */
static void handle_set_vring_num(const uint8_t *payload)
{
    const vhost_user_vring_state_t *vs = (const vhost_user_vring_state_t *)payload;
    uint32_t index = vu_le32_to_cpu(vs->index);
    uint32_t num = vu_le32_to_cpu(vs->num);

    if (index < VHOST_USER_NVME_MAX_QUEUES) {
        g_ctx.vrings[index].num = num;
        LOG_INFO("vhost-user: SET_VRING_NUM[%u]=%u", index, num);
    }
}

/**
 * @brief 处理 VHOST_USER_SET_VRING_ADDR
 * @param payload VhostUserVringAddr（index + 三环地址）
 */
static void handle_set_vring_addr(const uint8_t *payload)
{
    const vhost_user_vring_addr_t *va = (const vhost_user_vring_addr_t *)payload;
    uint32_t index = vu_le32_to_cpu(va->index);

    if (index < VHOST_USER_NVME_MAX_QUEUES) {
        g_ctx.vrings[index].desc_addr = vu_le64_to_cpu(va->desc_addr);
        g_ctx.vrings[index].avail_addr = vu_le64_to_cpu(va->avail_addr);
        g_ctx.vrings[index].used_addr = vu_le64_to_cpu(va->used_addr);
        LOG_INFO("vhost-user: SET_VRING_ADDR[%u]: desc=0x%llX avail=0x%llX used=0x%llX",
                 index,
                 (unsigned long long)g_ctx.vrings[index].desc_addr,
                 (unsigned long long)g_ctx.vrings[index].avail_addr,
                 (unsigned long long)g_ctx.vrings[index].used_addr);
    }
}

/**
 * @brief 处理 VHOST_USER_SET_VRING_BASE
 * @param payload VhostUserVringState（index + base）
 */
static void handle_set_vring_base(const uint8_t *payload)
{
    const vhost_user_vring_state_t *vs = (const vhost_user_vring_state_t *)payload;
    uint32_t index = vu_le32_to_cpu(vs->index);
    uint32_t base = vu_le32_to_cpu(vs->num);

    if (index < VHOST_USER_NVME_MAX_QUEUES) {
        g_ctx.vrings[index].base = base;
        g_ctx.vrings[index].last_avail_idx = (uint16_t)base;
        LOG_INFO("vhost-user: SET_VRING_BASE[%u]=%u", index, base);
    }
}

/**
 * @brief 处理 VHOST_USER_GET_VRING_BASE
 * @param payload VhostUserVringState（index）
 */
static void handle_get_vring_base(const uint8_t *payload)
{
    const vhost_user_vring_state_t *vs = (const vhost_user_vring_state_t *)payload;
    uint32_t index = vu_le32_to_cpu(vs->index);
    vhost_user_vring_state_t reply;

    memset(&reply, 0, sizeof(reply));
    reply.index = vu_cpu_to_le32(index);
    if (index < VHOST_USER_NVME_MAX_QUEUES) {
        reply.num = vu_cpu_to_le32(g_ctx.vrings[index].base);
    }

    LOG_INFO("vhost-user: GET_VRING_BASE[%u]=%u", index, reply.num);
    vu_send_reply(g_ctx.conn_fd, VHOST_USER_GET_VRING_BASE,
                  &reply, sizeof(reply));
}

/**
 * @brief 处理 VHOST_USER_SET_VRING_KICK
 * @details 接收 kick eventfd，QEMU 通过写该 eventfd 通知后端
 *          有新命令放入 available ring。
 * @param payload VhostUserVringState（index）
 * @param fd      kick eventfd（通过 SCM_RIGHTS 传递）
 */
static void handle_set_vring_kick(const uint8_t *payload, int fd)
{
    const vhost_user_vring_state_t *vs = (const vhost_user_vring_state_t *)payload;
    uint32_t index = vu_le32_to_cpu(vs->index);

    if (index < VHOST_USER_NVME_MAX_QUEUES) {
        if (g_ctx.vrings[index].kick_fd >= 0) {
            close(g_ctx.vrings[index].kick_fd);
        }
        g_ctx.vrings[index].kick_fd = fd;
        LOG_INFO("vhost-user: SET_VRING_KICK[%u], fd=%d", index, fd);
    } else if (fd >= 0) {
        close(fd);
    }
}

/**
 * @brief 处理 VHOST_USER_SET_VRING_CALL
 * @details 接收 call eventfd，后端通过写该 eventfd 通知 QEMU
 *          有命令完成（中断）。
 * @param payload VhostUserVringState（index）
 * @param fd      call eventfd（通过 SCM_RIGHTS 传递）
 */
static void handle_set_vring_call(const uint8_t *payload, int fd)
{
    const vhost_user_vring_state_t *vs = (const vhost_user_vring_state_t *)payload;
    uint32_t index = vu_le32_to_cpu(vs->index);

    if (index < VHOST_USER_NVME_MAX_QUEUES) {
        if (g_ctx.vrings[index].call_fd >= 0) {
            close(g_ctx.vrings[index].call_fd);
        }
        g_ctx.vrings[index].call_fd = fd;
        LOG_INFO("vhost-user: SET_VRING_CALL[%u], fd=%d", index, fd);
    } else if (fd >= 0) {
        close(fd);
    }
}

/**
 * @brief 处理 VHOST_USER_SET_VRING_ENABLE
 * @param payload VhostUserVringState（index + enable）
 */
static void handle_set_vring_enable(const uint8_t *payload)
{
    const vhost_user_vring_state_t *vs = (const vhost_user_vring_state_t *)payload;
    uint32_t index = vu_le32_to_cpu(vs->index);
    uint32_t enable = vu_le32_to_cpu(vs->num);

    if (index < VHOST_USER_NVME_MAX_QUEUES) {
        g_ctx.vrings[index].enabled = (enable != 0);
        LOG_INFO("vhost-user: SET_VRING_ENABLE[%u]=%u", index, enable);
    }
}

/**
 * @brief 处理 VHOST_USER_GET_PROTOCOL_FEATURES
 * @details 返回支持的协议特性位：CONFIG（寄存器读写）、
 *          REPLY_ACK、STATUS。
 */
static void handle_get_protocol_features(void)
{
    uint64_t features = VHOST_USER_PROTOCOL_F_CONFIG |
                        VHOST_USER_PROTOCOL_F_REPLY_ACK |
                        VHOST_USER_PROTOCOL_F_STATUS;

    LOG_INFO("vhost-user: GET_PROTOCOL_FEATURES=0x%llX",
             (unsigned long long)features);

    vu_send_reply(g_ctx.conn_fd, VHOST_USER_GET_PROTOCOL_FEATURES,
                  &features, sizeof(features));
}

/**
 * @brief 处理 VHOST_USER_SET_PROTOCOL_FEATURES
 * @param payload 协商后的协议特性位（8字节）
 */
static void handle_set_protocol_features(const uint8_t *payload)
{
    uint64_t features = 0;

    memcpy(&features, payload, sizeof(features));
    g_ctx.protocol_features = features;

    LOG_INFO("vhost-user: SET_PROTOCOL_FEATURES=0x%llX",
             (unsigned long long)features);
}

/**
 * @brief 处理 VHOST_USER_GET_CONFIG
 * @details 读取 NVMe 寄存器镜像(BAR0)指定偏移和长度的数据。
 * @param payload VhostUserConfig（offset + size）
 */
static void handle_get_config(const uint8_t *payload)
{
    const vhost_user_config_t *cfg = (const vhost_user_config_t *)payload;
    uint32_t offset = vu_le32_to_cpu(cfg->offset);
    uint32_t size = vu_le32_to_cpu(cfg->size);
    uint8_t reply[sizeof(vhost_user_config_t) + VHOST_USER_NVME_REG_SIZE];
    vhost_user_config_t *reply_cfg = (vhost_user_config_t *)reply;

    if (size > VHOST_USER_NVME_REG_SIZE) {
        size = VHOST_USER_NVME_REG_SIZE;
    }
    if (offset + size > VHOST_USER_NVME_REG_SIZE) {
        size = VHOST_USER_NVME_REG_SIZE - offset;
    }

    memset(reply, 0, sizeof(reply));
    reply_cfg->offset = vu_cpu_to_le32(offset);
    reply_cfg->size = vu_cpu_to_le32(size);
    reply_cfg->flags = 0;
    memcpy(reply_cfg->data, g_ctx.regs + offset, size);

    vu_send_reply(g_ctx.conn_fd, VHOST_USER_GET_CONFIG,
                  reply, (uint32_t)sizeof(vhost_user_config_t) + size);
}

/**
 * @brief 处理 VHOST_USER_SET_CONFIG
 * @details 写入 NVMe 寄存器镜像(BAR0)，调用 handle_reg_write
 *          处理 CC.EN、门铃等特殊寄存器。
 * @param payload VhostUserConfig（offset + size + data）
 */
static void handle_set_config(const uint8_t *payload)
{
    const vhost_user_config_t *cfg = (const vhost_user_config_t *)payload;
    uint32_t offset = vu_le32_to_cpu(cfg->offset);
    uint32_t size = vu_le32_to_cpu(cfg->size);
    uint32_t value = 0;

    if (size <= 4) {
        memcpy(&value, cfg->data, size);
    }

    LOG_INFO("vhost-user: SET_CONFIG offset=0x%X size=%u value=0x%X",
             offset, size, value);

    handle_reg_write(offset, size, value);
}

/* ============================================================
 *  vring 命令处理
 * ============================================================ */

/**
 * @brief 处理单个 NVMe 命令
 * @details 从 vring 描述符缓冲区读取 nvme_command_t，根据操作码
 *          分发处理：
 *          - Identify/Get Log Page：调用 nvme_ctrl_fill_* 填充数据，
 *            通过 PRP 写入共享内存
 *          - Read：从 FTL 读取数据，通过 PRP 写入共享内存
 *          - Write：通过 PRP 从共享内存读取数据，写入 FTL
 *          - Write Zeroes：写零到 FTL
 *          - Dataset Mgmt：通过 PRP 读取 range 列表，调用 ftl_trim
 *          - 其他 Admin：nvme_ctrl_process_admin_cmd
 *          - 其他 I/O：nvme_ctrl_process_io_cmd
 *
 *          处理完成后将 nvme_completion_t 写入描述符缓冲区的完成区。
 * @param vring vring 状态
 * @param cmd   NVMe 命令
 * @param cpl   输出：完成队列条目
 * @param qid   队列 ID（0=Admin，1+=I/O）
 */
static void process_nvme_command(vhost_user_vring_t *vring,
                                 const nvme_command_t *cmd,
                                 nvme_completion_t *cpl,
                                 uint16_t qid)
{
    uint16_t cid = vu_le16_to_cpu(cmd->cid);
    uint64_t prp1 = vu_le64_to_cpu(cmd->dptr_prp1);
    uint64_t prp2 = vu_le64_to_cpu(cmd->dptr_prp2);

    (void)vring;

    memset(cpl, 0, sizeof(nvme_completion_t));
    cpl->cid = vu_cpu_to_le16(cid);
    cpl->sqid = vu_cpu_to_le16(qid);

    /* ---- Admin 命令（qid==0）---- */
    if (qid == 0) {
        switch (cmd->opcode) {
        case NVME_ADMIN_IDENTIFY: {
            uint8_t cns = vu_le32_to_cpu(cmd->cdw10) & 0xFF;
            uint8_t id_data[4096];

            memset(id_data, 0, sizeof(id_data));
            if (cns == 0x01) {
                nvme_ctrl_fill_identify_controller(id_data, sizeof(id_data));
            } else if (cns == 0x00) {
                nvme_ctrl_fill_identify_namespace(id_data, sizeof(id_data),
                                                   vu_le32_to_cpu(cmd->nsid));
            } else {
                /* 其他 CNS 返回空 */
            }
            prp_write_data(prp1, prp2, 4096, id_data);
            cpl->status = vu_cpu_to_le16(0x0000);
            break;
        }
        case NVME_ADMIN_GET_LOG_PAGE: {
            uint8_t lid = vu_le32_to_cpu(cmd->cdw10) & 0xFF;
            uint32_t numd = (vu_le32_to_cpu(cmd->cdw10) >> 16) & 0xFFFF;
            uint32_t log_len = (numd + 1) * 4;
            uint8_t log_data[4096];

            memset(log_data, 0, sizeof(log_data));
            if (lid == 0x02) {
                nvme_ctrl_fill_smart_log(log_data, log_len < 512 ? log_len : 512);
            }
            if (log_len > sizeof(log_data)) log_len = sizeof(log_data);
            prp_write_data(prp1, prp2, log_len, log_data);
            cpl->status = vu_cpu_to_le16(0x0000);
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
        uint64_t slba = ((uint64_t)vu_le32_to_cpu(cmd->cdw11) << 32) |
                        vu_le32_to_cpu(cmd->cdw10);
        uint16_t nlb = (vu_le16_to_cpu(cmd->cdw12) & 0xFFFF) + 1;
        uint32_t transfer_len = nlb * 4096;
        uint32_t i = 0;

        if (transfer_len > VHOST_USER_NVME_MAX_IO_SIZE) {
            cpl->status = vu_cpu_to_le16(0x0006);
            break;
        }

        for (i = 0; i < nlb; i++) {
            if (ftl_read((uint32_t)(slba + i),
                         g_io_read_buf + i * 4096) != RET_OK) {
                memset(g_io_read_buf + i * 4096, 0, 4096);
            }
        }
        prp_write_data(prp1, prp2, transfer_len, g_io_read_buf);
        cpl->status = vu_cpu_to_le16(0x0000);
        break;
    }
    case NVME_IO_WRITE: {
        uint64_t slba = ((uint64_t)vu_le32_to_cpu(cmd->cdw11) << 32) |
                        vu_le32_to_cpu(cmd->cdw10);
        uint16_t nlb = (vu_le16_to_cpu(cmd->cdw12) & 0xFFFF) + 1;
        uint32_t transfer_len = nlb * 4096;
        uint32_t i = 0;
        uint16_t status = 0x0000;

        if (transfer_len > VHOST_USER_NVME_MAX_IO_SIZE) {
            cpl->status = vu_cpu_to_le16(0x0006);
            break;
        }

        if (prp_read_data(prp1, prp2, transfer_len, g_io_write_buf) != RET_OK) {
            cpl->status = vu_cpu_to_le16(0x0006);
            break;
        }

        for (i = 0; i < nlb; i++) {
            if (ftl_write((uint32_t)(slba + i),
                          g_io_write_buf + i * 4096) != RET_OK) {
                status = 0x0006;
                break;
            }
        }
        cpl->status = vu_cpu_to_le16(status);
        break;
    }
    case NVME_IO_WRITE_ZEROES: {
        uint64_t slba = ((uint64_t)vu_le32_to_cpu(cmd->cdw11) << 32) |
                        vu_le32_to_cpu(cmd->cdw10);
        uint16_t nlb = (vu_le16_to_cpu(cmd->cdw12) & 0xFFFF) + 1;
        uint32_t i = 0;
        uint16_t status = 0x0000;

        for (i = 0; i < nlb; i++) {
            if (ftl_write((uint32_t)(slba + i), g_zero_buf) != RET_OK) {
                status = 0x0006;
                break;
            }
        }
        cpl->status = vu_cpu_to_le16(status);
        break;
    }
    case NVME_IO_DATASET_MGMT: {
        uint8_t nr = (vu_le32_to_cpu(cmd->cdw10) & 0xFF) + 1;
        uint32_t transfer_len = nr * 16;
        uint32_t r = 0;
        uint16_t status = 0x0000;

        if (transfer_len > VHOST_USER_NVME_MAX_IO_SIZE) {
            cpl->status = vu_cpu_to_le16(0x0006);
            break;
        }

        if (prp_read_data(prp1, prp2, transfer_len, g_io_write_buf) != RET_OK) {
            cpl->status = vu_cpu_to_le16(0x0006);
            break;
        }

        for (r = 0; r < nr; r++) {
            uint8_t *range = g_io_write_buf + r * 16;
            uint32_t length = vu_le32_to_cpu(*(uint32_t *)(range + 4)) + 1;
            uint64_t range_slba = vu_le64_to_cpu(*(uint64_t *)(range + 8));
            if (ftl_trim((uint32_t)range_slba, length) != RET_OK) {
                status = 0x0006;
            }
        }
        cpl->status = vu_cpu_to_le16(status);
        break;
    }
    default:
        nvme_ctrl_process_io_cmd(cmd, cpl);
        break;
    }
}

/**
 * @brief 处理一个 vring 中的所有待处理命令
 * @details 检查 kick eventfd（非阻塞 read），若有新命令则遍历
 *          available ring 中未处理的条目，读取描述符和 NVMe 命令，
 *          调用 process_nvme_command 处理，将完成写入 used ring，
 *          最后通过 call eventfd 发送中断。
 * @param vring_idx vring 索引（对应队列 ID）
 */
static void process_vring(uint32_t vring_idx)
{
    vhost_user_vring_t *vring = &g_ctx.vrings[vring_idx];
    virtq_avail_t *avail = NULL;
    virtq_used_t *used = NULL;
    virtq_desc_t *desc_table = NULL;
    uint64_t kick_val = 0;
    uint16_t avail_idx = 0;
    uint16_t desc_idx = 0;
    virtq_desc_t *desc = NULL;
    nvme_command_t *cmd = NULL;
    nvme_completion_t *cpl = NULL;
    uint8_t *buf = NULL;

    if (!vring->enabled || vring->kick_fd < 0) {
        return;
    }

    /* 非阻塞读取 kick eventfd，消耗通知 */
    if (read(vring->kick_fd, &kick_val, sizeof(kick_val)) < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* 无新通知，但仍检查 available ring（可能有遗漏） */
        } else {
            return;
        }
    }

    /* 映射 vring 各环到虚拟地址 */
    desc_table = (virtq_desc_t *)gpa_to_hva(vring->desc_addr);
    avail = (virtq_avail_t *)gpa_to_hva(vring->avail_addr);
    used = (virtq_used_t *)gpa_to_hva(vring->used_addr);

    if (desc_table == NULL || avail == NULL || used == NULL) {
        return;
    }

    /* 内存屏障：确保读取到最新的 available ring */
    __sync_synchronize();

    avail_idx = vu_le16_to_cpu(avail->idx);

    /* 处理所有未处理的 available 条目 */
    while (vring->last_avail_idx != avail_idx) {
        desc_idx = vu_le16_to_cpu(
            avail->ring[vring->last_avail_idx % vring->num]);
        desc = &desc_table[desc_idx];

        /* 描述符缓冲区：[nvme_command_t 64B][nvme_completion_t 16B] */
        buf = (uint8_t *)gpa_to_hva(desc->addr);
        if (buf == NULL) {
            LOG_ERROR("vhost-user: vring[%u] 描述符地址转换失败, addr=0x%llX",
                      vring_idx, (unsigned long long)desc->addr);
            vring->last_avail_idx++;
            continue;
        }

        cmd = (nvme_command_t *)buf;
        cpl = (nvme_completion_t *)(buf + sizeof(nvme_command_t));

        /* 处理 NVMe 命令 */
        process_nvme_command(vring, cmd, cpl, (uint16_t)vring_idx);

        /* 将完成写入 used ring */
        used->ring[vring->used_idx % vring->num].id = vu_cpu_to_le32(desc_idx);
        used->ring[vring->used_idx % vring->num].len =
            vu_cpu_to_le32(sizeof(nvme_completion_t));
        vring->used_idx++;

        /* 内存屏障：确保 used ring 写入对 QEMU 可见 */
        __sync_synchronize();
        used->idx = vu_cpu_to_le16(vring->used_idx);
        __sync_synchronize();

        vring->last_avail_idx++;

        /* 通过 call eventfd 发送中断通知 */
        if (vring->call_fd >= 0) {
            uint64_t call_val = 1;
            write(vring->call_fd, &call_val, sizeof(call_val));
        }
    }
}

/* ============================================================
 *  vhost-user 消息分发
 * ============================================================ */

/**
 * @brief 分发并处理一条 vhost-user 消息
 * @param hdr     消息头部
 * @param payload payload 数据
 * @param fds     伴随的文件描述符数组
 * @param num_fds 文件描述符数量
 */
static void dispatch_message(const vhost_user_msg_hdr_t *hdr,
                             const uint8_t *payload,
                             const int *fds, int num_fds)
{
    switch (hdr->request) {
    case VHOST_USER_GET_FEATURES:
        handle_get_features();
        break;
    case VHOST_USER_SET_FEATURES:
        handle_set_features(payload);
        break;
    case VHOST_USER_SET_OWNER:
        handle_set_owner();
        break;
    case VHOST_USER_RESET_OWNER:
        handle_reset_owner();
        break;
    case VHOST_USER_SET_MEM_TABLE:
        handle_set_mem_table(payload, fds, num_fds);
        break;
    case VHOST_USER_SET_VRING_NUM:
        handle_set_vring_num(payload);
        break;
    case VHOST_USER_SET_VRING_ADDR:
        handle_set_vring_addr(payload);
        break;
    case VHOST_USER_SET_VRING_BASE:
        handle_set_vring_base(payload);
        break;
    case VHOST_USER_GET_VRING_BASE:
        handle_get_vring_base(payload);
        break;
    case VHOST_USER_SET_VRING_KICK:
        handle_set_vring_kick(payload, (num_fds > 0) ? fds[0] : -1);
        break;
    case VHOST_USER_SET_VRING_CALL:
        handle_set_vring_call(payload, (num_fds > 0) ? fds[0] : -1);
        break;
    case VHOST_USER_SET_VRING_ENABLE:
        handle_set_vring_enable(payload);
        break;
    case VHOST_USER_GET_PROTOCOL_FEATURES:
        handle_get_protocol_features();
        break;
    case VHOST_USER_SET_PROTOCOL_FEATURES:
        handle_set_protocol_features(payload);
        break;
    case VHOST_USER_GET_CONFIG:
        handle_get_config(payload);
        break;
    case VHOST_USER_SET_CONFIG:
        handle_set_config(payload);
        break;
    case VHOST_USER_GET_QUEUE_NUM: {
        uint32_t queue_num = g_ctx.config.max_queues;
        vu_send_reply(g_ctx.conn_fd, VHOST_USER_GET_QUEUE_NUM,
                      &queue_num, sizeof(queue_num));
        break;
    }
    default:
        LOG_WARN("vhost-user: 未处理的消息 ID=%u", hdr->request);
        /* 对于需要回复的消息，发送空回复 */
        if (hdr->flags & VHOST_USER_FLAG_NEED_REPLY) {
            vu_send_reply(g_ctx.conn_fd, hdr->request, NULL, 0);
        }
        break;
    }
}

/**
 * @brief 处理连接 socket 上的 vhost-user 消息
 * @details 非阻塞读取消息，分发到对应处理函数。连接断开时
 *          清理资源并重置状态，支持重新 accept。
 * @return RET_OK 成功，RET_ERR_INTERNAL 连接断开或错误
 */
static ret_code_t handle_connection_messages(void)
{
    vhost_user_msg_hdr_t hdr;
    uint8_t payload[VHOST_USER_MAX_PAYLOAD_SIZE];
    int fds[VHOST_USER_MAX_MEM_REGIONS];
    int num_fds = 0;
    ssize_t payload_len = 0;
    uint32_t i = 0;

    memset(fds, -1, sizeof(fds));
    payload_len = vu_recv_msg(g_ctx.conn_fd, &hdr, payload, fds,
                              VHOST_USER_MAX_MEM_REGIONS);

    if (payload_len < 0) {
        /* 连接断开或错误 */
        LOG_INFO("vhost-user: 连接断开, errno=%d", errno);
        /* 关闭未使用的 FD */
        for (i = 0; i < VHOST_USER_MAX_MEM_REGIONS; i++) {
            if (fds[i] >= 0) close(fds[i]);
        }
        return RET_ERR_INTERNAL;
    }

    num_fds = 0;
    for (i = 0; i < VHOST_USER_MAX_MEM_REGIONS; i++) {
        if (fds[i] >= 0) num_fds++;
    }

    dispatch_message(&hdr, payload, fds, num_fds);

    return RET_OK;
}

/**
 * @brief 关闭当前连接并清理资源
 * @details 关闭连接 socket，解除内存映射，关闭 eventfd，
 *          重置上下文状态，以便接受新连接。
 */
static void close_connection(void)
{
    uint32_t i = 0;

    if (g_ctx.conn_fd >= 0) {
        close(g_ctx.conn_fd);
        g_ctx.conn_fd = -1;
    }

    /* 解除内存映射 */
    for (i = 0; i < g_ctx.mem_region_count; i++) {
        if (g_ctx.mem_mappings[i] != NULL) {
            munmap(g_ctx.mem_mappings[i], g_ctx.mem_regions[i].memory_size);
            g_ctx.mem_mappings[i] = NULL;
        }
        if (g_ctx.mem_fds[i] >= 0) {
            close(g_ctx.mem_fds[i]);
            g_ctx.mem_fds[i] = -1;
        }
    }
    g_ctx.mem_region_count = 0;

    /* 关闭 vring eventfd */
    for (i = 0; i < VHOST_USER_NVME_MAX_QUEUES; i++) {
        if (g_ctx.vrings[i].kick_fd >= 0) {
            close(g_ctx.vrings[i].kick_fd);
            g_ctx.vrings[i].kick_fd = -1;
        }
        if (g_ctx.vrings[i].call_fd >= 0) {
            close(g_ctx.vrings[i].call_fd);
            g_ctx.vrings[i].call_fd = -1;
        }
        g_ctx.vrings[i].enabled = false;
        g_ctx.vrings[i].last_avail_idx = 0;
        g_ctx.vrings[i].used_idx = 0;
    }

    g_ctx.connected = false;
    g_ctx.owner_set = false;
    g_ctx.features = 0;
    g_ctx.protocol_features = 0;

    /* 重置寄存器 */
    init_nvme_regs();

    LOG_INFO("vhost-user: 连接已清理，等待新连接");
}

/* ============================================================
 *  对外接口实现
 * ============================================================ */

ret_code_t vhost_user_nvme_init(const vhost_user_nvme_config_t *config)
{
    struct sockaddr_un addr;
    uint32_t i = 0;

    if (config == NULL) {
        LOG_ERROR("vhost-user: 配置为空");
        return RET_ERR_PARAM;
    }

    memset(&g_ctx, 0, sizeof(g_ctx));
    memcpy(&g_ctx.config, config, sizeof(vhost_user_nvme_config_t));

    if (g_ctx.config.max_queues == 0) {
        g_ctx.config.max_queues = 2;  /* 默认 Admin + 1 I/O */
    }
    if (g_ctx.config.socket_path[0] == '\0') {
        strncpy(g_ctx.config.socket_path, VHOST_USER_NVME_DEFAULT_SOCKET,
                sizeof(g_ctx.config.socket_path) - 1);
    }

    /* 初始化 vring 状态 */
    for (i = 0; i < VHOST_USER_NVME_MAX_QUEUES; i++) {
        g_ctx.vrings[i].kick_fd = -1;
        g_ctx.vrings[i].call_fd = -1;
        g_ctx.vrings[i].num = 0;
        g_ctx.vrings[i].enabled = false;
        g_ctx.vrings[i].last_avail_idx = 0;
        g_ctx.vrings[i].used_idx = 0;
    }

    /* 初始化内存区域 FD */
    for (i = 0; i < VHOST_USER_MAX_MEM_REGIONS; i++) {
        g_ctx.mem_fds[i] = -1;
        g_ctx.mem_mappings[i] = NULL;
    }

    g_ctx.socket_fd = -1;
    g_ctx.conn_fd = -1;

    /* 初始化 NVMe 寄存器镜像 */
    init_nvme_regs();

    /* 删除已存在的 socket 文件 */
    unlink(g_ctx.config.socket_path);

    /* 创建 Unix domain socket */
    g_ctx.socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_ctx.socket_fd < 0) {
        LOG_ERROR("vhost-user: 创建 socket 失败, errno=%d", errno);
        return RET_ERR_INTERNAL;
    }

    /* 设置非阻塞 */
    fcntl(g_ctx.socket_fd, F_SETFL, O_NONBLOCK);

    /* 绑定地址 */
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_ctx.config.socket_path, sizeof(addr.sun_path) - 1);

    if (bind(g_ctx.socket_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("vhost-user: 绑定 socket 失败, path=%s, errno=%d",
                  g_ctx.config.socket_path, errno);
        close(g_ctx.socket_fd);
        g_ctx.socket_fd = -1;
        return RET_ERR_INTERNAL;
    }

    /* 监听 */
    if (listen(g_ctx.socket_fd, 1) < 0) {
        LOG_ERROR("vhost-user: 监听 socket 失败, errno=%d", errno);
        close(g_ctx.socket_fd);
        g_ctx.socket_fd = -1;
        return RET_ERR_INTERNAL;
    }

    g_ctx.running = true;

    LOG_INFO("vhost-user NVMe 后端初始化完成, socket=%s, max_queues=%u",
             g_ctx.config.socket_path, g_ctx.config.max_queues);

    return RET_OK;
}

ret_code_t vhost_user_nvme_deinit(void)
{
    g_ctx.running = false;

    close_connection();

    if (g_ctx.socket_fd >= 0) {
        close(g_ctx.socket_fd);
        g_ctx.socket_fd = -1;
    }

    unlink(g_ctx.config.socket_path);

    LOG_INFO("vhost-user NVMe 后端已关闭");
    return RET_OK;
}

void vhost_user_nvme_process(void)
{
    fd_set readfds;
    int maxfd = 0;
    struct timeval tv;
    uint32_t i = 0;

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
                LOG_WARN("vhost-user: 已有连接，拒绝新连接");
            } else {
                fcntl(client_fd, F_SETFL, O_NONBLOCK);
                g_ctx.conn_fd = client_fd;
                g_ctx.connected = true;
                LOG_INFO("vhost-user: QEMU 已连接");
            }
        }
    }

    /* 2. 处理 vhost-user 协议消息 */
    if (g_ctx.connected && g_ctx.conn_fd >= 0 &&
        FD_ISSET(g_ctx.conn_fd, &readfds)) {
        if (handle_connection_messages() != RET_OK) {
            close_connection();
        }
    }

    /* 3. 处理所有已使能 vring 的命令 */
    for (i = 0; i < VHOST_USER_NVME_MAX_QUEUES; i++) {
        if (g_ctx.vrings[i].enabled) {
            process_vring(i);
        }
    }
}
