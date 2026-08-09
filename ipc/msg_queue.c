/**
 * @file msg_queue.c
 * @brief 消息队列实现
 * @details 企业级固件模块间通信的消息队列机制实现
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
 */
typedef struct msg_node {
    message_t msg;          ///< 消息
    struct msg_node *next;  ///< 下一个节点指针
} msg_node_t;

/**
 * @brief 消息队列
 */
typedef struct {
    msg_node_t *head;       ///< 队列头
    msg_node_t *tail;       ///< 队列尾
    uint32_t count;         ///< 消息数量
    uint32_t max_size;      ///< 最大容量
    bool is_initialized;    ///< 初始化标志
} msg_queue_t;

/* ============================================================
 *  全局变量
 * ============================================================ */

static msg_queue_t g_queues[MODULE_MAX];  ///< 各模块的消息队列
static uint32_t g_msg_id_counter = 0;     ///< 消息ID计数器

/* ============================================================
 *  内部函数
 * ============================================================ */

/**
 * @brief 获取当前时间戳（毫秒）
 * @return 时间戳
 */
static uint32_t get_timestamp_ms(void)
{
    /* 简化实现，实际固件中使用硬件定时器 */
    static uint32_t counter = 0;
    return counter++;
}

/**
 * @brief 创建消息节点
 * @param[in] msg 消息指针
 * @return 消息节点指针，失败返回NULL
 */
static msg_node_t *create_node(const message_t *msg)
{
    msg_node_t *node = NULL;

    if (msg == NULL) {
        return NULL;
    }

    node = (msg_node_t *)malloc(sizeof(msg_node_t));
    if (node == NULL) {
        return NULL;
    }

    memcpy(&node->msg, msg, sizeof(message_t));
    node->next = NULL;

    return node;
}

/**
 * @brief 销毁消息节点
 * @param[in] node 消息节点指针
 */
static void destroy_node(msg_node_t *node)
{
    if (node != NULL) {
        free(node);
    }
}

/* ============================================================
 *  接口实现
 * ============================================================ */

ret_code_t msg_queue_init(module_id_t module_id, uint32_t queue_size)
{
    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }
    if (queue_size == 0U) {
        return RET_ERR_PARAM;
    }

    g_queues[module_id].head = NULL;
    g_queues[module_id].tail = NULL;
    g_queues[module_id].count = 0U;
    g_queues[module_id].max_size = queue_size;
    g_queues[module_id].is_initialized = true;

    return RET_OK;
}

ret_code_t msg_queue_deinit(module_id_t module_id)
{
    msg_node_t *node = NULL;
    msg_node_t *next = NULL;

    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }
    if (!g_queues[module_id].is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 释放所有节点 */
    node = g_queues[module_id].head;
    while (node != NULL) {
        next = node->next;
        destroy_node(node);
        node = next;
    }

    g_queues[module_id].head = NULL;
    g_queues[module_id].tail = NULL;
    g_queues[module_id].count = 0U;
    g_queues[module_id].max_size = 0U;
    g_queues[module_id].is_initialized = false;

    return RET_OK;
}

ret_code_t msg_queue_send(const message_t *msg)
{
    msg_node_t *node = NULL;
    module_id_t dst_module;

    if (msg == NULL) {
        return RET_ERR_PARAM;
    }

    dst_module = msg->header.dst_module;
    if (dst_module >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }
    if (!g_queues[dst_module].is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 检查队列是否已满 */
    if (g_queues[dst_module].count >= g_queues[dst_module].max_size) {
        return RET_ERR_NO_SPACE;
    }

    /* 创建新节点 */
    node = create_node(msg);
    if (node == NULL) {
        return RET_ERR_INTERNAL;
    }

    /* 设置消息ID和时间戳 */
    node->msg.header.msg_id = g_msg_id_counter++;
    node->msg.header.timestamp = get_timestamp_ms();

    /* 按优先级插入队列 */
    if (g_queues[dst_module].head == NULL) {
        /* 队列为空 */
        g_queues[dst_module].head = node;
        g_queues[dst_module].tail = node;
    } else {
        msg_priority_t new_prio = node->msg.header.priority;
        msg_node_t *curr = g_queues[dst_module].head;
        msg_node_t *prev = NULL;

        /* 找到合适的插入位置（高优先级在前） */
        while (curr != NULL && curr->msg.header.priority >= new_prio) {
            prev = curr;
            curr = curr->next;
        }

        if (prev == NULL) {
            /* 插入到队头 */
            node->next = g_queues[dst_module].head;
            g_queues[dst_module].head = node;
        } else if (curr == NULL) {
            /* 插入到队尾 */
            g_queues[dst_module].tail->next = node;
            g_queues[dst_module].tail = node;
        } else {
            /* 插入到中间 */
            node->next = curr;
            prev->next = node;
        }
    }

    g_queues[dst_module].count++;

    return RET_OK;
}

ret_code_t msg_queue_recv(module_id_t module_id, message_t *msg)
{
    msg_node_t *node = NULL;

    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }
    if (msg == NULL) {
        return RET_ERR_PARAM;
    }
    if (!g_queues[module_id].is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 检查队列是否为空 */
    if (g_queues[module_id].count == 0U) {
        return RET_ERR_BUSY;
    }

    /* 从队头取出消息 */
    node = g_queues[module_id].head;
    g_queues[module_id].head = node->next;
    g_queues[module_id].count--;

    /* 如果队列为空，更新尾指针 */
    if (g_queues[module_id].head == NULL) {
        g_queues[module_id].tail = NULL;
    }

    /* 复制消息数据 */
    memcpy(msg, &node->msg, sizeof(message_t));

    /* 释放节点 */
    destroy_node(node);

    return RET_OK;
}

ret_code_t msg_queue_recv_blocking(module_id_t module_id, message_t *msg, uint32_t timeout_ms)
{
    ret_code_t ret = RET_OK;
    uint32_t waited = 0U;

    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }
    if (msg == NULL) {
        return RET_ERR_PARAM;
    }
    if (!g_queues[module_id].is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 简化实现：轮询等待 */
    while (1) {
        ret = msg_queue_recv(module_id, msg);
        if (ret == RET_OK) {
            return RET_OK;
        }

        if (timeout_ms > 0U) {
            waited++;
            if (waited >= timeout_ms) {
                return RET_ERR_TIMEOUT;
            }
        }

        /* 简单延时，实际固件中使用操作系统延时 */
        /* 这里简化处理，直接返回 */
        if (timeout_ms == 0U) {
            /* 无限等待，简化为立即返回 */
            return ret;
        }
    }
}

uint32_t msg_queue_get_count(module_id_t module_id)
{
    if (module_id >= MODULE_MAX) {
        return 0U;
    }
    if (!g_queues[module_id].is_initialized) {
        return 0U;
    }

    return g_queues[module_id].count;
}

bool msg_queue_is_empty(module_id_t module_id)
{
    if (module_id >= MODULE_MAX) {
        return true;
    }
    if (!g_queues[module_id].is_initialized) {
        return true;
    }

    return (g_queues[module_id].count == 0U);
}

bool msg_queue_is_full(module_id_t module_id)
{
    if (module_id >= MODULE_MAX) {
        return true;
    }
    if (!g_queues[module_id].is_initialized) {
        return true;
    }

    return (g_queues[module_id].count >= g_queues[module_id].max_size);
}

ret_code_t msg_queue_clear(module_id_t module_id)
{
    msg_node_t *node = NULL;
    msg_node_t *next = NULL;

    if (module_id >= MODULE_MAX) {
        return RET_ERR_PARAM;
    }
    if (!g_queues[module_id].is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 释放所有节点 */
    node = g_queues[module_id].head;
    while (node != NULL) {
        next = node->next;
        destroy_node(node);
        node = next;
    }

    g_queues[module_id].head = NULL;
    g_queues[module_id].tail = NULL;
    g_queues[module_id].count = 0U;

    return RET_OK;
}
