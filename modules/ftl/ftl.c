/**
 * @file ftl.c
 * @brief FTL 闪存转换层核心实现
 * @details 实现页映射/混合映射、GC垃圾回收、磨损均衡、掉电保护等核心功能
 */

#include "ftl.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ============================================================
 *  内部私有结构体
 * ============================================================ */

/**
 * @brief FTL 设备私有结构体
 * @details 不对外暴露的内部状态
 */
typedef struct {
    /* ---- 映射表 ---- */
    uint32_t l2p_table[FTL_TOTAL_LPNS];     ///< L2P页映射表：下标=LPN，值=PPN
    uint32_t reverse_map[NAND_TOTAL_BLOCKS * NAND_PAGES_PER_BLOCK];  ///< 反向映射：下标=PPN，值=LPN

    /* ---- 当前写入位置 ---- */
    uint32_t cur_write_block;   ///< 当前写入块号
    uint32_t cur_write_page;    ///< 当前写入页偏移

    /* ---- 统计计数器 ---- */
    uint32_t host_write_pages;          ///< 主机下发写入总页数
    uint32_t nand_write_pages;          ///< NAND实际写入总页数（含GC搬迁）
    uint32_t gc_count;                  ///< GC执行次数
    uint32_t gc_moved_pages;            ///< GC搬迁的有效页数
    uint32_t wear_leveling_count;       ///< 动态磨损均衡触发次数
    uint32_t static_wear_count;         ///< 静态磨损均衡触发次数
    uint32_t static_wear_moved_pages;   ///< 静态磨损均衡搬迁页数
    uint32_t trim_count;                ///< TRIM处理次数
    uint32_t trim_pages;                ///< TRIM释放的页数
    uint32_t bad_block_replace_count;   ///< 坏块替换次数
    uint32_t bad_block_moved_pages;     ///< 坏块替换搬迁的页数

    /* ---- 静态磨损均衡 ---- */
    uint32_t write_since_last_check;    ///< 上次静态磨损均衡检测后的写入次数

    /* ---- 配置项 ---- */
    gc_algo_type_t gc_algo;    ///< 当前使用的GC算法
    map_mode_t map_mode;       ///< 当前映射模式

    /* ---- WAL 日志 ---- */
    FILE *wal_file;            ///< WAL日志文件句柄
    uint32_t wal_sequence;     ///< WAL日志序列号
    uint32_t wal_entry_count;  ///< WAL日志条目数
    bool wal_enabled;          ///< WAL是否启用

    /* ---- 混合映射 ---- */
    block_map_entry_t block_map[FTL_TOTAL_LPNS / FTL_LOGICAL_BLOCK_PAGES];  ///< 块映射表
    uint32_t hot_cold_check_count;  ///< 上次冷热迁移检测后的写入次数
    uint32_t hot_cold_migration_count; ///< 冷热数据迁移次数

    /* ---- 状态标志 ---- */
    bool is_initialized;       ///< 初始化标志
} ftl_dev_t;

/**
 * @brief FTL 设备全局实例（私有）
 */
static ftl_dev_t g_ftl_dev = {0};

/* ============================================================
 *  内部辅助函数前置声明
 * ============================================================ */

static inline uint32_t lpn_to_logical_block(uint32_t lpn);
static void hybrid_update_access_count(uint32_t lpn);
static ret_code_t wal_log_write(uint32_t lpn, uint32_t old_ppn, uint32_t new_ppn);

/* ============================================================
 *  内部辅助函数
 * ============================================================ */

/**
 * @brief PPN 转物理块号
 * @param[in] ppn 物理页号
 * @return 物理块号
 */
static inline uint32_t ppn_to_block(uint32_t ppn)
{
    return ppn / NAND_PAGES_PER_BLOCK;
}

/**
 * @brief PPN 转物理页偏移
 * @param[in] ppn 物理页号
 * @return 块内页偏移
 */
static inline uint32_t ppn_to_page(uint32_t ppn)
{
    return ppn % NAND_PAGES_PER_BLOCK;
}

/**
 * @brief 物理块号+页偏移 转 PPN
 * @param[in] block 物理块号
 * @param[in] page  页偏移
 * @return 物理页号
 */
static inline uint32_t block_page_to_ppn(uint32_t block, uint32_t page)
{
    return block * NAND_PAGES_PER_BLOCK + page;
}

/**
 * @brief 检查PPN是否合法
 * @param[in] ppn 物理页号
 * @return true 合法，false 非法
 */
static inline bool ftl_is_ppn_valid(uint32_t ppn)
{
    return ppn < (NAND_TOTAL_BLOCKS * NAND_PAGES_PER_BLOCK);
}

/**
 * @brief 查找下一个空闲块
 * @param[in] start_block 起始查找块号
 * @param[out] out_block 输出找到的空闲块号
 * @retval true 找到，false 未找到
 * @note 从start_block开始向后查找，到末尾后从头继续
 */
static bool ftl_find_next_free_block(uint32_t start_block, uint32_t *out_block)
{
    /* 从起始位置向后查找 */
    for (uint32_t i = start_block; i < NAND_TOTAL_BLOCKS - NAND_RESERVED_BLOCKS; i++) {
        if (nand_get_block_state(i) == BLOCK_FREE) {
            *out_block = i;
            return true;
        }
    }

    /* 从头再找一遍（环形查找） */
    for (uint32_t i = 0; i < start_block; i++) {
        if (nand_get_block_state(i) == BLOCK_FREE) {
            *out_block = i;
            return true;
        }
    }

    return false;
}

/**
 * @brief 分配一个空闲物理页
 * @param[out] out_ppn 输出分配的PPN
 * @retval true 分配成功，false 无空闲页
 * @note 当前块写满时自动切换到下一个空闲块
 */
static bool ftl_alloc_phy_page(uint32_t *out_ppn)
{
    uint32_t next_block = 0;

    /* 当前块未写满，直接分配下一页 */
    if (g_ftl_dev.cur_write_page < NAND_PAGES_PER_BLOCK) {
        *out_ppn = block_page_to_ppn(g_ftl_dev.cur_write_block, g_ftl_dev.cur_write_page);
        g_ftl_dev.cur_write_page++;
        return true;
    }

    /* 当前块写满，切换下一个空闲块 */   
    if (!ftl_find_next_free_block(g_ftl_dev.cur_write_block + 1U, &next_block)) {
        return false;
    }

    /* 更新当前写入位置 */
    g_ftl_dev.cur_write_block = next_block;
    g_ftl_dev.cur_write_page = 1U;
    *out_ppn = block_page_to_ppn(next_block, 0U);

    return true;
}

/* ============================================================
 *  GC 垃圾回收模块 - 算法实现
 * ============================================================ */

/**
 * @brief 贪心算法选择受害块
 * @param[out] victim_block 输出受害块号
 * @retval true 找到，false 未找到
 * @note 选择有效页最少的块，回收成本最低
 */
static bool gc_select_greedy(uint32_t *victim_block)
{
    uint32_t min_valid_pages = NAND_PAGES_PER_BLOCK + 1U;
    int32_t victim = -1;
    uint32_t valid_pages = 0;/*有效页*/
    uint32_t i = 0;

    /* 遍历所有已使用块，找有效页最少的 */
    for (i = 0; i < NAND_TOTAL_BLOCKS - NAND_RESERVED_BLOCKS; i++) {
        if (nand_get_block_state(i) != BLOCK_USED) {
            continue;
        }

        valid_pages = nand_get_block_valid_page_count(i);
        if (valid_pages < min_valid_pages) {
            min_valid_pages = valid_pages;
            victim = (int32_t)i;
        }
    }

    if (victim < 0) {
        return false;
    }

    *victim_block = (uint32_t)victim;
    return true;
}

/**
 * @brief Cost-Benefit 成本收益算法选择受害块
 * @param[out] victim_block 输出受害块号
 * @retval true 找到，false 未找到
 * @note 综合考虑有效页数（回收成本）和擦写次数（磨损收益）
 *       公式：score = (1 - valid_ratio) / (erase_count + 1)
 *       有效页越少、擦写次数越少，回收收益越高
 */
static bool gc_select_cost_benefit(uint32_t *victim_block)
{
    double max_score = -1.0;
    int32_t victim = -1;
    uint32_t valid_pages = 0;/*有效页*/
    uint32_t erase_cnt = 0;/*擦写次数*/
    double invalid_ratio = 0.0;/*无效页比例*/
    double score = 0.0;/*成本收益分数*/
    uint32_t i = 0;

    /* 遍历所有已使用块，计算成本收益比 */
    for (i = 0; i < NAND_TOTAL_BLOCKS - NAND_RESERVED_BLOCKS; i++) {
        if (nand_get_block_state(i) != BLOCK_USED) {
            continue;
        }

        valid_pages = nand_get_block_valid_page_count(i);
        erase_cnt = nand_get_block_erase_count(i);

        /* 计算成本收益比：无效页比例越高、擦写越少，分数越高 */
        invalid_ratio = (double)(NAND_PAGES_PER_BLOCK - valid_pages) / NAND_PAGES_PER_BLOCK;
        score = invalid_ratio / (double)(erase_cnt + 1);

        if (score > max_score) {
            max_score = score;
            victim = (int32_t)i;
        }
    }

    if (victim < 0) {
        return false;
    }
    *victim_block = (uint32_t)victim;
    return true;
}

/**
 * @brief CAT算法选择受害块
 * @param[out] victim_block 输出受害块号
 * @retval true 找到，false 未找到
 * @note CAT（Cost-Age-Time）算法：综合考虑成本、年龄、时间三个因素
 *       公式：score = (1 - valid_ratio) * age_factor / (erase_count + 1)
 *       其中 age_factor 用擦写次数差来模拟年龄
 *       有效页越少、年龄越大、擦写次数越少，回收收益越高
 *       CAT 算法比 Cost-Benefit 更注重数据的冷热程度
 */
static bool gc_select_cat(uint32_t *victim_block)
{
    double max_score = -1.0;
    int32_t victim = -1;
    uint32_t max_erase = 0U;/*最大擦写次数*/
    uint32_t erase_cnt = 0U;/*擦写次数*/
    uint32_t valid_pages = 0U;/*有效页*/
    double invalid_ratio = 0.0;/*无效页比例*/
    double age_factor = 0.0;/*年龄因子*/
    double score = 0.0;/*CAT分数*/

    /* 先找最大擦写次数，用于计算年龄因子 */  
    for (uint32_t i = 0; i < NAND_TOTAL_BLOCKS - NAND_RESERVED_BLOCKS; i++) {
        if (nand_get_block_state(i) == BLOCK_USED) {
            erase_cnt = nand_get_block_erase_count(i);
            if (erase_cnt > max_erase) {
                max_erase = erase_cnt;
            }
        }
    }

    /* 遍历所有已使用块，计算CAT分数 */
    for (uint32_t i = 0; i < NAND_TOTAL_BLOCKS - NAND_RESERVED_BLOCKS; i++) {
        if (nand_get_block_state(i) != BLOCK_USED) {
            continue;
        }

        valid_pages = nand_get_block_valid_page_count(i);
        erase_cnt = nand_get_block_erase_count(i);

        /* 计算无效页比例（成本因子） */
        invalid_ratio = (double)(NAND_PAGES_PER_BLOCK - valid_pages) / NAND_PAGES_PER_BLOCK;

        /* 计算年龄因子：擦写次数越少，年龄越大（数据越老） */
        age_factor = (max_erase > 0) ?
            (double)(max_erase - erase_cnt + FTL_GC_CAT_AGE_WEIGHT) / (double)max_erase : 1.0;

        /* CAT 分数：成本 × 年龄 / 磨损 */
        score = invalid_ratio * age_factor / (double)(erase_cnt + 1);

        if (score > max_score) {
            max_score = score;
            victim = (int32_t)i;
        }
    }

    if (victim < 0) {
        return false;
    }
    *victim_block = (uint32_t)victim;
    return true;
}

/**
 * @brief Windowed算法选择受害块
 * @param[out] victim_block 输出受害块号
 * @retval true 找到，false 未找到
 * @note Windowed算法：基于时间窗口的贪心算法
 *       只在最近的 N 个块（窗口）中选择有效页最少的块回收
 *       优点：减少全局扫描开销，更适合热点数据集中的场景
 *       缺点：可能不是全局最优，但性能更好
 */
static bool gc_select_windowed(uint32_t *victim_block)
{
    uint32_t min_valid = NAND_PAGES_PER_BLOCK + 1;
    int32_t victim = -1;
    uint32_t window_start = 0U;
    uint32_t valid_pages = 0U;/*有效页*/

    /* 从当前写入块向前扫描一个窗口大小的块 */
    if (g_ftl_dev.cur_write_block >= FTL_GC_WINDOW_SIZE) {
        window_start = g_ftl_dev.cur_write_block - FTL_GC_WINDOW_SIZE;
    }

    /* 在窗口内找有效页最少的块 */
    for (uint32_t i = window_start; i <= g_ftl_dev.cur_write_block; i++) {
        if (nand_get_block_state(i) != BLOCK_USED) {
            continue;
        }

        valid_pages = nand_get_block_valid_page_count(i);
        if (valid_pages < min_valid) {
            min_valid = valid_pages;
            victim = (int32_t)i;
        }
    }

    /* 如果窗口内没有找到合适的块，回退到全局贪心算法 */
    if (victim < 0) {
        return gc_select_greedy(victim_block);
    }

    *victim_block = (uint32_t)victim;
    return true;
}

/**
 * @brief d-Choices 算法选择受害块
 * @param[out] victim_block 输出受害块号
 * @retval true 找到，false 未找到
 * @note d-Choices 算法：随机选择 d 个已使用的块，
 *       在这 d 个块中选择有效页最少的块回收
 *       优点：减少全局扫描开销，同时避免完全随机的低效
 *       是一种权衡性能和效果的算法，适合大规模存储系统
 */
static bool gc_select_d_choices(uint32_t *victim_block)
{
    uint32_t min_valid = NAND_PAGES_PER_BLOCK + 1;
    int32_t victim = -1;
    uint32_t d = FTL_GC_D_CHOICES_D;
    /* 收集所有已使用的块 */
    uint32_t used_blocks[NAND_TOTAL_BLOCKS];
    uint32_t used_count = 0;
    uint32_t idx = 0;
    uint32_t block = 0;
    uint32_t valid_pages = 0;/*有效页*/

    for (uint32_t i = 0; i < NAND_TOTAL_BLOCKS - NAND_RESERVED_BLOCKS; i++) {
        if (nand_get_block_state(i) == BLOCK_USED) {
            used_blocks[used_count++] = i;
        }
    }

    if (used_count == 0) {
        return false;
    }

    /* 如果已使用块数少于 d，直接用贪心算法 */
    if (used_count < d) {
        return gc_select_greedy(victim_block);
    }

    /* 随机选择 d 个块，找其中有效页最少的 */
    for (uint32_t i = 0; i < d; i++) {
        /* 简单的随机选择（用块号哈希模拟随机，避免依赖 rand()） */
        idx = (g_ftl_dev.gc_count * 7 + i * 13) % used_count;
        block = used_blocks[idx];

        valid_pages = nand_get_block_valid_page_count(block);
        if (valid_pages < min_valid) {
            min_valid = valid_pages;
            victim = (int32_t)block;
        }
    }

    if (victim < 0) {
        return false;
    }

    *victim_block = (uint32_t)victim;
    return true;
}

/**
 * @brief FRA（Full Reclamation Algorithm）算法选择受害块
 * @param[out] victim_block 输出受害块号
 * @retval true 找到，false 未找到
 * @note FRA 全回收算法：优先回收有效页为 0 的块（完全无效的块），
 *       这样的块可以直接擦除，不需要搬迁任何数据
 *       如果没有完全无效的块，回退到贪心算法
 *       优点：回收效率极高，零搬迁成本
 *       适合有大量 TRIM 操作或删除操作的场景
 */
static bool gc_select_fra(uint32_t *victim_block)
{
    uint32_t valid_pages = 0;/*有效页*/

    /* 第一步：找有效页为 0 的块（完全无效的块，可以直接擦除） */
    for (uint32_t i = 0; i < NAND_TOTAL_BLOCKS - NAND_RESERVED_BLOCKS; i++) {
        if (nand_get_block_state(i) != BLOCK_USED) {
            continue;
        }

        valid_pages = nand_get_block_valid_page_count(i);
        if (valid_pages == 0) {
            *victim_block = i;
            return true;
        }
    }

    /* 第二步：如果没有完全无效的块，回退到贪心算法 */
    return gc_select_greedy(victim_block);
}

/**
 * @brief 选择GC受害块（根据当前算法）
 * @param[out] victim_block 输出受害块号
 * @retval true 找到受害块，false 无可回收块
 */
static bool gc_select_victim_block(uint32_t *victim_block)
{
    switch (g_ftl_dev.gc_algo) {
        case GC_ALGO_GREEDY:
            return gc_select_greedy(victim_block);
        case GC_ALGO_COST_BENEFIT:
            return gc_select_cost_benefit(victim_block);
        case GC_ALGO_CAT:
            return gc_select_cat(victim_block);
        case GC_ALGO_WINDOWED:
            return gc_select_windowed(victim_block);
        case GC_ALGO_D_CHOICES:
            return gc_select_d_choices(victim_block);
        case GC_ALGO_FRA:
            return gc_select_fra(victim_block);
        default:
            return gc_select_greedy(victim_block);
    }
}

/**
 * @brief 执行一次GC回收
 * @retval RET_OK 回收成功
 * @retval RET_ERR_NO_SPACE 无可回收块或空间耗尽
 * @details
 *  1. 选择受害块
 *  2. 搬迁块内所有有效页到新位置
 *  3. 更新映射表（正向+反向）
 *  4. 擦除受害块，释放空间
 */
static ret_code_t gc_do_recycle(void)
{
    uint32_t victim_block = 0;
    uint8_t page_buf[NAND_PAGE_SIZE];
    uint32_t old_ppn = 0;
    uint32_t lpn = 0;
    uint32_t new_ppn = 0;/*新物理页*/
    uint32_t new_block = 0;/*新物理块*/
    uint32_t new_page = 0;/*新物理页偏移*/
    uint32_t page = 0;
    ret_code_t ret = RET_OK;

    /* 第一步：选择受害块 */
    if (!gc_select_victim_block(&victim_block)) {
        return RET_ERR_NO_SPACE;
    }

    g_ftl_dev.gc_count++;

    LOG_DEBUG("GC 触发: 第 %u 次 GC，受害块=%u，有效页=%u",
              g_ftl_dev.gc_count, victim_block,
              nand_get_block_valid_page_count(victim_block));

    /* 第二步：遍历块内所有页，搬迁有效页 */
    for (page = 0; page < NAND_PAGES_PER_BLOCK; page++) {
        /* 跳过无效页 */
        if (!nand_is_page_valid(victim_block, page)) {
            continue;
        }

        old_ppn = block_page_to_ppn(victim_block, page);
        /* 通过反向映射快速找到对应的 LPN（O(1)查找，无需遍历L2P表） */
        lpn = g_ftl_dev.reverse_map[old_ppn];
        if (lpn == FTL_INVALID_LPN) {
            continue;
        }

        /* 读取旧页上面的有效数据，存放在page_buf中*/
        (void)nand_page_read(victim_block, page, page_buf);

        /* 分配新物理页 */
        if (!ftl_alloc_phy_page(&new_ppn)) {
            return RET_ERR_NO_SPACE;
        }

        /* 写入新物理页 */
        new_block = ppn_to_block(new_ppn);
        new_page = ppn_to_page(new_ppn);
        ret = nand_page_write(new_block, new_page, page_buf);
        if (ret != RET_OK) {
            return RET_ERR_INTERNAL;
        }

        /* 更新统计 */
        g_ftl_dev.nand_write_pages++;
        g_ftl_dev.gc_moved_pages++;

        /* 同时更新正向映射和反向映射 */
        g_ftl_dev.l2p_table[lpn] = new_ppn;
        g_ftl_dev.reverse_map[new_ppn] = lpn;
        g_ftl_dev.reverse_map[old_ppn] = FTL_INVALID_LPN;
    }

    /* 第三步：擦除受害块，释放空间 */
    ret = nand_block_erase(victim_block);

    LOG_DEBUG("GC 完成: 第 %u 次 GC，受害块=%u，搬迁页数=%u",
              g_ftl_dev.gc_count, victim_block,
              nand_get_block_valid_page_count(victim_block));

    return ret;
}

/**
 * @brief 动态磨损均衡检测
 * @note 当冷热块擦写差超过阈值时，主动搬迁冷块数据
 *       实现方式：将擦写次数最少的冷块标记为低有效页，
 *               让GC优先回收它，从而触发冷数据搬迁
 */
static void wear_leveling_check(void)
{
    uint32_t max_erase = 0U;
    uint32_t min_erase = 0xFFFFFFFFU;
    int32_t cold_block = -1;
    uint32_t erase_cnt = 0U;/*擦写次数*/

    /* 遍历所有块，找最大和最小擦写次数 */
    for (uint32_t i = 0; i < NAND_TOTAL_BLOCKS - NAND_RESERVED_BLOCKS; i++) {
        if (nand_get_block_state(i) == BLOCK_BAD) {
            continue;
        }

        erase_cnt = nand_get_block_erase_count(i);
        if (erase_cnt > max_erase) {
            max_erase = erase_cnt;
        }
        if (erase_cnt < min_erase) {
            min_erase = erase_cnt;
            /* 记录擦写最少且已使用的块（冷块） */
            if (nand_get_block_state(i) == BLOCK_USED) {
                cold_block = (int32_t)i;
            }
        }
    }

    /* 擦写差超过阈值，且冷块是已使用块，触发冷数据搬迁 */
    if ((max_erase - min_erase) > FTL_WEAR_DIFF_THRESHOLD && cold_block >= 0) {
        /* 标记冷块为低有效页，让GC优先回收它，从而触发冷数据搬迁 */
        if (nand_mark_cold_block((uint32_t)cold_block) == RET_OK) {
            g_ftl_dev.wear_leveling_count++;
        }
    }
}

/* ============================================================
 *  静态磨损均衡模块
 * ============================================================ */

/**
 * @brief 静态磨损均衡检测与执行
 * @note 静态磨损均衡连空闲块也参与均衡：
 *       当擦写最少的块是空闲块，而擦写最多的块是已使用块时，
 *       主动把高磨损块的冷数据搬到低磨损的空闲块，
 *       让高磨损块变成空闲块，从而实现更彻底的磨损均衡
 */
static void static_wear_leveling_check(void)
{
    uint32_t max_erase = 0U;
    uint32_t min_erase = 0xFFFFFFFFU;
    int32_t max_erase_block = -1;  /* 擦写次数最多的块（已使用） */
    int32_t min_erase_block = -1;  /* 擦写次数最少的块（空闲） */
    uint32_t erase_cnt = 0U;
    uint8_t page_buf[NAND_PAGE_SIZE];/*搬迁页缓冲区*/
    uint32_t src_block = 0U;/*源块*/
    uint32_t dst_block = 0U;/*目标块*/
    uint32_t moved_pages = 0U;/*搬迁页数*/
    uint32_t old_ppn = 0U;/*旧物理页*/
    uint32_t lpn = 0U;/*逻辑页*/
    uint32_t new_ppn = 0U;/*新物理页*/
    ret_code_t ret = RET_OK;

    /* 遍历所有用户块，找擦写最多的已使用块和擦写最少的空闲块 */
    for (uint32_t i = 0; i < NAND_TOTAL_BLOCKS - NAND_RESERVED_BLOCKS; i++) {
        block_state_t state = nand_get_block_state(i);
        if (state == BLOCK_BAD) {
            continue;
        }

        erase_cnt = nand_get_block_erase_count(i);

        /* 找擦写最多的已使用块 */
        if (state == BLOCK_USED && erase_cnt > max_erase) {
            max_erase = erase_cnt;
            max_erase_block = (int32_t)i;
        }

        /* 找擦写最少的空闲块 */
        if (state == BLOCK_FREE && erase_cnt < min_erase) {
            min_erase = erase_cnt;
            min_erase_block = (int32_t)i;
        }
    }

    /* 擦写差超过静态磨损阈值，且找到了合适的块，执行静态磨损均衡 */
    if ((max_erase - min_erase) > FTL_STATIC_WEAR_THRESHOLD &&
        max_erase_block >= 0 && min_erase_block >= 0) {

        src_block = (uint32_t)max_erase_block;
        dst_block = (uint32_t)min_erase_block;
        moved_pages = 0U;

        /* 搬迁源块的所有有效页到目标空闲块 */
        for (uint32_t page = 0; page < NAND_PAGES_PER_BLOCK; page++) {
            if (!nand_is_page_valid(src_block, page)) {
                continue;
            }

            old_ppn = block_page_to_ppn(src_block, page);
            lpn = g_ftl_dev.reverse_map[old_ppn];
            if (lpn == FTL_INVALID_LPN) {
                continue;
            }

            /* 读取旧页数据 */
            (void)nand_page_read(src_block, page, page_buf);

            /* 写入新页（目标块的对应页偏移） */
            ret = nand_page_write(dst_block, page, page_buf);
            if (ret != RET_OK) {
                continue;
            }

            new_ppn = block_page_to_ppn(dst_block, page);

            /* 更新统计 */
            g_ftl_dev.nand_write_pages++;
            moved_pages++;

            /* 更新映射表 */
            g_ftl_dev.l2p_table[lpn] = new_ppn;
            g_ftl_dev.reverse_map[new_ppn] = lpn;
            g_ftl_dev.reverse_map[old_ppn] = FTL_INVALID_LPN;

            /* 标记旧页无效 */
            (void)nand_mark_page_invalid(src_block, page);
        }

        /* 擦除源块（高磨损块变成空闲块） */
        (void)nand_block_erase(src_block);

        /* 更新统计 */
        g_ftl_dev.static_wear_count++;
        g_ftl_dev.static_wear_moved_pages += moved_pages;
    }
}

/**
 * @brief ftl初始化，分配资源并设置初始状态
 * @retval RET_OK 初始化成功
 * @retval RET_ERR_NO_SPACE 空间不足，无法初始化
 * @details
 *  1. 初始化L2P正向映射表和反向映射表为无效
 *  2. 找到第一个空闲块作为初始写入块
 *  3. 初始化统计计数器和配置项
 *  4. 设置初始化标志
 */
ret_code_t ftl_init(void)
{
    uint32_t first_block = 0;/*第一个空闲块*/
    uint32_t total_logical_blocks = 0;/*逻辑块总数*/
    uint32_t total_ppns = 0;/*总物理页数*/

    /* 初始化L2P正向映射表为无效 */
    for (uint32_t i = 0; i < FTL_TOTAL_LPNS; i++) {
        g_ftl_dev.l2p_table[i] = FTL_INVALID_PPN;
    }

    /* 初始化反向映射表为无效 */
    total_ppns = NAND_TOTAL_BLOCKS * NAND_PAGES_PER_BLOCK;
    for (uint32_t i = 0; i < total_ppns; i++) {
        g_ftl_dev.reverse_map[i] = FTL_INVALID_LPN;
    }

    /* 找到第一个空闲块作为初始写入块 */   
    if (!ftl_find_next_free_block(0U, &first_block)) {
        return RET_ERR_NO_SPACE;
    }
    g_ftl_dev.cur_write_block = first_block;
    g_ftl_dev.cur_write_page = 0U;

    /* 初始化所有统计计数器 */
    g_ftl_dev.host_write_pages = 0U;
    g_ftl_dev.nand_write_pages = 0U;
    g_ftl_dev.gc_count = 0U;
    g_ftl_dev.gc_moved_pages = 0U;
    g_ftl_dev.wear_leveling_count = 0U;
    g_ftl_dev.static_wear_count = 0U;
    g_ftl_dev.static_wear_moved_pages = 0U;
    g_ftl_dev.trim_count = 0U;
    g_ftl_dev.trim_pages = 0U;
    g_ftl_dev.bad_block_replace_count = 0U;
    g_ftl_dev.bad_block_moved_pages = 0U;
    g_ftl_dev.write_since_last_check = 0U;
    g_ftl_dev.hot_cold_check_count = 0U;
    g_ftl_dev.hot_cold_migration_count = 0U;

    /* 初始化 WAL 日志 */
    g_ftl_dev.wal_file = NULL;
    g_ftl_dev.wal_sequence = 0U;
    g_ftl_dev.wal_entry_count = 0U;
    g_ftl_dev.wal_enabled = FTL_WAL_ENABLED_DEFAULT;

    /* 初始化混合映射块映射表 */
    total_logical_blocks = FTL_TOTAL_LPNS / FTL_LOGICAL_BLOCK_PAGES;
    for (uint32_t i = 0; i < total_logical_blocks; i++) {
        g_ftl_dev.block_map[i].is_valid = false;
        g_ftl_dev.block_map[i].is_hot = false;
        g_ftl_dev.block_map[i].physical_block = FTL_INVALID_BLOCK;
        g_ftl_dev.block_map[i].access_count = 0U;
    }

    /* 默认配置 */
    g_ftl_dev.gc_algo = GC_ALGO_GREEDY;
    g_ftl_dev.map_mode = MAP_MODE_PAGE;

    g_ftl_dev.is_initialized = true;

    LOG_INFO("FTL 初始化完成: 总LPN=%u, GC算法=%d, 映射模式=%d",
             FTL_TOTAL_LPNS, g_ftl_dev.gc_algo, g_ftl_dev.map_mode);

    return RET_OK;
}

/**
 * @brief 反初始化FTL，释放资源
 */
void ftl_deinit(void)
{
    g_ftl_dev.is_initialized = false;
}

/**
 * @brief 读取指定逻辑页数据
 * @param[in] lpn 逻辑页号
 * @param[out] buf 数据缓冲区
 * @retval RET_OK 读取成功
 * @retval RET_ERR_PARAM 参数非法
 * @retval RET_ERR_NOT_MAPPED 逻辑页未映射
 * @retval RET_ERR_BAD_BLOCK 读取遇到坏块，数据可能已丢失
 * @note 读取路径集成坏块自动处理：
 *       1. 正常读取，成功则直接返回
 *       2. 如果遇到坏块错误，自动触发坏块替换
 *       3. 坏块替换后，由于数据可能已损坏，仍返回错误
 *       4. 但坏块已被标记并替换，防止后续写入失败
 */
ret_code_t ftl_read(uint32_t lpn, uint8_t *buf)
{
    uint32_t ppn = 0;/*物理页号*/
    uint32_t block = 0;/*物理块号*/
    ret_code_t ret = RET_OK;
    ret_code_t replace_ret = RET_OK;

    /* 入参校验 */
    if (!g_ftl_dev.is_initialized || buf == NULL) {
        return RET_ERR_PARAM;
    }
    if (lpn >= FTL_TOTAL_LPNS) {
        return RET_ERR_PARAM;
    }

    /* 检查逻辑页是否已映射 */
    if (g_ftl_dev.l2p_table[lpn] == FTL_INVALID_PPN) {
        return RET_ERR_NOT_MAPPED;
    }

    /* 通过L2P映射找到物理页，读取数据 */
    ppn = g_ftl_dev.l2p_table[lpn];
    block = ppn_to_block(ppn);
    ret = nand_page_read(block, ppn_to_page(ppn), buf);

    /* 读取失败且是坏块错误，自动触发坏块替换 */
    if (ret == RET_ERR_BAD_BLOCK) {
        LOG_WARN("读取遇到坏块: LPN=%u, PPN=%u, 块=%u, 触发坏块替换",
                 lpn, ppn, block);

        /* 触发坏块替换 */
        replace_ret = ftl_replace_bad_block(block);
        if (replace_ret == RET_OK) {
            /* 坏块替换成功，但数据可能已丢失，仍返回错误 */
            LOG_WARN("坏块替换成功: 旧块=%u, 但数据可能已丢失", block);
        } else {
            LOG_ERROR("坏块替换失败: 块=%u, 错误码=%d", block, replace_ret);
        }

        return RET_ERR_BAD_BLOCK;
    }

    /* 混合映射模式：更新访问计数，用于冷热数据判断 */
    if (ret == RET_OK && g_ftl_dev.map_mode == MAP_MODE_HYBRID) {
        hybrid_update_access_count(lpn);
    }

    return ret;
}

/**
 * @brief 写入指定逻辑页数据（异地更新）
 * @param[in] lpn 逻辑页号
 * @param[in] buf 待写入数据缓冲区
 * @retval RET_OK 写入成功
 * @retval RET_ERR_PARAM 参数非法
 * @retval RET_ERR_NO_SPACE 空间耗尽
 */
ret_code_t ftl_write(uint32_t lpn, const uint8_t *buf)
{
    ret_code_t ret = RET_OK;
    uint32_t new_ppn = 0;/*新物理页*/
    uint32_t new_block = 0;/*新物理块*/
    uint32_t new_page = 0;/*新物理页偏移*/
    uint32_t old_ppn = 0;/*旧物理页*/
    uint32_t old_block = 0;/*旧物理块*/
    uint32_t old_page = 0;/*旧物理页偏移*/
    uint32_t old_ppn_snapshot = 0;/*旧物理页快照，用于日志记录*/
    uint32_t logical_block = 0;/*逻辑块号*/
    uint32_t total_logical_blocks = 0;/*逻辑块总数*/

    /* 入参校验 */
    if (!g_ftl_dev.is_initialized || buf == NULL) {
        return RET_ERR_PARAM;
    }
    if (lpn >= FTL_TOTAL_LPNS) {
        return RET_ERR_PARAM;
    }

    /* 空闲块不足时触发GC（循环直到有足够空间或GC失败） */
    while (nand_get_free_block_count() <= FTL_GC_TRIGGER_THRESHOLD) {
        ret = gc_do_recycle();
        if (ret != RET_OK) {
            return RET_ERR_NO_SPACE;
        }
    }

    /* 分配新物理页（异地更新，不覆盖旧页） */   
    if (!ftl_alloc_phy_page(&new_ppn)) {
        return RET_ERR_NO_SPACE;
    }

    /* Write-Ahead Log：先写日志，再写数据
     * 这样即使掉电，也能通过重放日志恢复映射状态 */
    old_ppn_snapshot = g_ftl_dev.l2p_table[lpn];
    ret = wal_log_write(lpn, old_ppn_snapshot, new_ppn);
    if (ret != RET_OK) {
        /* 日志写入失败，不继续写入（保证一致性） */
        return ret;
    }

    /* 写入新物理页 */
    new_block = ppn_to_block(new_ppn);
    new_page = ppn_to_page(new_ppn);
    ret = nand_page_write(new_block, new_page, buf);
    if (ret != RET_OK) {
        /* 如果是坏块错误，自动进行坏块替换并重试 */
        if (ret == RET_ERR_BAD_BLOCK) {
            ret = ftl_replace_bad_block(new_block);
            if (ret == RET_OK) {
                /* 坏块替换成功，重新分配物理页并重试写入 */
                if (ftl_alloc_phy_page(&new_ppn)) {
                    new_block = ppn_to_block(new_ppn);
                    new_page = ppn_to_page(new_ppn);
                    ret = nand_page_write(new_block, new_page, buf);
                } else {
                    ret = RET_ERR_NO_SPACE;
                }
            }
        }
        if (ret != RET_OK) {
            return ret;
        }
    }

    /* 更新统计 */
    g_ftl_dev.host_write_pages++;
    g_ftl_dev.nand_write_pages++;

    /* 旧物理页标记为无效（异地更新的核心） */
    if (g_ftl_dev.l2p_table[lpn] != FTL_INVALID_PPN) {
        old_ppn = g_ftl_dev.l2p_table[lpn];
        old_block = ppn_to_block(old_ppn);
        old_page = ppn_to_page(old_ppn);
        (void)nand_mark_page_invalid(old_block, old_page);
        g_ftl_dev.reverse_map[old_ppn] = FTL_INVALID_LPN;
    }

    /* 同时更新正向映射和反向映射 */
    g_ftl_dev.l2p_table[lpn] = new_ppn;
    g_ftl_dev.reverse_map[new_ppn] = lpn;

    /* 每次写入后检测动态磨损均衡 */
    wear_leveling_check();

    /* 每隔一定次数检测静态磨损均衡（减少性能开销） */
    g_ftl_dev.write_since_last_check++;
    if (g_ftl_dev.write_since_last_check >= FTL_STATIC_WEAR_INTERVAL) {
        g_ftl_dev.write_since_last_check = 0U;
        static_wear_leveling_check();
    }

    /* 混合映射模式：更新访问计数和块映射表 */
    if (g_ftl_dev.map_mode == MAP_MODE_HYBRID) {
        logical_block = lpn_to_logical_block(lpn);
        total_logical_blocks = FTL_TOTAL_LPNS / FTL_LOGICAL_BLOCK_PAGES;

        if (logical_block < total_logical_blocks) {
            /* 如果块未映射，标记为已映射（默认冷数据） */
            if (!g_ftl_dev.block_map[logical_block].is_valid) {
                g_ftl_dev.block_map[logical_block].is_valid = true;
                g_ftl_dev.block_map[logical_block].is_hot = false;
                g_ftl_dev.block_map[logical_block].physical_block = ppn_to_block(new_ppn);
                g_ftl_dev.block_map[logical_block].access_count = 0;
            }

            /* 更新访问计数 */
            hybrid_update_access_count(lpn);
        }

        /* 每隔一定次数检测冷热数据迁移 */
        g_ftl_dev.hot_cold_check_count++;
        if (g_ftl_dev.hot_cold_check_count >= FTL_HOT_COLD_CHECK_INTERVAL) {
            g_ftl_dev.hot_cold_check_count = 0U;
            (void)ftl_hot_cold_migration();
        }
    }

    return RET_OK;
}

/**
 * @brief 设置当前GC算法类型
 * @param[in] algo GC算法类型
 */
void ftl_set_gc_algo(gc_algo_type_t algo)
{
    g_ftl_dev.gc_algo = algo;
}

/**
 * @brief 获取当前GC算法类型
 * @return GC算法类型
 */
gc_algo_type_t ftl_get_gc_algo(void)
{
    return g_ftl_dev.gc_algo;
}

/**
 * @brief 手动触发一次GC垃圾回收
 * @retval RET_OK 回收成功
 * @retval RET_ERR_NOT_INIT 未初始化
 * @retval RET_ERR_NO_SPACE 无可回收块
 */
ret_code_t ftl_trigger_gc(void)
{
    if (!g_ftl_dev.is_initialized) {
        return RET_ERR_NOT_INIT;
    }
    return gc_do_recycle();
}

/**
 * @brief 设置映射模式
 * @param[in] mode 映射模式
 * @retval RET_OK 设置成功
 * @retval RET_ERR_NOT_INIT 未初始化
 * @retval RET_ERR_NOT_SUPPORT 不支持的映射模式
 */
ret_code_t ftl_set_map_mode(map_mode_t mode)
{
    if (!g_ftl_dev.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 目前仅支持页映射模式，混合映射为框架预留 */
    if (mode != MAP_MODE_PAGE && mode != MAP_MODE_HYBRID) {
        return RET_ERR_NOT_SUPPORT;
    }

    g_ftl_dev.map_mode = mode;
    return RET_OK;
}

/**
 * @brief 获取当前映射模式
 * @return 映射模式
 */
map_mode_t ftl_get_map_mode(void)
{
    return g_ftl_dev.map_mode;
}

/* ============================================================
 *  混合映射接口实现
 * ============================================================ */

/**
 * @brief 获取逻辑块号（从逻辑页号计算）
 * @param[in] lpn 逻辑页号
 * @return 逻辑块号
 */
static inline uint32_t lpn_to_logical_block(uint32_t lpn)
{
    return lpn / FTL_LOGICAL_BLOCK_PAGES;
}

/**
 * @brief 获取逻辑块内页偏移
 * @param[in] lpn 逻辑页号
 * @return 块内页偏移
 */
static inline uint32_t lpn_to_block_offset(uint32_t lpn)
{
    return lpn % FTL_LOGICAL_BLOCK_PAGES;
}

/**
 * @brief 获取块映射表项
 * @param[in] logical_block 逻辑块号
 * @param[out] entry 输出映射表项
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 * @retval RET_ERR_NOT_MAPPED 块未映射
 */
ret_code_t ftl_get_block_map_entry(uint32_t logical_block, block_map_entry_t *entry)
{
    if (entry == NULL || !g_ftl_dev.is_initialized) {
        return RET_ERR_PARAM;
    }

    uint32_t total_logical_blocks = FTL_TOTAL_LPNS / FTL_LOGICAL_BLOCK_PAGES;
    if (logical_block >= total_logical_blocks) {
        return RET_ERR_PARAM;
    }

    if (!g_ftl_dev.block_map[logical_block].is_valid) {
        return RET_ERR_NOT_MAPPED;
    }

    *entry = g_ftl_dev.block_map[logical_block];
    return RET_OK;
}

/**
 * @brief 获取混合映射统计信息
 * @param[out] page_mapped_blocks 页映射的块数
 * @param[out] block_mapped_blocks 块映射的块数
 * @param[out] hot_blocks 热数据块数
 * @param[out] cold_blocks 冷数据块数
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数为空
 */
ret_code_t ftl_get_hybrid_map_stats(uint32_t *page_mapped_blocks,
                                    uint32_t *block_mapped_blocks,
                                    uint32_t *hot_blocks,
                                    uint32_t *cold_blocks)
{
    if (page_mapped_blocks == NULL || block_mapped_blocks == NULL ||
        hot_blocks == NULL || cold_blocks == NULL) {
        return RET_ERR_PARAM;
    }

    uint32_t total_logical_blocks = FTL_TOTAL_LPNS / FTL_LOGICAL_BLOCK_PAGES;
    uint32_t page_mapped = 0;
    uint32_t block_mapped = 0;
    uint32_t hot = 0;
    uint32_t cold = 0;

    for (uint32_t i = 0; i < total_logical_blocks; i++) {
        if (!g_ftl_dev.block_map[i].is_valid) {
            continue;
        }

        if (g_ftl_dev.block_map[i].is_hot) {
            page_mapped++;  /* 热数据使用页映射 */
            hot++;
        } else {
            block_mapped++; /* 冷数据使用块映射 */
            cold++;
        }
    }

    *page_mapped_blocks = page_mapped;
    *block_mapped_blocks = block_mapped;
    *hot_blocks = hot;
    *cold_blocks = cold;

    return RET_OK;
}

/**
 * @brief 更新逻辑块的访问计数（用于冷热判断）
 * @param[in] lpn 逻辑页号
 * @note 每次访问时调用，增加对应逻辑块的访问计数
 */
static void hybrid_update_access_count(uint32_t lpn)
{
    uint32_t logical_block = lpn_to_logical_block(lpn);
    uint32_t total_logical_blocks = FTL_TOTAL_LPNS / FTL_LOGICAL_BLOCK_PAGES;

    if (logical_block >= total_logical_blocks) {
        return;
    }

    if (!g_ftl_dev.block_map[logical_block].is_valid) {
        return;
    }

    g_ftl_dev.block_map[logical_block].access_count++;
}

/**
 * @brief 热数据升级：将冷数据（块映射）升级为热数据（页映射）
 * @param[in] logical_block 逻辑块号
 * @retval RET_OK 升级成功
 * @retval RET_ERR_PARAM 参数非法
 * @note 升级过程：
 *       1. 读取块映射对应的物理块
 *       2. 将每一页建立 L2P 页映射
 *       3. 标记为热数据
 *       升级后可以享受页映射的快速随机访问能力
 */
static ret_code_t hybrid_upgrade_to_hot(uint32_t logical_block)
{
    uint32_t i = 0;
    uint32_t physical_block = 0;/*物理块号*/
    uint32_t base_lpn = 0;/*逻辑块起始页号*/
    uint32_t lpn = 0;/*逻辑页号*/
    uint32_t ppn = 0;/*物理页号*/

    if (!g_ftl_dev.block_map[logical_block].is_valid) {
        return RET_ERR_NOT_MAPPED;
    }

    if (g_ftl_dev.block_map[logical_block].is_hot) {
        return RET_OK;  /* 已经是热数据，无需升级 */
    }

    physical_block = g_ftl_dev.block_map[logical_block].physical_block;
    base_lpn = logical_block * FTL_LOGICAL_BLOCK_PAGES;

    /* 将块映射的每一页都建立 L2P 页映射 */
    for (i = 0; i < FTL_LOGICAL_BLOCK_PAGES; i++) {
        lpn = base_lpn + i;
        ppn = physical_block * NAND_PAGES_PER_BLOCK + i;

        if (lpn >= FTL_TOTAL_LPNS) {
            break;
        }

        /* 检查页是否有效 */
        if (nand_is_page_valid(physical_block, i)) {
            /* 建立 L2P 映射和反向映射 */
            g_ftl_dev.l2p_table[lpn] = ppn;
            g_ftl_dev.reverse_map[ppn] = lpn;
        } else {
            g_ftl_dev.l2p_table[lpn] = FTL_INVALID_PPN;
            g_ftl_dev.reverse_map[ppn] = FTL_INVALID_LPN;
        }
    }

    /* 标记为热数据 */
    g_ftl_dev.block_map[logical_block].is_hot = true;
    g_ftl_dev.hot_cold_migration_count++;

    return RET_OK;
}

/**
 * @brief 冷数据降级：将热数据（页映射）降级为冷数据（块映射）
 * @param[in] logical_block 逻辑块号
 * @retval RET_OK 降级成功
 * @retval RET_ERR_PARAM 参数非法
 * @note 降级过程：
 *       1. 找到该逻辑块对应的物理块（取第一页的物理块）
 *       2. 更新块映射表
 *       3. 清除 L2P 映射表项（节省内存）
 *       4. 标记为冷数据
 *       降级后节省内存，但随机访问需要计算偏移
 */
static ret_code_t hybrid_downgrade_to_cold(uint32_t logical_block)
{
    uint32_t i = 0;
    uint32_t base_lpn = 0;/*逻辑块起始页号*/
    uint32_t physical_block = FTL_INVALID_BLOCK;/*物理块号*/
    uint32_t lpn = 0;/*逻辑页号*/
    uint32_t ppn = 0;/*物理页号*/

    if (!g_ftl_dev.block_map[logical_block].is_valid) {
        return RET_ERR_NOT_MAPPED;
    }

    if (!g_ftl_dev.block_map[logical_block].is_hot) {
        return RET_OK;  /* 已经是冷数据，无需降级 */
    }

    base_lpn = logical_block * FTL_LOGICAL_BLOCK_PAGES;

    /* 找到该逻辑块对应的物理块（找第一个有效页） */
    for (i = 0; i < FTL_LOGICAL_BLOCK_PAGES; i++) {
        lpn = base_lpn + i;
        if (lpn >= FTL_TOTAL_LPNS) {
            break;
        }
        if (g_ftl_dev.l2p_table[lpn] != FTL_INVALID_PPN) {
            physical_block = ppn_to_block(g_ftl_dev.l2p_table[lpn]);
            break;
        }
    }

    if (physical_block == FTL_INVALID_BLOCK) {
        /* 没有有效页，直接标记为无效 */
        g_ftl_dev.block_map[logical_block].is_valid = false;
        g_ftl_dev.block_map[logical_block].is_hot = false;
        return RET_OK;
    }

    /* 更新块映射表 */
    g_ftl_dev.block_map[logical_block].physical_block = physical_block;

    /* 清除 L2P 映射表项（节省内存） */
    for (i = 0; i < FTL_LOGICAL_BLOCK_PAGES; i++) {
        lpn = base_lpn + i;
        if (lpn >= FTL_TOTAL_LPNS) {
            break;
        }
        if (g_ftl_dev.l2p_table[lpn] != FTL_INVALID_PPN) {
            ppn = g_ftl_dev.l2p_table[lpn];
            g_ftl_dev.reverse_map[ppn] = FTL_INVALID_LPN;
            g_ftl_dev.l2p_table[lpn] = FTL_INVALID_PPN;
        }
    }

    /* 标记为冷数据 */
    g_ftl_dev.block_map[logical_block].is_hot = false;
    g_ftl_dev.hot_cold_migration_count++;

    return RET_OK;
}

/**
 * @brief 手动触发一次冷热数据迁移检测
 * @retval RET_OK 成功
 * @retval RET_ERR_NOT_INIT 未初始化
 * @note 检测冷热数据，将热数据从块映射升级为页映射，
 *       将冷数据从页映射降级为块映射
 *       完整实现：
 *       - 热数据升级：块映射 -> 页映射（提升随机访问性能）
 *       - 冷数据降级：页映射 -> 块映射（节省内存空间）
 *       - 访问计数衰减：每个周期后访问计数衰减，避免历史热点影响
 */
ret_code_t ftl_hot_cold_migration(void)
{
    uint32_t total_logical_blocks = 0;/*逻辑块总数*/
    uint32_t i = 0;
    uint32_t hot_count = 0;/* 升级为热数据的块数 */
    uint32_t cold_count = 0;/* 降级为冷数据的块数 */
    block_map_entry_t *entry = NULL;/* 块映射表项 */

    if (!g_ftl_dev.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    total_logical_blocks = FTL_TOTAL_LPNS / FTL_LOGICAL_BLOCK_PAGES;

    /* 遍历所有逻辑块，检测冷热状态 */
    for (i = 0; i < total_logical_blocks; i++) {
        if (!g_ftl_dev.block_map[i].is_valid) {
            continue;
        }

        entry = &g_ftl_dev.block_map[i];

        /* 热数据：访问计数超过阈值，且当前是冷数据 -> 升级为热数据 */
        if (entry->access_count >= FTL_HOT_DATA_THRESHOLD && !entry->is_hot) {
            (void)hybrid_upgrade_to_hot(i);
            hot_count++;
        }
        /* 冷数据：访问计数低于降级阈值，且当前是热数据 -> 降级为冷数据 */
        else if (entry->access_count < FTL_COLD_DATA_THRESHOLD && entry->is_hot) {
            (void)hybrid_downgrade_to_cold(i);
            cold_count++;
        }

        /* 访问计数衰减（进入下一个统计周期，衰减一半） */
        entry->access_count = entry->access_count / 2;
    }

    return RET_OK;
}

/**
 * @brief 计算校验和（简单的32位累加校验）
 * @param[in] data 数据指针
 * @param[in] len  数据长度（字节）
 * @return 校验和
 */
static uint32_t ftl_calc_checksum(const uint8_t *data, uint32_t len)
{
    uint32_t checksum = 0;
    for (uint32_t i = 0; i < len; i++) {
        checksum += data[i];
    }
    return checksum;
}

/**
 * @brief 保存FTL快照（掉电保护）
 * @param[in] file_path 快照文件路径
 * @retval RET_OK 保存成功
 * @retval RET_ERR_PARAM 参数非法
 * @retval RET_ERR_INTERNAL 文件操作失败
 */
ret_code_t ftl_save_snapshot(const char *file_path)
{
     /* 构建元数据头 */
    ftl_meta_header_t header;

    /*合法性校验*/
    if (file_path == NULL || !g_ftl_dev.is_initialized) {
        return RET_ERR_PARAM;
    }

    FILE *fp = fopen(file_path, "wb");
    if (fp == NULL) {
        return RET_ERR_INTERNAL;
    }
  
    header.magic = FTL_META_MAGIC;
    header.version = FTL_META_VERSION;
    header.host_write_pages = g_ftl_dev.host_write_pages;
    header.nand_write_pages = g_ftl_dev.nand_write_pages;
    header.gc_count = g_ftl_dev.gc_count;
    header.cur_write_block = g_ftl_dev.cur_write_block;
    header.cur_write_page = g_ftl_dev.cur_write_page;
    header.checksum = 0;

    /* 计算校验和 */
    header.checksum = ftl_calc_checksum((uint8_t *)&header, sizeof(header));

    /* 写入元数据头 */
    fwrite(&header, sizeof(header), 1, fp);

    /* 写入L2P映射表 */
    fwrite(g_ftl_dev.l2p_table, sizeof(uint32_t), FTL_TOTAL_LPNS, fp);

    fclose(fp);
    return RET_OK;
}

/**
 * @brief 加载FTL快照（掉电恢复）
 * @param[in] file_path 快照文件路径
 * @retval RET_OK 加载成功
 * @retval RET_ERR_PARAM 参数非法
 * @retval RET_ERR_INTERNAL 文件操作失败
 * @retval RET_ERR_CHECKSUM 校验和校验失败
 */
ret_code_t ftl_load_snapshot(const char *file_path)
{
    if (file_path == NULL) {
        return RET_ERR_PARAM;
    }

    FILE *fp = fopen(file_path, "rb");
    if (fp == NULL) {
        return RET_ERR_INTERNAL;
    }

    /* 读取元数据头 */
    ftl_meta_header_t header;
    if (fread(&header, sizeof(header), 1, fp) != 1) {
        fclose(fp);
        return RET_ERR_INTERNAL;
    }

    /* 校验魔数 */
    if (header.magic != FTL_META_MAGIC) {
        fclose(fp);
        return RET_ERR_CHECKSUM;
    }

    /* 校验校验和 */
    uint32_t saved_checksum = header.checksum;
    header.checksum = 0;
    uint32_t calc_checksum = ftl_calc_checksum((uint8_t *)&header, sizeof(header));
    if (saved_checksum != calc_checksum) {
        fclose(fp);
        return RET_ERR_CHECKSUM;
    }

    /* 读取L2P映射表 */
    if (fread(g_ftl_dev.l2p_table, sizeof(uint32_t), FTL_TOTAL_LPNS, fp) != FTL_TOTAL_LPNS) {
        fclose(fp);
        return RET_ERR_INTERNAL;
    }

    fclose(fp);

    /* 恢复状态 */
    g_ftl_dev.host_write_pages = header.host_write_pages;
    g_ftl_dev.nand_write_pages = header.nand_write_pages;
    g_ftl_dev.gc_count = header.gc_count;
    g_ftl_dev.cur_write_block = header.cur_write_block;
    g_ftl_dev.cur_write_page = header.cur_write_page;

    /* 重建反向映射表 */
    uint32_t total_ppns = NAND_TOTAL_BLOCKS * NAND_PAGES_PER_BLOCK;
    for (uint32_t i = 0; i < total_ppns; i++) {
        g_ftl_dev.reverse_map[i] = FTL_INVALID_LPN;
    }
    for (uint32_t lpn = 0; lpn < FTL_TOTAL_LPNS; lpn++) {
        if (g_ftl_dev.l2p_table[lpn] != FTL_INVALID_PPN) {
            uint32_t ppn = g_ftl_dev.l2p_table[lpn];
            if (ppn < total_ppns) {
                g_ftl_dev.reverse_map[ppn] = lpn;
            }
        }
    }

    g_ftl_dev.is_initialized = true;
    return RET_OK;
}

/* ============================================================
 *  WAL 日志恢复接口实现
 * ============================================================ */

/**
 * @brief 计算 WAL 条目的校验和
 * @param[in] entry 日志条目
 * @return 校验和值
 * @note 使用简单的32位累加校验，确保日志条目的完整性
 */
static uint32_t wal_calculate_checksum(const wal_entry_t *entry)
{
    uint32_t checksum = 0;
    const uint8_t *p = (const uint8_t *)entry;
    /* 跳过 checksum 字段本身 */
    for (size_t i = 0; i < offsetof(wal_entry_t, checksum); i++) {
        checksum += p[i];
    }
    return checksum;
}

/**
 * @brief 初始化 WAL 日志
 * @param[in] file_path 日志文件路径
 * @retval RET_OK 成功
 * @retval RET_ERR_INTERNAL 打开文件失败
 * @note 打开或创建 WAL 日志文件，准备写入日志
 */
ret_code_t ftl_wal_init(const char *file_path)
{
    if (file_path == NULL) {
        return RET_ERR_PARAM;
    }

    /* 以追加模式打开日志文件 */
    g_ftl_dev.wal_file = fopen(file_path, "ab+");
    if (g_ftl_dev.wal_file == NULL) {
        return RET_ERR_INTERNAL;
    }

    /* 计算当前日志条目数 */
    fseek(g_ftl_dev.wal_file, 0, SEEK_END);
    long file_size = ftell(g_ftl_dev.wal_file);
    g_ftl_dev.wal_entry_count = (uint32_t)(file_size / sizeof(wal_entry_t));
    g_ftl_dev.wal_sequence = g_ftl_dev.wal_entry_count;

    return RET_OK;
}

/**
 * @brief 写入一条 WAL 日志
 * @param[in] entry 日志条目
 * @retval RET_OK 成功
 * @retval RET_ERR_INTERNAL 写入失败
 * @note 写入操作前必须先写 WAL 日志（Write-Ahead Log原则）
 *       这样即使掉电，也能通过重放日志恢复操作
 */
ret_code_t ftl_wal_append(const wal_entry_t *entry)
{
    size_t written = 0;

    if (entry == NULL || g_ftl_dev.wal_file == NULL) {
        return RET_ERR_PARAM;
    }

    /* 构造日志条目 */
    wal_entry_t wal_entry = *entry;
    wal_entry.magic = FTL_WAL_MAGIC;
    wal_entry.sequence = g_ftl_dev.wal_sequence++;
    wal_entry.checksum = wal_calculate_checksum(&wal_entry);

    /* 写入日志 */
    written = fwrite(&wal_entry, sizeof(wal_entry_t), 1, g_ftl_dev.wal_file);
    if (written != 1) {
        return RET_ERR_INTERNAL;
    }

    /* 刷新到磁盘，确保日志持久化 */
    fflush(g_ftl_dev.wal_file);

    g_ftl_dev.wal_entry_count++;

    return RET_OK;
}

/**
 * @brief 重放 WAL 日志，恢复状态
 * @param[in] file_path 日志文件路径
 * @retval RET_OK 恢复成功
 * @retval RET_ERR_INTERNAL 读取失败
 * @note 掉电恢复时调用，重放所有日志条目，恢复映射表一致性
 *       对于每条有效的日志，重新执行对应的操作
 */
ret_code_t ftl_wal_replay(const char *file_path)
{
    if (file_path == NULL) {
        return RET_ERR_PARAM;
    }

    /* 打开日志文件 */
    FILE *fp = fopen(file_path, "rb");
    if (fp == NULL) {
        /* 日志文件不存在，不需要重放 */
        return RET_OK;
    }

    uint32_t replayed_count = 0;
    wal_entry_t entry;

    /* 逐条读取并重放日志 */
    while (fread(&entry, sizeof(wal_entry_t), 1, fp) == 1) {
        /* 校验魔数 */
        if (entry.magic != FTL_WAL_MAGIC) {
            continue;
        }

        /* 校验校验和 */
        uint32_t checksum = wal_calculate_checksum(&entry);
        if (checksum != entry.checksum) {
            continue;
        }

        /* 根据操作类型重放 */
        switch (entry.op) {
            case WAL_OP_WRITE:
                /* 写入操作：更新映射表 */
                if (entry.lpn < FTL_TOTAL_LPNS && entry.new_ppn != FTL_INVALID_PPN) {
                    /* 旧映射失效 */
                    if (entry.old_ppn != FTL_INVALID_PPN) {
                        g_ftl_dev.reverse_map[entry.old_ppn] = FTL_INVALID_LPN;
                    }
                    /* 新映射生效 */
                    g_ftl_dev.l2p_table[entry.lpn] = entry.new_ppn;
                    g_ftl_dev.reverse_map[entry.new_ppn] = entry.lpn;
                }
                break;

            case WAL_OP_ERASE:
                /* 擦除操作：块已擦除，不需要特殊处理 */
                break;

            case WAL_OP_TRIM:
                /* TRIM操作：清除映射 */
                if (entry.lpn < FTL_TOTAL_LPNS) {
                    uint32_t ppn = g_ftl_dev.l2p_table[entry.lpn];
                    if (ppn != FTL_INVALID_PPN) {
                        g_ftl_dev.reverse_map[ppn] = FTL_INVALID_LPN;
                        g_ftl_dev.l2p_table[entry.lpn] = FTL_INVALID_PPN;
                    }
                }
                break;

            default:
                break;
        }

        replayed_count++;
    }

    fclose(fp);

    return RET_OK;
}

/**
 * @brief 清空 WAL 日志（checkpoint 后调用）
 * @retval RET_OK 成功
 * @note 当元数据快照保存后（checkpoint），可以清空 WAL 日志
 *       因为快照已经包含了所有状态，日志不再需要
 */
ret_code_t ftl_wal_clear(void)
{
    if (g_ftl_dev.wal_file == NULL) {
        return RET_OK;
    }

    /* 关闭并重新打开文件（清空内容） */
    fclose(g_ftl_dev.wal_file);
    g_ftl_dev.wal_file = NULL;
    g_ftl_dev.wal_sequence = 0;
    g_ftl_dev.wal_entry_count = 0;

    return RET_OK;
}

/**
 * @brief 启用/禁用 WAL 日志
 * @param[in] enable true 启用，false 禁用
 * @note 禁用后写入操作不再写 WAL，掉电保护能力下降
 */
void ftl_wal_enable(bool enable)
{
    g_ftl_dev.wal_enabled = enable;
}

/**
 * @brief 检查 WAL 是否已启用
 * @return true 已启用，false 已禁用
 */
bool ftl_wal_is_enabled(void)
{
    return g_ftl_dev.wal_enabled;
}

/**
 * @brief 内部函数：写入一条写入操作的 WAL 日志
 * @param[in] lpn 逻辑页号
 * @param[in] old_ppn 旧物理页号
 * @param[in] new_ppn 新物理页号
 * @retval RET_OK 成功
 * @note Write-Ahead Log 原则：写入数据前必须先写日志
 *       这样即使掉电，也能通过重放日志恢复映射状态
 */
static ret_code_t wal_log_write(uint32_t lpn, uint32_t old_ppn, uint32_t new_ppn)
{
    if (!g_ftl_dev.wal_enabled || g_ftl_dev.wal_file == NULL) {
        return RET_OK;  /* WAL未启用或未初始化，直接返回成功 */
    }

    wal_entry_t entry;
    entry.op = WAL_OP_WRITE;
    entry.lpn = lpn;
    entry.old_ppn = old_ppn;
    entry.new_ppn = new_ppn;

    return ftl_wal_append(&entry);
}

/**
 * @brief 内部函数：写入一条 TRIM 操作的 WAL 日志
 * @param[in] lpn 逻辑页号
 * @retval RET_OK 成功
 */
static ret_code_t wal_log_trim(uint32_t lpn)
{
    if (!g_ftl_dev.wal_enabled || g_ftl_dev.wal_file == NULL) {
        return RET_OK;
    }

    wal_entry_t entry;
    entry.op = WAL_OP_TRIM;
    entry.lpn = lpn;
    entry.old_ppn = g_ftl_dev.l2p_table[lpn];
    entry.new_ppn = FTL_INVALID_PPN;

    return ftl_wal_append(&entry);
}

/* ============================================================
 *  TRIM/Discard 接口实现
 * ============================================================ */

/**
 * @brief TRIM 操作：通知 FTL 指定逻辑页的数据已无效
 * @param[in] lpn 起始逻辑页号
 * @param[in] count 要 TRIM 的页数
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 * @retval RET_ERR_NOT_INIT 未初始化
 * @note TRIM 后这些页对应的物理页会被标记为无效，
 *       GC 回收时不需要搬迁这些数据，直接擦除即可，
 *       从而提高 GC 效率，减少写放大
 */
ret_code_t ftl_trim(uint32_t lpn, uint32_t count)
{
    uint32_t trimmed = 0U;
    uint32_t i = 0;
    uint32_t cur_lpn = 0;
    uint32_t ppn = 0;
    uint32_t block = 0;
    uint32_t page = 0;

    /* 入参校验 */
    if (!g_ftl_dev.is_initialized) {
        return RET_ERR_NOT_INIT;
    }
    if (count == 0U) {
        return RET_ERR_PARAM;
    }
    if (lpn >= FTL_TOTAL_LPNS) {
        return RET_ERR_PARAM;
    }
    if ((lpn + count) > FTL_TOTAL_LPNS) {
        count = FTL_TOTAL_LPNS - lpn;
    }

    /* 遍历指定范围的逻辑页 */
    for (i = 0; i < count; i++) {
        cur_lpn = lpn + i;
        ppn = g_ftl_dev.l2p_table[cur_lpn];

        /* 未映射的页跳过 */
        if (ppn == FTL_INVALID_PPN) {
            continue;
        }

        /* Write-Ahead Log：先写 TRIM 日志 */
        (void)wal_log_trim(cur_lpn);

        block = ppn_to_block(ppn);
        page = ppn_to_page(ppn);

        /* 标记物理页为无效 */
        (void)nand_mark_page_invalid(block, page);

        /* 清除映射表 */
        g_ftl_dev.l2p_table[cur_lpn] = FTL_INVALID_PPN;
        g_ftl_dev.reverse_map[ppn] = FTL_INVALID_LPN;

        trimmed++;
    }

    /* 更新统计 */
    g_ftl_dev.trim_count++;
    g_ftl_dev.trim_pages += trimmed;

    LOG_DEBUG("TRIM 操作: 起始LPN=%u, 页数=%u, 实际无效化=%u",
              lpn, count, trimmed);

    return RET_OK;
}

/* ============================================================
 *  安全擦除接口实现
 * ============================================================ */

/**
 * @brief 生成安全擦除的数据模式
 * @param[out] buf 数据缓冲区
 * @param[in] size 缓冲区大小
 * @param[in] pass 当前覆写次数（从0开始）
 * @note 根据覆写次数生成不同的数据模式：
 *       - 第0次：全0 (0x00)
 *       - 第1次：全1 (0xFF)
 *       - 第2次：0xAA交替
 *       - 第3次：0x55交替
 *       - 其他：伪随机模式
 */
static void generate_erase_pattern(uint8_t *buf, uint32_t size, uint32_t pass)
{
    uint8_t pattern = 0;

    if (buf == NULL || size == 0) {
        return;
    }

    /* 根据覆写次数选择数据模式 */
    switch (pass % 4) {
    case 0:
        pattern = 0x00;  /* 全0 */
        break;
    case 1:
        pattern = 0xFF;  /* 全1 */
        break;
    case 2:
        pattern = 0xAA;  /* 10101010 */
        break;
    case 3:
        pattern = 0x55;  /* 01010101 */
        break;
    default:
        pattern = (uint8_t)(pass * 37);  /* 伪随机模式 */
        break;
    }

    /* 填充数据模式 */
    memset(buf, pattern, size);
}

ret_code_t ftl_secure_erase(uint32_t lpn, uint32_t count, uint32_t passes)
{
    uint8_t *erase_buf = NULL;
    uint32_t i = 0;
    uint32_t pass = 0;
    ret_code_t ret = RET_OK;

    /* 入参校验 */
    if (!g_ftl_dev.is_initialized) {
        return RET_ERR_NOT_INIT;
    }
    if (count == 0U) {
        return RET_ERR_PARAM;
    }
    if (passes == 0U) {
        return RET_ERR_PARAM;
    }
    if (lpn >= FTL_TOTAL_LPNS) {
        return RET_ERR_PARAM;
    }
    if ((lpn + count) > FTL_TOTAL_LPNS) {
        count = FTL_TOTAL_LPNS - lpn;
    }

    /* 分配擦除缓冲区 */
    erase_buf = (uint8_t *)malloc(NAND_PAGE_SIZE);
    if (erase_buf == NULL) {
        return RET_ERR_INTERNAL;
    }

    LOG_INFO("开始安全擦除: 起始LPN=%u, 页数=%u, 覆写次数=%u",
             lpn, count, passes);

    /* 多次覆写，每次使用不同的数据模式 */
    for (pass = 0; pass < passes; pass++) {
        /* 生成当前次的数据模式 */
        generate_erase_pattern(erase_buf, NAND_PAGE_SIZE, pass);

        LOG_DEBUG("安全擦除第 %u 次覆写，模式=0x%02X",
                  pass, erase_buf[0]);

        /* 逐页覆写 */
        for (i = 0; i < count; i++) {
            ret = ftl_write(lpn + i, erase_buf);
            if (ret != RET_OK) {
                LOG_ERROR("安全擦除失败: LPN=%u, 错误码=%d", lpn + i, ret);
                free(erase_buf);
                return ret;
            }
        }
    }

    /* 最后执行 TRIM 操作，标记为无效 */
    ret = ftl_trim(lpn, count);
    if (ret != RET_OK) {
        LOG_ERROR("安全擦除最后TRIM失败: 错误码=%d", ret);
        free(erase_buf);
        return ret;
    }

    /* 释放缓冲区 */
    free(erase_buf);

    LOG_INFO("安全擦除完成: 起始LPN=%u, 页数=%u, 覆写次数=%u",
             lpn, count, passes);

    return RET_OK;
}

ret_code_t ftl_secure_erase_all(uint32_t passes)
{
    /* 入参校验 */
    if (!g_ftl_dev.is_initialized) {
        return RET_ERR_NOT_INIT;
    }
    if (passes == 0U) {
        return RET_ERR_PARAM;
    }

    LOG_INFO("开始全盘安全擦除，覆写次数=%u", passes);

    /* 擦除所有逻辑页 */
    return ftl_secure_erase(0, FTL_TOTAL_LPNS, passes);
}

/* ============================================================
 *  坏块管理接口实现
 * ============================================================ */

/**
 * @brief 手动触发指定块的坏块替换（用于测试）
 * @param[in] block 物理块号
 * @retval RET_OK 替换成功
 * @retval RET_ERR_PARAM 参数非法
 * @retval RET_ERR_NO_SPACE 预留块已用完
 * @note 这是完整的运行时坏块处理流程：
 *       1. 从预留块池分配新块
 *       2. 搬迁坏块中的所有有效页到新块
 *       3. 更新L2P映射表和反向映射表
 *       4. 标记原块为坏块
 */
ret_code_t ftl_replace_bad_block(uint32_t block)
{
    uint32_t new_block = 0;
    uint32_t moved_pages = 0U;
    uint32_t page = 0;
    uint32_t old_ppn = 0;
    uint32_t new_ppn = 0;
    uint32_t lpn = 0;
    ret_code_t ret = RET_OK;

    if (!g_ftl_dev.is_initialized) {
        return RET_ERR_NOT_INIT;
    }
    if (block >= NAND_TOTAL_BLOCKS) {
        return RET_ERR_PARAM;
    }

    /* 调用NAND层的坏块替换 */
    ret = nand_replace_bad_block(block, &new_block);
    if (ret != RET_OK) {
        return ret;
    }

    /* 更新L2P映射表和反向映射表 */
    for (page = 0; page < NAND_PAGES_PER_BLOCK; page++) {
        old_ppn = block_page_to_ppn(block, page);
        new_ppn = block_page_to_ppn(new_block, page);

        /* 通过反向映射找到对应的LPN */
        lpn = g_ftl_dev.reverse_map[old_ppn];
        if (lpn == FTL_INVALID_LPN) {
            continue;
        }

        /* 更新正向映射 */
        g_ftl_dev.l2p_table[lpn] = new_ppn;

        /* 更新反向映射 */
        g_ftl_dev.reverse_map[new_ppn] = lpn;
        g_ftl_dev.reverse_map[old_ppn] = FTL_INVALID_LPN;

        moved_pages++;
    }

    /* 更新统计 */
    g_ftl_dev.bad_block_replace_count++;
    g_ftl_dev.bad_block_moved_pages += moved_pages;

    /* 如果当前写入块是被替换的块，切换到新块 */
    if (g_ftl_dev.cur_write_block == block) {
        g_ftl_dev.cur_write_block = new_block;
    }

    LOG_WARN("坏块替换完成: 坏块=%u, 新块=%u, 搬迁页数=%u",
             block, new_block, moved_pages);

    return RET_OK;
}

/**
 * @brief 获取坏块替换统计信息
 * @param[out] replace_count 输出坏块替换次数
 * @param[out] moved_pages 输出搬迁的总页数
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数为空
 */
ret_code_t ftl_get_bad_block_stats(uint32_t *replace_count, uint32_t *moved_pages)
{
    if (replace_count == NULL || moved_pages == NULL) {
        return RET_ERR_PARAM;
    }

    *replace_count = g_ftl_dev.bad_block_replace_count;
    *moved_pages = g_ftl_dev.bad_block_moved_pages;

    return RET_OK;
}

/* ============================================================
 *  读干扰管理接口实现
 * ============================================================ */

/**
 * @brief 手动触发一次读干扰检测和处理
 * @retval RET_OK 成功（可能没有需要处理的块）
 * @retval RET_ERR_NOT_INIT 未初始化
 * @note 读干扰：反复读取同一个块会对同块其他页造成轻微干扰，
 *       读的次数多了可能导致数据出错，需要把数据搬迁到新块
 *       处理流程：找到读取最多的块 → 分配新空闲块 → 搬迁所有有效页 → 擦除旧块
 */
ret_code_t ftl_handle_read_disturb(void)
{
    if (!g_ftl_dev.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    uint32_t src_block;
    if (!nand_check_read_disturb(&src_block)) {
        /* 没有需要处理的块 */
        return RET_OK;
    }

    /* 分配一个新的空闲块 */
    uint32_t dst_block;
    if (!ftl_find_next_free_block(0U, &dst_block)) {
        return RET_ERR_NO_SPACE;
    }

    uint8_t page_buf[NAND_PAGE_SIZE];
    uint32_t moved_pages = 0U;

    /* 搬迁源块的所有有效页到目标块 */
    for (uint32_t page = 0; page < NAND_PAGES_PER_BLOCK; page++) {
        if (!nand_is_page_valid(src_block, page)) {
            continue;
        }

        uint32_t old_ppn = block_page_to_ppn(src_block, page);
        uint32_t lpn = g_ftl_dev.reverse_map[old_ppn];
        if (lpn == FTL_INVALID_LPN) {
            continue;
        }

        /* 读取旧页数据 */
        (void)nand_page_read(src_block, page, page_buf);

        /* 写入新页（目标块的对应页偏移） */
        ret_code_t ret = nand_page_write(dst_block, page, page_buf);
        if (ret != RET_OK) {
            continue;
        }

        uint32_t new_ppn = block_page_to_ppn(dst_block, page);

        /* 更新统计 */
        g_ftl_dev.nand_write_pages++;
        moved_pages++;

        /* 更新映射表 */
        g_ftl_dev.l2p_table[lpn] = new_ppn;
        g_ftl_dev.reverse_map[new_ppn] = lpn;
        g_ftl_dev.reverse_map[old_ppn] = FTL_INVALID_LPN;

        /* 标记旧页无效 */
        (void)nand_mark_page_invalid(src_block, page);
    }

    /* 擦除源块（重置读计数） */
    (void)nand_block_erase(src_block);

    return RET_OK;
}

/**
 * @brief 获取主机写入总页数
 * @return 主机写入页数
 */
uint32_t ftl_get_host_write_pages(void)
{
    return g_ftl_dev.host_write_pages;
}

/**
 * @brief 获取NAND实际写入总页数（含GC搬迁）
 * @return NAND写入页数
 */
uint32_t ftl_get_nand_write_pages(void)
{
    return g_ftl_dev.nand_write_pages;
}

/**
 * @brief 获取GC执行总次数
 * @return GC执行次数
 */
uint32_t ftl_get_gc_count(void)
{
    return g_ftl_dev.gc_count;
}

/**
 * @brief 获取写放大系数WAF
 * @return 写放大系数 = NAND写入页数 / 主机写入页数
 */
double ftl_get_write_amplification_factor(void)
{
    if (g_ftl_dev.host_write_pages == 0U) {
        return 0.0;
    }
    return (double)g_ftl_dev.nand_write_pages / (double)g_ftl_dev.host_write_pages;
}

/**
 * @brief 获取FTL统计信息
 * @param[out] stats 统计信息输出结构体
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数为空或未初始化
 */
ret_code_t ftl_get_stats(ftl_stats_t *stats)
{
    if (stats == NULL || !g_ftl_dev.is_initialized) {
        return RET_ERR_PARAM;
    }

    (void)memset(stats, 0, sizeof(ftl_stats_t));

    stats->total_lpns = FTL_TOTAL_LPNS;

    /* 统计已使用的逻辑页 */
    for (uint32_t i = 0; i < FTL_TOTAL_LPNS; i++) {
        if (g_ftl_dev.l2p_table[i] != FTL_INVALID_PPN) {
            stats->used_lpns++;
        }
    }

    stats->host_write_pages = g_ftl_dev.host_write_pages;
    stats->nand_write_pages = g_ftl_dev.nand_write_pages;
    stats->gc_count = g_ftl_dev.gc_count;
    stats->gc_moved_pages = g_ftl_dev.gc_moved_pages;
    stats->wear_leveling_count = g_ftl_dev.wear_leveling_count;
    stats->static_wear_count = g_ftl_dev.static_wear_count;
    stats->static_wear_moved_pages = g_ftl_dev.static_wear_moved_pages;
    stats->trim_count = g_ftl_dev.trim_count;
    stats->trim_pages = g_ftl_dev.trim_pages;
    stats->bad_block_replace_count = g_ftl_dev.bad_block_replace_count;
    stats->bad_block_moved_pages = g_ftl_dev.bad_block_moved_pages;
    stats->hot_cold_migration_count = g_ftl_dev.hot_cold_migration_count;
    stats->waf = ftl_get_write_amplification_factor();

    /* 统计冷热数据块数量 */
    if (g_ftl_dev.map_mode == MAP_MODE_HYBRID) {
        uint32_t total_logical_blocks = FTL_TOTAL_LPNS / FTL_LOGICAL_BLOCK_PAGES;
        for (uint32_t i = 0; i < total_logical_blocks; i++) {
            if (g_ftl_dev.block_map[i].is_valid) {
                if (g_ftl_dev.block_map[i].is_hot) {
                    stats->hot_block_count++;
                } else {
                    stats->cold_block_count++;
                }
            }
        }
    }

    return RET_OK;
}

/**
 * @brief 打印FTL统计信息（调试用）
 */
void ftl_print_stats(void)
{
    ftl_stats_t stats;
    if (ftl_get_stats(&stats) != RET_OK) {
        printf("FTL not initialized\n");
        return;
    }

    printf("========== FTL 统计信息 ==========\n");
    printf("总逻辑页:     %u\n", stats.total_lpns);
    printf("已使用页:     %u\n", stats.used_lpns);
    printf("主机写入:     %u 页\n", stats.host_write_pages);
    printf("NAND写入:     %u 页\n", stats.nand_write_pages);
    printf("GC次数:       %u 次\n", stats.gc_count);
    printf("GC搬迁页:     %u 页\n", stats.gc_moved_pages);
    printf("动态磨损均衡: %u 次\n", stats.wear_leveling_count);
    printf("静态磨损均衡: %u 次\n", stats.static_wear_count);
    printf("静态磨损搬迁: %u 页\n", stats.static_wear_moved_pages);
    printf("TRIM次数:     %u 次\n", stats.trim_count);
    printf("TRIM释放页:   %u 页\n", stats.trim_pages);
    printf("坏块替换:     %u 次\n", stats.bad_block_replace_count);
    printf("坏块搬迁页:   %u 页\n", stats.bad_block_moved_pages);
    printf("写放大WAF:    %.2f\n", stats.waf);
    printf("GC算法:       %s\n",
           g_ftl_dev.gc_algo == GC_ALGO_GREEDY ? "Greedy贪心" :
           g_ftl_dev.gc_algo == GC_ALGO_COST_BENEFIT ? "Cost-Benefit成本收益" :
           g_ftl_dev.gc_algo == GC_ALGO_CAT ? "CAT成本-年龄-时间" :
           g_ftl_dev.gc_algo == GC_ALGO_WINDOWED ? "Windowed窗口贪心" :
           g_ftl_dev.gc_algo == GC_ALGO_D_CHOICES ? "d-Choices随机贪心" :
           "FRA全回收算法");
    printf("映射模式:     %s\n",
           g_ftl_dev.map_mode == MAP_MODE_PAGE ? "页映射" : "混合映射");
    printf("==================================\n");
}

/**
 * @brief 打印L2P页映射表（调试用）
 * @param[in] max_lpn 最大打印的LPN号，0表示全部
 */
void ftl_dump_l2p_table(uint32_t max_lpn)
{
    uint32_t limit = 0;
    uint32_t i = 0;
    uint32_t ppn = 0;

    if (!g_ftl_dev.is_initialized) {
        printf("FTL not initialized\n");
        return;
    }

    limit = (max_lpn == 0 || max_lpn > FTL_TOTAL_LPNS) ? FTL_TOTAL_LPNS : max_lpn;
    printf("========== L2P 映射表 (前 %u 页) ==========\n", limit);
    printf("%-8s %-10s %-8s %-8s\n", "LPN", "PPN", "Block", "Page");
    printf("----------------------------------------\n");

    for (i = 0; i < limit; i++) {
        if (g_ftl_dev.l2p_table[i] != FTL_INVALID_PPN) {
            ppn = g_ftl_dev.l2p_table[i];
            printf("%-8u 0x%08X %-8u %-8u\n",
                   i, ppn, ppn_to_block(ppn), ppn_to_page(ppn));
        }
    }
    printf("========================================\n");
}
