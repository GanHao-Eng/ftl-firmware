/**
 * @file ftl.h
 * @brief FTL 闪存转换层对外接口
 * @details 实现地址映射、垃圾回收、磨损均衡等核心功能
 */

#ifndef FTL_FTL_H
#define FTL_FTL_H

#include "common/common.h"
#include "nand.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  FTL 配置参数
 * ============================================================ */

/** @brief 逻辑页总数（70%可用空间，30%OP预留） */
#define FTL_TOTAL_LPNS              ((NAND_TOTAL_BLOCKS * NAND_PAGES_PER_BLOCK) * 7U / 10U)

/** @brief GC 触发阈值：空闲块低于该值时触发GC */
#define FTL_GC_TRIGGER_THRESHOLD    5U

/** @brief 动态磨损均衡阈值：块擦写差超过该值时触发冷数据搬迁 */
#define FTL_WEAR_DIFF_THRESHOLD     50U

/** @brief 静态磨损均衡阈值：擦写差超过该值时触发静态磨损均衡 */
#define FTL_STATIC_WEAR_THRESHOLD   100U

/** @brief 静态磨损均衡检测间隔：每写入多少页检测一次 */
#define FTL_STATIC_WEAR_INTERVAL    1000U

/** @brief 无效物理页标记 */
#define FTL_INVALID_PPN             0xFFFFFFFFU

/** @brief 无效逻辑页标记 */
#define FTL_INVALID_LPN             0xFFFFFFFFU

/** @brief 无效块号标记 */
#define FTL_INVALID_BLOCK           0xFFFFFFFFU

/** @brief 元数据魔数（用于掉电恢复校验） */
#define FTL_META_MAGIC              0x46544C31U  /* "FTL1" */

/** @brief 元数据版本号 */
#define FTL_META_VERSION            0x00010000U

/** @brief WAL日志魔数 */
#define FTL_WAL_MAGIC               0x57414C31U  /* "WAL1" */

/** @brief WAL日志最大条目数 */
#define FTL_WAL_MAX_ENTRIES         1024U

/** @brief WAL日志默认文件路径 */
#define FTL_WAL_DEFAULT_FILE        "ftl_wal.log"

/** @brief WAL是否默认启用 */
#define FTL_WAL_ENABLED_DEFAULT     true

/** @brief WAL日志条目类型 */
typedef enum {
    WAL_OP_WRITE = 0,   ///< 写入操作
    WAL_OP_ERASE = 1,   ///< 擦除操作
    WAL_OP_TRIM = 2     ///< TRIM操作
} wal_op_type_t;

/**
 * @brief WAL日志条目结构体
 */
typedef struct {
    uint32_t magic;       ///< 魔数
    wal_op_type_t op;     ///< 操作类型
    uint32_t lpn;         ///< 逻辑页号
    uint32_t old_ppn;     ///< 旧物理页号
    uint32_t new_ppn;     ///< 新物理页号
    uint32_t sequence;    ///< 序列号
    uint32_t checksum;    ///< 校验和
} wal_entry_t;

/* ============================================================
 *  枚举类型定义
 * ============================================================ */

/**
 * @brief GC 算法类型枚举
 */
typedef enum {
    GC_ALGO_GREEDY = 0,       ///< 贪心算法：选择有效页最少的块
    GC_ALGO_COST_BENEFIT = 1, ///< 成本收益算法：综合有效页数与擦写次数
    GC_ALGO_CAT = 2,          ///< CAT算法：成本-年龄-时间综合算法
    GC_ALGO_WINDOWED = 3,     ///< Windowed算法：基于时间窗口的贪心算法
    GC_ALGO_D_CHOICES = 4,    ///< d-Choices算法：随机选择d个块中最优的
    GC_ALGO_FRA = 5           ///< FRA算法：全回收算法，回收所有可回收块
} gc_algo_type_t;

/** @brief CAT算法年龄权重 */
#define FTL_GC_CAT_AGE_WEIGHT     100U

/** @brief Windowed算法窗口大小（块数） */
#define FTL_GC_WINDOW_SIZE        32U

/** @brief d-Choices算法的d值（随机选择的块数） */
#define FTL_GC_D_CHOICES_D        5U

/** @brief 混合映射：逻辑块大小（多少个逻辑页组成一个逻辑块） */
#define FTL_LOGICAL_BLOCK_PAGES   64U

/** @brief 混合映射：热数据访问计数阈值 */
#define FTL_HOT_DATA_THRESHOLD    10U

/** @brief 混合映射：冷热迁移检测间隔（写入次数） */
#define FTL_HOT_COLD_CHECK_INTERVAL  100U

/** @brief 混合映射：冷数据降级阈值（访问次数低于该值则降级为块映射） */
#define FTL_COLD_DATA_THRESHOLD     2U

/** @brief 混合映射：最大热数据块数量限制（防止内存占用过大） */
#define FTL_MAX_HOT_BLOCKS          64U

/**
 * @brief 映射模式枚举
 */
typedef enum {
    MAP_MODE_PAGE = 0,    ///< 页映射：全页级映射，灵活性最高，内存占用大
    MAP_MODE_HYBRID = 1   ///< 混合映射：热数据页映射，冷数据块映射，平衡内存与性能
} map_mode_t;

/* ============================================================
 *  结构体定义
 * ============================================================ */

/**
 * @brief 块映射项结构体
 * @details 混合映射模式下，冷数据使用块级映射
 */
typedef struct {
    uint32_t physical_block;   ///< 映射的物理块号
    bool is_valid;             ///< 映射项是否有效
    bool is_hot;               ///< 是否为热数据（热数据使用页映射）
    uint32_t access_count;     ///< 访问计数，用于冷热判断
} block_map_entry_t;

/**
 * @brief FTL 统计信息结构体
 */
typedef struct {
    uint32_t total_lpns;              ///< 总逻辑页数
    uint32_t used_lpns;               ///< 已使用逻辑页数
    uint32_t host_write_pages;        ///< 主机写入页数
    uint32_t nand_write_pages;        ///< NAND实际写入页数（含GC搬迁）
    uint32_t gc_count;                ///< GC执行次数
    uint32_t gc_moved_pages;          ///< GC搬迁的有效页数
    uint32_t wear_leveling_count;     ///< 动态磨损均衡触发次数
    uint32_t static_wear_count;       ///< 静态磨损均衡触发次数
    uint32_t static_wear_moved_pages; ///< 静态磨损均衡搬迁页数
    uint32_t trim_count;              ///< TRIM处理次数
    uint32_t trim_pages;              ///< TRIM释放的页数
    uint32_t bad_block_replace_count; ///< 坏块替换次数
    uint32_t bad_block_moved_pages;   ///< 坏块替换搬迁页数
    uint32_t hot_cold_migration_count; ///< 冷热数据迁移次数
    uint32_t hot_block_count;          ///< 热数据块数量
    uint32_t cold_block_count;         ///< 冷数据块数量
    double   waf;                      ///< 写放大系数
} ftl_stats_t;

/**
 * @brief FTL 元数据持久化结构体
 * @details 用于掉电保护，保存关键元数据到NAND
 */
typedef struct {
    uint32_t magic;              ///< 魔数，校验有效性
    uint32_t version;            ///< 版本号
    uint32_t checksum;           ///< 校验和
    uint32_t host_write_pages;   ///< 主机写入计数
    uint32_t nand_write_pages;   ///< NAND写入计数
    uint32_t gc_count;           ///< GC计数
    uint32_t cur_write_block;    ///< 当前写入块
    uint32_t cur_write_page;     ///< 当前写入页偏移
} ftl_meta_header_t;

/* ============================================================
 *  初始化与反初始化
 * ============================================================ */

/**
 * @brief 初始化FTL层
 * @retval RET_OK 初始化成功
 * @retval RET_ERR_NO_SPACE 无可用空闲块
 * @retval RET_ERR_INTERNAL 内部错误
 */
ret_code_t ftl_init(void);

/**
 * @brief 反初始化FTL层
 */
void ftl_deinit(void);

/* ============================================================
 *  读写接口
 * ============================================================ */

/**
 * @brief 读取指定逻辑页数据
 * @param[in]  lpn 逻辑页号
 * @param[out] buf 数据输出缓冲区
 * @retval RET_OK 读取成功
 * @retval RET_ERR_NOT_MAPPED 逻辑页未映射
 * @retval RET_ERR_PARAM 参数非法
 */
ret_code_t ftl_read(uint32_t lpn, uint8_t *buf);

/**
 * @brief 写入指定逻辑页数据（异地更新）
 * @param[in] lpn 逻辑页号
 * @param[in] buf 待写入数据缓冲区
 * @retval RET_OK 写入成功
 * @retval RET_ERR_NO_SPACE 空间耗尽，GC无法回收
 * @retval RET_ERR_PARAM 参数非法
 */
ret_code_t ftl_write(uint32_t lpn, const uint8_t *buf);

/* ============================================================
 *  GC 控制接口
 * ============================================================ */

/**
 * @brief 设置GC算法类型
 * @param[in] algo 算法类型
 */
void ftl_set_gc_algo(gc_algo_type_t algo);

/**
 * @brief 获取当前GC算法类型
 * @return GC算法类型
 */
gc_algo_type_t ftl_get_gc_algo(void);

/**
 * @brief 手动触发一次GC
 * @retval RET_OK 成功
 * @retval RET_ERR_NO_SPACE 无可回收块
 * @retval RET_ERR_NOT_INIT 未初始化
 */
ret_code_t ftl_trigger_gc(void);

/* ============================================================
 *  映射模式接口
 * ============================================================ */

/**
 * @brief 设置映射模式
 * @param[in] mode 映射模式（页映射/混合映射）
 * @retval RET_OK 成功
 * @retval RET_ERR_NOT_SUPPORT 不支持的模式
 */
ret_code_t ftl_set_map_mode(map_mode_t mode);

/**
 * @brief 获取当前映射模式
 * @return 映射模式
 */
map_mode_t ftl_get_map_mode(void);

/* ============================================================
 *  混合映射接口
 * ============================================================ */

/**
 * @brief 获取块映射表项
 * @param[in] logical_block 逻辑块号
 * @param[out] entry 输出映射表项
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 * @retval RET_ERR_NOT_MAPPED 块未映射
 */
ret_code_t ftl_get_block_map_entry(uint32_t logical_block, block_map_entry_t *entry);

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
                                    uint32_t *cold_blocks);

/**
 * @brief 手动触发一次冷热数据迁移检测
 * @retval RET_OK 成功
 * @retval RET_ERR_NOT_INIT 未初始化
 * @note 检测冷热数据，将热数据从块映射升级为页映射，
 *       将冷数据从页映射降级为块映射
 */
ret_code_t ftl_hot_cold_migration(void);

/* ============================================================
 *  坏块管理接口
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
ret_code_t ftl_replace_bad_block(uint32_t block);

/**
 * @brief 获取坏块替换统计信息
 * @param[out] replace_count 输出坏块替换次数
 * @param[out] moved_pages 输出搬迁的总页数
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数为空
 */
ret_code_t ftl_get_bad_block_stats(uint32_t *replace_count, uint32_t *moved_pages);

/* ============================================================
 *  读干扰管理接口
 * ============================================================ */

/**
 * @brief 手动触发一次读干扰检测和处理
 * @retval RET_OK 成功（可能没有需要处理的块）
 * @retval RET_ERR_NOT_INIT 未初始化
 * @note 读干扰：反复读取同一个块会对同块其他页造成轻微干扰，
 *       读的次数多了可能导致数据出错，需要把数据搬迁到新块
 */
ret_code_t ftl_handle_read_disturb(void);

/* ============================================================
 *  TRIM/Discard 接口
 * ============================================================ */

/**
 * @brief TRIM 操作：通知 FTL 指定逻辑页的数据已无效
 * @param[in] lpn 起始逻辑页号
 * @param[in] count 要 TRIM 的页数
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 * @retval RET_ERR_NOT_INIT 未初始化
 * @note TRIM 后这些页的数据会被标记为无效，GC 可以直接回收，不需要搬迁
 */
ret_code_t ftl_trim(uint32_t lpn, uint32_t count);

/* ============================================================
 *  安全擦除接口
 * ============================================================ */

/**
 * @brief 安全擦除指定范围的逻辑页
 * @param[in] lpn 起始逻辑页号
 * @param[in] count 逻辑页数量
 * @param[in] passes 覆写次数（建议 >= 3）
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_INIT 未初始化
 * @note 安全擦除通过多次覆写不同数据模式来确保数据不可恢复
 *       覆写模式：全0 → 全1 → 随机数据 → 擦除
 */
ret_code_t ftl_secure_erase(uint32_t lpn, uint32_t count, uint32_t passes);

/**
 * @brief 安全擦除整个设备
 * @param[in] passes 覆写次数（建议 >= 3）
 * @retval RET_OK 成功
 * @retval RET_ERR_NOT_INIT 未初始化
 * @note 擦除所有逻辑页，恢复到出厂状态
 */
ret_code_t ftl_secure_erase_all(uint32_t passes);

/* ============================================================
 *  掉电保护接口
 * ============================================================ */

/**
 * @brief 保存元数据快照（模拟掉电保护）
 * @param[in] file_path 元数据保存路径
 * @retval RET_OK 保存成功
 * @retval RET_ERR_PARAM 参数非法
 * @retval RET_ERR_INTERNAL 写入失败
 */
ret_code_t ftl_save_snapshot(const char *file_path);

/**
 * @brief 从快照恢复元数据（模拟掉电恢复）
 * @param[in] file_path 元数据文件路径
 * @retval RET_OK 恢复成功
 * @retval RET_ERR_CHECKSUM 校验和错误，数据损坏
 * @retval RET_ERR_INTERNAL 读取失败
 */
ret_code_t ftl_load_snapshot(const char *file_path);

/* ============================================================
 *  WAL 日志恢复接口
 * ============================================================ */

/**
 * @brief 初始化 WAL 日志
 * @param[in] file_path 日志文件路径
 * @retval RET_OK 成功
 * @retval RET_ERR_INTERNAL 打开文件失败
 */
ret_code_t ftl_wal_init(const char *file_path);

/**
 * @brief 写入一条 WAL 日志
 * @param[in] entry 日志条目
 * @retval RET_OK 成功
 * @retval RET_ERR_INTERNAL 写入失败
 * @note 写入操作前必须先写 WAL 日志（Write-Ahead Log原则）
 */
ret_code_t ftl_wal_append(const wal_entry_t *entry);

/**
 * @brief 重放 WAL 日志，恢复状态
 * @param[in] file_path 日志文件路径
 * @retval RET_OK 恢复成功
 * @retval RET_ERR_INTERNAL 读取失败
 * @note 掉电恢复时调用，重放所有日志条目，恢复映射表一致性
 */
ret_code_t ftl_wal_replay(const char *file_path);

/**
 * @brief 清空 WAL 日志（ checkpoint 后调用）
 * @retval RET_OK 成功
 */
ret_code_t ftl_wal_clear(void);

/**
 * @brief 启用/禁用 WAL 日志
 * @param[in] enable true 启用，false 禁用
 * @note 禁用后写入操作不再写 WAL，掉电保护能力下降
 */
void ftl_wal_enable(bool enable);

/**
 * @brief 检查 WAL 是否已启用
 * @return true 已启用，false 已禁用
 */
bool ftl_wal_is_enabled(void);

/* ============================================================
 *  统计与调试接口
 * ============================================================ */

/**
 * @brief 获取主机写入页数
 * @return 主机写入页数
 */
uint32_t ftl_get_host_write_pages(void);

/**
 * @brief 获取NAND实际写入页数
 * @return NAND写入页数
 */
uint32_t ftl_get_nand_write_pages(void);

/**
 * @brief 获取GC执行次数
 * @return GC次数
 */
uint32_t ftl_get_gc_count(void);

/**
 * @brief 获取写放大系数
 * @return WAF值
 */
double ftl_get_write_amplification_factor(void);

/**
 * @brief 获取FTL统计信息
 * @param[out] stats 统计信息输出结构体
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数为空或未初始化
 */
ret_code_t ftl_get_stats(ftl_stats_t *stats);

/**
 * @brief 打印FTL统计信息（调试用）
 */
void ftl_print_stats(void);

/**
 * @brief 打印L2P映射表（调试用）
 * @param[in] max_lpn 最大打印的LPN号，0表示全部
 */
void ftl_dump_l2p_table(uint32_t max_lpn);

#ifdef __cplusplus
}
#endif

#endif /* FTL_FTL_H */
