/**
 * @file dma.c
 * @brief DMA 模块实现
 * @details 企业级固件的 DMA（直接内存访问）模块实现
 *          使用线程模拟硬件 DMA 控制器，提供异步数据传输功能
 *          实际固件中 DMA 由硬件完成，这里用软件模拟以便测试
 */

#define _GNU_SOURCE

#include "dma.h"
#include "thread.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

/* ============================================================
 *  内部数据结构
 * ============================================================ */

/**
 * @brief DMA 通道私有数据
 */
typedef struct {
    uint32_t channel_id;        ///< 通道ID
    dma_ch_state_t state;       ///< 通道状态
    dma_transfer_desc_t desc;   ///< 当前传输描述符
    uint32_t transferred;       ///< 已传输字节数
    uint64_t start_time_ms;     ///< 开始时间（毫秒）
    uint64_t complete_time_ms;  ///< 完成时间（毫秒）
    uint32_t transfer_count;    ///< 传输次数
    uint32_t error_count;       ///< 错误计数
    dma_callback_t callback;    ///< 完成回调
    void *user_data;            ///< 用户数据
    bool is_allocated;          ///< 是否已分配
    uint32_t thread_id;         ///< 传输线程ID
    mutex_handle_t mutex;       ///< 通道互斥锁
    cond_handle_t cond;         ///< 完成条件变量
} dma_channel_priv_t;

/* ============================================================
 *  全局变量
 * ============================================================ */

static dma_channel_priv_t g_channels[DMA_MAX_CHANNELS];  ///< DMA 通道表
static bool g_dma_initialized = false;                   ///< 初始化标志

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
 * @brief DMA 传输线程函数
 * @param[in] arg 线程参数（DMA 通道指针）
 * @return 线程返回值
 * @note 模拟硬件 DMA 传输，实际硬件中由 DMA 控制器完成
 *       这里使用 memcpy 模拟数据传输，延时模拟传输时间
 */
static void *dma_transfer_thread(void *arg)
{
    dma_channel_priv_t *ch = (dma_channel_priv_t *)arg;
    uint8_t *src = NULL;
    uint8_t *dst = NULL;
    uint32_t remaining = 0;
    uint32_t chunk_size = 0;
    uint32_t transfer_delay_us = 0;

    if (ch == NULL) {
        return NULL;
    }

    /* 加锁保护通道状态 */
    mutex_lock(ch->mutex);

    /* 检查通道状态 */
    if (ch->state != DMA_CH_STATE_READY) {
        mutex_unlock(ch->mutex);
        return NULL;
    }

    /* 更新通道状态 */
    ch->state = DMA_CH_STATE_RUNNING;
    ch->start_time_ms = get_timestamp_ms();
    ch->transferred = 0;

    /* 获取源地址和目标地址 */
    src = (uint8_t *)ch->desc.src_addr;
    dst = (uint8_t *)ch->desc.dst_addr;
    remaining = ch->desc.length;

    printf("[DMA] 通道 %u 开始传输: 源=%p, 目标=%p, 长度=%u\n",
           ch->channel_id, ch->desc.src_addr, ch->desc.dst_addr, ch->desc.length);

    mutex_unlock(ch->mutex);

    /* 模拟 DMA 传输，分块传输以便支持暂停 */
    chunk_size = 4096;  /* 每次传输 4KB */
    transfer_delay_us = 10;  /* 每块传输延时 10us，模拟硬件传输速度 */

    while (remaining > 0) {
        uint32_t to_transfer = 0;

        /* 检查是否需要暂停 */
        mutex_lock(ch->mutex);
        if (ch->state == DMA_CH_STATE_PAUSED) {
            mutex_unlock(ch->mutex);
            thread_sleep(1);  /* 等待恢复 */
            continue;
        }
        if (ch->state != DMA_CH_STATE_RUNNING) {
            mutex_unlock(ch->mutex);
            break;
        }
        mutex_unlock(ch->mutex);

        /* 计算本次传输大小 */
        to_transfer = (remaining < chunk_size) ? remaining : chunk_size;

        /* 执行数据传输（模拟硬件 DMA） */
        if (src != NULL && dst != NULL) {
            memcpy(dst, src, to_transfer);
        }

        /* 更新传输进度 */
        mutex_lock(ch->mutex);
        ch->transferred += to_transfer;
        mutex_unlock(ch->mutex);

        /* 更新地址指针（如果启用了地址自增） */
        if (ch->desc.src_increment) {
            src += to_transfer;
        }
        if (ch->desc.dst_increment) {
            dst += to_transfer;
        }

        remaining -= to_transfer;

        /* 模拟传输延时 */
        usleep(transfer_delay_us);
    }

    /* 传输完成 */
    mutex_lock(ch->mutex);

    if (ch->state == DMA_CH_STATE_RUNNING) {
        ch->state = DMA_CH_STATE_COMPLETE;
        ch->complete_time_ms = get_timestamp_ms();
        ch->transfer_count++;

        printf("[DMA] 通道 %u 传输完成: 已传输=%u 字节, 耗时=%llu ms\n",
               ch->channel_id, ch->transferred,
               (unsigned long long)(ch->complete_time_ms - ch->start_time_ms));
    } else {
        ch->error_count++;
        printf("[DMA] 通道 %u 传输中止: 已传输=%u 字节\n",
               ch->channel_id, ch->transferred);
    }

    /* 调用完成回调 */
    if (ch->callback != NULL && ch->desc.interrupt_enable) {
        bool success = (ch->state == DMA_CH_STATE_COMPLETE);
        dma_callback_t cb = ch->callback;
        void *user_data = ch->user_data;
        uint32_t transferred = ch->transferred;
        uint32_t channel_id = ch->channel_id;

        mutex_unlock(ch->mutex);
        cb(channel_id, success, transferred, user_data);
        mutex_lock(ch->mutex);
    }

    /* 唤醒等待线程 */
    cond_broadcast(ch->cond);

    mutex_unlock(ch->mutex);

    return NULL;
}

/* ============================================================
 *  DMA 控制器接口实现
 * ============================================================ */

ret_code_t dma_init(void)
{
    uint32_t i = 0;

    if (g_dma_initialized) {
        return RET_OK;
    }

    /* 初始化线程管理模块（如果尚未初始化） */
    thread_manager_init();

    /* 初始化通道表 */
    memset(g_channels, 0, sizeof(g_channels));
    for (i = 0; i < DMA_MAX_CHANNELS; i++) {
        g_channels[i].channel_id = i;
        g_channels[i].state = DMA_CH_STATE_IDLE;
        g_channels[i].is_allocated = false;
        g_channels[i].mutex = mutex_create();
        g_channels[i].cond = cond_create();
    }

    g_dma_initialized = true;

    printf("[DMA] DMA 控制器初始化完成，通道数=%d\n", DMA_MAX_CHANNELS);

    return RET_OK;
}

ret_code_t dma_deinit(void)
{
    uint32_t i = 0;

    if (!g_dma_initialized) {
        return RET_OK;
    }

    /* 停止所有正在传输的通道 */
    for (i = 0; i < DMA_MAX_CHANNELS; i++) {
        if (g_channels[i].is_allocated) {
            if (g_channels[i].state == DMA_CH_STATE_RUNNING ||
                g_channels[i].state == DMA_CH_STATE_PAUSED) {
                dma_stop_transfer(i);
            }
            mutex_destroy(g_channels[i].mutex);
            cond_destroy(g_channels[i].cond);
        }
    }

    g_dma_initialized = false;

    printf("[DMA] DMA 控制器反初始化完成\n");

    return RET_OK;
}

uint32_t dma_alloc_channel(void)
{
    uint32_t i = 0;

    if (!g_dma_initialized) {
        return 0xFFFFFFFF;
    }

    /* 查找空闲通道 */
    for (i = 0; i < DMA_MAX_CHANNELS; i++) {
        if (!g_channels[i].is_allocated) {
            g_channels[i].is_allocated = true;
            g_channels[i].state = DMA_CH_STATE_IDLE;
            g_channels[i].transferred = 0;

            printf("[DMA] 分配通道 %u\n", i);

            return i;
        }
    }

    printf("[DMA] 没有空闲通道\n");
    return 0xFFFFFFFF;
}

ret_code_t dma_free_channel(uint32_t channel)
{
    if (!g_dma_initialized) {
        return RET_ERR_NOT_INIT;
    }
    if (channel >= DMA_MAX_CHANNELS) {
        return RET_ERR_PARAM;
    }
    if (!g_channels[channel].is_allocated) {
        return RET_ERR_PARAM;
    }

    /* 如果正在传输，先停止 */
    if (g_channels[channel].state == DMA_CH_STATE_RUNNING ||
        g_channels[channel].state == DMA_CH_STATE_PAUSED) {
        dma_stop_transfer(channel);
    }

    g_channels[channel].is_allocated = false;
    g_channels[channel].state = DMA_CH_STATE_IDLE;

    printf("[DMA] 释放通道 %u\n", channel);

    return RET_OK;
}

ret_code_t dma_config_channel(uint32_t channel, const dma_transfer_desc_t *desc)
{
    if (!g_dma_initialized) {
        return RET_ERR_NOT_INIT;
    }
    if (channel >= DMA_MAX_CHANNELS || desc == NULL) {
        return RET_ERR_PARAM;
    }
    if (!g_channels[channel].is_allocated) {
        return RET_ERR_PARAM;
    }
    if (g_channels[channel].state == DMA_CH_STATE_RUNNING) {
        return RET_ERR_BUSY;
    }

    /* 复制传输描述符 */
    memcpy(&g_channels[channel].desc, desc, sizeof(dma_transfer_desc_t));
    g_channels[channel].state = DMA_CH_STATE_READY;
    g_channels[channel].transferred = 0;

    printf("[DMA] 配置通道 %u: 方向=%d, 长度=%u\n",
           channel, desc->direction, desc->length);

    return RET_OK;
}

ret_code_t dma_start_transfer(uint32_t channel)
{
    if (!g_dma_initialized) {
        return RET_ERR_NOT_INIT;
    }
    if (channel >= DMA_MAX_CHANNELS) {
        return RET_ERR_PARAM;
    }
    if (!g_channels[channel].is_allocated) {
        return RET_ERR_PARAM;
    }
    if (g_channels[channel].state != DMA_CH_STATE_READY &&
        g_channels[channel].state != DMA_CH_STATE_PAUSED) {
        return RET_ERR_BUSY;
    }

    /* 如果是暂停状态，直接恢复 */
    if (g_channels[channel].state == DMA_CH_STATE_PAUSED) {
        return dma_resume_transfer(channel);
    }

    /* 创建传输线程（模拟硬件 DMA） */
    g_channels[channel].thread_id = thread_create(
        "dma_transfer", dma_transfer_thread, &g_channels[channel],
        THREAD_PRIORITY_HIGH);

    if (g_channels[channel].thread_id == 0) {
        g_channels[channel].state = DMA_CH_STATE_ERROR;
        g_channels[channel].error_count++;
        return RET_ERR_INTERNAL;
    }

    /* 启动线程 */
    return thread_start(g_channels[channel].thread_id);
}

ret_code_t dma_stop_transfer(uint32_t channel)
{
    if (!g_dma_initialized) {
        return RET_ERR_NOT_INIT;
    }
    if (channel >= DMA_MAX_CHANNELS) {
        return RET_ERR_PARAM;
    }
    if (!g_channels[channel].is_allocated) {
        return RET_ERR_PARAM;
    }

    mutex_lock(g_channels[channel].mutex);

    /* 设置状态为错误，让传输线程自行退出 */
    if (g_channels[channel].state == DMA_CH_STATE_RUNNING ||
        g_channels[channel].state == DMA_CH_STATE_PAUSED) {
        g_channels[channel].state = DMA_CH_STATE_ERROR;
    }

    mutex_unlock(g_channels[channel].mutex);

    /* 等待传输线程结束 */
    if (g_channels[channel].thread_id != 0) {
        thread_join(g_channels[channel].thread_id, NULL);
        g_channels[channel].thread_id = 0;
    }

    return RET_OK;
}

ret_code_t dma_pause_transfer(uint32_t channel)
{
    if (!g_dma_initialized) {
        return RET_ERR_NOT_INIT;
    }
    if (channel >= DMA_MAX_CHANNELS) {
        return RET_ERR_PARAM;
    }
    if (!g_channels[channel].is_allocated) {
        return RET_ERR_PARAM;
    }
    if (g_channels[channel].state != DMA_CH_STATE_RUNNING) {
        return RET_ERR_INTERNAL;
    }

    mutex_lock(g_channels[channel].mutex);
    g_channels[channel].state = DMA_CH_STATE_PAUSED;
    mutex_unlock(g_channels[channel].mutex);

    printf("[DMA] 通道 %u 暂停传输\n", channel);

    return RET_OK;
}

ret_code_t dma_resume_transfer(uint32_t channel)
{
    if (!g_dma_initialized) {
        return RET_ERR_NOT_INIT;
    }
    if (channel >= DMA_MAX_CHANNELS) {
        return RET_ERR_PARAM;
    }
    if (!g_channels[channel].is_allocated) {
        return RET_ERR_PARAM;
    }
    if (g_channels[channel].state != DMA_CH_STATE_PAUSED) {
        return RET_ERR_INTERNAL;
    }

    mutex_lock(g_channels[channel].mutex);
    g_channels[channel].state = DMA_CH_STATE_RUNNING;
    mutex_unlock(g_channels[channel].mutex);

    printf("[DMA] 通道 %u 恢复传输\n", channel);

    return RET_OK;
}

ret_code_t dma_wait_complete(uint32_t channel, uint32_t timeout_ms)
{
    ret_code_t ret = RET_OK;
    uint64_t start_time = 0;
    uint64_t elapsed = 0;

    if (!g_dma_initialized) {
        return RET_ERR_NOT_INIT;
    }
    if (channel >= DMA_MAX_CHANNELS) {
        return RET_ERR_PARAM;
    }
    if (!g_channels[channel].is_allocated) {
        return RET_ERR_PARAM;
    }

    /* 记录开始时间，用于超时检测 */
    start_time = get_timestamp_ms();

    /* 等待传输完成（包括 READY 状态，因为线程可能还没开始运行） */
    while (g_channels[channel].state == DMA_CH_STATE_READY ||
           g_channels[channel].state == DMA_CH_STATE_RUNNING ||
           g_channels[channel].state == DMA_CH_STATE_PAUSED) {
        /* 检查超时 */
        elapsed = get_timestamp_ms() - start_time;
        if (elapsed >= timeout_ms) {
            return RET_ERR_TIMEOUT;
        }

        /* 短暂休眠，避免占用 CPU */
        thread_sleep(1);
    }

    /* 等待传输线程结束 */
    if (g_channels[channel].thread_id != 0) {
        thread_join(g_channels[channel].thread_id, NULL);
        g_channels[channel].thread_id = 0;
    }

    return RET_OK;
}

ret_code_t dma_set_callback(uint32_t channel, dma_callback_t callback,
                            void *user_data)
{
    if (!g_dma_initialized) {
        return RET_ERR_NOT_INIT;
    }
    if (channel >= DMA_MAX_CHANNELS) {
        return RET_ERR_PARAM;
    }
    if (!g_channels[channel].is_allocated) {
        return RET_ERR_PARAM;
    }

    g_channels[channel].callback = callback;
    g_channels[channel].user_data = user_data;

    return RET_OK;
}

ret_code_t dma_get_channel_status(uint32_t channel, dma_channel_status_t *status)
{
    if (status == NULL) {
        return RET_ERR_PARAM;
    }
    if (!g_dma_initialized) {
        return RET_ERR_NOT_INIT;
    }
    if (channel >= DMA_MAX_CHANNELS) {
        return RET_ERR_PARAM;
    }
    if (!g_channels[channel].is_allocated) {
        return RET_ERR_PARAM;
    }

    /* 复制通道状态 */
    mutex_lock(g_channels[channel].mutex);
    status->channel_id = g_channels[channel].channel_id;
    status->state = g_channels[channel].state;
    memcpy(&status->desc, &g_channels[channel].desc, sizeof(dma_transfer_desc_t));
    status->transferred = g_channels[channel].transferred;
    status->start_time_ms = g_channels[channel].start_time_ms;
    status->complete_time_ms = g_channels[channel].complete_time_ms;
    status->transfer_count = g_channels[channel].transfer_count;
    status->error_count = g_channels[channel].error_count;
    status->callback = g_channels[channel].callback;
    status->user_data = g_channels[channel].user_data;
    status->is_allocated = g_channels[channel].is_allocated;
    mutex_unlock(g_channels[channel].mutex);

    return RET_OK;
}

dma_ch_state_t dma_get_state(uint32_t channel)
{
    if (!g_dma_initialized || channel >= DMA_MAX_CHANNELS) {
        return DMA_CH_STATE_ERROR;
    }
    if (!g_channels[channel].is_allocated) {
        return DMA_CH_STATE_ERROR;
    }

    return g_channels[channel].state;
}

ret_code_t dma_transfer_sync(const dma_transfer_desc_t *desc, uint32_t *transferred)
{
    uint32_t channel = 0;
    ret_code_t ret = RET_OK;

    if (desc == NULL) {
        return RET_ERR_PARAM;
    }
    if (!g_dma_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 分配通道 */
    channel = dma_alloc_channel();
    if (channel == 0xFFFFFFFF) {
        return RET_ERR_NO_SPACE;
    }

    /* 配置通道 */
    ret = dma_config_channel(channel, desc);
    if (ret != RET_OK) {
        dma_free_channel(channel);
        return ret;
    }

    /* 启动传输 */
    ret = dma_start_transfer(channel);
    if (ret != RET_OK) {
        dma_free_channel(channel);
        return ret;
    }

    /* 等待传输完成 */
    ret = dma_wait_complete(channel, 5000);  /* 5秒超时 */

    /* 获取已传输字节数 */
    if (transferred != NULL) {
        *transferred = g_channels[channel].transferred;
    }

    /* 释放通道 */
    dma_free_channel(channel);

    return ret;
}

void dma_print_status(void)
{
    const char *state_str[] = {"空闲", "就绪", "传输中", "暂停", "完成", "错误"};
    uint32_t i = 0;
    uint32_t allocated_count = 0;
    uint32_t running_count = 0;

    if (!g_dma_initialized) {
        printf("DMA 控制器未初始化\n");
        return;
    }

    /* 统计通道状态 */
    for (i = 0; i < DMA_MAX_CHANNELS; i++) {
        if (g_channels[i].is_allocated) {
            allocated_count++;
            if (g_channels[i].state == DMA_CH_STATE_RUNNING) {
                running_count++;
            }
        }
    }

    printf("DMA 控制器状态:\n");
    printf("  总通道数:   %d\n", DMA_MAX_CHANNELS);
    printf("  已分配:     %u\n", allocated_count);
    printf("  传输中:     %u\n", running_count);
    printf("  %-6s %-8s %-10s %-10s\n",
           "通道", "状态", "已传输", "传输次数");

    for (i = 0; i < DMA_MAX_CHANNELS; i++) {
        if (g_channels[i].is_allocated) {
            printf("  %-6u %-8s %-10u %-10u\n",
                   i,
                   state_str[g_channels[i].state],
                   g_channels[i].transferred,
                   g_channels[i].transfer_count);
        }
    }
}
