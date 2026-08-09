/**
 * @file log.h
 * @brief 日志系统接口
 * @details 提供分级日志功能，支持DEBUG/INFO/WARN/ERROR四个级别
 */

#ifndef FTL_LOG_H
#define FTL_LOG_H

#include "common/common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  日志级别定义
 * ============================================================ */

/**
 * @brief 日志级别枚举
 */
typedef enum {
    LOG_LEVEL_DEBUG = 0,  ///< 调试信息，最详细
    LOG_LEVEL_INFO  = 1,  ///< 一般信息，正常运行流程
    LOG_LEVEL_WARN  = 2,  ///< 警告信息，需要注意但不影响运行
    LOG_LEVEL_ERROR = 3,  ///< 错误信息，影响功能的错误
    LOG_LEVEL_NONE  = 4   ///< 不输出任何日志
} log_level_t;

/* ============================================================
 *  日志配置接口
 * ============================================================ */

/**
 * @brief 设置日志输出级别
 * @param[in] level 日志级别，低于该级别的日志不会输出
 */
void log_set_level(log_level_t level);

/**
 * @brief 获取当前日志级别
 * @return 当前日志级别
 */
log_level_t log_get_level(void);

/**
 * @brief 设置日志输出文件
 * @param[in] file_path 日志文件路径，NULL表示只输出到控制台
 * @retval RET_OK 成功
 * @retval RET_ERR_INTERNAL 文件打开失败
 */
ret_code_t log_set_output_file(const char *file_path);

/**
 * @brief 关闭日志文件
 */
void log_close_file(void);

/* ============================================================
 *  日志输出宏（带文件名、行号、级别）
 * ============================================================ */

/**
 * @brief 输出DEBUG级别日志
 */
#define LOG_DEBUG(fmt, ...) \
    log_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/**
 * @brief 输出INFO级别日志
 */
#define LOG_INFO(fmt, ...) \
    log_write(LOG_LEVEL_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/**
 * @brief 输出WARN级别日志
 */
#define LOG_WARN(fmt, ...) \
    log_write(LOG_LEVEL_WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/**
 * @brief 输出ERROR级别日志
 */
#define LOG_ERROR(fmt, ...) \
    log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/* ============================================================
 *  内部实现函数（不要直接调用，使用上面的宏）
 * ============================================================ */

/**
 * @brief 写日志（内部函数，通过宏调用）
 * @param[in] level 日志级别
 * @param[in] file  文件名
 * @param[in] line  行号
 * @param[in] fmt   格式化字符串
 */
void log_write(log_level_t level, const char *file, int line, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* FTL_LOG_H */
