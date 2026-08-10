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
    thermal_state_t thermal;         ///< 温度管理状态
    power_state_info_t power;        ///< 电源管理状态
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

/**
 * @brief 处理温度管理
 * @details 根据当前温度自动调整温度状态：
 *          - 温度超过警告阈值 → 进入警告状态
 *          - 温度超过严重阈值 → 进入严重状态，触发降频
 *          - 温度超过关机阈值 → 进入关机状态
 *          - 温度恢复到正常阈值以下 → 恢复正常
 */
static void process_thermal_management(void)
{
    thermal_state_t *thermal = &g_manager.thermal;
    const thermal_config_t *config = &g_manager.config.thermal;

    /* 根据当前温度判断状态 */
    if (thermal->current_temp >= config->shutdown_threshold) {
        /* 温度过高，需要关机 */
        if (thermal->state != TEMP_STATE_SHUTDOWN) {
            printf("[管理模块] 温度过高 (%d°C)，触发关机保护\n", thermal->current_temp);
            thermal->state = TEMP_STATE_SHUTDOWN;
        }
    } else if (thermal->current_temp >= config->critical_threshold) {
        /* 温度严重，触发降频 */
        if (thermal->state != TEMP_STATE_CRITICAL) {
            printf("[管理模块] 温度严重 (%d°C)，触发降频\n", thermal->current_temp);
            thermal->state = TEMP_STATE_CRITICAL;
            thermal->throttling_count++;
        }
        thermal->total_throttling_ms++;
    } else if (thermal->current_temp >= config->warning_threshold) {
        /* 温度警告 */
        if (thermal->state != TEMP_STATE_WARNING) {
            printf("[管理模块] 温度警告 (%d°C)\n", thermal->current_temp);
            thermal->state = TEMP_STATE_WARNING;
        }
    } else if (thermal->current_temp <= config->normal_threshold) {
        /* 温度恢复正常 */
        if (thermal->state != TEMP_STATE_NORMAL) {
            printf("[管理模块] 温度恢复正常 (%d°C)\n", thermal->current_temp);
            thermal->state = TEMP_STATE_NORMAL;
        }
    }

    /* 更新历史最高温度 */
    if (thermal->current_temp > thermal->max_temp) {
        thermal->max_temp = thermal->current_temp;
    }
}

/**
 * @brief 处理电源管理
 * @details 根据系统活动时间自动调整电源状态：
 *          - 活跃状态 → 空闲超时 → 空闲状态
 *          - 空闲状态 → 待机超时 → 待机状态
 *          - 待机状态 → 睡眠超时 → 睡眠状态
 *          - 有活动时 → 恢复到活跃状态
 */
static void process_power_management(void)
{
    power_state_info_t *power = &g_manager.power;
    const power_config_t *config = &g_manager.config.power;
    uint64_t now = 0;
    uint64_t idle_time = 0;

    if (!config->power_management) {
        return;
    }

    now = get_timestamp_ms();

    /* 计算空闲时间 */
    if (power->last_activity_ms <= now) {
        idle_time = now - power->last_activity_ms;
    } else {
        idle_time = 0;
    }

    /* 根据空闲时间调整电源状态 */
    if (idle_time >= config->sleep_timeout_ms) {
        if (power->current_state != POWER_STATE_SLEEP) {
            printf("[管理模块] 进入睡眠状态\n");
            power->current_state = POWER_STATE_SLEEP;
            power->power_state_changes++;
        }
    } else if (idle_time >= config->standby_timeout_ms) {
        if (power->current_state != POWER_STATE_STANDBY) {
            printf("[管理模块] 进入待机状态\n");
            power->current_state = POWER_STATE_STANDBY;
            power->power_state_changes++;
        }
    } else if (idle_time >= config->idle_timeout_ms) {
        if (power->current_state != POWER_STATE_IDLE) {
            printf("[管理模块] 进入空闲状态\n");
            power->current_state = POWER_STATE_IDLE;
            power->power_state_changes++;
        }
    }

    /* 统计各状态时间 */
    if (power->current_state == POWER_STATE_ACTIVE) {
        power->total_active_ms++;
    } else {
        power->total_idle_ms++;
    }
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

    /* 初始化温度管理状态 */
    g_manager.thermal.current_temp = 25;  /* 默认室温 25°C */
    g_manager.thermal.max_temp = 25;
    g_manager.thermal.state = TEMP_STATE_NORMAL;
    g_manager.thermal.throttling_count = 0U;
    g_manager.thermal.total_throttling_ms = 0U;

    /* 设置默认温度阈值 */
    if (g_manager.config.thermal.warning_threshold == 0) {
        g_manager.config.thermal.warning_threshold = 70;   /* 70°C 警告 */
        g_manager.config.thermal.critical_threshold = 80;  /* 80°C 严重 */
        g_manager.config.thermal.shutdown_threshold = 90;  /* 90°C 关机 */
        g_manager.config.thermal.normal_threshold = 60;    /* 60°C 恢复正常 */
        g_manager.config.thermal.thermal_throttling = true;
    }

    /* 初始化电源管理状态 */
    g_manager.power.current_state = POWER_STATE_ACTIVE;
    g_manager.power.target_state = POWER_STATE_ACTIVE;
    g_manager.power.last_activity_ms = 0U;
    g_manager.power.power_state_changes = 0U;
    g_manager.power.total_active_ms = 0U;
    g_manager.power.total_idle_ms = 0U;

    /* 设置默认电源管理超时 */
    if (g_manager.config.power.idle_timeout_ms == 0) {
        g_manager.config.power.idle_timeout_ms = 5000;     /* 5秒空闲 */
        g_manager.config.power.standby_timeout_ms = 30000; /* 30秒待机 */
        g_manager.config.power.sleep_timeout_ms = 60000;   /* 60秒睡眠 */
        g_manager.config.power.power_management = true;
    }

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

    /* 执行温度管理 */
    process_thermal_management();

    /* 执行电源管理 */
    process_power_management();

    /* 同步温度和电源状态到统计信息 */
    memcpy(&g_manager.stats.thermal, &g_manager.thermal, sizeof(thermal_state_t));
    memcpy(&g_manager.stats.power, &g_manager.power, sizeof(power_state_info_t));

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

/* ============================================================
 *  温度管理接口实现
 * ============================================================ */

ret_code_t manager_update_temperature(int32_t temperature)
{
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 更新当前温度 */
    g_manager.thermal.current_temp = temperature;

    /* 立即执行温度管理处理 */
    process_thermal_management();

    return RET_OK;
}

ret_code_t manager_get_thermal_state(thermal_state_t *state)
{
    if (state == NULL) {
        return RET_ERR_PARAM;
    }
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    memcpy(state, &g_manager.thermal, sizeof(thermal_state_t));

    return RET_OK;
}

temp_state_t manager_get_temp_state(void)
{
    if (!g_manager.is_initialized) {
        return TEMP_STATE_UNKNOWN;
    }

    return g_manager.thermal.state;
}

void manager_print_thermal_info(void)
{
    const char *state_str[] = {"正常", "警告", "严重", "关机"};

    if (!g_manager.is_initialized) {
        printf("管理模块未初始化\n");
        return;
    }

    printf("温度管理信息:\n");
    printf("  当前温度:     %d°C\n", g_manager.thermal.current_temp);
    printf("  历史最高:     %d°C\n", g_manager.thermal.max_temp);
    printf("  温度状态:     %s\n", state_str[g_manager.thermal.state]);
    printf("  降频次数:     %u\n", g_manager.thermal.throttling_count);
    printf("  总降频时间:   %llu ms\n", (unsigned long long)g_manager.thermal.total_throttling_ms);
    printf("  警告阈值:     %d°C\n", g_manager.config.thermal.warning_threshold);
    printf("  严重阈值:     %d°C\n", g_manager.config.thermal.critical_threshold);
    printf("  关机阈值:     %d°C\n", g_manager.config.thermal.shutdown_threshold);
}

/* ============================================================
 *  电源管理接口实现
 * ============================================================ */

ret_code_t manager_set_power_state(power_state_t state)
{
    if (state >= POWER_STATE_MAX) {
        return RET_ERR_PARAM;
    }
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 设置目标电源状态 */
    g_manager.power.target_state = state;
    g_manager.power.current_state = state;
    g_manager.power.power_state_changes++;

    /* 重置活动时间 */
    g_manager.power.last_activity_ms = get_timestamp_ms();

    return RET_OK;
}

power_state_t manager_get_power_state(void)
{
    if (!g_manager.is_initialized) {
        return POWER_STATE_ACTIVE;
    }

    return g_manager.power.current_state;
}

ret_code_t manager_report_activity(void)
{
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 更新最后活动时间 */
    g_manager.power.last_activity_ms = get_timestamp_ms();

    /* 如果当前不是活跃状态，恢复到活跃状态 */
    if (g_manager.power.current_state != POWER_STATE_ACTIVE) {
        g_manager.power.current_state = POWER_STATE_ACTIVE;
        g_manager.power.power_state_changes++;
    }

    return RET_OK;
}

ret_code_t manager_get_power_state_info(power_state_info_t *info)
{
    if (info == NULL) {
        return RET_ERR_PARAM;
    }
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    memcpy(info, &g_manager.power, sizeof(power_state_info_t));

    return RET_OK;
}

void manager_print_power_info(void)
{
    const char *state_str[] = {"活跃", "空闲", "待机", "睡眠", "深度睡眠"};

    if (!g_manager.is_initialized) {
        printf("管理模块未初始化\n");
        return;
    }

    printf("电源管理信息:\n");
    printf("  当前状态:     %s\n", state_str[g_manager.power.current_state]);
    printf("  目标状态:     %s\n", state_str[g_manager.power.target_state]);
    printf("  状态变更次数: %u\n", g_manager.power.power_state_changes);
    printf("  总活跃时间:   %llu ms\n", (unsigned long long)g_manager.power.total_active_ms);
    printf("  总空闲时间:   %llu ms\n", (unsigned long long)g_manager.power.total_idle_ms);
    printf("  空闲超时:     %u ms\n", g_manager.config.power.idle_timeout_ms);
    printf("  待机超时:     %u ms\n", g_manager.config.power.standby_timeout_ms);
    printf("  睡眠超时:     %u ms\n", g_manager.config.power.sleep_timeout_ms);
}
