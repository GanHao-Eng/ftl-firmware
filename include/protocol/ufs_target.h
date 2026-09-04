/**
 * @file ufs_target.h
 * @brief UFS（Universal Flash Storage）目标端简化实现头文件
 * @details 实现UFS基本命令集框架，用于展示UFS协议架构理解
 *          UFS基于SCSI命令集，通过MMIO寄存器和命令队列与主机通信
 * @note UFS协议栈层次：
 *       - 应用层：SCSI命令集（INQUIRY/READ/WRITE等）
 *       - 传输层：UPIU（UFS Protocol Information Unit）
 *       - 链路层：UniPro（MIPI）
 *       - 物理层：MIPI M-PHY
 *       当前实现应用层和传输层框架，链路层和物理层由硬件实现
 */
#ifndef UFS_TARGET_H
#define UFS_TARGET_H

#include <stdint.h>
#include <stdbool.h>
#include "common/common.h"

/* ============================================================
 *  UFS 常量定义
 * ============================================================ */

#define UFS_MAX_LUNS          8U     ///< 最大逻辑单元数
#define UFS_MAX_CMD_QUEUE     32U    ///< 最大命令队列深度
#define UFS_SECTOR_SIZE       512U   ///< 扇区大小（UFS默认512字节）
#define UFS_MAX_RETRY_COUNT   3U     ///< 命令最大重试次数
#define UFS_TEMP_WARNING_THRESHOLD  70  ///< 温度告警阈值（摄氏度）
#define UFS_TEMP_CRITICAL_THRESHOLD 85  ///< 温度严重阈值（摄氏度）
#define UFS_LIFETIME_WARNING_PERCENT 80 ///< 寿命告警阈值（百分比）

/* UFS 命令状态码 */
#define UFS_STATUS_GOOD           0x00U  ///< 命令成功完成
#define UFS_STATUS_CHECK_CONDITION 0x02U  ///< 需要检查请求感知数据
#define UFS_STATUS_BUSY           0x08U  ///< 设备忙

/* ============================================================
 *  SCSI 命令操作码（UFS应用层使用SCSI命令集）
 * ============================================================ */

#define SCSI_OP_TEST_UNIT_READY  0x00U  ///< 测试单元就绪
#define SCSI_OP_INQUIRY           0x12U  ///< 查询设备信息
#define SCSI_OP_READ_10           0x28U  ///< 读（10字节CDB）
#define SCSI_OP_WRITE_10          0x2AU  ///< 写（10字节CDB）
#define SCSI_OP_READ_CAPACITY_10  0x25U  ///< 读容量（10字节）
#define SCSI_OP_SYNCHRONIZE_CACHE 0x35U  ///< 同步缓存（Flush）
#define SCSI_OP_UNMAP             0x42U  ///< 取消映射（TRIM）
#define SCSI_OP_MODE_SENSE_6      0x1AU  ///< 模式感知（6字节）
#define SCSI_OP_MODE_SELECT_6     0x15U  ///< 模式选择（6字节）
#define SCSI_OP_LOG_SENSE         0x4DU  ///< 日志感知
#define SCSI_OP_START_STOP_UNIT   0x1BU  ///< 启动/停止单元（电源管理）
#define SCSI_OP_PREVENT_ALLOW_MEDIUM_REMOVAL 0x1EU  ///< 禁止/允许介质移除（写保护）
#define SCSI_OP_SECURITY_PROTOCOL_IN  0xA2U  ///< 安全协议输入
#define SCSI_OP_SECURITY_PROTOCOL_OUT 0xB5U  ///< 安全协议输出

/* ============================================================
 *  UPIU 事务类型
 * ============================================================ */

typedef enum {
    UPIU_TYPE_NOP_OUT         = 0x00,  ///< NOP输出
    UPIU_TYPE_COMMAND         = 0x01,  ///< 命令
    UPIU_TYPE_DATA_OUT        = 0x02,  ///< 数据输出（主机到设备）
    UPIU_TYPE_TASK_REQUEST    = 0x04,  ///< 任务管理请求
    UPIU_TYPE_QUERY_REQUEST   = 0x16,  ///< 查询请求
    UPIU_TYPE_NOP_IN          = 0x20,  ///< NOP输入
    UPIU_TYPE_RESPONSE        = 0x21,  ///< 响应
    UPIU_TYPE_DATA_IN         = 0x22,  ///< 数据输入（设备到主机）
    UPIU_TYPE_TASK_RESPONSE   = 0x24,  ///< 任务管理响应
    UPIU_TYPE_QUERY_RESPONSE  = 0x36,  ///< 查询响应
} upiu_type_t;

/* ============================================================
 *  UFS 命令描述符（CDB - Command Descriptor Block）
 * ============================================================ */

/**
 * @brief SCSI CDB（10字节命令）
 */
typedef struct {
    uint8_t  opcode;          ///< 操作码
    uint8_t  flags;           ///< 标志位
    uint32_t lba;             ///< 逻辑块地址（大端）
    uint8_t  group_number;    ///< 组号
    uint16_t transfer_length; ///< 传输长度（大端，扇区数）
    uint8_t  control;         ///< 控制字节
} scsi_cdb_10_t;

/* ============================================================
 *  UPIU 数据包结构
 * ============================================================ */

/**
 * @brief UPIU 基本头（32字节）
 */
typedef struct {
    uint8_t  trans_type;      ///< 事务类型（upiu_type_t）
    uint8_t  flags;           ///< 标志位
    uint8_t  lun;             ///< 逻辑单元号
    uint8_t  task_id;         ///< 任务ID
    uint8_t  iid;             ///< 发起者ID
    uint8_t  rsvd1[3];        ///< 保留
    uint32_t data_segment_len;///< 数据段长度（大端）
    uint8_t  rsvd2[8];        ///< 保留
    uint16_t rsvd3;           ///< 保留
    uint8_t  scsi_status;     ///< SCSI状态码
    uint8_t  service_code;    ///< 服务码
} upiu_header_t;

/**
 * @brief UFS 命令请求（命令UPIU + CDB）
 */
typedef struct {
    upiu_header_t header;     ///< UPIU头
    uint8_t  cdb[16];         ///< SCSI CDB（最大16字节）
} ufs_cmd_request_t;

/**
 * @brief UFS 响应（响应UPIU + 感知数据）
 */
typedef struct {
    upiu_header_t header;     ///< UPIU头
    uint8_t  sense_data[18];  ///< 请求感知数据（SCSI Sense Data）
} ufs_cmd_response_t;

/* ============================================================
 *  UFS 设备信息
 * ============================================================ */

/**
 * @brief UFS 设备标识信息（INQUIRY命令返回）
 */
typedef struct {
    uint8_t  peripheral_type;  ///< 外设类型（0=直接访问设备）
    uint8_t  rmb;              ///< 可移动介质标志
    uint8_t  version;          ///< SPC版本
    uint8_t  response_format;  ///< 响应格式
    uint8_t  additional_length;///< 附加长度
    uint8_t  rsvd[3];          ///< 保留
    char     vendor[8];        ///< 厂商ID
    char     product[16];      ///< 产品ID
    char     revision[4];      ///< 版本号
} ufs_inquiry_data_t;

/* ============================================================
 *  UFS 系统特性数据结构
 * ============================================================ */

/**
 * @brief UFS电源模式枚举
 */
typedef enum {
    UFS_POWER_MODE_ACTIVE   = 0,  ///< 活跃模式（正常工作）
    UFS_POWER_MODE_IDLE     = 1,  ///< 空闲模式（低功耗，快速唤醒）
    UFS_POWER_MODE_SLEEP    = 2,  ///< 休眠模式（最低功耗，唤醒延迟较高）
    UFS_POWER_MODE_POWER_DOWN = 3, ///< 掉电模式（完全关闭）
} ufs_power_mode_t;

/**
 * @brief UFS健康状态信息
 */
typedef struct {
    int32_t  temperature;          ///< 当前温度（摄氏度）
    uint32_t lifetime_used_percent;///< 已用寿命百分比（0-100）
    uint32_t total_erase_count;    ///< 总擦除次数
    uint32_t power_on_count;       ///< 上电次数
    uint32_t power_on_minutes;     ///< 累计上电时间（分钟）
    uint32_t unsafe_shutdown_count;///< 非正常关机次数
    bool     temperature_warning;   ///< 温度告警标志
    bool     lifetime_warning;      ///< 寿命告警标志
    bool     write_protected;       ///< 写保护标志
} ufs_health_info_t;

/**
 * @brief UFS错误统计信息
 */
typedef struct {
    uint32_t total_cmd_count;       ///< 总命令数
    uint32_t success_count;         ///< 成功完成数
    uint32_t retry_count;           ///< 重试次数
    uint32_t failure_count;         ///< 失败次数
    uint32_t media_error_count;     ///< 介质错误数
    uint32_t timeout_error_count;   ///< 超时错误数
} ufs_error_stats_t;

/**
 * @brief UFS命令队列条目
 */
typedef struct {
    bool     valid;                  ///< 条目有效标志
    uint8_t  task_id;               ///< 任务ID
    uint8_t  retry_count;           ///< 已重试次数
    ufs_cmd_request_t request;      ///< 命令请求
} ufs_cmd_queue_entry_t;

/* ============================================================
 *  UFS 目标端接口
 * ============================================================ */

/**
 * @brief 初始化UFS目标端
 * @retval RET_OK 初始化成功
 * @retval RET_ERR_INTERNAL 初始化失败
 */
ret_code_t ufs_target_init(void);

/**
 * @brief 反初始化UFS目标端
 */
void ufs_target_deinit(void);

/**
 * @brief 处理UFS命令请求
 * @param[in]  request  命令请求
 * @param[out] response 命令响应
 * @param[in,out] data  数据缓冲区（读命令填充数据，写命令读取数据）
 * @param[in]  data_len 数据缓冲区长度
 * @retval RET_OK 处理成功
 * @retval RET_ERR_PARAM 参数非法
 */
ret_code_t ufs_target_process_cmd(const ufs_cmd_request_t *request,
                                   ufs_cmd_response_t *response,
                                   uint8_t *data, uint32_t data_len);

/**
 * @brief 带重试的命令处理
 * @details 命令失败时自动重试，最多UFS_MAX_RETRY_COUNT次
 * @param[in]  request  命令请求
 * @param[out] response 命令响应
 * @param[in,out] data  数据缓冲区
 * @param[in]  data_len 数据缓冲区长度
 * @retval RET_OK 处理成功
 */
ret_code_t ufs_target_process_cmd_with_retry(const ufs_cmd_request_t *request,
                                              ufs_cmd_response_t *response,
                                              uint8_t *data, uint32_t data_len);

/**
 * @brief 获取UFS设备容量
 * @param[out] total_sectors 总扇区数
 * @param[out] sector_size   扇区大小（字节）
 * @retval RET_OK 成功
 */
ret_code_t ufs_target_get_capacity(uint64_t *total_sectors, uint32_t *sector_size);

/* ============================================================
 *  UFS 系统特性接口
 * ============================================================ */

/**
 * @brief 设置UFS电源模式
 * @param[in] mode 电源模式
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 */
ret_code_t ufs_target_set_power_mode(ufs_power_mode_t mode);

/**
 * @brief 获取当前电源模式
 * @return 当前电源模式
 */
ufs_power_mode_t ufs_target_get_power_mode(void);

/**
 * @brief 获取UFS健康状态信息
 * @param[out] info 健康状态信息
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 */
ret_code_t ufs_target_get_health_info(ufs_health_info_t *info);

/**
 * @brief 获取UFS错误统计信息
 * @param[out] stats 错误统计信息
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 */
ret_code_t ufs_target_get_error_stats(ufs_error_stats_t *stats);

/**
 * @brief 重置错误统计信息
 * @retval RET_OK 成功
 */
ret_code_t ufs_target_reset_error_stats(void);

/**
 * @brief 设置写保护状态
 * @param[in] enable true=启用写保护, false=禁用写保护
 * @retval RET_OK 成功
 */
ret_code_t ufs_target_set_write_protect(bool enable);

/**
 * @brief 获取写保护状态
 * @return true=写保护启用, false=写保护禁用
 */
bool ufs_target_get_write_protect(void);

/**
 * @brief 后台操作触发（BKOPS - Background Operations）
 * @details 触发垃圾回收、磨损均衡等后台操作
 * @retval RET_OK 成功
 */
ret_code_t ufs_target_trigger_background_ops(void);

/**
 * @brief UFS健康监控周期处理
 * @details 应定期调用，更新温度、寿命等健康指标
 * @retval RET_OK 成功
 */
ret_code_t ufs_target_health_monitor_process(void);

#endif /* UFS_TARGET_H */
