/**
 * @file vfio_user_nvme.h
 * @brief vfio-user NVMe 后端协议头文件
 * @details 定义 vfio-user 协议消息格式、消息 ID、PCIe 配置空间、
 *          BAR0 布局、DMA 映射及后端上下文结构体，提供 vfio-user
 *          NVMe 后端的初始化、事件处理与反初始化接口。
 *
 *          vfio-user 是 QEMU 提供的用户态 VFIO 后端协议，后端通过
 *          Unix domain socket 与 QEMU 通信，模拟完整的 PCIe 设备
 *          （配置空间 + BAR + MSI-X 中断）。QEMU 通过
 *          -device vfio-user-pci,socket=... 连接本后端。
 *
 *          工作流程：
 *          1. 后端监听 Unix socket，QEMU 发起连接
 *          2. 协议握手：GET_API_VERSION → DEVICE_GET_INFO →
 *             DEVICE_GET_REGION_INFO → DEVICE_GET_IRQ_INFO →
 *             DMA_MAP → DEVICE_SET_IRQS
 *          3. QEMU 通过 REGION_READ/WRITE 访问 PCI 配置空间和 BAR0
 *          4. 主机驱动写 NVMe 寄存器（CC.EN=1 → CSTS.RDY=1）
 *          5. 主机驱动写门铃寄存器 → 后端从 guest 内存 SQ 读命令
 *          6. 调用 nvme_ctrl_process_admin_cmd/io_cmd 处理命令
 *          7. Read/Write 命令通过 PRP 指针访问共享内存传输数据
 *          8. 完成写入 CQ，通过 MSI-X eventfd 发中断
 */

#ifndef VFIO_USER_NVME_H
#define VFIO_USER_NVME_H

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
#define VFIO_USER_NVME_DEFAULT_SOCKET  "/tmp/ftl-vfio-user.sock"

/** @brief 最大 DMA 内存区域数 */
#define VFIO_USER_MAX_DMA_REGIONS      16U

/** @brief vfio-user 消息最大 payload 大小 */
#define VFIO_USER_MAX_PAYLOAD_SIZE     8192U

/** @brief 最大 I/O 数据传输大小（1MB） */
#define VFIO_USER_NVME_MAX_IO_SIZE     (1024U * 1024U)

/** @brief PCIe 配置空间大小（256字节） */
#define VFIO_PCI_CONFIG_SIZE           256U

/** @brief BAR0 大小（64KB） */
#define VFIO_BAR0_SIZE                 0x10000U

/** @brief NVMe 控制器寄存器在 BAR0 内的偏移 */
#define VFIO_BAR0_NVME_REGS_OFFSET     0x0000U

/** @brief NVMe 控制器寄存器大小（4KB） */
#define VFIO_BAR0_NVME_REGS_SIZE       4096U

/** @brief 门铃寄存器在 BAR0 内的偏移 */
#define VFIO_BAR0_DOORBELL_OFFSET      0x1000U

/** @brief MSI-X 向量表在 BAR0 内的偏移 */
#define VFIO_BAR0_MSIX_TABLE_OFFSET    0x2000U

/** @brief MSI-X PBA 在 BAR0 内的偏移 */
#define VFIO_BAR0_MSIX_PBA_OFFSET      0x3000U

/** @brief MSI-X 向量数 */
#define VFIO_MSIX_VECTOR_COUNT         1U

/** @brief MSI-X 向量表条目大小（16字节） */
#define VFIO_MSIX_TABLE_ENTRY_SIZE     16U

/** @brief Region 索引：PCI 配置空间 */
#define VFIO_REGION_PCI_CONFIG         0U

/** @brief Region 索引：BAR0 */
#define VFIO_REGION_BAR0               1U

/** @brief Region 总数 */
#define VFIO_REGION_COUNT              2U

/** @brief IRQ 索引：MSI-X */
#define VFIO_IRQ_MSIX                  0U

/** @brief IRQ 总数 */
#define VFIO_IRQ_COUNT                 1U

/* ============================================================
 *  vfio-user 消息 ID
 * ============================================================ */

/**
 * @brief vfio-user 协议消息 ID
 * @details 参考 QEMU 源码 include/hw/vfio/vfio-user.h
 */
typedef enum {
    VFU_GET_API_VERSION      = 1,   ///< 获取 API 版本
    VFU_SET_RESET            = 2,   ///< 设置/触发设备复位
    VFU_DMA_MAP              = 3,   ///< 映射 DMA 内存区域
    VFU_DMA_UNMAP            = 4,   ///< 解除 DMA 内存映射
    VFU_DEVICE_GET_INFO      = 5,   ///< 获取设备信息（region/irq 数）
    VFU_DEVICE_GET_REGION_INFO = 6, ///< 获取 region 信息
    VFU_DEVICE_GET_IRQ_INFO  = 7,   ///< 获取 IRQ 信息
    VFU_DEVICE_SET_IRQS      = 8,   ///< 设置 IRQ（eventfd）
    VFU_REGION_READ          = 9,   ///< 读 region（PCI config / BAR）
    VFU_REGION_WRITE         = 10,  ///< 写 region（PCI config / BAR）
    VFU_DMA_READ             = 11,  ///< 后端读 guest DMA 内存
    VFU_DMA_WRITE            = 12,  ///< 后端写 guest DMA 内存
    VFU_INTERRUPT            = 13,  ///< server→client 中断通知
    VFU_MAX                  = 14   ///< 消息 ID 上限
} vfu_msg_id_t;

/* ============================================================
 *  vfio-user 消息标志
 * ============================================================ */

/** @brief 消息需要回复 */
#define VFU_MSG_FLAG_REPLY            (1U << 2)

/* ============================================================
 *  IRQ 数据类型
 * ============================================================ */

/** @brief IRQ 数据类型：无（禁用中断） */
#define VFU_IRQ_DATA_TYPE_NONE        0U

/** @brief IRQ 数据类型：eventfd */
#define VFU_IRQ_DATA_TYPE_EVENTFD     1U

/* ============================================================
 *  Region 类型
 * ============================================================ */

/** @brief Region 类型：PCI 配置空间 */
#define VFIO_REGION_TYPE_PCI_CONFIG   1U

/** @brief Region 类型：PCI BAR */
#define VFIO_REGION_TYPE_PCI_BAR      2U

/* ============================================================
 *  PCIe 配置空间常量
 * ============================================================ */

/** @brief Vendor ID: Red Hat */
#define VFIO_PCI_VENDOR_ID            0x1B36U

/** @brief Device ID: NVMe */
#define VFIO_PCI_DEVICE_ID            0x0010U

/** @brief Command: IO Space + Memory Space enable */
#define VFIO_PCI_COMMAND              0x0006U

/** @brief Status: Capabilities List */
#define VFIO_PCI_STATUS               0x0010U

/** @brief Revision ID */
#define VFIO_PCI_REVISION_ID          0x01U

/** @brief Class Code: Mass Storage / NVMe (0x010802) */
#define VFIO_PCI_CLASS_CODE           0x010802U

/** @brief Cache Line Size */
#define VFIO_PCI_CACHE_LINE_SIZE      0x10U

/** @brief Header Type: Type 0 */
#define VFIO_PCI_HEADER_TYPE          0x00U

/** @brief Subsystem Vendor ID */
#define VFIO_PCI_SUBSYS_VENDOR_ID     0x1B36U

/** @brief Subsystem ID */
#define VFIO_PCI_SUBSYS_ID            0x0010U

/** @brief Capability Pointer: MSI-X at 0x40 */
#define VFIO_PCI_CAP_POINTER          0x40U

/** @brief MSI-X Capability ID */
#define VFIO_PCI_MSIX_CAP_ID          0x11U

/** @brief MSI-X Message Control: Table Size=0 (1 vector), Function Mask=0, Enable=1 */
#define VFIO_PCI_MSIX_MSG_CTRL        0x8000U

/** @brief MSI-X Table Offset/BIR: BAR0 内偏移 0x2000, BIR=0 */
#define VFIO_PCI_MSIX_TABLE_OFFSET    0x00002000U

/** @brief MSI-X PBA Offset/BIR: BAR0 内偏移 0x3000, BIR=0 */
#define VFIO_PCI_MSIX_PBA_OFFSET      0x00003000U

/** @brief BAR0: 64-bit, 内存空间, prefetchable, size 64KB
 *  bits[1:0]=0 (memory), bit[2]=1 (64-bit), bit[3]=1 (prefetchable),
 *  size encoded as ~(0xFFFF) | 0x0F = 0xFFFF_FFF0, but stored as
 *  low 32 bits = 0xFFFF0000 | 0x0C = 0xFFFF000C */
#define VFIO_PCI_BAR0_LOW             0xFFFF000CU

/** @brief BAR1: BAR0 高32位（0，32位地址空间） */
#define VFIO_PCI_BAR1                 0x00000000U

/* ============================================================
 *  vfio-user 消息结构
 * ============================================================ */

/**
 * @brief vfio-user 消息固定头部（16字节）
 * @details 每个 vfio-user 消息由固定头部 + payload 组成，
 *          支持通过 SCM_RIGHTS 辅助数据传递文件描述符。
 */
typedef struct {
    uint16_t msg_id;       ///< 消息 ID（vfu_msg_id_t）
    uint16_t msg_version;  ///< 协议版本（0）
    uint32_t msg_size;     ///< payload 字节数
    uint32_t msg_flags;    ///< 消息标志
    uint32_t msg_errno;    ///< 错误码（回复时使用，0=成功）
} vfu_msg_hdr_t;

/**
 * @brief VFU_DEVICE_GET_INFO 回复结构（24字节）
 */
typedef struct {
    uint32_t argsz;        ///< 结构大小
    uint32_t flags;        ///< 标志
    uint32_t num_regions;  ///< region 数量
    uint32_t num_irqs;     ///< IRQ 数量
    uint32_t flags_ext;    ///< 扩展标志
    uint32_t reserved;     ///< 保留
} vfu_device_info_t;

/**
 * @brief VFU_DEVICE_GET_REGION_INFO 请求/回复结构（40字节）
 */
typedef struct {
    uint32_t argsz;        ///< 结构大小
    uint32_t flags;        ///< 标志
    uint32_t index;        ///< 请求时: region 索引
    uint32_t cap_offset;   ///< capability 偏移
    uint64_t size;         ///< region 大小
    uint64_t offset;       ///< 在文件描述符中的偏移（mmap offset）
    uint32_t type;         ///< region 类型
    uint32_t subtype;      ///< region 子类型（BAR 编号）
    uint32_t flags_ext;    ///< 扩展标志
    uint32_t reserved;     ///< 保留
} vfu_region_info_t;

/**
 * @brief VFU_DEVICE_GET_IRQ_INFO 请求/回复结构（24字节）
 */
typedef struct {
    uint32_t argsz;        ///< 结构大小
    uint32_t flags;        ///< 标志
    uint32_t index;        ///< IRQ 索引
    uint32_t count;        ///< 中断向量数
    uint32_t flags_ext;    ///< 扩展标志
    uint32_t reserved;     ///< 保留
} vfu_irq_info_t;

/**
 * @brief VFU_DEVICE_SET_IRQS 请求结构（变长）
 * @details data 字段携带 eventfd 数组（每个 eventfd 为 int，4字节）
 */
typedef struct {
    uint32_t argsz;        ///< 结构大小（含 data）
    uint32_t flags;        ///< 标志
    uint32_t index;        ///< IRQ 索引
    uint32_t start;        ///< 起始向量
    uint32_t count;        ///< 向量数
    uint32_t data_type;    ///< 数据类型（EVENTFD=1, NONE=0）
    uint8_t  data[1];      ///< eventfd 数组（变长）
} vfu_set_irqs_t;

/**
 * @brief VFU_REGION_READ/WRITE 请求结构（变长）
 */
typedef struct {
    uint32_t argsz;        ///< 结构大小（含 data）
    uint32_t flags;        ///< 标志
    uint32_t region_index; ///< region 索引
    uint32_t count;        ///< 读写字节数
    uint64_t offset;       ///< region 内偏移
    uint8_t  data[1];      ///< 数据（变长，写时携带数据）
} vfu_region_rw_t;

/**
 * @brief VFU_DMA_MAP 请求结构（32字节）
 * @details 通过 SCM_RIGHTS 辅助数据传递内存文件描述符
 */
typedef struct {
    uint32_t argsz;        ///< 结构大小
    uint32_t flags;        ///< 标志
    uint64_t vaddr;        ///< guest 物理地址
    uint64_t size;         ///< 区域大小
    uint64_t offset;       ///< mmap 偏移
    uint32_t prot;         ///< 保护标志（PROT_READ/PROT_WRITE）
    uint32_t reserved;     ///< 保留
} vfu_dma_map_t;

/**
 * @brief VFU_DMA_UNMAP 请求结构（24字节）
 */
typedef struct {
    uint32_t argsz;        ///< 结构大小
    uint32_t flags;        ///< 标志
    uint64_t vaddr;        ///< guest 物理地址
    uint64_t size;         ///< 区域大小
} vfu_dma_unmap_t;

/**
 * @brief VFU_INTERRUPT 消息结构（server→client，变长）
 * @details 用于主动通知 QEMU 触发中断，替代写 eventfd 方式
 */
typedef struct {
    uint32_t argsz;        ///< 结构大小（含 data）
    uint32_t flags;        ///< 标志
    uint32_t index;        ///< IRQ 类型索引
    uint32_t start;        ///< 起始向量
    uint32_t count;        ///< 向量数
    uint32_t data_type;    ///< 数据类型
    uint8_t  data[1];      ///< 数据（变长）
} vfu_interrupt_t;

/* ============================================================
 *  后端配置与上下文
 * ============================================================ */

/**
 * @brief vfio-user NVMe 后端配置
 */
typedef struct {
    char     socket_path[256];  ///< Unix domain socket 路径
    uint32_t max_queues;        ///< 最大队列数（含 Admin 队列）
} vfio_user_nvme_config_t;

/**
 * @brief vfio-user NVMe 后端上下文
 * @details 保存后端运行时全部状态，包括 socket 描述符、DMA 内存映射、
 *          MSI-X 中断状态、PCIe 配置空间镜像及 NVMe 寄存器镜像。
 */
typedef struct {
    int  socket_fd;        ///< 监听 socket 描述符
    int  conn_fd;          ///< 已连接 socket 描述符
    bool running;          ///< 后端是否运行中
    bool connected;        ///< QEMU 是否已连接

    /* DMA 内存映射 */
    vfu_dma_map_t dma_regions[VFIO_USER_MAX_DMA_REGIONS]; ///< DMA 区域信息
    void         *dma_mappings[VFIO_USER_MAX_DMA_REGIONS]; ///< mmap 虚拟地址
    int           dma_fds[VFIO_USER_MAX_DMA_REGIONS];      ///< 区域文件描述符
    uint32_t      dma_region_count;                        ///< 已映射区域数

    /* MSI-X 中断 */
    int     msix_evtfd;     ///< MSI-X eventfd（QEMU 通过 SET_IRQS 传递）
    bool    msix_enabled;   ///< MSI-X 是否已使能
    uint8_t msix_table[VFIO_MSIX_TABLE_ENTRY_SIZE]; ///< MSI-X 向量表（16字节）
    uint8_t msix_pba[8];    ///< MSI-X Pending Bit Array（8字节）

    /* PCIe 配置空间（256字节，region index 0） */
    uint8_t pci_config[VFIO_PCI_CONFIG_SIZE];

    /* NVMe 寄存器（BAR0 偏移 0x0000，4KB） */
    uint8_t nvme_regs[VFIO_BAR0_NVME_REGS_SIZE];

    /* 配置 */
    vfio_user_nvme_config_t config;
} vfio_user_nvme_ctx_t;

/* ============================================================
 *  对外接口函数
 * ============================================================ */

/**
 * @brief 初始化 vfio-user NVMe 后端
 * @details 创建 Unix domain socket，绑定并监听，初始化 PCIe 配置空间、
 *          NVMe 寄存器镜像和 MSI-X 状态。应在 NVMe 控制器初始化之后调用。
 * @param config 后端配置（socket 路径、最大队列数）
 * @retval RET_OK 初始化成功
 * @retval RET_ERR_PARAM 配置为空或参数非法
 * @retval RET_ERR_INTERNAL socket 创建/绑定/监听失败
 */
ret_code_t vfio_user_nvme_init(const vfio_user_nvme_config_t *config);

/**
 * @brief 反初始化 vfio-user NVMe 后端
 * @details 关闭连接和监听 socket，解除所有 DMA 区域 mmap 映射，
 *          关闭 MSI-X eventfd。
 * @retval RET_OK 成功
 */
ret_code_t vfio_user_nvme_deinit(void);

/**
 * @brief vfio-user NVMe 后端事件处理（单次调用，非阻塞）
 * @details 应在主循环中周期性调用。处理：
 *          1. 监听 socket：接受 QEMU 新连接
 *          2. 已连接 socket：接收并处理 vfio-user 协议消息
 *          3. 门铃触发：从 guest 内存 SQ 读取 NVMe 命令并处理
 *
 *          命令处理流程：SQ 门铃写 → 从共享内存读 nvme_command_t →
 *          调用 nvme_ctrl_process_admin_cmd/io_cmd → PRP 数据传输 →
 *          完成写入 CQ → MSI-X eventfd 发中断
 */
void vfio_user_nvme_process(void);

#ifdef __cplusplus
}
#endif

#endif /* VFIO_USER_NVME_H */
