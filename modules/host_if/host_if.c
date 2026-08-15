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

    /* NVMe 企业级特性状态 */
    nvme_id_ctrl_t id_ctrl;    ///< Identify Controller 数据
    nvme_id_ns_t id_ns;        ///< Identify Namespace 数据
    nvme_smart_log_t smart_log;///< SMART/健康日志

    /* Feature 当前值 */
    uint32_t feature_arbitration;    ///< 仲裁特性
    uint32_t feature_power_mgmt;     ///< 电源管理特性
    uint32_t feature_temp_threshold; ///< 温度阈值
    uint32_t feature_error_recovery; ///< 错误恢复
    uint32_t feature_volatile_wc;    ///< 易失性写缓存
    uint32_t feature_num_queues;     ///< 队列数
    uint32_t feature_irq_coalesce;   ///< 中断聚合
    uint32_t feature_write_atomicity;///< 写原子性
    uint32_t feature_async_event;    ///< 异步事件配置
    uint32_t feature_auto_pst;       ///< 自动电源状态转换
    uint32_t feature_keep_alive;     ///< 保持活动
    uint32_t feature_hctm;           ///< 主机控制热管理

    /* 断电保护状态 */
    bool plp_enabled;          ///< 断电保护使能
    bool wal_dirty;            ///< WAL日志是否有未恢复数据
    uint64_t power_cycles;     ///< 上电循环次数
    uint64_t unsafe_shutdowns; ///< 不安全关机次数
    uint64_t power_on_hours;   ///< 上电时间（小时）

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

/* ============================================================
 *  NVMe Admin 命令处理
 * ============================================================ */

/**
 * @brief 填充 Identify Controller 数据
 * @details 按照 NVMe 规范填充控制器识别数据结构，包含厂商信息、能力、电源状态等
 */
static void host_if_fill_id_ctrl(void)
{
    nvme_id_ctrl_t *id = &g_host_if.id_ctrl;

    memset(id, 0, sizeof(nvme_id_ctrl_t));

    /* 厂商信息 */
    id->vid = 0x1EDC;       /* 厂商ID（示例） */
    id->ssvid = 0x1EDC;     /* 子系统厂商ID */
    memcpy(id->sn, "FTLFW20260001", 14);  /* 序列号 */
    memcpy(id->mn, "FTL-Firmware NVMe SSD", 22);  /* 型号 */
    memcpy(id->fr, "1.0.0", 5);  /* 固件版本 */
    id->ieee[0] = 0x00;
    id->ieee[1] = 0x0B;
    id->ieee[2] = 0xBA;

    /* 控制器能力 */
    id->cntlid = 0x0001;    /* 控制器ID */
    id->ver = 0x00020000;   /* NVMe 2.0 */
    id->mdts = 6;           /* 最大数据传输大小（2^6 * 4KB = 256KB） */
    id->cmic = 0;           /* 单端口 */
    id->rab = 4;            /* 推荐仲裁突发 */

    /* 管理能力 */
    id->oacs = 0x003F;      /* 支持固件更新、格式化、安全等Admin命令 */
    id->acl = 3;            /* 中止命令限制 */
    id->aerl = 3;           /* 异步事件请求限制 */
    id->frmw = 0x03;        /* 支持固件激活，支持7个插槽 */
    id->lpa = 0x07;         /* 支持SMART日志、固件插槽日志、命令效果日志 */
    id->elpe = 63;          /* 错误日志条目数（64个） */
    id->npss = 4;           /* 电源状态数（5个：0-4） */
    id->wctemp = 345;       /* 警告温度阈值（72摄氏度 = 345K） */
    id->cctemp = 358;       /* 临界温度阈值（85摄氏度 = 358K） */
    id->mtfa = 100;         /* 最大固件激活时间（100ms） */
    id->hmpre = 0;          /* 主机内存缓冲区首选大小 */
    id->hmmin = 0;          /* 主机内存缓冲区最小大小 */

    /* 总NVM容量（示例：100GB） */
    uint64_t total_cap = 100ULL * 1024 * 1024 * 1024;
    memcpy(id->tnvmcap, &total_cap, 8);
    memcpy(id->unvmcap, &total_cap, 8);

    /* 命令集能力 */
    id->sqes = 0x66;        /* SQ条目大小：最小6，最大6（64字节） */
    id->cqes = 0x44;        /* CQ条目大小：最小4，最大4（16字节） */
    id->maxcmd = 1024;      /* 最大命令数 */
    id->nn = 1;             /* 命名空间数 */
    id->oncs = 0x003F;      /* 支持Compare、Write Zeroes、Dataset Mgmt、Verify等 */
    id->fuses = 0x0001;     /* 支持Compare和Write融合 */
    id->fna = 0x03;         /* 支持格式化、安全擦除 */
    id->vwc = 0x01;         /* 存在易失性写缓存 */
    id->awun = 0;           /* 原子写单位NVM */
    id->awupf = 0;          /* 原子写单位电源故障 */
    id->sgls = 0;           /* 不支持SGL（仅PRP） */
    id->mnan = 1;           /* 最大命名空间数 */

    /* 电源状态描述符（简化版） */
    /* PS0：最大性能 */
    id->psd[0][0] = 0x01;   /* 最大PS */
    *(uint16_t *)&id->psd[0][2] = 10000;  /* 最大功耗（10W，单位0.01W） */
    *(uint32_t *)&id->psd[0][8] = 0;      /* 进入延迟 */
    *(uint32_t *)&id->psd[0][12] = 0;     /* 退出延迟 */
    /* PS3：低功耗 */
    id->psd[3][0] = 0x03;
    *(uint16_t *)&id->psd[3][2] = 500;    /* 最大功耗（0.5W） */
    *(uint32_t *)&id->psd[3][8] = 5000;   /* 进入延迟（5ms） */
    *(uint32_t *)&id->psd[3][12] = 5000;  /* 退出延迟（5ms） */
    /* PS4：最深睡眠 */
    id->psd[4][0] = 0x04;
    *(uint16_t *)&id->psd[4][2] = 50;     /* 最大功耗（0.05W） */
    *(uint32_t *)&id->psd[4][8] = 50000;  /* 进入延迟（50ms） */
    *(uint32_t *)&id->psd[4][12] = 100000;/* 退出延迟（100ms） */
}

/**
 * @brief 填充 Identify Namespace 数据
 * @details 按照 NVMe 规范填充命名空间识别数据结构
 */
static void host_if_fill_id_ns(void)
{
    nvme_id_ns_t *ns = &g_host_if.id_ns;

    memset(ns, 0, sizeof(nvme_id_ns_t));

    /* 命名空间大小（示例：100GB，以4KB LBA为单位） */
    uint64_t lba_count = (100ULL * 1024 * 1024 * 1024) / 4096;
    ns->nsze = lba_count;
    ns->ncap = lba_count;
    ns->nuse = lba_count / 2;  /* 假设已使用一半 */

    /* 命名空间特性 */
    ns->nsfeat = 0x0F;    /* 支持薄配置、原子写、命名空间写入保护、UUID */
    ns->nlbaf = 3;        /* 支持4种LBA格式 */
    ns->flbas = 0x00;     /* 使用LBA格式0（4KB，无元数据） */
    ns->mc = 0x03;        /* 支持元数据作为数据一部分和单独元数据 */
    ns->dpc = 0x0F;       /* 支持所有端到端数据保护类型 */
    ns->dps = 0x00;       /* 当前未启用端到端保护 */
    ns->nmic = 0x01;      /* 支持多路径I/O */
    ns->rescap = 0x3F;    /* 支持所有预留类型 */
    ns->fpi = 0x80;       /* 支持格式化进度指示 */

    /* 原子写单位 */
    ns->nawun = 0;
    ns->nawupf = 0;
    ns->nacwu = 0;

    /* 最佳I/O参数 */
    ns->noiob = 128;      /* 最佳I/O边界（128个LBA = 512KB） */
    ns->npwg = 128;       /* 最佳写粒度 */
    ns->npwa = 128;       /* 最佳写对齐 */
    ns->nows = 128;       /* 最佳写大小 */

    /* NVM容量 */
    uint64_t nvm_cap = 100ULL * 1024 * 1024 * 1024;
    ns->nvmcap[0] = nvm_cap;
    ns->nvmcap[1] = 0;

    /* LBA格式定义 */
    /* LBAF 0：4KB数据，0元数据，相对性能最佳 */
    ns->lbaf[0][0] = 12;  /* LBA数据大小 = 2^12 = 4096 */
    ns->lbaf[0][1] = 0;   /* 元数据大小 */
    ns->lbaf[0][3] = 0;   /* 相对性能（0=最佳） */
    /* LBAF 1：512B数据，0元数据 */
    ns->lbaf[1][0] = 9;   /* 2^9 = 512 */
    ns->lbaf[1][3] = 2;   /* 相对性能较差 */
    /* LBAF 2：4KB数据，8B元数据（DIF） */
    ns->lbaf[2][0] = 12;
    ns->lbaf[2][1] = 8;   /* 8字节元数据（保护信息） */
    ns->lbaf[2][3] = 1;
    /* LBAF 3：4KB数据，64B元数据 */
    ns->lbaf[3][0] = 12;
    ns->lbaf[3][1] = 64;
    ns->lbaf[3][3] = 3;

    /* Namespace GUID */
    ns->nguid[0] = 0x1234;
    ns->nguid[1] = 0x5678;
    ns->nguid[2] = 0x9ABC;
    ns->nguid[3] = 0xDEF0;
    ns->nguid[4] = 0x1111;
    ns->nguid[5] = 0x2222;
    ns->nguid[6] = 0x3333;
    ns->nguid[7] = 0x4444;

    /* EUI-64 */
    ns->eui64[0] = 0x00;
    ns->eui64[1] = 0x0B;
    ns->eui64[2] = 0xBA;
    ns->eui64[3] = 0x00;
    ns->eui64[4] = 0x00;
    ns->eui64[5] = 0x00;
    ns->eui64[6] = 0x00;
    ns->eui64[7] = 0x01;

    /* ANA组ID */
    ns->anagrpid = 1;
    ns->nsattr = 0;       /* 命名空间属性：非共享 */
    ns->nvmsetid = 0;     /* NVM Set ID */
    ns->endgid = 0;       /* Endurance Group ID */
}

/**
 * @brief 填充 SMART/健康日志数据
 * @details 从NAND模块和统计信息中收集健康数据，填充SMART日志结构
 */
static void host_if_fill_smart_log(void)
{
    nvme_smart_log_t *smart = &g_host_if.smart_log;
    nand_stats_t nand_stats;

    memset(smart, 0, sizeof(nvme_smart_log_t));

    /* 获取NAND统计信息 */
    nand_get_stats(&nand_stats);

    /* 严重警告 */
    smart->critical_warning = 0;
    /* 检查可用备用空间 */
    uint32_t bad_blocks = nand_stats.bad_blocks;
    uint32_t total_blocks = NAND_TOTAL_BLOCKS;
    uint8_t avail_spare = (uint8_t)((total_blocks - bad_blocks) * 100 / total_blocks);
    smart->avail_spare = avail_spare;
    smart->spare_thresh = 10;  /* 阈值10% */
    if (avail_spare < smart->spare_thresh) {
        smart->critical_warning |= 0x01;  /* 可用空间低 */
    }

    /* 温度（从管理模块获取，示例：45摄氏度 = 318K） */
    smart->temperature = 318;  /* 45°C + 273 */
    if (smart->temperature > g_host_if.id_ctrl.wctemp) {
        smart->critical_warning |= 0x02;  /* 温度过高 */
    }

    /* 寿命估算（简化：固定值，实际应基于NAND擦写次数统计） */
    smart->percent_used = 5;  /* 已使用5%寿命 */
    if (smart->percent_used > 90) {
        smart->critical_warning |= 0x04;  /* 可靠性降级 */
    }

    /* 数据单元统计（每512KB为1个单位） */
    uint64_t data_units_read = g_host_if.stats.total_read_bytes / (512 * 1024);
    uint64_t data_units_written = g_host_if.stats.total_write_bytes / (512 * 1024);
    memcpy(smart->data_units_read, &data_units_read, 8);
    memcpy(smart->data_units_written, &data_units_written, 8);

    /* 主机命令数 */
    uint64_t read_cmds = g_host_if.stats.read_cmds;
    uint64_t write_cmds = g_host_if.stats.write_cmds;
    memcpy(smart->host_read_cmds, &read_cmds, 8);
    memcpy(smart->host_write_cmds, &write_cmds, 8);

    /* 控制器忙碌时间（简化：假设10分钟） */
    uint64_t busy_time = 10;
    memcpy(smart->ctrl_busy_time, &busy_time, 8);

    /* 上电循环次数 */
    memcpy(smart->power_cycles, &g_host_if.power_cycles, 8);

    /* 上电时间 */
    memcpy(smart->power_on_hours, &g_host_if.power_on_hours, 8);

    /* 不安全关机次数 */
    memcpy(smart->unsafe_shutdowns, &g_host_if.unsafe_shutdowns, 8);

    /* 媒体错误数 */
    uint64_t media_errors = nand_stats.crc_error_count;
    memcpy(smart->media_errors, &media_errors, 8);

    /* 错误日志条目数 */
    uint64_t err_logs = g_host_if.stats.failed_cmds;
    memcpy(smart->num_err_log_entries, &err_logs, 8);

    /* 温度传感器 */
    smart->temp_sensor[0] = 318;  /* 传感器1：45°C */
    smart->temp_sensor[1] = 320;  /* 传感器2：47°C */
    smart->temp_sensor[2] = 315;  /* 传感器3：42°C */

    /* 温度统计时间 */
    smart->warning_temp_time = 0;
    smart->critical_temp_time = 0;
}

/**
 * @brief 处理 Identify 命令
 * @param[in] cmd NVMe命令指针
 * @return NVMe状态码
 * @details 根据CNS字段返回Controller或Namespace识别数据
 */
static nvme_status_t host_if_process_identify(const nvme_cmd_t *cmd)
{
    uint8_t cns = (uint8_t)(cmd->nsid & 0xFF);  /* CNS在nsid字段的低字节 */

    if (cmd->data_buf == NULL || cmd->data_len < 4096) {
        return NVME_STATUS_INVALID_FIELD;
    }

    switch (cns) {
        case NVME_IDENTIFY_CNS_CONTROLLER:
            /* 返回Identify Controller数据 */
            memcpy(cmd->data_buf, &g_host_if.id_ctrl, sizeof(nvme_id_ctrl_t));
            break;

        case NVME_IDENTIFY_CNS_NAMESPACE:
            /* 返回Identify Namespace数据 */
            memcpy(cmd->data_buf, &g_host_if.id_ns, sizeof(nvme_id_ns_t));
            break;

        case NVME_IDENTIFY_CNS_NS_LIST:
            /* 返回命名空间列表（简化：只有NSID=1） */
            memset(cmd->data_buf, 0, 4096);
            ((uint32_t *)cmd->data_buf)[0] = 1;
            break;

        default:
            return NVME_STATUS_INVALID_FIELD;
    }

    return NVME_STATUS_SUCCESS;
}

/**
 * @brief 处理 Get Log Page 命令
 * @param[in] cmd NVMe命令指针
 * @return NVMe状态码
 * @details 根据Log Page ID返回对应的日志数据
 */
static nvme_status_t host_if_process_get_log_page(const nvme_cmd_t *cmd)
{
    uint8_t log_id = (uint8_t)(cmd->nsid & 0xFF);  /* Log ID在nsid字段的低字节 */

    if (cmd->data_buf == NULL) {
        return NVME_STATUS_INVALID_FIELD;
    }

    switch (log_id) {
        case NVME_LOG_SMART:
            /* 更新并返回SMART/健康日志 */
            host_if_fill_smart_log();
            memcpy(cmd->data_buf, &g_host_if.smart_log,
                   (cmd->data_len < sizeof(nvme_smart_log_t)) ?
                   cmd->data_len : sizeof(nvme_smart_log_t));
            break;

        case NVME_LOG_ERROR:
            /* 错误信息日志（简化：返回空） */
            memset(cmd->data_buf, 0, cmd->data_len);
            break;

        case NVME_LOG_FW_SLOT:
            /* 固件插槽信息日志（简化：只有插槽1有效） */
            memset(cmd->data_buf, 0, cmd->data_len);
            ((uint8_t *)cmd->data_buf)[0] = 0x01;  /* 插槽1当前激活 */
            memcpy(&((uint8_t *)cmd->data_buf)[8], "1.0.0", 5);  /* 插槽1版本 */
            break;

        case NVME_LOG_CMD_EFFECTS:
            /* 命令效果日志（简化：返回全0） */
            memset(cmd->data_buf, 0, cmd->data_len);
            break;

        default:
            return NVME_STATUS_INVALID_FIELD;
    }

    return NVME_STATUS_SUCCESS;
}

/**
 * @brief 处理 Set Feature 命令
 * @param[in] cmd NVMe命令指针
 * @return NVMe状态码
 * @details 设置控制器特性值，保存到私有数据中
 */
static nvme_status_t host_if_process_set_feature(const nvme_cmd_t *cmd)
{
    uint8_t feature_id = (uint8_t)(cmd->nsid & 0xFF);  /* Feature ID在nsid字段低字节 */
    uint32_t value = (uint32_t)(cmd->slba & 0xFFFFFFFF);  /* 特性值在slba字段 */

    switch (feature_id) {
        case NVME_FEAT_ARBITRATION:
            g_host_if.feature_arbitration = value;
            break;
        case NVME_FEAT_POWER_MGMT:
            g_host_if.feature_power_mgmt = value;
            break;
        case NVME_FEAT_TEMP_THRESHOLD:
            g_host_if.feature_temp_threshold = value;
            break;
        case NVME_FEAT_ERROR_RECOVERY:
            g_host_if.feature_error_recovery = value;
            break;
        case NVME_FEAT_VOLATILE_WC:
            g_host_if.feature_volatile_wc = value;
            break;
        case NVME_FEAT_NUM_QUEUES:
            g_host_if.feature_num_queues = value;
            break;
        case NVME_FEAT_IRQ_COALESCE:
            g_host_if.feature_irq_coalesce = value;
            break;
        case NVME_FEAT_WRITE_ATOMICITY:
            g_host_if.feature_write_atomicity = value;
            break;
        case NVME_FEAT_ASYNC_EVENT:
            g_host_if.feature_async_event = value;
            break;
        case NVME_FEAT_AUTO_PST:
            g_host_if.feature_auto_pst = value;
            break;
        case NVME_FEAT_KEEP_ALIVE:
            g_host_if.feature_keep_alive = value;
            break;
        case NVME_FEAT_HCTM:
            g_host_if.feature_hctm = value;
            break;
        default:
            return NVME_STATUS_INVALID_FIELD;
    }

    return NVME_STATUS_SUCCESS;
}

/**
 * @brief 处理 Get Feature 命令
 * @param[in] cmd NVMe命令指针
 * @return NVMe状态码
 * @details 获取控制器特性值，返回到data_buf中
 */
static nvme_status_t host_if_process_get_feature(const nvme_cmd_t *cmd)
{
    uint8_t feature_id = (uint8_t)(cmd->nsid & 0xFF);
    uint32_t value = 0;

    switch (feature_id) {
        case NVME_FEAT_ARBITRATION:
            value = g_host_if.feature_arbitration;
            break;
        case NVME_FEAT_POWER_MGMT:
            value = g_host_if.feature_power_mgmt;
            break;
        case NVME_FEAT_TEMP_THRESHOLD:
            value = g_host_if.feature_temp_threshold;
            break;
        case NVME_FEAT_ERROR_RECOVERY:
            value = g_host_if.feature_error_recovery;
            break;
        case NVME_FEAT_VOLATILE_WC:
            value = g_host_if.feature_volatile_wc;
            break;
        case NVME_FEAT_NUM_QUEUES:
            value = g_host_if.feature_num_queues;
            break;
        case NVME_FEAT_IRQ_COALESCE:
            value = g_host_if.feature_irq_coalesce;
            break;
        case NVME_FEAT_WRITE_ATOMICITY:
            value = g_host_if.feature_write_atomicity;
            break;
        case NVME_FEAT_ASYNC_EVENT:
            value = g_host_if.feature_async_event;
            break;
        case NVME_FEAT_AUTO_PST:
            value = g_host_if.feature_auto_pst;
            break;
        case NVME_FEAT_KEEP_ALIVE:
            value = g_host_if.feature_keep_alive;
            break;
        case NVME_FEAT_HCTM:
            value = g_host_if.feature_hctm;
            break;
        default:
            return NVME_STATUS_INVALID_FIELD;
    }

    if (cmd->data_buf != NULL && cmd->data_len >= 4) {
        *(uint32_t *)cmd->data_buf = value;
    }

    return NVME_STATUS_SUCCESS;
}

/**
 * @brief 处理 Admin 命令
 * @param[in] cmd NVMe命令指针
 * @return NVMe状态码
 * @details Admin命令总入口，分发到具体处理函数
 */
static nvme_status_t host_if_process_admin_cmd(const nvme_cmd_t *cmd)
{
    switch (cmd->opcode) {
        case NVME_ADMIN_IDENTIFY:
            return host_if_process_identify(cmd);

        case NVME_ADMIN_GET_LOG_PAGE:
            return host_if_process_get_log_page(cmd);

        case NVME_ADMIN_SET_FEATURE:
            return host_if_process_set_feature(cmd);

        case NVME_ADMIN_GET_FEATURE:
            return host_if_process_get_feature(cmd);

        case NVME_ADMIN_CREATE_SQ:
        case NVME_ADMIN_DELETE_SQ:
        case NVME_ADMIN_CREATE_CQ:
        case NVME_ADMIN_DELETE_CQ:
            /* 队列管理命令，简化处理：直接成功 */
            return NVME_STATUS_SUCCESS;

        case NVME_ADMIN_ABORT:
            /* 中止命令，简化处理 */
            return NVME_STATUS_SUCCESS;

        case NVME_ADMIN_ASYNC_EVENT:
            /* 异步事件请求，简化处理：暂不支持 */
            return NVME_STATUS_SUCCESS;

        case NVME_ADMIN_FW_DOWNLOAD:
        case NVME_ADMIN_FW_ACTIVATE:
            /* 固件更新命令，简化处理 */
            return NVME_STATUS_SUCCESS;

        case NVME_ADMIN_FORMAT_NVM:
            /* 格式化NVM命令，简化处理 */
            return NVME_STATUS_SUCCESS;

        default:
            return NVME_STATUS_INVALID_OPCODE;
    }
}

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

    /* 初始化 NVMe Identify 数据 */
    host_if_fill_id_ctrl();
    host_if_fill_id_ns();

    /* 初始化 Feature 默认值 */
    g_host_if.feature_arbitration = 0x00000001;  /* 默认仲裁 */
    g_host_if.feature_power_mgmt = 0x00000000;   /* PS0状态 */
    g_host_if.feature_temp_threshold = 345;       /* 72°C警告阈值 */
    g_host_if.feature_error_recovery = 0x00000000;/* 默认错误恢复 */
    g_host_if.feature_volatile_wc = 0x00000001;  /* 写缓存使能 */
    g_host_if.feature_num_queues = 0x0000FFFF;    /* 最大队列数 */
    g_host_if.feature_irq_coalesce = 0x00000000; /* 无中断聚合 */
    g_host_if.feature_write_atomicity = 0x00000000;
    g_host_if.feature_async_event = 0x00000000;
    g_host_if.feature_auto_pst = 0x00000000;
    g_host_if.feature_keep_alive = 0x00000000;
    g_host_if.feature_hctm = 0x00000000;

    /* 初始化断电保护状态 */
    g_host_if.plp_enabled = true;
    g_host_if.wal_dirty = false;
    g_host_if.power_cycles = 1;
    g_host_if.unsafe_shutdowns = 0;
    g_host_if.power_on_hours = 0;

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
        if (node->cmd.is_admin) {
            /* Admin 命令处理 */
            status = host_if_process_admin_cmd(&node->cmd);
            is_read = false;
        } else {
            /* I/O 命令处理 */
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

/* ============================================================
 *  NVMe Admin 命令对外接口实现
 * ============================================================ */

ret_code_t host_if_get_id_ctrl(nvme_id_ctrl_t *id_ctrl)
{
    if (id_ctrl == NULL) {
        return RET_ERR_PARAM;
    }
    if (!g_host_if.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    memcpy(id_ctrl, &g_host_if.id_ctrl, sizeof(nvme_id_ctrl_t));
    return RET_OK;
}

ret_code_t host_if_get_id_ns(nvme_id_ns_t *id_ns)
{
    if (id_ns == NULL) {
        return RET_ERR_PARAM;
    }
    if (!g_host_if.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    memcpy(id_ns, &g_host_if.id_ns, sizeof(nvme_id_ns_t));
    return RET_OK;
}

ret_code_t host_if_get_smart_log(nvme_smart_log_t *smart_log)
{
    if (smart_log == NULL) {
        return RET_ERR_PARAM;
    }
    if (!g_host_if.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 更新并返回SMART日志 */
    host_if_fill_smart_log();
    memcpy(smart_log, &g_host_if.smart_log, sizeof(nvme_smart_log_t));
    return RET_OK;
}

void host_if_print_smart_info(void)
{
    nvme_smart_log_t smart;

    if (!g_host_if.is_initialized) {
        printf("主机接口未初始化\n");
        return;
    }

    host_if_fill_smart_log();
    memcpy(&smart, &g_host_if.smart_log, sizeof(nvme_smart_log_t));

    printf("=== NVMe SMART/健康信息 ===\n");
    printf("  严重警告:     0x%02X\n", smart.critical_warning);
    if (smart.critical_warning & 0x01) printf("    - 可用空间低\n");
    if (smart.critical_warning & 0x02) printf("    - 温度过高\n");
    if (smart.critical_warning & 0x04) printf("    - 可靠性降级\n");
    if (smart.critical_warning & 0x08) printf("    - 只读模式\n");
    if (smart.critical_warning & 0x10) printf("    - 易失性内存备份失败\n");

    printf("  当前温度:     %u °C (%u K)\n",
           smart.temperature - 273, smart.temperature);
    printf("  可用备用空间: %u%%\n", smart.avail_spare);
    printf("  备用阈值:     %u%%\n", smart.spare_thresh);
    printf("  寿命已使用:   %u%%\n", smart.percent_used);

    /* 读取128位统计值的低64位 */
    uint64_t data_read = 0, data_written = 0;
    uint64_t read_cmds = 0, write_cmds = 0;
    uint64_t power_cycles = 0, power_hours = 0;
    uint64_t unsafe_shutdowns = 0, media_errors = 0;
    uint64_t err_logs = 0;

    memcpy(&data_read, smart.data_units_read, 8);
    memcpy(&data_written, smart.data_units_written, 8);
    memcpy(&read_cmds, smart.host_read_cmds, 8);
    memcpy(&write_cmds, smart.host_write_cmds, 8);
    memcpy(&power_cycles, smart.power_cycles, 8);
    memcpy(&power_hours, smart.power_on_hours, 8);
    memcpy(&unsafe_shutdowns, smart.unsafe_shutdowns, 8);
    memcpy(&media_errors, smart.media_errors, 8);
    memcpy(&err_logs, smart.num_err_log_entries, 8);

    printf("  数据读取:     %llu 单位 (每单位512KB)\n", (unsigned long long)data_read);
    printf("  数据写入:     %llu 单位 (每单位512KB)\n", (unsigned long long)data_written);
    printf("  读命令数:     %llu\n", (unsigned long long)read_cmds);
    printf("  写命令数:     %llu\n", (unsigned long long)write_cmds);
    printf("  上电循环:     %llu\n", (unsigned long long)power_cycles);
    printf("  上电时间:     %llu 小时\n", (unsigned long long)power_hours);
    printf("  不安全关机:   %llu\n", (unsigned long long)unsafe_shutdowns);
    printf("  媒体错误数:   %llu\n", (unsigned long long)media_errors);
    printf("  错误日志数:   %llu\n", (unsigned long long)err_logs);

    printf("  温度传感器1:  %u °C\n", smart.temp_sensor[0] - 273);
    printf("  温度传感器2:  %u °C\n", smart.temp_sensor[1] - 273);
    printf("  温度传感器3:  %u °C\n", smart.temp_sensor[2] - 273);
    printf("============================\n");
}

/* ============================================================
 *  断电保护（PLP）实现
 * ============================================================ */

ret_code_t host_if_plp_simulate_power_loss(void)
{
    if (!g_host_if.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    printf("[PLP] 模拟断电事件...\n");

    /* 标记WAL日志为脏（表示有未完成的写入需要恢复） */
    g_host_if.wal_dirty = true;

    /* 增加不安全关机计数 */
    g_host_if.unsafe_shutdowns++;

    printf("[PLP] 断电保护已触发，WAL日志已标记为脏\n");
    printf("[PLP] 不安全关机次数: %llu\n",
           (unsigned long long)g_host_if.unsafe_shutdowns);

    return RET_OK;
}

ret_code_t host_if_plp_recovery(void)
{
    if (!g_host_if.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    printf("[PLP] 开始断电恢复...\n");

    if (!g_host_if.wal_dirty) {
        printf("[PLP] WAL日志干净，无需恢复\n");
        return RET_OK;
    }

    /* 模拟WAL日志恢复过程 */
    printf("[PLP] 正在从WAL日志恢复未完成的写入...\n");

    /* 模拟恢复步骤 */
    printf("[PLP]   1. 扫描WAL日志...\n");
    printf("[PLP]   2. 验证日志条目校验和...\n");
    printf("[PLP]   3. 重放已提交但未刷盘的事务...\n");
    printf("[PLP]   4. 回滚未完成的事务...\n");
    printf("[PLP]   5. 更新FTL映射表...\n");
    printf("[PLP]   6. 清理WAL日志...\n");

    /* 恢复完成，清除脏标记 */
    g_host_if.wal_dirty = false;

    /* 增加上电循环计数 */
    g_host_if.power_cycles++;

    printf("[PLP] 断电恢复完成，数据一致性已验证\n");
    printf("[PLP] 上电循环次数: %llu\n",
           (unsigned long long)g_host_if.power_cycles);

    return RET_OK;
}

ret_code_t host_if_get_power_status(uint64_t *power_cycles,
                                     uint64_t *unsafe_shutdowns,
                                     uint64_t *power_on_hours)
{
    if (!g_host_if.is_initialized) {
        return RET_ERR_NOT_INIT;
    }

    if (power_cycles != NULL) {
        *power_cycles = g_host_if.power_cycles;
    }
    if (unsafe_shutdowns != NULL) {
        *unsafe_shutdowns = g_host_if.unsafe_shutdowns;
    }
    if (power_on_hours != NULL) {
        *power_on_hours = g_host_if.power_on_hours;
    }

    return RET_OK;
}

/* ============================================================
 *  端到端数据保护（DIF/DIX）实现
 * ============================================================ */

/**
 * @brief DIF 模块全局状态
 */
static struct {
    dif_config_t config;     ///< DIF配置
    dif_stats_t stats;       ///< DIF错误统计
    bool is_initialized;     ///< 初始化标志
} g_dif = {0};

/**
 * @brief CRC-16 查找表（T10 PI标准，多项式0x8BB7）
 */
static const uint16_t crc16_t10pi_table[256] = {
    0x0000, 0x8BB7, 0x9CD9, 0x176E, 0xB205, 0x39B2, 0x2EDC, 0xA56B,
    0xEFBD, 0x640A, 0x7364, 0xF8D3, 0x5DB8, 0xD60F, 0xC161, 0x4AD6,
    0x54CD, 0xDF7A, 0xC814, 0x43A3, 0xE6C8, 0x6D7F, 0x7A11, 0xF1A6,
    0xBB70, 0x30C7, 0x27A9, 0xAC1E, 0x0975, 0x82C2, 0x95AC, 0x1E1B,
    0xA99A, 0x222D, 0x3543, 0xBEF4, 0x1B9F, 0x9028, 0x8746, 0x0CF1,
    0x4627, 0xCD90, 0xDAFE, 0x5149, 0xF422, 0x7F95, 0x68FB, 0xE34C,
    0xFD57, 0x76E0, 0x618E, 0xEA39, 0x4F52, 0xC4E5, 0xD38B, 0x583C,
    0x12EA, 0x995D, 0x8E33, 0x0584, 0xA0EF, 0x2B58, 0x3C36, 0xB781,
    0x883D, 0x038A, 0x14E4, 0x9F53, 0x3A38, 0xB18F, 0xA6E1, 0x2D56,
    0x6780, 0xEC37, 0xFB59, 0x70EE, 0xD585, 0x5E32, 0x495C, 0xC2EB,
    0xDCF0, 0x5747, 0x4029, 0xCB9E, 0x6EF5, 0xE542, 0xF22C, 0x799B,
    0x334D, 0xB8FA, 0xAF94, 0x2423, 0x8148, 0x0AFF, 0x1D91, 0x9626,
    0x21A7, 0xAA10, 0xBD7E, 0x36C9, 0x93A2, 0x1815, 0x0F7B, 0x84CC,
    0xCE1A, 0x45AD, 0x52C3, 0xD974, 0x7C1F, 0xF7A8, 0xE0C6, 0x6B71,
    0x756A, 0xFEDD, 0xE9B3, 0x6204, 0xC76F, 0x4CD8, 0x5BB6, 0xD001,
    0x9AD7, 0x1160, 0x060E, 0x8DB9, 0x28D2, 0xA365, 0xB40B, 0x3FBC,
    0x1083, 0x9B34, 0x8C5A, 0x07ED, 0xA286, 0x2931, 0x3E5F, 0xB5E8,
    0xFF3E, 0x7489, 0x63E7, 0xE850, 0x4D3B, 0xC68C, 0xD1E2, 0x5A55,
    0x444E, 0xCFF9, 0xD897, 0x5320, 0xF64B, 0x7DFC, 0x6A92, 0xE125,
    0xABF3, 0x2044, 0x372A, 0xBC9D, 0x19F6, 0x9241, 0x852F, 0x0E98,
    0xB919, 0x32AE, 0x25C0, 0xAE77, 0x0B1C, 0x80AB, 0x97C5, 0x1C72,
    0x56A4, 0xDD13, 0xCA7D, 0x41CA, 0xE4A1, 0x6F16, 0x7878, 0xF3CF,
    0xEDD4, 0x6663, 0x710D, 0xFABA, 0x5FD1, 0xD466, 0xC308, 0x48BF,
    0x0269, 0x89DE, 0x9EB0, 0x1507, 0xB06C, 0x3BDB, 0x2CB5, 0xA702,
    0x98BE, 0x1309, 0x0467, 0x8FD0, 0x2ABB, 0xA10C, 0xB662, 0x3DD5,
    0x7703, 0xFCB4, 0xEBDA, 0x606D, 0xC506, 0x4EB1, 0x59DF, 0xD268,
    0xCC73, 0x47C4, 0x50AA, 0xDB1D, 0x7E76, 0xF5C1, 0xE2AF, 0x6918,
    0x23CE, 0xA879, 0xBF17, 0x34A0, 0x91CB, 0x1A7C, 0x0D12, 0x86A5,
    0x3124, 0xBA93, 0xADFD, 0x264A, 0x8321, 0x0896, 0x1FF8, 0x944F,
    0xDE99, 0x552E, 0x4240, 0xC9F7, 0x6C9C, 0xE72B, 0xF045, 0x7BF2,
    0x65E9, 0xEE5E, 0xF930, 0x7287, 0xD7EC, 0x5C5B, 0x4B35, 0xC082,
    0x8A54, 0x01E3, 0x168D, 0x9D3A, 0x3851, 0xB3E6, 0xA488, 0x2F3F
};

uint16_t dif_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFF;  /* 初始值 */

    if (data == NULL || len == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < len; i++) {
        crc = (crc << 8) ^ crc16_t10pi_table[((crc >> 8) ^ data[i]) & 0xFF];
    }

    return crc;
}

ret_code_t dif_init(const dif_config_t *config)
{
    if (config == NULL) {
        return RET_ERR_PARAM;
    }

    memcpy(&g_dif.config, config, sizeof(dif_config_t));
    memset(&g_dif.stats, 0, sizeof(dif_stats_t));
    g_dif.is_initialized = true;

    printf("[DIF] 端到端数据保护初始化完成: 类型=%d, CRC校验=%s, 应用标签校验=%s, 参考标签校验=%s\n",
           config->type,
           config->guard_check_enable ? "启用" : "禁用",
           config->app_tag_check_enable ? "启用" : "禁用",
           config->ref_tag_check_enable ? "启用" : "禁用");

    return RET_OK;
}

ret_code_t dif_generate(const uint8_t *data, uint32_t data_len,
                        uint64_t lba, dif_protection_t *protection)
{
    if (data == NULL || protection == NULL || data_len == 0) {
        return RET_ERR_PARAM;
    }
    if (!g_dif.is_initialized) {
        return RET_ERR_NOT_INIT;
    }
    if (g_dif.config.type == DIF_TYPE_DISABLED) {
        return RET_ERR_NOT_SUPPORT;
    }

    uint32_t lba_count = data_len / NAND_PAGE_SIZE;
    if (lba_count == 0) {
        lba_count = 1;
    }

    for (uint32_t i = 0; i < lba_count; i++) {
        const uint8_t *lba_data = data + i * NAND_PAGE_SIZE;
        uint32_t lba_data_len = (i == lba_count - 1) ?
                                (data_len - i * NAND_PAGE_SIZE) : NAND_PAGE_SIZE;

        /* 计算CRC */
        protection[i].crc = dif_crc16(lba_data, lba_data_len);

        /* 设置应用标签 */
        protection[i].app_tag = g_dif.config.app_tag;

        /* 设置参考标签 */
        switch (g_dif.config.type) {
            case DIF_TYPE1:
                protection[i].ref_tag = (uint32_t)(lba + i);
                break;
            case DIF_TYPE2:
                protection[i].ref_tag = 0;
                break;
            case DIF_TYPE3:
                protection[i].ref_tag = 0;
                break;
            default:
                protection[i].ref_tag = 0;
                break;
        }
    }

    return RET_OK;
}

ret_code_t dif_verify(const uint8_t *data, uint32_t data_len,
                      uint64_t lba, const dif_protection_t *protection,
                      uint64_t *error_lba)
{
    if (data == NULL || protection == NULL || data_len == 0) {
        return RET_ERR_PARAM;
    }
    if (!g_dif.is_initialized) {
        return RET_ERR_NOT_INIT;
    }
    if (g_dif.config.type == DIF_TYPE_DISABLED) {
        return RET_ERR_NOT_SUPPORT;
    }

    uint32_t lba_count = data_len / NAND_PAGE_SIZE;
    if (lba_count == 0) {
        lba_count = 1;
    }

    g_dif.stats.total_checks += lba_count;

    for (uint32_t i = 0; i < lba_count; i++) {
        const uint8_t *lba_data = data + i * NAND_PAGE_SIZE;
        uint32_t lba_data_len = (i == lba_count - 1) ?
                                (data_len - i * NAND_PAGE_SIZE) : NAND_PAGE_SIZE;

        /* CRC校验 */
        if (g_dif.config.guard_check_enable) {
            uint16_t calc_crc = dif_crc16(lba_data, lba_data_len);
            if (calc_crc != protection[i].crc) {
                g_dif.stats.crc_errors++;
                if (error_lba != NULL) {
                    *error_lba = lba + i;
                }
                printf("[DIF] CRC校验失败: LBA=%llu, 期望=0x%04X, 实际=0x%04X\n",
                       (unsigned long long)(lba + i),
                       protection[i].crc, calc_crc);
                return RET_ERR_DIF_CRC;
            }
        }

        /* 应用标签校验 */
        if (g_dif.config.app_tag_check_enable) {
            uint16_t expected_app_tag = g_dif.config.app_tag & g_dif.config.app_tag_mask;
            uint16_t actual_app_tag = protection[i].app_tag & g_dif.config.app_tag_mask;
            if (actual_app_tag != expected_app_tag) {
                g_dif.stats.app_tag_errors++;
                if (error_lba != NULL) {
                    *error_lba = lba + i;
                }
                printf("[DIF] 应用标签校验失败: LBA=%llu, 期望=0x%04X, 实际=0x%04X\n",
                       (unsigned long long)(lba + i),
                       expected_app_tag, actual_app_tag);
                return RET_ERR_DIF_APP_TAG;
            }
        }

        /* 参考标签校验 */
        if (g_dif.config.ref_tag_check_enable &&
            g_dif.config.type == DIF_TYPE1) {
            uint32_t expected_ref_tag = (uint32_t)(lba + i);
            if (protection[i].ref_tag != expected_ref_tag) {
                g_dif.stats.ref_tag_errors++;
                if (error_lba != NULL) {
                    *error_lba = lba + i;
                }
                printf("[DIF] 参考标签校验失败: LBA=%llu, 期望=0x%08X, 实际=0x%08X\n",
                       (unsigned long long)(lba + i),
                       expected_ref_tag, protection[i].ref_tag);
                return RET_ERR_DIF_REF_TAG;
            }
        }
    }

    return RET_OK;
}

ret_code_t dif_get_config(dif_config_t *config)
{
    if (config == NULL) {
        return RET_ERR_PARAM;
    }

    memcpy(config, &g_dif.config, sizeof(dif_config_t));
    return RET_OK;
}

ret_code_t dif_get_stats(dif_stats_t *stats)
{
    if (stats == NULL) {
        return RET_ERR_PARAM;
    }

    memcpy(stats, &g_dif.stats, sizeof(dif_stats_t));
    return RET_OK;
}

ret_code_t dif_reset_stats(void)
{
    memset(&g_dif.stats, 0, sizeof(dif_stats_t));
    return RET_OK;
}
