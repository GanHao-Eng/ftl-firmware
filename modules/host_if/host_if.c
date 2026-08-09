/**
 * @file host_if.c
 * @brief 主机接口模块实现
 * @details 企业级固件的主机接口模块实现，模拟 NVMe 协议接口
 */

#include "host_if.h"
#include "ftl.h"
#include "nand.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ============================================================
 *  内部数据结构
 * ============================================================ */

/**
 * @brief 命令队列节点
 */
typedef struct cmd_node {
    nvme_cmd_t cmd;          ///< NVMe 命令
    struct cmd_node *next;   ///< 下一个节点
} cmd_node_t;

/**
 * @brief 主机接口私有数据
 */
typedef struct {
    host_if_config_t config;   ///< 配置
    host_if_stats_t stats;     ///< 统计信息

    /* 提交队列 */
    cmd_node_t *sq_head;       ///< 提交队列头
    cmd_node_t *sq_tail;       ///< 提交队列尾
    uint32_t sq_count;         ///< 提交队列数量

    /* 完成队列 */
    nvme_cqe_t *cq_buf;        ///< 完成队列缓冲区
    uint32_t cq_head;          ///< 完成队列头
    uint32_t cq_tail;          ///< 完成队列尾
    uint32_t cq_count;         ///< 完成队列数量
    uint8_t phase_tag;         ///< 阶段标签

    bool is_initialized;       ///< 初始化标志
} host_if_dev_t;

/* ============================================================
 *  全局变量
 * ============================================================ */

static host_if_dev_t g_host_if;  ///< 主机接口设备
static uint16_t g_cid_counter = 0; ///< 命令ID计数器

/* ============================================================
 *  内部函数
 * ============================================================ */

/**
 * @brief 创建命令节点
 * @param[in] cmd 命令指针
 * @return 命令节点指针，失败返回NULL
 */
static cmd_node_t *create_cmd_node(const nvme_cmd_t *cmd)
{
    cmd_node_t *node = NULL;

    if (cmd == NULL) {
        return NULL;
    }

    node = (cmd_node_t *)malloc(sizeof(cmd_node_t));
    if (node == NULL) {
        return NULL;
    }

    memcpy(&node->cmd, cmd, sizeof(nvme_cmd_t));
    node->next = NULL;

    return node;
}

/**
 * @brief 销毁命令节点
 * @param[in] node 命令节点指针
 */
static void destroy_cmd_node(cmd_node_t *node)
{
    if (node != NULL) {
        free(node);
    }
}

/**
 * @brief 将命令转换为 FTL 请求
 * @param[in] cmd NVMe 命令指针
 * @param[out] ftl_req FTL 请求指针
 * @note 预留函数，用于消息队列模式
 */
__attribute__((unused))
static void cmd_to_ftl_req(const nvme_cmd_t *cmd, msg_ftl_req_t *ftl_req)
{
    if (cmd == NULL || ftl_req == NULL) {
        return;
    }

    ftl_req->lpn = (uint32_t)cmd->slba;
    ftl_req->count = cmd->nlb + 1;
    ftl_req->data_buf = cmd->data_buf;
    ftl_req->data_len = cmd->data_len;
}

/**
 * @brief 添加完成队列条目
 * @param[in] cqe 完成队列条目指针
 * @retval RET_OK 成功
 * @retval RET_ERR_NO_SPACE 队列已满
 */
static ret_code_t add_cqe(const nvme_cqe_t *cqe)
{
    if (cqe == NULL) {
        return RET_ERR_PARAM;
    }

    if (g_host_if.cq_count >= g_host_if.config.queue_size) {
        return RET_ERR_NO_SPACE;
    }

    memcpy(&g_host_if.cq_buf[g_host_if.cq_tail], cqe, sizeof(nvme_cqe_t));
    g_host_if.cq_buf[g_host_if.cq_tail].phase_tag = g_host_if.phase_tag;

    g_host_if.cq_tail = (g_host_if.cq_tail + 1) % g_host_if.config.queue_size;
    g_host_if.cq_count++;

    /* 检查是否需要翻转阶段标签 */
    if (g_host_if.cq_tail == 0) {
        g_host_if.phase_tag = !g_host_if.phase_tag;
    }

    return RET_OK;
}

/**
 * @brief 处理读命令
 * @param[in] cmd 命令指针
 * @return 状态码
 */
static nvme_status_t process_read_cmd(const nvme_cmd_t *cmd)
{
    ret_code_t ret = RET_OK;
    uint32_t lpn = 0;
    uint32_t count = 0;
    uint32_t i = 0;
    uint8_t *buf = NULL;

    if (cmd == NULL) {
        return NVME_STATUS_INVALID_FIELD;
    }

    /* 计算起始 LPN 和数量 */
    lpn = (uint32_t)cmd->slba;
    count = cmd->nlb + 1;
    buf = cmd->data_buf;

    /* 循环读取每个页（简化实现，逐页读取） */
    for (i = 0; i < count; i++) {
        ret = ftl_read(lpn + i, buf + i * NAND_PAGE_SIZE);
        if (ret != RET_OK) {
            return NVME_STATUS_INTERNAL_ERROR;
        }
    }

    /* 更新统计 */
    g_host_if.stats.read_cmds++;
    g_host_if.stats.total_cmds++;
    g_host_if.stats.total_read_bytes += count * NAND_PAGE_SIZE;

    return NVME_STATUS_SUCCESS;
}

/**
 * @brief 处理写命令
 * @param[in] cmd 命令指针
 * @return 状态码
 */
static nvme_status_t process_write_cmd(const nvme_cmd_t *cmd)
{
    ret_code_t ret = RET_OK;
    uint32_t lpn = 0;
    uint32_t count = 0;
    uint32_t i = 0;
    const uint8_t *buf = NULL;

    if (cmd == NULL) {
        return NVME_STATUS_INVALID_FIELD;
    }

    /* 计算起始 LPN 和数量 */
    lpn = (uint32_t)cmd->slba;
    count = cmd->nlb + 1;
    buf = cmd->data_buf;

    /* 循环写入每个页（简化实现，逐页写入） */
    for (i = 0; i < count; i++) {
        ret = ftl_write(lpn + i, buf + i * NAND_PAGE_SIZE);
        if (ret != RET_OK) {
            return NVME_STATUS_INTERNAL_ERROR;
        }
    }

    /* 更新统计 */
    g_host_if.stats.write_cmds++;
    g_host_if.stats.total_cmds++;
    g_host_if.stats.total_write_bytes += count * NAND_PAGE_SIZE;

    return NVME_STATUS_SUCCESS;
}

/**
 * @brief 处理 TRIM 命令
 * @param[in] cmd 命令指针
 * @return 状态码
 */
static nvme_status_t process_trim_cmd(const nvme_cmd_t *cmd)
{
    ret_code_t ret = RET_OK;
    uint32_t lpn = 0;
    uint32_t count = 0;

    if (cmd == NULL) {
        return NVME_STATUS_INVALID_FIELD;
    }

    /* 计算 LPN 和数量 */
    lpn = (uint32_t)cmd->slba;
    count = cmd->nlb + 1;

    /* 直接调用 FTL TRIM 接口（简化实现，不通过消息队列） */
    ret = ftl_trim(lpn, count);
    if (ret != RET_OK) {
        return NVME_STATUS_INTERNAL_ERROR;
    }

    /* 更新统计 */
    g_host_if.stats.trim_cmds++;
    g_host_if.stats.total_cmds++;

    return NVME_STATUS_SUCCESS;
}

/* ============================================================
 *  接口实现
 * ============================================================ */

ret_code_t host_if_init(const host_if_config_t *config)
{
    if (config == NULL) {
        return RET_ERR_PARAM;
    }
    if (config->queue_size == 0U) {
        return RET_ERR_PARAM;
    }

    memset(&g_host_if, 0, sizeof(g_host_if));

    /* 保存配置 */
    memcpy(&g_host_if.config, config, sizeof(host_if_config_t));

    /* 分配完成队列缓冲区 */
    g_host_if.cq_buf = (nvme_cqe_t *)malloc(sizeof(nvme_cqe_t) * config->queue_size);
    if (g_host_if.cq_buf == NULL) {
        return RET_ERR_INTERNAL;
    }

    /* 初始化提交队列 */
    g_host_if.sq_head = NULL;
    g_host_if.sq_tail = NULL;
    g_host_if.sq_count = 0U;

    /* 初始化完成队列 */
    g_host_if.cq_head = 0U;
    g_host_if.cq_tail = 0U;
    g_host_if.cq_count = 0U;
    g_host_if.phase_tag = 1U;

    /* 初始化消息队列 */
    msg_queue_init(MODULE_HOST_IF, config->queue_size);

    g_host_if.is_initialized = true;

    return RET_OK;
}

ret_code_t host_if_deinit(void)
{
    cmd_node_t *node = NULL;
    cmd_node_t *next = NULL;

    if (!g_host_if.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 释放提交队列 */
    node = g_host_if.sq_head;
    while (node != NULL) {
        next = node->next;
        destroy_cmd_node(node);
        node = next;
    }

    /* 释放完成队列 */
    if (g_host_if.cq_buf != NULL) {
        free(g_host_if.cq_buf);
        g_host_if.cq_buf = NULL;
    }

    /* 销毁消息队列 */
    msg_queue_deinit(MODULE_HOST_IF);

    g_host_if.is_initialized = false;

    return RET_OK;
}

ret_code_t host_if_submit_cmd(const nvme_cmd_t *cmd)
{
    cmd_node_t *node = NULL;

    if (cmd == NULL) {
        return RET_ERR_PARAM;
    }
    if (!g_host_if.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 检查队列是否已满 */
    if (g_host_if.sq_count >= g_host_if.config.queue_size) {
        return RET_ERR_NO_SPACE;
    }

    /* 创建命令节点 */
    node = create_cmd_node(cmd);
    if (node == NULL) {
        return RET_ERR_INTERNAL;
    }

    /* 设置命令ID */
    if (node->cmd.cid == 0U) {
        node->cmd.cid = g_cid_counter++;
    }

    /* 添加到提交队列尾部 */
    if (g_host_if.sq_head == NULL) {
        g_host_if.sq_head = node;
        g_host_if.sq_tail = node;
    } else {
        g_host_if.sq_tail->next = node;
        g_host_if.sq_tail = node;
    }

    g_host_if.sq_count++;

    return RET_OK;
}

ret_code_t host_if_poll_cq(nvme_cqe_t *cqe)
{
    if (cqe == NULL) {
        return RET_ERR_PARAM;
    }
    if (!g_host_if.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 检查完成队列是否为空 */
    if (g_host_if.cq_count == 0U) {
        return RET_ERR_BUSY;
    }

    /* 从完成队列头取出条目 */
    memcpy(cqe, &g_host_if.cq_buf[g_host_if.cq_head], sizeof(nvme_cqe_t));

    g_host_if.cq_head = (g_host_if.cq_head + 1) % g_host_if.config.queue_size;
    g_host_if.cq_count--;

    return RET_OK;
}

ret_code_t host_if_process(void)
{
    cmd_node_t *node = NULL;
    nvme_cqe_t cqe;
    nvme_status_t status = NVME_STATUS_SUCCESS;

    if (!g_host_if.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 处理提交队列中的命令 */
    while (g_host_if.sq_count > 0U) {
        /* 从提交队列头取出命令 */
        node = g_host_if.sq_head;
        g_host_if.sq_head = node->next;
        g_host_if.sq_count--;

        if (g_host_if.sq_head == NULL) {
            g_host_if.sq_tail = NULL;
        }

        /* 根据命令类型处理 */
        switch (node->cmd.opcode) {
        case NVME_CMD_READ:
            status = process_read_cmd(&node->cmd);
            break;

        case NVME_CMD_WRITE:
            status = process_write_cmd(&node->cmd);
            break;

        case NVME_CMD_DATASET_MGMT:
            status = process_trim_cmd(&node->cmd);
            break;

        case NVME_CMD_FLUSH:
            /* Flush 命令简化处理 */
            status = NVME_STATUS_SUCCESS;
            break;

        default:
            status = NVME_STATUS_INVALID_OPCODE;
            break;
        }

        /* 添加完成队列条目 */
        memset(&cqe, 0, sizeof(cqe));
        cqe.cid = node->cmd.cid;
        cqe.status = status;
        cqe.sqid = 0U;
        cqe.sqhd = g_host_if.sq_count;

        add_cqe(&cqe);

        /* 更新统计 */
        if (status == NVME_STATUS_SUCCESS) {
            g_host_if.stats.completed_cmds++;
        } else {
            g_host_if.stats.failed_cmds++;
        }

        /* 释放命令节点 */
        destroy_cmd_node(node);
    }

    return RET_OK;
}

ret_code_t host_if_get_stats(host_if_stats_t *stats)
{
    if (stats == NULL) {
        return RET_ERR_PARAM;
    }
    if (!g_host_if.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    memcpy(stats, &g_host_if.stats, sizeof(host_if_stats_t));

    return RET_OK;
}

ret_code_t host_if_reset_stats(void)
{
    if (!g_host_if.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    memset(&g_host_if.stats, 0, sizeof(host_if_stats_t));

    return RET_OK;
}

void host_if_print_stats(void)
{
    if (!g_host_if.is_initialized) {
        printf("主机接口未初始化\n");
        return;
    }

    printf("主机接口统计信息:\n");
    printf("  总命令数:     %llu\n", (unsigned long long)g_host_if.stats.total_cmds);
    printf("  读命令数:     %llu\n", (unsigned long long)g_host_if.stats.read_cmds);
    printf("  写命令数:     %llu\n", (unsigned long long)g_host_if.stats.write_cmds);
    printf("  TRIM命令数:   %llu\n", (unsigned long long)g_host_if.stats.trim_cmds);
    printf("  已完成命令数: %llu\n", (unsigned long long)g_host_if.stats.completed_cmds);
    printf("  失败命令数:   %llu\n", (unsigned long long)g_host_if.stats.failed_cmds);
    printf("  总读取字节:   %llu\n", (unsigned long long)g_host_if.stats.total_read_bytes);
    printf("  总写入字节:   %llu\n", (unsigned long long)g_host_if.stats.total_write_bytes);
}
