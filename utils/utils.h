/**
 * @file utils.h
 * @brief 工具函数接口
 * @details 通用工具函数
 */

#ifndef FIRMWARE_UTILS_H
#define FIRMWARE_UTILS_H

#include "common/common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  位操作工具
 * ============================================================ */

/**
 * @brief 设置指定位
 * @param[in,out] val 值指针
 * @param[in] bit 位号
 */
static inline void utils_set_bit(uint32_t *val, uint32_t bit)
{
    if (val != NULL && bit < 32U) {
        *val |= (1U << bit);
    }
}

/**
 * @brief 清除指定位
 * @param[in,out] val 值指针
 * @param[in] bit 位号
 */
static inline void utils_clear_bit(uint32_t *val, uint32_t bit)
{
    if (val != NULL && bit < 32U) {
        *val &= ~(1U << bit);
    }
}

/**
 * @brief 测试指定位
 * @param[in] val 值
 * @param[in] bit 位号
 * @return true 置位，false 未置位
 */
static inline bool utils_test_bit(uint32_t val, uint32_t bit)
{
    if (bit >= 32U) {
        return false;
    }
    return (val & (1U << bit)) != 0U;
}

/**
 * @brief 翻转指定位
 * @param[in,out] val 值指针
 * @param[in] bit 位号
 */
static inline void utils_toggle_bit(uint32_t *val, uint32_t bit)
{
    if (val != NULL && bit < 32U) {
        *val ^= (1U << bit);
    }
}

/* ============================================================
 *  对齐工具
 * ============================================================ */

/**
 * @brief 向上对齐
 * @param[in] val 值
 * @param[in] align 对齐大小（必须是2的幂）
 * @return 对齐后的值
 */
static inline uint32_t utils_align_up(uint32_t val, uint32_t align)
{
    if (align == 0U) {
        return val;
    }
    return (val + align - 1U) & ~(align - 1U);
}

/**
 * @brief 向下对齐
 * @param[in] val 值
 * @param[in] align 对齐大小（必须是2的幂）
 * @return 对齐后的值
 */
static inline uint32_t utils_align_down(uint32_t val, uint32_t align)
{
    if (align == 0U) {
        return val;
    }
    return val & ~(align - 1U);
}

/**
 * @brief 检查是否对齐
 * @param[in] val 值
 * @param[in] align 对齐大小
 * @return true 已对齐，false 未对齐
 */
static inline bool utils_is_aligned(uint32_t val, uint32_t align)
{
    if (align == 0U) {
        return true;
    }
    return (val % align) == 0U;
}

/* ============================================================
 *  数学工具
 * ============================================================ */

/**
 * @brief 计算最小值
 * @param[in] a 值a
 * @param[in] b 值b
 * @return 较小的值
 */
static inline uint32_t utils_min_u32(uint32_t a, uint32_t b)
{
    return (a < b) ? a : b;
}

/**
 * @brief 计算最大值
 * @param[in] a 值a
 * @param[in] b 值b
 * @return 较大的值
 */
static inline uint32_t utils_max_u32(uint32_t a, uint32_t b)
{
    return (a > b) ? a : b;
}

/**
 * @brief 计算绝对值
 * @param[in] val 值
 * @return 绝对值
 */
static inline int32_t utils_abs(int32_t val)
{
    return (val < 0) ? -val : val;
}

/**
 * @brief 计算 log2（向下取整）
 * @param[in] val 值
 * @return log2(val)，val为0返回0
 */
static inline uint32_t utils_log2(uint32_t val)
{
    uint32_t result = 0U;

    if (val == 0U) {
        return 0U;
    }

    while (val > 1U) {
        val >>= 1;
        result++;
    }

    return result;
}

/* ============================================================
 *  内存操作工具
 * ============================================================ */

/**
 * @brief 安全的内存拷贝
 * @param[out] dst 目标缓冲区
 * @param[in] dst_size 目标缓冲区大小
 * @param[in] src 源缓冲区
 * @param[in] src_size 源数据大小
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_OVERWRITE 缓冲区溢出
 */
ret_code_t utils_memcpy_safe(void *dst, uint32_t dst_size, const void *src, uint32_t src_size);

/**
 * @brief 安全的字符串拷贝
 * @param[out] dst 目标字符串
 * @param[in] dst_size 目标缓冲区大小
 * @param[in] src 源字符串
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_OVERWRITE 缓冲区溢出
 */
ret_code_t utils_strcpy_safe(char *dst, uint32_t dst_size, const char *src);

/**
 * @brief 计算字符串长度
 * @param[in] str 字符串
 * @param[in] max_len 最大长度
 * @return 字符串长度
 */
uint32_t utils_strnlen(const char *str, uint32_t max_len);

/* ============================================================
 *  时间工具
 * ============================================================ */

/**
 * @brief 毫秒延时
 * @param[in] ms 毫秒数
 */
void utils_delay_ms(uint32_t ms);

/**
 * @brief 微秒延时
 * @param[in] us 微秒数
 */
void utils_delay_us(uint32_t us);

/* ============================================================
 *  CRC 工具
 * ============================================================ */

/**
 * @brief 计算 CRC32
 * @param[in] data 数据指针
 * @param[in] len 数据长度
 * @return CRC32 值
 */
uint32_t utils_crc32(const uint8_t *data, uint32_t len);

/**
 * @brief 计算 CRC16
 * @param[in] data 数据指针
 * @param[in] len 数据长度
 * @return CRC16 值
 */
uint16_t utils_crc16(const uint8_t *data, uint32_t len);

/* ============================================================
 *  版本信息
 * ============================================================ */

/**
 * @brief 版本信息结构体
 */
typedef struct {
    uint8_t major;      ///< 主版本号
    uint8_t minor;      ///< 次版本号
    uint8_t patch;      ///< 补丁版本号
    uint32_t build;     ///< 构建号
    const char *name;   ///< 固件名称
    const char *date;   ///< 构建日期
    const char *time;   ///< 构建时间
} firmware_version_t;

/**
 * @brief 获取固件版本信息
 * @return 版本信息指针
 */
const firmware_version_t *utils_get_version(void);

/**
 * @brief 打印版本信息
 */
void utils_print_version(void);

#ifdef __cplusplus
}
#endif

#endif /* FIRMWARE_UTILS_H */
