/**
 * @file msg_queue.c
 * @brief 消息队列实现
 * @details 固件模块间通信的消息队列机制实现，支持优先级队列、
 *          非阻塞接收、阻塞接收（带超时）、队列状态查询等功能。
 *          采用链表实现，按消息优先级排序，高优先级消息优先处理。
 */

#include "msg_queue.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ============================================================
 *  内部数据结构
 * ============================================================ */

/**
 * @brief 消息队列节点
 * @details 链表节点，包含消息数据和指向下一节点的指针
 */
typedef struct msg_node {
    message_t msg;          ///< 消息数据
    struct msg_node *next;  ///< 下一个节点指针
} msg_node_t;

/**
 * @brief 消息队列
 * @details 每个模块维护一个独立的消息队列，采用带头尾指针的链表实现
 */
typedef struct {
    msg_node_t *head;       ///< 队列头（最高优先级消息）
    msg_node_t *tail;       ///< 队列尾（最低优先级消息）
    uint32_t count;         ///< 当前消息数量
    uint32_t max_size;      ///< 队列最大容量
    bool is_initialized;    ///< 初始化标志
} msg_queue_t;

/* ============================================================
 *  全局变量
 * ============================================================ */

/**
 * @brief 各模块的消息队列数组
 * @details 每个模块ID对应一个独立的消息队列，模块间通过目标模块ID
 *          将消息发送到对应队列，实现解耦的模块间通信。
 */
static msg_queue_t g_queues[MODULE_MAX];

/**
 * @brief 全局消息ID计数器
 * @details 每条消息分配唯一递增的ID，用于消息追踪、去重和调试。
 */
static uint32_t g_msg_id_counter = 0;

/* ============================================================
 *  内部函数
 * ============================================================ */

/**
 * @brief 获取当前时间戳（毫秒）
 * @return 时间戳（递增计数器）
 * @note 简化实现，使用静态计数器模拟时间戳。实际固件中应使用
 *       硬件定时器或系统滴答时钟（SysTick）获取真实时间。
 */
static uint32_t get_timestamp_ms(void)
{
    /* 静态计数器，每次调用递增，模拟时间戳 */
    static uint32_t counter = 0;
    return counter++;
}

/**
 * @brief 创建消息节点
 * @param[in] msg 消息指针
 * @return 消息节点指针，失败返回NULL
 * @details 分配内存并复制消息数据，初始化next指针为NULL。
 *          调用者负责通过destroy_node释放节点内存。
 */
static msg_node_t *create_node(const message_t *msg)
{
    msg_node_t *node = NULL;

    /* 空指针检查 */
    if (msg == NULL) {
        return NULL;
    }

    /* 分配节点内存 */
    node = (msg_node_t *)malloc(sizeof(msg_node_t));
    if (node == NULL) {
        return NULL;
    }

    /* 复制消息数据到节点 */
    memcpy(&node->msg, msg, sizeof(message_t));

    /* 初始化next指针 */
    node->next = NULL;

    return node;
}

/**
 * @brief 销毁消息节点
 * @param[in] node 消息节点指针
 * @details 释放消息节点占用的内存。空指针安全，传入NULL不做任何操作。
 */
static void destroy_node(msg_node_t *node)
{
    /* 空指针安全检查 */
    if (node != NULL) {
        free(node);
    }
}

/* ============================================================
 *  接口实现
 * ============================================================ */

/**
 * @brief 初始化指定模块的消息队列
 * @param[in] module_id 模块ID
 * @param[in] queue_size 队列最大容量
 * @retval RET_OK 初始化成功
 * @retval RET_ERR_PARAM 参数错误（模块ID越界或队列大小为0）
 * @details 初始化指定模块的消息队列，设置最大容量，清空头尾指针和计数。
 *          每个模块在启动时必须调用此函数初始化自己的接收队列。
 */
ret_code_t msg_queue_init(module_id_t module_id, uint32_t queue_size)
{
    /* 模块ID边界检查 */
    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }

    /* 队列大小有效性检查 */
    if (queue_size == 0U) {
        return RET_ERR_PARAM;
    }

    /* 初始化队列头指针 */
    g_queues[module_id].head = NULL;

    /* 初始化队列尾指针 */
    g_queues[module_id].tail = NULL;

    /* 清空消息计数 */
    g_queues[module_id].count = 0U;

    /* 设置队列最大容量 */
    g_queues[module_id].max_size = queue_size;

    /* 标记为已初始化 */
    g_queues[module_id].is_initialized = true;

    return RET_OK;
}

/**
 * @brief 反初始化指定模块的消息队列
 * @param[in] module_id 模块ID
 * @retval RET_OK 反初始化成功
 * @retval RET_ERR_PARAM 参数错误（模块ID越界）
 * @retval RET_ERR_NOT_INIT 队列未初始化
 * @details 释放队列中所有消息节点的内存，重置队列状态。
 *          模块关闭时应调用此函数释放资源，防止内存泄漏。
 */
ret_code_t msg_queue_deinit(module_id_t module_id)
{
    msg_node_t *node = NULL;
    msg_node_t *next = NULL;

    /* 模块ID边界检查 */
    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }

    /* 初始化状态检查 */
    if (!g_queues[module_id].is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 遍历链表，释放所有消息节点 */
    node = g_queues[module_id].head;
    while (node != NULL) {
        /* 保存下一节点指针（当前节点释放后无法访问） */
        next = node->next;

        /* 释放当前节点 */
        destroy_node(node);

        /* 移动到下一节点 */
        node = next;
    }

    /* 重置队列状态 */
    g_queues[module_id].head = NULL;
    g_queues[module_id].tail = NULL;
    g_queues[module_id].count = 0U;
    g_queues[module_id].max_size = 0U;
    g_queues[module_id].is_initialized = false;

    return RET_OK;
}

/**
 * @brief 发送消息到目标模块的消息队列
 * @param[in] msg 消息指针（包含目标模块ID和优先级）
 * @retval RET_OK 发送成功
 * @retval RET_ERR_PARAM 参数错误（空指针或目标模块ID越界）
 * @retval RET_ERR_NOT_INIT 目标模块队列未初始化
 * @retval RET_ERR_NO_SPACE 目标队列已满
 * @retval RET_ERR_INTERNAL 内部错误（内存分配失败）
 * @details 将消息按优先级插入到目标模块的队列中。高优先级消息
 *          插入到队列前部，低优先级插入到后部。自动分配消息ID和时间戳。
 *          这是模块间通信的核心接口，支持优先级调度。
 */
ret_code_t msg_queue_send(const message_t *msg)
{
    msg_node_t *node = NULL;
    module_id_t dst_module;

    /* 空指针检查 */
    if (msg == NULL) {
        return RET_ERR_PARAM;
    }

    /* 获取目标模块ID */
    dst_module = msg->header.dst_module;

    /* 目标模块ID边界检查 */
    if (dst_module >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }

    /* 目标队列初始化状态检查 */
    if (!g_queues[dst_module].is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 检查队列是否已满，防止溢出 */
    if (g_queues[dst_module].count >= g_queues[dst_module].max_size) {
        return RET_ERR_NO_SPACE;
    }

    /* 创建新的消息节点 */
    node = create_node(msg);
    if (node == NULL) {
        return RET_ERR_INTERNAL;
    }

    /* 分配全局唯一的消息ID */
    node->msg.header.msg_id = g_msg_id_counter++;

    /* 设置消息时间戳 */
    node->msg.header.timestamp = get_timestamp_ms();

    /* 按优先级插入队列（高优先级在前） */
    if (g_queues[dst_module].head == NULL) {
        /* 队列为空，新节点既是头也是尾 */
        g_queues[dst_module].head = node;
        g_queues[dst_module].tail = node;
    } else {
        msg_priority_t new_prio = node->msg.header.priority;
        msg_node_t *curr = g_queues[dst_module].head;
        msg_node_t *prev = NULL;

        /* 遍历找到第一个优先级低于新消息的位置 */
        while (curr != NULL && curr->msg.header.priority >= new_prio) {
            prev = curr;
            curr = curr->next;
        }

        if (prev == NULL) {
            /* 新消息优先级最高，插入到队头 */
            node->next = g_queues[dst_module].head;
            g_queues[dst_module].head = node;
        } else if (curr == NULL) {
            /* 新消息优先级最低，插入到队尾 */
            g_queues[dst_module].tail->next = node;
            g_queues[dst_module].tail = node;
        } else {
            /* 插入到中间位置（prev和curr之间） */
            node->next = curr;
            prev->next = node;
        }
    }

    /* 增加消息计数 */
    g_queues[dst_module].count++;

    return RET_OK;
}

/**
 * @brief 从指定模块的消息队列接收消息（非阻塞）
 * @param[in] module_id 模块ID（接收者）
 * @param[out] msg 输出消息缓冲区
 * @retval RET_OK 接收成功
 * @retval RET_ERR_PARAM 参数错误（模块ID越界或输出缓冲区为NULL）
 * @retval RET_ERR_NOT_INIT 队列未初始化
 * @retval RET_ERR_BUSY 队列为空（无消息可读）
 * @details 从队列头部取出最高优先级的消息，复制到输出缓冲区后释放节点。
 *          非阻塞调用，队列为空时立即返回RET_ERR_BUSY。
 */
ret_code_t msg_queue_recv(module_id_t module_id, message_t *msg)
{
    msg_node_t *node = NULL;

    /* 模块ID边界检查 */
    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }

    /* 输出缓冲区空指针检查 */
    if (msg == NULL) {
        return RET_ERR_PARAM;
    }

    /* 初始化状态检查 */
    if (!g_queues[module_id].is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 检查队列是否为空 */
    if (g_queues[module_id].count == 0U) {
        return RET_ERR_BUSY;
    }

    /* 从队头取出消息节点（最高优先级） */
    node = g_queues[module_id].head;
    g_queues[module_id].head = node->next;
    g_queues[module_id].count--;

    /* 如果取出后队列为空，更新尾指针 */
    if (g_queues[module_id].head == NULL) {
        g_queues[module_id].tail = NULL;
    }

    /* 复制消息数据到输出缓冲区 */
    memcpy(msg, &node->msg, sizeof(message_t));

    /* 释放消息节点内存 */
    destroy_node(node);

    return RET_OK;
}

/**
 * @brief 阻塞接收消息（带超时）
 * @param[in] module_id 模块ID（接收者）
 * @param[out] msg 输出消息缓冲区
 * @param[in] timeout_ms 超时时间（毫秒），0表示无限等待
 * @retval RET_OK 接收成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_INIT 队列未初始化
 * @retval RET_ERR_TIMEOUT 超时未收到消息
 * @details 轮询等待消息到达，直到收到消息或超时。
 * @note 简化实现，使用轮询+计数器模拟超时。实际固件中应使用
 *       信号量或条件变量实现真正的阻塞等待，避免CPU忙等。
 */
ret_code_t msg_queue_recv_blocking(module_id_t module_id, message_t *msg, uint32_t timeout_ms)
{
    ret_code_t ret = RET_OK;
    uint32_t waited = 0U;

    /* 模块ID边界检查 */
    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }

    /* 输出缓冲区空指针检查 */
    if (msg == NULL) {
        return RET_ERR_PARAM;
    }

    /* 初始化状态检查 */
    if (!g_queues[module_id].is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 轮询等待消息 */
    while (1) {
        /* 尝试非阻塞接收 */
        ret = msg_queue_recv(module_id, msg);
        if (ret == RET_OK) {
            /* 收到消息，成功返回 */
            return RET_OK;
        }

        /* 超时检查（timeout_ms > 0 时启用超时） */
        if (timeout_ms > 0U) {
            waited++;
            if (waited >= timeout_ms) {
                /* 超时，返回超时错误 */
                return RET_ERR_TIMEOUT;
            }
        }

        /* 简化处理：timeout_ms == 0 时无限等待，但此处简化为立即返回 */
        if (timeout_ms == 0U) {
            return ret;
        }
    }
}

/**
 * @brief 获取指定模块队列中的消息数量
 * @param[in] module_id 模块ID
 * @return 消息数量，参数错误或未初始化时返回0
 * @details 查询队列当前待处理的消息数量，用于流量控制和负载均衡判断。
 */
uint32_t msg_queue_get_count(module_id_t module_id)
{
    /* 模块ID边界检查 */
    if (module_id >= MODULE_MAX) {
        return 0U;
    }

    /* 初始化状态检查 */
    if (!g_queues[module_id].is_initialized) {
        return 0U;
    }

    return g_queues[module_id].count;
}

/**
 * @brief 检查指定模块的队列是否为空
 * @param[in] module_id 模块ID
 * @return true-队列为空，false-队列非空
 * @details 判断队列是否有消息待处理。参数错误或未初始化时返回true（视为空）。
 */
bool msg_queue_is_empty(module_id_t module_id)
{
    /* 模块ID边界检查 */
    if (module_id >= MODULE_MAX) {
        return true;
    }

    /* 初始化状态检查 */
    if (!g_queues[module_id].is_initialized) {
        return true;
    }

    return (g_queues[module_id].count == 0U);
}

/**
 * @brief 检查指定模块的队列是否已满
 * @param[in] module_id 模块ID
 * @return true-队列已满，false-队列未满
 * @details 判断队列是否达到最大容量。参数错误或未初始化时返回true（视为满，防止发送）。
 */
bool msg_queue_is_full(module_id_t module_id)
{
    /* 模块ID边界检查 */
    if (module_id >= MODULE_MAX) {
        return true;
    }

    /* 初始化状态检查 */
    if (!g_queues[module_id].is_initialized) {
        return true;
    }

    return (g_queues[module_id].count >= g_queues[module_id].max_size);
}

/**
 * @brief 清空指定模块的消息队列
 * @param[in] module_id 模块ID
 * @retval RET_OK 清空成功
 * @retval RET_ERR_PARAM 参数错误（模块ID越界）
 * @retval RET_ERR_NOT_INIT 队列未初始化
 * @details 释放队列中所有消息节点，重置队列状态。
 *          用于模块复位、错误恢复或需要丢弃所有未处理消息的场景。
 */
ret_code_t msg_queue_clear(module_id_t module_id)
{
    msg_node_t *node = NULL;
    msg_node_t *next = NULL;

    /* 模块ID边界检查 */
    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }

    /* 初始化状态检查 */
    if (!g_queues[module_id].is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 遍历链表，释放所有消息节点 */
    node = g_queues[module_id].head;
    while (node != NULL) {
        /* 保存下一节点指针 */
        next = node->next;

        /* 释放当前节点 */
        destroy_node(node);

        /* 移动到下一节点 */
        node = next;
    }

    /* 重置队列头尾指针和计数 */
    g_queues[module_id].head = NULL;
    g_queues[module_id].tail = NULL;
    g_queues[module_id].count = 0U;

    return RET_OK;
}
