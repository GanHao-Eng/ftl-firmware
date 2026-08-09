/**
 * @file msg_queue.h
 * @brief 消息队列接口
 * @details 企业级固件模块间通信的消息队列机制
 */

#ifndef FIRMWARE_MSG_QUEUE_H
#define FIRMWARE_MSG_QUEUE_H

#include "common/common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  消息类型定义
 * ============================================================ */

/**
 * @brief 模块ID枚举
 */
typedef enum {
    MODULE_NAND = 0,       ///< NAND 模块
    MODULE_FTL = 1,        ///< FTL 模块
    MODULE_HOST_IF = 2,    ///< 主机接口模块
    MODULE_MANAGER = 3,    ///< 管理模块
    MODULE_LOG = 4,        ///< 日志模块
    MODULE_MAX = 5         ///< 模块数量
} module_id_t;

/**
 * @brief 消息类型枚举
 */
typedef enum {
    MSG_TYPE_NAND_READ = 0,         ///< NAND 读请求
    MSG_TYPE_NAND_WRITE = 1,        ///< NAND 写请求
    MSG_TYPE_NAND_ERASE = 2,        ///< NAND 擦除请求
    MSG_TYPE_NAND_READ_RESP = 3,    ///< NAND 读响应
    MSG_TYPE_NAND_WRITE_RESP = 4,   ///< NAND 写响应
    MSG_TYPE_NAND_ERASE_RESP = 5,   ///< NAND 擦除响应

    MSG_TYPE_FTL_READ = 10,         ///< FTL 读请求
    MSG_TYPE_FTL_WRITE = 11,        ///< FTL 写请求
    MSG_TYPE_FTL_TRIM = 12,         ///< FTL TRIM 请求
    MSG_TYPE_FTL_READ_RESP = 13,    ///< FTL 读响应
    MSG_TYPE_FTL_WRITE_RESP = 14,   ///< FTL 写响应
    MSG_TYPE_FTL_TRIM_RESP = 15,    ///< FTL TRIM 响应

    MSG_TYPE_HOST_CMD = 20,         ///< 主机命令
    MSG_TYPE_HOST_CMD_COMPLETE = 21,///< 主机命令完成

    MSG_TYPE_MGR_HEALTH_CHECK = 30, ///< 健康检查
    MSG_TYPE_MGR_ERROR_REPORT = 31, ///< 错误报告
    MSG_TYPE_MGR_CONFIG = 32,       ///< 配置更新

    MSG_TYPE_LOG_WRITE = 40,        ///< 日志写入请求

    MSG_TYPE_MAX = 50               ///< 消息类型最大值
} msg_type_t;

/**
 * @brief 消息优先级枚举
 */
typedef enum {
    MSG_PRIORITY_LOW = 0,      ///< 低优先级
    MSG_PRIORITY_NORMAL = 1,   ///< 普通优先级
    MSG_PRIORITY_HIGH = 2,     ///< 高优先级
    MSG_PRIORITY_URGENT = 3    ///< 紧急优先级
} msg_priority_t;

/* ============================================================
 *  消息数据结构
 * ============================================================ */

/**
 * @brief NAND 操作请求数据
 */
typedef struct {
    uint32_t block;       ///< 物理块号
    uint32_t page;        ///< 物理页号
    uint8_t *data_buf;    ///< 数据缓冲区指针
    uint32_t data_len;    ///< 数据长度
} msg_nand_req_t;

/**
 * @brief NAND 操作响应数据
 */
typedef struct {
    ret_code_t result;    ///< 操作结果
    uint32_t block;       ///< 物理块号
    uint32_t page;        ///< 物理页号
    uint8_t *data_buf;    ///< 数据缓冲区指针
    uint32_t data_len;    ///< 数据长度
} msg_nand_resp_t;

/**
 * @brief FTL 操作请求数据
 */
typedef struct {
    uint32_t lpn;         ///< 逻辑页号
    uint32_t count;       ///< 页数
    uint8_t *data_buf;    ///< 数据缓冲区指针
    uint32_t data_len;    ///< 数据长度
} msg_ftl_req_t;

/**
 * @brief FTL 操作响应数据
 */
typedef struct {
    ret_code_t result;    ///< 操作结果
    uint32_t lpn;         ///< 逻辑页号
    uint32_t count;       ///< 页数
    uint8_t *data_buf;    ///< 数据缓冲区指针
    uint32_t data_len;    ///< 数据长度
} msg_ftl_resp_t;

/**
 * @brief 健康检查数据
 */
typedef struct {
    module_id_t module_id;  ///< 模块ID
    bool is_healthy;        ///< 是否健康
    uint32_t error_count;   ///< 错误计数
    uint32_t uptime_ms;     ///< 运行时间（毫秒）
} msg_health_check_t;

/**
 * @brief 错误报告数据
 */
typedef struct {
    module_id_t module_id;  ///< 模块ID
    uint32_t error_code;    ///< 错误码
    char error_msg[128];    ///< 错误信息
    uint32_t timestamp;     ///< 时间戳
} msg_error_report_t;

/**
 * @brief 消息头
 */
typedef struct {
    msg_type_t type;          ///< 消息类型
    msg_priority_t priority;  ///< 消息优先级
    module_id_t src_module;   ///< 源模块
    module_id_t dst_module;   ///< 目标模块
    uint32_t msg_id;          ///< 消息ID
    uint32_t timestamp;       ///< 时间戳
    uint32_t data_len;        ///< 数据长度
} msg_header_t;

/**
 * @brief 消息结构体
 */
typedef struct {
    msg_header_t header;    ///< 消息头
    union {
        msg_nand_req_t nand_req;      ///< NAND 请求
        msg_nand_resp_t nand_resp;    ///< NAND 响应
        msg_ftl_req_t ftl_req;        ///< FTL 请求
        msg_ftl_resp_t ftl_resp;      ///< FTL 响应
        msg_health_check_t health;    ///< 健康检查
        msg_error_report_t error;     ///< 错误报告
        uint8_t raw_data[256];        ///< 原始数据
    } data;                 ///< 消息数据
} message_t;

/* ============================================================
 *  消息队列接口
 * ============================================================ */

/**
 * @brief 初始化消息队列
 * @param[in] module_id 模块ID
 * @param[in] queue_size 队列大小
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_INTERNAL 内部错误
 */
ret_code_t msg_queue_init(module_id_t module_id, uint32_t queue_size);

/**
 * @brief 销毁消息队列
 * @param[in] module_id 模块ID
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t msg_queue_deinit(module_id_t module_id);

/**
 * @brief 发送消息
 * @param[in] msg 消息指针
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NO_SPACE 队列已满
 */
ret_code_t msg_queue_send(const message_t *msg);

/**
 * @brief 接收消息（非阻塞）
 * @param[in] module_id 模块ID
 * @param[out] msg 消息缓冲区指针
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_BUSY 队列为空
 */
ret_code_t msg_queue_recv(module_id_t module_id, message_t *msg);

/**
 * @brief 接收消息（阻塞）
 * @param[in] module_id 模块ID
 * @param[out] msg 消息缓冲区指针
 * @param[in] timeout_ms 超时时间（毫秒），0表示无限等待
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_TIMEOUT 超时
 */
ret_code_t msg_queue_recv_blocking(module_id_t module_id, message_t *msg, uint32_t timeout_ms);

/**
 * @brief 获取队列中消息数量
 * @param[in] module_id 模块ID
 * @return 消息数量，参数错误返回0
 */
uint32_t msg_queue_get_count(module_id_t module_id);

/**
 * @brief 检查队列是否为空
 * @param[in] module_id 模块ID
 * @return true 为空，false 不为空或参数错误
 */
bool msg_queue_is_empty(module_id_t module_id);

/**
 * @brief 检查队列是否已满
 * @param[in] module_id 模块ID
 * @return true 已满，false 未满或参数错误
 */
bool msg_queue_is_full(module_id_t module_id);

/**
 * @brief 清空队列
 * @param[in] module_id 模块ID
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t msg_queue_clear(module_id_t module_id);

#ifdef __cplusplus
}
#endif

#endif /* FIRMWARE_MSG_QUEUE_H */
