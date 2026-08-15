/**
 * @file log.c
 * @brief 日志系统实现
 * @details 企业级固件的分级日志系统实现，支持 DEBUG/INFO/WARN/ERROR 四级日志。
 *          支持控制台彩色输出和文件输出，自动添加时间戳、文件名和行号。
 *          通过宏 LOG_DEBUG/LOG_INFO/LOG_WARN/LOG_ERROR 调用，
 *          自动传递文件名和行号。
 */

#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>

/* ============================================================
 *  内部私有变量
 * ============================================================ */

/**
 * @brief 当前日志级别
 * @details 低于此级别的日志将被过滤掉不输出。
 *          默认 INFO 级别，DEBUG 日志不输出。
 */
static log_level_t g_log_level = LOG_LEVEL_INFO;

/**
 * @brief 日志文件指针
 * @details 非NULL时日志同时输出到文件，NULL时仅输出到控制台。
 */
static FILE *g_log_file = NULL;

/**
 * @brief 日志级别字符串表
 * @details 索引对应 log_level_t 枚举值，用于日志输出时显示级别名称。
 */
static const char *g_level_str[] = {
    "DEBUG",
    "INFO ",
    "WARN ",
    "ERROR"
};

/**
 * @brief 日志级别颜色（ANSI转义码，控制台输出用）
 * @details DEBUG=青色，INFO=绿色，WARN=黄色，ERROR=红色。
 *          仅控制台输出使用颜色，文件输出不使用。
 */
static const char *g_level_color[] = {
    "\033[36m",  /* DEBUG - 青色 */
    "\033[32m",  /* INFO - 绿色 */
    "\033[33m",  /* WARN - 黄色 */
    "\033[31m"   /* ERROR - 红色 */
};

/* ============================================================
 *  内部辅助函数
 * ============================================================ */

/**
 * @brief 获取当前时间字符串
 * @param[out] buf 输出缓冲区
 * @param[in]  buf_size 缓冲区大小
 * @details 获取当前本地时间，格式化为 "YYYY-MM-DD HH:MM:SS" 字符串。
 *          用于日志输出的时间戳。
 */
static void log_get_time(char *buf, int buf_size)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    /* 格式化时间字符串 */
    strftime(buf, buf_size, "%Y-%m-%d %H:%M:%S", tm_info);
}

/**
 * @brief 从完整路径中提取文件名
 * @param[in] path 完整文件路径
 * @return 文件名指针（指向原字符串中的文件名部分）
 * @details 支持 Linux（/）和 Windows（\）路径分隔符。
 *          找不到分隔符时返回原路径。不分配新内存，返回指针指向原字符串。
 */
static const char *log_basename(const char *path)
{
    const char *name = NULL;

    /* 先查找 Linux 路径分隔符 */
    name = strrchr(path, '/');
    if (name == NULL) {
        /* 没找到则查找 Windows 路径分隔符 */
        name = strrchr(path, '\\');
    }

    /* 找到分隔符则返回分隔符后一个字符，否则返回原路径 */
    return (name != NULL) ? name + 1 : path;
}

/* ============================================================
 *  日志配置接口实现
 * ============================================================ */

/**
 * @brief 设置日志级别
 * @param[in] level 日志级别（DEBUG/INFO/WARN/ERROR/NONE）
 * @details 设置全局日志输出级别，低于此级别的日志将被过滤。
 *          设置为 LOG_LEVEL_NONE 时关闭所有日志输出。
 *          无效的级别值被忽略。
 */
void log_set_level(log_level_t level)
{
    /* 验证级别有效性后设置 */
    if (level >= LOG_LEVEL_DEBUG && level <= LOG_LEVEL_NONE) {
        g_log_level = level;
    }
}

/**
 * @brief 获取当前日志级别
 * @return 当前日志级别
 */
log_level_t log_get_level(void)
{
    return g_log_level;
}

/**
 * @brief 设置日志输出文件
 * @param[in] file_path 日志文件路径（NULL表示关闭文件输出）
 * @retval RET_OK 设置成功
 * @retval RET_ERR_INTERNAL 文件打开失败
 * @details 设置日志同时输出到指定文件（追加模式）。
 *          如果之前已打开文件，先关闭再打开新文件。
 *          传入NULL时仅关闭文件输出，保留控制台输出。
 */
ret_code_t log_set_output_file(const char *file_path)
{
    /* 先关闭已打开的文件（确保只有一个文件句柄） */
    log_close_file();

    /* NULL表示关闭文件输出 */
    if (file_path == NULL) {
        return RET_OK;
    }

    /* 以追加模式打开日志文件 */
    g_log_file = fopen(file_path, "a");
    if (g_log_file == NULL) {
        return RET_ERR_INTERNAL;
    }

    return RET_OK;
}

/**
 * @brief 关闭日志文件
 * @details 关闭当前打开的日志文件，将文件指针置为NULL。
 *          未打开文件时直接返回（幂等性）。
 */
void log_close_file(void)
{
    if (g_log_file != NULL) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

/* ============================================================
 *  日志输出实现
 * ============================================================ */

/**
 * @brief 写日志（核心函数）
 * @param[in] level 日志级别
 * @param[in] file 文件名（由宏自动传递 __FILE__）
 * @param[in] line 行号（由宏自动传递 __LINE__）
 * @param[in] fmt 格式化字符串（printf风格）
 * @param[in] ... 可变参数
 * @details 日志输出的核心函数，通常不直接调用，而是通过 LOG_XXX 宏调用。
 *          输出格式：[时间] [级别] 文件名:行号 - 消息
 *          控制台输出带颜色，文件输出不带颜色。
 *          低于当前日志级别的消息被过滤。
 */
void log_write(log_level_t level, const char *file, int line, const char *fmt, ...)
{
    char time_buf[32];
    const char *filename = NULL;
    va_list args;

    /* 级别过滤：低于当前级别或无效级别不输出 */
    if (level < g_log_level || level >= LOG_LEVEL_NONE) {
        return;
    }

    /* 获取时间戳字符串 */
    log_get_time(time_buf, sizeof(time_buf));

    /* 从完整路径提取文件名（日志中只显示文件名，不显示完整路径） */
    filename = log_basename(file);

    /* 输出到控制台（带颜色） */
    printf("%s[%s] [%s] %s:%d - ",
           g_level_color[level],
           time_buf,
           g_level_str[level],
           filename,
           line);

    /* 输出格式化消息 */
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    /* 重置颜色并换行 */
    printf("\033[0m\n");

    /* 输出到文件（如果配置了文件输出，不带颜色） */
    if (g_log_file != NULL) {
        fprintf(g_log_file, "[%s] [%s] %s:%d - ",
                time_buf,
                g_level_str[level],
                filename,
                line);

        /* 输出格式化消息到文件 */
        va_start(args, fmt);
        vfprintf(g_log_file, fmt, args);
        va_end(args);

        /* 换行并立即刷新（确保日志及时写入文件） */
        fprintf(g_log_file, "\n");
        fflush(g_log_file);
    }
}
