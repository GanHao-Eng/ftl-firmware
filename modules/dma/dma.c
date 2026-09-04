/**
 * @file dma.c
 * @brief DMA 模块实现
 * @details  DMA（直接内存访问）模块实现。
 *          使用线程模拟硬件 DMA 控制器，提供异步数据传输功能。
 *          支持多通道、传输暂停/恢复、完成回调、同步传输等特性。
 *          实际固件中 DMA 由硬件完成，这里用软件模拟以便测试和验证。
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
 * @details 每个 DMA 通道维护独立的传输状态、描述符、回调和同步原语
 */
typedef struct {
    uint32_t channel_id;        ///< 通道ID（0 ~ DMA_MAX_CHANNELS-1）
    dma_ch_state_t state;       ///< 通道状态（IDLE/READY/RUNNING/PAUSED/COMPLETE/ERROR）
    dma_transfer_desc_t desc;   ///< 当前传输描述符（源地址、目标地址、长度等）
    uint32_t transferred;       ///< 已传输字节数
    uint64_t start_time_ms;     ///< 传输开始时间（毫秒）
    uint64_t complete_time_ms;  ///< 传输完成时间（毫秒）
    uint32_t transfer_count;    ///< 该通道累计传输次数
    uint32_t error_count;       ///< 该通道累计错误次数
    dma_callback_t callback;    ///< 传输完成回调函数
    void *user_data;            ///< 用户数据（回调时透传）
    bool is_allocated;          ///< 通道是否已分配
    uint32_t thread_id;         ///< 传输线程ID（0表示无线程）
    mutex_handle_t mutex;       ///< 通道互斥锁（保护状态访问）
    cond_handle_t cond;         ///< 完成条件变量（用于等待传输完成）
} dma_channel_priv_t;

/* ============================================================
 *  全局变量
 * ============================================================ */

/**
 * @brief DMA 通道表
 * @details 静态数组管理所有 DMA 通道，通道通过 dma_alloc_channel 分配
 */
static dma_channel_priv_t g_channels[DMA_MAX_CHANNELS];

/**
 * @brief DMA 控制器初始化标志
 */
static bool g_dma_initialized = false;

/* ============================================================
 *  内部函数
 * ============================================================ */

/**
 * @brief 获取当前时间戳（毫秒）
 * @return 时间戳（毫秒，从系统启动开始单调递增）
 * @details 使用 CLOCK_MONOTONIC 时钟，不受系统时间调整影响，
 *          适合用于计算时间差和超时检测。
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
 * @brief DMA 传输线程函数
 * @param[in] arg 线程参数（DMA 通道指针 dma_channel_priv_t*）
 * @return 线程返回值（始终为NULL）
 * @note 模拟硬件 DMA 传输，实际硬件中由 DMA 控制器自动完成。
 *       这里使用 memcpy 模拟数据传输，usleep 模拟传输时间，
 *       分块传输以便支持暂停/恢复功能。
 */
static void *dma_transfer_thread(void *arg)
{
    dma_channel_priv_t *ch = (dma_channel_priv_t *)arg;
    uint8_t *src = NULL;
    uint8_t *dst = NULL;
    uint32_t remaining = 0;
    uint32_t chunk_size = 0;
    uint32_t transfer_delay_us = 0;

    /* 参数检查 */
    if (ch == NULL) {
        return NULL;
    }

    /* 加锁保护通道状态 */
    mutex_lock(ch->mutex);

    /* 检查通道状态，只有 READY 状态可以开始传输 */
    if (ch->state != DMA_CH_STATE_READY) {
        mutex_unlock(ch->mutex);
        return NULL;
    }

    /* 更新通道状态为运行中 */
    ch->state = DMA_CH_STATE_RUNNING;
    ch->start_time_ms = get_timestamp_ms();
    ch->transferred = 0;

    /* 获取源地址和目标地址（void* 转为 uint8_t* 以便指针运算） */
    src = (uint8_t *)ch->desc.src_addr;
    dst = (uint8_t *)ch->desc.dst_addr;
    remaining = ch->desc.length;

    printf("[DMA] 通道 %u 开始传输: 源=%p, 目标=%p, 长度=%u\n",
           ch->channel_id, ch->desc.src_addr, ch->desc.dst_addr, ch->desc.length);

    mutex_unlock(ch->mutex);

    /* 配置传输参数：分块大小和每块延时 */
    chunk_size = 4096;  /* 每次传输 4KB，模拟硬件 DMA 的突发传输 */
    transfer_delay_us = 10;  /* 每块传输延时 10us，模拟硬件传输速度 */

    /* 分块传输循环 */
    while (remaining > 0) {
        uint32_t to_transfer = 0;

        /* 检查通道状态（加锁） */
        mutex_lock(ch->mutex);

        /* 如果通道被暂停，等待恢复 */
        if (ch->state == DMA_CH_STATE_PAUSED) {
            mutex_unlock(ch->mutex);
            thread_sleep(1);  /* 休眠1ms等待恢复信号 */
            continue;
        }

        /* 如果通道状态不是 RUNNING（被停止或出错），退出循环 */
        if (ch->state != DMA_CH_STATE_RUNNING) {
            mutex_unlock(ch->mutex);
            break;
        }

        mutex_unlock(ch->mutex);

        /* 计算本次传输大小（最后一块可能不足 chunk_size） */
        to_transfer = (remaining < chunk_size) ? remaining : chunk_size;

        /* 执行数据传输（模拟硬件 DMA 的内存拷贝） */
        if (src != NULL && dst != NULL) {
            memcpy(dst, src, to_transfer);
        }

        /* 更新传输进度（加锁保护） */
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

        /* 减少剩余字节数 */
        remaining -= to_transfer;

        /* 模拟传输延时（硬件 DMA 传输需要时间） */
        usleep(transfer_delay_us);
    }

    /* 传输完成处理（加锁） */
    mutex_lock(ch->mutex);

    /* 根据最终状态判断是正常完成还是异常中止 */
    if (ch->state == DMA_CH_STATE_RUNNING) {
        /* 正常完成 */
        ch->state = DMA_CH_STATE_COMPLETE;
        ch->complete_time_ms = get_timestamp_ms();
        ch->transfer_count++;

        printf("[DMA] 通道 %u 传输完成: 已传输=%u 字节, 耗时=%llu ms\n",
               ch->channel_id, ch->transferred,
               (unsigned long long)(ch->complete_time_ms - ch->start_time_ms));
    } else {
        /* 异常中止（被停止或出错） */
        ch->error_count++;

        printf("[DMA] 通道 %u 传输中止: 已传输=%u 字节\n",
               ch->channel_id, ch->transferred);
    }

    /* 调用完成回调（如果设置了回调且使能了中断） */
    if (ch->callback != NULL && ch->desc.interrupt_enable) {
        /* 先保存回调相关数据（防止回调中修改通道状态） */
        bool success = (ch->state == DMA_CH_STATE_COMPLETE);
        dma_callback_t cb = ch->callback;
        void *user_data = ch->user_data;
        uint32_t transferred = ch->transferred;
        uint32_t channel_id = ch->channel_id;

        /* 解锁后调用回调（避免死锁） */
        mutex_unlock(ch->mutex);
        cb(channel_id, success, transferred, user_data);
        mutex_lock(ch->mutex);
    }

    /* 唤醒所有等待传输完成的线程 */
    cond_broadcast(ch->cond);

    mutex_unlock(ch->mutex);

    return NULL;
}

/* ============================================================
 *  DMA 控制器接口实现
 * ============================================================ */

/**
 * @brief 初始化 DMA 控制器
 * @retval RET_OK 初始化成功
 * @details 初始化 DMA 控制器，为每个通道创建互斥锁和条件变量。
 *          同时确保线程管理模块已初始化（DMA 传输依赖线程模块）。
 *          重复调用是安全的，已初始化时直接返回成功。
 */
ret_code_t dma_init(void)
{
    uint32_t i = 0;

    /* 已初始化则直接返回（幂等性保证） */
    if (g_dma_initialized) {
        return RET_OK;
    }

    /* 初始化线程管理模块（DMA 传输线程依赖线程模块） */
    thread_manager_init();

    /* 初始化通道表 */
    memset(g_channels, 0, sizeof(g_channels));

    /* 为每个通道初始化互斥锁和条件变量 */
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

/**
 * @brief 反初始化 DMA 控制器
 * @retval RET_OK 反初始化成功
 * @details 停止所有正在传输的通道，销毁每个通道的互斥锁和条件变量。
 *          未初始化时直接返回成功。
 */
ret_code_t dma_deinit(void)
{
    uint32_t i = 0;

    /* 未初始化则直接返回 */
    if (!g_dma_initialized) {
        return RET_OK;
    }

    /* 停止所有正在传输的通道，并销毁同步原语 */
    for (i = 0; i < DMA_MAX_CHANNELS; i++) {
        if (g_channels[i].is_allocated) {
            /* 如果通道正在传输或暂停，先停止传输 */
            if (g_channels[i].state == DMA_CH_STATE_RUNNING ||
                g_channels[i].state == DMA_CH_STATE_PAUSED) {
                dma_stop_transfer(i);
            }
            /* 销毁互斥锁和条件变量 */
            mutex_destroy(g_channels[i].mutex);
            cond_destroy(g_channels[i].cond);
        }
    }

    g_dma_initialized = false;

    printf("[DMA] DMA 控制器反初始化完成\n");

    return RET_OK;
}

/**
 * @brief 分配一个空闲的 DMA 通道
 * @return 通道ID（0 ~ DMA_MAX_CHANNELS-1），失败返回 0xFFFFFFFF
 * @details 从通道表中查找第一个未分配的通道，标记为已分配并返回ID。
 *          采用简单的线性查找策略，适合通道数量较少的场景。
 */
uint32_t dma_alloc_channel(void)
{
    uint32_t i = 0;

    /* 初始化检查 */
    if (!g_dma_initialized) {
        return 0xFFFFFFFF;
    }

    /* 线性查找第一个空闲通道 */
    for (i = 0; i < DMA_MAX_CHANNELS; i++) {
        if (!g_channels[i].is_allocated) {
            /* 标记为已分配，重置状态 */
            g_channels[i].is_allocated = true;
            g_channels[i].state = DMA_CH_STATE_IDLE;
            g_channels[i].transferred = 0;

            printf("[DMA] 分配通道 %u\n", i);

            return i;
        }
    }

    /* 没有空闲通道 */
    printf("[DMA] 没有空闲通道\n");
    return 0xFFFFFFFF;
}

/**
 * @brief 释放 DMA 通道
 * @param[in] channel 通道ID
 * @retval RET_OK 释放成功
 * @retval RET_ERR_NOT_INIT DMA 未初始化
 * @retval RET_ERR_PARAM 通道ID越界或通道未分配
 * @details 如果通道正在传输，先停止传输，然后标记通道为空闲。
 *          释放后的通道可以被重新分配。
 */
ret_code_t dma_free_channel(uint32_t channel)
{
    /* 初始化检查 */
    if (!g_dma_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 通道ID边界检查 */
    if (channel >= DMA_MAX_CHANNELS) {
        return RET_ERR_PARAM;
    }

    /* 通道分配状态检查 */
    if (!g_channels[channel].is_allocated) {
        return RET_ERR_PARAM;
    }

    /* 如果通道正在传输或暂停，先停止传输 */
    if (g_channels[channel].state == DMA_CH_STATE_RUNNING ||
        g_channels[channel].state == DMA_CH_STATE_PAUSED) {
        dma_stop_transfer(channel);
    }

    /* 标记为空闲，重置状态 */
    g_channels[channel].is_allocated = false;
    g_channels[channel].state = DMA_CH_STATE_IDLE;

    printf("[DMA] 释放通道 %u\n", channel);

    return RET_OK;
}

/**
 * @brief 配置 DMA 通道传输参数
 * @param[in] channel 通道ID
 * @param[in] desc 传输描述符指针（源地址、目标地址、长度、方向等）
 * @retval RET_OK 配置成功
 * @retval RET_ERR_NOT_INIT DMA 未初始化
 * @retval RET_ERR_PARAM 通道ID越界、描述符为空或通道未分配
 * @retval RET_ERR_BUSY 通道正在传输中
 * @details 设置通道的传输描述符，将通道状态置为 READY，等待启动传输。
 *          传输描述符包含源地址、目标地址、传输长度、传输方向、
 *          数据宽度、突发长度、地址自增使能、中断使能等参数。
 */
ret_code_t dma_config_channel(uint32_t channel, const dma_transfer_desc_t *desc)
{
    /* 初始化检查 */
    if (!g_dma_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 参数检查 */
    if (channel >= DMA_MAX_CHANNELS || desc == NULL) {
        return RET_ERR_PARAM;
    }

    /* 通道分配状态检查 */
    if (!g_channels[channel].is_allocated) {
        return RET_ERR_PARAM;
    }

    /* 通道忙检查：正在传输时不能重新配置 */
    if (g_channels[channel].state == DMA_CH_STATE_RUNNING) {
        return RET_ERR_BUSY;
    }

    /* 复制传输描述符到通道 */
    memcpy(&g_channels[channel].desc, desc, sizeof(dma_transfer_desc_t));

    /* 设置通道状态为就绪，等待启动 */
    g_channels[channel].state = DMA_CH_STATE_READY;
    g_channels[channel].transferred = 0;

    printf("[DMA] 配置通道 %u: 方向=%d, 长度=%u\n",
           channel, desc->direction, desc->length);

    return RET_OK;
}

/**
 * @brief 启动 DMA 传输
 * @param[in] channel 通道ID
 * @retval RET_OK 启动成功
 * @retval RET_ERR_NOT_INIT DMA 未初始化
 * @retval RET_ERR_PARAM 通道ID越界或通道未分配
 * @retval RET_ERR_BUSY 通道状态不允许启动
 * @retval RET_ERR_INTERNAL 内部错误（线程创建失败）
 * @details 启动已配置好的 DMA 通道传输。如果通道处于 PAUSED 状态，
 *          则调用恢复函数继续传输。如果处于 READY 状态，则创建
 *          传输线程开始异步传输。
 */
ret_code_t dma_start_transfer(uint32_t channel)
{
    /* 初始化检查 */
    if (!g_dma_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 通道ID边界检查 */
    if (channel >= DMA_MAX_CHANNELS) {
        return RET_ERR_PARAM;
    }

    /* 通道分配状态检查 */
    if (!g_channels[channel].is_allocated) {
        return RET_ERR_PARAM;
    }

    /* 通道状态检查：只有 READY 或 PAUSED 状态可以启动 */
    if (g_channels[channel].state != DMA_CH_STATE_READY &&
        g_channels[channel].state != DMA_CH_STATE_PAUSED) {
        return RET_ERR_BUSY;
    }

    /* 如果是暂停状态，直接恢复传输 */
    if (g_channels[channel].state == DMA_CH_STATE_PAUSED) {
        return dma_resume_transfer(channel);
    }

    /* 创建传输线程（模拟硬件 DMA 控制器） */
    g_channels[channel].thread_id = thread_create(
        "dma_transfer", dma_transfer_thread, &g_channels[channel],
        THREAD_PRIORITY_HIGH);

    /* 线程创建失败处理 */
    if (g_channels[channel].thread_id == 0) {
        g_channels[channel].state = DMA_CH_STATE_ERROR;
        g_channels[channel].error_count++;
        return RET_ERR_INTERNAL;
    }

    /* 启动传输线程 */
    return thread_start(g_channels[channel].thread_id);
}

/**
 * @brief 停止 DMA 传输
 * @param[in] channel 通道ID
 * @retval RET_OK 停止成功
 * @retval RET_ERR_NOT_INIT DMA 未初始化
 * @retval RET_ERR_PARAM 通道ID越界或通道未分配
 * @details 中止正在进行的 DMA 传输。通过将通道状态置为 ERROR，
 *          让传输线程自行检测并退出。然后等待线程结束，确保资源释放。
 */
ret_code_t dma_stop_transfer(uint32_t channel)
{
    /* 初始化检查 */
    if (!g_dma_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 通道ID边界检查 */
    if (channel >= DMA_MAX_CHANNELS) {
        return RET_ERR_PARAM;
    }

    /* 通道分配状态检查 */
    if (!g_channels[channel].is_allocated) {
        return RET_ERR_PARAM;
    }

    /* 加锁设置通道状态为错误，通知传输线程退出 */
    mutex_lock(g_channels[channel].mutex);

    if (g_channels[channel].state == DMA_CH_STATE_RUNNING ||
        g_channels[channel].state == DMA_CH_STATE_PAUSED) {
        g_channels[channel].state = DMA_CH_STATE_ERROR;
    }

    mutex_unlock(g_channels[channel].mutex);

    /* 等待传输线程结束（确保线程完全退出后再返回） */
    if (g_channels[channel].thread_id != 0) {
        thread_join(g_channels[channel].thread_id, NULL);
        g_channels[channel].thread_id = 0;
    }

    return RET_OK;
}

/**
 * @brief 暂停 DMA 传输
 * @param[in] channel 通道ID
 * @retval RET_OK 暂停成功
 * @retval RET_ERR_NOT_INIT DMA 未初始化
 * @retval RET_ERR_PARAM 通道ID越界或通道未分配
 * @retval RET_ERR_INTERNAL 通道不在运行状态
 * @details 将正在传输的通道置为 PAUSED 状态，传输线程检测到后
 *          会进入等待循环，直到通道被恢复或停止。已传输的数据保留。
 */
ret_code_t dma_pause_transfer(uint32_t channel)
{
    /* 初始化检查 */
    if (!g_dma_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 通道ID边界检查 */
    if (channel >= DMA_MAX_CHANNELS) {
        return RET_ERR_PARAM;
    }

    /* 通道分配状态检查 */
    if (!g_channels[channel].is_allocated) {
        return RET_ERR_PARAM;
    }

    /* 只有运行中的通道可以暂停 */
    if (g_channels[channel].state != DMA_CH_STATE_RUNNING) {
        return RET_ERR_INTERNAL;
    }

    /* 加锁设置暂停状态 */
    mutex_lock(g_channels[channel].mutex);
    g_channels[channel].state = DMA_CH_STATE_PAUSED;
    mutex_unlock(g_channels[channel].mutex);

    printf("[DMA] 通道 %u 暂停传输\n", channel);

    return RET_OK;
}

/**
 * @brief 恢复 DMA 传输
 * @param[in] channel 通道ID
 * @retval RET_OK 恢复成功
 * @retval RET_ERR_NOT_INIT DMA 未初始化
 * @retval RET_ERR_PARAM 通道ID越界或通道未分配
 * @retval RET_ERR_INTERNAL 通道不在暂停状态
 * @details 将暂停的通道恢复为 RUNNING 状态，传输线程检测到后
 *          会从上次暂停的位置继续传输。
 */
ret_code_t dma_resume_transfer(uint32_t channel)
{
    /* 初始化检查 */
    if (!g_dma_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 通道ID边界检查 */
    if (channel >= DMA_MAX_CHANNELS) {
        return RET_ERR_PARAM;
    }

    /* 通道分配状态检查 */
    if (!g_channels[channel].is_allocated) {
        return RET_ERR_PARAM;
    }

    /* 只有暂停的通道可以恢复 */
    if (g_channels[channel].state != DMA_CH_STATE_PAUSED) {
        return RET_ERR_INTERNAL;
    }

    /* 加锁设置运行状态 */
    mutex_lock(g_channels[channel].mutex);
    g_channels[channel].state = DMA_CH_STATE_RUNNING;
    mutex_unlock(g_channels[channel].mutex);

    printf("[DMA] 通道 %u 恢复传输\n", channel);

    return RET_OK;
}

/**
 * @brief 等待 DMA 传输完成（带超时）
 * @param[in] channel 通道ID
 * @param[in] timeout_ms 超时时间（毫秒）
 * @retval RET_OK 传输完成
 * @retval RET_ERR_NOT_INIT DMA 未初始化
 * @retval RET_ERR_PARAM 通道ID越界或通道未分配
 * @retval RET_ERR_TIMEOUT 等待超时
 * @details 轮询等待通道传输完成。包括 READY 状态（线程可能还未开始运行），
 *          避免在线程启动前就返回。超时后返回超时错误，传输仍在后台继续。
 *          传输完成后等待线程结束，确保资源完全释放。
 */
ret_code_t dma_wait_complete(uint32_t channel, uint32_t timeout_ms)
{
    uint64_t start_time = 0;
    uint64_t elapsed = 0;

    /* 初始化检查 */
    if (!g_dma_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 通道ID边界检查 */
    if (channel >= DMA_MAX_CHANNELS) {
        return RET_ERR_PARAM;
    }

    /* 通道分配状态检查 */
    if (!g_channels[channel].is_allocated) {
        return RET_ERR_PARAM;
    }

    /* 记录开始时间，用于超时检测 */
    start_time = get_timestamp_ms();

    /* 轮询等待传输完成（包括 READY 状态，因为线程可能还没开始运行） */
    while (g_channels[channel].state == DMA_CH_STATE_READY ||
           g_channels[channel].state == DMA_CH_STATE_RUNNING ||
           g_channels[channel].state == DMA_CH_STATE_PAUSED) {
        /* 检查超时 */
        elapsed = get_timestamp_ms() - start_time;
        if (elapsed >= timeout_ms) {
            return RET_ERR_TIMEOUT;
        }

        /* 短暂休眠，避免占用 CPU（轮询间隔1ms） */
        thread_sleep(1);
    }

    /* 传输完成后等待线程结束，确保资源完全释放 */
    if (g_channels[channel].thread_id != 0) {
        thread_join(g_channels[channel].thread_id, NULL);
        g_channels[channel].thread_id = 0;
    }

    return RET_OK;
}

/**
 * @brief 设置 DMA 传输完成回调
 * @param[in] channel 通道ID
 * @param[in] callback 回调函数指针
 * @param[in] user_data 用户数据（回调时透传）
 * @retval RET_OK 设置成功
 * @retval RET_ERR_NOT_INIT DMA 未初始化
 * @retval RET_ERR_PARAM 通道ID越界或通道未分配
 * @details 设置传输完成时的回调函数。当传输完成且描述符中使能了中断时，
 *          回调函数会被调用，参数包括通道ID、是否成功、已传输字节数和用户数据。
 */
ret_code_t dma_set_callback(uint32_t channel, dma_callback_t callback,
                            void *user_data)
{
    /* 初始化检查 */
    if (!g_dma_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 通道ID边界检查 */
    if (channel >= DMA_MAX_CHANNELS) {
        return RET_ERR_PARAM;
    }

    /* 通道分配状态检查 */
    if (!g_channels[channel].is_allocated) {
        return RET_ERR_PARAM;
    }

    /* 设置回调函数和用户数据 */
    g_channels[channel].callback = callback;
    g_channels[channel].user_data = user_data;

    return RET_OK;
}

/**
 * @brief 获取 DMA 通道状态
 * @param[in] channel 通道ID
 * @param[out] status 通道状态输出缓冲区
 * @retval RET_OK 获取成功
 * @retval RET_ERR_PARAM 参数错误（输出缓冲区为空）
 * @retval RET_ERR_NOT_INIT DMA 未初始化
 * @retval RET_ERR_PARAM 通道ID越界或通道未分配
 * @details 获取通道的完整状态信息，包括状态、描述符、传输进度、
 *          时间戳、统计计数、回调等。加锁保护，确保数据一致性。
 */
ret_code_t dma_get_channel_status(uint32_t channel, dma_channel_status_t *status)
{
    /* 输出缓冲区空指针检查 */
    if (status == NULL) {
        return RET_ERR_PARAM;
    }

    /* 初始化检查 */
    if (!g_dma_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 通道ID边界检查 */
    if (channel >= DMA_MAX_CHANNELS) {
        return RET_ERR_PARAM;
    }

    /* 通道分配状态检查 */
    if (!g_channels[channel].is_allocated) {
        return RET_ERR_PARAM;
    }

    /* 加锁复制通道状态（确保数据一致性） */
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

/**
 * @brief 获取 DMA 通道状态（简化版）
 * @param[in] channel 通道ID
 * @return 通道状态，参数错误时返回 DMA_CH_STATE_ERROR
 * @details 快速获取通道状态，不需要完整状态信息时使用。
 *          不加锁，适合状态轮询场景。
 */
dma_ch_state_t dma_get_state(uint32_t channel)
{
    /* 参数检查，失败返回错误状态 */
    if (!g_dma_initialized || channel >= DMA_MAX_CHANNELS) {
        return DMA_CH_STATE_ERROR;
    }
    if (!g_channels[channel].is_allocated) {
        return DMA_CH_STATE_ERROR;
    }

    return g_channels[channel].state;
}

/**
 * @brief 同步 DMA 传输（一站式接口）
 * @param[in] desc 传输描述符指针
 * @param[out] transferred 已传输字节数输出（可为NULL）
 * @retval RET_OK 传输成功
 * @retval RET_ERR_PARAM 参数错误（描述符为空）
 * @retval RET_ERR_NOT_INIT DMA 未初始化
 * @retval RET_ERR_NO_SPACE 没有空闲通道
 * @retval RET_ERR_TIMEOUT 传输超时
 * @details 同步传输的便捷接口，内部完成通道分配、配置、启动、
 *          等待完成和释放的全流程。调用者阻塞直到传输完成或超时。
 *          适合简单的一次性数据传输场景。
 */
ret_code_t dma_transfer_sync(const dma_transfer_desc_t *desc, uint32_t *transferred)
{
    uint32_t channel = 0;
    ret_code_t ret = RET_OK;

    /* 描述符空指针检查 */
    if (desc == NULL) {
        return RET_ERR_PARAM;
    }

    /* 初始化检查 */
    if (!g_dma_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 第一步：分配通道 */
    channel = dma_alloc_channel();
    if (channel == 0xFFFFFFFF) {
        return RET_ERR_NO_SPACE;
    }

    /* 第二步：配置通道 */
    ret = dma_config_channel(channel, desc);
    if (ret != RET_OK) {
        dma_free_channel(channel);
        return ret;
    }

    /* 第三步：启动传输 */
    ret = dma_start_transfer(channel);
    if (ret != RET_OK) {
        dma_free_channel(channel);
        return ret;
    }

    /* 第四步：等待传输完成（5秒超时） */
    ret = dma_wait_complete(channel, 5000);

    /* 获取已传输字节数（如果调用者需要） */
    if (transferred != NULL) {
        *transferred = g_channels[channel].transferred;
    }

    /* 第五步：释放通道 */
    dma_free_channel(channel);

    return ret;
}

/**
 * @brief 打印 DMA 控制器状态
 * @details 打印 DMA 控制器的整体状态和每个已分配通道的详细信息，
 *          包括通道状态、已传输字节数、传输次数等。用于调试和监控。
 */
void dma_print_status(void)
{
    /* 状态字符串表（索引对应 dma_ch_state_t 枚举值） */
    const char *state_str[] = {"空闲", "就绪", "传输中", "暂停", "完成", "错误"};
    uint32_t i = 0;
    uint32_t allocated_count = 0;
    uint32_t running_count = 0;

    /* 初始化检查 */
    if (!g_dma_initialized) {
        printf("DMA 控制器未初始化\n");
        return;
    }

    /* 统计已分配和正在传输的通道数 */
    for (i = 0; i < DMA_MAX_CHANNELS; i++) {
        if (g_channels[i].is_allocated) {
            allocated_count++;
            if (g_channels[i].state == DMA_CH_STATE_RUNNING) {
                running_count++;
            }
        }
    }

    /* 打印控制器整体状态 */
    printf("DMA 控制器状态:\n");
    printf("  总通道数:   %d\n", DMA_MAX_CHANNELS);
    printf("  已分配:     %u\n", allocated_count);
    printf("  传输中:     %u\n", running_count);

    /* 打印通道表头 */
    printf("  %-6s %-8s %-10s %-10s\n",
           "通道", "状态", "已传输", "传输次数");

    /* 打印每个已分配通道的详细信息 */
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
