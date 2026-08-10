/**
 * @file thread.h
 * @brief 线程管理模块
 * @details 企业级固件的线程管理模块，提供线程创建、销毁、同步等功能
 *          基于 POSIX 线程库（pthread）实现，模拟固件中的多任务调度
 */

#ifndef FIRMWARE_THREAD_H
#define FIRMWARE_THREAD_H

#include "common/common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  线程状态定义
 * ============================================================ */

/**
 * @brief 线程状态枚举
 */
typedef enum {
    THREAD_STATE_UNINIT = 0,    ///< 未初始化
    THREAD_STATE_READY = 1,     ///< 就绪
    THREAD_STATE_RUNNING = 2,   ///< 运行中
    THREAD_STATE_SUSPENDED = 3, ///< 挂起
    THREAD_STATE_STOPPED = 4,   ///< 已停止
    THREAD_STATE_ERROR = 5,     ///< 错误状态
    THREAD_STATE_MAX = 6        ///< 状态最大值
} thread_state_t;

/**
 * @brief 线程优先级枚举
 */
typedef enum {
    THREAD_PRIORITY_LOW = 0,    ///< 低优先级
    THREAD_PRIORITY_NORMAL = 1, ///< 普通优先级
    THREAD_PRIORITY_HIGH = 2,   ///< 高优先级
    THREAD_PRIORITY_URGENT = 3, ///< 紧急优先级
    THREAD_PRIORITY_MAX = 4     ///< 优先级最大值
} thread_priority_t;

/**
 * @brief 线程入口函数类型
 * @param[in] arg 线程参数
 * @return 线程返回值
 */
typedef void *(*thread_func_t)(void *arg);

/* ============================================================
 *  线程信息结构体
 * ============================================================ */

/**
 * @brief 线程信息结构体
 */
typedef struct {
    uint32_t thread_id;              ///< 线程ID
    char name[32];                   ///< 线程名称
    thread_state_t state;            ///< 线程状态
    thread_priority_t priority;      ///< 线程优先级
    uint64_t start_time_ms;          ///< 启动时间（毫秒）
    uint64_t total_run_time_ms;      ///< 总运行时间（毫秒）
    uint32_t run_count;              ///< 运行次数
    uint32_t error_count;            ///< 错误计数
    void *private_data;              ///< 私有数据指针
} thread_info_t;

/* ============================================================
 *  互斥锁接口
 * ============================================================ */

/**
 * @brief 互斥锁句柄
 */
typedef void *mutex_handle_t;

/**
 * @brief 创建互斥锁
 * @return 互斥锁句柄，失败返回 NULL
 */
mutex_handle_t mutex_create(void);

/**
 * @brief 销毁互斥锁
 * @param[in] mutex 互斥锁句柄
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t mutex_destroy(mutex_handle_t mutex);

/**
 * @brief 加锁
 * @param[in] mutex 互斥锁句柄
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t mutex_lock(mutex_handle_t mutex);

/**
 * @brief 解锁
 * @param[in] mutex 互斥锁句柄
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t mutex_unlock(mutex_handle_t mutex);

/**
 * @brief 尝试加锁（非阻塞）
 * @param[in] mutex 互斥锁句柄
 * @retval RET_OK 成功
 * @retval RET_ERR_BUSY 锁已被占用
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t mutex_trylock(mutex_handle_t mutex);

/* ============================================================
 *  条件变量接口
 * ============================================================ */

/**
 * @brief 条件变量句柄
 */
typedef void *cond_handle_t;

/**
 * @brief 创建条件变量
 * @return 条件变量句柄，失败返回 NULL
 */
cond_handle_t cond_create(void);

/**
 * @brief 销毁条件变量
 * @param[in] cond 条件变量句柄
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t cond_destroy(cond_handle_t cond);

/**
 * @brief 等待条件变量
 * @param[in] cond 条件变量句柄
 * @param[in] mutex 互斥锁句柄
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t cond_wait(cond_handle_t cond, mutex_handle_t mutex);

/**
 * @brief 超时等待条件变量
 * @param[in] cond 条件变量句柄
 * @param[in] mutex 互斥锁句柄
 * @param[in] timeout_ms 超时时间（毫秒）
 * @retval RET_OK 成功
 * @retval RET_ERR_TIMEOUT 超时
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t cond_timedwait(cond_handle_t cond, mutex_handle_t mutex, uint32_t timeout_ms);

/**
 * @brief 唤醒一个等待线程
 * @param[in] cond 条件变量句柄
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t cond_signal(cond_handle_t cond);

/**
 * @brief 唤醒所有等待线程
 * @param[in] cond 条件变量句柄
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t cond_broadcast(cond_handle_t cond);

/* ============================================================
 *  线程管理接口
 * ============================================================ */

/**
 * @brief 初始化线程管理模块
 * @retval RET_OK 成功
 * @retval RET_ERR_INTERNAL 内部错误
 */
ret_code_t thread_manager_init(void);

/**
 * @brief 反初始化线程管理模块
 * @retval RET_OK 成功
 */
ret_code_t thread_manager_deinit(void);

/**
 * @brief 创建线程
 * @param[in] name 线程名称
 * @param[in] func 线程入口函数
 * @param[in] arg 线程参数
 * @param[in] priority 线程优先级
 * @return 线程ID，失败返回 0
 */
uint32_t thread_create(const char *name, thread_func_t func, void *arg,
                       thread_priority_t priority);

/**
 * @brief 销毁线程
 * @param[in] thread_id 线程ID
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_FOUND 线程不存在
 */
ret_code_t thread_destroy(uint32_t thread_id);

/**
 * @brief 启动线程
 * @param[in] thread_id 线程ID
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_FOUND 线程不存在
 */
ret_code_t thread_start(uint32_t thread_id);

/**
 * @brief 停止线程
 * @param[in] thread_id 线程ID
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_FOUND 线程不存在
 */
ret_code_t thread_stop(uint32_t thread_id);

/**
 * @brief 等待线程结束
 * @param[in] thread_id 线程ID
 * @param[out] retval 线程返回值指针（可为 NULL）
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_FOUND 线程不存在
 */
ret_code_t thread_join(uint32_t thread_id, void **retval);

/**
 * @brief 获取线程信息
 * @param[in] thread_id 线程ID
 * @param[out] info 线程信息指针
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_FOUND 线程不存在
 */
ret_code_t thread_get_info(uint32_t thread_id, thread_info_t *info);

/**
 * @brief 获取线程状态
 * @param[in] thread_id 线程ID
 * @return 线程状态，线程不存在返回 THREAD_STATE_UNINIT
 */
thread_state_t thread_get_state(uint32_t thread_id);

/**
 * @brief 线程休眠
 * @param[in] ms 休眠时间（毫秒）
 */
void thread_sleep(uint32_t ms);

/**
 * @brief 获取当前线程ID
 * @return 当前线程ID
 */
uint32_t thread_get_current_id(void);

/**
 * @brief 打印所有线程状态
 */
void thread_manager_print_status(void);

#ifdef __cplusplus
}
#endif

#endif /* FIRMWARE_THREAD_H */
