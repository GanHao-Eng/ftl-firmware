/**
 * @file os_linux.c
 * @brief Linux平台操作系统抽象层实现
 * @details 基于POSIX接口实现OS抽象层，用于模拟器和开发环境
 *          RTOS平台实现预留扩展（os_freertos.c等）
 */
#include "hal/os_abstract.h"
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

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
