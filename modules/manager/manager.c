/**
 * @file manager.c
 * @brief 管理模块实现
 * @details 企业级固件的管理模块实现，负责模块生命周期管理、
 *          健康监控、错误处理、自动恢复、温度管理和电源管理。
 *          是固件的"大脑"，协调所有子模块的运行和故障处理。
 */

#include "manager.h"
#include <string.h>
#include <stdio.h>

/* ============================================================
 *  内部数据结构
 * ============================================================ */

/**
 * @brief 管理模块私有数据
 * @details 管理模块维护固件配置、统计信息、所有模块的状态、
 *          温度管理状态和电源管理状态
 */
typedef struct {
    firmware_config_t config;        ///< 固件配置（心跳、看门狗、温度、电源等）
    firmware_stats_t stats;          ///< 固件统计信息（运行时间、错误数、恢复数等）
    module_info_t modules[MODULE_MAX]; ///< 模块信息数组（所有子模块的状态）
    thermal_state_t thermal;         ///< 温度管理状态
    power_state_info_t power;        ///< 电源管理状态
    bool is_initialized;             ///< 初始化标志
    bool is_running;                 ///< 运行标志（所有模块是否已启动）
} manager_dev_t;

/* ============================================================
 *  全局变量
 * ============================================================ */

/**
 * @brief 管理模块全局设备实例
 * @details 单例模式，整个固件只有一个管理模块实例
 */
static manager_dev_t g_manager;

/* ============================================================
 *  内部函数
 * ============================================================ */

/**
 * @brief 获取当前时间戳（毫秒）
 * @return 时间戳（单调递增计数器）
 * @note 简化实现，使用静态计数器模拟。实际固件中使用硬件定时器
 *       或系统滴答时钟（SysTick）获取精确时间。
 */
static uint64_t get_timestamp_ms(void)
{
    /* 简化实现：静态自增计数器，每次调用+1 */
    static uint64_t counter = 0;
    return counter++;
}

/**
 * @brief 检查模块健康状态
 * @param[in] module_id 模块ID
 * @return 健康状态（健康/警告/严重/未知）
 * @note 根据错误计数和心跳状态综合判断健康状态：
 *       - 0 错误且心跳正常 = 健康
 *       - 错误数 < 阈值 = 警告
 *       - 错误数 >= 阈值 或 心跳超时 = 严重
 */
static health_status_t check_module_health(module_id_t module_id)
{
    module_info_t *info = NULL;
    uint64_t now = 0;
    uint64_t heartbeat_age = 0;

    /* 模块ID边界检查 */
    if (module_id >= MODULE_MAX) {
        return HEALTH_STATUS_UNKNOWN;
    }

    info = &g_manager.modules[module_id];
    now = get_timestamp_ms();

    /* 计算心跳年龄（距上次心跳的时间），防止时间回绕 */
    if (info->last_heartbeat_ms <= now) {
        heartbeat_age = now - info->last_heartbeat_ms;
    } else {
        heartbeat_age = 0;
    }

    /* 心跳超时，直接判定为严重（模块可能已挂死） */
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
 * @details 遍历所有已初始化模块，取最差的健康状态作为整体状态。
 *          健康状态枚举值越大越严重，因此取最大值。
 */
static health_status_t calc_overall_health(void)
{
    uint32_t i = 0;
    health_status_t overall = HEALTH_STATUS_HEALTHY;

    /* 遍历所有模块，取最差健康状态 */
    for (i = 0; i < MODULE_MAX; i++) {
        /* 跳过未初始化的模块 */
        if (g_manager.modules[i].state == MODULE_STATE_UNINIT) {
            continue;
        }

        /* 健康状态值越大越严重，取最大值 */
        if (g_manager.modules[i].health > overall) {
            overall = g_manager.modules[i].health;
        }
    }

    return overall;
}

/**
 * @brief 自动恢复模块
 * @param[in] module_id 模块ID
 * @retval RET_OK 恢复成功
 * @retval RET_ERR_PARAM 参数错误
 * @note 简化实现：重置模块状态和错误计数。
 *       实际固件中需要执行模块级别的复位和重新初始化流程，
 *       包括保存上下文、复位硬件、重新初始化、恢复上下文等步骤。
 */
static ret_code_t auto_recover_module(module_id_t module_id)
{
    /* 模块ID边界检查 */
    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }

    /* 简化恢复流程：复位 → 清零错误 → 就绪 */
    g_manager.modules[module_id].state = MODULE_STATE_RESET;
    g_manager.modules[module_id].error_count = 0U;
    g_manager.modules[module_id].health = HEALTH_STATUS_HEALTHY;
    g_manager.modules[module_id].state = MODULE_STATE_READY;

    /* 累计恢复次数 */
    g_manager.stats.total_recoveries++;

    return RET_OK;
}

/**
 * @brief 处理错误报告
 * @param[in] module_id 报告错误的模块ID
 * @param[in] error_code 错误码
 * @param[in] error_msg 错误信息字符串（可为NULL）
 * @details 处理来自各模块的错误报告，更新错误计数和健康状态。
 *          如果错误数超过阈值且启用了自动恢复，则触发自动恢复。
 */
static void process_error_report(module_id_t module_id, uint32_t error_code, const char *error_msg)
{
    module_info_t *info = NULL;

    /* 模块ID边界检查 */
    if (module_id >= MODULE_MAX) {
        return;
    }

    info = &g_manager.modules[module_id];

    /* 更新错误计数（模块级和全局级） */
    info->error_count++;
    g_manager.stats.total_errors++;

    /* 重新计算模块健康状态 */
    info->health = check_module_health(module_id);

    /* 检查是否需要自动恢复：启用自动恢复且错误数达到阈值 */
    if (g_manager.config.auto_recovery &&
        info->error_count >= g_manager.config.max_error_count) {
        printf("[管理模块] 模块 %u 错误过多，触发自动恢复\n", module_id);
        auto_recover_module(module_id);
    }

    /* 打印错误信息（如果有） */
    if (error_msg != NULL) {
        printf("[管理模块] 模块 %u 错误: 码=%u, 信息=%s\n",
               module_id, error_code, error_msg);
    }
}

/**
 * @brief 处理健康检查
 * @details 执行周期性健康检查，包括：
 *          - 更新运行中模块的运行时间
 *          - 更新各模块消息队列长度
 *          - 看门狗检测（心跳超时检测）
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
        /* 跳过未初始化的模块 */
        if (g_manager.modules[i].state == MODULE_STATE_UNINIT) {
            continue;
        }

        /* 更新运行时间（仅对运行中的模块） */
        if (g_manager.modules[i].state == MODULE_STATE_RUNNING) {
            g_manager.modules[i].uptime_ms++;
        }

        /* 更新消息队列长度（从消息队列模块获取） */
        g_manager.modules[i].msg_queue_count = msg_queue_get_count((module_id_t)i);

        /* 看门狗检测：检查运行中模块的心跳是否超时 */
        if (g_manager.modules[i].state == MODULE_STATE_RUNNING) {
            /* 计算心跳年龄 */
            if (g_manager.modules[i].last_heartbeat_ms <= now) {
                heartbeat_age = now - g_manager.modules[i].last_heartbeat_ms;
            } else {
                heartbeat_age = 0;
            }

            /* 心跳超时，触发错误报告 */
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
 *          - 温度超过关机阈值 → 进入关机状态（保护硬件）
 *          - 温度超过严重阈值 → 进入严重状态，触发降频
 *          - 温度超过警告阈值 → 进入警告状态
 *          - 温度恢复到正常阈值以下 → 恢复正常
 *          同时更新历史最高温度记录。
 */
static void process_thermal_management(void)
{
    thermal_state_t *thermal = &g_manager.thermal;
    const thermal_config_t *config = &g_manager.config.thermal;

    /* 根据当前温度判断状态（从高到低判断，确保最严重状态优先） */
    if (thermal->current_temp >= config->shutdown_threshold) {
        /* 温度过高，需要关机保护 */
        if (thermal->state != TEMP_STATE_SHUTDOWN) {
            printf("[管理模块] 温度过高 (%d°C)，触发关机保护\n", thermal->current_temp);
            thermal->state = TEMP_STATE_SHUTDOWN;
        }
    } else if (thermal->current_temp >= config->critical_threshold) {
        /* 温度严重，触发降频（减少发热） */
        if (thermal->state != TEMP_STATE_CRITICAL) {
            printf("[管理模块] 温度严重 (%d°C)，触发降频\n", thermal->current_temp);
            thermal->state = TEMP_STATE_CRITICAL;
            thermal->throttling_count++;
        }
        /* 累计降频时间 */
        thermal->total_throttling_ms++;
    } else if (thermal->current_temp >= config->warning_threshold) {
        /* 温度警告（尚未需要降频） */
        if (thermal->state != TEMP_STATE_WARNING) {
            printf("[管理模块] 温度警告 (%d°C)\n", thermal->current_temp);
            thermal->state = TEMP_STATE_WARNING;
        }
    } else if (thermal->current_temp <= config->normal_threshold) {
        /* 温度恢复正常（滞后阈值，避免频繁切换） */
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
 *          - 活跃 → 空闲超时 → 空闲状态
 *          - 空闲 → 待机超时 → 待机状态
 *          - 待机 → 睡眠超时 → 睡眠状态
 *          - 有活动时 → 恢复到活跃状态（通过 manager_report_activity）
 *          同时统计各状态的累计时间。
 */
static void process_power_management(void)
{
    power_state_info_t *power = &g_manager.power;
    const power_config_t *config = &g_manager.config.power;
    uint64_t now = 0;
    uint64_t idle_time = 0;

    /* 电源管理未启用则直接返回 */
    if (!config->power_management) {
        return;
    }

    now = get_timestamp_ms();

    /* 计算空闲时间（距上次活动的时间） */
    if (power->last_activity_ms <= now) {
        idle_time = now - power->last_activity_ms;
    } else {
        idle_time = 0;
    }

    /* 根据空闲时间调整电源状态（从深到浅判断） */
    if (idle_time >= config->sleep_timeout_ms) {
        /* 进入睡眠状态（最低功耗） */
        if (power->current_state != POWER_STATE_SLEEP) {
            printf("[管理模块] 进入睡眠状态\n");
            power->current_state = POWER_STATE_SLEEP;
            power->power_state_changes++;
        }
    } else if (idle_time >= config->standby_timeout_ms) {
        /* 进入待机状态 */
        if (power->current_state != POWER_STATE_STANDBY) {
            printf("[管理模块] 进入待机状态\n");
            power->current_state = POWER_STATE_STANDBY;
            power->power_state_changes++;
        }
    } else if (idle_time >= config->idle_timeout_ms) {
        /* 进入空闲状态 */
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

/**
 * @brief 初始化管理模块
 * @param[in] config 固件配置指针
 * @retval RET_OK 初始化成功
 * @retval RET_ERR_PARAM 配置指针为空
 * @details 初始化管理模块，保存固件配置，初始化所有模块信息、
 *          统计信息、温度管理状态和电源管理状态。
 *          如果配置中温度/电源阈值为0，使用默认值。
 */
ret_code_t manager_init(const firmware_config_t *config)
{
    uint32_t i = 0;

    /* 配置指针空检查 */
    if (config == NULL) {
        return RET_ERR_PARAM;
    }

    /* 清零管理模块数据 */
    memset(&g_manager, 0, sizeof(g_manager));

    /* 保存固件配置 */
    memcpy(&g_manager.config, config, sizeof(firmware_config_t));

    /* 初始化所有模块信息 */
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
    g_manager.stats.power_on_count = 1U;  /* 上电次数初始为1 */
    g_manager.stats.overall_health = HEALTH_STATUS_UNKNOWN;

    /* 初始化温度管理状态（默认室温25°C） */
    g_manager.thermal.current_temp = 25;
    g_manager.thermal.max_temp = 25;
    g_manager.thermal.state = TEMP_STATE_NORMAL;
    g_manager.thermal.throttling_count = 0U;
    g_manager.thermal.total_throttling_ms = 0U;

    /* 设置默认温度阈值（如果配置中未设置） */
    if (g_manager.config.thermal.warning_threshold == 0) {
        g_manager.config.thermal.warning_threshold = 70;   /* 70°C 警告 */
        g_manager.config.thermal.critical_threshold = 80;  /* 80°C 严重（降频） */
        g_manager.config.thermal.shutdown_threshold = 90;  /* 90°C 关机保护 */
        g_manager.config.thermal.normal_threshold = 60;    /* 60°C 恢复正常（滞后） */
        g_manager.config.thermal.thermal_throttling = true;
    }

    /* 初始化电源管理状态（默认活跃状态） */
    g_manager.power.current_state = POWER_STATE_ACTIVE;
    g_manager.power.target_state = POWER_STATE_ACTIVE;
    g_manager.power.last_activity_ms = 0U;
    g_manager.power.power_state_changes = 0U;
    g_manager.power.total_active_ms = 0U;
    g_manager.power.total_idle_ms = 0U;

    /* 设置默认电源管理超时（如果配置中未设置） */
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

/**
 * @brief 反初始化管理模块
 * @retval RET_OK 反初始化成功
 * @retval RET_ERR_NOT_INIT 管理模块未初始化
 * @details 重置管理模块状态，标记为未初始化和未运行。
 */
ret_code_t manager_deinit(void)
{
    /* 初始化检查 */
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    g_manager.is_initialized = false;
    g_manager.is_running = false;

    printf("[管理模块] 反初始化完成\n");

    return RET_OK;
}

/**
 * @brief 初始化所有模块
 * @retval RET_OK 初始化成功
 * @retval RET_ERR_NOT_INIT 管理模块未初始化
 * @details 按顺序初始化所有子模块（日志→NAND→FTL→主机接口）。
 *          实际固件中这里会调用各模块的初始化函数，这里简化为
 *          状态转换。初始化后模块处于 READY 状态，等待启动。
 */
ret_code_t manager_init_all_modules(void)
{
    /* 初始化检查 */
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    printf("[管理模块] 开始初始化所有模块...\n");

    /* 初始化日志模块（最先初始化，其他模块依赖日志） */
    g_manager.modules[MODULE_LOG].state = MODULE_STATE_INIT;
    g_manager.modules[MODULE_LOG].state = MODULE_STATE_READY;
    printf("[管理模块] 日志模块初始化完成\n");

    /* 初始化 NAND 模块（存储介质底层） */
    g_manager.modules[MODULE_NAND].state = MODULE_STATE_INIT;
    g_manager.modules[MODULE_NAND].state = MODULE_STATE_READY;
    printf("[管理模块] NAND 模块初始化完成\n");

    /* 初始化 FTL 模块（闪存转换层，依赖NAND） */
    g_manager.modules[MODULE_FTL].state = MODULE_STATE_INIT;
    g_manager.modules[MODULE_FTL].state = MODULE_STATE_READY;
    printf("[管理模块] FTL 模块初始化完成\n");

    /* 初始化主机接口模块（最上层，依赖FTL） */
    g_manager.modules[MODULE_HOST_IF].state = MODULE_STATE_INIT;
    g_manager.modules[MODULE_HOST_IF].state = MODULE_STATE_READY;
    printf("[管理模块] 主机接口模块初始化完成\n");

    /* 所有模块初始化完成，整体健康状态设为健康 */
    g_manager.stats.overall_health = HEALTH_STATUS_HEALTHY;

    printf("[管理模块] 所有模块初始化完成\n");

    return RET_OK;
}

/**
 * @brief 启动所有模块
 * @retval RET_OK 启动成功
 * @retval RET_ERR_NOT_INIT 管理模块未初始化
 * @details 将所有处于 READY 状态的模块切换为 RUNNING 状态，
 *          初始化健康状态和心跳时间。启动后管理模块进入运行状态。
 */
ret_code_t manager_start_all_modules(void)
{
    uint32_t i = 0;

    /* 初始化检查 */
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    printf("[管理模块] 启动所有模块...\n");

    /* 启动所有就绪的模块 */
    for (i = 0; i < MODULE_MAX; i++) {
        if (g_manager.modules[i].state == MODULE_STATE_READY) {
            g_manager.modules[i].state = MODULE_STATE_RUNNING;
            g_manager.modules[i].health = HEALTH_STATUS_HEALTHY;
            /* 记录初始心跳时间 */
            g_manager.modules[i].last_heartbeat_ms = get_timestamp_ms();
        }
    }

    g_manager.is_running = true;

    printf("[管理模块] 所有模块已启动\n");

    return RET_OK;
}

/**
 * @brief 停止所有模块
 * @retval RET_OK 停止成功
 * @retval RET_ERR_NOT_INIT 管理模块未初始化
 * @details 将所有运行中的模块切换回 READY 状态，
 *          管理模块退出运行状态。
 */
ret_code_t manager_stop_all_modules(void)
{
    uint32_t i = 0;

    /* 初始化检查 */
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

/**
 * @brief 管理模块主处理函数
 * @retval RET_OK 处理成功
 * @retval RET_ERR_NOT_INIT 管理模块未初始化
 * @details 管理模块的周期性处理入口，应在主循环中定期调用。
 *          执行以下工作：
 *          1. 处理消息队列中的消息（健康检查请求、错误报告等）
 *          2. 执行健康检查（心跳超时检测、运行时间更新）
 *          3. 执行温度管理（阈值判断、降频/关机保护）
 *          4. 执行电源管理（空闲超时、状态切换）
 *          5. 同步温度和电源状态到统计信息
 */
ret_code_t manager_process(void)
{
    message_t msg;
    ret_code_t ret = RET_OK;

    /* 初始化检查 */
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 处理消息队列中的所有消息 */
    while (msg_queue_get_count(MODULE_MANAGER) > 0) {
        ret = msg_queue_recv(MODULE_MANAGER, &msg);
        if (ret != RET_OK) {
            break;
        }

        /* 根据消息类型分发处理 */
        switch (msg.header.type) {
        case MSG_TYPE_MGR_HEALTH_CHECK:
            /* 健康检查请求（当前简化处理，健康检查在下方统一执行） */
            break;

        case MSG_TYPE_MGR_ERROR_REPORT:
            /* 错误报告：调用错误处理函数 */
            process_error_report(msg.header.src_module,
                                 msg.data.error.error_code,
                                 msg.data.error.error_msg);
            break;

        default:
            /* 未知消息类型，忽略 */
            break;
        }
    }

    /* 执行健康检查（心跳超时、运行时间、队列长度） */
    process_health_check();

    /* 执行温度管理（阈值判断、降频、关机保护） */
    process_thermal_management();

    /* 执行电源管理（空闲超时、状态切换） */
    process_power_management();

    /* 同步温度和电源状态到统计信息（供外部查询） */
    memcpy(&g_manager.stats.thermal, &g_manager.thermal, sizeof(thermal_state_t));
    memcpy(&g_manager.stats.power, &g_manager.power, sizeof(power_state_info_t));

    return RET_OK;
}

/**
 * @brief 注册模块信息
 * @param[in] module_id 模块ID
 * @param[in] info 模块信息指针
 * @retval RET_OK 注册成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_INIT 管理模块未初始化
 * @details 注册模块的详细信息到管理模块，覆盖默认初始化的信息。
 *          模块在初始化完成后应调用此函数注册自身信息。
 */
ret_code_t manager_register_module(module_id_t module_id, const module_info_t *info)
{
    /* 模块ID边界检查 */
    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }

    /* 信息指针空检查 */
    if (info == NULL) {
        return RET_ERR_PARAM;
    }

    /* 初始化检查 */
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 复制模块信息 */
    memcpy(&g_manager.modules[module_id], info, sizeof(module_info_t));

    return RET_OK;
}

/**
 * @brief 更新模块状态
 * @param[in] module_id 模块ID
 * @param[in] state 新的模块状态
 * @retval RET_OK 更新成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_INIT 管理模块未初始化
 * @details 模块状态变化时调用此函数通知管理模块。
 *          状态包括：未初始化、初始化中、就绪、运行中、错误、复位中。
 */
ret_code_t manager_update_module_state(module_id_t module_id, module_state_t state)
{
    /* 模块ID边界检查 */
    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }

    /* 状态有效性检查 */
    if (state >= MODULE_STATE_MAX) {
        return RET_ERR_PARAM;
    }

    /* 初始化检查 */
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 更新模块状态 */
    g_manager.modules[module_id].state = state;

    return RET_OK;
}

/**
 * @brief 更新模块健康状态
 * @param[in] module_id 模块ID
 * @param[in] health 健康状态
 * @retval RET_OK 更新成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_INIT 管理模块未初始化
 * @details 模块自检发现健康状态变化时调用此函数。
 *          更新后自动重新计算整体健康状态。
 */
ret_code_t manager_update_module_health(module_id_t module_id, health_status_t health)
{
    /* 模块ID边界检查 */
    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }

    /* 健康状态有效性检查 */
    if (health >= HEALTH_STATUS_MAX) {
        return RET_ERR_PARAM;
    }

    /* 初始化检查 */
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 更新模块健康状态 */
    g_manager.modules[module_id].health = health;

    /* 重新计算整体健康状态 */
    g_manager.stats.overall_health = calc_overall_health();

    return RET_OK;
}

/**
 * @brief 报告错误
 * @param[in] module_id 报告错误的模块ID
 * @param[in] error_code 错误码
 * @param[in] error_msg 错误信息（可为NULL）
 * @retval RET_OK 报告发送成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_INIT 管理模块未初始化
 * @details 模块发生错误时调用此函数，通过消息队列向管理模块
 *          发送错误报告。管理模块会在 manager_process 中处理。
 *          使用高优先级消息确保错误及时处理。
 */
ret_code_t manager_report_error(module_id_t module_id, uint32_t error_code, const char *error_msg)
{
    message_t msg;

    /* 模块ID边界检查 */
    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }

    /* 初始化检查 */
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 构造错误报告消息 */
    memset(&msg, 0, sizeof(msg));
    msg.header.type = MSG_TYPE_MGR_ERROR_REPORT;
    msg.header.priority = MSG_PRIORITY_HIGH;  /* 错误报告使用高优先级 */
    msg.header.src_module = module_id;
    msg.header.dst_module = MODULE_MANAGER;

    /* 填充错误数据 */
    msg.data.error.module_id = module_id;
    msg.data.error.error_code = error_code;
    if (error_msg != NULL) {
        /* 安全拷贝错误信息字符串 */
        strncpy(msg.data.error.error_msg, error_msg, sizeof(msg.data.error.error_msg) - 1);
    }
    msg.data.error.timestamp = (uint32_t)get_timestamp_ms();

    /* 发送消息到管理模块的消息队列 */
    return msg_queue_send(&msg);
}

/**
 * @brief 发送心跳
 * @param[in] module_id 模块ID
 * @retval RET_OK 心跳发送成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_INIT 管理模块未初始化
 * @details 模块定期调用此函数发送心跳，告知管理模块自己仍在正常运行。
 *          管理模块通过心跳超时检测模块是否挂死（看门狗机制）。
 */
ret_code_t manager_send_heartbeat(module_id_t module_id)
{
    /* 模块ID边界检查 */
    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }

    /* 初始化检查 */
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 更新最后心跳时间 */
    g_manager.modules[module_id].last_heartbeat_ms = get_timestamp_ms();

    return RET_OK;
}

/**
 * @brief 获取模块信息
 * @param[in] module_id 模块ID
 * @param[out] info 模块信息输出缓冲区
 * @retval RET_OK 获取成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_INIT 管理模块未初始化
 * @details 获取指定模块的完整信息，包括状态、健康、错误计数、
 *          运行时间、心跳时间和消息队列长度。
 */
ret_code_t manager_get_module_info(module_id_t module_id, module_info_t *info)
{
    /* 模块ID边界检查 */
    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }

    /* 输出缓冲区空指针检查 */
    if (info == NULL) {
        return RET_ERR_PARAM;
    }

    /* 初始化检查 */
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 复制模块信息到输出缓冲区 */
    memcpy(info, &g_manager.modules[module_id], sizeof(module_info_t));

    return RET_OK;
}

/**
 * @brief 获取固件统计信息
 * @param[out] stats 统计信息输出缓冲区
 * @retval RET_OK 获取成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_INIT 管理模块未初始化
 * @details 获取固件全局统计信息，包括总运行时间、总错误数、
 *          总恢复次数、复位次数、上电次数、整体健康状态、
 *          温度状态和电源状态。
 */
ret_code_t manager_get_stats(firmware_stats_t *stats)
{
    /* 输出缓冲区空指针检查 */
    if (stats == NULL) {
        return RET_ERR_PARAM;
    }

    /* 初始化检查 */
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 复制统计信息到输出缓冲区 */
    memcpy(stats, &g_manager.stats, sizeof(firmware_stats_t));

    return RET_OK;
}

/**
 * @brief 打印模块状态
 * @details 以表格形式打印所有已初始化模块的状态，包括
 *          模块名、状态、健康、错误数和消息队列长度。用于调试。
 */
void manager_print_module_status(void)
{
    uint32_t i = 0;
    /* 状态字符串表（索引对应 module_state_t 枚举值） */
    const char *state_str[] = {"未初始化", "初始化中", "就绪", "运行中", "错误", "复位中"};
    /* 健康字符串表（索引对应 health_status_t 枚举值） */
    const char *health_str[] = {"未知", "健康", "警告", "严重"};
    /* 模块名称表（索引对应 module_id_t 枚举值） */
    const char *module_names[] = {
        "NAND模块",
        "FTL模块",
        "主机接口",
        "管理模块",
        "日志模块"
    };

    /* 初始化检查 */
    if (!g_manager.is_initialized) {
        printf("管理模块未初始化\n");
        return;
    }

    /* 打印模块状态表格 */
    printf("模块状态:\n");
    printf("  %-12s %-10s %-10s %-8s %-8s\n",
           "模块", "状态", "健康", "错误数", "队列");

    /* 遍历打印每个已初始化模块 */
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

/**
 * @brief 打印固件统计信息
 * @details 打印固件全局统计信息，包括总运行时间、错误数、
 *          恢复次数、复位次数、上电次数和整体健康状态。
 */
void manager_print_stats(void)
{
    /* 健康字符串表 */
    const char *health_str[] = {"未知", "健康", "警告", "严重"};

    /* 初始化检查 */
    if (!g_manager.is_initialized) {
        printf("管理模块未初始化\n");
        return;
    }

    /* 打印固件统计信息 */
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

/**
 * @brief 更新当前温度
 * @param[in] temperature 当前温度（摄氏度）
 * @retval RET_OK 更新成功
 * @retval RET_ERR_NOT_INIT 管理模块未初始化
 * @details 温度传感器读取到新温度后调用此函数。
 *          更新后立即执行温度管理处理，判断是否需要降频或关机。
 */
ret_code_t manager_update_temperature(int32_t temperature)
{
    /* 初始化检查 */
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 更新当前温度 */
    g_manager.thermal.current_temp = temperature;

    /* 立即执行温度管理处理（阈值判断、状态切换） */
    process_thermal_management();

    return RET_OK;
}

/**
 * @brief 获取温度管理状态
 * @param[out] state 温度状态输出缓冲区
 * @retval RET_OK 获取成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_INIT 管理模块未初始化
 * @details 获取完整的温度管理状态，包括当前温度、历史最高温度、
 *          温度状态、降频次数和总降频时间。
 */
ret_code_t manager_get_thermal_state(thermal_state_t *state)
{
    /* 输出缓冲区空指针检查 */
    if (state == NULL) {
        return RET_ERR_PARAM;
    }

    /* 初始化检查 */
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 复制温度状态 */
    memcpy(state, &g_manager.thermal, sizeof(thermal_state_t));

    return RET_OK;
}

/**
 * @brief 获取当前温度状态
 * @return 温度状态（正常/警告/严重/关机），未初始化返回正常
 * @details 快速获取温度状态，不需要完整信息时使用。
 */
temp_state_t manager_get_temp_state(void)
{
    /* 初始化检查 */
    if (!g_manager.is_initialized) {
        return TEMP_STATE_NORMAL;
    }

    return g_manager.thermal.state;
}

/**
 * @brief 打印温度管理信息
 * @details 打印温度管理的详细信息，包括当前温度、历史最高温度、
 *          温度状态、降频次数、总降频时间和各温度阈值。
 */
void manager_print_thermal_info(void)
{
    /* 温度状态字符串表 */
    const char *state_str[] = {"正常", "警告", "严重", "关机"};

    /* 初始化检查 */
    if (!g_manager.is_initialized) {
        printf("管理模块未初始化\n");
        return;
    }

    /* 打印温度管理信息 */
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

/**
 * @brief 设置电源状态
 * @param[in] state 目标电源状态
 * @retval RET_OK 设置成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_INIT 管理模块未初始化
 * @details 手动设置电源状态（覆盖自动电源管理）。
 *          设置后重置活动时间，防止立即被自动管理切回。
 */
ret_code_t manager_set_power_state(power_state_t state)
{
    /* 电源状态有效性检查 */
    if (state >= POWER_STATE_MAX) {
        return RET_ERR_PARAM;
    }

    /* 初始化检查 */
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 设置目标和当前电源状态 */
    g_manager.power.target_state = state;
    g_manager.power.current_state = state;
    g_manager.power.power_state_changes++;

    /* 重置活动时间（防止自动管理立即切回） */
    g_manager.power.last_activity_ms = get_timestamp_ms();

    return RET_OK;
}

/**
 * @brief 获取当前电源状态
 * @return 电源状态，未初始化返回活跃状态
 * @details 快速获取当前电源状态。
 */
power_state_t manager_get_power_state(void)
{
    /* 初始化检查 */
    if (!g_manager.is_initialized) {
        return POWER_STATE_ACTIVE;
    }

    return g_manager.power.current_state;
}

/**
 * @brief 报告系统活动
 * @retval RET_OK 报告成功
 * @retval RET_ERR_NOT_INIT 管理模块未初始化
 * @details 系统有活动（如收到主机命令）时调用此函数，
 *          更新最后活动时间。如果当前处于低功耗状态，
 *          立即恢复到活跃状态。
 */
ret_code_t manager_report_activity(void)
{
    /* 初始化检查 */
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

/**
 * @brief 获取电源状态信息
 * @param[out] info 电源状态信息输出缓冲区
 * @retval RET_OK 获取成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_INIT 管理模块未初始化
 * @details 获取完整的电源管理状态信息，包括当前状态、目标状态、
 *          状态变更次数、总活跃时间、总空闲时间和最后活动时间。
 */
ret_code_t manager_get_power_state_info(power_state_info_t *info)
{
    /* 输出缓冲区空指针检查 */
    if (info == NULL) {
        return RET_ERR_PARAM;
    }

    /* 初始化检查 */
    if (!g_manager.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 复制电源状态信息 */
    memcpy(info, &g_manager.power, sizeof(power_state_info_t));

    return RET_OK;
}

/**
 * @brief 打印电源管理信息
 * @details 打印电源管理的详细信息，包括当前状态、目标状态、
 *          状态变更次数、总活跃时间、总空闲时间和各超时阈值。
 */
void manager_print_power_info(void)
{
    /* 电源状态字符串表 */
    const char *state_str[] = {"活跃", "空闲", "待机", "睡眠", "深度睡眠"};

    /* 初始化检查 */
    if (!g_manager.is_initialized) {
        printf("管理模块未初始化\n");
        return;
    }

    /* 打印电源管理信息 */
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
