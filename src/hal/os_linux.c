/**
 * @file os_linux.c
 * @brief Linux平台操作系统抽象层实现
 * @details 基于POSIX接口实现OS抽象层，用于模拟器和开发环境
 *          RTOS平台实现预留扩展
 */
#define _DEFAULT_SOURCE
#include "hal/os_abstract.h"
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

/* ============================================================
 *  互斥锁实现
 * ============================================================ */

os_mutex_t os_mutex_create(void)
{
    pthread_mutex_t *mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    if (mutex == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(mutex, NULL) != 0) {
        free(mutex);
        return NULL;
    }
    return (os_mutex_t)mutex;
}

void os_mutex_destroy(os_mutex_t mutex)
{
    if (mutex != NULL) {
        pthread_mutex_destroy((pthread_mutex_t *)mutex);
        free(mutex);
    }
}

ret_code_t os_mutex_lock(os_mutex_t mutex)
{
    if (mutex == NULL) {
        return RET_ERR_PARAM;
    }
    pthread_mutex_lock((pthread_mutex_t *)mutex);
    return RET_OK;
}

ret_code_t os_mutex_unlock(os_mutex_t mutex)
{
    if (mutex == NULL) {
        return RET_ERR_PARAM;
    }
    pthread_mutex_unlock((pthread_mutex_t *)mutex);
    return RET_OK;
}

/* ============================================================
 *  时间实现
 * ============================================================ */

uint64_t os_get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

void os_delay_ms(uint32_t ms)
{
    usleep(ms * 1000U);
}

/* ============================================================
 *  线程实现
 * ============================================================ */

typedef struct {
    pthread_t tid;
    os_thread_func_t func;
    void *arg;
    char name[32];
} linux_thread_t;

static void *linux_thread_entry(void *arg)
{
    linux_thread_t *thread = (linux_thread_t *)arg;
    if (thread->func != NULL) {
        thread->func(thread->arg);
    }
    return NULL;
}

os_thread_t os_thread_create(os_thread_func_t func, void *arg,
                              const char *name, uint32_t priority,
                              uint32_t stack_size)
{
    (void)priority;
    (void)stack_size;

    linux_thread_t *thread = (linux_thread_t *)malloc(sizeof(linux_thread_t));
    if (thread == NULL) {
        return NULL;
    }
    thread->func = func;
    thread->arg = arg;
    if (name != NULL) {
        strncpy(thread->name, name, sizeof(thread->name) - 1);
        thread->name[sizeof(thread->name) - 1] = '\0';
    } else {
        thread->name[0] = '\0';
    }

    if (pthread_create(&thread->tid, NULL, linux_thread_entry, thread) != 0) {
        free(thread);
        return NULL;
    }
    return (os_thread_t)thread;
}

void os_thread_destroy(os_thread_t thread)
{
    if (thread != NULL) {
        linux_thread_t *t = (linux_thread_t *)thread;
        pthread_join(t->tid, NULL);
        free(t);
    }
}

/* ============================================================
 *  平台信息
 * ============================================================ */

os_platform_t os_get_platform(void)
{
    return OS_PLATFORM_LINUX;
}

const char *os_get_platform_name(void)
{
    return "Linux";
}

/* ============================================================
 *  消息队列实现（基于 POSIX 条件变量）
 * ============================================================ */

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    uint8_t        *buffer;       ///< 环形缓冲区
    uint32_t        head;          ///< 读指针
    uint32_t        tail;          ///< 写指针
    uint32_t        count;         ///< 当前消息数
    uint32_t        length;        ///< 队列长度
    uint32_t        item_size;    ///< 每条消息大小
} linux_queue_t;

os_queue_t os_queue_create(uint32_t queue_length, uint32_t item_size)
{
    if (queue_length == 0 || item_size == 0) {
        return NULL;
    }

    linux_queue_t *q = (linux_queue_t *)calloc(1, sizeof(linux_queue_t));
    if (q == NULL) {
        return NULL;
    }

    q->buffer = (uint8_t *)calloc(queue_length, item_size);
    if (q->buffer == NULL) {
        free(q);
        return NULL;
    }

    q->length = queue_length;
    q->item_size = item_size;
    q->head = 0;
    q->tail = 0;
    q->count = 0;

    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);

    return (os_queue_t)q;
}

void os_queue_destroy(os_queue_t queue)
{
    if (queue != NULL) {
        linux_queue_t *q = (linux_queue_t *)queue;
        pthread_mutex_destroy(&q->mutex);
        pthread_cond_destroy(&q->cond);
        free(q->buffer);
        free(q);
    }
}

ret_code_t os_queue_send(os_queue_t queue, const void *item, uint32_t timeout_ms)
{
    if (queue == NULL || item == NULL) {
        return RET_ERR_PARAM;
    }

    linux_queue_t *q = (linux_queue_t *)queue;
    struct timespec ts;

    pthread_mutex_lock(&q->mutex);

    /* 等待队列有空间 */
    while (q->count >= q->length) {
        if (timeout_ms == 0) {
            pthread_mutex_unlock(&q->mutex);
            return RET_ERR_TIMEOUT;
        }
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        int rc = pthread_cond_timedwait(&q->cond, &q->mutex, &ts);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&q->mutex);
            return RET_ERR_TIMEOUT;
        }
    }

    /* 写入消息 */
    memcpy(q->buffer + q->tail * q->item_size, item, q->item_size);
    q->tail = (q->tail + 1) % q->length;
    q->count++;

    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);

    return RET_OK;
}

ret_code_t os_queue_receive(os_queue_t queue, void *item, uint32_t timeout_ms)
{
    if (queue == NULL || item == NULL) {
        return RET_ERR_PARAM;
    }

    linux_queue_t *q = (linux_queue_t *)queue;
    struct timespec ts;

    pthread_mutex_lock(&q->mutex);

    /* 等待队列有数据 */
    while (q->count == 0) {
        if (timeout_ms == 0) {
            pthread_mutex_unlock(&q->mutex);
            return RET_ERR_TIMEOUT;
        }
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        int rc = pthread_cond_timedwait(&q->cond, &q->mutex, &ts);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&q->mutex);
            return RET_ERR_TIMEOUT;
        }
    }

    /* 读取消息 */
    memcpy(item, q->buffer + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->length;
    q->count--;

    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);

    return RET_OK;
}

/* ============================================================
 *  事件标志组实现
 * ============================================================ */

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    uint32_t        bits;
} linux_event_group_t;

os_event_group_t os_event_group_create(void)
{
    linux_event_group_t *eg = (linux_event_group_t *)calloc(1, sizeof(linux_event_group_t));
    if (eg == NULL) {
        return NULL;
    }
    pthread_mutex_init(&eg->mutex, NULL);
    pthread_cond_init(&eg->cond, NULL);
    eg->bits = 0;
    return (os_event_group_t)eg;
}

void os_event_group_destroy(os_event_group_t event_group)
{
    if (event_group != NULL) {
        linux_event_group_t *eg = (linux_event_group_t *)event_group;
        pthread_mutex_destroy(&eg->mutex);
        pthread_cond_destroy(&eg->cond);
        free(eg);
    }
}

ret_code_t os_event_group_set(os_event_group_t event_group, uint32_t bits)
{
    if (event_group == NULL) {
        return RET_ERR_PARAM;
    }
    linux_event_group_t *eg = (linux_event_group_t *)event_group;
    pthread_mutex_lock(&eg->mutex);
    eg->bits |= bits;
    pthread_cond_broadcast(&eg->cond);
    pthread_mutex_unlock(&eg->mutex);
    return RET_OK;
}

uint32_t os_event_group_wait(os_event_group_t event_group, uint32_t bits,
                             bool clear_on_exit, bool wait_for_all,
                             uint32_t timeout_ms)
{
    if (event_group == NULL || bits == 0) {
        return 0;
    }

    linux_event_group_t *eg = (linux_event_group_t *)event_group;
    struct timespec ts;
    uint32_t result = 0;

    pthread_mutex_lock(&eg->mutex);

    while (1) {
        /* 检查条件 */
        uint32_t matched = eg->bits & bits;
        if (wait_for_all) {
            if (matched == bits) {
                result = matched;
                break;
            }
        } else {
            if (matched != 0) {
                result = matched;
                break;
            }
        }

        if (timeout_ms == 0) {
            pthread_mutex_unlock(&eg->mutex);
            return 0;
        }

        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        int rc = pthread_cond_timedwait(&eg->cond, &eg->mutex, &ts);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&eg->mutex);
            return 0;
        }
    }

    if (clear_on_exit) {
        eg->bits &= ~result;
    }

    pthread_mutex_unlock(&eg->mutex);
    return result;
}

/* ============================================================
 *  任务通知实现（简化版：单值通知）
 * ============================================================ */

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    uint32_t        value;
    bool            pending;
} linux_task_notify_t;

static linux_task_notify_t g_main_notify = {0};

ret_code_t os_task_notify(os_thread_t task_handle, uint32_t value)
{
    (void)task_handle;  /* Linux 下只支持主线程通知 */
    pthread_mutex_lock(&g_main_notify.mutex);
    g_main_notify.value = value;
    g_main_notify.pending = true;
    pthread_cond_signal(&g_main_notify.cond);
    pthread_mutex_unlock(&g_main_notify.mutex);
    return RET_OK;
}

ret_code_t os_task_notify_wait(uint32_t *value, uint32_t timeout_ms)
{
    struct timespec ts;

    pthread_mutex_lock(&g_main_notify.mutex);

    while (!g_main_notify.pending) {
        if (timeout_ms == 0) {
            pthread_mutex_unlock(&g_main_notify.mutex);
            return RET_ERR_TIMEOUT;
        }
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        int rc = pthread_cond_timedwait(&g_main_notify.cond, &g_main_notify.mutex, &ts);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&g_main_notify.mutex);
            return RET_ERR_TIMEOUT;
        }
    }

    if (value != NULL) {
        *value = g_main_notify.value;
    }
    g_main_notify.pending = false;

    pthread_mutex_unlock(&g_main_notify.mutex);
    return RET_OK;
}
