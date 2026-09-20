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

/* ============================================================
 *  消息队列抽象
 * ============================================================ */

/**
 * @brief 消息队列句柄（不透明类型）
 */
typedef void *os_queue_t;

/**
 * @brief 创建消息队列
 * @param[in] queue_length 队列长度（消息条数）
 * @param[in] item_size 每条消息大小（字节）
 * @return 队列句柄，失败返回NULL
 */
os_queue_t os_queue_create(uint32_t queue_length, uint32_t item_size);

/**
 * @brief 销毁消息队列
 * @param[in] queue 队列句柄
 */
void os_queue_destroy(os_queue_t queue);

/**
 * @brief 向队列发送消息（阻塞）
 * @param[in] queue 队列句柄
 * @param[in] item 消息数据指针
 * @param[in] timeout_ms 超时时间（毫秒），0表示不等待
 * @retval RET_OK 成功
 * @retval RET_ERR_TIMEOUT 超时
 * @retval RET_ERR_PARAM 参数非法
 */
ret_code_t os_queue_send(os_queue_t queue, const void *item, uint32_t timeout_ms);

/**
 * @brief 从队列接收消息（阻塞）
 * @param[in] queue 队列句柄
 * @param[out] item 接收缓冲区指针
 * @param[in] timeout_ms 超时时间（毫秒），0表示不等待
 * @retval RET_OK 成功
 * @retval RET_ERR_TIMEOUT 超时
 * @retval RET_ERR_PARAM 参数非法
 */
ret_code_t os_queue_receive(os_queue_t queue, void *item, uint32_t timeout_ms);

/* ============================================================
 *  事件标志组抽象
 * ============================================================ */

/**
 * @brief 事件标志组句柄（不透明类型）
 */
typedef void *os_event_group_t;

/**
 * @brief 创建事件标志组
 * @return 事件组句柄，失败返回NULL
 */
os_event_group_t os_event_group_create(void);

/**
 * @brief 销毁事件标志组
 * @param[in] event_group 事件组句柄
 */
void os_event_group_destroy(os_event_group_t event_group);

/**
 * @brief 设置事件标志位
 * @param[in] event_group 事件组句柄
 * @param[in] bits 要设置的位掩码
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 */
ret_code_t os_event_group_set(os_event_group_t event_group, uint32_t bits);

/**
 * @brief 等待事件标志位
 * @param[in] event_group 事件组句柄
 * @param[in] bits 要等待的位掩码
 * @param[in] clear_on_exit 退出时是否清除标志
 * @param[in] wait_for_all 是否等待所有位都置位
 * @param[in] timeout_ms 超时时间（毫秒）
 * @return 实际置位的标志位，0表示超时
 */
uint32_t os_event_group_wait(os_event_group_t event_group, uint32_t bits,
                             bool clear_on_exit, bool wait_for_all,
                             uint32_t timeout_ms);

/* ============================================================
 *  任务通知抽象（轻量级事件机制）
 * ============================================================ */

/**
 * @brief 向指定任务发送通知
 * @param[in] task_handle 任务句柄
 * @param[in] value 通知值
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数非法
 */
ret_code_t os_task_notify(os_thread_t task_handle, uint32_t value);

/**
 * @brief 等待任务通知
 * @param[out] value 接收通知值的指针
 * @param[in] timeout_ms 超时时间（毫秒）
 * @retval RET_OK 成功
 * @retval RET_ERR_TIMEOUT 超时
 */
ret_code_t os_task_notify_wait(uint32_t *value, uint32_t timeout_ms);

#endif /* OS_ABSTRACT_H */
