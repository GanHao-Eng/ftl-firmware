/**
 * @file manager.h
 * @brief 管理模块
 * @details 企业级固件的管理模块，负责模块初始化、健康监控、错误处理等
 */

#ifndef FIRMWARE_MANAGER_H
#define FIRMWARE_MANAGER_H

#include "common/common.h"
#include "msg_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  模块状态定义
 * ============================================================ */

/**
 * @brief 模块状态枚举
 */
typedef enum {
    MODULE_STATE_UNINIT = 0,    ///< 未初始化
    MODULE_STATE_INIT = 1,      ///< 初始化中
    MODULE_STATE_READY = 2,     ///< 就绪
    MODULE_STATE_RUNNING = 3,   ///< 运行中
    MODULE_STATE_ERROR = 4,     ///< 错误状态
    MODULE_STATE_RESET = 5,     ///< 复位中
    MODULE_STATE_MAX = 6        ///< 状态最大值
} module_state_t;

/**
 * @brief 模块健康状态枚举
 */
typedef enum {
    HEALTH_STATUS_UNKNOWN = 0,   ///< 未知
    HEALTH_STATUS_HEALTHY = 1,   ///< 健康
    HEALTH_STATUS_WARNING = 2,   ///< 警告
    HEALTH_STATUS_CRITICAL = 3,  ///< 严重
    HEALTH_STATUS_MAX = 4        ///< 健康状态最大值
} health_status_t;

/**
 * @brief 模块信息结构体
 */
typedef struct {
    module_id_t module_id;        ///< 模块ID
    module_state_t state;         ///< 模块状态
    health_status_t health;       ///< 健康状态
    uint32_t error_count;         ///< 错误计数
    uint32_t warning_count;       ///< 警告计数
    uint64_t uptime_ms;           ///< 运行时间（毫秒）
    uint64_t last_heartbeat_ms;   ///< 上次心跳时间
    uint32_t msg_queue_count;     ///< 消息队列长度
} module_info_t;

/* ============================================================
 *  固件配置
 * ============================================================ */

/**
 * @brief 固件配置结构体
 */
typedef struct {
    uint32_t heartbeat_interval_ms;   ///< 心跳间隔（毫秒）
    uint32_t health_check_interval_ms;///< 健康检查间隔（毫秒）
    uint32_t watchdog_timeout_ms;     ///< 看门狗超时（毫秒）
    uint32_t max_error_count;         ///< 最大错误计数
    bool auto_recovery;               ///< 是否启用自动恢复
} firmware_config_t;

/* ============================================================
 *  固件统计信息
 * ============================================================ */

/**
 * @brief 固件统计结构体
 */
typedef struct {
    uint64_t total_uptime_ms;       ///< 总运行时间（毫秒）
    uint32_t total_errors;          ///< 总错误数
    uint32_t total_recoveries;      ///< 总恢复次数
    uint32_t reset_count;           ///< 复位次数
    uint32_t power_on_count;        ///< 上电次数
    health_status_t overall_health; ///< 整体健康状态
} firmware_stats_t;

/* ============================================================
 *  管理模块接口
 * ============================================================ */

/**
 * @brief 初始化管理模块
 * @param[in] config 配置指针
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_INTERNAL 内部错误
 */
ret_code_t manager_init(const firmware_config_t *config);

/**
 * @brief 反初始化管理模块
 * @retval RET_OK 成功
 */
ret_code_t manager_deinit(void);

/**
 * @brief 初始化所有模块
 * @retval RET_OK 成功
 * @retval RET_ERR_INTERNAL 内部错误
 */
ret_code_t manager_init_all_modules(void);

/**
 * @brief 启动所有模块
 * @retval RET_OK 成功
 * @retval RET_ERR_INTERNAL 内部错误
 */
ret_code_t manager_start_all_modules(void);

/**
 * @brief 停止所有模块
 * @retval RET_OK 成功
 */
ret_code_t manager_stop_all_modules(void);

/**
 * @brief 管理模块主循环
 * @details 处理健康检查、错误处理、模块监控等
 * @retval RET_OK 成功
 * @retval RET_ERR_NOT_INIT 未初始化
 */
ret_code_t manager_process(void);

/**
 * @brief 注册模块
 * @param[in] module_id 模块ID
 * @param[in] info 模块信息指针
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t manager_register_module(module_id_t module_id, const module_info_t *info);

/**
 * @brief 更新模块状态
 * @param[in] module_id 模块ID
 * @param[in] state 模块状态
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t manager_update_module_state(module_id_t module_id, module_state_t state);

/**
 * @brief 更新模块健康状态
 * @param[in] module_id 模块ID
 * @param[in] health 健康状态
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t manager_update_module_health(module_id_t module_id, health_status_t health);

/**
 * @brief 报告模块错误
 * @param[in] module_id 模块ID
 * @param[in] error_code 错误码
 * @param[in] error_msg 错误信息
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t manager_report_error(module_id_t module_id, uint32_t error_code, const char *error_msg);

/**
 * @brief 发送心跳
 * @param[in] module_id 模块ID
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t manager_send_heartbeat(module_id_t module_id);

/**
 * @brief 获取模块信息
 * @param[in] module_id 模块ID
 * @param[out] info 模块信息指针
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t manager_get_module_info(module_id_t module_id, module_info_t *info);

/**
 * @brief 获取固件统计信息
 * @param[out] stats 统计信息指针
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t manager_get_stats(firmware_stats_t *stats);

/**
 * @brief 打印所有模块状态
 */
void manager_print_module_status(void);

/**
 * @brief 打印固件统计信息
 */
void manager_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* FIRMWARE_MANAGER_H */
