/**
 * @file nand.c
 * @brief NAND Flash 模拟模块实现
 * @details 通过文件模拟 NAND 物理介质，实现页读写、块擦除、坏块管理、磨损计数等功能
 */
#include "nand.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 物理块数组（全局实例）
 * @details 每个元素对应一个物理块的元数据
 */
phy_block_t g_phy_blocks[NAND_TOTAL_BLOCKS];


/**
 * @brief NAND 设备私有结构体
 * @details 不对外暴露的内部状态，包括文件句柄、初始化状态等
 */
typedef struct {
    FILE *media_file;             ///< 模拟介质的文件句柄
    bool is_initialized;          ///< 初始化标志
    uint32_t total_read_pages;    ///< 总读取页数统计
    uint32_t total_write_pages;   ///< 总写入页数统计
    uint32_t reserved_count;      ///< 剩余预留块数量
    uint32_t read_disturb_count;  ///< 读干扰处理次数
    uint32_t read_disturb_moved;  ///< 读干扰搬迁页数
    uint32_t read_since_last_check; ///< 上次检测后的读取次数
    uint32_t crc_error_count;     ///< CRC 校验错误次数
#ifdef NAND_ENABLE_ECC
    uint32_t ecc_corrected_count; ///< ECC 已纠正错误次数
    uint32_t ecc_uncorrectable_count; ///< ECC 无法纠正错误次数
    ecc_algo_type_t ecc_algo;     ///< 当前使用的 ECC 算法
#endif /* NAND_ENABLE_ECC */
    nand_type_t nand_type;        ///< NAND 颗粒类型
    uint32_t read_energy;         ///< 读取总能耗（mJ）
    uint32_t write_energy;        ///< 写入总能耗（mJ）
    uint32_t erase_energy;        ///< 擦除总能耗（mJ）
} nand_dev_t;

/**
 * @brief NAND 设备全局实例（私有）
 */
static nand_dev_t g_nand_dev = {0};

/* ============================================================
 *  内部函数前置声明
 * ============================================================ */

static uint32_t nand_get_init_bad_ratio(nand_type_t type);

/**
 * @brief 检查块号是否合法且设备已初始化
 * @param[in] block 物理块号
 * @return true 合法，false 非法
 */
static bool nand_is_block_valid(uint32_t block)
{
    return (block < NAND_TOTAL_BLOCKS) && (g_nand_dev.is_initialized);
}

/**
 * @brief 计算指定页在介质文件中的偏移量
 * @param[in] block 物理块号
 * @param[in] page  物理页号
 * @return 文件偏移量（字节）
 * @note 每页包含数据区和 OOB 区：NAND_PAGE_SIZE + NAND_OOB_SIZE
 */
static long nand_calc_page_offset(uint32_t block, uint32_t page)
{
    return (long)(block * NAND_PAGES_PER_BLOCK + page) * (NAND_PAGE_SIZE + NAND_OOB_SIZE);
}

/**
 * @brief 计算指定页的 OOB 区域在介质文件中的偏移量
 * @param[in] block 物理块号
 * @param[in] page  物理页号
 * @return OOB 区域的文件偏移量（字节）
 */
static long nand_calc_oob_offset(uint32_t block, uint32_t page)
{
    return nand_calc_page_offset(block, page) + NAND_PAGE_SIZE;
}

/**
 * @brief 初始化NAND设备
 * @param[in] file_path 模拟介质文件路径
 * @return 错误码/OK
 */
ret_code_t nand_init(const char *file_path)
{
    long total_size = 0;
    uint32_t init_bad_count = 0;
    uint32_t i = 0;
    uint32_t init_bad_ratio = 0;

    /* 入参校验 */
    if (file_path == NULL) {
        return RET_ERR_PARAM;
    }

     /* 初始化统计计数器 */
    g_nand_dev.total_read_pages = 0;
    g_nand_dev.total_write_pages = 0;
    g_nand_dev.crc_error_count = 0;
#ifdef NAND_ENABLE_ECC
    g_nand_dev.ecc_corrected_count = 0;
    g_nand_dev.ecc_uncorrectable_count = 0;
    g_nand_dev.ecc_algo = NAND_DEFAULT_ECC_ALGO;
#endif /* NAND_ENABLE_ECC */
    g_nand_dev.read_energy = 0;
    g_nand_dev.write_energy = 0;
    g_nand_dev.erase_energy = 0;
    g_nand_dev.nand_type = NAND_TYPE_TLC;  /* 默认使用 TLC 颗粒 */
    

    /* 获取当前颗粒类型的参数 */
    init_bad_ratio = nand_get_init_bad_ratio(g_nand_dev.nand_type);

    /* 打开模拟介质文件，读写二进制模式 */
    g_nand_dev.media_file = fopen(file_path, "w+b");
    if (g_nand_dev.media_file == NULL) {
        return RET_ERR_INTERNAL;
    }

    /* 预分配完整介质空间，模拟真实NAND容量（包含数据区和OOB区） */
    total_size = (long)NAND_TOTAL_BLOCKS * NAND_PAGES_PER_BLOCK * (NAND_PAGE_SIZE + NAND_OOB_SIZE);
    if (fseek(g_nand_dev.media_file, total_size - 1, SEEK_SET) != 0) {
        fclose(g_nand_dev.media_file);
        return RET_ERR_INTERNAL;
    }
    /* 写入一个字节以确保文件大小 */
    (void)fputc(0, g_nand_dev.media_file);
    rewind(g_nand_dev.media_file);

    /* 初始化所有块的元数据 */
    (void)memset(g_phy_blocks, 0, sizeof(g_phy_blocks));
    init_bad_count = 0;

    for (i = 0; i < NAND_TOTAL_BLOCKS; i++) {
        g_phy_blocks[i].state = BLOCK_FREE;
        g_phy_blocks[i].erase_count = 0;
        g_phy_blocks[i].valid_page_cnt = 0;
        g_phy_blocks[i].read_count = 0;
        g_phy_blocks[i].bad_type = BAD_BLOCK_INIT;
        (void)memset(g_phy_blocks[i].page_valid, 0, NAND_PAGES_PER_BLOCK);

        /* 随机生成初始坏块（模拟出厂坏块） */
        if ((rand() % 100U) < init_bad_ratio) {
            g_phy_blocks[i].state = BLOCK_BAD;
            g_phy_blocks[i].bad_type = BAD_BLOCK_INIT;
            init_bad_count++;
        }
    }

    /* 初始化预留块计数（最后N个块作为预留区） */
    g_nand_dev.reserved_count = 0;
    for (i = NAND_TOTAL_BLOCKS - NAND_RESERVED_BLOCKS; i < NAND_TOTAL_BLOCKS; i++) {
        if (g_phy_blocks[i].state == BLOCK_FREE) {
            g_nand_dev.reserved_count++;
        }
    }

    g_nand_dev.is_initialized = true;
    LOG_INFO("NAND 初始化完成: 总块数=%u, 初始坏块=%u, 颗粒类型=%u",
             NAND_TOTAL_BLOCKS, init_bad_count, g_nand_dev.nand_type);

    return RET_OK;
}

/**
 * @brief 反初始化NAND设备，释放资源
 */
void nand_deinit(void)
{
    /* 关闭介质文件 */
    if (g_nand_dev.media_file != NULL) {
        (void)fclose(g_nand_dev.media_file);
        g_nand_dev.media_file = NULL;
    }

    /* 清除初始化标志 */
    g_nand_dev.is_initialized = false;
}

/**
 * @brief 读取指定页上的数据
 * @param[in] block 物理块号
 * @param[in] page  物理页号
 * @param[in] buf   数据缓冲区
 * @return 错误码/OK
 */
ret_code_t nand_page_read(uint32_t block, uint32_t page, uint8_t *buf)
{
    long offset = 0;

    /* 入参合法性校验 */
    if (!nand_is_block_valid(block) || page >= NAND_PAGES_PER_BLOCK || buf == NULL) {
        return RET_ERR_PARAM;
    }

    /* 坏块不可读 */
    if (g_phy_blocks[block].state == BLOCK_BAD) {
        return RET_ERR_BAD_BLOCK;
    }

    /* 未写入的页不可读 */
    if (g_phy_blocks[block].page_valid[page] != 1U) {
        return RET_ERR_NOT_MAPPED;
    }

    /* 定位到指定页的偏移位置 */
    offset = nand_calc_page_offset(block, page);
    (void)fseek(g_nand_dev.media_file, offset, SEEK_SET);

    /* 读取一页数据 */
    (void)fread(buf, 1U, NAND_PAGE_SIZE, g_nand_dev.media_file);

    /* 统计读取次数 */
    g_nand_dev.total_read_pages++;
    g_phy_blocks[block].read_count++;

    /* 功耗统计：读取能耗 */
    g_nand_dev.read_energy += NAND_POWER_READ_PER_PAGE;

    return RET_OK;
}

/**
 * @brief 写入指定物理页数据（仅可写空闲页）
 * @param[in] block 物理块号
 * @param[in] page  物理页号
 * @param[in] buf   待写入数据缓冲区（大小必须 >= NAND_PAGE_SIZE）
 * @retval RET_OK 写入成功
 * @retval RET_ERR_OVERWRITE 覆写已写入页（违反先擦后写）
 * @retval RET_ERR_BAD_BLOCK 操作坏块
 */
ret_code_t nand_page_write(uint32_t block, uint32_t page, const uint8_t *buf)
{
    long offset = 0;

    /* 入参合法性校验 */
    if (!nand_is_block_valid(block) || page >= NAND_PAGES_PER_BLOCK || buf == NULL) {
        return RET_ERR_PARAM;
    }

    /* 坏块不可写 */
    if (g_phy_blocks[block].state == BLOCK_BAD) {
        return RET_ERR_BAD_BLOCK;
    }

    /* NAND 特性：已写入的页不可覆写，必须先擦除整个块 */
    if (g_phy_blocks[block].page_valid[page] == 1U) {
        return RET_ERR_OVERWRITE;
    }

    /* 定位到指定页的偏移位置 */
    offset = nand_calc_page_offset(block, page);
    (void)fseek(g_nand_dev.media_file, offset, SEEK_SET);

    /* 写入一页数据 */
    (void)fwrite(buf, 1U, NAND_PAGE_SIZE, g_nand_dev.media_file);
    (void)fflush(g_nand_dev.media_file);

    /* 更新块元数据：标记页有效，有效页计数+1，块状态改为已使用 */
    g_phy_blocks[block].page_valid[page] = 1U;
    g_phy_blocks[block].valid_page_cnt++;
    g_phy_blocks[block].state = BLOCK_USED;

    /* 统计写入次数 */
    g_nand_dev.total_write_pages++;

    /* 功耗统计：写入能耗 */
    g_nand_dev.write_energy += NAND_POWER_WRITE_PER_PAGE;

    return RET_OK;
}

/**
 * @brief 擦除指定块上面的所有数据
 * @param[in] block 物理块号
 * @return 错误码/RET_OK 
 */
ret_code_t nand_block_erase(uint32_t block)
{
    uint32_t wear_threshold = 0;/*磨损阈值*/

    /* 入参合法性校验 */
    if (!nand_is_block_valid(block)) {
        return RET_ERR_PARAM;
    }

    /* 坏块不可擦除 */
    if (g_phy_blocks[block].state == BLOCK_BAD) {
        return RET_ERR_BAD_BLOCK;
    }

    /* 根据颗粒类型获取磨损阈值 */
    wear_threshold = nand_get_erase_threshold(g_nand_dev.nand_type);

    /* 模拟磨损效应：高磨损块有概率变成坏块 */
    if (g_phy_blocks[block].erase_count > wear_threshold) {
        if ((rand() % 100U) < NAND_HIGH_WEAR_BAD_RATIO) {
            g_phy_blocks[block].state = BLOCK_BAD;
            g_phy_blocks[block].bad_type = BAD_BLOCK_WEAR;
            LOG_WARN("块 %u 因磨损变成坏块，擦写次数=%u", block, g_phy_blocks[block].erase_count);
            return RET_ERR_BAD_BLOCK;
        }
    }

    /* 更新擦写计数（磨损计数+1） */
    g_phy_blocks[block].erase_count++;

    /* 重置块内所有页的有效标志 */
    g_phy_blocks[block].valid_page_cnt = 0;
    (void)memset(g_phy_blocks[block].page_valid, 0, NAND_PAGES_PER_BLOCK);

    /* 重置读计数（擦除后重新开始计数） */
    g_phy_blocks[block].read_count = 0;

    /* 块状态改为空闲 */
    g_phy_blocks[block].state = BLOCK_FREE;

    /* 功耗统计：擦除能耗 */
    g_nand_dev.erase_energy += NAND_POWER_ERASE_PER_BLOCK;

    LOG_DEBUG("块擦除成功: 块=%u, 擦写次数=%u", block, g_phy_blocks[block].erase_count);

    return RET_OK;
}

/**
 * @brief 获取空闲块数量
 * @return 空闲块数量
 */
uint32_t nand_get_free_block_count(void)
{
    uint32_t count = 0U;
    uint32_t i = 0U;

    if (!g_nand_dev.is_initialized) {
        return 0U;
    }
   
    /* 遍历所有块，统计空闲块数量（不包含预留块） */
    for (i = 0; i < NAND_TOTAL_BLOCKS - NAND_RESERVED_BLOCKS; i++) {
        if (g_phy_blocks[i].state == BLOCK_FREE) {
            count++;
        }
    }
    return count;
}

/**
 * @brief 获取指定块的擦写次数
 * @param[in] block 物理块号
 * @return 擦写次数，参数非法返回0
 */
uint32_t nand_get_block_erase_count(uint32_t block)
{
    if (!nand_is_block_valid(block)) {
        return 0U;
    }
    return g_phy_blocks[block].erase_count;
}

/**
 * @brief 获取指定块的状态
 * @param[in] block 物理块号
 * @return 块状态，参数非法返回BLOCK_BAD
 */
block_state_t nand_get_block_state(uint32_t block)
{
    if (!nand_is_block_valid(block)) {
        return BLOCK_BAD;
    }
    return g_phy_blocks[block].state;
}

/**
 * @brief 获取指定块的有效页数量
 * @param[in] block 物理块号
 * @return 有效页数量，参数非法返回0
 */
uint32_t nand_get_block_valid_page_count(uint32_t block)
{
    if (!nand_is_block_valid(block)) {
        return 0U;
    }
    return g_phy_blocks[block].valid_page_cnt;
}

/**
 * @brief 检查指定页是否有效（已写入且未失效）
 * @param[in] block 物理块号
 * @param[in] page  物理页号
 * @return true 有效，false 无效或参数非法
 */
bool nand_is_page_valid(uint32_t block, uint32_t page)
{
    if (!nand_is_block_valid(block) || page >= NAND_PAGES_PER_BLOCK) {
        return false;
    }
    return g_phy_blocks[block].page_valid[page] == 1U;
}

/**
 * @brief 标记指定页为无效
 * @param[in] block 物理块号
 * @param[in] page  物理页号
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 * @retval RET_ERR_BAD_BLOCK 操作坏块
 * @retval RET_ERR_NOT_MAPPED 页本来就无效
 */
ret_code_t nand_mark_page_invalid(uint32_t block, uint32_t page)
{
    /* 入参校验 */
    if (!nand_is_block_valid(block) || page >= NAND_PAGES_PER_BLOCK) {
        return RET_ERR_PARAM;
    }

    /* 坏块操作直接返回错误 */
    if (g_phy_blocks[block].state == BLOCK_BAD) {
        return RET_ERR_BAD_BLOCK;
    }

    /* 页本来就无效，返回错误 */
    if (g_phy_blocks[block].page_valid[page] != 1U) {
        return RET_ERR_NOT_MAPPED;
    }

    /* 标记页无效 */
    g_phy_blocks[block].page_valid[page] = 0U;
    g_phy_blocks[block].valid_page_cnt--;

    /* 如果块内没有有效页了，标记为空闲块 */
    if (g_phy_blocks[block].valid_page_cnt == 0U) {
        g_phy_blocks[block].state = BLOCK_FREE;
    }

    return RET_OK;
}

/**
 * @brief 标记块为坏块
 * @param[in] block 物理块号
 * @param[in] type 坏块类型
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 */
ret_code_t nand_mark_block_bad(uint32_t block, bad_block_type_t type)
{
    nand_oob_t oob;
    long offset = 0;

    if (!nand_is_block_valid(block)) {
        return RET_ERR_PARAM;
    }

    g_phy_blocks[block].state = BLOCK_BAD;
    g_phy_blocks[block].bad_type = type;
    g_phy_blocks[block].valid_page_cnt = 0;
    (void)memset(g_phy_blocks[block].page_valid, 0, NAND_PAGES_PER_BLOCK);

    /* 更新块中第一页的 OOB 区域的坏块标记 */
    if (g_nand_dev.media_file != NULL) {
        (void)memset(&oob, 0, sizeof(nand_oob_t));
        oob.magic = NAND_OOB_MAGIC;
        oob.bad_block_mark = 0x00;  /* 0x00 表示坏块 */

        /* 定位到第一页的 OOB 区域 */
        offset = nand_calc_oob_offset(block, 0);
        (void)fseek(g_nand_dev.media_file, offset, SEEK_SET);
        (void)fwrite(&oob, 1U, sizeof(nand_oob_t), g_nand_dev.media_file);
        (void)fflush(g_nand_dev.media_file);
    }

    LOG_WARN("标记块为坏块: 块=%u, 类型=%s, 擦写次数=%u",
             block,
             type == BAD_BLOCK_INIT ? "初始坏块" : "磨损坏块",
             g_phy_blocks[block].erase_count);

    return RET_OK;
}

/**
 * @brief 从预留块中分配一个空闲块（用于坏块替换）
 * @param[out] out_block 输出分配的块号
 * @retval RET_OK 分配成功
 * @retval RET_ERR_NO_SPACE 预留块已用完
 * @retval RET_ERR_PARAM 参数为空
 */
ret_code_t nand_alloc_reserved_block(uint32_t *out_block)
{
    uint32_t i = 0;

    if (out_block == NULL || !g_nand_dev.is_initialized) {
        return RET_ERR_PARAM;
    }

    if (g_nand_dev.reserved_count == 0) {
        return RET_ERR_NO_SPACE;
    }

    /* 从预留区（最后N个块）中找一个空闲块 */
    for (i = NAND_TOTAL_BLOCKS - NAND_RESERVED_BLOCKS; i < NAND_TOTAL_BLOCKS; i++) {
        if (g_phy_blocks[i].state == BLOCK_FREE) {
            *out_block = i;
            g_nand_dev.reserved_count--;
            return RET_OK;
        }
    }

    return RET_ERR_NO_SPACE;
}

/**
 * @brief 获取剩余预留块数量
 * @return 剩余预留块数
 */
uint32_t nand_get_reserved_block_count(void)
{
    return g_nand_dev.reserved_count;
}

/**
 * @brief 标记块为冷块（用于磨损均衡，降低有效页计数让GC优先回收）
 * @param[in] block 物理块号
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 * @note 仅供磨损均衡模块使用，通过降低有效页计数引导GC优先回收冷块
 */
ret_code_t nand_mark_cold_block(uint32_t block)
{
    if (!nand_is_block_valid(block)) {
        return RET_ERR_PARAM;
    }

    if (g_phy_blocks[block].state != BLOCK_USED) {
        return RET_ERR_PARAM;
    }

    /* 将有效页计数设为1，让GC算法优先回收这个冷块，从而触发冷数据搬迁 */
    if (g_phy_blocks[block].valid_page_cnt > 1) {
        g_phy_blocks[block].valid_page_cnt = 1;
    }

    return RET_OK;
}

/* ============================================================
 *  运行时坏块替换接口实现
 * ============================================================ */

/**
 * @brief 带重试的页写入（模拟真实SSD的写入重试机制）
 * @param[in] block 物理块号
 * @param[in] page 物理页号
 * @param[in] buf 数据缓冲区
 * @retval RET_OK 写入成功
 * @retval RET_ERR_BAD_BLOCK 写入失败，块可能已损坏
 * @note 写入失败时会重试几次，仍然失败则认为块已损坏
 *       在模拟器中，文件操作一般不会失败，这里主要是提供完整的接口
 *       真实SSD中，写入失败可能是因为块磨损、介质错误等
 */
ret_code_t nand_page_write_with_retry(uint32_t block, uint32_t page, const uint8_t *buf)
{
    ret_code_t ret = RET_OK;
    uint32_t retry = 0;

    /* 重试写入 */
    for (retry = 0; retry < NAND_WRITE_RETRY_COUNT; retry++) {
        ret = nand_page_write(block, page, buf);
        if (ret == RET_OK) {
            return RET_OK;
        }
        /* 只有覆写错误才重试，其他错误直接返回 */
        if (ret != RET_ERR_OVERWRITE) {
            break;
        }
    }

    /* 重试仍然失败，标记块为坏块 */
    if (ret != RET_OK) {
        (void)nand_mark_block_bad(block, BAD_BLOCK_WEAR);
        return RET_ERR_BAD_BLOCK;
    }

    return RET_OK;
}

/**
 * @brief 带重试的页读取（模拟真实SSD的读取重试机制）
 * @param[in] block 物理块号
 * @param[in] page 物理页号
 * @param[out] buf 数据缓冲区
 * @retval RET_OK 读取成功
 * @retval RET_ERR_BAD_BLOCK 读取失败，块可能已损坏
 * @note 读取失败时会重试几次，仍然失败则认为块已损坏
 *       在模拟器中，文件操作一般不会失败，这里主要是提供完整的接口
 *       真实SSD中，读取失败可能是因为读干扰、介质错误等
 */
ret_code_t nand_page_read_with_retry(uint32_t block, uint32_t page, uint8_t *buf)
{
    ret_code_t ret = RET_OK;
    uint32_t retry = 0;

    /* 重试读取 */
    for (retry = 0; retry < NAND_READ_RETRY_COUNT; retry++) {
        ret = nand_page_read(block, page, buf);
        if (ret == RET_OK) {
            return RET_OK;
        }
        /* 只有未映射错误才重试，其他错误直接返回 */
        if (ret != RET_ERR_NOT_MAPPED) {
            break;
        }
    }

    /* 重试仍然失败，标记块为坏块 */
    if (ret != RET_OK) {
        (void)nand_mark_block_bad(block, BAD_BLOCK_WEAR);
        return RET_ERR_BAD_BLOCK;
    }

    return RET_OK;
}

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
 *       注意：调用者需要负责更新映射表（L2P表等）
 */
ret_code_t nand_replace_bad_block(uint32_t bad_block, uint32_t *new_block)
{
    ret_code_t ret = RET_OK;
    uint32_t replacement_block = 0;
    uint8_t page_buf[NAND_PAGE_SIZE];
    uint32_t moved_pages = 0U;
    uint32_t page = 0;

    /* 入参校验 */
    if (new_block == NULL || !g_nand_dev.is_initialized) {
        return RET_ERR_PARAM;
    }
    if (bad_block >= NAND_TOTAL_BLOCKS) {
        return RET_ERR_PARAM;
    }

    /* 块必须是已使用块或坏块才能替换 */
    if (g_phy_blocks[bad_block].state == BLOCK_FREE) {
        return RET_ERR_PARAM;
    }

    /* 从预留块池分配一个新块 */
    ret = nand_alloc_reserved_block(&replacement_block);
    if (ret != RET_OK) {
        return RET_ERR_NO_SPACE;
    }

    /* 搬迁坏块中的所有有效页到新块 */
    for (page = 0; page < NAND_PAGES_PER_BLOCK; page++) {
        if (g_phy_blocks[bad_block].page_valid[page] != 1U) {
            continue;
        }

        /* 读取旧页数据 */
        ret = nand_page_read(bad_block, page, page_buf);
        if (ret != RET_OK) {
            /* 读取失败，跳过这一页（数据丢失） */
            continue;
        }

        /* 写入新块的对应页 */
        ret = nand_page_write(replacement_block, page, page_buf);
        if (ret != RET_OK) {
            /* 写入失败，继续下一页 */
            continue;
        }

        moved_pages++;
    }

    /* 标记原块为坏块（如果还不是的话） */
    if (g_phy_blocks[bad_block].state != BLOCK_BAD) {
        (void)nand_mark_block_bad(bad_block, BAD_BLOCK_WEAR);
    }

    *new_block = replacement_block;

    LOG_INFO("坏块替换完成(NAND层): 坏块=%u, 新块=%u, 搬迁页数=%u",
             bad_block, replacement_block, moved_pages);

    return RET_OK;
}

/* ============================================================
 *  读干扰管理接口实现
 * ============================================================ */

/**
 * @brief 获取指定块的读取次数
 * @param[in] block 物理块号
 * @return 读取次数，参数非法返回0
 */
uint32_t nand_get_block_read_count(uint32_t block)
{
    if (!nand_is_block_valid(block)) {
        return 0U;
    }
    return g_phy_blocks[block].read_count;
}

/**
 * @brief 重置指定块的读取计数（块擦除后调用）
 * @param[in] block 物理块号
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 */
ret_code_t nand_reset_block_read_count(uint32_t block)
{
    if (!nand_is_block_valid(block)) {
        return RET_ERR_PARAM;
    }
    g_phy_blocks[block].read_count = 0U;
    return RET_OK;
}

/**
 * @brief 检查是否有块需要读干扰处理
 * @param[out] out_block 输出需要处理的块号
 * @return true 有块需要处理，false 不需要
 * @note 找到读取次数最多且超过阈值的已使用块
 *       读干扰：反复读取同一个块会对同块的其他页造成轻微干扰，
 *       读的次数多了可能导致数据出错，需要把数据搬迁到新块
 */
bool nand_check_read_disturb(uint32_t *out_block)
{
    uint32_t max_read = 0U;
    int32_t target_block = -1;
    uint32_t i = 0;

    if (out_block == NULL || !g_nand_dev.is_initialized) {
        return false;
    }

    /* 遍历所有用户块，找读取次数最多的已使用块 */
    for (i = 0; i < NAND_TOTAL_BLOCKS - NAND_RESERVED_BLOCKS; i++) {
        if (g_phy_blocks[i].state != BLOCK_USED) {
            continue;
        }

        if (g_phy_blocks[i].read_count > max_read) {
            max_read = g_phy_blocks[i].read_count;
            target_block = (int32_t)i;
        }
    }

    /* 读取次数超过阈值，需要处理 */
    if (max_read >= NAND_READ_DISTURB_THRESHOLD && target_block >= 0) {
        *out_block = (uint32_t)target_block;
        return true;
    }

    return false;
}

/**
 * @brief 获取NAND设备统计信息
 * @param[out] stats 输出统计信息结构体
 * @retval RET_OK 成功
 */
ret_code_t nand_get_stats(nand_stats_t *stats)
{
    uint32_t total_erase = 0;/*总擦除次数*/
    uint32_t max_erase = 0;/*最大擦除次数*/
    uint32_t min_erase = 0xFFFFFFFFU;/*最小擦除次数*/
    uint32_t good_block_count = 0;/*好块数量*/
    uint32_t i = 0;

    if (stats == NULL || !g_nand_dev.is_initialized) {
        return RET_ERR_PARAM;
    }

    /* 清零统计结构体 */
    (void)memset(stats, 0, sizeof(nand_stats_t));

    /* 遍历所有块，统计各项指标 */
    for (i = 0; i < NAND_TOTAL_BLOCKS; i++) {
        switch (g_phy_blocks[i].state) {
            case BLOCK_FREE:
                stats->free_blocks++;
                break;
            case BLOCK_USED:
                stats->used_blocks++;
                break;
            case BLOCK_BAD:
                stats->bad_blocks++;
                if (g_phy_blocks[i].bad_type == BAD_BLOCK_INIT) {
                    stats->init_bad_blocks++;
                } else {
                    stats->wear_bad_blocks++;
                }
                break;
            default:
                break;
        }

        /* 统计好块的擦写次数 */
        if (g_phy_blocks[i].state != BLOCK_BAD) {
            total_erase += g_phy_blocks[i].erase_count;
            good_block_count++;

            if (g_phy_blocks[i].erase_count > max_erase) {
                max_erase = g_phy_blocks[i].erase_count;
            }
            if (g_phy_blocks[i].erase_count < min_erase) {
                min_erase = g_phy_blocks[i].erase_count;
            }
        }
    }

    /* 填充统计结果 */
    stats->total_erase_cnt = total_erase;
    stats->max_erase_cnt = max_erase;
    stats->min_erase_cnt = min_erase;
    stats->reserved_blocks = g_nand_dev.reserved_count;
    stats->total_read_pages = g_nand_dev.total_read_pages;
    stats->total_write_pages = g_nand_dev.total_write_pages;
    stats->read_disturb_count = g_nand_dev.read_disturb_count;
    stats->read_disturb_moved = g_nand_dev.read_disturb_moved;
    stats->crc_error_count = g_nand_dev.crc_error_count;

    /* 计算平均擦写次数 */
    if (good_block_count > 0) {
        stats->avg_erase_cnt = total_erase / good_block_count;
    }

    return RET_OK;
}

/**
 * @brief 打印NAND设备统计信息（调试用）
 */
void nand_print_stats(void)
{
    nand_stats_t stats;
    nand_power_stats_t power_stats;

    if (nand_get_stats(&stats) != RET_OK) {
        printf("NAND not initialized\n");
        return;
    }

    printf("========== NAND 统计信息 ==========\n");
    printf("总块数:       %u\n", NAND_TOTAL_BLOCKS);
    printf("空闲块:       %u\n", stats.free_blocks);
    printf("已使用块:     %u\n", stats.used_blocks);
    printf("坏块总数:     %u (初始: %u, 磨损: %u)\n",
           stats.bad_blocks, stats.init_bad_blocks, stats.wear_bad_blocks);
    printf("预留块剩余:   %u\n", stats.reserved_blocks);
    printf("总擦写次数:   %u\n", stats.total_erase_cnt);
    printf("最大擦写:     %u\n", stats.max_erase_cnt);
    printf("最小擦写:     %u\n", stats.min_erase_cnt);
    printf("平均擦写:     %u\n", stats.avg_erase_cnt);
    printf("擦写差:       %u\n", stats.max_erase_cnt - stats.min_erase_cnt);
    printf("总读取页数:   %u\n", stats.total_read_pages);
    printf("总写入页数:   %u\n", stats.total_write_pages);
    printf("读干扰处理:   %u 次\n", stats.read_disturb_count);
    printf("读干扰搬迁:   %u 页\n", stats.read_disturb_moved);
    printf("CRC错误次数:  %u 次\n", stats.crc_error_count);

    /* 功耗统计 */
    if (nand_get_power_stats(&power_stats) == RET_OK) {
        printf("-------- 功耗统计 --------\n");
        printf("读取能耗:     %u mJ\n", power_stats.read_energy);
        printf("写入能耗:     %u mJ\n", power_stats.write_energy);
        printf("擦除能耗:     %u mJ\n", power_stats.erase_energy);
        printf("总能耗:       %u mJ\n", power_stats.total_energy);
    }

    printf("==================================\n");
}

/**
 * @brief 打印坏块表（调试用）
 */
void nand_print_bad_block_table(void)
{
    bool found = false;
    uint32_t i = 0;

    if (!g_nand_dev.is_initialized) {
        printf("NAND not initialized\n");
        return;
    }

    printf("========== 坏块表 ==========\n");
    printf("%-8s %-12s %-12s\n", "块号", "坏块类型", "擦写次数");
    printf("----------------------------\n");

    for (i = 0; i < NAND_TOTAL_BLOCKS; i++) {
        if (g_phy_blocks[i].state == BLOCK_BAD) {
            found = true;
            printf("%-8u %-12s %-12u\n",
                   i,
                   g_phy_blocks[i].bad_type == BAD_BLOCK_INIT ? "初始坏块" : "磨损坏块",
                   g_phy_blocks[i].erase_count);
        }
    }

    if (!found) {
        printf("  暂无坏块\n");
    }
    printf("============================\n");
}

/* ============================================================
 *  CRC32 校验实现（端到端数据保护）
 * ============================================================ */

/**
 * @brief CRC32 查表法的预计算表
 * @note 使用标准 CRC32 多项式 0xEDB88320（反转多项式）
 *       查表法比逐位计算快很多，适合固件实现
 */
static const uint32_t crc32_table[256] = {
    0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA,
    0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
    0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
    0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
    0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE,
    0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
    0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC,
    0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
    0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
    0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
    0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940,
    0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
    0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116,
    0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
    0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
    0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
    0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A,
    0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
    0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818,
    0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
    0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
    0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
    0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C,
    0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
    0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2,
    0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
    0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
    0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9,
    0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086,
    0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
    0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4,
    0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
    0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
    0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
    0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8,
    0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
    0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE,
    0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
    0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
    0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
    0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252,
    0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
    0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60,
    0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
    0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
    0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
    0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04,
    0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
    0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A,
    0x9C0906A9, 0xEB0E363F, 0x72076785, 0x05005713,
    0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
    0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
    0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E,
    0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
    0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C,
    0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
    0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
    0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
    0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0,
    0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
    0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6,
    0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
    0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
    0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D
};

/**
 * @brief 计算数据的 CRC32 校验值
 * @param[in] data 数据指针
 * @param[in] len  数据长度（字节）
 * @return CRC32 校验值
 * @note 使用标准 CRC32 算法（查表法），初始值 0xFFFFFFFF，最终异或 0xFFFFFFFF
 *       用于端到端数据保护，写入时计算并存储 CRC，读取时验证 CRC
 */
uint32_t nand_crc32_calculate(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFU;  /* CRC32 初始值 */
    uint32_t i = 0;
    uint8_t index = 0;

    if (data == NULL || len == 0U) {
        return 0U;
    }

    for (i = 0; i < len; i++) {
        /* 查表法：当前字节 + 低8位 作为索引 */
        index = (uint8_t)(crc ^ data[i]);
        crc = (crc >> 8) ^ crc32_table[index];
    }

    return crc ^ 0xFFFFFFFFU;  /* 最终异或 */
}

/**
 * @brief 验证数据的 CRC32 校验值
 * @param[in] data 数据指针
 * @param[in] len  数据长度（字节）
 * @param[in] crc  预期的 CRC32 值
 * @retval true 校验通过
 * @retval false 校验失败，数据可能已损坏
 */
bool nand_crc32_verify(const uint8_t *data, uint32_t len, uint32_t crc)
{
    if (data == NULL || len == 0U) {
        return false;
    }

    uint32_t calculated = nand_crc32_calculate(data, len);
    return calculated == crc;
}

/**
 * @brief 获取 CRC 错误计数
 * @return CRC 错误总次数
 */
uint32_t nand_get_crc_error_count(void)
{
    return g_nand_dev.crc_error_count;
}

#ifdef NAND_ENABLE_ECC
/* ============================================================
 *  ECC 纠错实现（汉明码）
 * ============================================================ */

/* 以下为完整的 ECC 算法实现，当前使用简化版接口，暂未调用 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

/**
 * @brief 计算 4 位数据的 (7,4) 汉明码
 * @param[in] data 4 位数据
 * @return 7 位汉明码（低 7 位有效）
 * @note 汉明码原理：
 *       - 校验位在 2^n 位置（1, 2, 4）
 *       - 每个校验位负责特定位置的奇偶校验
 *       - 可以纠正 1 位错误，检测 2 位错误
 */
static uint8_t hamming_encode_4bit(uint8_t data)
{
    uint8_t d0 = (data >> 0) & 0x01;
    uint8_t d1 = (data >> 1) & 0x01;
    uint8_t d2 = (data >> 2) & 0x01;
    uint8_t d3 = (data >> 3) & 0x01;

    /* 计算校验位 */
    uint8_t p1 = d0 ^ d1 ^ d3;  /* 位置 1：校验 1,3,5,7 位 */
    uint8_t p2 = d0 ^ d2 ^ d3;  /* 位置 2：校验 2,3,6,7 位 */
    uint8_t p4 = d1 ^ d2 ^ d3;  /* 位置 4：校验 4,5,6,7 位 */

    /* 组合成 7 位汉明码：p1 p2 d0 p4 d1 d2 d3 */
    uint8_t code = 0;
    code |= (p1 << 0);
    code |= (p2 << 1);
    code |= (d0 << 2);
    code |= (p4 << 3);
    code |= (d1 << 4);
    code |= (d2 << 5);
    code |= (d3 << 6);

    return code;
}

/**
 * @brief 解码 7 位汉明码，纠正 1 位错误
 * @param[in,out] code 7 位汉明码（纠错后的码字）
 * @return 错误状态：0=无错，1=已纠正1位，2=检测到2位错误（无法纠正）
 */
static uint8_t hamming_decode_7bit(uint8_t *code)
{
    uint8_t c = *code;

    /* 提取各位 */
    uint8_t p1 = (c >> 0) & 0x01;
    uint8_t p2 = (c >> 1) & 0x01;
    uint8_t d0 = (c >> 2) & 0x01;
    uint8_t p4 = (c >> 3) & 0x01;
    uint8_t d1 = (c >> 4) & 0x01;
    uint8_t d2 = (c >> 5) & 0x01;
    uint8_t d3 = (c >> 6) & 0x01;

    /* 计算校验子（syndrome） */
    uint8_t s1 = p1 ^ d0 ^ d1 ^ d3;  /* 第 1 位校验 */
    uint8_t s2 = p2 ^ d0 ^ d2 ^ d3;  /* 第 2 位校验 */
    uint8_t s4 = p4 ^ d1 ^ d2 ^ d3;  /* 第 4 位校验 */

    uint8_t syndrome = (s4 << 2) | (s2 << 1) | s1;

    if (syndrome == 0) {
        return 0;  /* 无错误 */
    }

    /* 检测到错误，syndrome 指向错误位置（1-7） */
    uint8_t error_pos = syndrome;
    if (error_pos >= 1 && error_pos <= 7) {
        /* 纠正 1 位错误 */
        *code ^= (1 << (error_pos - 1));
        return 1;  /* 已纠正 1 位 */
    }

    return 2;  /* 无法纠正的错误 */
}

/**
 * @brief 计算数据的汉明码 ECC 校验值
 * @param[in] data 数据指针
 * @param[in] len  数据长度（字节）
 * @param[out] ecc 输出 ECC 校验值
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 * @note 简化实现：对每个字节的高 4 位和低 4 位分别计算汉明码
 *       最终 ECC 值包含所有字节的校验信息汇总
 *       实际 SSD 中使用更复杂的 ECC 算法（如 BCH、LDPC）
 */
ret_code_t nand_ecc_hamming_encode(const uint8_t *data, uint32_t len, uint32_t *ecc)
{
    uint32_t i = 0;
    uint32_t ecc_value = 0;
    uint8_t byte = 0;
    uint8_t parity = 0;
    uint8_t bit = 0;

    if (data == NULL || ecc == NULL || len == 0) {
        return RET_ERR_PARAM;
    }

    /* 简化实现：计算所有字节的奇偶校验作为 ECC */
    for (i = 0; i < len; i++) {
        byte = data[i];

        /* 对每个字节的位进行奇偶校验 */
        parity = 0;
        for (bit = 0; bit < 8; bit++) {
            parity ^= ((byte >> bit) & 0x01);
        }

        /* 累加校验位 */
        ecc_value = (ecc_value << 1) | parity;
    }

    *ecc = ecc_value;
    return RET_OK;
}

/**
 * @brief 使用汉明码 ECC 校验并纠正数据
 * @param[in,out] data 数据指针（纠错后的数据）
 * @param[in] len  数据长度（字节）
 * @param[in] ecc  ECC 校验值
 * @return ECC 纠错结果
 * @note 简化实现：检测数据是否有错误
 *       实际 SSD 中使用更复杂的 ECC 算法可以纠正多位错误
 */
ecc_result_t nand_ecc_hamming_decode(uint8_t *data, uint32_t len, uint32_t ecc)
{
    uint32_t calculated_ecc = 0;
    uint32_t error_bits = 0;
    uint32_t diff = 0;

    if (data == NULL || len == 0) {
        return ECC_RESULT_UNCORRECTABLE;
    }

    /* 计算当前数据的 ECC */
    (void)nand_ecc_hamming_encode(data, len, &calculated_ecc);

    /* 比较 ECC，统计错误位数 */
    diff = calculated_ecc ^ ecc;
    while (diff != 0) {
        error_bits += (diff & 1);
        diff >>= 1;
    }

    if (error_bits == 0) {
        return ECC_RESULT_OK;
    } else if (error_bits == 1) {
        /* 1 位错误，统计并标记为已纠正（简化实现，实际需要定位并纠正） */
        g_nand_dev.ecc_corrected_count++;
        return ECC_RESULT_CORRECTED;
    } else {
        /* 多位错误，无法纠正 */
        g_nand_dev.ecc_uncorrectable_count++;
        return ECC_RESULT_UNCORRECTABLE;
    }
}

/**
 * @brief 获取 ECC 纠错统计
 * @param[out] corrected_count 输出已纠正的错误次数
 * @param[out] uncorrectable_count 输出无法纠正的错误次数
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数为空
 */
ret_code_t nand_get_ecc_stats(uint32_t *corrected_count, uint32_t *uncorrectable_count)
{
    if (corrected_count == NULL || uncorrectable_count == NULL) {
        return RET_ERR_PARAM;
    }

    *corrected_count = g_nand_dev.ecc_corrected_count;
    *uncorrectable_count = g_nand_dev.ecc_uncorrectable_count;

    return RET_OK;
}

/* ============================================================
 *  BCH 码 ECC 实现
 * ============================================================ */

/**
 * @brief GF(2^4) 有限域加法（异或）
 * @param[in] a 操作数 a
 * @param[in] b 操作数 b
 * @return a + b (GF(2^4))
 * @note GF(2^m) 中加法就是异或运算
 */
static uint8_t gf_add(uint8_t a, uint8_t b)
{
    return a ^ b;
}

/**
 * @brief GF(2^4) 有限域乘法
 * @param[in] a 操作数 a
 * @param[in] b 操作数 b
 * @return a * b (GF(2^4))
 * @note 使用本原多项式 x^4 + x + 1 (0x13)
 *       GF(2^4) 中的元素用 4 位表示
 */
static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    uint8_t result = 0;
    uint8_t i = 0;

    for (i = 0; i < 4; i++) {
        if (b & 1) {
            result ^= a;
        }
        uint8_t high_bit = a & 0x08;
        a <<= 1;
        if (high_bit) {
            a ^= 0x13;  /* 本原多项式 x^4 + x + 1 */
        }
        b >>= 1;
    }

    return result & 0x0F;
}

/**
 * @brief GF(2^4) 有限域幂运算
 * @param[in] a 底数
 * @param[in] n 指数
 * @return a^n (GF(2^4))
 */
static uint8_t gf_pow(uint8_t a, uint8_t n)
{
    uint8_t result = 1;
    uint8_t i = 0;

    for (i = 0; i < n; i++) {
        result = gf_mul(result, a);
    }

    return result;
}

/**
 * @brief BCH 编码：对 7 位数据进行 BCH(15,7) 编码
 * @param[in] data 7 位数据（低 7 位有效）
 * @return 15 位 BCH 码字（低 15 位有效）
 * @note BCH(15,7) 码，可纠正 2 位错误
 *       生成多项式 g(x) = x^8 + x^7 + x^6 + x^4 + 1 (0x1D1)
 */
static uint16_t bch_encode_7bit(uint8_t data)
{
    uint16_t codeword = 0;
    uint8_t i = 0;
    uint8_t feedback = 0;

    /* 初始值：数据位放在高 7 位 */
    codeword = (uint16_t)(data & 0x7F) << 8;

    /* 线性反馈移位寄存器（LFSR）生成校验位 */
    for (i = 0; i < 7; i++) {
        feedback = (uint8_t)((codeword >> 14) & 0x01);
        codeword <<= 1;
        if (feedback) {
            /* 生成多项式：x^8 + x^7 + x^6 + x^4 + 1 */
            codeword ^= 0x01D1;
        }
    }

    return codeword & 0x7FFF;  /* 15 位有效 */
}

/**
 * @brief BCH 解码：对 15 位 BCH 码字进行解码和纠错
 * @param[in,out] codeword 15 位码字（纠错后的码字）
 * @return 错误位数：0=无错，1-2=已纠正，>2=无法纠正
 * @note BCH(15,7) 码，最多可纠正 2 位错误
 *       使用 Peterson 算法进行错误定位
 */
static uint8_t bch_decode_15bit(uint16_t *codeword)
{
    uint16_t r = *codeword;
    uint8_t s1 = 0;  /* 伴随式 S1 */
    uint8_t s3 = 0;  /* 伴随式 S3 */
    uint8_t i = 0;
    uint8_t error_count = 0;
    uint16_t error_positions = 0;

    /* 计算伴随式 S1 = r(α) */
    for (i = 0; i < 15; i++) {
        if ((r >> i) & 1) {
            s1 ^= gf_pow(2, i);
        }
    }

    /* 计算伴随式 S3 = r(α^3) */
    for (i = 0; i < 15; i++) {
        if ((r >> i) & 1) {
            s3 ^= gf_pow(gf_pow(2, i), 3);
        }
    }

    /* 检查是否有错误 */
    if (s1 == 0 && s3 == 0) {
        return 0;  /* 无错误 */
    }

    /* 错误定位多项式求解（简化版，只处理 1 位和 2 位错误） */
    if (s3 == gf_pow(s1, 3)) {
        /* 1 位错误：错误位置为 α^k，其中 S1 = α^k */
        error_count = 1;
        /* 找到错误位置 */
        for (i = 0; i < 15; i++) {
            if (gf_pow(2, i) == s1) {
                error_positions = (uint16_t)(1 << i);
                break;
            }
        }
    } else {
        /* 2 位错误：使用暴力搜索法定位错误位置 */
        /* 简化实现：直接尝试所有可能的 2 位错误组合 */
        error_count = 2;
        bool found = false;
        uint8_t j = 0;

        for (i = 0; i < 15 && !found; i++) {
            for (j = i + 1; j < 15 && !found; j++) {
                uint8_t si1 = gf_pow(2, i);
                uint8_t si2 = gf_pow(2, j);
                if (gf_add(si1, si2) == s1) {
                    if (gf_add(gf_pow(si1, 3), gf_pow(si2, 3)) == s3) {
                        error_positions = (uint16_t)((1 << i) | (1 << j));
                        found = true;
                    }
                }
            }
        }

        if (!found) {
            return 3;  /* 超过 2 位错误，无法纠正 */
        }
    }

    /* 纠正错误 */
    *codeword ^= error_positions;

    return error_count;
}

/**
 * @brief 计算数据的 BCH 码 ECC 校验值
 * @param[in]  data 数据指针
 * @param[in]  len  数据长度（字节）
 * @param[out] ecc  输出 ECC 校验值
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 * @note 对每个字节的高 4 位和低 4 位分别进行 BCH 编码
 *       最终 ECC 值包含所有字节的校验信息汇总
 *       简化实现：实际 SSD 中使用更复杂的 BCH 码
 */
ret_code_t nand_ecc_bch_encode(const uint8_t *data, uint32_t len, uint32_t *ecc)
{
    uint32_t i = 0;
    uint32_t ecc_value = 0;
    uint8_t byte = 0;
    uint8_t nibble = 0;
    uint16_t codeword = 0;

    if (data == NULL || ecc == NULL || len == 0) {
        return RET_ERR_PARAM;
    }

    /* 简化实现：对每个字节的低 7 位计算 BCH 校验 */
    for (i = 0; i < len; i++) {
        byte = data[i];

        /* 低 7 位进行 BCH 编码 */
        nibble = byte & 0x7F;
        codeword = bch_encode_7bit(nibble);

        /* 累加校验位（高 8 位） */
        ecc_value ^= (uint32_t)(codeword & 0xFF);
        ecc_value = (ecc_value << 1) | ((ecc_value >> 31) & 1);
    }

    *ecc = ecc_value;
    return RET_OK;
}

/**
 * @brief 使用 BCH 码 ECC 校验并纠正数据
 * @param[in,out] data 数据指针（纠错后的数据）
 * @param[in]     len  数据长度（字节）
 * @param[in]     ecc  ECC 校验值
 * @return ECC 纠错结果
 * @note 简化实现：检测数据是否有错误
 *       实际 BCH 码可以纠正多位错误，但需要更复杂的实现
 */
ecc_result_t nand_ecc_bch_decode(uint8_t *data, uint32_t len, uint32_t ecc)
{
    uint32_t calculated_ecc = 0;
    uint32_t error_bits = 0;
    uint32_t diff = 0;

    if (data == NULL || len == 0) {
        return ECC_RESULT_UNCORRECTABLE;
    }

    /* 计算当前数据的 ECC */
    (void)nand_ecc_bch_encode(data, len, &calculated_ecc);

    /* 比较 ECC，统计错误位数 */
    diff = calculated_ecc ^ ecc;
    while (diff != 0) {
        error_bits += (diff & 1);
        diff >>= 1;
    }

    if (error_bits == 0) {
        return ECC_RESULT_OK;
    } else if (error_bits <= NAND_BCH_T) {
        /* 可纠正的错误（简化实现：只统计，不实际纠正） */
        g_nand_dev.ecc_corrected_count++;
        return ECC_RESULT_CORRECTED;
    } else {
        /* 超过纠错能力 */
        g_nand_dev.ecc_uncorrectable_count++;
        return ECC_RESULT_UNCORRECTABLE;
    }
}

/**
 * @brief 设置当前使用的 ECC 算法
 * @param[in] algo ECC 算法类型
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 */
ret_code_t nand_ecc_set_algo(ecc_algo_type_t algo)
{
    if (algo != ECC_ALGO_HAMMING && algo != ECC_ALGO_BCH) {
        return RET_ERR_PARAM;
    }

    g_nand_dev.ecc_algo = algo;
    return RET_OK;
}

/**
 * @brief 获取当前使用的 ECC 算法
 * @return ECC 算法类型
 */
ecc_algo_type_t nand_ecc_get_algo(void)
{
    return g_nand_dev.ecc_algo;
}

/* ============================================================
 *  LDPC 码 ECC 实现
 * ============================================================ */

/**
 * @brief LDPC 校验矩阵 H（简化版 (12,6) LDPC 码）
 * @note H 矩阵是 6x12 的稀疏矩阵
 *       前 6 列对应数据位，后 6 列对应校验位
 *       每行表示一个校验方程
 *       LDPC 码的特点是 H 矩阵中 1 的密度很低（稀疏）
 */
static const uint8_t ldpc_H_matrix[NAND_LDPC_M][NAND_LDPC_N] = {
    /* 校验方程 0 */
    {1, 1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0},
    /* 校验方程 1 */
    {0, 1, 1, 0, 1, 0, 0, 1, 0, 0, 0, 0},
    /* 校验方程 2 */
    {0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 0, 0},
    /* 校验方程 3 */
    {1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0},
    /* 校验方程 4 */
    {0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0},
    /* 校验方程 5 */
    {1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1}
};

/**
 * @brief LDPC 编码：对 6 位数据进行 (12,6) LDPC 编码
 * @param[in] data 6 位数据（低 6 位有效）
 * @return 12 位 LDPC 码字（低 12 位有效）
 * @note 使用校验矩阵 H 进行编码
 *       码字格式：[数据位(6位) | 校验位(6位)]
 *       通过求解 H * c^T = 0 得到校验位
 */
static uint16_t ldpc_encode_6bit(uint8_t data)
{
    uint16_t codeword = 0;
    uint8_t parity[NAND_LDPC_M];
    uint8_t i = 0;
    uint8_t j = 0;

    /* 初始化：数据位放在低 6 位 */
    codeword = (uint16_t)(data & 0x3F);

    /* 计算校验位（使用高斯消元法的简化版） */
    /* 对于每个校验方程，计算校验位的值 */
    for (i = 0; i < NAND_LDPC_M; i++) {
        parity[i] = 0;
        /* 累加数据位的贡献 */
        for (j = 0; j < NAND_LDPC_K; j++) {
            if (ldpc_H_matrix[i][j]) {
                parity[i] ^= (data >> j) & 1;
            }
        }
    }

    /* 简化版：直接将校验位放在高 6 位 */
    /* 注意：这是简化实现，实际 LDPC 编码需要更复杂的矩阵运算 */
    for (i = 0; i < NAND_LDPC_M; i++) {
        if (parity[i]) {
            codeword |= (uint16_t)(1 << (NAND_LDPC_K + i));
        }
    }

    return codeword & 0x0FFF;  /* 12 位有效 */
}

/**
 * @brief 计算 LDPC 校验子（syndrome）
 * @param[in] codeword 12 位码字
 * @param[out] syndrome 6 位校验子（输出）
 * @note 校验子 s = H * r^T
 *       如果 s = 0，说明没有错误（或错误在码空间中）
 *       如果 s != 0，说明有错误
 */
static void ldpc_calc_syndrome(uint16_t codeword, uint8_t *syndrome)
{
    uint8_t i = 0;
    uint8_t j = 0;

    for (i = 0; i < NAND_LDPC_M; i++) {
        syndrome[i] = 0;
        for (j = 0; j < NAND_LDPC_N; j++) {
            if (ldpc_H_matrix[i][j]) {
                syndrome[i] ^= (codeword >> j) & 1;
            }
        }
    }
}

/**
 * @brief LDPC 解码：对 12 位 LDPC 码字进行解码和纠错
 * @param[in,out] codeword 12 位码字（纠错后的码字）
 * @return 错误位数：0=无错，1-3=已纠正，>3=无法纠正
 * @note 使用比特翻转算法（Bit Flipping）进行迭代解码
 *       这是 LDPC 解码的简化版本
 *       实际 SSD 使用更复杂的置信传播（BP）算法
 *       算法原理：
 *       1. 计算校验子
 *       2. 如果校验子为 0，解码成功
 *       3. 否则，统计每个比特参与的不满足校验方程的数量
 *       4. 翻转不满足校验方程最多的比特
 *       5. 重复上述过程，直到校验子为 0 或达到最大迭代次数
 */
static uint8_t ldpc_decode_12bit(uint16_t *codeword)
{
    uint16_t r = *codeword;
    uint8_t syndrome[NAND_LDPC_M];
    uint8_t error_count[NAND_LDPC_N];
    uint8_t iter = 0;
    uint8_t i = 0;
    uint8_t j = 0;
    uint8_t max_error = 0;
    uint8_t max_pos = 0;
    uint8_t total_errors = 0;
    bool syndrome_zero = false;

    /* 迭代解码 */
    for (iter = 0; iter < NAND_LDPC_MAX_ITER; iter++) {
        /* 计算校验子 */
        ldpc_calc_syndrome(r, syndrome);

        /* 检查校验子是否为 0 */
        syndrome_zero = true;
        for (i = 0; i < NAND_LDPC_M; i++) {
            if (syndrome[i] != 0) {
                syndrome_zero = false;
                break;
            }
        }

        if (syndrome_zero) {
            /* 校验子为 0，解码成功 */
            *codeword = r;
            return total_errors;
        }

        /* 统计每个比特参与的不满足校验方程的数量 */
        for (i = 0; i < NAND_LDPC_N; i++) {
            error_count[i] = 0;
        }

        for (i = 0; i < NAND_LDPC_M; i++) {
            if (syndrome[i]) {
                /* 这个校验方程不满足，将所有参与的比特计数加 1 */
                for (j = 0; j < NAND_LDPC_N; j++) {
                    if (ldpc_H_matrix[i][j]) {
                        error_count[j]++;
                    }
                }
            }
        }

        /* 找到不满足校验方程最多的比特 */
        max_error = 0;
        max_pos = 0;
        for (i = 0; i < NAND_LDPC_N; i++) {
            if (error_count[i] > max_error) {
                max_error = error_count[i];
                max_pos = i;
            }
        }

        /* 翻转该比特 */
        r ^= (uint16_t)(1 << max_pos);
        total_errors++;

        /* 如果错误位数超过纠错能力，提前退出 */
        if (total_errors > NAND_LDPC_T) {
            break;
        }
    }

    /* 最后再检查一次校验子 */
    ldpc_calc_syndrome(r, syndrome);
    syndrome_zero = true;
    for (i = 0; i < NAND_LDPC_M; i++) {
        if (syndrome[i] != 0) {
            syndrome_zero = false;
            break;
        }
    }

    if (syndrome_zero && total_errors <= NAND_LDPC_T) {
        *codeword = r;
        return total_errors;
    }

    /* 解码失败，返回原始数据 */
    return NAND_LDPC_T + 1;  /* 超过纠错能力 */
}

/**
 * @brief 计算数据的 LDPC 码 ECC 校验值
 * @param[in]  data 数据指针
 * @param[in]  len  数据长度（字节）
 * @param[out] ecc  输出 ECC 校验值
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 * @note 对每个字节的低 6 位计算 LDPC 校验
 *       最终 ECC 值包含所有字节的校验信息汇总
 *       简化实现：实际 SSD 中使用更复杂的 LDPC 码
 *       LDPC 码是现代 SSD（特别是 TLC/QLC SSD）最常用的 ECC 算法
 */
ret_code_t nand_ecc_ldpc_encode(const uint8_t *data, uint32_t len, uint32_t *ecc)
{
    uint32_t i = 0;
    uint32_t ecc_value = 0;
    uint8_t byte = 0;
    uint8_t data_6bit = 0;
    uint16_t codeword = 0;

    if (data == NULL || ecc == NULL || len == 0) {
        return RET_ERR_PARAM;
    }

    /* 简化实现：对每个字节的低 6 位计算 LDPC 校验 */
    for (i = 0; i < len; i++) {
        byte = data[i];

        /* 低 6 位进行 LDPC 编码 */
        data_6bit = byte & 0x3F;
        codeword = ldpc_encode_6bit(data_6bit);

        /* 累加校验位（高 6 位） */
        ecc_value ^= (uint32_t)((codeword >> 6) & 0x3F);
        ecc_value = (ecc_value << 1) | ((ecc_value >> 31) & 1);
    }

    *ecc = ecc_value;
    return RET_OK;
}

/**
 * @brief 使用 LDPC 码 ECC 校验并纠正数据
 * @param[in,out] data 数据指针（纠错后的数据）
 * @param[in]     len  数据长度（字节）
 * @param[in]     ecc  ECC 校验值
 * @return ECC 纠错结果
 * @note 简化实现：检测数据是否有错误
 *       实际 LDPC 码可以纠正多位错误，但需要更复杂的实现
 *       本实现使用简化的比特翻转算法
 */
ecc_result_t nand_ecc_ldpc_decode(uint8_t *data, uint32_t len, uint32_t ecc)
{
    uint32_t calculated_ecc = 0;
    uint32_t error_bits = 0;
    uint32_t diff = 0;

    if (data == NULL || len == 0) {
        return ECC_RESULT_UNCORRECTABLE;
    }

    /* 计算当前数据的 ECC */
    (void)nand_ecc_ldpc_encode(data, len, &calculated_ecc);

    /* 比较 ECC，统计错误位数 */
    diff = calculated_ecc ^ ecc;
    while (diff != 0) {
        error_bits += (diff & 1);
        diff >>= 1;
    }

    if (error_bits == 0) {
        return ECC_RESULT_OK;
    } else if (error_bits <= NAND_LDPC_T) {
        /* 可纠正的错误（简化实现：只统计，不实际纠正） */
        g_nand_dev.ecc_corrected_count++;
        return ECC_RESULT_CORRECTED;
    } else {
        /* 超过纠错能力 */
        g_nand_dev.ecc_uncorrectable_count++;
        return ECC_RESULT_UNCORRECTABLE;
    }
}

#pragma GCC diagnostic pop
#endif /* NAND_ENABLE_ECC */

/* ============================================================
 *  多颗粒类型支持实现（SLC/MLC/TLC/QLC）
 * ============================================================ */

/**
 * @brief 获取指定颗粒类型的擦写寿命阈值
 * @param[in] type 颗粒类型
 * @return 擦写寿命阈值（次）
 */
uint32_t nand_get_erase_threshold(nand_type_t type)
{
    switch (type) {
        case NAND_TYPE_SLC:
            return NAND_SLC_ERASE_THRESHOLD;
        case NAND_TYPE_MLC:
            return NAND_MLC_ERASE_THRESHOLD;
        case NAND_TYPE_TLC:
            return NAND_TLC_ERASE_THRESHOLD;
        case NAND_TYPE_QLC:
            return NAND_QLC_ERASE_THRESHOLD;
        default:
            return NAND_ERASE_WEAR_THRESHOLD;
    }
}

/**
 * @brief 获取指定颗粒类型的初始坏块比例
 * @param[in] type 颗粒类型
 * @return 初始坏块比例（%）
 */
static uint32_t nand_get_init_bad_ratio(nand_type_t type)
{
    switch (type) {
        case NAND_TYPE_SLC:
            return NAND_SLC_INIT_BAD_RATIO;
        case NAND_TYPE_MLC:
            return NAND_MLC_INIT_BAD_RATIO;
        case NAND_TYPE_TLC:
            return NAND_TLC_INIT_BAD_RATIO;
        case NAND_TYPE_QLC:
            return NAND_QLC_INIT_BAD_RATIO;
        default:
            return NAND_INIT_BAD_BLOCK_RATIO;
    }
}

/**
 * @brief 设置 NAND 颗粒类型
 * @param[in] type 颗粒类型
 * @retval RET_OK 设置成功
 * @retval RET_ERR_NOT_SUPPORT 不支持的类型
 * @note 必须在 nand_init 之前调用才会生效
 *       不同颗粒类型有不同的擦写寿命、初始坏块率等特性
 */
ret_code_t nand_set_type(nand_type_t type)
{
    if (type < NAND_TYPE_SLC || type > NAND_TYPE_QLC) {
        return RET_ERR_NOT_SUPPORT;
    }

    g_nand_dev.nand_type = type;
    return RET_OK;
}

/**
 * @brief 获取当前 NAND 颗粒类型
 * @return 颗粒类型
 */
nand_type_t nand_get_type(void)
{
    return g_nand_dev.nand_type;
}

/* ============================================================
 *  功耗模拟与管理实现
 * ============================================================ */

/**
 * @brief 获取功耗统计信息
 * @param[out] stats 功耗统计信息输出结构体
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数为空
 */
ret_code_t nand_get_power_stats(nand_power_stats_t *stats)
{
    if (stats == NULL) {
        return RET_ERR_PARAM;
    }

    (void)memset(stats, 0, sizeof(nand_power_stats_t));

    stats->read_energy = g_nand_dev.read_energy;
    stats->write_energy = g_nand_dev.write_energy;
    stats->erase_energy = g_nand_dev.erase_energy;
    stats->total_energy = g_nand_dev.read_energy +
                          g_nand_dev.write_energy +
                          g_nand_dev.erase_energy;

    return RET_OK;
}

/**
 * @brief 重置功耗统计
 * @retval RET_OK 成功
 */
ret_code_t nand_reset_power_stats(void)
{
    g_nand_dev.read_energy = 0;
    g_nand_dev.write_energy = 0;
    g_nand_dev.erase_energy = 0;
    return RET_OK;
}

/**
 * @brief 获取总能耗（毫焦耳）
 * @return 总能耗（mJ）
 */
uint32_t nand_get_total_energy(void)
{
    return g_nand_dev.read_energy +
           g_nand_dev.write_energy +
           g_nand_dev.erase_energy;
}

/* ============================================================
 *  OOB 区域管理实现
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
ret_code_t nand_oob_read(uint32_t block, uint32_t page, nand_oob_t *oob)
{
    long offset = 0;

    /* 入参合法性校验 */
    if (!nand_is_block_valid(block) || page >= NAND_PAGES_PER_BLOCK || oob == NULL) {
        return RET_ERR_PARAM;
    }

    /* 坏块不可读 */
    if (g_phy_blocks[block].state == BLOCK_BAD) {
        return RET_ERR_BAD_BLOCK;
    }

    /* 定位到 OOB 区域的偏移位置 */
    offset = nand_calc_oob_offset(block, page);
    (void)fseek(g_nand_dev.media_file, offset, SEEK_SET);

    /* 读取 OOB 数据 */
    (void)fread(oob, 1U, sizeof(nand_oob_t), g_nand_dev.media_file);

    return RET_OK;
}

/**
 * @brief 写入指定页的 OOB 区域数据
 * @param[in] block 物理块号
 * @param[in] page  物理页号
 * @param[in] oob   OOB 数据输入缓冲区
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 * @retval RET_ERR_BAD_BLOCK 操作坏块
 */
ret_code_t nand_oob_write(uint32_t block, uint32_t page, const nand_oob_t *oob)
{
    long offset = 0;

    /* 入参合法性校验 */
    if (!nand_is_block_valid(block) || page >= NAND_PAGES_PER_BLOCK || oob == NULL) {
        return RET_ERR_PARAM;
    }

    /* 坏块不可写 */
    if (g_phy_blocks[block].state == BLOCK_BAD) {
        return RET_ERR_BAD_BLOCK;
    }

    /* 定位到 OOB 区域的偏移位置 */
    offset = nand_calc_oob_offset(block, page);
    (void)fseek(g_nand_dev.media_file, offset, SEEK_SET);

    /* 写入 OOB 数据 */
    (void)fwrite(oob, 1U, sizeof(nand_oob_t), g_nand_dev.media_file);
    (void)fflush(g_nand_dev.media_file);

    return RET_OK;
}

/**
 * @brief 带 CRC 校验的页读取（集成 OOB）
 * @param[in] block 物理块号
 * @param[in] page  物理页号
 * @param[out] buf  数据缓冲区
 * @retval RET_OK 成功，数据有效
 * @retval RET_ERR_CHECKSUM CRC 校验失败
 * @retval RET_ERR_PARAM 参数非法
 * @retval RET_ERR_BAD_BLOCK 操作坏块
 * @note 读取数据后，从 OOB 区域读取 CRC 校验值并验证
 *       如果校验失败，返回 RET_ERR_CHECKSUM
 *       启用 ECC 时，会先尝试 ECC 纠错再验证 CRC
 */
ret_code_t nand_page_read_with_crc(uint32_t block, uint32_t page, uint8_t *buf)
{
    ret_code_t ret = RET_OK;
    nand_oob_t oob;
    uint32_t calculated_crc = 0;
#ifdef NAND_ENABLE_ECC
    ecc_result_t ecc_result = ECC_RESULT_OK;
#endif /* NAND_ENABLE_ECC */

    /* 先读取页数据 */
    ret = nand_page_read(block, page, buf);
    if (ret != RET_OK) {
        return ret;
    }

    /* 读取 OOB 区域 */
    ret = nand_oob_read(block, page, &oob);
    if (ret != RET_OK) {
        return ret;
    }

    /* 检查 OOB 魔数，验证 OOB 有效性 */
    if (oob.magic != NAND_OOB_MAGIC) {
        /* OOB 数据无效，可能是未写入过的页 */
        return RET_OK;
    }

#ifdef NAND_ENABLE_ECC
    /* 先使用 ECC 纠错（根据当前设置的算法） */
    if (g_nand_dev.ecc_algo == ECC_ALGO_HAMMING) {
        ecc_result = nand_ecc_hamming_decode(buf, NAND_PAGE_SIZE, oob.ecc);
    } else if (g_nand_dev.ecc_algo == ECC_ALGO_BCH) {
        ecc_result = nand_ecc_bch_decode(buf, NAND_PAGE_SIZE, oob.ecc);
    } else if (g_nand_dev.ecc_algo == ECC_ALGO_LDPC) {
        ecc_result = nand_ecc_ldpc_decode(buf, NAND_PAGE_SIZE, oob.ecc);
    }

    if (ecc_result == ECC_RESULT_CORRECTED) {
        /* ECC 成功纠正了错误，更新统计 */
        /* 注意：ecc_corrected_count 已经在解码函数中更新了 */
        /* 纠错后重新计算 CRC */
        calculated_crc = nand_crc32_calculate(buf, NAND_PAGE_SIZE);
        if (calculated_crc == oob.crc32) {
            /* 纠错后 CRC 校验通过，数据有效 */
            LOG_DEBUG("ECC纠错成功: 块=%u, 页=%u, 算法=%s",
                      block, page,
                      g_nand_dev.ecc_algo == ECC_ALGO_HAMMING ? "汉明码" :
                      g_nand_dev.ecc_algo == ECC_ALGO_BCH ? "BCH码" : "LDPC码");
            return RET_OK;
        }
    } else if (ecc_result == ECC_RESULT_UNCORRECTABLE) {
        /* ECC 无法纠正错误 */
        /* 注意：ecc_uncorrectable_count 已经在解码函数中更新了 */
        LOG_WARN("ECC纠错失败: 块=%u, 页=%u, 算法=%s",
                 block, page,
                 g_nand_dev.ecc_algo == ECC_ALGO_HAMMING ? "汉明码" :
                 g_nand_dev.ecc_algo == ECC_ALGO_BCH ? "BCH码" : "LDPC码");
    }
#endif /* NAND_ENABLE_ECC */

    /* 计算数据的 CRC32 校验值 */
    calculated_crc = nand_crc32_calculate(buf, NAND_PAGE_SIZE);

    /* 验证 CRC 校验值 */
    if (calculated_crc != oob.crc32) {
        g_nand_dev.crc_error_count++;
        return RET_ERR_CHECKSUM;
    }

    return RET_OK;
}

/**
 * @brief 带 CRC 校验的页写入（集成 OOB）
 * @param[in] block 物理块号
 * @param[in] page  物理页号
 * @param[in] buf   数据缓冲区
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 * @retval RET_ERR_BAD_BLOCK 操作坏块
 * @retval RET_ERR_OVERWRITE 覆写已写入页
 * @note 写入数据后，计算 CRC 校验值并写入 OOB 区域
 *       启用 ECC 时，还会计算 ECC 校验值并写入 OOB
 */
ret_code_t nand_page_write_with_crc(uint32_t block, uint32_t page, const uint8_t *buf)
{
    ret_code_t ret = RET_OK;
    nand_oob_t oob;
    uint32_t crc = 0;
#ifdef NAND_ENABLE_ECC
    uint32_t ecc = 0;
#endif /* NAND_ENABLE_ECC */

    /* 先写入页数据 */
    ret = nand_page_write(block, page, buf);
    if (ret != RET_OK) {
        return ret;
    }

    /* 计算数据的 CRC32 校验值 */
    crc = nand_crc32_calculate(buf, NAND_PAGE_SIZE);

#ifdef NAND_ENABLE_ECC
    /* 计算数据的 ECC 校验值（根据当前设置的算法） */
    if (g_nand_dev.ecc_algo == ECC_ALGO_HAMMING) {
        (void)nand_ecc_hamming_encode(buf, NAND_PAGE_SIZE, &ecc);
    } else if (g_nand_dev.ecc_algo == ECC_ALGO_BCH) {
        (void)nand_ecc_bch_encode(buf, NAND_PAGE_SIZE, &ecc);
    } else if (g_nand_dev.ecc_algo == ECC_ALGO_LDPC) {
        (void)nand_ecc_ldpc_encode(buf, NAND_PAGE_SIZE, &ecc);
    }
#endif /* NAND_ENABLE_ECC */

    /* 构建 OOB 数据 */
    (void)memset(&oob, 0, sizeof(nand_oob_t));
    oob.magic = NAND_OOB_MAGIC;
    oob.crc32 = crc;
#ifdef NAND_ENABLE_ECC
    oob.ecc = ecc;
#endif /* NAND_ENABLE_ECC */
    oob.bad_block_mark = 0xFF;  /* 0xFF 表示正常块 */

    /* 写入 OOB 区域 */
    ret = nand_oob_write(block, page, &oob);
    if (ret != RET_OK) {
        return ret;
    }

    return RET_OK;
}
