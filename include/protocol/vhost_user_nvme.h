/**
 * @file vhost_user_nvme.h
 * @brief vhost-user NVMe 后端协议头文件
 * @details 定义 vhost-user 协议消息格式、特性位、vring 状态、
 *          内存区域映射及后端上下文结构体，提供 vhost-user NVMe
 *          后端的初始化、事件处理与反初始化接口。
 *
 *          vhost-user 是 QEMU 提供的用户态 vhost 后端协议，后端通过
 *          Unix domain socket 与 QEMU 通信，使用共享内存映射 NVMe
 *          控制器寄存器(BAR0)和提交/完成队列。QEMU 通过
 *          -device vhost-user-nvme,chardev=... 连接本后端。
 *
 *          工作流程：
 *          1. 后端监听 Unix socket，QEMU 发起连接
 *          2. 协议握手：GET_FEATURES → SET_FEATURES →
 *             GET_PROTOCOL_FEATURES → SET_PROTOCOL_FEATURES →
 *             SET_OWNER → SET_MEM_TABLE → SET_VRING_* → SET_VRING_ENABLE
 *          3. QEMU 写门铃寄存器(SET_CONFIG) → 后端检测门铃变化
 *          4. 后端从 vring available ring 取 NVMe 命令(64字节)
 *          5. 调用 nvme_ctrl_process_admin_cmd/io_cmd 处理命令
 *          6. Read/Write 命令通过 PRP 指针访问共享内存传输数据
 *          7. 完成写入 used ring，通过 call eventfd 发中断
 */

#ifndef VHOST_USER_NVME_H
#define VHOST_USER_NVME_H

#include "common/common.h"
#include "protocol/nvme_controller.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  常量定义
 * ============================================================ */

/** @brief 默认 Unix socket 路径 */
#define VHOST_USER_NVME_DEFAULT_SOCKET  "/tmp/ftl-vhost-user.sock"

/** @brief 最大 vhost-user 队列数（Admin + I/O） */
#define VHOST_USER_NVME_MAX_QUEUES      8U

/** @brief 最大内存区域数 */
#define VHOST_USER_MAX_MEM_REGIONS      16U

/** @brief NVMe 寄存器空间大小（BAR0，4KB） */
#define VHOST_USER_NVME_REG_SIZE        4096U

/** @brief vhost-user 消息最大 payload 大小 */
#define VHOST_USER_MAX_PAYLOAD_SIZE     4096U

/** @brief 最大 I/O 数据传输大小（1MB） */
#define VHOST_USER_NVME_MAX_IO_SIZE     (1024U * 1024U)

/* ============================================================
 *  vhost-user 消息 ID 枚举
 * ============================================================ */

/**
 * @brief vhost-user 协议请求消息 ID
 * @details 遵循 QEMU vhost-user 规范定义
 */
typedef enum {
    VHOST_USER_NONE                 = 0,
    VHOST_USER_GET_FEATURES         = 1,
    VHOST_USER_SET_FEATURES         = 2,
    VHOST_USER_SET_OWNER            = 3,
    VHOST_USER_RESET_OWNER          = 4,
    VHOST_USER_SET_MEM_TABLE        = 5,
    VHOST_USER_SET_LOG_BASE         = 6,
    VHOST_USER_SET_LOG_FD           = 7,
    VHOST_USER_SET_VRING_NUM        = 8,
    VHOST_USER_SET_VRING_ADDR       = 9,
    VHOST_USER_SET_VRING_BASE       = 10,
    VHOST_USER_GET_VRING_BASE       = 11,
    VHOST_USER_SET_VRING_KICK       = 12,
    VHOST_USER_SET_VRING_CALL       = 13,
    VHOST_USER_SET_VRING_ERR        = 14,
    VHOST_USER_GET_PROTOCOL_FEATURES = 15,
    VHOST_USER_SET_PROTOCOL_FEATURES = 16,
    VHOST_USER_GET_QUEUE_NUM        = 17,
    VHOST_USER_SET_VRING_ENABLE     = 18,
    VHOST_USER_SEND_RARP            = 19,
    VHOST_USER_NET_SET_MTU          = 20,
    VHOST_USER_SET_SLAVE_REQ_FD     = 21,
    VHOST_USER_IOTLB_MSG            = 22,
    VHOST_USER_SET_VRING_ENDIAN     = 23,
    VHOST_USER_GET_CONFIG           = 24,
    VHOST_USER_SET_CONFIG           = 25,
    VHOST_USER_CREATE_CRYPTO_SESSION = 26,
    VHOST_USER_CLOSE_CRYPTO_SESSION = 27,
    VHOST_USER_POSTCOPY_ADVISE      = 28,
    VHOST_USER_POSTCOPY_LISTEN      = 29,
    VHOST_USER_POSTCOPY_END         = 30,
    VHOST_USER_GET_INFLIGHT_FD      = 31,
    VHOST_USER_SET_INFLIGHT_FD      = 32,
    VHOST_USER_SET_STATUS           = 38,
    VHOST_USER_GET_STATUS           = 39,
    VHOST_USER_MAX                  = 40
} vhost_user_request_t;

/* ============================================================
 *  vhost-user 特性位
 * ============================================================ */

/** @brief 协议特性位（bit 30），表示支持 GET/SET_PROTOCOL_FEATURES */
#define VHOST_USER_F_PROTOCOL_FEATURES     (1ULL << 30)

/** @brief vhost-user 协议特性：支持 config 空间读写 */
#define VHOST_USER_PROTOCOL_F_CONFIG       (1ULL << 1)

/** @brief vhost-user 协议特性：支持 reply ack */
#define VHOST_USER_PROTOCOL_F_REPLY_ACK    (1ULL << 3)

/** @brief vhost-user 协议特性：支持状态获取/设置 */
#define VHOST_USER_PROTOCOL_F_STATUS       (1ULL << 7)

/* ============================================================
 *  vhost-user 消息标志位
 * ============================================================ */

/** @brief 消息需要回复 */
#define VHOST_USER_FLAG_REPLY              (1U << 2)

/** @brief 消息需要回复确认 */
#define VHOST_USER_FLAG_NEED_REPLY         (1U << 3)

/* ============================================================
 *  virtio vring 数据结构
 * ============================================================ */

/**
 * @brief virtio 描述符（vring desc entry，16字节）
 */
typedef struct {
    uint64_t addr;    ///< 缓冲区 guest 物理地址
    uint32_t len;     ///< 缓冲区长度
    uint16_t flags;   ///< 描述符标志（VRING_DESC_F_NEXT=0x1, WRITE=0x2）
    uint16_t next;    ///< 链中下一个描述符索引
} virtq_desc_t;

/**
 * @brief virtio available ring（前端写，后端读）
 */
typedef struct {
    uint16_t flags;    ///< 标志（VRING_AVAIL_F_NO_INTERRUPT=0x1）
    uint16_t idx;      ///< 下一个可用条目索引
    uint16_t ring[1];  ///< 描述符索引数组（变长）
} virtq_avail_t;

/**
 * @brief virtio used ring entry（后端写，前端读）
 */
typedef struct {
    uint32_t id;       ///< 描述符索引（起始描述符）
    uint32_t len;      ///< 写入总长度
} virtq_used_elem_t;

/**
 * @brief virtio used ring
 */
typedef struct {
    uint16_t flags;          ///< 标志（VRING_USED_F_NO_NOTIFY=0x1）
    uint16_t idx;            ///< 下一个使用条目索引
    virtq_used_elem_t ring[1]; ///< 使用条目数组（变长）
} virtq_used_t;

/** @brief 描述符标志：链中有下一个描述符 */
#define VRING_DESC_F_NEXT    0x0001U

/** @brief 描述符标志：设备可写（对于 Read 命令的数据缓冲区） */
#define VRING_DESC_F_WRITE   0x0002U

/* ============================================================
 *  vhost-user 消息结构
 * ============================================================ */

/**
 * @brief vhost-user 消息固定头部（12字节）
 * @details 每个 vhost-user 消息由固定头部 + payload 组成，
 *          支持通过 SCM_RIGHTS 辅助数据传递文件描述符。
 */
typedef struct {
    uint32_t request;  ///< 消息 ID（vhost_user_request_t）
    uint32_t flags;    ///< 消息标志
    uint32_t size;     ///< payload 字节数
} vhost_user_msg_hdr_t;

/**
 * @brief vhost-user 内存区域描述符（VhostUserMemoryRegion，32字节）
 */
typedef struct {
    uint64_t guest_phys_addr;  ///< 区域在 guest 中的起始物理地址
    uint64_t memory_size;      ///< 区域大小（字节）
    uint64_t userspace_addr;   ///< 区域在 QEMU 进程中的虚拟地址（忽略）
    uint64_t mmap_offset;      ///< 在对应 FD 中的 mmap 偏移
} vhost_user_mem_region_t;

/**
 * @brief vhost-user 内存表（VhostUserMemory）
 * @details SET_MEM_TABLE 消息的 payload，携带所有内存区域描述符，
 *          每个区域对应一个通过 SCM_RIGHTS 传递的文件描述符。
 */
typedef struct {
    uint32_t nregions;    ///< 内存区域数量
    uint32_t padding;     ///< 保留对齐
    vhost_user_mem_region_t regions[VHOST_USER_MAX_MEM_REGIONS]; ///< 区域数组
} vhost_user_memory_t;

/**
 * @brief vhost-user vring 地址描述（VhostUserVringAddr，32字节）
 */
typedef struct {
    uint32_t index;       ///< vring 索引
    uint32_t flags;       ///< 标志
    uint64_t desc_addr;   ///< 描述符表 guest 物理地址
    uint64_t avail_addr;  ///< available ring guest 物理地址
    uint64_t used_addr;   ///< used ring guest 物理地址
} vhost_user_vring_addr_t;

/**
 * @brief vhost-user vring 状态（VhostUserVringState，8字节）
 */
typedef struct {
    uint32_t index;  ///< vring 索引
    uint32_t num;    ///< vring 大小（描述符数）或 base 值
} vhost_user_vring_state_t;

/**
 * @brief vhost-user config 空间消息（VhostUserConfig，8字节+数据）
 * @details GET_CONFIG/SET_CONFIG 的 payload，用于 NVMe 寄存器读写。
 *          offset 是 BAR0 内偏移，size 是读写长度，flags 指示方向。
 */
typedef struct {
    uint32_t offset;  ///< 配置空间偏移（BAR0 内偏移）
    uint32_t size;    ///< 读写长度
    uint32_t flags;   ///< 标志
    uint8_t  data[1]; ///< 配置数据（变长）
} vhost_user_config_t;

/* ============================================================
 *  vring 运行时状态
 * ============================================================ */

/**
 * @brief vhost-user vring 运行时状态
 * @details 每个 vring 对应一个 NVMe 队列（Admin=vring0, I/O=vring1+）。
 *          包含 vring 大小、各环地址、kick/call eventfd、使能状态
 *          及上次处理的 available 索引。
 */
typedef struct {
    uint32_t num;            ///< vring 大小（描述符数）
    uint64_t desc_addr;      ///< 描述符表 guest 物理地址
    uint64_t avail_addr;     ///< available ring guest 物理地址
    uint64_t used_addr;      ///< used ring guest 物理地址
    uint32_t base;           ///< 上次使用的 available 索引（SET_VRING_BASE）
    int      kick_fd;        ///< kick eventfd（QEMU 写，后端读，通知有新命令）
    int      call_fd;        ///< call eventfd（后端写，QEMU 读，通知命令完成）
    bool     enabled;        ///< vring 是否已使能
    uint16_t last_avail_idx; ///< 上次已处理的 available ring 索引
    uint16_t used_idx;       ///< used ring 当前写入索引
} vhost_user_vring_t;

/* ============================================================
 *  后端配置与上下文
 * ============================================================ */

/**
 * @brief vhost-user NVMe 后端配置
 */
typedef struct {
    char     socket_path[256];  ///< Unix domain socket 路径
    uint32_t max_queues;        ///< 最大队列数（含 Admin 队列）
} vhost_user_nvme_config_t;

/**
 * @brief vhost-user NVMe 后端上下文
 * @details 保存后端运行时全部状态，包括 socket 描述符、内存映射、
 *          vring 数组、NVMe 寄存器镜像及协商的特性位。
 */
typedef struct {
    int      socket_fd;                          ///< 监听 socket 描述符
    int      conn_fd;                            ///< 已连接 socket 描述符
    bool     running;                            ///< 后端是否运行中
    bool     connected;                          ///< QEMU 是否已连接
    uint64_t features;                           ///< 协商后的 vhost 特性位
    uint64_t protocol_features;                  ///< 协商后的协议特性位
    bool     owner_set;                          ///< SET_OWNER 是否已完成

    /* 内存区域映射 */
    vhost_user_mem_region_t mem_regions[VHOST_USER_MAX_MEM_REGIONS]; ///< 区域信息
    void    *mem_mappings[VHOST_USER_MAX_MEM_REGIONS];               ///< mmap 虚拟地址
    int      mem_fds[VHOST_USER_MAX_MEM_REGIONS];                    ///< 区域 FD
    uint32_t mem_region_count;                                        ///< 区域数量

    /* vring 数组 */
    vhost_user_vring_t vrings[VHOST_USER_NVME_MAX_QUEUES];

    /* NVMe 寄存器镜像（BAR0，4KB） */
    uint8_t  regs[VHOST_USER_NVME_REG_SIZE];

    /* 配置 */
    vhost_user_nvme_config_t config;
} vhost_user_nvme_ctx_t;

/* ============================================================
 *  对外接口函数
 * ============================================================ */

/**
 * @brief 初始化 vhost-user NVMe 后端
 * @details 创建 Unix domain socket，绑定并监听，初始化寄存器镜像
 *          和 vring 状态。应在 NVMe 控制器初始化之后调用。
 * @param config 后端配置（socket 路径、最大队列数）
 * @retval RET_OK 初始化成功
 * @retval RET_ERR_PARAM 配置为空或参数非法
 * @retval RET_ERR_INTERNAL socket 创建/绑定/监听失败
 */
ret_code_t vhost_user_nvme_init(const vhost_user_nvme_config_t *config);

/**
 * @brief 反初始化 vhost-user NVMe 后端
 * @details 关闭连接和监听 socket，解除所有内存区域 mmap 映射，
 *          关闭所有 eventfd。
 * @retval RET_OK 成功
 */
ret_code_t vhost_user_nvme_deinit(void);

/**
 * @brief vhost-user NVMe 后端事件处理（单次调用，非阻塞）
 * @details 应在主循环中周期性调用。处理：
 *          1. 监听 socket：接受 QEMU 新连接
 *          2. 已连接 socket：接收并处理 vhost-user 协议消息
 *          3. 已使能 vring：检查 kick eventfd，处理 NVMe 命令
 *
 *          命令处理流程：从 available ring 取描述符 → 读取
 *          nvme_command_t → 调用 nvme_ctrl_process_admin_cmd/io_cmd →
 *          PRP 数据传输 → 完成写入 used ring → call eventfd 发中断
 */
void vhost_user_nvme_process(void);

#ifdef __cplusplus
}
#endif

#endif /* VHOST_USER_NVME_H */
