/**
 * @file raid.c
 * @brief RAID 模块实现
 * @details 企业级固件的 RAID（独立磁盘冗余阵列）模块实现
 *          支持 RAID 0（条带化）和 RAID 1（镜像）
 *          使用多个独立缓冲区模拟多个 FTL 实例
 */

#include "raid.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ============================================================
 *  内部数据结构
 * ============================================================ */

/**
 * @brief RAID 成员私有数据
 */
typedef struct {
    bool is_valid;              ///< 是否有效
    bool is_online;             ///< 是否在线
    uint32_t ftl_instance_id;   ///< FTL 实例ID
    uint8_t *data_buffer;       ///< 数据缓冲区（模拟 FTL 存储）
    uint64_t buffer_size;       ///< 缓冲区大小（字节）
    uint64_t total_lpns;        ///< 总逻辑页数量
    uint64_t read_count;        ///< 读取次数
    uint64_t write_count;       ///< 写入次数
    uint64_t error_count;       ///< 错误次数
} raid_member_priv_t;

/**
 * @brief RAID 控制器私有数据
 */
typedef struct {
    bool is_initialized;        ///< 是否初始化
    raid_config_t config;       ///< RAID 配置
    raid_state_t state;         ///< RAID 状态
    raid_member_priv_t members[RAID_MAX_MEMBERS];  ///< 成员数组
    raid_stats_t stats;         ///< 统计信息
    uint64_t logical_capacity;  ///< 逻辑容量（页）
} raid_controller_t;

/* ============================================================
 *  全局变量
 * ============================================================ */

/**
 * @brief RAID 控制器实例
 */
static raid_controller_t g_raid;

/* ============================================================
 *  内部函数
 * ============================================================ */

/**
 * @brief 计算逻辑容量
 * @return 逻辑页数量
 */
static uint64_t calculate_logical_capacity(void)
{
    uint32_t i = 0;
    uint64_t min_capacity = UINT64_MAX;
    uint64_t capacity = 0;

    /* 找到最小的成员容量 */
    for (i = 0; i < RAID_MAX_MEMBERS; i++) {
        if (g_raid.members[i].is_valid && g_raid.members[i].is_online) {
            if (g_raid.members[i].total_lpns < min_capacity) {
                min_capacity = g_raid.members[i].total_lpns;
            }
        }
    }

    if (min_capacity == UINT64_MAX) {
        return 0;
    }

    /* 根据 RAID 级别计算逻辑容量 */
    switch (g_raid.config.level) {
    case RAID_LEVEL_0:
        /* RAID 0：容量 = 成员数 × 最小容量 */
        capacity = min_capacity * g_raid.config.member_count;
        break;

    case RAID_LEVEL_1:
        /* RAID 1：容量 = 最小容量（镜像） */
        capacity = min_capacity;
        break;

    default:
        capacity = 0;
        break;
    }

    return capacity;
}

/**
 * @brief RAID 0 条带化映射：逻辑页 → 成员索引 + 成员内页号
 * @param[in] lpn 逻辑页号
 * @param[out] member_index 成员索引
 * @param[out] member_lpn 成员内页号
 */
static void raid0_map(uint64_t lpn, uint32_t *member_index, uint64_t *member_lpn)
{
    uint64_t stripe = 0;
    uint64_t offset = 0;

    /* 条带化：按条带大小分配到不同成员 */
    stripe = lpn / g_raid.config.stripe_size;
    offset = lpn % g_raid.config.stripe_size;

    *member_index = (uint32_t)(stripe % g_raid.config.member_count);
    *member_lpn = (stripe / g_raid.config.member_count) * g_raid.config.stripe_size + offset;
}

/**
 * @brief 成员读操作
 * @param[in] member_index 成员索引
 * @param[in] lpn 逻辑页号
 * @param[out] buf 数据缓冲区
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_INTERNAL 内部错误
 */
static ret_code_t member_read(uint32_t member_index, uint64_t lpn, uint8_t *buf)
{
    raid_member_priv_t *member = NULL;
    uint64_t offset = 0;

    if (member_index >= RAID_MAX_MEMBERS) {
        return RET_ERR_PARAM;
    }
    if (buf == NULL) {
        return RET_ERR_PARAM;
    }

    member = &g_raid.members[member_index];
    if (!member->is_valid || !member->is_online) {
        return RET_ERR_INTERNAL;
    }

    /* 检查页号范围 */
    if (lpn >= member->total_lpns) {
        return RET_ERR_PARAM;
    }

    /* 从缓冲区读取数据 */
    offset = lpn * NAND_PAGE_SIZE;
    memcpy(buf, member->data_buffer + offset, NAND_PAGE_SIZE);

    member->read_count++;
    g_raid.stats.total_reads++;
    g_raid.stats.total_read_bytes += NAND_PAGE_SIZE;

    return RET_OK;
}

/**
 * @brief 成员写操作
 * @param[in] member_index 成员索引
 * @param[in] lpn 逻辑页号
 * @param[in] buf 数据缓冲区
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_INTERNAL 内部错误
 */
static ret_code_t member_write(uint32_t member_index, uint64_t lpn, const uint8_t *buf)
{
    raid_member_priv_t *member = NULL;
    uint64_t offset = 0;

    if (member_index >= RAID_MAX_MEMBERS) {
        return RET_ERR_PARAM;
    }
    if (buf == NULL) {
        return RET_ERR_PARAM;
    }

    member = &g_raid.members[member_index];
    if (!member->is_valid || !member->is_online) {
        return RET_ERR_INTERNAL;
    }

    /* 检查页号范围 */
    if (lpn >= member->total_lpns) {
        return RET_ERR_PARAM;
    }

    /* 写入数据到缓冲区 */
    offset = lpn * NAND_PAGE_SIZE;
    memcpy(member->data_buffer + offset, buf, NAND_PAGE_SIZE);

    member->write_count++;
    g_raid.stats.total_writes++;
    g_raid.stats.total_write_bytes += NAND_PAGE_SIZE;

    return RET_OK;
}

/* ============================================================
 *  接口函数实现
 * ============================================================ */

ret_code_t raid_init(const raid_config_t *config)
{
    uint32_t i = 0;

    if (config == NULL) {
        return RET_ERR_PARAM;
    }
    if (config->level >= RAID_LEVEL_MAX) {
        return RET_ERR_PARAM;
    }
    if (config->member_count == 0 || config->member_count > RAID_MAX_MEMBERS) {
        return RET_ERR_PARAM;
    }

    /* RAID 1 要求成员数为偶数（通常是2的倍数） */
    if (config->level == RAID_LEVEL_1 && (config->member_count % 2) != 0) {
        return RET_ERR_PARAM;
    }

    if (g_raid.is_initialized) {
        return RET_OK;
    }

    /* 初始化 RAID 控制器 */
    memset(&g_raid, 0, sizeof(g_raid));
    g_raid.config = *config;
    g_raid.state = RAID_STATE_READY;

    /* 初始化成员数组 */
    for (i = 0; i < RAID_MAX_MEMBERS; i++) {
        g_raid.members[i].is_valid = false;
        g_raid.members[i].is_online = false;
    }

    g_raid.is_initialized = true;

    printf("[RAID] RAID 模块初始化完成: 级别=%u, 成员数=%u, 条带大小=%u 页\n",
           config->level, config->member_count, config->stripe_size);

    return RET_OK;
}

ret_code_t raid_deinit(void)
{
    uint32_t i = 0;

    if (!g_raid.is_initialized) {
        return RET_OK;
    }

    /* 释放成员缓冲区 */
    for (i = 0; i < RAID_MAX_MEMBERS; i++) {
        if (g_raid.members[i].data_buffer != NULL) {
            free(g_raid.members[i].data_buffer);
            g_raid.members[i].data_buffer = NULL;
        }
    }

    g_raid.is_initialized = false;
    g_raid.state = RAID_STATE_UNINIT;

    printf("[RAID] RAID 模块反初始化完成\n");

    return RET_OK;
}

ret_code_t raid_add_member(uint32_t member_index, uint32_t ftl_instance_id)
{
    raid_member_priv_t *member = NULL;
    uint64_t buffer_size = 0;

    if (!g_raid.is_initialized) {
        return RET_ERR_NOT_INIT;
    }
    if (member_index >= RAID_MAX_MEMBERS) {
        return RET_ERR_PARAM;
    }

    member = &g_raid.members[member_index];
    if (member->is_valid) {
        return RET_ERR_INTERNAL;
    }

    /* 计算缓冲区大小（模拟 1024 个页的 FTL 实例） */
    member->total_lpns = 1024;
    buffer_size = member->total_lpns * NAND_PAGE_SIZE;

    /* 分配数据缓冲区 */
    member->data_buffer = (uint8_t *)calloc(1, buffer_size);
    if (member->data_buffer == NULL) {
        return RET_ERR_NO_SPACE;
    }

    member->is_valid = true;
    member->is_online = true;
    member->ftl_instance_id = ftl_instance_id;
    member->buffer_size = buffer_size;
    member->read_count = 0;
    member->write_count = 0;
    member->error_count = 0;

    /* 更新逻辑容量 */
    g_raid.logical_capacity = calculate_logical_capacity();

    printf("[RAID] 添加成员 %u: FTL实例ID=%u, 容量=%llu 页\n",
           member_index, ftl_instance_id, (unsigned long long)member->total_lpns);

    return RET_OK;
}

ret_code_t raid_remove_member(uint32_t member_index)
{
    raid_member_priv_t *member = NULL;

    if (!g_raid.is_initialized) {
        return RET_ERR_NOT_INIT;
    }
    if (member_index >= RAID_MAX_MEMBERS) {
        return RET_ERR_PARAM;
    }

    member = &g_raid.members[member_index];
    if (!member->is_valid) {
        return RET_OK;
    }

    /* 释放缓冲区 */
    if (member->data_buffer != NULL) {
        free(member->data_buffer);
        member->data_buffer = NULL;
    }

    member->is_valid = false;
    member->is_online = false;

    /* 更新逻辑容量 */
    g_raid.logical_capacity = calculate_logical_capacity();

    printf("[RAID] 移除成员 %u\n", member_index);

    return RET_OK;
}

ret_code_t raid_read(uint64_t lpn, uint8_t *buf)
{
    uint32_t member_index = 0;
    uint64_t member_lpn = 0;
    uint32_t i = 0;
    ret_code_t ret = RET_OK;

    if (!g_raid.is_initialized) {
        return RET_ERR_NOT_INIT;
    }
    if (buf == NULL) {
        return RET_ERR_PARAM;
    }
    if (lpn >= g_raid.logical_capacity) {
        return RET_ERR_PARAM;
    }

    switch (g_raid.config.level) {
    case RAID_LEVEL_0:
        /* RAID 0：条带化读取 */
        raid0_map(lpn, &member_index, &member_lpn);
        ret = member_read(member_index, member_lpn, buf);
        break;

    case RAID_LEVEL_1:
        /* RAID 1：镜像读取，从第一个在线成员读取 */
        for (i = 0; i < g_raid.config.member_count; i++) {
            if (g_raid.members[i].is_valid && g_raid.members[i].is_online) {
                ret = member_read(i, lpn, buf);
                if (ret == RET_OK) {
                    break;
                }
            }
        }
        if (i >= g_raid.config.member_count) {
            ret = RET_ERR_INTERNAL;
        }
        break;

    default:
        ret = RET_ERR_NOT_SUPPORT;
        break;
    }

    if (ret != RET_OK) {
        g_raid.stats.error_count++;
    }

    return ret;
}

ret_code_t raid_write(uint64_t lpn, const uint8_t *buf)
{
    uint32_t member_index = 0;
    uint64_t member_lpn = 0;
    uint32_t i = 0;
    ret_code_t ret = RET_OK;

    if (!g_raid.is_initialized) {
        return RET_ERR_NOT_INIT;
    }
    if (buf == NULL) {
        return RET_ERR_PARAM;
    }
    if (lpn >= g_raid.logical_capacity) {
        return RET_ERR_PARAM;
    }

    switch (g_raid.config.level) {
    case RAID_LEVEL_0:
        /* RAID 0：条带化写入 */
        raid0_map(lpn, &member_index, &member_lpn);
        ret = member_write(member_index, member_lpn, buf);
        break;

    case RAID_LEVEL_1:
        /* RAID 1：镜像写入，写入所有在线成员 */
        for (i = 0; i < g_raid.config.member_count; i++) {
            if (g_raid.members[i].is_valid && g_raid.members[i].is_online) {
                ret = member_write(i, lpn, buf);
                if (ret != RET_OK) {
                    g_raid.members[i].error_count++;
                    break;
                }
            }
        }
        break;

    default:
        ret = RET_ERR_NOT_SUPPORT;
        break;
    }

    if (ret != RET_OK) {
        g_raid.stats.error_count++;
    }

    return ret;
}

raid_state_t raid_get_state(void)
{
    if (!g_raid.is_initialized) {
        return RAID_STATE_UNINIT;
    }
    return g_raid.state;
}

ret_code_t raid_get_config(raid_config_t *config)
{
    if (config == NULL) {
        return RET_ERR_PARAM;
    }
    if (!g_raid.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    *config = g_raid.config;
    return RET_OK;
}

ret_code_t raid_get_stats(raid_stats_t *stats)
{
    if (stats == NULL) {
        return RET_ERR_PARAM;
    }
    if (!g_raid.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    *stats = g_raid.stats;
    return RET_OK;
}

ret_code_t raid_get_member_info(uint32_t member_index, raid_member_t *member)
{
    raid_member_priv_t *member_priv = NULL;

    if (member == NULL) {
        return RET_ERR_PARAM;
    }
    if (member_index >= RAID_MAX_MEMBERS) {
        return RET_ERR_PARAM;
    }
    if (!g_raid.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    member_priv = &g_raid.members[member_index];

    member->is_valid = member_priv->is_valid;
    member->is_online = member_priv->is_online;
    member->ftl_instance_id = member_priv->ftl_instance_id;
    member->total_lpns = member_priv->total_lpns;
    member->read_count = member_priv->read_count;
    member->write_count = member_priv->write_count;
    member->error_count = member_priv->error_count;

    return RET_OK;
}

uint64_t raid_get_logical_capacity(void)
{
    if (!g_raid.is_initialized) {
        return 0;
    }
    return g_raid.logical_capacity;
}

void raid_print_status(void)
{
    uint32_t i = 0;
    raid_member_t member_info;
    const char *level_str = NULL;
    const char *state_str = NULL;

    if (!g_raid.is_initialized) {
        printf("[RAID] 模块未初始化\n");
        return;
    }

    /* RAID 级别字符串 */
    switch (g_raid.config.level) {
    case RAID_LEVEL_0:
        level_str = "RAID 0 (条带化)";
        break;
    case RAID_LEVEL_1:
        level_str = "RAID 1 (镜像)";
        break;
    default:
        level_str = "未知";
        break;
    }

    /* 状态字符串 */
    switch (g_raid.state) {
    case RAID_STATE_UNINIT:
        state_str = "未初始化";
        break;
    case RAID_STATE_READY:
        state_str = "就绪";
        break;
    case RAID_STATE_DEGRADED:
        state_str = "降级";
        break;
    case RAID_STATE_ERROR:
        state_str = "错误";
        break;
    default:
        state_str = "未知";
        break;
    }

    printf("\nRAID 状态信息:\n");
    printf("  RAID 级别:     %s\n", level_str);
    printf("  状态:          %s\n", state_str);
    printf("  成员数量:      %u\n", g_raid.config.member_count);
    printf("  条带大小:      %u 页\n", g_raid.config.stripe_size);
    printf("  逻辑容量:      %llu 页\n", (unsigned long long)g_raid.logical_capacity);
    printf("  总读取次数:    %llu\n", (unsigned long long)g_raid.stats.total_reads);
    printf("  总写入次数:    %llu\n", (unsigned long long)g_raid.stats.total_writes);
    printf("  总读取字节:    %llu\n", (unsigned long long)g_raid.stats.total_read_bytes);
    printf("  总写入字节:    %llu\n", (unsigned long long)g_raid.stats.total_write_bytes);
    printf("  错误次数:      %llu\n", (unsigned long long)g_raid.stats.error_count);

    printf("\n成员信息:\n");
    printf("  索引  状态    FTL实例ID  容量(页)  读次数  写次数  错误数\n");
    for (i = 0; i < g_raid.config.member_count; i++) {
        raid_get_member_info(i, &member_info);
        printf("  %-4u  %-6s  %-10u  %-8llu  %-6llu  %-6llu  %-6llu\n",
               i,
               member_info.is_online ? "在线" : "离线",
               member_info.ftl_instance_id,
               (unsigned long long)member_info.total_lpns,
               (unsigned long long)member_info.read_count,
               (unsigned long long)member_info.write_count,
               (unsigned long long)member_info.error_count);
    }
    printf("\n");
}
