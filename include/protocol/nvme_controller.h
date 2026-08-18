/**
 * @file nvme_controller.h
 * @brief NVMe 控制器协议栈头文件
 * @details 定义 NVMe 控制器的数据结构、命令/完成队列格式、
 *          Admin/I/O 操作码、状态码及控制器接口函数。
 */

#ifndef NVME_CONTROLLER_H
#define NVME_CONTROLLER_H

#include "common/common.h"
#include <stdint.h>
#include <stdbool.h>

/* ============================================================
 *  常量定义
 * ============================================================ */

/** @brief NVMe 版本号 1.4 */
#define NVME_VERSION        0x00010400U

/** @brief Admin 队列大小（条目数） */
#define NVME_ADMIN_QSIZE    256U

/** @brief 最大 I/O 队列数 */
#define NVME_MAX_IO_QUEUES  4U

/* ============================================================
 *  控制器寄存器偏移（BAR0 空间）
 * ============================================================ */
#define NVME_REG_CAP        0x00U  ///< Controller Capabilities
#define NVME_REG_VS         0x08U  ///< Version
#define NVME_REG_INTMS      0x0CU  ///< Interrupt Mask Set
#define NVME_REG_INTMC      0x10U  ///< Interrupt Mask Clear
#define NVME_REG_CC         0x14U  ///< Controller Configuration
#define NVME_REG_CSTS       0x1CU  ///< Controller Status
#define NVME_REG_NSSR       0x20U  ///< NVM Subsystem Reset
#define NVME_REG_AQA        0x24U  ///< Admin Queue Attributes
#define NVME_REG_ASQ        0x28U  ///< Admin SQ Base Address
#define NVME_REG_ACQ        0x30U  ///< Admin CQ Base Address

/* ============================================================
 *  Admin 命令操作码
 * ============================================================ */
#define NVME_ADMIN_DELETE_IOSQ       0x00U
#define NVME_ADMIN_CREATE_IOSQ       0x01U
#define NVME_ADMIN_GET_LOG_PAGE      0x02U
#define NVME_ADMIN_DELETE_IOCQ       0x04U
#define NVME_ADMIN_CREATE_IOCQ       0x05U
#define NVME_ADMIN_IDENTIFY          0x06U
#define NVME_ADMIN_ABORT             0x08U
#define NVME_ADMIN_SET_FEATURES      0x09U
#define NVME_ADMIN_GET_FEATURES      0x0AU
#define NVME_ADMIN_ASYNC_EVENT       0x0CU
#define NVME_ADMIN_FW_DOWNLOAD       0x11U
#define NVME_ADMIN_FW_ACTIVATE       0x10U
#define NVME_ADMIN_FORMAT_NVM        0x80U
#define NVME_ADMIN_KEEP_ALIVE        0x18U

/* ============================================================
 *  I/O 命令操作码
 * ============================================================ */
#define NVME_IO_FLUSH                0x00U
#define NVME_IO_WRITE                0x01U
#define NVME_IO_READ                 0x02U
#define NVME_IO_WRITE_UNCORRECTABLE  0x04U
#define NVME_IO_COMPARE              0x05U
#define NVME_IO_WRITE_ZEROES         0x08U
#define NVME_IO_DATASET_MGMT         0x09U
#define NVME_IO_VERIFY               0x0CU

/* ============================================================
 *  状态码
 * ============================================================ */
#define NVME_SC_SUCCESS              0x0000U
#define NVME_SC_INVALID_OPCODE       0x0001U
#define NVME_SC_INVALID_FIELD        0x0002U
#define NVME_SC_CMD_ID_CONFLICT      0x0003U
#define NVME_SC_DATA_XFER_ERROR      0x0004U
#define NVME_SC_ABORTED              0x0005U
#define NVME_SC_INTERNAL_ERROR       0x0006U
#define NVME_SC_CMD_ABORT_REQ        0x0007U
#define NVME_SC_INVALID_NAMESPACE    0x000BU
#define NVME_SC_LBA_OUT_OF_RANGE     0x0009U
#define NVME_SC_CAP_EXCEEDED         0x0008U

/* ============================================================
 *  数据结构定义
 * ============================================================ */

/**
 * @brief NVMe 命令结构体（64字节，Submission Queue Entry）
 * @details 按照 NVMe 1.4 规范定义的通用命令格式
 */
typedef struct {
    uint8_t  opcode;        ///< 操作码
    uint8_t  flags;         ///< 标志（PSDT, FUSE 等）
    uint16_t cid;           ///< 命令 ID
    uint32_t nsid;          ///< 命名空间 ID
    uint32_t reserved1[2];  ///< 保留
    uint64_t mptr;          ///< 元数据指针
    uint64_t dptr_prp1;     ///< PRP Entry 1 / SGL 段 1
    uint64_t dptr_prp2;     ///< PRP Entry 2 / SGL 段 2
    uint32_t cdw10;         ///< 命令双字 10
    uint32_t cdw11;         ///< 命令双字 11
    uint32_t cdw12;         ///< 命令双字 12
    uint32_t cdw13;         ///< 命令双字 13
    uint32_t cdw14;         ///< 命令双字 14
    uint32_t cdw15;         ///< 命令双字 15
} nvme_command_t;

/**
 * @brief NVMe 完成队列项（16字节，Completion Queue Entry）
 * @details 按照 NVMe 1.4 规范定义
 */
typedef struct {
    uint32_t dw0;           ///< 命令特定返回值
    uint32_t rsvd1;         ///< 保留
    uint16_t sqhd;          ///< SQ 头指针
    uint16_t sqid;          ///< SQ ID
    uint16_t cid;           ///< 命令 ID
    uint16_t status;        ///< 状态字段（含相位位）
} nvme_completion_t;

/**
 * @brief NVMe 控制器状态枚举
 */
typedef enum {
    NVME_CTRL_STATE_RESET = 0,  ///< 复位状态
    NVME_CTRL_STATE_READY = 1,  ///< 就绪状态
    NVME_CTRL_STATE_ERROR = 2   ///< 错误状态
} nvme_ctrl_state_t;

/**
 * @brief NVMe 控制器寄存器结构体
 * @details 对应 BAR0 空间的控制器寄存器
 */
typedef struct {
    uint64_t cap;           ///< 控制器能力
    uint32_t vs;            ///< 版本
    uint32_t intms;         ///< 中断掩码集
    uint32_t intmc;         ///< 中断掩码清除
    uint32_t cc;            ///< 控制器配置
    uint32_t reserved1;
    uint32_t csts;          ///< 控制器状态
    uint32_t nssr;          ///< NVM 子系统复位
    uint32_t aqa;           ///< Admin 队列属性
    uint64_t asq;           ///< Admin SQ 地址
    uint64_t acq;           ///< Admin CQ 地址
} nvme_ctrl_regs_t;

/**
 * @brief NVMe 提交队列（SQ）
 */
typedef struct {
    uint16_t qid;           ///< 队列 ID
    uint16_t size;          ///< 队列大小（条目数）
    uint16_t head;          ///< 头指针（控制器侧）
    uint16_t tail;          ///< 尾指针（主机侧）
    uint16_t cqid;          ///< 关联的 CQ ID
    bool     is_admin;      ///< 是否为 Admin 队列
    nvme_command_t *entries;///< 队列条目数组
} nvme_sq_t;

/**
 * @brief NVMe 完成队列（CQ）
 */
typedef struct {
    uint16_t qid;           ///< 队列 ID
    uint16_t size;          ///< 队列大小（条目数）
    uint16_t head;          ///< 头指针（主机侧）
    uint16_t tail;          ///< 尾指针（控制器侧）
    uint16_t vector;        ///< 中断向量
    bool     phase;         ///< 相位位
    bool     is_admin;      ///< 是否为 Admin 队列
    nvme_completion_t *entries; ///< 队列条目数组
} nvme_cq_t;

/* ============================================================
 *  函数接口
 * ============================================================ */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 NVMe 控制器
 * @retval RET_OK 成功
 * @retval RET_ERR_INTERNAL 内部错误
 */
ret_code_t nvme_ctrl_init(void);

/**
 * @brief 反初始化 NVMe 控制器
 * @retval RET_OK 成功
 */
ret_code_t nvme_ctrl_deinit(void);

/**
 * @brief 写控制器配置寄存器（CC）
 * @param value 寄存器值
 */
void nvme_ctrl_write_cc(uint32_t value);

/**
 * @brief 写 Admin 队列属性寄存器（AQA）
 * @param value 寄存器值
 */
void nvme_ctrl_write_aqa(uint32_t value);

/**
 * @brief 写 Admin SQ 地址寄存器（ASQ）
 * @param value 寄存器值
 * @param is_high 是否为高 32 位
 */
void nvme_ctrl_write_asq(uint32_t value, bool is_high);

/**
 * @brief 写 Admin CQ 地址寄存器（ACQ）
 * @param value 寄存器值
 * @param is_high 是否为高 32 位
 */
void nvme_ctrl_write_acq(uint32_t value, bool is_high);

/**
 * @brief 写 SQ 门铃寄存器
 * @param qid 队列 ID
 * @param value 尾指针值
 */
void nvme_ctrl_sq_doorbell(uint16_t qid, uint32_t value);

/**
 * @brief 写 CQ 门铃寄存器
 * @param qid 队列 ID
 * @param value 头指针值
 */
void nvme_ctrl_cq_doorbell(uint16_t qid, uint32_t value);

/**
 * @brief 处理 Admin 命令
 * @param cmd 命令
 * @param cpl 完成条目输出
 * @retval RET_OK 成功
 */
ret_code_t nvme_ctrl_process_admin_cmd(const nvme_command_t *cmd,
                                       nvme_completion_t *cpl);

/**
 * @brief 处理 I/O 命令
 * @param cmd 命令
 * @param cpl 完成条目输出
 * @retval RET_OK 成功
 */
ret_code_t nvme_ctrl_process_io_cmd(const nvme_command_t *cmd,
                                    nvme_completion_t *cpl);

/**
 * @brief 处理控制器事件（从 SQ 取命令并执行）
 */
void nvme_ctrl_process(void);

/**
 * @brief 获取控制器状态
 * @return 控制器状态
 */
nvme_ctrl_state_t nvme_ctrl_get_state(void);

/**
 * @brief 获取控制器寄存器指针
 * @return 寄存器结构体指针
 */
nvme_ctrl_regs_t *nvme_ctrl_get_regs(void);

/**
 * @brief 填充 SMART/Health 日志
 * @param buf 输出缓冲区
 * @param len 缓冲区长度
 */
void nvme_ctrl_fill_smart_log(uint8_t *buf, uint32_t len);

/**
 * @brief 填充 Identify Namespace 数据
 * @param buf 输出缓冲区
 * @param len 缓冲区长度
 * @param nsid 命名空间 ID
 */
void nvme_ctrl_fill_identify_namespace(uint8_t *buf, uint32_t len, uint32_t nsid);

/**
 * @brief 填充 Identify Controller 数据
 * @param buf 输出缓冲区
 * @param len 缓冲区长度
 */
void nvme_ctrl_fill_identify_controller(uint8_t *buf, uint32_t len);

/**
 * @brief 写控制器寄存器（偏移方式）
 * @param offset 寄存器偏移
 * @param value 写入值
 */
void nvme_ctrl_write_reg(uint32_t offset, uint32_t value);

/**
 * @brief 触发中断
 * @param vector 中断向量
 */
void nvme_ctrl_trigger_irq(uint16_t vector);

#ifdef __cplusplus
}
#endif

#endif /* NVME_CONTROLLER_H */
