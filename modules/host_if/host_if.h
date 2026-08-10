/**
 * @file host_if.h
 * @brief 主机接口模块
 * @details 企业级固件的主机接口模块，模拟 NVMe 协议接口
 */

#ifndef FIRMWARE_HOST_IF_H
#define FIRMWARE_HOST_IF_H

#include "common/common.h"
#include "msg_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  NVMe 命令定义
 * ============================================================ */

/**
 * @brief NVMe 命令操作码
 */
typedef enum {
    NVME_CMD_FLUSH = 0x00,          ///< Flush 命令
    NVME_CMD_WRITE = 0x01,          ///< Write 命令
    NVME_CMD_READ = 0x02,           ///< Read 命令
    NVME_CMD_WRITE_UNCOR = 0x04,    ///< Write Uncorrectable 命令
    NVME_CMD_COMPARE = 0x05,        ///< Compare 命令
    NVME_CMD_WRITE_ZEROES = 0x08,   ///< Write Zeroes 命令
    NVME_CMD_DATASET_MGMT = 0x09,   ///< Dataset Management (TRIM) 命令
    NVME_CMD_VERIFY = 0x0C,         ///< Verify 命令
    NVME_CMD_MAX = 0x10             ///< 命令码最大值
} nvme_opcode_t;

/**
 * @brief NVMe 命令状态
 */
typedef enum {
    NVME_STATUS_SUCCESS = 0x00,         ///< 成功
    NVME_STATUS_INVALID_OPCODE = 0x01,  ///< 无效操作码
    NVME_STATUS_INVALID_FIELD = 0x02,   ///< 无效字段
    NVME_STATUS_DATA_TRANSFER = 0x03,   ///< 数据传输错误
    NVME_STATUS_ABORTED = 0x04,         ///< 命令已中止
    NVME_STATUS_INTERNAL_ERROR = 0x05,  ///< 内部错误
    NVME_STATUS_MAX = 0x10              ///< 状态码最大值
} nvme_status_t;

/**
 * @brief NVMe 命令结构体
 */
typedef struct {
    nvme_opcode_t opcode;     ///< 操作码
    uint32_t nsid;            ///< 命名空间ID
    uint64_t slba;            ///< 起始逻辑块地址
    uint32_t nlb;             ///< 逻辑块数量（0 表示 1 个）
    uint16_t cid;             ///< 命令ID
    uint8_t fua;              ///< 强制单元访问
    uint8_t lr;               ///< 受限重试
    uint8_t *data_buf;        ///< 数据缓冲区指针
    uint32_t data_len;        ///< 数据长度
} nvme_cmd_t;

/**
 * @brief NVMe 完成队列条目
 */
typedef struct {
    uint32_t dw0;             ///< 命令特定信息
    uint32_t reserved;        ///< 保留
    uint16_t sqhd;            ///< 提交队列头指针
    uint16_t sqid;            ///< 提交队列ID
    uint16_t cid;             ///< 命令ID
    nvme_status_t status;     ///< 状态码
    uint8_t phase_tag;        ///< 阶段标签
} nvme_cqe_t;

/* ============================================================
 *  主机接口配置
 * ============================================================ */

/**
 * @brief 主机接口配置结构体
 */
typedef struct {
    uint32_t queue_size;      ///< 命令队列大小
    uint32_t max_cmd;         ///< 最大并发命令数
    uint32_t lba_size;        ///< LBA 大小（字节）
    uint64_t total_lbas;      ///< 总 LBA 数量
    bool is_nvm;              ///< 是否为 NVM 命名空间
} host_if_config_t;

/* ============================================================
 *  主机接口统计信息
 * ============================================================ */

/**
 * @brief 主机接口统计结构体
 */
typedef struct {
    uint64_t total_cmds;        ///< 总命令数
    uint64_t read_cmds;         ///< 读命令数
    uint64_t write_cmds;        ///< 写命令数
    uint64_t trim_cmds;         ///< TRIM 命令数
    uint64_t completed_cmds;    ///< 已完成命令数
    uint64_t failed_cmds;       ///< 失败命令数
    uint64_t total_read_bytes;  ///< 总读取字节数
    uint64_t total_write_bytes; ///< 总写入字节数
    uint32_t avg_latency_us;    ///< 平均延迟（微秒）
} host_if_stats_t;

/**
 * @brief 性能统计结构体
 */
typedef struct {
    /* IOPS 统计 */
    uint64_t read_iops;         ///< 读 IOPS
    uint64_t write_iops;        ///< 写 IOPS
    uint64_t total_iops;        ///< 总 IOPS

    /* 带宽统计 */
    uint64_t read_bw_bps;       ///< 读带宽（字节/秒）
    uint64_t write_bw_bps;      ///< 写带宽（字节/秒）
    uint64_t total_bw_bps;      ///< 总带宽（字节/秒）

    /* 延迟统计 */
    uint64_t min_latency_us;    ///< 最小延迟（微秒）
    uint64_t max_latency_us;    ///< 最大延迟（微秒）
    uint64_t avg_latency_us;    ///< 平均延迟（微秒）
    uint64_t total_latency_us;  ///< 总延迟（微秒）
    uint64_t latency_count;     ///< 延迟统计次数

    /* 时间窗口统计 */
    uint64_t window_start_ms;   ///< 统计窗口开始时间
    uint64_t window_read_cmds;  ///< 窗口内读命令数
    uint64_t window_write_cmds; ///< 窗口内写命令数
    uint64_t window_read_bytes; ///< 窗口内读字节数
    uint64_t window_write_bytes;///< 窗口内写字节数
} performance_stats_t;

/* ============================================================
 *  主机接口
 * ============================================================ */

/**
 * @brief 初始化主机接口模块
 * @param[in] config 配置指针
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_INTERNAL 内部错误
 */
ret_code_t host_if_init(const host_if_config_t *config);

/**
 * @brief 反初始化主机接口模块
 * @retval RET_OK 成功
 */
ret_code_t host_if_deinit(void);

/**
 * @brief 提交 NVMe 命令
 * @param[in] cmd 命令指针
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NO_SPACE 队列已满
 */
ret_code_t host_if_submit_cmd(const nvme_cmd_t *cmd);

/**
 * @brief 轮询完成队列
 * @param[out] cqe 完成队列条目指针
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_BUSY 队列为空
 */
ret_code_t host_if_poll_cq(nvme_cqe_t *cqe);

/**
 * @brief 主机接口主循环处理
 * @details 处理接收到的命令，转发给 FTL 模块
 * @retval RET_OK 成功
 * @retval RET_ERR_NOT_INIT 未初始化
 */
ret_code_t host_if_process(void);

/**
 * @brief 获取主机接口统计信息
 * @param[out] stats 统计信息指针
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t host_if_get_stats(host_if_stats_t *stats);

/**
 * @brief 重置主机接口统计信息
 * @retval RET_OK 成功
 */
ret_code_t host_if_reset_stats(void);

/**
 * @brief 打印主机接口统计信息
 */
void host_if_print_stats(void);

/* ============================================================
 *  性能监控接口
 * ============================================================ */

/**
 * @brief 获取性能统计信息
 * @param[out] stats 性能统计信息指针
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_INIT 未初始化
 * @note 性能统计包括 IOPS、带宽、延迟等指标
 */
ret_code_t host_if_get_performance_stats(performance_stats_t *stats);

/**
 * @brief 重置性能统计信息
 * @retval RET_OK 成功
 * @retval RET_ERR_NOT_INIT 未初始化
 */
ret_code_t host_if_reset_performance_stats(void);

/**
 * @brief 打印性能统计信息
 */
void host_if_print_performance_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* FIRMWARE_HOST_IF_H */
