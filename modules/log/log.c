/**
 * @file log.c
 * @brief 日志系统实现
 * @details 实现分级日志功能，支持控制台和文件输出
 */

#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>

/* ============================================================
 *  内部私有变量
 * ============================================================ */

/** @brief 当前日志级别 */
static log_level_t g_log_level = LOG_LEVEL_INFO;

/** @brief 日志文件指针 */
static FILE *g_log_file = NULL;

/** @brief 日志级别字符串 */
static const char *g_level_str[] = {
    "DEBUG",
    "INFO ",
    "WARN ",
    "ERROR"
};

/** @brief 日志级别颜色（控制台输出用） */
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
 */
static void log_get_time(char *buf, int buf_size)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buf, buf_size, "%Y-%m-%d %H:%M:%S", tm_info);
}

/**
 * @brief 从完整路径中提取文件名
 * @param[in] path 完整文件路径
 * @return 文件名指针
 */
static const char *log_basename(const char *path)
{
    const char *name = strrchr(path, '/');
    if (name == NULL) {
        name = strrchr(path, '\\');  /* Windows 路径 */
    }
    return (name != NULL) ? name + 1 : path;
}

/* ============================================================
 *  日志配置接口实现
 * ============================================================ */

void log_set_level(log_level_t level)
{
    if (level >= LOG_LEVEL_DEBUG && level <= LOG_LEVEL_NONE) {
        g_log_level = level;
    }
}

log_level_t log_get_level(void)
{
    return g_log_level;
}

ret_code_t log_set_output_file(const char *file_path)
{
    /* 先关闭已打开的文件 */
    log_close_file();

    if (file_path == NULL) {
        return RET_OK;
    }

    g_log_file = fopen(file_path, "a");
    if (g_log_file == NULL) {
        return RET_ERR_INTERNAL;
    }

    return RET_OK;
}

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

void log_write(log_level_t level, const char *file, int line, const char *fmt, ...)
{
    /* 级别不够，不输出 */
    if (level < g_log_level || level >= LOG_LEVEL_NONE) {
        return;
    }

    char time_buf[32];
    log_get_time(time_buf, sizeof(time_buf));

    const char *filename = log_basename(file);

    /* 输出到控制台 */
    printf("%s[%s] [%s] %s:%d - ",
           g_level_color[level],
           time_buf,
           g_level_str[level],
           filename,
           line);

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    printf("\033[0m\n");  /* 重置颜色 */

    /* 输出到文件（如果配置了） */
    if (g_log_file != NULL) {
        fprintf(g_log_file, "[%s] [%s] %s:%d - ",
                time_buf,
                g_level_str[level],
                filename,
                line);

        va_start(args, fmt);
        vfprintf(g_log_file, fmt, args);
        va_end(args);

        fprintf(g_log_file, "\n");
        fflush(g_log_file);
    }
}
