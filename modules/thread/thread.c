/**
 * @file thread.c
 * @brief 线程管理模块实现
 * @details 企业级固件的线程管理模块实现，基于 POSIX 线程库（pthread）
 *          提供线程创建、销毁、同步等功能，模拟固件中的多任务调度
 */

#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE

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
 */
#define MAX_THREADS 32

/**
 * @brief 线程私有数据结构体
 */
typedef struct {
    uint32_t thread_id;              ///< 线程ID
    char name[32];                   ///< 线程名称
    pthread_t pthread;               ///< POSIX 线程句柄
    thread_func_t func;              ///< 线程入口函数
    void *arg;                       ///< 线程参数
    void *retval;                    ///< 线程返回值
    thread_state_t state;            ///< 线程状态
    thread_priority_t priority;      ///< 线程优先级
    uint64_t start_time_ms;          ///< 启动时间（毫秒）
    uint64_t total_run_time_ms;      ///< 总运行时间（毫秒）
    uint32_t run_count;              ///< 运行次数
    uint32_t error_count;            ///< 错误计数
    bool is_initialized;             ///< 初始化标志
    bool should_stop;                ///< 停止标志
} thread_priv_t;

/**
 * @brief 互斥锁私有数据
 */
typedef struct {
    pthread_mutex_t mutex;           ///< POSIX 互斥锁
    bool is_initialized;             ///< 初始化标志
} mutex_priv_t;

/**
 * @brief 条件变量私有数据
 */
typedef struct {
    pthread_cond_t cond;             ///< POSIX 条件变量
    bool is_initialized;             ///< 初始化标志
} cond_priv_t;

/* ============================================================
 *  全局变量
 * ============================================================ */

static thread_priv_t g_threads[MAX_THREADS];  ///< 线程表
static uint32_t g_thread_count = 0;           ///< 线程数量
static uint32_t g_next_thread_id = 1;         ///< 下一个线程ID
static bool g_thread_manager_initialized = false; ///< 初始化标志

/* ============================================================
 *  内部函数
 * ============================================================ */

/**
 * @brief 获取当前时间戳（毫秒）
 * @return 时间戳
 */
static uint64_t get_timestamp_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/**
 * @brief 查找空闲线程槽位
 * @return 线程槽位索引，失败返回 -1
 */
static int find_free_slot(void)
{
    uint32_t i = 0;

    for (i = 0; i < MAX_THREADS; i++) {
        if (!g_threads[i].is_initialized) {
            return (int)i;
        }
    }

    return -1;
}

/**
 * @brief 根据线程ID查找线程
 * @param[in] thread_id 线程ID
 * @return 线程指针，失败返回 NULL
 */
static thread_priv_t *find_thread_by_id(uint32_t thread_id)
{
    uint32_t i = 0;

    for (i = 0; i < MAX_THREADS; i++) {
        if (g_threads[i].is_initialized && g_threads[i].thread_id == thread_id) {
            return &g_threads[i];
        }
    }

    return NULL;
}

/**
 * @brief 线程包装函数
 * @param[in] arg 线程参数
 * @return 线程返回值
 * @note 用于在线程入口函数前后添加状态管理和统计
 */
static void *thread_wrapper(void *arg)
{
    thread_priv_t *thread = (thread_priv_t *)arg;
    void *retval = NULL;

    if (thread == NULL) {
        return NULL;
    }

    /* 更新线程状态 */
    thread->state = THREAD_STATE_RUNNING;
    thread->start_time_ms = get_timestamp_ms();
    thread->run_count++;

    printf("[线程管理] 线程 %s (ID=%u) 开始运行\n",
           thread->name, thread->thread_id);

    /* 调用实际的线程入口函数 */
    retval = thread->func(thread->arg);

    /* 更新线程状态 */
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

mutex_handle_t mutex_create(void)
{
    mutex_priv_t *mutex = NULL;

    /* 分配互斥锁内存 */
    mutex = (mutex_priv_t *)malloc(sizeof(mutex_priv_t));
    if (mutex == NULL) {
        return NULL;
    }

    /* 初始化 POSIX 互斥锁 */
    if (pthread_mutex_init(&mutex->mutex, NULL) != 0) {
        free(mutex);
        return NULL;
    }

    mutex->is_initialized = true;

    return (mutex_handle_t)mutex;
}

ret_code_t mutex_destroy(mutex_handle_t mutex)
{
    mutex_priv_t *m = (mutex_priv_t *)mutex;

    if (m == NULL || !m->is_initialized) {
        return RET_ERR_PARAM;
    }

    /* 销毁 POSIX 互斥锁 */
    pthread_mutex_destroy(&m->mutex);
    m->is_initialized = false;

    free(m);

    return RET_OK;
}

ret_code_t mutex_lock(mutex_handle_t mutex)
{
    mutex_priv_t *m = (mutex_priv_t *)mutex;

    if (m == NULL || !m->is_initialized) {
        return RET_ERR_PARAM;
    }

    if (pthread_mutex_lock(&m->mutex) != 0) {
        return RET_ERR_INTERNAL;
    }

    return RET_OK;
}

ret_code_t mutex_unlock(mutex_handle_t mutex)
{
    mutex_priv_t *m = (mutex_priv_t *)mutex;

    if (m == NULL || !m->is_initialized) {
        return RET_ERR_PARAM;
    }

    if (pthread_mutex_unlock(&m->mutex) != 0) {
        return RET_ERR_INTERNAL;
    }

    return RET_OK;
}

ret_code_t mutex_trylock(mutex_handle_t mutex)
{
    mutex_priv_t *m = (mutex_priv_t *)mutex;
    int ret = 0;

    if (m == NULL || !m->is_initialized) {
        return RET_ERR_PARAM;
    }

    ret = pthread_mutex_trylock(&m->mutex);
    if (ret == 0) {
        return RET_OK;
    } else if (ret == EBUSY) {
        return RET_ERR_BUSY;
    }

    return RET_ERR_INTERNAL;
}

/* ============================================================
 *  条件变量接口实现
 * ============================================================ */

cond_handle_t cond_create(void)
{
    cond_priv_t *cond = NULL;

    /* 分配条件变量内存 */
    cond = (cond_priv_t *)malloc(sizeof(cond_priv_t));
    if (cond == NULL) {
        return NULL;
    }

    /* 初始化 POSIX 条件变量 */
    if (pthread_cond_init(&cond->cond, NULL) != 0) {
        free(cond);
        return NULL;
    }

    cond->is_initialized = true;

    return (cond_handle_t)cond;
}

ret_code_t cond_destroy(cond_handle_t cond)
{
    cond_priv_t *c = (cond_priv_t *)cond;

    if (c == NULL || !c->is_initialized) {
        return RET_ERR_PARAM;
    }

    /* 销毁 POSIX 条件变量 */
    pthread_cond_destroy(&c->cond);
    c->is_initialized = false;

    free(c);

    return RET_OK;
}

ret_code_t cond_wait(cond_handle_t cond, mutex_handle_t mutex)
{
    cond_priv_t *c = (cond_priv_t *)cond;
    mutex_priv_t *m = (mutex_priv_t *)mutex;

    if (c == NULL || !c->is_initialized || m == NULL || !m->is_initialized) {
        return RET_ERR_PARAM;
    }

    if (pthread_cond_wait(&c->cond, &m->mutex) != 0) {
        return RET_ERR_INTERNAL;
    }

    return RET_OK;
}

ret_code_t cond_timedwait(cond_handle_t cond, mutex_handle_t mutex, uint32_t timeout_ms)
{
    cond_priv_t *c = (cond_priv_t *)cond;
    mutex_priv_t *m = (mutex_priv_t *)mutex;
    struct timespec ts;
    int ret = 0;

    if (c == NULL || !c->is_initialized || m == NULL || !m->is_initialized) {
        return RET_ERR_PARAM;
    }

    /* 计算超时时间 */
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    ret = pthread_cond_timedwait(&c->cond, &m->mutex, &ts);
    if (ret == 0) {
        return RET_OK;
    } else if (ret == ETIMEDOUT) {
        return RET_ERR_TIMEOUT;
    }

    return RET_ERR_INTERNAL;
}

ret_code_t cond_signal(cond_handle_t cond)
{
    cond_priv_t *c = (cond_priv_t *)cond;

    if (c == NULL || !c->is_initialized) {
        return RET_ERR_PARAM;
    }

    if (pthread_cond_signal(&c->cond) != 0) {
        return RET_ERR_INTERNAL;
    }

    return RET_OK;
}

ret_code_t cond_broadcast(cond_handle_t cond)
{
    cond_priv_t *c = (cond_priv_t *)cond;

    if (c == NULL || !c->is_initialized) {
        return RET_ERR_PARAM;
    }

    if (pthread_cond_broadcast(&c->cond) != 0) {
        return RET_ERR_INTERNAL;
    }

    return RET_OK;
}

/* ============================================================
 *  线程管理接口实现
 * ============================================================ */

ret_code_t thread_manager_init(void)
{
    if (g_thread_manager_initialized) {
        return RET_OK;
    }

    /* 初始化线程表 */
    memset(g_threads, 0, sizeof(g_threads));
    g_thread_count = 0;
    g_next_thread_id = 1;

    g_thread_manager_initialized = true;

    printf("[线程管理] 线程管理模块初始化完成\n");

    return RET_OK;
}

ret_code_t thread_manager_deinit(void)
{
    uint32_t i = 0;

    if (!g_thread_manager_initialized) {
        return RET_OK;
    }

    /* 停止所有线程 */
    for (i = 0; i < MAX_THREADS; i++) {
        if (g_threads[i].is_initialized) {
            if (g_threads[i].state == THREAD_STATE_RUNNING) {
                g_threads[i].should_stop = true;
                pthread_join(g_threads[i].pthread, NULL);
            }
            g_threads[i].is_initialized = false;
        }
    }

    g_thread_manager_initialized = false;

    printf("[线程管理] 线程管理模块反初始化完成\n");

    return RET_OK;
}

uint32_t thread_create(const char *name, thread_func_t func, void *arg,
                       thread_priority_t priority)
{
    int slot = 0;
    thread_priv_t *thread = NULL;

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

ret_code_t thread_destroy(uint32_t thread_id)
{
    thread_priv_t *thread = NULL;

    if (!g_thread_manager_initialized) {
        return RET_ERR_NOT_INIT;
    }

    thread = find_thread_by_id(thread_id);
    if (thread == NULL) {
        return RET_ERR_PARAM;
    }

    /* 如果线程正在运行，先停止 */
    if (thread->state == THREAD_STATE_RUNNING) {
        thread->should_stop = true;
        pthread_join(thread->pthread, NULL);
    }

    thread->is_initialized = false;
    g_thread_count--;

    printf("[线程管理] 销毁线程 %s (ID=%u)\n",
           thread->name, thread->thread_id);

    return RET_OK;
}

ret_code_t thread_start(uint32_t thread_id)
{
    thread_priv_t *thread = NULL;
    int ret = 0;

    if (!g_thread_manager_initialized) {
        return RET_ERR_NOT_INIT;
    }

    thread = find_thread_by_id(thread_id);
    if (thread == NULL) {
        return RET_ERR_PARAM;
    }

    if (thread->state != THREAD_STATE_READY) {
        return RET_ERR_INTERNAL;
    }

    /* 创建 POSIX 线程 */
    ret = pthread_create(&thread->pthread, NULL, thread_wrapper, thread);
    if (ret != 0) {
        thread->state = THREAD_STATE_ERROR;
        thread->error_count++;
        return RET_ERR_INTERNAL;
    }

    return RET_OK;
}

ret_code_t thread_stop(uint32_t thread_id)
{
    thread_priv_t *thread = NULL;

    if (!g_thread_manager_initialized) {
        return RET_ERR_NOT_INIT;
    }

    thread = find_thread_by_id(thread_id);
    if (thread == NULL) {
        return RET_ERR_PARAM;
    }

    if (thread->state != THREAD_STATE_RUNNING) {
        return RET_OK;
    }

    /* 设置停止标志，等待线程自行退出 */
    thread->should_stop = true;
    pthread_join(thread->pthread, NULL);
    thread->state = THREAD_STATE_STOPPED;

    return RET_OK;
}

ret_code_t thread_join(uint32_t thread_id, void **retval)
{
    thread_priv_t *thread = NULL;

    if (!g_thread_manager_initialized) {
        return RET_ERR_NOT_INIT;
    }

    thread = find_thread_by_id(thread_id);
    if (thread == NULL) {
        return RET_ERR_PARAM;
    }

    if (thread->state != THREAD_STATE_RUNNING) {
        if (retval != NULL) {
            *retval = thread->retval;
        }
        return RET_OK;
    }

    /* 等待线程结束 */
    pthread_join(thread->pthread, retval);
    thread->state = THREAD_STATE_STOPPED;

    return RET_OK;
}

ret_code_t thread_get_info(uint32_t thread_id, thread_info_t *info)
{
    thread_priv_t *thread = NULL;

    if (info == NULL) {
        return RET_ERR_PARAM;
    }
    if (!g_thread_manager_initialized) {
        return RET_ERR_NOT_INIT;
    }

    thread = find_thread_by_id(thread_id);
    if (thread == NULL) {
        return RET_ERR_PARAM;
    }

    /* 复制线程信息 */
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

thread_state_t thread_get_state(uint32_t thread_id)
{
    thread_priv_t *thread = NULL;

    if (!g_thread_manager_initialized) {
        return THREAD_STATE_UNINIT;
    }

    thread = find_thread_by_id(thread_id);
    if (thread == NULL) {
        return THREAD_STATE_UNINIT;
    }

    return thread->state;
}

void thread_sleep(uint32_t ms)
{
    usleep(ms * 1000);
}

uint32_t thread_get_current_id(void)
{
    pthread_t self = 0;
    uint32_t i = 0;

    if (!g_thread_manager_initialized) {
        return 0;
    }

    self = pthread_self();

    /* 查找当前线程 */
    for (i = 0; i < MAX_THREADS; i++) {
        if (g_threads[i].is_initialized &&
            pthread_equal(g_threads[i].pthread, self)) {
            return g_threads[i].thread_id;
        }
    }

    return 0;
}

void thread_manager_print_status(void)
{
    const char *state_str[] = {"未初始化", "就绪", "运行中", "挂起", "已停止", "错误"};
    const char *priority_str[] = {"低", "普通", "高", "紧急"};
    uint32_t i = 0;

    if (!g_thread_manager_initialized) {
        printf("线程管理模块未初始化\n");
        return;
    }

    printf("线程管理状态:\n");
    printf("  总线程数: %u\n", g_thread_count);
    printf("  %-6s %-16s %-8s %-8s %-12s\n",
           "ID", "名称", "状态", "优先级", "运行时间(ms)");

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
