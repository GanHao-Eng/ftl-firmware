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
    performance_stats_t perf;  ///< 性能统计

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
 * @brief 获取当前时间戳（毫秒）
 * @return 时间戳
 * @note 简化实现，实际固件中使用硬件定时器
 */
static uint64_t get_timestamp_ms(void)
{
    static uint64_t counter = 0;
    return counter++;
}

/**
 * @brief 更新性能统计
 * @param[in] is_read 是否为读命令
 * @param[in] bytes 传输字节数
 * @param[in] latency_us 延迟（微秒）
 * @note 更新 IOPS、带宽、延迟等性能指标
 */
static void update_performance_stats(bool is_read, uint32_t bytes, uint64_t latency_us)
{
    performance_stats_t *perf = &g_host_if.perf;
    uint64_t now = 0;
    uint64_t elapsed = 0;

    now = get_timestamp_ms();

    /* 检查是否需要重置统计窗口（每秒重置一次） */
    if (perf->window_start_ms == 0) {
        perf->window_start_ms = now;
    }

    elapsed = now - perf->window_start_ms;
    if (elapsed >= 1000) {
        /* 计算 IOPS */
        perf->read_iops = perf->window_read_cmds;
        perf->write_iops = perf->window_write_cmds;
        perf->total_iops = perf->read_iops + perf->write_iops;

        /* 计算带宽（字节/秒） */
        perf->read_bw_bps = perf->window_read_bytes;
        perf->write_bw_bps = perf->window_write_bytes;
        perf->total_bw_bps = perf->read_bw_bps + perf->write_bw_bps;

        /* 重置窗口统计 */
        perf->window_start_ms = now;
        perf->window_read_cmds = 0;
        perf->window_write_cmds = 0;
        perf->window_read_bytes = 0;
        perf->window_write_bytes = 0;
    }

    /* 更新窗口统计 */
    if (is_read) {
        perf->window_read_cmds++;
        perf->window_read_bytes += bytes;
    } else {
        perf->window_write_cmds++;
        perf->window_write_bytes += bytes;
    }

    /* 更新延迟统计 */
    if (perf->latency_count == 0) {
        perf->min_latency_us = latency_us;
        perf->max_latency_us = latency_us;
    } else {
        if (latency_us < perf->min_latency_us) {
            perf->min_latency_us = latency_us;
        }
        if (latency_us > perf->max_latency_us) {
            perf->max_latency_us = latency_us;
        }
    }

    perf->total_latency_us += latency_us;
    perf->latency_count++;
    perf->avg_latency_us = perf->total_latency_us / perf->latency_count;
}

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
 * @note Dataset Management 命令，用于标记不再使用的数据块
 *       对应 NVMe 的 Deallocate 操作
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

/**
 * @brief 处理 Write Zeroes 命令
 * @param[in] cmd 命令指针
 * @return 状态码
 * @note 将指定范围的逻辑块写入全零数据
 *       与 TRIM 不同，Write Zeroes 实际写入数据，TRIM 只是标记无效
 */
static nvme_status_t process_write_zeroes_cmd(const nvme_cmd_t *cmd)
{
    ret_code_t ret = RET_OK;
    uint32_t lpn = 0;
    uint32_t count = 0;
    uint32_t i = 0;
    uint8_t *zero_buf = NULL;

    if (cmd == NULL) {
        return NVME_STATUS_INVALID_FIELD;
    }

    /* 计算起始 LPN 和数量 */
    lpn = (uint32_t)cmd->slba;
    count = cmd->nlb + 1;

    /* 分配零缓冲区 */
    zero_buf = (uint8_t *)calloc(NAND_PAGE_SIZE, sizeof(uint8_t));
    if (zero_buf == NULL) {
        return NVME_STATUS_INTERNAL_ERROR;
    }

    /* 循环写入每个页（全零数据） */
    for (i = 0; i < count; i++) {
        ret = ftl_write(lpn + i, zero_buf);
        if (ret != RET_OK) {
            free(zero_buf);
            return NVME_STATUS_INTERNAL_ERROR;
        }
    }

    /* 释放缓冲区 */
    free(zero_buf);

    /* 更新统计 */
    g_host_if.stats.write_cmds++;
    g_host_if.stats.total_cmds++;
    g_host_if.stats.total_write_bytes += count * NAND_PAGE_SIZE;

    return NVME_STATUS_SUCCESS;
}

/* ============================================================
 *  接口实现
 * ============================================================ */

/**
 * @brief 初始化主机接口模块
 * @param[in] config 主机接口配置指针
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误（空指针或队列大小为0）
 * @retval RET_ERR_INTERNAL 内部错误（内存分配失败）
 * @details 初始化 NVMe 主机接口，包括：
 *          1. 分配完成队列（CQ）缓冲区
 *          2. 初始化提交队列（SQ）链表
 *          3. 初始化阶段标签（Phase Tag）
 *          4. 初始化消息队列
 *          5. 重置统计信息
 */
ret_code_t host_if_init(const host_if_config_t *config)
{
    if (config == NULL) {
        return RET_ERR_PARAM;
    }
    if (config->queue_size == 0U) {
        return RET_ERR_PARAM;
    }

    /* 清空全局设备结构体 */
    memset(&g_host_if, 0, sizeof(g_host_if));

    /* 保存配置 */
    memcpy(&g_host_if.config, config, sizeof(host_if_config_t));

    /* 分配完成队列缓冲区（环形队列） */
    g_host_if.cq_buf = (nvme_cqe_t *)malloc(sizeof(nvme_cqe_t) * config->queue_size);
    if (g_host_if.cq_buf == NULL) {
        return RET_ERR_INTERNAL;
    }

    /* 初始化提交队列（链表实现） */
    g_host_if.sq_head = NULL;
    g_host_if.sq_tail = NULL;
    g_host_if.sq_count = 0U;

    /* 初始化完成队列（环形数组实现） */
    g_host_if.cq_head = 0U;
    g_host_if.cq_tail = 0U;
    g_host_if.cq_count = 0U;
    /* 阶段标签初始为1，主机侧初始为0，第一个CQE时主机检测到变化 */
    g_host_if.phase_tag = 1U;

    /* 初始化消息队列（用于与其他模块通信） */
    msg_queue_init(MODULE_HOST_IF, config->queue_size);

    g_host_if.is_initialized = true;

    return RET_OK;
}

/**
 * @brief 反初始化主机接口模块
 * @retval RET_OK 成功
 * @retval RET_ERR_NOT_INIT 未初始化
 * @details 释放所有资源，包括：
 *          1. 释放提交队列中所有未处理的命令节点
 *          2. 释放完成队列缓冲区
 *          3. 销毁消息队列
 *          4. 重置初始化状态
 */
ret_code_t host_if_deinit(void)
{
    cmd_node_t *node = NULL;
    cmd_node_t *next = NULL;

    if (!g_host_if.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 遍历并释放提交队列中的所有命令节点 */
    node = g_host_if.sq_head;
    while (node != NULL) {
        next = node->next;
        destroy_cmd_node(node);
        node = next;
    }

    /* 释放完成队列缓冲区 */
    if (g_host_if.cq_buf != NULL) {
        free(g_host_if.cq_buf);
        g_host_if.cq_buf = NULL;
    }

    /* 销毁消息队列 */
    msg_queue_deinit(MODULE_HOST_IF);

    g_host_if.is_initialized = false;

    return RET_OK;
}

/**
 * @brief 提交 NVMe 命令到提交队列
 * @param[in] cmd NVMe 命令指针
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误（空指针）
 * @retval RET_ERR_NOT_INIT 未初始化
 * @retval RET_ERR_NO_SPACE 提交队列已满
 * @retval RET_ERR_INTERNAL 内部错误（内存分配失败）
 * @details 将 NVMe 命令添加到提交队列（SQ）尾部，等待 host_if_process 处理：
 *          1. 检查队列是否已满
 *          2. 创建命令节点并复制命令数据
 *          3. 自动分配命令ID（如果未设置）
 *          4. 添加到链表尾部
 * @note 这是异步接口，命令提交后立即返回，实际处理在 host_if_process 中完成
 */
ret_code_t host_if_submit_cmd(const nvme_cmd_t *cmd)
{
    cmd_node_t *node = NULL;

    if (cmd == NULL) {
        return RET_ERR_PARAM;
    }
    if (!g_host_if.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 检查提交队列是否已满，防止溢出 */
    if (g_host_if.sq_count >= g_host_if.config.queue_size) {
        return RET_ERR_NO_SPACE;
    }

    /* 创建命令节点，复制命令数据 */
    node = create_cmd_node(cmd);
    if (node == NULL) {
        return RET_ERR_INTERNAL;
    }

    /* 如果命令未设置ID，自动分配递增的ID */
    if (node->cmd.cid == 0U) {
        node->cmd.cid = g_cid_counter++;
    }

    /* 添加到提交队列尾部（链表实现，FIFO顺序） */
    if (g_host_if.sq_head == NULL) {
        /* 队列为空，头尾都指向新节点 */
        g_host_if.sq_head = node;
        g_host_if.sq_tail = node;
    } else {
        /* 队列非空，追加到尾部 */
        g_host_if.sq_tail->next = node;
        g_host_if.sq_tail = node;
    }

    g_host_if.sq_count++;

    return RET_OK;
}

/**
 * @brief 轮询完成队列，获取已完成的命令
 * @param[out] cqe 完成队列条目输出指针
 * @retval RET_OK 成功，获取到一个完成条目
 * @retval RET_ERR_PARAM 参数错误（空指针）
 * @retval RET_ERR_NOT_INIT 未初始化
 * @retval RET_ERR_BUSY 完成队列为空，没有已完成的命令
 * @details 从完成队列（CQ）头部取出一个已完成的命令条目：
 *          1. 检查队列是否为空
 *          2. 复制完成队列条目到输出缓冲区
 *          3. 更新队列头指针（环形队列取模）
 *          4. 减少队列计数
 * @note 这是非阻塞接口，队列为空时立即返回 RET_ERR_BUSY
 *       主机驱动通过轮询此接口获取命令完成状态
 */
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

    /* 从完成队列头取出条目（环形数组实现） */
    memcpy(cqe, &g_host_if.cq_buf[g_host_if.cq_head], sizeof(nvme_cqe_t));

    /* 更新队列头指针，取模实现环形 */
    g_host_if.cq_head = (g_host_if.cq_head + 1) % g_host_if.config.queue_size;
    g_host_if.cq_count--;

    return RET_OK;
}

/**
 * @brief 处理提交队列中的所有命令
 * @retval RET_OK 成功
 * @retval RET_ERR_NOT_INIT 未初始化
 * @details 主机接口的核心处理函数，在主循环中周期性调用：
 *          1. 从提交队列（SQ）头部依次取出命令
 *          2. 根据命令操作码（opcode）分发到对应的处理函数
 *          3. 支持的命令类型：读、写、Write Zeroes、TRIM(Dataset Management)、Flush
 *          4. 模拟命令处理延迟（基础延迟+每页延迟）
 *          5. 更新性能统计（IOPS、带宽、延迟）
 *          6. 生成完成队列条目（CQE）并添加到完成队列
 *          7. 更新命令统计（成功/失败计数）
 *          8. 释放命令节点内存
 * @note 这是同步处理接口，会处理完队列中所有命令后返回
 *       实际固件中通常在中断上下文中处理单个命令
 */
ret_code_t host_if_process(void)
{
    cmd_node_t *node = NULL;
    nvme_cqe_t cqe;
    nvme_status_t status = NVME_STATUS_SUCCESS;
    uint64_t latency_us = 0;
    uint32_t page_count = 0;
    uint32_t bytes = 0;
    bool is_read = false;

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
            is_read = true;
            break;

        case NVME_CMD_WRITE:
            status = process_write_cmd(&node->cmd);
            is_read = false;
            break;

        case NVME_CMD_WRITE_ZEROES:
            status = process_write_zeroes_cmd(&node->cmd);
            is_read = false;
            break;

        case NVME_CMD_DATASET_MGMT:
            status = process_trim_cmd(&node->cmd);
            is_read = false;
            break;

        case NVME_CMD_FLUSH:
            /* Flush 命令简化处理：确保数据持久化 */
            status = NVME_STATUS_SUCCESS;
            is_read = false;
            break;

        default:
            status = NVME_STATUS_INVALID_OPCODE;
            is_read = false;
            break;
        }

        /* 计算延迟（简化模拟：基础延迟 + 每页延迟） */
        page_count = node->cmd.nlb + 1;
        latency_us = 10 + page_count * 5;  /* 基础10us + 每页5us */
        bytes = page_count * NAND_PAGE_SIZE;

        /* 更新性能统计（仅对成功的读写命令） */
        if (status == NVME_STATUS_SUCCESS &&
            (node->cmd.opcode == NVME_CMD_READ ||
             node->cmd.opcode == NVME_CMD_WRITE ||
             node->cmd.opcode == NVME_CMD_WRITE_ZEROES)) {
            update_performance_stats(is_read, bytes, latency_us);
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

/* ============================================================
 *  性能监控接口实现
 * ============================================================ */

ret_code_t host_if_get_performance_stats(performance_stats_t *stats)
{
    if (stats == NULL) {
        return RET_ERR_PARAM;
    }
    if (!g_host_if.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    memcpy(stats, &g_host_if.perf, sizeof(performance_stats_t));

    return RET_OK;
}

ret_code_t host_if_reset_performance_stats(void)
{
    if (!g_host_if.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    memset(&g_host_if.perf, 0, sizeof(performance_stats_t));
    g_host_if.perf.min_latency_us = UINT64_MAX;

    return RET_OK;
}

void host_if_print_performance_stats(void)
{
    if (!g_host_if.is_initialized) {
        printf("主机接口未初始化\n");
        return;
    }

    printf("性能统计信息:\n");
    printf("  读 IOPS:      %llu\n", (unsigned long long)g_host_if.perf.read_iops);
    printf("  写 IOPS:      %llu\n", (unsigned long long)g_host_if.perf.write_iops);
    printf("  总 IOPS:      %llu\n", (unsigned long long)g_host_if.perf.total_iops);
    printf("  读带宽:       %llu B/s\n", (unsigned long long)g_host_if.perf.read_bw_bps);
    printf("  写带宽:       %llu B/s\n", (unsigned long long)g_host_if.perf.write_bw_bps);
    printf("  总带宽:       %llu B/s\n", (unsigned long long)g_host_if.perf.total_bw_bps);
    printf("  最小延迟:     %llu us\n", (unsigned long long)g_host_if.perf.min_latency_us);
    printf("  最大延迟:     %llu us\n", (unsigned long long)g_host_if.perf.max_latency_us);
    printf("  平均延迟:     %llu us\n", (unsigned long long)g_host_if.perf.avg_latency_us);
}
