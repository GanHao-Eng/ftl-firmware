/**
 * @file raid.c
 * @brief RAID 模块实现
 * @details  RAID（独立磁盘冗余阵列）模块实现。
 *          支持 RAID 0（条带化）和 RAID 1（镜像）两种级别。
 *          使用多个独立缓冲区模拟多个 FTL 实例，提供数据冗余和性能提升。
 *          RAID 0 通过条带化提升并发性能，RAID 1 通过镜像提供数据冗余。
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
 * @details 每个 RAID 成员对应一个模拟的 FTL 实例，包含独立的数据缓冲区
 */
typedef struct {
    bool is_valid;              ///< 成员是否有效（已添加）
    bool is_online;             ///< 成员是否在线（可读写）
    uint32_t ftl_instance_id;   ///< FTL 实例ID（用于标识和日志）
    uint8_t *data_buffer;       ///< 数据缓冲区（模拟 FTL 存储介质）
    uint64_t buffer_size;       ///< 缓冲区大小（字节）
    uint64_t total_lpns;        ///< 总逻辑页数量（容量）
    uint64_t read_count;        ///< 读取次数统计
    uint64_t write_count;       ///< 写入次数统计
    uint64_t error_count;       ///< 错误次数统计
} raid_member_priv_t;

/**
 * @brief RAID 控制器私有数据
 * @details 管理整个 RAID 阵列的配置、状态、成员和统计信息
 */
typedef struct {
    bool is_initialized;        ///< RAID 控制器是否初始化
    raid_config_t config;       ///< RAID 配置（级别、成员数、条带大小）
    raid_state_t state;         ///< RAID 状态（未初始化/就绪/降级/错误）
    raid_member_priv_t members[RAID_MAX_MEMBERS];  ///< 成员数组
    raid_stats_t stats;         ///< 全局统计信息
    uint64_t logical_capacity;  ///< 逻辑容量（页，对上层暴露的容量）
} raid_controller_t;

/* ============================================================
 *  全局变量
 * ============================================================ */

/**
 * @brief RAID 控制器全局实例
 * @details 单例模式，整个系统只有一个 RAID 控制器实例
 */
static raid_controller_t g_raid;

/* ============================================================
 *  内部函数
 * ============================================================ */

/**
 * @brief 计算逻辑容量
 * @return 逻辑页数量
 * @details 根据 RAID 级别和在线成员的最小容量计算逻辑容量。
 *          RAID 0：容量 = 成员数 × 最小容量（条带化）
 *          RAID 1：容量 = 最小容量（镜像，所有成员存相同数据）
 */
static uint64_t calculate_logical_capacity(void)
{
    uint32_t i = 0;
    uint64_t min_capacity = UINT64_MAX;
    uint64_t capacity = 0;

    /* 遍历所有有效在线成员，找到最小容量（RAID 容量由最小成员决定） */
    for (i = 0; i < RAID_MAX_MEMBERS; i++) {
        if (g_raid.members[i].is_valid && g_raid.members[i].is_online) {
            if (g_raid.members[i].total_lpns < min_capacity) {
                min_capacity = g_raid.members[i].total_lpns;
            }
        }
    }

    /* 没有有效成员时返回0 */
    if (min_capacity == UINT64_MAX) {
        return 0;
    }

    /* 根据 RAID 级别计算逻辑容量 */
    switch (g_raid.config.level) {
    case RAID_LEVEL_0:
        /* RAID 0：容量 = 成员数 × 最小容量（数据条带化分布到所有成员） */
        capacity = min_capacity * g_raid.config.member_count;
        break;

    case RAID_LEVEL_1:
        /* RAID 1：容量 = 最小容量（所有成员存储相同数据，容量不叠加） */
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
 * @param[out] member_index 成员索引（数据所在的成员）
 * @param[out] member_lpn 成员内页号（在该成员中的偏移）
 * @details 将连续的逻辑地址按条带大小分配到不同成员，
 *          实现并发访问提升性能。条带内连续地址在同一成员，
 *          条带间轮询分配到不同成员。
 */
static void raid0_map(uint64_t lpn, uint32_t *member_index, uint64_t *member_lpn)
{
    uint64_t stripe = 0;   ///< 条带号
    uint64_t offset = 0;   ///< 条带内偏移

    /* 计算条带号和条带内偏移 */
    stripe = lpn / g_raid.config.stripe_size;
    offset = lpn % g_raid.config.stripe_size;

    /* 条带号对成员数取模，决定数据在哪个成员 */
    *member_index = (uint32_t)(stripe % g_raid.config.member_count);

    /* 计算成员内页号：条带号/成员数 × 条带大小 + 条带内偏移 */
    *member_lpn = (stripe / g_raid.config.member_count) * g_raid.config.stripe_size + offset;
}

/**
 * @brief 成员读操作
 * @param[in] member_index 成员索引
 * @param[in] lpn 成员内逻辑页号
 * @param[out] buf 数据输出缓冲区（大小 >= NAND_PAGE_SIZE）
 * @retval RET_OK 读取成功
 * @retval RET_ERR_PARAM 参数错误（索引越界或缓冲区为空）
 * @retval RET_ERR_INTERNAL 成员无效或离线
 * @details 从指定成员的缓冲区中读取一页数据，并更新统计计数。
 *          这是 RAID 读写的底层操作，上层通过映射函数决定访问哪个成员。
 */
static ret_code_t member_read(uint32_t member_index, uint64_t lpn, uint8_t *buf)
{
    raid_member_priv_t *member = NULL;
    uint64_t offset = 0;

    /* 成员索引边界检查 */
    if (member_index >= RAID_MAX_MEMBERS) {
        return RET_ERR_PARAM;
    }

    /* 输出缓冲区空指针检查 */
    if (buf == NULL) {
        return RET_ERR_PARAM;
    }

    /* 获取成员指针 */
    member = &g_raid.members[member_index];

    /* 成员有效性和在线状态检查 */
    if (!member->is_valid || !member->is_online) {
        return RET_ERR_INTERNAL;
    }

    /* 页号范围检查 */
    if (lpn >= member->total_lpns) {
        return RET_ERR_PARAM;
    }

    /* 计算页在缓冲区中的字节偏移 */
    offset = lpn * NAND_PAGE_SIZE;

    /* 从缓冲区读取一页数据 */
    memcpy(buf, member->data_buffer + offset, NAND_PAGE_SIZE);

    /* 更新统计计数 */
    member->read_count++;
    g_raid.stats.total_reads++;
    g_raid.stats.total_read_bytes += NAND_PAGE_SIZE;

    return RET_OK;
}

/**
 * @brief 成员写操作
 * @param[in] member_index 成员索引
 * @param[in] lpn 成员内逻辑页号
 * @param[in] buf 数据输入缓冲区（大小 >= NAND_PAGE_SIZE）
 * @retval RET_OK 写入成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_INTERNAL 成员无效或离线
 * @details 向指定成员的缓冲区写入一页数据，并更新统计计数。
 *          RAID 1 镜像写入时会对所有在线成员调用此函数。
 */
static ret_code_t member_write(uint32_t member_index, uint64_t lpn, const uint8_t *buf)
{
    raid_member_priv_t *member = NULL;
    uint64_t offset = 0;

    /* 成员索引边界检查 */
    if (member_index >= RAID_MAX_MEMBERS) {
        return RET_ERR_PARAM;
    }

    /* 输入缓冲区空指针检查 */
    if (buf == NULL) {
        return RET_ERR_PARAM;
    }

    /* 获取成员指针 */
    member = &g_raid.members[member_index];

    /* 成员有效性和在线状态检查 */
    if (!member->is_valid || !member->is_online) {
        return RET_ERR_INTERNAL;
    }

    /* 页号范围检查 */
    if (lpn >= member->total_lpns) {
        return RET_ERR_PARAM;
    }

    /* 计算页在缓冲区中的字节偏移 */
    offset = lpn * NAND_PAGE_SIZE;

    /* 写入数据到缓冲区 */
    memcpy(member->data_buffer + offset, buf, NAND_PAGE_SIZE);

    /* 更新统计计数 */
    member->write_count++;
    g_raid.stats.total_writes++;
    g_raid.stats.total_write_bytes += NAND_PAGE_SIZE;

    return RET_OK;
}

/* ============================================================
 *  接口函数实现
 * ============================================================ */

/**
 * @brief 初始化 RAID 控制器
 * @param[in] config RAID 配置指针（级别、成员数、条带大小）
 * @retval RET_OK 初始化成功
 * @retval RET_ERR_PARAM 参数错误（空指针、级别越界、成员数无效）
 * @details 初始化 RAID 控制器，设置 RAID 级别、成员数量和条带大小。
 *          RAID 1 要求成员数为偶数（通常为2的倍数）。
 *          重复调用是安全的，已初始化时直接返回成功。
 */
ret_code_t raid_init(const raid_config_t *config)
{
    uint32_t i = 0;

    /* 配置指针空检查 */
    if (config == NULL) {
        return RET_ERR_PARAM;
    }

    /* RAID 级别有效性检查 */
    if (config->level >= RAID_LEVEL_MAX) {
        return RET_ERR_PARAM;
    }

    /* 成员数量有效性检查（1 ~ RAID_MAX_MEMBERS） */
    if (config->member_count == 0 || config->member_count > RAID_MAX_MEMBERS) {
        return RET_ERR_PARAM;
    }

    /* RAID 1 要求成员数为偶数（镜像需要成对） */
    if (config->level == RAID_LEVEL_1 && (config->member_count % 2) != 0) {
        return RET_ERR_PARAM;
    }

    /* 已初始化则直接返回（幂等性保证） */
    if (g_raid.is_initialized) {
        return RET_OK;
    }

    /* 初始化 RAID 控制器结构体 */
    memset(&g_raid, 0, sizeof(g_raid));
    g_raid.config = *config;
    g_raid.state = RAID_STATE_READY;

    /* 初始化所有成员为无效状态 */
    for (i = 0; i < RAID_MAX_MEMBERS; i++) {
        g_raid.members[i].is_valid = false;
        g_raid.members[i].is_online = false;
    }

    g_raid.is_initialized = true;

    printf("[RAID] RAID 模块初始化完成: 级别=%u, 成员数=%u, 条带大小=%u 页\n",
           config->level, config->member_count, config->stripe_size);

    return RET_OK;
}

/**
 * @brief 反初始化 RAID 控制器
 * @retval RET_OK 反初始化成功
 * @details 释放所有成员的数据缓冲区，重置控制器状态。
 *          未初始化时直接返回成功。
 */
ret_code_t raid_deinit(void)
{
    uint32_t i = 0;

    /* 未初始化则直接返回 */
    if (!g_raid.is_initialized) {
        return RET_OK;
    }

    /* 遍历释放所有成员的数据缓冲区 */
    for (i = 0; i < RAID_MAX_MEMBERS; i++) {
        if (g_raid.members[i].data_buffer != NULL) {
            free(g_raid.members[i].data_buffer);
            g_raid.members[i].data_buffer = NULL;
        }
    }

    /* 重置控制器状态 */
    g_raid.is_initialized = false;
    g_raid.state = RAID_STATE_UNINIT;

    printf("[RAID] RAID 模块反初始化完成\n");

    return RET_OK;
}

/**
 * @brief 添加 RAID 成员
 * @param[in] member_index 成员索引（0 ~ RAID_MAX_MEMBERS-1）
 * @param[in] ftl_instance_id FTL 实例ID（用于标识）
 * @retval RET_OK 添加成功
 * @retval RET_ERR_NOT_INIT RAID 未初始化
 * @retval RET_ERR_PARAM 成员索引越界
 * @retval RET_ERR_INTERNAL 成员已存在
 * @retval RET_ERR_NO_SPACE 内存分配失败
 * @details 为指定索引添加一个 RAID 成员，分配独立的数据缓冲区
 *          模拟 FTL 实例。当前实现固定每个成员 1024 页容量。
 *          添加成员后自动重新计算逻辑容量。
 */
ret_code_t raid_add_member(uint32_t member_index, uint32_t ftl_instance_id)
{
    raid_member_priv_t *member = NULL;
    uint64_t buffer_size = 0;

    /* 初始化检查 */
    if (!g_raid.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 成员索引边界检查 */
    if (member_index >= RAID_MAX_MEMBERS) {
        return RET_ERR_PARAM;
    }

    /* 获取成员指针 */
    member = &g_raid.members[member_index];

    /* 成员已存在检查 */
    if (member->is_valid) {
        return RET_ERR_INTERNAL;
    }

    /* 设置成员容量（模拟 1024 个页的 FTL 实例） */
    member->total_lpns = 1024;
    buffer_size = member->total_lpns * NAND_PAGE_SIZE;

    /* 分配数据缓冲区（calloc 初始化为0） */
    member->data_buffer = (uint8_t *)calloc(1, buffer_size);
    if (member->data_buffer == NULL) {
        return RET_ERR_NO_SPACE;
    }

    /* 初始化成员状态 */
    member->is_valid = true;
    member->is_online = true;
    member->ftl_instance_id = ftl_instance_id;
    member->buffer_size = buffer_size;
    member->read_count = 0;
    member->write_count = 0;
    member->error_count = 0;

    /* 成员变化后重新计算逻辑容量 */
    g_raid.logical_capacity = calculate_logical_capacity();

    printf("[RAID] 添加成员 %u: FTL实例ID=%u, 容量=%llu 页\n",
           member_index, ftl_instance_id, (unsigned long long)member->total_lpns);

    return RET_OK;
}

/**
 * @brief 移除 RAID 成员
 * @param[in] member_index 成员索引
 * @retval RET_OK 移除成功
 * @retval RET_ERR_NOT_INIT RAID 未初始化
 * @retval RET_ERR_PARAM 成员索引越界
 * @details 移除指定索引的 RAID 成员，释放其数据缓冲区。
 *          移除成员后自动重新计算逻辑容量。
 *          成员不存在时直接返回成功（幂等性）。
 */
ret_code_t raid_remove_member(uint32_t member_index)
{
    raid_member_priv_t *member = NULL;

    /* 初始化检查 */
    if (!g_raid.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 成员索引边界检查 */
    if (member_index >= RAID_MAX_MEMBERS) {
        return RET_ERR_PARAM;
    }

    /* 获取成员指针 */
    member = &g_raid.members[member_index];

    /* 成员不存在则直接返回 */
    if (!member->is_valid) {
        return RET_OK;
    }

    /* 释放成员数据缓冲区 */
    if (member->data_buffer != NULL) {
        free(member->data_buffer);
        member->data_buffer = NULL;
    }

    /* 标记成员为无效离线 */
    member->is_valid = false;
    member->is_online = false;

    /* 成员变化后重新计算逻辑容量 */
    g_raid.logical_capacity = calculate_logical_capacity();

    printf("[RAID] 移除成员 %u\n", member_index);

    return RET_OK;
}

/**
 * @brief RAID 读操作
 * @param[in] lpn 逻辑页号
 * @param[out] buf 数据输出缓冲区
 * @retval RET_OK 读取成功
 * @retval RET_ERR_NOT_INIT RAID 未初始化
 * @retval RET_ERR_PARAM 参数错误或页号越界
 * @retval RET_ERR_INTERNAL 读取失败（所有成员都不可用）
 * @retval RET_ERR_NOT_SUPPORT 不支持的 RAID 级别
 * @details 从 RAID 阵列读取一页数据。
 *          RAID 0：通过条带化映射找到对应成员直接读取。
 *          RAID 1：从第一个在线成员读取（镜像数据相同，任一成员均可）。
 */
ret_code_t raid_read(uint64_t lpn, uint8_t *buf)
{
    uint32_t member_index = 0;
    uint64_t member_lpn = 0;
    uint32_t i = 0;
    ret_code_t ret = RET_OK;

    /* 初始化检查 */
    if (!g_raid.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 输出缓冲区空指针检查 */
    if (buf == NULL) {
        return RET_ERR_PARAM;
    }

    /* 逻辑页号范围检查 */
    if (lpn >= g_raid.logical_capacity) {
        return RET_ERR_PARAM;
    }

    /* 根据 RAID 级别执行不同的读策略 */
    switch (g_raid.config.level) {
    case RAID_LEVEL_0:
        /* RAID 0：条带化映射后从对应成员读取 */
        raid0_map(lpn, &member_index, &member_lpn);
        ret = member_read(member_index, member_lpn, buf);
        break;

    case RAID_LEVEL_1:
        /* RAID 1：镜像读取，遍历找到第一个在线成员读取 */
        for (i = 0; i < g_raid.config.member_count; i++) {
            if (g_raid.members[i].is_valid && g_raid.members[i].is_online) {
                ret = member_read(i, lpn, buf);
                if (ret == RET_OK) {
                    /* 读取成功，跳出循环 */
                    break;
                }
            }
        }
        /* 所有成员都读取失败 */
        if (i >= g_raid.config.member_count) {
            ret = RET_ERR_INTERNAL;
        }
        break;

    default:
        ret = RET_ERR_NOT_SUPPORT;
        break;
    }

    /* 读取失败时增加错误计数 */
    if (ret != RET_OK) {
        g_raid.stats.error_count++;
    }

    return ret;
}

/**
 * @brief RAID 写操作
 * @param[in] lpn 逻辑页号
 * @param[in] buf 数据输入缓冲区
 * @retval RET_OK 写入成功
 * @retval RET_ERR_NOT_INIT RAID 未初始化
 * @retval RET_ERR_PARAM 参数错误或页号越界
 * @retval RET_ERR_NOT_SUPPORT 不支持的 RAID 级别
 * @details 向 RAID 阵列写入一页数据。
 *          RAID 0：通过条带化映射写入对应成员（只有一份数据）。
 *          RAID 1：镜像写入所有在线成员（多份冗余，任一成员损坏不丢数据）。
 */
ret_code_t raid_write(uint64_t lpn, const uint8_t *buf)
{
    uint32_t member_index = 0;
    uint64_t member_lpn = 0;
    uint32_t i = 0;
    ret_code_t ret = RET_OK;

    /* 初始化检查 */
    if (!g_raid.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 输入缓冲区空指针检查 */
    if (buf == NULL) {
        return RET_ERR_PARAM;
    }

    /* 逻辑页号范围检查 */
    if (lpn >= g_raid.logical_capacity) {
        return RET_ERR_PARAM;
    }

    /* 根据 RAID 级别执行不同的写策略 */
    switch (g_raid.config.level) {
    case RAID_LEVEL_0:
        /* RAID 0：条带化映射后写入对应成员（只有一份） */
        raid0_map(lpn, &member_index, &member_lpn);
        ret = member_write(member_index, member_lpn, buf);
        break;

    case RAID_LEVEL_1:
        /* RAID 1：镜像写入，写入所有在线成员（多份冗余） */
        for (i = 0; i < g_raid.config.member_count; i++) {
            if (g_raid.members[i].is_valid && g_raid.members[i].is_online) {
                ret = member_write(i, lpn, buf);
                if (ret != RET_OK) {
                    /* 写入失败，记录该成员错误并中止（实际应继续写其他成员） */
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

    /* 写入失败时增加全局错误计数 */
    if (ret != RET_OK) {
        g_raid.stats.error_count++;
    }

    return ret;
}

/**
 * @brief 获取 RAID 状态
 * @return RAID 状态（未初始化/就绪/降级/错误）
 * @details 未初始化时返回 RAID_STATE_UNINIT。
 */
raid_state_t raid_get_state(void)
{
    if (!g_raid.is_initialized) {
        return RAID_STATE_UNINIT;
    }
    return g_raid.state;
}

/**
 * @brief 获取 RAID 配置
 * @param[out] config 配置输出缓冲区
 * @retval RET_OK 获取成功
 * @retval RET_ERR_PARAM 输出缓冲区为空
 * @retval RET_ERR_NOT_INIT RAID 未初始化
 * @details 返回复制的 RAID 配置，包括级别、成员数、条带大小。
 */
ret_code_t raid_get_config(raid_config_t *config)
{
    /* 输出缓冲区空指针检查 */
    if (config == NULL) {
        return RET_ERR_PARAM;
    }

    /* 初始化检查 */
    if (!g_raid.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 复制配置 */
    *config = g_raid.config;
    return RET_OK;
}

/**
 * @brief 获取 RAID 统计信息
 * @param[out] stats 统计信息输出缓冲区
 * @retval RET_OK 获取成功
 * @retval RET_ERR_PARAM 输出缓冲区为空
 * @retval RET_ERR_NOT_INIT RAID 未初始化
 * @details 返回全局统计信息，包括总读写次数、总读写字节数、错误次数。
 */
ret_code_t raid_get_stats(raid_stats_t *stats)
{
    /* 输出缓冲区空指针检查 */
    if (stats == NULL) {
        return RET_ERR_PARAM;
    }

    /* 初始化检查 */
    if (!g_raid.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 复制统计信息 */
    *stats = g_raid.stats;
    return RET_OK;
}

/**
 * @brief 获取指定成员的信息
 * @param[in] member_index 成员索引
 * @param[out] member 成员信息输出缓冲区
 * @retval RET_OK 获取成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_INIT RAID 未初始化
 * @details 返回指定成员的详细信息，包括有效性、在线状态、FTL实例ID、
 *          容量、读写次数和错误次数。
 */
ret_code_t raid_get_member_info(uint32_t member_index, raid_member_t *member)
{
    raid_member_priv_t *member_priv = NULL;

    /* 输出缓冲区空指针检查 */
    if (member == NULL) {
        return RET_ERR_PARAM;
    }

    /* 成员索引边界检查 */
    if (member_index >= RAID_MAX_MEMBERS) {
        return RET_ERR_PARAM;
    }

    /* 初始化检查 */
    if (!g_raid.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 获取成员私有数据指针 */
    member_priv = &g_raid.members[member_index];

    /* 复制成员信息到输出缓冲区 */
    member->is_valid = member_priv->is_valid;
    member->is_online = member_priv->is_online;
    member->ftl_instance_id = member_priv->ftl_instance_id;
    member->total_lpns = member_priv->total_lpns;
    member->read_count = member_priv->read_count;
    member->write_count = member_priv->write_count;
    member->error_count = member_priv->error_count;

    return RET_OK;
}

/**
 * @brief 获取 RAID 逻辑容量
 * @return 逻辑容量（页），未初始化时返回0
 * @details 返回对上层暴露的逻辑容量，单位为页。
 *          RAID 0：成员数 × 最小容量
 *          RAID 1：最小容量
 */
uint64_t raid_get_logical_capacity(void)
{
    if (!g_raid.is_initialized) {
        return 0;
    }
    return g_raid.logical_capacity;
}

/**
 * @brief 打印 RAID 状态信息
 * @details 以可读格式打印 RAID 控制器的整体状态和每个成员的详细信息，
 *          包括 RAID 级别、状态、成员数、条带大小、逻辑容量、统计信息
 *          和各成员的在线状态、容量、读写计数。用于调试和监控。
 */
void raid_print_status(void)
{
    uint32_t i = 0;
    raid_member_t member_info;
    const char *level_str = NULL;
    const char *state_str = NULL;

    /* 初始化检查 */
    if (!g_raid.is_initialized) {
        printf("[RAID] 模块未初始化\n");
        return;
    }

    /* RAID 级别字符串转换 */
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

    /* 状态字符串转换 */
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

    /* 打印 RAID 整体状态 */
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

    /* 打印各成员详细信息 */
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
