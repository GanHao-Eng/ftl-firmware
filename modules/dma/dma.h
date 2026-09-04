/**
 * @file dma.h
 * @brief DMA 模块
 * @details DMA（直接内存访问）模块，模拟硬件 DMA 控制器
 *          提供异步数据传输功能，减少 CPU 占用，提高系统性能
 */

#ifndef FIRMWARE_DMA_H
#define FIRMWARE_DMA_H

#include "common/common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  DMA 配置定义
 * ============================================================ */

/**
 * @brief 最大 DMA 通道数量
 */
#define DMA_MAX_CHANNELS 8

/**
 * @brief DMA 传输方向枚举
 */
typedef enum {
    DMA_DIR_MEM_TO_MEM = 0,   ///< 内存到内存
    DMA_DIR_MEM_TO_DEV = 1,   ///< 内存到设备
    DMA_DIR_DEV_TO_MEM = 2,   ///< 设备到内存
    DMA_DIR_DEV_TO_DEV = 3,   ///< 设备到设备
    DMA_DIR_MAX = 4           ///< 方向最大值
} dma_direction_t;

/**
 * @brief DMA 传输宽度枚举
 */
typedef enum {
    DMA_WIDTH_BYTE = 0,       ///< 字节（8位）
    DMA_WIDTH_HALFWORD = 1,   ///< 半字（16位）
    DMA_WIDTH_WORD = 2,       ///< 字（32位）
    DMA_WIDTH_DOUBLEWORD = 3, ///< 双字（64位）
    DMA_WIDTH_MAX = 4         ///< 宽度最大值
} dma_width_t;

/**
 * @brief DMA 突发长度枚举
 */
typedef enum {
    DMA_BURST_1 = 0,          ///< 突发长度 1
    DMA_BURST_4 = 1,          ///< 突发长度 4
    DMA_BURST_8 = 2,          ///< 突发长度 8
    DMA_BURST_16 = 3,         ///< 突发长度 16
    DMA_BURST_MAX = 4         ///< 突发长度最大值
} dma_burst_t;

/**
 * @brief DMA 通道状态枚举
 */
typedef enum {
    DMA_CH_STATE_IDLE = 0,    ///< 空闲
    DMA_CH_STATE_READY = 1,   ///< 就绪
    DMA_CH_STATE_RUNNING = 2, ///< 传输中
    DMA_CH_STATE_PAUSED = 3,  ///< 暂停
    DMA_CH_STATE_COMPLETE = 4,///< 完成
    DMA_CH_STATE_ERROR = 5,   ///< 错误
    DMA_CH_STATE_MAX = 6      ///< 状态最大值
} dma_ch_state_t;

/* ============================================================
 *  DMA 传输描述符
 * ============================================================ */

/**
 * @brief DMA 传输描述符结构体
 */
typedef struct {
    void *src_addr;             ///< 源地址
    void *dst_addr;             ///< 目标地址
    uint32_t length;            ///< 传输长度（字节）
    dma_direction_t direction;  ///< 传输方向
    dma_width_t src_width;      ///< 源传输宽度
    dma_width_t dst_width;      ///< 目标传输宽度
    dma_burst_t src_burst;      ///< 源突发长度
    dma_burst_t dst_burst;      ///< 目标突发长度
    bool src_increment;         ///< 源地址自增
    bool dst_increment;         ///< 目标地址自增
    bool interrupt_enable;      ///< 完成中断使能
    bool circular_mode;         ///< 循环模式
} dma_transfer_desc_t;

/**
 * @brief DMA 传输完成回调函数类型
 * @param[in] channel DMA 通道号
 * @param[in] success 是否成功
 * @param[in] transferred 已传输字节数
 * @param[in] user_data 用户数据指针
 */
typedef void (*dma_callback_t)(uint32_t channel, bool success,
                               uint32_t transferred, void *user_data);

/* ============================================================
 *  DMA 通道状态结构体
 * ============================================================ */

/**
 * @brief DMA 通道状态结构体
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
} dma_channel_status_t;

/* ============================================================
 *  DMA 控制器接口
 * ============================================================ */

/**
 * @brief 初始化 DMA 控制器
 * @retval RET_OK 成功
 * @retval RET_ERR_INTERNAL 内部错误
 */
ret_code_t dma_init(void);

/**
 * @brief 反初始化 DMA 控制器
 * @retval RET_OK 成功
 */
ret_code_t dma_deinit(void);

/**
 * @brief 分配 DMA 通道
 * @return 通道号，失败返回 0xFFFFFFFF
 */
uint32_t dma_alloc_channel(void);

/**
 * @brief 释放 DMA 通道
 * @param[in] channel 通道号
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t dma_free_channel(uint32_t channel);

/**
 * @brief 配置 DMA 通道
 * @param[in] channel 通道号
 * @param[in] desc 传输描述符指针
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_FOUND 通道不存在
 */
ret_code_t dma_config_channel(uint32_t channel, const dma_transfer_desc_t *desc);

/**
 * @brief 启动 DMA 传输
 * @param[in] channel 通道号
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_FOUND 通道不存在
 * @retval RET_ERR_BUSY 通道忙
 */
ret_code_t dma_start_transfer(uint32_t channel);

/**
 * @brief 停止 DMA 传输
 * @param[in] channel 通道号
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_FOUND 通道不存在
 */
ret_code_t dma_stop_transfer(uint32_t channel);

/**
 * @brief 暂停 DMA 传输
 * @param[in] channel 通道号
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_FOUND 通道不存在
 */
ret_code_t dma_pause_transfer(uint32_t channel);

/**
 * @brief 恢复 DMA 传输
 * @param[in] channel 通道号
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_FOUND 通道不存在
 */
ret_code_t dma_resume_transfer(uint32_t channel);

/**
 * @brief 等待 DMA 传输完成
 * @param[in] channel 通道号
 * @param[in] timeout_ms 超时时间（毫秒）
 * @retval RET_OK 成功
 * @retval RET_ERR_TIMEOUT 超时
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_FOUND 通道不存在
 */
ret_code_t dma_wait_complete(uint32_t channel, uint32_t timeout_ms);

/**
 * @brief 设置 DMA 传输完成回调
 * @param[in] channel 通道号
 * @param[in] callback 回调函数
 * @param[in] user_data 用户数据
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_FOUND 通道不存在
 */
ret_code_t dma_set_callback(uint32_t channel, dma_callback_t callback,
                            void *user_data);

/**
 * @brief 获取 DMA 通道状态
 * @param[in] channel 通道号
 * @param[out] status 状态指针
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_FOUND 通道不存在
 */
ret_code_t dma_get_channel_status(uint32_t channel, dma_channel_status_t *status);

/**
 * @brief 获取 DMA 通道状态（简化版）
 * @param[in] channel 通道号
 * @return 通道状态，通道不存在返回 DMA_CH_STATE_ERROR
 */
dma_ch_state_t dma_get_state(uint32_t channel);

/**
 * @brief 执行同步 DMA 传输（阻塞）
 * @param[in] desc 传输描述符指针
 * @param[out] transferred 已传输字节数指针（可为 NULL）
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_INTERNAL 内部错误
 * @note 简化接口，内部自动分配和释放通道
 */
ret_code_t dma_transfer_sync(const dma_transfer_desc_t *desc, uint32_t *transferred);

/**
 * @brief 打印 DMA 控制器状态
 */
void dma_print_status(void);

#ifdef __cplusplus
}
#endif

#endif /* FIRMWARE_DMA_H */
