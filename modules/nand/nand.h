/**
 * @file nand.h
 * @brief NAND Flash 模拟模块对外接口
 * @details 模拟 NAND 物理介质的特性，包括页读写、块擦除、坏块管理、磨损计数等
 */

#ifndef FTL_NAND_H
#define FTL_NAND_H

#include "common/common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  NAND 物理参数配置
 * ============================================================ */

/** @brief 单页大小（字节） */
#define NAND_PAGE_SIZE              4096U

/** @brief 每个物理块包含的页数 */
#define NAND_PAGES_PER_BLOCK        256U

/** @brief 总物理块数量 */
#define NAND_TOTAL_BLOCKS           128U

/** @brief 初始坏块比例（百分比） */
#define NAND_INIT_BAD_BLOCK_RATIO   2U

/** @brief 擦写磨损阈值，超过后概率产生坏块 */
#define NAND_ERASE_WEAR_THRESHOLD   1000U

/** @brief 高磨损下坏块产生概率（百分比） */
#define NAND_HIGH_WEAR_BAD_RATIO    5U

/** @brief 预留块数量（用于坏块替换） */
#define NAND_RESERVED_BLOCKS        8U

/** @brief 读干扰阈值：单个块读取次数超过该值触发读干扰处理 */
#define NAND_READ_DISTURB_THRESHOLD 100000U

/** @brief 读干扰检测间隔：每读取多少页检测一次 */
#define NAND_READ_DISTURB_INTERVAL  1000U

/** @brief 写入失败重试次数 */
#define NAND_WRITE_RETRY_COUNT      3U

/** @brief 读取失败重试次数 */
#define NAND_READ_RETRY_COUNT       3U

/** @brief 坏块替换触发条件：连续写入失败次数超过该值则标记为坏块 */
#define NAND_BAD_BLOCK_THRESHOLD    2U

/**
 * @brief NAND 颗粒类型枚举
 */
typedef enum {
    NAND_TYPE_SLC = 0,    ///< SLC：单层单元，1位/单元，寿命长，速度快，成本高
    NAND_TYPE_MLC = 1,    ///< MLC：多层单元，2位/单元，寿命中等，速度中等
    NAND_TYPE_TLC = 2,    ///< TLC：三层单元，3位/单元，寿命短，速度慢，成本低
    NAND_TYPE_QLC = 3     ///< QLC：四层单元，4位/单元，寿命更短，密度更高
} nand_type_t;

/** @brief SLC 擦写寿命阈值（次） */
#define NAND_SLC_ERASE_THRESHOLD    100000U

/** @brief MLC 擦写寿命阈值（次） */
#define NAND_MLC_ERASE_THRESHOLD    3000U

/** @brief TLC 擦写寿命阈值（次） */
#define NAND_TLC_ERASE_THRESHOLD    1000U

/** @brief QLC 擦写寿命阈值（次） */
#define NAND_QLC_ERASE_THRESHOLD    300U

/** @brief SLC 初始坏块比例（%） */
#define NAND_SLC_INIT_BAD_RATIO     1U

/** @brief MLC 初始坏块比例（%） */
#define NAND_MLC_INIT_BAD_RATIO     2U

/** @brief TLC 初始坏块比例（%） */
#define NAND_TLC_INIT_BAD_RATIO     3U

/** @brief QLC 初始坏块比例（%） */
#define NAND_QLC_INIT_BAD_RATIO     5U

/* ============================================================
 *  功耗模拟配置
 * ============================================================ */

/** @brief 单页读取功耗（单位：毫焦耳 mJ） */
#define NAND_POWER_READ_PER_PAGE    1U

/** @brief 单页写入功耗（单位：毫焦耳 mJ） */
#define NAND_POWER_WRITE_PER_PAGE   3U

/** @brief 单块擦除功耗（单位：毫焦耳 mJ） */
#define NAND_POWER_ERASE_PER_BLOCK  10U

/** @brief 空闲状态功耗（单位：毫焦耳/秒 mJ/s） */
#define NAND_POWER_IDLE_PER_SEC     1U

/**
 * @brief 功耗统计结构体
 */
typedef struct {
    uint32_t read_energy;       ///< 读取总能耗（mJ）
    uint32_t write_energy;      ///< 写入总能耗（mJ）
    uint32_t erase_energy;      ///< 擦除总能耗（mJ）
    uint32_t total_energy;      ///< 总能耗（mJ）
    uint32_t active_time_ms;    ///< 活动时间（ms）
    uint32_t idle_time_ms;      ///< 空闲时间（ms）
} nand_power_stats_t;

/** @brief CRC32 校验值大小（字节） */
#define NAND_CRC_SIZE               4U

/** @brief 每页的 OOB（Out-of-Band）区域大小，用于存储 CRC 等元数据 */
#define NAND_OOB_SIZE               16U

/**
 * @brief OOB 区域数据结构
 * @details 每个物理页都有一个 OOB 区域，用于存储元数据
 */
typedef struct {
    uint32_t magic;           ///< 魔数，用于验证 OOB 有效性
    uint32_t crc32;           ///< CRC32 校验值
    uint32_t ecc;             ///< ECC 校验值
    uint8_t  bad_block_mark;  ///< 坏块标记（0xFF=正常，0x00=坏块）
    uint8_t  reserved[3];     ///< 预留字节
} nand_oob_t;

/** @brief OOB 魔数，用于验证 OOB 数据有效性 */
#define NAND_OOB_MAGIC             0x4F4F4231U  /* "OOB1" */

/**
 * @brief 是否启用 ECC 纠错功能
 * @note 默认不启用，需要时定义此宏即可打开 ECC 功能
 *       支持汉明码和 BCH 码两种算法，默认使用 BCH 码
 *       实际 SSD 通常使用 BCH 码或 LDPC 码
 */
#define NAND_ENABLE_ECC

/** @brief 默认 ECC 算法：BCH 码 */
#define NAND_DEFAULT_ECC_ALGO      ECC_ALGO_BCH

/** @brief BCH 码参数：BCH(15,7)，可纠正 2 位错误 */
#define NAND_BCH_M                 4U    /* GF(2^m) 中的 m */
#define NAND_BCH_N                 15U   /* 码长 n = 2^m - 1 */
#define NAND_BCH_K                 7U    /* 数据位长度 k */
#define NAND_BCH_T                 2U    /* 可纠正错误位数 t */

/** @brief LDPC 码参数：简化版 (12,6) LDPC 码 */
#define NAND_LDPC_N                12U   /* 码长 n */
#define NAND_LDPC_K                6U    /* 数据位长度 k */
#define NAND_LDPC_M                6U    /* 校验位数量 m = n - k */
#define NAND_LDPC_MAX_ITER         10U   /* 最大迭代次数 */
#define NAND_LDPC_T                3U    /* 可纠正错误位数 t */

#ifdef NAND_ENABLE_ECC
/** @brief ECC 算法类型 */
typedef enum {
    ECC_ALGO_NONE = 0,      ///< 无 ECC
    ECC_ALGO_HAMMING = 1,   ///< 汉明码 ECC（可纠正1位错误，检测2位错误）
    ECC_ALGO_BCH = 2,       ///< BCH 码 ECC（可纠正多位错误，SSD常用）
    ECC_ALGO_LDPC = 3       ///< LDPC 码 ECC（纠错能力最强，现代SSD常用）
} ecc_algo_type_t;

/** @brief ECC 纠错结果 */
typedef enum {
    ECC_RESULT_OK = 0,          ///< 无错误
    ECC_RESULT_CORRECTED = 1,   ///< 有错误但已纠正
    ECC_RESULT_UNCORRECTABLE = 2 ///< 错误无法纠正
} ecc_result_t;
#endif /* NAND_ENABLE_ECC */

/* ============================================================
 *  枚举类型定义
 * ============================================================ */

/**
 * @brief 物理块状态枚举
 */
typedef enum {
    BLOCK_FREE = 0,    ///< 空闲块，可写入
    BLOCK_USED = 1,    ///< 已使用块（包含有效数据）
    BLOCK_BAD  = 2     ///< 坏块，不可用
} block_state_t;

/**
 * @brief 坏块类型枚举
 */
typedef enum {
    BAD_BLOCK_INIT = 0,    ///< 初始坏块（出厂坏块）
    BAD_BLOCK_WEAR = 1     ///< 磨损坏块（使用中产生）
} bad_block_type_t;

/* ============================================================
 *  结构体定义
 * ============================================================ */

/**
 * @brief 物理块控制结构体
 * @details 每个物理块的元数据，包括状态、擦写次数、有效页位图等
 */
typedef struct {
    block_state_t state;               ///< 块状态：空闲/使用/坏块
    uint32_t erase_count;              ///< 块擦写次数（磨损计数）
    uint32_t valid_page_cnt;           ///< 块内有效页数量
    uint32_t read_count;               ///< 块读取次数（读干扰检测用）
    uint8_t  need_reclaim;             ///< 读干扰标记：1=需要回收(read reclaim)
    bad_block_type_t bad_type;         ///< 坏块类型（仅坏块有效）
    uint8_t  page_valid[NAND_PAGES_PER_BLOCK];  ///< 页有效位图：1=有效，0=无效
} phy_block_t;

/**
 * @brief NAND 设备统计信息结构体
 */
typedef struct {
    uint32_t free_blocks;           ///< 空闲块数量
    uint32_t used_blocks;           ///< 已使用块数量
    uint32_t bad_blocks;            ///< 坏块数量
    uint32_t init_bad_blocks;       ///< 初始坏块数量
    uint32_t wear_bad_blocks;       ///< 磨损坏块数量
    uint32_t reserved_blocks;       ///< 预留块数量
    uint32_t total_erase_cnt;       ///< 总擦写次数
    uint32_t max_erase_cnt;         ///< 最大擦写次数
    uint32_t min_erase_cnt;         ///< 最小擦写次数
    uint32_t avg_erase_cnt;         ///< 平均擦写次数
    uint32_t total_read_pages;      ///< 总读取页数
    uint32_t total_write_pages;     ///< 总写入页数
    uint32_t read_disturb_count;    ///< 读干扰处理次数
    uint32_t read_disturb_moved;    ///< 读干扰搬迁页数
    uint32_t crc_error_count;       ///< CRC 校验错误次数
} nand_stats_t;

/* ============================================================
 *  全局变量声明
 * ============================================================ */

/**
 * @brief 物理块数组（全局实例）
 * @note 为简化模拟，FTL模块可直接访问；真实固件严格分层，通过接口访问
 */
extern phy_block_t g_phy_blocks[NAND_TOTAL_BLOCKS];

/* ============================================================
 *  初始化与反初始化
 * ============================================================ */

/**
 * @brief 初始化NAND模拟设备
 * @param[in] file_path 模拟NAND介质的文件路径
 * @retval RET_OK 初始化成功
 * @retval RET_ERR_PARAM 入参为空
 * @retval RET_ERR_INTERNAL 文件创建失败
 */
ret_code_t nand_init(const char *file_path);

/**
 * @brief 反初始化NAND设备，释放资源
 */
void nand_deinit(void);

/* ============================================================
 *  页操作接口
 * ============================================================ */

/**
 * @brief 读取指定物理页数据
 * @param[in]  block 物理块号
 * @param[in]  page  物理页号
 * @param[out] buf   数据输出缓冲区（大小必须 >= NAND_PAGE_SIZE）
 * @retval RET_OK 读取成功
 * @retval RET_ERR_PARAM 参数越界或指针为空
 * @retval RET_ERR_BAD_BLOCK 操作坏块
 * @retval RET_ERR_NOT_MAPPED 读取未写入的页
 */
ret_code_t nand_page_read(uint32_t block, uint32_t page, uint8_t *buf);

/**
 * @brief 写入指定物理页数据（仅可写空闲页）
 * @param[in] block 物理块号
 * @param[in] page  物理页号
 * @param[in] buf   待写入数据缓冲区（大小必须 >= NAND_PAGE_SIZE）
 * @retval RET_OK 写入成功
 * @retval RET_ERR_OVERWRITE 覆写已写入页（违反先擦后写）
 * @retval RET_ERR_BAD_BLOCK 操作坏块
 */
ret_code_t nand_page_write(uint32_t block, uint32_t page, const uint8_t *buf);

/* ============================================================
 *  块操作接口
 * ============================================================ */

/**
 * @brief 擦除指定物理块
 * @param[in] block 物理块号
 * @retval RET_OK 擦除成功
 * @retval RET_ERR_BAD_BLOCK 擦除失败，标记为坏块
 * @retval RET_ERR_PARAM 参数非法
 */
ret_code_t nand_block_erase(uint32_t block);

/* ============================================================
 *  状态查询接口
 * ============================================================ */

/**
 * @brief 获取当前空闲块数量
 * @return 空闲块总数，未初始化返回0
 */
uint32_t nand_get_free_block_count(void);

/**
 * @brief 获取指定块的擦写次数
 * @param[in] block 物理块号
 * @return 擦写次数，参数非法返回0
 */
uint32_t nand_get_block_erase_count(uint32_t block);

/**
 * @brief 获取指定块的状态
 * @param[in] block 物理块号
 * @return 块状态，参数非法返回BLOCK_BAD
 */
block_state_t nand_get_block_state(uint32_t block);

/**
 * @brief 获取指定块的有效页数量
 * @param[in] block 物理块号
 * @return 有效页数量，参数非法返回0
 */
uint32_t nand_get_block_valid_page_count(uint32_t block);

/**
 * @brief 检查指定页是否有效（已写入且未失效）
 * @param[in] block 物理块号
 * @param[in] page  物理页号
 * @return true 有效，false 无效或参数非法
 */
bool nand_is_page_valid(uint32_t block, uint32_t page);

/**
 * @brief 标记指定页为无效
 * @param[in] block 物理块号
 * @param[in] page  物理页号
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 * @retval RET_ERR_BAD_BLOCK 操作坏块
 * @retval RET_ERR_NOT_MAPPED 页本来就无效
 */
ret_code_t nand_mark_page_invalid(uint32_t block, uint32_t page);

/* ============================================================
 *  坏块管理接口
 * ============================================================ */

/**
 * @brief 标记指定块为坏块
 * @param[in] block 物理块号
 * @param[in] type  坏块类型
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 */
ret_code_t nand_mark_block_bad(uint32_t block, bad_block_type_t type);

/**
 * @brief 从预留块中分配一个空闲块（用于坏块替换）
 * @param[out] out_block 输出分配的块号
 * @retval RET_OK 分配成功
 * @retval RET_ERR_NO_SPACE 预留块已用完
 * @retval RET_ERR_PARAM 参数为空
 */
ret_code_t nand_alloc_reserved_block(uint32_t *out_block);

/**
 * @brief 获取剩余预留块数量
 * @return 剩余预留块数
 */
uint32_t nand_get_reserved_block_count(void);

/**
 * @brief 标记块为冷块（用于磨损均衡，降低有效页计数让GC优先回收）
 * @param[in] block 物理块号
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 * @note 仅供磨损均衡模块使用，通过降低有效页计数引导GC优先回收冷块
 */
ret_code_t nand_mark_cold_block(uint32_t block);

/* ============================================================
 *  运行时坏块替换接口
 * ============================================================ */

/**
 * @brief 运行时坏块替换：将坏块中的有效数据搬迁到预留块
 * @param[in] bad_block 坏块号
 * @param[out] new_block 输出新的替换块号
 * @retval RET_OK 替换成功
 * @retval RET_ERR_PARAM 参数非法
 * @retval RET_ERR_NO_SPACE 预留块已用完
 * @retval RET_ERR_BAD_BLOCK 块不是坏块
 * @note 这是完整的运行时坏块处理流程：
 *       1. 从预留块池分配一个新块
 *       2. 将坏块中的所有有效页搬迁到新块
 *       3. 标记原块为坏块
 *       4. 返回新块号
 */
ret_code_t nand_replace_bad_block(uint32_t bad_block, uint32_t *new_block);

/**
 * @brief 带重试的页写入（模拟真实SSD的写入重试机制）
 * @param[in] block 物理块号
 * @param[in] page 物理页号
 * @param[in] buf 数据缓冲区
 * @retval RET_OK 写入成功
 * @retval RET_ERR_BAD_BLOCK 写入失败，块可能已损坏
 * @note 写入失败时会重试几次，仍然失败则认为块已损坏
 */
ret_code_t nand_page_write_with_retry(uint32_t block, uint32_t page, const uint8_t *buf);

/**
 * @brief 带重试的页读取（模拟真实SSD的读取重试机制）
 * @param[in] block 物理块号
 * @param[in] page 物理页号
 * @param[out] buf 数据缓冲区
 * @retval RET_OK 读取成功
 * @retval RET_ERR_BAD_BLOCK 读取失败，块可能已损坏
 * @note 读取失败时会重试几次，仍然失败则认为块已损坏
 */
ret_code_t nand_page_read_with_retry(uint32_t block, uint32_t page, uint8_t *buf);

/* ============================================================
 *  读干扰管理接口
 * ============================================================ */

/**
 * @brief 获取指定块的读取次数
 * @param[in] block 物理块号
 * @return 读取次数，参数非法返回0
 */
uint32_t nand_get_block_read_count(uint32_t block);

/**
 * @brief 重置指定块的读取计数（块擦除后调用）
 * @param[in] block 物理块号
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 */
ret_code_t nand_reset_block_read_count(uint32_t block);

/**
 * @brief 模拟数据保留(Data Retention)错误注入
 * @param[in] block 物理块号
 * @param[in] error_rate 错误率（0-100，每100字节翻转的位数）
 * @return 实际注入的错误位数，参数非法返回0
 * @note 模拟高温/长时间存储后NAND浮栅电子泄漏导致的位错误
 *       真实SSD中数据保留错误率随温度和时间指数增长
 *       注入错误后可通过ECC纠错验证数据恢复能力
 */
uint32_t nand_inject_retention_errors(uint32_t block, uint32_t error_rate);

/**
 * @brief 检查是否有块需要读干扰处理
 * @param[out] out_block 输出需要处理的块号
 * @return true 有块需要处理，false 不需要
 * @note 找到读取次数最多且超过阈值的已使用块
 */
bool nand_check_read_disturb(uint32_t *out_block);

/* ============================================================
 *  统计与调试接口
 * ============================================================ */

/**
 * @brief 获取NAND设备统计信息
 * @param[out] stats 统计信息输出结构体
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数为空或未初始化
 */
ret_code_t nand_get_stats(nand_stats_t *stats);

/**
 * @brief 打印NAND设备统计信息（调试用）
 */
void nand_print_stats(void);

/**
 * @brief 打印坏块表（调试用）
 */
void nand_print_bad_block_table(void);

/* ============================================================
 *  CRC 校验接口（端到端数据保护）
 * ============================================================ */

/**
 * @brief 计算数据的 CRC32 校验值
 * @param[in] data 数据指针
 * @param[in] len  数据长度（字节）
 * @return CRC32 校验值
 * @note 使用标准 CRC32 算法，用于端到端数据保护
 *       写入时计算并存储 CRC，读取时验证 CRC，检测数据损坏
 */
uint32_t nand_crc32_calculate(const uint8_t *data, uint32_t len);

/**
 * @brief 验证数据的 CRC32 校验值
 * @param[in] data 数据指针
 * @param[in] len  数据长度（字节）
 * @param[in] crc  预期的 CRC32 值
 * @retval true 校验通过
 * @retval false 校验失败，数据可能已损坏
 */
bool nand_crc32_verify(const uint8_t *data, uint32_t len, uint32_t crc);

/**
 * @brief 获取 CRC 错误计数
 * @return CRC 错误总次数
 */
uint32_t nand_get_crc_error_count(void);

#ifdef NAND_ENABLE_ECC
/* ============================================================
 *  ECC 纠错接口（端到端数据保护增强）
 * ============================================================ */

/**
 * @brief 计算数据的汉明码 ECC 校验值
 * @param[in] data 数据指针
 * @param[in] len  数据长度（字节）
 * @param[out] ecc 输出 ECC 校验值
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 * @note 使用 (7,4) 汉明码，每 4 位数据生成 3 位校验位
 *       可以纠正 1 位错误，检测 2 位错误
 */
ret_code_t nand_ecc_hamming_encode(const uint8_t *data, uint32_t len, uint32_t *ecc);

/**
 * @brief 使用汉明码 ECC 校验并纠正数据
 * @param[in,out] data 数据指针（纠错后的数据）
 * @param[in] len  数据长度（字节）
 * @param[in] ecc  ECC 校验值
 * @return ECC 纠错结果
 * @note 检测数据错误，如果是 1 位错误则自动纠正
 *       如果是 2 位错误则检测到但无法纠正
 */
ecc_result_t nand_ecc_hamming_decode(uint8_t *data, uint32_t len, uint32_t ecc);

/**
 * @brief 计算数据的 BCH 码 ECC 校验值
 * @param[in]  data 数据指针
 * @param[in]  len  数据长度（字节）
 * @param[out] ecc  输出 ECC 校验值
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 * @note 使用 BCH(15,7) 码，可纠正 2 位错误
 *       BCH 码是 SSD 中最常用的 ECC 算法之一
 *       比汉明码纠错能力更强，适合 NAND 闪存
 */
ret_code_t nand_ecc_bch_encode(const uint8_t *data, uint32_t len, uint32_t *ecc);

/**
 * @brief 使用 BCH 码 ECC 校验并纠正数据
 * @param[in,out] data 数据指针（纠错后的数据）
 * @param[in]     len  数据长度（字节）
 * @param[in]     ecc  ECC 校验值
 * @return ECC 纠错结果
 * @note 检测数据错误，最多可纠正 2 位错误
 *       如果错误位数超过纠错能力，则返回无法纠正
 */
ecc_result_t nand_ecc_bch_decode(uint8_t *data, uint32_t len, uint32_t ecc);

/**
 * @brief 计算数据的 LDPC 码 ECC 校验值
 * @param[in]  data 数据指针
 * @param[in]  len  数据长度（字节）
 * @param[out] ecc  输出 ECC 校验值
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 * @note 使用简化版 (12,6) LDPC 码，可纠正 3 位错误
 *       LDPC（低密度奇偶校验码）是现代 SSD 最常用的 ECC 算法
 *       纠错能力比 BCH 码更强，接近香农极限
 *       本实现为简化版，实际 SSD 使用更复杂的 LDPC 码
 */
ret_code_t nand_ecc_ldpc_encode(const uint8_t *data, uint32_t len, uint32_t *ecc);

/**
 * @brief 使用 LDPC 码 ECC 校验并纠正数据
 * @param[in,out] data 数据指针（纠错后的数据）
 * @param[in]     len  数据长度（字节）
 * @param[in]     ecc  ECC 校验值
 * @return ECC 纠错结果
 * @note 使用置信传播（BP）算法进行迭代解码
 *       最多可纠正 3 位错误
 *       如果错误位数超过纠错能力，则返回无法纠正
 */
ecc_result_t nand_ecc_ldpc_decode(uint8_t *data, uint32_t len, uint32_t ecc);

/**
 * @brief 设置当前使用的 ECC 算法
 * @param[in] algo ECC 算法类型
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 */
ret_code_t nand_ecc_set_algo(ecc_algo_type_t algo);

/**
 * @brief 获取当前使用的 ECC 算法
 * @return ECC 算法类型
 */
ecc_algo_type_t nand_ecc_get_algo(void);

/**
 * @brief 获取 ECC 纠错统计
 * @param[out] corrected_count 输出已纠正的错误次数
 * @param[out] uncorrectable_count 输出无法纠正的错误次数
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数为空
 */
ret_code_t nand_get_ecc_stats(uint32_t *corrected_count, uint32_t *uncorrectable_count);
#endif /* NAND_ENABLE_ECC */

/* ============================================================
 *  多颗粒类型支持接口（SLC/MLC/TLC/QLC）
 * ============================================================ */

/**
 * @brief 设置 NAND 颗粒类型
 * @param[in] type 颗粒类型
 * @retval RET_OK 设置成功
 * @retval RET_ERR_NOT_SUPPORT 不支持的类型
 * @note 不同颗粒类型有不同的擦写寿命、初始坏块率等特性
 *       必须在 nand_init 之前调用才会生效
 */
ret_code_t nand_set_type(nand_type_t type);

/**
 * @brief 获取当前 NAND 颗粒类型
 * @return 颗粒类型
 */
nand_type_t nand_get_type(void);

/**
 * @brief 获取指定颗粒类型的擦写寿命
 * @param[in] type 颗粒类型
 * @return 擦写寿命（次）
 */
uint32_t nand_get_erase_threshold(nand_type_t type);

/* ============================================================
 *  功耗模拟与管理接口
 * ============================================================ */

/**
 * @brief 获取功耗统计信息
 * @param[out] stats 功耗统计信息输出结构体
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数为空
 */
ret_code_t nand_get_power_stats(nand_power_stats_t *stats);

/**
 * @brief 重置功耗统计
 * @retval RET_OK 成功
 */
ret_code_t nand_reset_power_stats(void);

/**
 * @brief 获取总能耗（毫焦耳）
 * @return 总能耗（mJ）
 */
uint32_t nand_get_total_energy(void);

/* ============================================================
 *  OOB 区域管理接口
 * ============================================================ */

/**
 * @brief 读取指定页的 OOB 区域数据
 * @param[in] block 物理块号
 * @param[in] page  物理页号
 * @param[out] oob  OOB 数据输出缓冲区
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 * @retval RET_ERR_BAD_BLOCK 操作坏块
 */
ret_code_t nand_oob_read(uint32_t block, uint32_t page, nand_oob_t *oob);

/**
 * @brief 写入指定页的 OOB 区域数据
 * @param[in] block 物理块号
 * @param[in] page  物理页号
 * @param[in] oob   OOB 数据输入缓冲区
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 * @retval RET_ERR_BAD_BLOCK 操作坏块
 */
ret_code_t nand_oob_write(uint32_t block, uint32_t page, const nand_oob_t *oob);

/**
 * @brief 带 CRC 校验的页读取（集成 OOB）
 * @param[in] block 物理块号
 * @param[in] page  物理页号
 * @param[out] buf  数据缓冲区
 * @retval RET_OK 成功，数据有效
 * @retval RET_ERR_CHECKSUM CRC 校验失败
 * @retval RET_ERR_PARAM 参数非法
 * @retval RET_ERR_BAD_BLOCK 操作坏块
 */
ret_code_t nand_page_read_with_crc(uint32_t block, uint32_t page, uint8_t *buf);

/**
 * @brief 带 CRC 校验的页写入（集成 OOB）
 * @param[in] block 物理块号
 * @param[in] page  物理页号
 * @param[in] buf   数据缓冲区
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 * @retval RET_ERR_BAD_BLOCK 操作坏块
 * @retval RET_ERR_OVERWRITE 覆写已写入页
 */
ret_code_t nand_page_write_with_crc(uint32_t block, uint32_t page, const uint8_t *buf);

#ifdef __cplusplus
}
#endif

#endif /* FTL_NAND_H */
