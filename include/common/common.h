/**
 * @file common.h
 * @brief FTL模拟器公共类型定义与通用工具宏
 * @details 所有模块共用的基础类型、错误码、工具宏，统一收口管理
 */

#ifndef FTL_COMMON_H
#define FTL_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ============================================================
 *  通用返回码定义
 * ============================================================ */

/**
 * @brief 通用返回码枚举
 * @note 所有模块接口统一使用该返回码，错误语义全局一致
 *       负数表示错误，0表示成功
 */
typedef enum {
    RET_OK              =  0,   ///< 执行成功
    RET_ERR_PARAM       = -1,   ///< 入参非法（空指针、越界等）
    RET_ERR_NO_SPACE    = -2,   ///< 存储空间不足
    RET_ERR_BAD_BLOCK   = -3,   ///< 操作坏块
    RET_ERR_OVERWRITE   = -4,   ///< 覆写已写入的页（违反NAND先擦后写特性）
    RET_ERR_NOT_MAPPED  = -5,   ///< 逻辑页未映射
    RET_ERR_NOT_INIT    = -6,   ///< 设备未初始化
    RET_ERR_INTERNAL    = -7,   ///< 内部未知错误
    RET_ERR_CHECKSUM    = -8,   ///< 校验和错误（数据损坏）
    RET_ERR_TIMEOUT     = -9,   ///< 操作超时
    RET_ERR_BUSY        = -10,  ///< 设备忙
    RET_ERR_NOT_SUPPORT = -11   ///< 不支持的操作
} ret_code_t;

/* ============================================================
 *  通用工具宏
 * ============================================================ */

/**
 * @brief 获取数组元素个数
 */
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr)     (sizeof(arr) / sizeof((arr)[0]))
#endif

/**
 * @brief 取两个数的较小值
 */
#ifndef MIN
#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#endif

/**
 * @brief 取两个数的较大值
 */
#ifndef MAX
#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#endif

/**
 * @brief 对齐到指定边界（向上取整）
 */
#ifndef ALIGN_UP
#define ALIGN_UP(x, align)  (((x) + (align) - 1) & ~((align) - 1))
#endif

/**
 * @brief 对齐到指定边界（向下取整）
 */
#ifndef ALIGN_DOWN
#define ALIGN_DOWN(x, align) ((x) & ~((align) - 1))
#endif

/**
 * @brief 编译期断言
 */
#ifndef STATIC_ASSERT
#define STATIC_ASSERT(cond, msg) typedef char static_assert_##msg[(cond) ? 1 : -1]
#endif

#ifdef __cplusplus
}
#endif

#endif /* FTL_COMMON_H */
