/**
 * @file thread.c
 * @brief 线程管理模块实现
 * @details 企业级固件的线程管理模块实现，基于 POSIX 线程库（pthread）。
 *          提供线程创建、销毁、启动、停止、等待、状态查询等功能，
 *          以及互斥锁和条件变量等同步原语。模拟固件中的多任务调度，
 *          实际嵌入式固件中通常使用 RTOS（如 FreeRTOS、ThreadX）。
 */

#define _GNU_SOURCE

#include "thread.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

/* ============================================================
 *  内部数据结构
 * ============================================================ */

/**
 * @brief 最大线程数量
 * @details 静态分配线程表，避免动态内存分配带来的不确定性
 */
#define MAX_THREADS 32

/**
 * @brief 线程私有数据结构体
 * @details 每个线程维护独立的状态、优先级、统计信息和停止标志
 */
typedef struct {
    uint32_t thread_id;              ///< 线程ID（全局唯一，从1开始）
    char name[32];                   ///< 线程名称（用于日志和调试）
    pthread_t pthread;               ///< POSIX 线程句柄
    thread_func_t func;              ///< 线程入口函数
    void *arg;                       ///< 线程参数（入口函数参数）
    void *retval;                    ///< 线程返回值（join时获取）
    thread_state_t state;            ///< 线程状态（READY/RUNNING/STOPPED等）
    thread_priority_t priority;      ///< 线程优先级（低/普通/高/紧急）
    uint64_t start_time_ms;          ///< 启动时间（毫秒）
    uint64_t total_run_time_ms;      ///< 总运行时间（毫秒，累计）
    uint32_t run_count;              ///< 运行次数
    uint32_t error_count;            ///< 错误计数
    bool is_initialized;             ///< 槽位是否已初始化
    bool should_stop;                ///< 停止标志（线程函数应轮询此标志）
} thread_priv_t;

/**
 * @brief 互斥锁私有数据
 * @details 封装 POSIX 互斥锁，添加初始化标志用于有效性检查
 */
typedef struct {
    pthread_mutex_t mutex;           ///< POSIX 互斥锁
    bool is_initialized;             ///< 初始化标志
} mutex_priv_t;

/**
 * @brief 条件变量私有数据
 * @details 封装 POSIX 条件变量，添加初始化标志用于有效性检查
 */
typedef struct {
    pthread_cond_t cond;             ///< POSIX 条件变量
    bool is_initialized;             ///< 初始化标志
} cond_priv_t;

/* ============================================================
 *  全局变量
 * ============================================================ */

/**
 * @brief 线程表（静态数组）
 * @details 所有线程信息存储在静态数组中，通过 is_initialized 标记槽位占用
 */
static thread_priv_t g_threads[MAX_THREADS];

/**
 * @brief 当前线程数量
 */
static uint32_t g_thread_count = 0;

/**
 * @brief 下一个线程ID（自增分配）
 */
static uint32_t g_next_thread_id = 1;

/**
 * @brief 线程管理器初始化标志
 */
static bool g_thread_manager_initialized = false;

/* ============================================================
 *  内部函数
 * ============================================================ */

/**
 * @brief 获取当前时间戳（毫秒）
 * @return 时间戳（毫秒，单调递增）
 * @details 使用 CLOCK_MONOTONIC 时钟，不受系统时间调整影响，
 *          适合计算时间差和运行时长统计。
 */
static uint64_t get_timestamp_ms(void)
{
    struct timespec ts;

    /* 获取单调时钟时间 */
    clock_gettime(CLOCK_MONOTONIC, &ts);

    /* 转换为毫秒：秒*1000 + 纳秒/1000000 */
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/**
 * @brief 查找空闲线程槽位
 * @return 线程槽位索引，失败返回 -1
 * @details 线性查找第一个未初始化的槽位。MAX_THREADS=32，
 *          线性查找性能足够，无需复杂的空闲链表。
 */
static int find_free_slot(void)
{
    uint32_t i = 0;

    /* 遍历线程表，查找第一个空闲槽位 */
    for (i = 0; i < MAX_THREADS; i++) {
        if (!g_threads[i].is_initialized) {
            return (int)i;
        }
    }

    /* 没有空闲槽位 */
    return -1;
}

/**
 * @brief 根据线程ID查找线程
 * @param[in] thread_id 线程ID
 * @return 线程指针，失败返回 NULL
 * @details 线性查找匹配指定ID的线程。线程ID是全局唯一的自增值。
 */
static thread_priv_t *find_thread_by_id(uint32_t thread_id)
{
    uint32_t i = 0;

    /* 遍历线程表，查找匹配ID的线程 */
    for (i = 0; i < MAX_THREADS; i++) {
        if (g_threads[i].is_initialized && g_threads[i].thread_id == thread_id) {
            return &g_threads[i];
        }
    }

    return NULL;
}

/**
 * @brief 线程包装函数
 * @param[in] arg 线程参数（thread_priv_t* 指针）
 * @return 线程返回值
 * @note 所有创建的线程都通过此包装函数启动，用于在线程入口函数
 *       前后添加状态管理、时间统计和日志输出。实际的业务逻辑在
 *       thread->func 中执行。
 */
static void *thread_wrapper(void *arg)
{
    thread_priv_t *thread = (thread_priv_t *)arg;
    void *retval = NULL;

    /* 参数检查 */
    if (thread == NULL) {
        return NULL;
    }

    /* 更新线程状态为运行中，记录启动时间 */
    thread->state = THREAD_STATE_RUNNING;
    thread->start_time_ms = get_timestamp_ms();
    thread->run_count++;

    printf("[线程管理] 线程 %s (ID=%u) 开始运行\n",
           thread->name, thread->thread_id);

    /* 调用实际的线程入口函数（业务逻辑） */
    retval = thread->func(thread->arg);

    /* 线程函数返回后，更新状态和统计信息 */
    thread->retval = retval;
    thread->state = THREAD_STATE_STOPPED;
    thread->total_run_time_ms += get_timestamp_ms() - thread->start_time_ms;

    printf("[线程管理] 线程 %s (ID=%u) 结束运行，运行时间=%llu ms\n",
           thread->name, thread->thread_id,
           (unsigned long long)thread->total_run_time_ms);

    return retval;
}

/* ============================================================
 *  互斥锁接口实现
 * ============================================================ */

/**
 * @brief 创建互斥锁
 * @return 互斥锁句柄，失败返回 NULL
 * @details 分配互斥锁内存并初始化 POSIX 互斥锁。
 *          使用默认属性（快速互斥锁，非递归）。
 *          实际固件中互斥锁通常静态分配，这里用动态分配模拟。
 */
mutex_handle_t mutex_create(void)
{
    mutex_priv_t *mutex = NULL;

    /* 分配互斥锁私有数据内存 */
    mutex = (mutex_priv_t *)malloc(sizeof(mutex_priv_t));
    if (mutex == NULL) {
        return NULL;
    }

    /* 初始化 POSIX 互斥锁（默认属性） */
    if (pthread_mutex_init(&mutex->mutex, NULL) != 0) {
        free(mutex);
        return NULL;
    }

    mutex->is_initialized = true;

    return (mutex_handle_t)mutex;
}

/**
 * @brief 销毁互斥锁
 * @param[in] mutex 互斥锁句柄
 * @retval RET_OK 销毁成功
 * @retval RET_ERR_PARAM 参数错误（空指针或未初始化）
 * @details 销毁 POSIX 互斥锁并释放内存。
 *          调用前应确保没有线程持有该锁。
 */
ret_code_t mutex_destroy(mutex_handle_t mutex)
{
    mutex_priv_t *m = (mutex_priv_t *)mutex;

    /* 参数和初始化状态检查 */
    if (m == NULL || !m->is_initialized) {
        return RET_ERR_PARAM;
    }

    /* 销毁 POSIX 互斥锁 */
    pthread_mutex_destroy(&m->mutex);
    m->is_initialized = false;

    /* 释放内存 */
    free(m);

    return RET_OK;
}

/**
 * @brief 加锁（阻塞）
 * @param[in] mutex 互斥锁句柄
 * @retval RET_OK 加锁成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_INTERNAL 内部错误
 * @details 阻塞等待获取互斥锁。如果锁已被其他线程持有，
 *          当前线程会挂起直到锁可用。
 */
ret_code_t mutex_lock(mutex_handle_t mutex)
{
    mutex_priv_t *m = (mutex_priv_t *)mutex;

    /* 参数和初始化状态检查 */
    if (m == NULL || !m->is_initialized) {
        return RET_ERR_PARAM;
    }

    /* 阻塞加锁 */
    if (pthread_mutex_lock(&m->mutex) != 0) {
        return RET_ERR_INTERNAL;
    }

    return RET_OK;
}

/**
 * @brief 解锁
 * @param[in] mutex 互斥锁句柄
 * @retval RET_OK 解锁成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_INTERNAL 内部错误
 * @details 释放互斥锁。调用者必须是当前持有锁的线程。
 */
ret_code_t mutex_unlock(mutex_handle_t mutex)
{
    mutex_priv_t *m = (mutex_priv_t *)mutex;

    /* 参数和初始化状态检查 */
    if (m == NULL || !m->is_initialized) {
        return RET_ERR_PARAM;
    }

    /* 解锁 */
    if (pthread_mutex_unlock(&m->mutex) != 0) {
        return RET_ERR_INTERNAL;
    }

    return RET_OK;
}

/**
 * @brief 尝试加锁（非阻塞）
 * @param[in] mutex 互斥锁句柄
 * @retval RET_OK 加锁成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_BUSY 锁已被占用
 * @retval RET_ERR_INTERNAL 内部错误
 * @details 非阻塞尝试获取互斥锁。如果锁已被占用，立即返回 RET_ERR_BUSY，
 *          不会阻塞等待。适合不能阻塞的临界场景。
 */
ret_code_t mutex_trylock(mutex_handle_t mutex)
{
    mutex_priv_t *m = (mutex_priv_t *)mutex;
    int ret = 0;

    /* 参数和初始化状态检查 */
    if (m == NULL || !m->is_initialized) {
        return RET_ERR_PARAM;
    }

    /* 非阻塞尝试加锁 */
    ret = pthread_mutex_trylock(&m->mutex);
    if (ret == 0) {
        return RET_OK;
    } else if (ret == EBUSY) {
        /* 锁已被其他线程持有 */
        return RET_ERR_BUSY;
    }

    return RET_ERR_INTERNAL;
}

/* ============================================================
 *  条件变量接口实现
 * ============================================================ */

/**
 * @brief 创建条件变量
 * @return 条件变量句柄，失败返回 NULL
 * @details 分配条件变量内存并初始化 POSIX 条件变量。
 *          条件变量必须与互斥锁配合使用，用于线程间事件通知。
 */
cond_handle_t cond_create(void)
{
    cond_priv_t *cond = NULL;

    /* 分配条件变量私有数据内存 */
    cond = (cond_priv_t *)malloc(sizeof(cond_priv_t));
    if (cond == NULL) {
        return NULL;
    }

    /* 初始化 POSIX 条件变量（默认属性） */
    if (pthread_cond_init(&cond->cond, NULL) != 0) {
        free(cond);
        return NULL;
    }

    cond->is_initialized = true;

    return (cond_handle_t)cond;
}

/**
 * @brief 销毁条件变量
 * @param[in] cond 条件变量句柄
 * @retval RET_OK 销毁成功
 * @retval RET_ERR_PARAM 参数错误
 * @details 销毁 POSIX 条件变量并释放内存。
 *          调用前应确保没有线程在该条件变量上等待。
 */
ret_code_t cond_destroy(cond_handle_t cond)
{
    cond_priv_t *c = (cond_priv_t *)cond;

    /* 参数和初始化状态检查 */
    if (c == NULL || !c->is_initialized) {
        return RET_ERR_PARAM;
    }

    /* 销毁 POSIX 条件变量 */
    pthread_cond_destroy(&c->cond);
    c->is_initialized = false;

    /* 释放内存 */
    free(c);

    return RET_OK;
}

/**
 * @brief 等待条件变量（阻塞）
 * @param[in] cond 条件变量句柄
 * @param[in] mutex 互斥锁句柄（调用前必须已加锁）
 * @retval RET_OK 等待成功（被信号唤醒）
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_INTERNAL 内部错误
 * @details 阻塞等待条件变量信号。调用时会原子性地释放互斥锁并挂起，
 *          被唤醒后重新获取互斥锁。必须在循环中检查条件（防止虚假唤醒）。
 */
ret_code_t cond_wait(cond_handle_t cond, mutex_handle_t mutex)
{
    cond_priv_t *c = (cond_priv_t *)cond;
    mutex_priv_t *m = (mutex_priv_t *)mutex;

    /* 参数和初始化状态检查 */
    if (c == NULL || !c->is_initialized || m == NULL || !m->is_initialized) {
        return RET_ERR_PARAM;
    }

    /* 等待条件变量（原子释放锁并挂起，唤醒后重新加锁） */
    if (pthread_cond_wait(&c->cond, &m->mutex) != 0) {
        return RET_ERR_INTERNAL;
    }

    return RET_OK;
}

/**
 * @brief 超时等待条件变量
 * @param[in] cond 条件变量句柄
 * @param[in] mutex 互斥锁句柄
 * @param[in] timeout_ms 超时时间（毫秒）
 * @retval RET_OK 等待成功（被信号唤醒）
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_TIMEOUT 等待超时
 * @retval RET_ERR_INTERNAL 内部错误
 * @details 带超时的条件变量等待。超时后返回 RET_ERR_TIMEOUT，
 *          此时互斥锁已重新获取。使用 CLOCK_REALTIME 计算绝对超时时间。
 */
ret_code_t cond_timedwait(cond_handle_t cond, mutex_handle_t mutex, uint32_t timeout_ms)
{
    cond_priv_t *c = (cond_priv_t *)cond;
    mutex_priv_t *m = (mutex_priv_t *)mutex;
    struct timespec ts;
    int ret = 0;

    /* 参数和初始化状态检查 */
    if (c == NULL || !c->is_initialized || m == NULL || !m->is_initialized) {
        return RET_ERR_PARAM;
    }

    /* 计算绝对超时时间（当前时间 + 超时） */
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    /* 纳秒溢出处理（超过1秒则进位） */
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    /* 超时等待条件变量 */
    ret = pthread_cond_timedwait(&c->cond, &m->mutex, &ts);
    if (ret == 0) {
        return RET_OK;
    } else if (ret == ETIMEDOUT) {
        /* 超时 */
        return RET_ERR_TIMEOUT;
    }

    return RET_ERR_INTERNAL;
}

/**
 * @brief 发送条件变量信号（唤醒一个等待线程）
 * @param[in] cond 条件变量句柄
 * @retval RET_OK 发送成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_INTERNAL 内部错误
 * @details 唤醒一个在该条件变量上等待的线程。如果没有线程在等待，
 *          信号会丢失（不会累积）。
 */
ret_code_t cond_signal(cond_handle_t cond)
{
    cond_priv_t *c = (cond_priv_t *)cond;

    /* 参数和初始化状态检查 */
    if (c == NULL || !c->is_initialized) {
        return RET_ERR_PARAM;
    }

    /* 唤醒一个等待线程 */
    if (pthread_cond_signal(&c->cond) != 0) {
        return RET_ERR_INTERNAL;
    }

    return RET_OK;
}

/**
 * @brief 广播条件变量信号（唤醒所有等待线程）
 * @param[in] cond 条件变量句柄
 * @retval RET_OK 广播成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_INTERNAL 内部错误
 * @details 唤醒所有在该条件变量上等待的线程。被唤醒的线程会
 *          竞争互斥锁，只有一个能继续执行，其余重新等待。
 */
ret_code_t cond_broadcast(cond_handle_t cond)
{
    cond_priv_t *c = (cond_priv_t *)cond;

    /* 参数和初始化状态检查 */
    if (c == NULL || !c->is_initialized) {
        return RET_ERR_PARAM;
    }

    /* 唤醒所有等待线程 */
    if (pthread_cond_broadcast(&c->cond) != 0) {
        return RET_ERR_INTERNAL;
    }

    return RET_OK;
}

/* ============================================================
 *  线程管理接口实现
 * ============================================================ */

/**
 * @brief 初始化线程管理器
 * @retval RET_OK 初始化成功
 * @details 初始化线程表，重置线程计数和ID分配器。
 *          重复调用是安全的，已初始化时直接返回成功。
 */
ret_code_t thread_manager_init(void)
{
    /* 已初始化则直接返回（幂等性保证） */
    if (g_thread_manager_initialized) {
        return RET_OK;
    }

    /* 初始化线程表（清零） */
    memset(g_threads, 0, sizeof(g_threads));
    g_thread_count = 0;
    g_next_thread_id = 1;

    g_thread_manager_initialized = true;

    printf("[线程管理] 线程管理模块初始化完成\n");

    return RET_OK;
}

/**
 * @brief 反初始化线程管理器
 * @retval RET_OK 反初始化成功
 * @details 停止所有正在运行的线程（设置停止标志并join），
 *          然后重置管理器状态。未初始化时直接返回成功。
 */
ret_code_t thread_manager_deinit(void)
{
    uint32_t i = 0;

    /* 未初始化则直接返回 */
    if (!g_thread_manager_initialized) {
        return RET_OK;
    }

    /* 停止所有正在运行的线程 */
    for (i = 0; i < MAX_THREADS; i++) {
        if (g_threads[i].is_initialized) {
            if (g_threads[i].state == THREAD_STATE_RUNNING) {
                /* 设置停止标志，等待线程自行退出 */
                g_threads[i].should_stop = true;
                pthread_join(g_threads[i].pthread, NULL);
            }
            /* 标记槽位为未初始化 */
            g_threads[i].is_initialized = false;
        }
    }

    g_thread_manager_initialized = false;

    printf("[线程管理] 线程管理模块反初始化完成\n");

    return RET_OK;
}

/**
 * @brief 创建线程（不启动）
 * @param[in] name 线程名称（可为NULL）
 * @param[in] func 线程入口函数
 * @param[in] arg 线程参数（传递给入口函数）
 * @param[in] priority 线程优先级
 * @return 线程ID（>0），失败返回 0
 * @details 创建线程但不立即启动，线程处于 READY 状态。
 *          需要调用 thread_start 来实际启动线程。
 *          这种两阶段创建允许在启动前配置线程属性。
 */
uint32_t thread_create(const char *name, thread_func_t func, void *arg,
                       thread_priority_t priority)
{
    int slot = 0;
    thread_priv_t *thread = NULL;

    /* 初始化检查和入口函数空指针检查 */
    if (!g_thread_manager_initialized || func == NULL) {
        return 0;
    }

    /* 查找空闲槽位 */
    slot = find_free_slot();
    if (slot < 0) {
        printf("[线程管理] 线程数量已达上限\n");
        return 0;
    }

    thread = &g_threads[slot];

    /* 初始化线程数据 */
    memset(thread, 0, sizeof(thread_priv_t));
    thread->thread_id = g_next_thread_id++;
    /* 复制线程名称（安全拷贝，确保以'\0'结尾） */
    if (name != NULL) {
        strncpy(thread->name, name, sizeof(thread->name) - 1);
    }
    thread->func = func;
    thread->arg = arg;
    thread->priority = priority;
    thread->state = THREAD_STATE_READY;
    thread->is_initialized = true;
    thread->should_stop = false;

    g_thread_count++;

    printf("[线程管理] 创建线程 %s (ID=%u), 优先级=%d\n",
           thread->name, thread->thread_id, priority);

    return thread->thread_id;
}

/**
 * @brief 销毁线程
 * @param[in] thread_id 线程ID
 * @retval RET_OK 销毁成功
 * @retval RET_ERR_NOT_INIT 线程管理器未初始化
 * @retval RET_ERR_PARAM 线程ID不存在
 * @details 如果线程正在运行，先设置停止标志并等待其退出，
 *          然后释放线程槽位。
 */
ret_code_t thread_destroy(uint32_t thread_id)
{
    thread_priv_t *thread = NULL;

    /* 初始化检查 */
    if (!g_thread_manager_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 查找线程 */
    thread = find_thread_by_id(thread_id);
    if (thread == NULL) {
        return RET_ERR_PARAM;
    }

    /* 如果线程正在运行，先停止并等待退出 */
    if (thread->state == THREAD_STATE_RUNNING) {
        thread->should_stop = true;
        pthread_join(thread->pthread, NULL);
    }

    /* 释放槽位 */
    thread->is_initialized = false;
    g_thread_count--;

    printf("[线程管理] 销毁线程 %s (ID=%u)\n",
           thread->name, thread->thread_id);

    return RET_OK;
}

/**
 * @brief 启动线程
 * @param[in] thread_id 线程ID
 * @retval RET_OK 启动成功
 * @retval RET_ERR_NOT_INIT 线程管理器未初始化
 * @retval RET_ERR_PARAM 线程ID不存在
 * @retval RET_ERR_INTERNAL 线程状态不允许启动或创建失败
 * @details 启动处于 READY 状态的线程，创建 POSIX 线程并执行
 *          thread_wrapper 包装函数。只有 READY 状态的线程可以启动。
 */
ret_code_t thread_start(uint32_t thread_id)
{
    thread_priv_t *thread = NULL;
    int ret = 0;

    /* 初始化检查 */
    if (!g_thread_manager_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 查找线程 */
    thread = find_thread_by_id(thread_id);
    if (thread == NULL) {
        return RET_ERR_PARAM;
    }

    /* 只有 READY 状态的线程可以启动 */
    if (thread->state != THREAD_STATE_READY) {
        return RET_ERR_INTERNAL;
    }

    /* 创建 POSIX 线程，通过包装函数执行 */
    ret = pthread_create(&thread->pthread, NULL, thread_wrapper, thread);
    if (ret != 0) {
        thread->state = THREAD_STATE_ERROR;
        thread->error_count++;
        return RET_ERR_INTERNAL;
    }

    return RET_OK;
}

/**
 * @brief 停止线程
 * @param[in] thread_id 线程ID
 * @retval RET_OK 停止成功
 * @retval RET_ERR_NOT_INIT 线程管理器未初始化
 * @retval RET_ERR_PARAM 线程ID不存在
 * @details 设置停止标志并等待线程自行退出。线程函数应定期检查
 *          should_stop 标志以实现优雅退出。非运行状态的线程直接返回成功。
 */
ret_code_t thread_stop(uint32_t thread_id)
{
    thread_priv_t *thread = NULL;

    /* 初始化检查 */
    if (!g_thread_manager_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 查找线程 */
    thread = find_thread_by_id(thread_id);
    if (thread == NULL) {
        return RET_ERR_PARAM;
    }

    /* 非运行状态直接返回 */
    if (thread->state != THREAD_STATE_RUNNING) {
        return RET_OK;
    }

    /* 设置停止标志，等待线程自行退出（协作式停止） */
    thread->should_stop = true;
    pthread_join(thread->pthread, NULL);
    thread->state = THREAD_STATE_STOPPED;

    return RET_OK;
}

/**
 * @brief 等待线程结束
 * @param[in] thread_id 线程ID
 * @param[out] retval 线程返回值输出（可为NULL）
 * @retval RET_OK 等待成功
 * @retval RET_ERR_NOT_INIT 线程管理器未初始化
 * @retval RET_ERR_PARAM 线程ID不存在
 * @details 阻塞等待指定线程结束。如果线程已经停止，直接返回返回值。
 *          如果线程还在 READY 状态（未启动），pthread_join 会等待
 *          线程启动并完成。
 */
ret_code_t thread_join(uint32_t thread_id, void **retval)
{
    thread_priv_t *thread = NULL;

    /* 初始化检查 */
    if (!g_thread_manager_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 查找线程 */
    thread = find_thread_by_id(thread_id);
    if (thread == NULL) {
        return RET_ERR_PARAM;
    }

    /* 如果线程已经停止或出错，直接返回保存的返回值 */
    if (thread->state == THREAD_STATE_STOPPED ||
        thread->state == THREAD_STATE_ERROR) {
        if (retval != NULL) {
            *retval = thread->retval;
        }
        return RET_OK;
    }

    /* 线程还在运行或未启动，等待其结束 */
    pthread_join(thread->pthread, retval);
    thread->state = THREAD_STATE_STOPPED;

    return RET_OK;
}

/**
 * @brief 获取线程信息
 * @param[in] thread_id 线程ID
 * @param[out] info 线程信息输出缓冲区
 * @retval RET_OK 获取成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_INIT 线程管理器未初始化
 * @details 获取线程的完整信息，包括ID、名称、状态、优先级、
 *          运行时间、运行次数、错误次数和私有数据。
 */
ret_code_t thread_get_info(uint32_t thread_id, thread_info_t *info)
{
    thread_priv_t *thread = NULL;

    /* 输出缓冲区空指针检查 */
    if (info == NULL) {
        return RET_ERR_PARAM;
    }

    /* 初始化检查 */
    if (!g_thread_manager_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 查找线程 */
    thread = find_thread_by_id(thread_id);
    if (thread == NULL) {
        return RET_ERR_PARAM;
    }

    /* 复制线程信息到输出缓冲区 */
    info->thread_id = thread->thread_id;
    strncpy(info->name, thread->name, sizeof(info->name) - 1);
    info->state = thread->state;
    info->priority = thread->priority;
    info->start_time_ms = thread->start_time_ms;
    info->total_run_time_ms = thread->total_run_time_ms;
    info->run_count = thread->run_count;
    info->error_count = thread->error_count;
    info->private_data = thread->arg;

    return RET_OK;
}

/**
 * @brief 获取线程状态
 * @param[in] thread_id 线程ID
 * @return 线程状态，参数错误时返回 THREAD_STATE_UNINIT
 * @details 快速获取线程状态，不需要完整信息时使用。
 */
thread_state_t thread_get_state(uint32_t thread_id)
{
    thread_priv_t *thread = NULL;

    /* 初始化检查 */
    if (!g_thread_manager_initialized) {
        return THREAD_STATE_UNINIT;
    }

    /* 查找线程 */
    thread = find_thread_by_id(thread_id);
    if (thread == NULL) {
        return THREAD_STATE_UNINIT;
    }

    return thread->state;
}

/**
 * @brief 线程休眠
 * @param[in] ms 休眠时间（毫秒）
 * @details 当前线程休眠指定毫秒数。基于 usleep 实现，
 *          实际休眠时间可能略大于指定值（系统调度延迟）。
 */
void thread_sleep(uint32_t ms)
{
    /* usleep 参数为微秒，毫秒转微秒 */
    usleep(ms * 1000);
}

/**
 * @brief 获取当前线程ID
 * @return 当前线程ID，失败返回0
 * @details 通过 pthread_self 获取当前 POSIX 线程句柄，
 *          然后在线程表中查找匹配的线程ID。
 *          非管理线程（如主线程）返回0。
 */
uint32_t thread_get_current_id(void)
{
    pthread_t self = 0;
    uint32_t i = 0;

    /* 初始化检查 */
    if (!g_thread_manager_initialized) {
        return 0;
    }

    /* 获取当前 POSIX 线程句柄 */
    self = pthread_self();

    /* 在线程表中查找匹配的线程 */
    for (i = 0; i < MAX_THREADS; i++) {
        if (g_threads[i].is_initialized &&
            pthread_equal(g_threads[i].pthread, self)) {
            return g_threads[i].thread_id;
        }
    }

    /* 当前线程不是由线程管理器创建的 */
    return 0;
}

/**
 * @brief 打印线程管理器状态
 * @details 以可读格式打印所有线程的状态信息，包括ID、名称、
 *          状态、优先级和运行时间。用于调试和监控。
 */
void thread_manager_print_status(void)
{
    /* 状态字符串表（索引对应 thread_state_t 枚举值） */
    const char *state_str[] = {"未初始化", "就绪", "运行中", "挂起", "已停止", "错误"};
    /* 优先级字符串表（索引对应 thread_priority_t 枚举值） */
    const char *priority_str[] = {"低", "普通", "高", "紧急"};
    uint32_t i = 0;

    /* 初始化检查 */
    if (!g_thread_manager_initialized) {
        printf("线程管理模块未初始化\n");
        return;
    }

    /* 打印线程管理器整体状态 */
    printf("线程管理状态:\n");
    printf("  总线程数: %u\n", g_thread_count);
    printf("  %-6s %-16s %-8s %-8s %-12s\n",
           "ID", "名称", "状态", "优先级", "运行时间(ms)");

    /* 打印每个已初始化线程的信息 */
    for (i = 0; i < MAX_THREADS; i++) {
        if (g_threads[i].is_initialized) {
            printf("  %-6u %-16s %-8s %-8s %-12llu\n",
                   g_threads[i].thread_id,
                   g_threads[i].name,
                   state_str[g_threads[i].state],
                   priority_str[g_threads[i].priority],
                   (unsigned long long)g_threads[i].total_run_time_ms);
        }
    }
}
