/**
 * @file os_abstract.h
 * @brief 操作系统抽象层（OSAL）头文件
 * @details 为固件提供跨平台的操作系统接口抽象，支持Linux用户态和RTOS部署
 *          当前实现Linux平台接口，RTOS平台接口预留扩展
 * @note 设计原则：
 *       - 所有OS相关操作通过抽象接口调用，不直接使用平台API
 *       - 时间单位统一为毫秒(ms)
 *       - 错误码统一使用ret_code_t
 */
#ifndef OS_ABSTRACT_H
#define OS_ABSTRACT_H

#include <stdint.h>
#include <stdbool.h>
#include "common/common.h"

/* ============================================================
 *  平台类型定义
 * ============================================================ */

/**
 * @brief 操作系统平台类型
 */
typedef enum {
    OS_PLATFORM_LINUX = 0,   ///< Linux用户态（模拟器/开发环境）
    OS_PLATFORM_FREERTOS,    ///< FreeRTOS（嵌入式部署）
    OS_PLATFORM_RTTHREAD,    ///< RT-Thread（嵌入式部署）
    OS_PLATFORM_BAREMETAL,   ///< 裸机（无OS）
    OS_PLATFORM_MAX
} os_platform_t;

/* ============================================================
 *  互斥锁抽象
 * ============================================================ */

/**
 * @brief 互斥锁句柄（不透明类型，由平台实现定义）
 */
typedef void *os_mutex_t;

/**
 * @brief 创建互斥锁
 * @return 互斥锁句柄，失败返回NULL
 */
os_mutex_t os_mutex_create(void);

/**
 * @brief 销毁互斥锁
 * @param[in] mutex 互斥锁句柄
 */
void os_mutex_destroy(os_mutex_t mutex);

/**
 * @brief 加锁（阻塞）
 * @param[in] mutex 互斥锁句柄
 * @retval RET_OK 加锁成功
 * @retval RET_ERR_PARAM 参数非法
 */
ret_code_t os_mutex_lock(os_mutex_t mutex);

/**
 * @brief 解锁
 * @param[in] mutex 互斥锁句柄
 * @retval RET_OK 解锁成功
 * @retval RET_ERR_PARAM 参数非法
 */
ret_code_t os_mutex_unlock(os_mutex_t mutex);

/* ============================================================
 *  时间抽象
 * ============================================================ */

/**
 * @brief 获取系统启动以来的毫秒数
 * @return 毫秒数
 */
uint64_t os_get_time_ms(void);

/**
 * @brief 延时指定毫秒数
 * @param[in] ms 毫秒数
 */
void os_delay_ms(uint32_t ms);

/* ============================================================
 *  线程抽象
 * ============================================================ */

/**
 * @brief 线程句柄
 */
typedef void *os_thread_t;

/**
 * @brief 线程入口函数类型
 */
typedef void (*os_thread_func_t)(void *arg);

/**
 * @brief 创建线程
 * @param[in] func 线程入口函数
 * @param[in] arg  线程参数
 * @param[in] name 线程名称（调试用）
 * @param[in] priority 线程优先级（平台相关）
 * @param[in] stack_size 栈大小（字节）
 * @return 线程句柄，失败返回NULL
 */
os_thread_t os_thread_create(os_thread_func_t func, void *arg,
                              const char *name, uint32_t priority,
                              uint32_t stack_size);

/**
 * @brief 销毁线程
 * @param[in] thread 线程句柄
 */
void os_thread_destroy(os_thread_t thread);

/* ============================================================
 *  平台信息
 * ============================================================ */

/**
 * @brief 获取当前平台类型
 * @return 平台类型
 */
os_platform_t os_get_platform(void);

/**
 * @brief 获取平台名称字符串
 * @return 平台名称
 */
const char *os_get_platform_name(void);

#endif /* OS_ABSTRACT_H */
