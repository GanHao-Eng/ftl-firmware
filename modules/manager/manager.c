/**
 * @file manager.c
 * @brief 管理模块实现
 * @details 企业级固件的管理模块实现，负责模块初始化、健康监控、错误处理等
 */

#include "manager.h"
#include <string.h>
#include <stdio.h>

/* ============================================================
 *  内部数据结构
 * ============================================================ */

/**
 * @brief 管理模块私有数据
 */
typedef struct {
    firmware_config_t config;        ///< 配置
    firmware_stats_t stats;          ///< 统计信息
    module_info_t modules[MODULE_MAX]; ///< 模块信息
    bool is_initialized;             ///< 初始化标志
    bool is_running;                 ///< 运行标志
} manager_dev_t;

/* ============================================================
 *  全局变量
 * ============================================================ */

static manager_dev_t g_manager;  ///< 管理模块设备

/* ============================================================
 *  内部函数
 * ============================================================ */

/**
 * @brief 获取当前时间戳（毫秒）
 * @return 时间戳
 */
static uint64_t get_timestamp_ms(void)
{
    /* 简化实现，实际固件中使用硬件定时器 */
    static uint64_t counter = 0;
    return counter++;
}

/**
 * @brief 检查模块健康状态
 * @param[in] module_id 模块ID
 * @return 健康状态
 * @note 根据错误计数和心跳状态综合判断健康状态
 *       - 0 错误 = 健康
 *       - 错误数 < 阈值 = 警告
 *       - 错误数 >= 阈值 或 心跳超时 = 严重
 */
static health_status_t check_module_health(module_id_t module_id)
{
    module_info_t *info = NULL;
    uint64_t now = 0;
    uint64_t heartbeat_age = 0;

    if (module_id >= MODULE_MAX) {
        return HEALTH_STATUS_UNKNOWN;
    }

    info = &g_manager.modules[module_id];
    now = get_timestamp_ms();

    /* 计算心跳年龄（距上次心跳的时间） */
    if (info->last_heartbeat_ms <= now) {
        heartbeat_age = now - info->last_heartbeat_ms;
    } else {
        heartbeat_age = 0;
    }

    /* 心跳超时，直接判定为严重 */
    if (heartbeat_age > g_manager.config.watchdog_timeout_ms) {
        return HEALTH_STATUS_CRITICAL;
    }

    /* 根据错误计数判断健康状态 */
    if (info->error_count == 0U) {
        return HEALTH_STATUS_HEALTHY;
    } else if (info->error_count < g_manager.config.max_error_count) {
        return HEALTH_STATUS_WARNING;
    } else {
        return HEALTH_STATUS_CRITICAL;
    }
}

/**
 * @brief 计算整体健康状态
 * @return 整体健康状态
 */
static health_status_t calc_overall_health(void)
{
    uint32_t i = 0;
    health_status_t overall = HEALTH_STATUS_HEALTHY;

    for (i = 0; i < MODULE_MAX; i++) {
        if (g_manager.modules[i].state == MODULE_STATE_UNINIT) {
            continue;
        }

        if (g_manager.modules[i].health > overall) {
            overall = g_manager.modules[i].health;
        }
    }

    return overall;
}

/**
 * @brief 自动恢复模块
 * @param[in] module_id 模块ID
 * @retval RET_OK 成功
 * @retval RET_ERR_INTERNAL 内部错误
 */
static ret_code_t auto_recover_module(module_id_t module_id)
{
    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }

    /* 简化实现：重置模块状态 */
    g_manager.modules[module_id].state = MODULE_STATE_RESET;
    g_manager.modules[module_id].error_count = 0U;
    g_manager.modules[module_id].health = HEALTH_STATUS_HEALTHY;
    g_manager.modules[module_id].state = MODULE_STATE_READY;

    g_manager.stats.total_recoveries++;

    return RET_OK;
}

/**
 * @brief 处理错误报告
 * @param[in] module_id 模块ID
 * @param[in] error_code 错误码
 * @param[in] error_msg 错误信息
 */
static void process_error_report(module_id_t module_id, uint32_t error_code, const char *error_msg)
{
    module_info_t *info = NULL;

    if (module_id >= MODULE_MAX) {
        return;
    }

    info = &g_manager.modules[module_id];

    /* 更新错误计数 */
    info->error_count++;
    g_manager.stats.total_errors++;

    /* 更新健康状态 */
    info->health = check_module_health(module_id);

    /* 检查是否需要自动恢复 */
    if (g_manager.config.auto_recovery &&
        info->error_count >= g_manager.config.max_error_count) {
        printf("[管理模块] 模块 %u 错误过多，触发自动恢复\n", module_id);
        auto_recover_module(module_id);
    }

    /* 打印错误信息 */
    if (error_msg != NULL) {
        printf("[管理模块] 模块 %u 错误: 码=%u, 信息=%s\n",
               module_id, error_code, error_msg);
    }
}

/**
 * @brief 处理健康检查
 * @details 执行周期性健康检查，包括：
 *          - 更新模块运行时间
 *          - 更新消息队列长度
 *          - 检测心跳超时（看门狗）
 *          - 更新整体健康状态
 *          - 更新总运行时间
 */
static void process_health_check(void)
{
    uint32_t i = 0;
    uint64_t now = 0;
    uint64_t heartbeat_age = 0;

    now = get_timestamp_ms();

    /* 遍历所有已初始化的模块 */
    for (i = 0; i < MODULE_MAX; i++) {
        if (g_manager.modules[i].state == MODULE_STATE_UNINIT) {
            continue;
        }

        /* 更新运行时间（仅对运行中的模块） */
        if (g_manager.modules[i].state == MODULE_STATE_RUNNING) {
            g_manager.modules[i].uptime_ms++;
        }

        /* 更新消息队列长度 */
        g_manager.modules[i].msg_queue_count = msg_queue_get_count((module_id_t)i);

        /* 看门狗检测：检查心跳是否超时 */
        if (g_manager.modules[i].state == MODULE_STATE_RUNNING) {
            /* 计算心跳年龄 */
            if (g_manager.modules[i].last_heartbeat_ms <= now) {
                heartbeat_age = now - g_manager.modules[i].last_heartbeat_ms;
            } else {
                heartbeat_age = 0;
            }

            /* 心跳超时，触发错误 */
            if (heartbeat_age > g_manager.config.watchdog_timeout_ms) {
                printf("[管理模块] 模块 %u 心跳超时，触发错误\n", i);
                process_error_report((module_id_t)i, 0xFF, "心跳超时");
            }
        }
    }

    /* 更新整体健康状态 */
    g_manager.stats.overall_health = calc_overall_health();

    /* 更新总运行时间 */
    g_manager.stats.total_uptime_ms++;
}

/* ============================================================
 *  接口实现
 * ============================================================ */
ret_code_t manager_init(const firmware_config_t *config)
{
    uint32_t i = 0;

    if (config == NULL) {
        return RET_ERR_PARAM;
    }

    memset(&g_manager, 0, sizeof(g_manager));

    /* 保存配置 */
    memcpy(&g_manager.config, config, sizeof(firmware_config_t));

    /* 初始化模块信息 */
    for (i = 0; i < MODULE_MAX; i++) {
        g_manager.modules[i].module_id = (module_id_t)i;
        g_manager.modules[i].state = MODULE_STATE_UNINIT;
        g_manager.modules[i].health = HEALTH_STATUS_UNKNOWN;
        g_manager.modules[i].error_count = 0U;
        g_manager.modules[i].warning_count = 0U;
        g_manager.modules[i].uptime_ms = 0U;
        g_manager.modules[i].last_heartbeat_ms = 0U;
        g_manager.modules[i].msg_queue_count = 0U;
    }

    /* 初始化统计信息 */
    g_manager.stats.total_uptime_ms = 0U;
    g_manager.stats.total_errors = 0U;
    g_manager.stats.total_recoveries = 0U;
    g_manager.stats.reset_count = 0U;
    g_manager.stats.power_on_count = 1U;
    g_manager.stats.overall_health = HEALTH_STATUS_UNKNOWN;

    g_manager.is_initialized = true;
    g_manager.is_running = false;

    printf("[管理模块] 初始化完成\n");

    return RET_OK;
}

ret_code_t manager_deinit(void)
{
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    g_manager.is_initialized = false;
    g_manager.is_running = false;

    printf("[管理模块] 反初始化完成\n");

    return RET_OK;
}

ret_code_t manager_init_all_modules(void)
{
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    printf("[管理模块] 开始初始化所有模块...\n");

    /* 初始化日志模块 */
    g_manager.modules[MODULE_LOG].state = MODULE_STATE_INIT;
    g_manager.modules[MODULE_LOG].state = MODULE_STATE_READY;
    printf("[管理模块] 日志模块初始化完成\n");

    /* 初始化 NAND 模块 */
    g_manager.modules[MODULE_NAND].state = MODULE_STATE_INIT;
    g_manager.modules[MODULE_NAND].state = MODULE_STATE_READY;
    printf("[管理模块] NAND 模块初始化完成\n");

    /* 初始化 FTL 模块 */
    g_manager.modules[MODULE_FTL].state = MODULE_STATE_INIT;
    g_manager.modules[MODULE_FTL].state = MODULE_STATE_READY;
    printf("[管理模块] FTL 模块初始化完成\n");

    /* 初始化主机接口模块 */
    g_manager.modules[MODULE_HOST_IF].state = MODULE_STATE_INIT;
    g_manager.modules[MODULE_HOST_IF].state = MODULE_STATE_READY;
    printf("[管理模块] 主机接口模块初始化完成\n");

    /* 更新整体健康状态 */
    g_manager.stats.overall_health = HEALTH_STATUS_HEALTHY;

    printf("[管理模块] 所有模块初始化完成\n");

    return RET_OK;
}

ret_code_t manager_start_all_modules(void)
{
    uint32_t i = 0;

    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    printf("[管理模块] 启动所有模块...\n");

    /* 启动所有就绪的模块 */
    for (i = 0; i < MODULE_MAX; i++) {
        if (g_manager.modules[i].state == MODULE_STATE_READY) {
            g_manager.modules[i].state = MODULE_STATE_RUNNING;
            g_manager.modules[i].health = HEALTH_STATUS_HEALTHY;
            g_manager.modules[i].last_heartbeat_ms = get_timestamp_ms();
        }
    }

    g_manager.is_running = true;

    printf("[管理模块] 所有模块已启动\n");

    return RET_OK;
}

ret_code_t manager_stop_all_modules(void)
{
    uint32_t i = 0;

    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    printf("[管理模块] 停止所有模块...\n");

    /* 停止所有运行中的模块 */
    for (i = 0; i < MODULE_MAX; i++) {
        if (g_manager.modules[i].state == MODULE_STATE_RUNNING) {
            g_manager.modules[i].state = MODULE_STATE_READY;
        }
    }

    g_manager.is_running = false;

    printf("[管理模块] 所有模块已停止\n");

    return RET_OK;
}

ret_code_t manager_process(void)
{
    message_t msg;
    ret_code_t ret = RET_OK;

    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 处理接收到的消息 */
    while (msg_queue_get_count(MODULE_MANAGER) > 0) {
        ret = msg_queue_recv(MODULE_MANAGER, &msg);
        if (ret != RET_OK) {
            break;
        }

        switch (msg.header.type) {
        case MSG_TYPE_MGR_HEALTH_CHECK:
            /* 健康检查请求 */
            break;

        case MSG_TYPE_MGR_ERROR_REPORT:
            /* 错误报告 */
            process_error_report(msg.header.src_module,
                                 msg.data.error.error_code,
                                 msg.data.error.error_msg);
            break;

        default:
            break;
        }
    }

    /* 执行健康检查 */
    process_health_check();

    return RET_OK;
}

ret_code_t manager_register_module(module_id_t module_id, const module_info_t *info)
{
    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }
    if (info == NULL) {
        return RET_ERR_PARAM;
    }
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    memcpy(&g_manager.modules[module_id], info, sizeof(module_info_t));

    return RET_OK;
}

ret_code_t manager_update_module_state(module_id_t module_id, module_state_t state)
{
    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }
    if (state >= MODULE_STATE_MAX) {
        return RET_ERR_PARAM;
    }
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    g_manager.modules[module_id].state = state;

    return RET_OK;
}

ret_code_t manager_update_module_health(module_id_t module_id, health_status_t health)
{
    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }
    if (health >= HEALTH_STATUS_MAX) {
        return RET_ERR_PARAM;
    }
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    g_manager.modules[module_id].health = health;

    /* 更新整体健康状态 */
    g_manager.stats.overall_health = calc_overall_health();

    return RET_OK;
}

ret_code_t manager_report_error(module_id_t module_id, uint32_t error_code, const char *error_msg)
{
    message_t msg;

    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 构造错误报告消息 */
    memset(&msg, 0, sizeof(msg));
    msg.header.type = MSG_TYPE_MGR_ERROR_REPORT;
    msg.header.priority = MSG_PRIORITY_HIGH;
    msg.header.src_module = module_id;
    msg.header.dst_module = MODULE_MANAGER;

    msg.data.error.module_id = module_id;
    msg.data.error.error_code = error_code;
    if (error_msg != NULL) {
        strncpy(msg.data.error.error_msg, error_msg, sizeof(msg.data.error.error_msg) - 1);
    }
    msg.data.error.timestamp = (uint32_t)get_timestamp_ms();

    /* 发送消息到管理模块 */
    return msg_queue_send(&msg);
}

ret_code_t manager_send_heartbeat(module_id_t module_id)
{
    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    g_manager.modules[module_id].last_heartbeat_ms = get_timestamp_ms();

    return RET_OK;
}

ret_code_t manager_get_module_info(module_id_t module_id, module_info_t *info)
{
    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }
    if (info == NULL) {
        return RET_ERR_PARAM;
    }
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    memcpy(info, &g_manager.modules[module_id], sizeof(module_info_t));

    return RET_OK;
}

ret_code_t manager_get_stats(firmware_stats_t *stats)
{
    if (stats == NULL) {
        return RET_ERR_PARAM;
    }
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    memcpy(stats, &g_manager.stats, sizeof(firmware_stats_t));

    return RET_OK;
}

void manager_print_module_status(void)
{
    uint32_t i = 0;
    const char *state_str[] = {"未初始化", "初始化中", "就绪", "运行中", "错误", "复位中"};
    const char *health_str[] = {"未知", "健康", "警告", "严重"};
    const char *module_names[] = {
        "NAND模块",
        "FTL模块",
        "主机接口",
        "管理模块",
        "日志模块"
    };

    if (!g_manager.is_initialized) {
        printf("管理模块未初始化\n");
        return;
    }

    printf("模块状态:\n");
    printf("  %-12s %-10s %-10s %-8s %-8s\n",
           "模块", "状态", "健康", "错误数", "队列");

    for (i = 0; i < MODULE_MAX; i++) {
        if (g_manager.modules[i].state == MODULE_STATE_UNINIT) {
            continue;
        }

        printf("  %-12s %-10s %-10s %-8u %-8u\n",
               module_names[i],
               state_str[g_manager.modules[i].state],
               health_str[g_manager.modules[i].health],
               g_manager.modules[i].error_count,
               g_manager.modules[i].msg_queue_count);
    }
}

void manager_print_stats(void)
{
    const char *health_str[] = {"未知", "健康", "警告", "严重"};

    if (!g_manager.is_initialized) {
        printf("管理模块未初始化\n");
        return;
    }

    printf("固件统计信息:\n");
    printf("  总运行时间:   %llu ms\n", (unsigned long long)g_manager.stats.total_uptime_ms);
    printf("  总错误数:     %u\n", g_manager.stats.total_errors);
    printf("  总恢复次数:   %u\n", g_manager.stats.total_recoveries);
    printf("  复位次数:     %u\n", g_manager.stats.reset_count);
    printf("  上电次数:     %u\n", g_manager.stats.power_on_count);
    printf("  整体健康状态: %s\n", health_str[g_manager.stats.overall_health]);
}
