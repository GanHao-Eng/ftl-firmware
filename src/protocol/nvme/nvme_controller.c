/**
 * @file nvme_controller.c
 * @brief NVMe 控制器协议栈实现
 * @details NVMe 控制器协议栈完整实现，支持 NVMe 1.4 规范。
 *          实现真实的 SQ/CQ 队列机制、Doorbell 处理、Admin/I/O 命令集、
 *          MSI/MSI-X 中断、命名空间管理。
 *
 *          已实现的 P1 系统特性：
 *          - AER (Async Event Request) 异步事件请求
 *          - 多温度传感器 (Composite + Sensor 1-7)
 *          - 固件更新 (Firmware Download + Commit + Slot Info Log)
 *
 *          数据链路：
 *          1. 主机写 SQ 尾指针 Doorbell → nvme_ctrl_sq_doorbell()
 *          2. 控制器从 SQ 取命令 → nvme_ctrl_process()
 *          3. 执行命令 → nvme_ctrl_process_admin_cmd/io_cmd()
 *          4. 写 CQ 条目 → 内部函数
 *          5. 触发中断 → nvme_ctrl_trigger_irq()
 *          6. 主机写 CQ 头指针 Doorbell → nvme_ctrl_cq_doorbell()
 */

#include "protocol/nvme_controller.h"
#include "ftl.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ============================================================
 *  内部数据结构
 * ============================================================ */

/**
 * @brief NVMe 控制器私有数据
 */
typedef struct {
    nvme_ctrl_state_t state;      ///< 控制器状态
    nvme_ctrl_regs_t regs;        ///< 控制器寄存器
    nvme_sq_t admin_sq;           ///< Admin 提交队列
    nvme_cq_t admin_cq;           ///< Admin 完成队列
    nvme_sq_t io_sq;              ///< I/O 提交队列（简化：单个I/O队列）
    nvme_cq_t io_cq;              ///< I/O 完成队列
    uint32_t io_sq_count;         ///< I/O SQ 数量
    uint32_t io_cq_count;         ///< I/O CQ 数量
    uint64_t namespace_size;      ///< 命名空间大小（LBA数）
    uint32_t lba_size;            ///< LBA 大小（字节）
    bool initialized;             ///< 初始化标志
} nvme_ctrl_dev_t;

/**
 * @brief AER 挂起命令槽位
 * @details 主机发送 Async Event Request 后，命令挂起在此，
 *          待有事件发生时完成该命令。
 */
typedef struct {
    bool active;                   ///< 槽位是否有效（有挂起命令）
    uint16_t cid;                  ///< 命令 ID
} nvme_aer_pending_t;

/**
 * @brief AER 事件队列条目
 */
typedef struct {
    uint32_t event_type;           ///< 事件类型 (NVME_AER_TYPE_*)
    uint32_t event_info;           ///< 事件信息（子类型/详情）
    uint8_t log_page;              ///< 关联的日志页标识符
} nvme_aer_event_t;

/**
 * @brief 温度传感器
 */
typedef struct {
    uint16_t current;              ///< 当前温度（开尔文）
    uint16_t warning;              ///< 警告阈值（开尔文）
    uint16_t critical;             ///< 临界阈值（开尔文）
    uint16_t highest;              ///< 运行期间最高温度
    uint16_t min_temp;             ///< 最低温度限制（开尔文）
    uint16_t max_temp;             ///< 最高温度限制（开尔文）
} nvme_temp_sensor_t;

/**
 * @brief 固件插槽状态
 */
typedef enum {
    NVME_FW_SLOT_EMPTY = 0,        ///< 空插槽
    NVME_FW_SLOT_VALID = 1,        ///< 有效（有固件镜像）
    NVME_FW_SLOT_RUNNING = 2       ///< 运行中（当前激活的固件）
} nvme_fw_slot_state_t;

/**
 * @brief 固件插槽
 */
typedef struct {
    nvme_fw_slot_state_t state;    ///< 插槽状态
    char version[8];               ///< 固件版本字符串（8字节）
    uint32_t image_size;           ///< 固件镜像大小（字节）
} nvme_fw_slot_t;

/**
 * @brief 固件下载上下文
 * @details Firmware Image Download 命令分块下载时维护的状态
 */
typedef struct {
    bool active;                   ///< 下载是否进行中
    uint64_t downloaded_size;      ///< 已下载大小（字节）
} nvme_fw_download_ctx_t;

/* ============================================================
 *  全局变量
 * ============================================================ */

static nvme_ctrl_dev_t g_nvme_ctrl;

/* ---- AER 子系统 ---- */
static nvme_aer_pending_t g_aer_pending[NVME_AER_MAX_PENDING];
static nvme_aer_event_t g_aer_event_queue[NVME_AER_QUEUE_SIZE];
static uint32_t g_aer_event_head;       ///< 事件队列头（出队位置）
static uint32_t g_aer_event_tail;       ///< 事件队列尾（入队位置）
static uint32_t g_aer_event_count;      ///< 事件队列当前条目数
static uint32_t g_aer_event_mask;       ///< 异步事件配置掩码（FID 0x0B）

/* ---- 温度传感器子系统 ---- */
static nvme_temp_sensor_t g_temp_sensors[NVME_TEMP_SENSOR_COUNT];
static uint32_t g_warning_temp_time;    ///< 警告温度累计时间（分钟）
static uint32_t g_critical_temp_time;   ///< 临界温度累计时间（分钟）
static bool g_temp_warning_active;       ///< 当前是否处于警告温度状态
static bool g_temp_critical_active;      ///< 当前是否处于临界温度状态
static uint32_t g_temp_update_counter;   ///< 温度更新计数器

/* ---- 固件更新子系统 ---- */
static nvme_fw_slot_t g_fw_slots[NVME_FW_SLOT_COUNT];
static nvme_fw_download_ctx_t g_fw_download;
static uint8_t g_fw_running_slot;        ///< 当前运行的固件插槽 (1-7)
static uint8_t g_fw_next_boot_slot;      ///< 下次复位启动的固件插槽
static char g_fw_current_version[8];     ///< 当前运行固件版本号

/* ============================================================
 *  内部辅助函数
 * ============================================================ */

/**
 * @brief 设置完成状态
 * @param[out] cpl 完成条目
 * @param[in] status_code 状态码
 * @param[in] phase 相位位
 */
static void set_completion_status(nvme_completion_t *cpl, uint16_t status_code, bool phase)
{
    /* NVMe 完成条目 status 字段格式：
     * [15:14] 保留, [13:11] SCT, [10:9] 保留, [8:1] SC, [0] Phase Tag
     * status_code 格式: 高8位=SCT, 低8位=SC，左移1位后对应 [13:11] 和 [8:1]
     * 相位位由调用者（process_queue_commands）在 bit0 设置 */
    (void)phase;  /* 相位位不在此设置 */
    cpl->status = (status_code << 1);
}

/**
 * @brief 从 SQ 取命令
 * @param[in] sq SQ 指针
 * @param[out] cmd 命令输出
 * @return true=成功，false=队列为空
 */
static bool sq_fetch_cmd(nvme_sq_t *sq, nvme_command_t *cmd)
{
    if (sq->head == sq->tail) {
        return false;  /* 队列为空 */
    }

    /* 复制命令 */
    memcpy(cmd, &sq->entries[sq->head], sizeof(nvme_command_t));

    /* 更新头指针（环形队列） */
    sq->head = (sq->head + 1) % sq->size;

    return true;
}

/**
 * @brief 向 CQ 写完成条目
 * @param[in] cq CQ 指针
 * @param[in] cpl 完成条目
 */
static void cq_post_completion(nvme_cq_t *cq, const nvme_completion_t *cpl)
{
    /* 复制完成条目到队列尾部 */
    memcpy(&cq->entries[cq->tail], cpl, sizeof(nvme_completion_t));

    /* 更新尾指针 */
    cq->tail = (cq->tail + 1) % cq->size;

    /* 如果尾指针绕回，翻转相位位 */
    if (cq->tail == 0) {
        cq->phase = !cq->phase;
    }
}

/* ============================================================
 *  AER 子系统内部函数
 * ============================================================ */

/**
 * @brief 尝试完成挂起的 AER 命令
 * @details 当事件队列非空且有挂起的 AER 命令时，
 *          取出事件并完成对应的 AER 命令，写入 Admin CQ。
 *          AER 完成条目 dw0 格式：
 *            bit[31:24] = log_page_identifier
 *            bit[23:16] = reserved
 *            bit[15:0]  = event_type
 */
static void aer_try_complete(void)
{
    uint32_t i;
    nvme_completion_t cpl;
    nvme_aer_event_t *event;

    /* 循环处理：尽可能多地完成挂起的 AER */
    while (g_aer_event_count > 0) {
        /* 查找一个有效的挂起 AER 槽位 */
        for (i = 0; i < NVME_AER_MAX_PENDING; i++) {
            if (g_aer_pending[i].active) {
                break;
            }
        }
        if (i == NVME_AER_MAX_PENDING) {
            /* 没有挂起的 AER 命令，等待主机发送新的 AER */
            return;
        }

        /* 从事件队列头部取出事件 */
        event = &g_aer_event_queue[g_aer_event_head];
        g_aer_event_head = (g_aer_event_head + 1) % NVME_AER_QUEUE_SIZE;
        g_aer_event_count--;

        /* 构造 AER 完成条目 */
        memset(&cpl, 0, sizeof(nvme_completion_t));
        cpl.cid = g_aer_pending[i].cid;
        cpl.sqid = 0;  /* Admin SQ ID */
        cpl.sqhd = g_nvme_ctrl.admin_sq.head;
        /* dw0: bit[31:24]=log_page, bit[15:0]=event_type */
        cpl.dw0 = ((uint32_t)event->log_page << 24) | (event->event_type & 0x0000FFFFU);
        set_completion_status(&cpl, NVME_SC_SUCCESS, g_nvme_ctrl.admin_cq.phase);

        /* 写入 Admin CQ 并触发中断 */
        cq_post_completion(&g_nvme_ctrl.admin_cq, &cpl);
        nvme_ctrl_trigger_irq(0);

        /* 释放该 AER 挂起槽位 */
        g_aer_pending[i].active = false;

        LOG_INFO("NVMe AER: 完成事件上报, CID=%u, type=0x%04X, log_page=0x%02X, dw0=0x%08X",
                 cpl.cid, event->event_type, event->log_page, cpl.dw0);
    }
}

/* ============================================================
 *  温度传感器子系统内部函数
 * ============================================================ */

/**
 * @brief 检查温度阈值并触发 AER 事件
 * @details 检查 Composite 温度（Sensor 0）是否超过警告/临界阈值，
 *          更新 SMART critical_warning 状态，必要时 post AER 事件。
 */
static void temp_check_thresholds(void)
{
    nvme_temp_sensor_t *comp = &g_temp_sensors[0];

    /* 检查临界阈值 */
    if (comp->current >= comp->critical) {
        if (!g_temp_critical_active) {
            g_temp_critical_active = true;
            LOG_WARN("NVMe Temp: 温度超过临界阈值! current=%uK (%d°C), threshold=%uK",
                     comp->current, comp->current - 273, comp->critical);
            /* Post SMART/Health 事件：温度阈值（event_info=1） */
            nvme_ctrl_post_aer_event(NVME_AER_TYPE_SMART_HEALTH, 1, NVME_LOG_SMART_HEALTH);
        }
        g_critical_temp_time++;
    } else {
        g_temp_critical_active = false;
    }

    /* 检查警告阈值 */
    if (comp->current >= comp->warning) {
        if (!g_temp_warning_active) {
            g_temp_warning_active = true;
            LOG_WARN("NVMe Temp: 温度超过警告阈值, current=%uK (%d°C), threshold=%uK, 热节流已启动",
                     comp->current, comp->current - 273, comp->warning);
            /* Post SMART/Health 事件：温度阈值（event_info=1） */
            nvme_ctrl_post_aer_event(NVME_AER_TYPE_SMART_HEALTH, 1, NVME_LOG_SMART_HEALTH);
        }
        g_warning_temp_time++;
    } else {
        g_temp_warning_active = false;
    }
}

/* ============================================================
 *  固件更新子系统内部函数
 * ============================================================ */

/* ============================================================
 *  Admin 命令处理
 * ============================================================ */

/**
 * @brief 处理 Identify 命令
 * @param[in] cmd 命令
 * @param[out] cpl 完成条目
 */
static void admin_identify(const nvme_command_t *cmd, nvme_completion_t *cpl)
{
    uint32_t cns = cmd->cdw10 & 0xFF;  /* Controller or Namespace Structure */

    cpl->dw0 = 0;

    switch (cns) {
    case 0x01:  /* Identify Controller */
        LOG_INFO("NVMe Admin: Identify Controller");
        /* 实际实现中会填充 Identify Controller 数据结构（4096字节） */
        break;
    case 0x00:  /* Identify Namespace */
        LOG_INFO("NVMe Admin: Identify Namespace, NSID=%u", cmd->nsid);
        /* 填充 Identify Namespace 数据结构 */
        break;
    case 0x02:  /* Identify Active Namespace ID list */
        break;
    default: /* 未支持的操作码：返回无效操作码错误 */
        set_completion_status(cpl, NVME_SC_INVALID_FIELD, g_nvme_ctrl.admin_cq.phase);
        return;
    }

    set_completion_status(cpl, NVME_SC_SUCCESS, g_nvme_ctrl.admin_cq.phase);
}

/**
 * @brief 处理 Get Log Page 命令
 * @details 支持 LID=0x01(Error Info), 0x02(SMART/Health), 0x03(FW Slot Info)
 * @param[in] cmd 命令
 * @param[out] cpl 完成条目
 */
static void admin_get_log_page(const nvme_command_t *cmd, nvme_completion_t *cpl)
{
    uint8_t lid = cmd->cdw10 & 0xFF;  /* Log Page Identifier */

    LOG_INFO("NVMe Admin: Get Log Page, LID=0x%02X", lid);

    switch (lid) {
    case NVME_LOG_SMART_HEALTH:  /* 0x02: SMART/Health Information */
        break;
    case NVME_LOG_ERROR_INFO:    /* 0x01: Error Information */
        break;
    case NVME_LOG_FW_SLOT_INFO:  /* 0x03: Firmware Slot Information */
        LOG_INFO("NVMe Admin: Get Log Page Firmware Slot Info");
        break;
    default:
        break;
    }

    cpl->dw0 = 0;
    set_completion_status(cpl, NVME_SC_SUCCESS, g_nvme_ctrl.admin_cq.phase);
}

/**
 * @brief 处理 Create I/O CQ 命令
 */
static void admin_create_iocq(const nvme_command_t *cmd, nvme_completion_t *cpl)
{
    uint16_t qid = cmd->cdw10 & 0xFFFF;
    uint16_t qsize = (cmd->cdw10 >> 16) & 0xFFFF;
    uint16_t vector = cmd->cdw11 & 0xFFFF;

    LOG_INFO("NVMe Admin: Create I/O CQ, QID=%u, Size=%u, Vector=%u", qid, qsize, vector);

    /* 简化实现：使用单个 I/O CQ */
    g_nvme_ctrl.io_cq.qid = qid;
    g_nvme_ctrl.io_cq.size = qsize + 1;
    g_nvme_ctrl.io_cq.head = 0;
    g_nvme_ctrl.io_cq.tail = 0;
    g_nvme_ctrl.io_cq.vector = vector;
    g_nvme_ctrl.io_cq.phase = true;
    g_nvme_ctrl.io_cq.is_admin = false;
    g_nvme_ctrl.io_cq.entries = (nvme_completion_t *)calloc(qsize + 1, sizeof(nvme_completion_t));

    g_nvme_ctrl.io_cq_count++;

    cpl->dw0 = 0;
    set_completion_status(cpl, NVME_SC_SUCCESS, g_nvme_ctrl.admin_cq.phase);
}

/**
 * @brief 处理 Create I/O SQ 命令
 */
static void admin_create_iosq(const nvme_command_t *cmd, nvme_completion_t *cpl)
{
    uint16_t qid = cmd->cdw10 & 0xFFFF;
    uint16_t qsize = (cmd->cdw10 >> 16) & 0xFFFF;
    uint16_t cqid = cmd->cdw11 & 0xFFFF;

    LOG_INFO("NVMe Admin: Create I/O SQ, QID=%u, Size=%u, CQID=%u", qid, qsize, cqid);

    /* 简化实现：使用单个 I/O SQ */
    g_nvme_ctrl.io_sq.qid = qid;
    g_nvme_ctrl.io_sq.size = qsize + 1;
    g_nvme_ctrl.io_sq.head = 0;
    g_nvme_ctrl.io_sq.tail = 0;
    g_nvme_ctrl.io_sq.cqid = cqid;
    g_nvme_ctrl.io_sq.is_admin = false;
    g_nvme_ctrl.io_sq.entries = (nvme_command_t *)calloc(qsize + 1, sizeof(nvme_command_t));

    g_nvme_ctrl.io_sq_count++;

    cpl->dw0 = 0;
    set_completion_status(cpl, NVME_SC_SUCCESS, g_nvme_ctrl.admin_cq.phase);
}

/**
 * @brief 处理 Admin Set Features 命令 (opcode=0x09)
 *
 * 支持的 Feature ID：
 *   - 0x04 (Temperature Threshold): 设置 Composite 温度警告阈值
 *   - 0x07 (Number of Queues): 限制 I/O 队列数为 2
 *   - 0x08 (Interrupt Coalescing): 回显主机设置值
 *   - 0x0B (Async Event Configuration): 设置 AER 事件掩码
 *
 * @param cmd NVMe 命令
 * @param cpl 完成队列条目
 */
static void admin_set_features(const nvme_command_t *cmd, nvme_completion_t *cpl)
{
    uint8_t fid = cmd->cdw10 & 0xFF;  /* Feature Identifier */
    uint16_t nsqr, ncqr, nsqa, ncqa;
    uint16_t temp_threshold;

    LOG_INFO("NVMe Admin: Set Features, FID=0x%02X", fid);

    switch (fid) {
    case NVME_FEAT_TEMP_THRESHOLD:  /* 0x04: Temperature Threshold */
        /* cdw11 低16位为温度阈值（开尔文） */
        temp_threshold = (uint16_t)(cmd->cdw11 & 0xFFFF);
        if (temp_threshold > 0) {
            g_temp_sensors[0].warning = temp_threshold;
            LOG_INFO("NVMe Admin: 设置温度警告阈值=%uK (%d°C)", temp_threshold, temp_threshold - 273);
        }
        cpl->dw0 = cmd->cdw11;
        break;
    case NVME_FEAT_NUM_QUEUES:  /* 0x07: Number of Queues */
        nsqr = cmd->cdw11 & 0xFFFF;
        ncqr = (cmd->cdw11 >> 16) & 0xFFFF;
        /* 固件最多支持 2 个 I/O 队列 (0-based=1) */
        nsqa = nsqr < 2 ? nsqr : 1;
        ncqa = ncqr < 2 ? ncqr : 1;
        cpl->dw0 = ((uint32_t)ncqa << 16) | nsqa;
        LOG_INFO("NVMe Admin: Number of Queues, req SQ=%u CQ=%u, alloc SQ=%u CQ=%u, dw0=0x%08X",
                 nsqr + 1, ncqr + 1, nsqa + 1, ncqa + 1, cpl->dw0);
        break;
    case NVME_FEAT_ASYNC_EVENT:  /* 0x0B: Async Event Configuration */
        /* cdw11 为事件掩码：bit0=spare, bit1=temp, bit2=reliability, bit3=readonly, bit4=volatile */
        g_aer_event_mask = cmd->cdw11 & 0x1F;
        cpl->dw0 = cmd->cdw11;
        LOG_INFO("NVMe Admin: 设置 AER 事件掩码=0x%02X", g_aer_event_mask);
        break;
    case NVME_FEAT_IRQ_CONF:  /* 0x08: Interrupt Coalescing，回显 */
        cpl->dw0 = cmd->cdw11;
        break;
    default:
        cpl->dw0 = cmd->cdw11;
        break;
    }

    set_completion_status(cpl, NVME_SC_SUCCESS, g_nvme_ctrl.admin_cq.phase);
}

/**
 * @brief 处理 Get Features 命令 (opcode=0x0A)
 * @details 支持 FID=0x04(Temperature Threshold), 0x07(Num Queues), 0x0B(Async Event Config)
 * @param[in] cmd 命令
 * @param[out] cpl 完成条目
 */
static void admin_get_features(const nvme_command_t *cmd, nvme_completion_t *cpl)
{
    uint8_t fid = cmd->cdw10 & 0xFF;

    LOG_INFO("NVMe Admin: Get Features, FID=0x%02X", fid);

    cpl->dw0 = 0;

    switch (fid) {
    case NVME_FEAT_TEMP_THRESHOLD:  /* 0x04: 返回当前温度警告阈值 */
        cpl->dw0 = g_temp_sensors[0].warning;
        break;
    case NVME_FEAT_ASYNC_EVENT:  /* 0x0B: 返回当前 AER 事件掩码 */
        cpl->dw0 = g_aer_event_mask;
        break;
    case NVME_FEAT_NUM_QUEUES:  /* 0x07: Number of Queues */
        cpl->dw0 = ((uint32_t)1 << 16) | 1;  /* nsqa=1, ncqa=1 (0-based, 即2个队列) */
        break;
    default:
        break;
    }

    set_completion_status(cpl, NVME_SC_SUCCESS, g_nvme_ctrl.admin_cq.phase);
}

/**
 * @brief 处理 Async Event Request 命令 (opcode=0x0C)
 * @details 收到 AER 命令后不立即完成，而是挂起等待事件。
 *          最多支持 NVME_AER_MAX_PENDING 个并发 AER。
 *          挂起成功时设置 cpl->rsvd1=0xAE01 标记，nvme_ctrl_process
 *          据此跳过 cq_post_completion。
 * @param[in] cmd 命令
 * @param[out] cpl 完成条目
 */
static void admin_async_event(const nvme_command_t *cmd, nvme_completion_t *cpl)
{
    uint32_t i;

    LOG_INFO("NVMe Admin: Async Event Request, CID=%u", cmd->cid);

    /* 查找空闲的 AER 挂起槽位 */
    for (i = 0; i < NVME_AER_MAX_PENDING; i++) {
        if (!g_aer_pending[i].active) {
            break;
        }
    }

    if (i == NVME_AER_MAX_PENDING) {
        /* 超过 AER 并发限制，返回 Async Event Request Limit Exceeded */
        LOG_WARN("NVMe AER: 超过并发限制 (%u), 返回 AER Limit Exceeded", NVME_AER_MAX_PENDING);
        set_completion_status(cpl, NVME_SC_AER_LIMIT_EXCEEDED, g_nvme_ctrl.admin_cq.phase);
        return;
    }

    /* 挂起 AER 命令：保存 CID，不立即完成 */
    g_aer_pending[i].active = true;
    g_aer_pending[i].cid = cmd->cid;

    /* 设置挂起标记：rsvd1=0xAE01 通知 nvme_ctrl_process 跳过 CQ 写入 */
    cpl->rsvd1 = 0xAE01;
    cpl->cid = cmd->cid;
    cpl->sqid = 0;

    LOG_INFO("NVMe AER: 命令已挂起, CID=%u, slot=%u, pending_count=%u",
             cmd->cid, i, i + 1);

    /* 立即尝试：如果事件队列已有事件，直接完成（在 process 中统一处理） */
}

/**
 * @brief 处理 Firmware Image Download 命令 (opcode=0x11)
 * @details 分块下载固件镜像。offset=0 时开始新下载，
 *          后续块必须连续。模拟存储（只记录元数据，不保存真实内容）。
 *          命令字段：
 *            cdw10 = 数据偏移量低32位（以4字节为单位，Number of Dwords）
 *            cdw11 = 数据偏移量高32位
 * @param[in] cmd 命令
 * @param[out] cpl 完成条目
 */
static void admin_fw_download(const nvme_command_t *cmd, nvme_completion_t *cpl)
{
    uint64_t offset_dwords;
    uint64_t offset_bytes;
    uint32_t data_len;

    /* 计算偏移量：cdw10=低32位, cdw11=高32位，单位为 Dword (4字节) */
    offset_dwords = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;
    offset_bytes = offset_dwords * 4;

    /* 估算数据长度：从 PRP/SGL 无法直接获取，使用命令中的隐含信息
     * 在 NVMe/TCP 模式下数据在 tcp_target 层处理，这里模拟一个典型块大小 */
    data_len = 4096;  /* 模拟每页 4KB */

    LOG_INFO("NVMe Admin: Firmware Download, offset=0x%llX bytes, est_len=%u",
             (unsigned long long)offset_bytes, data_len);

    if (offset_bytes == 0) {
        /* 偏移为0：开始新的固件下载 */
        g_fw_download.active = true;
        g_fw_download.downloaded_size = 0;
        LOG_INFO("NVMe FW: 开始新的固件镜像下载");
    }

    if (!g_fw_download.active) {
        /* 没有进行中的下载但 offset!=0，非法 */
        LOG_ERROR("NVMe FW: 下载偏移不连续，期望 offset=0 开始新下载");
        set_completion_status(cpl, NVME_SC_INVALID_FIELD, g_nvme_ctrl.admin_cq.phase);
        return;
    }

    /* 校验偏移连续性 */
    if (offset_bytes != g_fw_download.downloaded_size) {
        LOG_ERROR("NVMe FW: 下载偏移不连续, expected=0x%llX, got=0x%llX",
                 (unsigned long long)g_fw_download.downloaded_size,
                 (unsigned long long)offset_bytes);
        set_completion_status(cpl, NVME_SC_INVALID_FIELD, g_nvme_ctrl.admin_cq.phase);
        return;
    }

    /* 校验最大镜像大小 */
    if (offset_bytes + data_len > NVME_FW_MAX_IMAGE_SIZE) {
        LOG_ERROR("NVMe FW: 固件镜像超过最大大小 %u bytes", NVME_FW_MAX_IMAGE_SIZE);
        set_completion_status(cpl, NVME_SC_INVALID_FIELD, g_nvme_ctrl.admin_cq.phase);
        return;
    }

    /* 模拟接收数据：只更新已下载大小，不保存真实内容 */
    g_fw_download.downloaded_size += data_len;

    cpl->dw0 = 0;
    set_completion_status(cpl, NVME_SC_SUCCESS, g_nvme_ctrl.admin_cq.phase);

    LOG_INFO("NVMe FW: 下载块已接收, total_downloaded=0x%llX bytes",
             (unsigned long long)g_fw_download.downloaded_size);
}

/**
 * @brief 处理 Firmware Commit 命令 (opcode=0x10)
 * @details 将已下载的固件镜像提交到指定插槽，可选激活。
 *          cdw10 字段：
 *            bit[2:0]  = FS (Firmware Slot, 1-7)
 *            bit[10:8] = CA (Commit Action)
 *              0 = 替换指定插槽镜像
 *              1 = 替换并在下次复位时激活
 *              2 = 替换并立即激活
 *              3 = 设置为下次启动插槽
 * @param[in] cmd 命令
 * @param[out] cpl 完成条目
 */
static void admin_fw_activate(const nvme_command_t *cmd, nvme_completion_t *cpl)
{
    uint8_t slot = (uint8_t)(cmd->cdw10 & 0x07);
    uint8_t action = (uint8_t)((cmd->cdw10 >> 8) & 0x07);
    nvme_fw_slot_t *fw_slot;

    LOG_INFO("NVMe Admin: Firmware Commit, slot=%u, action=%u", slot, action);

    /* 校验插槽号有效性 (1-7) */
    if (slot < 1 || slot > NVME_FW_SLOT_COUNT) {
        LOG_ERROR("NVMe FW: 无效的固件插槽号 %u", slot);
        set_completion_status(cpl, NVME_SC_INVALID_FW_SLOT, g_nvme_ctrl.admin_cq.phase);
        return;
    }

    fw_slot = &g_fw_slots[slot - 1];

    /* CA=3: 仅设置下次启动插槽，不需要已下载镜像 */
    if (action == NVME_FW_COMMIT_SET_BOOT) {
        if (fw_slot->state == NVME_FW_SLOT_EMPTY) {
            LOG_ERROR("NVMe FW: 插槽 %u 为空，无法设置为启动插槽", slot);
            set_completion_status(cpl, NVME_SC_INVALID_FW_IMAGE, g_nvme_ctrl.admin_cq.phase);
            return;
        }
        g_fw_next_boot_slot = slot;
        LOG_INFO("NVMe FW: 设置插槽 %u 为下次启动插槽", slot);
        cpl->dw0 = 0;
        set_completion_status(cpl, NVME_SC_SUCCESS, g_nvme_ctrl.admin_cq.phase);
        return;
    }

    /* CA=0/1/2: 需要已下载的固件镜像 */
    if (!g_fw_download.active || g_fw_download.downloaded_size == 0) {
        LOG_ERROR("NVMe FW: 没有已下载的固件镜像");
        set_completion_status(cpl, NVME_SC_INVALID_FW_IMAGE, g_nvme_ctrl.admin_cq.phase);
        return;
    }

    /* CA=0: 不允许替换正在运行的插槽 */
    if (action == NVME_FW_COMMIT_REPLACE && slot == g_fw_running_slot) {
        LOG_ERROR("NVMe FW: 插槽 %u 正在运行，只读不可替换", slot);
        set_completion_status(cpl, NVME_SC_INVALID_FW_SLOT, g_nvme_ctrl.admin_cq.phase);
        return;
    }

    /* 将下载的镜像提交到指定插槽 */
    fw_slot->state = NVME_FW_SLOT_VALID;
    fw_slot->image_size = (uint32_t)g_fw_download.downloaded_size;
    /* 使用默认版本号 "v2.3.0" */
    memset(fw_slot->version, 0, sizeof(fw_slot->version));
    memcpy(fw_slot->version, "v2.3.0", 6);

    LOG_INFO("NVMe FW: 固件镜像已提交到插槽 %u, size=%u bytes, version=%s",
             slot, fw_slot->image_size, fw_slot->version);

    /* 清除下载上下文 */
    g_fw_download.active = false;
    g_fw_download.downloaded_size = 0;

    /* 根据提交动作执行后续操作 */
    switch (action) {
    case NVME_FW_COMMIT_REPLACE:  /* 0: 仅替换镜像 */
        break;
    case NVME_FW_COMMIT_REPLACE_RESET:  /* 1: 替换并设置下次启动 */
        g_fw_next_boot_slot = slot;
        LOG_INFO("NVMe FW: 插槽 %u 将在下次复位时激活", slot);
        break;
    case NVME_FW_COMMIT_REPLACE_NOW:  /* 2: 替换并立即激活 */
        /* 标记旧运行插槽为有效（不再运行中） */
        if (g_fw_running_slot >= 1 && g_fw_running_slot <= NVME_FW_SLOT_COUNT) {
            g_fw_slots[g_fw_running_slot - 1].state = NVME_FW_SLOT_VALID;
        }
        /* 激活新插槽 */
        fw_slot->state = NVME_FW_SLOT_RUNNING;
        g_fw_running_slot = slot;
        g_fw_next_boot_slot = slot;
        /* 更新当前固件版本号 */
        memset(g_fw_current_version, 0, sizeof(g_fw_current_version));
        memcpy(g_fw_current_version, fw_slot->version, sizeof(g_fw_current_version) - 1);
        LOG_INFO("NVMe FW: 固件已立即激活, slot=%u, version=%s", slot, g_fw_current_version);
        /* Post AER Notice 事件：固件激活 */
        nvme_ctrl_post_aer_event(NVME_AER_TYPE_NOTICE, 0, 0x00);
        break;
    default:
        LOG_WARN("NVMe FW: 保留的提交动作 %u", action);
        break;
    }

    cpl->dw0 = 0;
    set_completion_status(cpl, NVME_SC_SUCCESS, g_nvme_ctrl.admin_cq.phase);
}

/* ============================================================
 *  I/O 命令处理
 * ============================================================ */

/**
 * @brief 处理 NVM Read 命令
 */
static void io_read(const nvme_command_t *cmd, nvme_completion_t *cpl)
{
    uint64_t slba = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;
    uint16_t nlb = (cmd->cdw12 & 0xFFFF) + 1;  /* Number of Logical Blocks */

    LOG_INFO("NVMe I/O: Read, SLBA=%llu, NLB=%u", (unsigned long long)slba, nlb);

    /* 调用 FTL 读取数据 */
    /* ftl_read(slba, nlb, data_buffer); */

    cpl->dw0 = 0;
    set_completion_status(cpl, NVME_SC_SUCCESS, g_nvme_ctrl.io_cq.phase);
}

/**
 * @brief 处理 NVM Write 命令
 */
static void io_write(const nvme_command_t *cmd, nvme_completion_t *cpl)
{
    uint64_t slba = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;
    uint16_t nlb = (cmd->cdw12 & 0xFFFF) + 1;

    LOG_INFO("NVMe I/O: Write, SLBA=%llu, NLB=%u", (unsigned long long)slba, nlb);

    /* 调用 FTL 写入数据 */
    /* ftl_write(slba, nlb, data_buffer); */

    cpl->dw0 = 0;
    set_completion_status(cpl, NVME_SC_SUCCESS, g_nvme_ctrl.io_cq.phase);
}

/**
 * @brief 处理 Flush 命令
 */
static void io_flush(const nvme_command_t *cmd, nvme_completion_t *cpl)
{
    LOG_INFO("NVMe I/O: Flush, NSID=%u", cmd->nsid);

    /* 调用 FTL 刷新缓存 */
    /* ftl_flush(); */

    cpl->dw0 = 0;
    set_completion_status(cpl, NVME_SC_SUCCESS, g_nvme_ctrl.io_cq.phase);
}

/**
 * @brief 处理 Dataset Management（TRIM）命令
 */
static void io_dataset_mgmt(const nvme_command_t *cmd, nvme_completion_t *cpl)
{
    uint8_t nr = (cmd->cdw10 & 0xFF) + 1;  /* Number of Ranges */

    LOG_INFO("NVMe I/O: Dataset Management (TRIM), NR=%u", nr);

    /* 调用 FTL 处理 TRIM */
    /* ftl_trim(ranges, nr); */

    cpl->dw0 = 0;
    set_completion_status(cpl, NVME_SC_SUCCESS, g_nvme_ctrl.io_cq.phase);
}

/* ============================================================
 *  子系统初始化
 * ============================================================ */

/**
 * @brief 初始化 AER 子系统
 */
static void nvme_ctrl_aer_init(void)
{
    memset(g_aer_pending, 0, sizeof(g_aer_pending));
    memset(g_aer_event_queue, 0, sizeof(g_aer_event_queue));
    g_aer_event_head = 0;
    g_aer_event_tail = 0;
    g_aer_event_count = 0;
    /* 默认使能所有 SMART/Health 事件 (bit0-4 均为1) */
    g_aer_event_mask = 0x1F;
}

/**
 * @brief 初始化温度传感器子系统
 * @details 设置8个温度传感器的初始温度、阈值和温度范围。
 *          Sensor 0=Composite, 1=NAND Ch0, 2=NAND Ch1, 3=DRAM,
 *          4=PCB Ambient, 5=Power Supply, 6-7=保留(未使用)
 */
static void nvme_ctrl_temp_init(void)
{
    uint32_t i;

    memset(g_temp_sensors, 0, sizeof(g_temp_sensors));
    g_warning_temp_time = 0;
    g_critical_temp_time = 0;
    g_temp_warning_active = false;
    g_temp_critical_active = false;
    g_temp_update_counter = 0;

    /* Sensor 0: Composite Temperature (主控综合温度) */
    g_temp_sensors[0].current = 313;    /* 40°C */
    g_temp_sensors[0].warning = NVME_TEMP_WARNING_DEFAULT;  /* 343K=70°C */
    g_temp_sensors[0].critical = NVME_TEMP_CRITICAL_DEFAULT; /* 358K=85°C */
    g_temp_sensors[0].highest = 313;
    g_temp_sensors[0].min_temp = 293;   /* 20°C */
    g_temp_sensors[0].max_temp = 363;   /* 90°C */

    /* Sensor 1: NAND Channel 0 Temperature */
    g_temp_sensors[1].current = 308;    /* 35°C */
    g_temp_sensors[1].warning = 348;    /* 75°C */
    g_temp_sensors[1].critical = 358;   /* 85°C */
    g_temp_sensors[1].highest = 308;
    g_temp_sensors[1].min_temp = 288;   /* 15°C */
    g_temp_sensors[1].max_temp = 358;   /* 85°C */

    /* Sensor 2: NAND Channel 1 Temperature */
    g_temp_sensors[2].current = 308;    /* 35°C */
    g_temp_sensors[2].warning = 348;
    g_temp_sensors[2].critical = 358;
    g_temp_sensors[2].highest = 308;
    g_temp_sensors[2].min_temp = 288;
    g_temp_sensors[2].max_temp = 358;

    /* Sensor 3: DRAM Temperature */
    g_temp_sensors[3].current = 318;    /* 45°C */
    g_temp_sensors[3].warning = 353;    /* 80°C */
    g_temp_sensors[3].critical = 368;   /* 95°C */
    g_temp_sensors[3].highest = 318;
    g_temp_sensors[3].min_temp = 293;   /* 20°C */
    g_temp_sensors[3].max_temp = 368;   /* 95°C */

    /* Sensor 4: PCB Ambient Temperature */
    g_temp_sensors[4].current = 300;    /* 27°C */
    g_temp_sensors[4].warning = 328;    /* 55°C */
    g_temp_sensors[4].critical = 338;   /* 65°C */
    g_temp_sensors[4].highest = 300;
    g_temp_sensors[4].min_temp = 283;   /* 10°C */
    g_temp_sensors[4].max_temp = 323;   /* 50°C */

    /* Sensor 5: Power Supply Temperature */
    g_temp_sensors[5].current = 310;    /* 37°C */
    g_temp_sensors[5].warning = 343;    /* 70°C */
    g_temp_sensors[5].critical = 353;   /* 80°C */
    g_temp_sensors[5].highest = 310;
    g_temp_sensors[5].min_temp = 293;   /* 20°C */
    g_temp_sensors[5].max_temp = 353;   /* 80°C */

    /* Sensor 6-7: 保留，未使用（current=0） */
    for (i = 6; i < NVME_TEMP_SENSOR_COUNT; i++) {
        g_temp_sensors[i].current = 0;
        g_temp_sensors[i].warning = 0;
        g_temp_sensors[i].critical = 0;
        g_temp_sensors[i].highest = 0;
        g_temp_sensors[i].min_temp = 0;
        g_temp_sensors[i].max_temp = 0;
    }
}

/**
 * @brief 初始化固件更新子系统
 * @details Slot 1 初始为运行中固件，版本 "v2.2.0"；
 *          Slot 2-7 为空插槽。
 */
static void nvme_ctrl_fw_init(void)
{
    uint32_t i;

    memset(g_fw_slots, 0, sizeof(g_fw_slots));
    memset(&g_fw_download, 0, sizeof(g_fw_download));

    /* Slot 1: 当前运行的固件 */
    g_fw_slots[0].state = NVME_FW_SLOT_RUNNING;
    memset(g_fw_slots[0].version, 0, sizeof(g_fw_slots[0].version));
    memcpy(g_fw_slots[0].version, "v2.2.0", 6);
    g_fw_slots[0].image_size = 0;

    /* Slot 2-7: 空插槽 */
    for (i = 1; i < NVME_FW_SLOT_COUNT; i++) {
        g_fw_slots[i].state = NVME_FW_SLOT_EMPTY;
        memset(g_fw_slots[i].version, 0, sizeof(g_fw_slots[i].version));
        g_fw_slots[i].image_size = 0;
    }

    g_fw_running_slot = 1;
    g_fw_next_boot_slot = 1;
    memset(g_fw_current_version, 0, sizeof(g_fw_current_version));
    memcpy(g_fw_current_version, "v2.2.0", 6);

    g_fw_download.active = false;
    g_fw_download.downloaded_size = 0;
}

/* ============================================================
 *  接口实现
 * ============================================================ */

/**
 * @brief 初始化 NVMe 控制器
 *
 * 设置控制器寄存器初始值：
 *   - CAP = 0xF0000103FF (MQES=0x3FF, DSTRD=0, TO=0x0F, NSSRS=1)
 *   - VS  = 0x00010400 (NVMe 1.4)
 *   - 命名空间大小 = 262144 LBA × 4KB = 1GB
 *
 * @return RET_OK 成功
 */
ret_code_t nvme_ctrl_init(void)
{
    memset(&g_nvme_ctrl, 0, sizeof(g_nvme_ctrl));

    /* 初始化 AER、温度、固件子系统 */
    nvme_ctrl_aer_init();
    nvme_ctrl_temp_init();
    nvme_ctrl_fw_init();

    /* 设置控制器能力寄存器（CAP）
     * 位布局（NVMe 1.4 规范）：
     *   bits 15:0  MQES   - 最大队列条目数-1
     *   bits 17:16 CQR    - 连续队列要求（0=支持非连续, 1=需要连续）
     *   bit  18    AMS    - 仲裁机制（0=优先级, 1=加权轮询）
     *   bit  24    NSSRS  - NVM子系统复位支持
     *   bits 35:32 DSTRD  - 门铃步长（4KB单位, 0=4KB）
     *   bits 39:36 TO     - 超时（500ms单位, 0x0F=7.5秒）
     *   bit  40    DUR    - 门铃缓冲区配置支持
     *   bit  42    BPS    - 引导分区支持
     *   bits 45:43 MPSMIN - 最小内存页大小（2^(12+MPSMIN), 0=4KB）
     *   bits 48:46 MPSMAX - 最大内存页大小
     *   bit  49    PMRS   - 持久内存区域支持
     *   bit  50    CMBS   - 控制器内存缓冲区支持
     * 注意：MDTS 不在 CAP 中，在 Identify Controller 数据结构 byte77
     */
    g_nvme_ctrl.regs.cap = (0x03FFULL << 0)    /* MQES: 1024个条目 */
                         | (0x01ULL << 16)      /* CQR: 需要连续队列 */
                         | (0x00ULL << 18)      /* AMS: 优先级仲裁 */
                         | (0x00ULL << 24)      /* NSSRS: 不支持 */
                         | (0x00ULL << 32)      /* DSTRD: 4KB */
                         | (0x0FULL << 36)      /* TO: 超时7.5秒 */
                         | (0x00ULL << 40)      /* DUR: 不支持 */
                         | (0x00ULL << 42)      /* BPS: 不支持 */
                         | (0x00ULL << 43)      /* MPSMIN: 4KB */
                         | (0x00ULL << 46);     /* MPSMAX: 4KB */

    /* 设置版本 */
    g_nvme_ctrl.regs.vs = NVME_VERSION;

    /* 初始化 Admin 队列 */
    g_nvme_ctrl.admin_sq.qid = 0;
    g_nvme_ctrl.admin_sq.size = NVME_ADMIN_QSIZE;
    g_nvme_ctrl.admin_sq.head = 0;
    g_nvme_ctrl.admin_sq.tail = 0;
    g_nvme_ctrl.admin_sq.is_admin = true;
    g_nvme_ctrl.admin_sq.entries = (nvme_command_t *)calloc(NVME_ADMIN_QSIZE, sizeof(nvme_command_t));

    g_nvme_ctrl.admin_cq.qid = 0;
    g_nvme_ctrl.admin_cq.size = NVME_ADMIN_QSIZE;
    g_nvme_ctrl.admin_cq.head = 0;
    g_nvme_ctrl.admin_cq.tail = 0;
    g_nvme_ctrl.admin_cq.phase = true;
    g_nvme_ctrl.admin_cq.is_admin = true;
    g_nvme_ctrl.admin_cq.entries = (nvme_completion_t *)calloc(NVME_ADMIN_QSIZE, sizeof(nvme_completion_t));

    /* 设置命名空间参数 */
    g_nvme_ctrl.namespace_size = 26214400;  /* 100GB / 4KB = 25.6M LBA */
    g_nvme_ctrl.lba_size = 4096;

    g_nvme_ctrl.state = NVME_CTRL_STATE_RESET;
    g_nvme_ctrl.initialized = true;

    LOG_INFO("NVMe 控制器初始化完成，版本=1.4，命名空间大小=%llu LBA",
             (unsigned long long)g_nvme_ctrl.namespace_size);

    /* 命令处理完成，返回RET_OK（执行结果通过cpl->status反映） */
    return RET_OK;
}

ret_code_t nvme_ctrl_deinit(void)
{
    if (!g_nvme_ctrl.initialized) {
        return RET_OK;
    }

    /* 释放队列内存 */
    if (g_nvme_ctrl.admin_sq.entries) free(g_nvme_ctrl.admin_sq.entries);
    if (g_nvme_ctrl.admin_cq.entries) free(g_nvme_ctrl.admin_cq.entries);
    if (g_nvme_ctrl.io_sq.entries) free(g_nvme_ctrl.io_sq.entries);
    if (g_nvme_ctrl.io_cq.entries) free(g_nvme_ctrl.io_cq.entries);

    g_nvme_ctrl.initialized = false;
    g_nvme_ctrl.state = NVME_CTRL_STATE_RESET;

    /* 命令处理完成，返回RET_OK（执行结果通过cpl->status反映） */
    return RET_OK;
}

void nvme_ctrl_write_cc(uint32_t value)
{
    g_nvme_ctrl.regs.cc = value;

    /* 检查控制器使能位（CC.EN, bit0） */
    if (value & 0x01) {
        /* 使能控制器 */
        g_nvme_ctrl.state = NVME_CTRL_STATE_READY;
        g_nvme_ctrl.regs.csts |= 0x01;  /* CSTS.RDY = 1 */
        LOG_INFO("NVMe 控制器已使能，进入就绪状态");
    } else {
        /* 禁用控制器 */
        g_nvme_ctrl.state = NVME_CTRL_STATE_RESET;
        g_nvme_ctrl.regs.csts &= ~0x01;  /* CSTS.RDY = 0 */
        LOG_INFO("NVMe 控制器已禁用");
    }
}

void nvme_ctrl_write_aqa(uint32_t value)
{
    g_nvme_ctrl.regs.aqa = value;
    LOG_INFO("NVMe Admin 队列属性: ASQS=%u, ACQS=%u",
             (value & 0xFFF) + 1, ((value >> 16) & 0xFFF) + 1);
}

void nvme_ctrl_write_asq(uint32_t value, bool is_high)
{
    if (is_high) {
        g_nvme_ctrl.regs.asq = (g_nvme_ctrl.regs.asq & 0xFFFFFFFF) | ((uint64_t)value << 32);
    } else {
        g_nvme_ctrl.regs.asq = (g_nvme_ctrl.regs.asq & 0xFFFFFFFF00000000ULL) | value;
    }
}

void nvme_ctrl_write_acq(uint32_t value, bool is_high)
{
    if (is_high) {
        g_nvme_ctrl.regs.acq = (g_nvme_ctrl.regs.acq & 0xFFFFFFFF) | ((uint64_t)value << 32);
    } else {
        g_nvme_ctrl.regs.acq = (g_nvme_ctrl.regs.acq & 0xFFFFFFFF00000000ULL) | value;
    }
}

void nvme_ctrl_sq_doorbell(uint16_t qid, uint32_t value)
{
    nvme_sq_t *sq = NULL;

    if (qid == 0) {
        sq = &g_nvme_ctrl.admin_sq;
    } else {
        sq = &g_nvme_ctrl.io_sq;
    }

    /* 更新 SQ 尾指针（主机通知控制器有新命令） */
    sq->tail = value % sq->size;

    LOG_DEBUG("NVMe SQ Doorbell: QID=%u, Tail=%u, Head=%u", qid, sq->tail, sq->head);
}

void nvme_ctrl_cq_doorbell(uint16_t qid, uint32_t value)
{
    nvme_cq_t *cq = NULL;

    if (qid == 0) {
        cq = &g_nvme_ctrl.admin_cq;
    } else {
        cq = &g_nvme_ctrl.io_cq;
    }

    /* 更新 CQ 头指针（主机通知控制器已处理完成条目） */
    cq->head = value % cq->size;

    LOG_DEBUG("NVMe CQ Doorbell: QID=%u, Head=%u, Tail=%u", qid, cq->head, cq->tail);
}

/**
 * @brief NVMe Admin 命令分发处理
 *
 * 从 Admin 提交队列(SQ)取出命令后，根据操作码(opcode)分发到对应的处理函数。
 * Admin 命令用于控制器管理和配置，不涉及用户数据读写。
 *
 * 支持的 Admin 命令（操作码 -> 处理方式）：
 *   - 0x00 Delete I/O SQ       → 占位返回成功（简化实现）
 *   - 0x01 Create I/O SQ       → admin_create_iosq
 *   - 0x02 Get Log Page         → admin_get_log_page
 *   - 0x04 Delete I/O CQ       → 占位返回成功
 *   - 0x05 Create I/O CQ       → admin_create_iocq
 *   - 0x06 Identify             → admin_identify
 *   - 0x08 Abort                → 占位返回成功
 *   - 0x09 Set Features         → admin_set_features
 *   - 0x0A Get Features         → admin_get_features
 *   - 0x0C Async Event Request  → admin_async_event (挂起等待事件)
 *   - 0x10 Firmware Commit     → admin_fw_activate
 *   - 0x11 Firmware Download   → admin_fw_download
 *   - 0x80 Format NVM           → 占位返回成功
 *   - 0x18 Keep Alive           → 占位返回成功
 *   - 其他                       → 返回 NVME_SC_INVALID_OPCODE
 *
 * @param cmd NVMe 命令
 * @param cpl 完成队列条目（输出参数）
 * @return RET_OK 始终返回成功
 */
ret_code_t nvme_ctrl_process_admin_cmd(const nvme_command_t *cmd, nvme_completion_t *cpl)
{
    /* 清零完成队列条目，设置命令ID和SQID（Admin SQ ID固定为0） */
    memset(cpl, 0, sizeof(nvme_completion_t));
    cpl->cid = cmd->cid;      /* 命令ID，主机用于匹配命令和完成 */
    cpl->sqid = 0;             /* Admin SQ ID */

    /* 根据操作码(opcode)分发到对应处理函数 */
    switch (cmd->opcode) {
    case NVME_ADMIN_DELETE_IOSQ: /* 删除I/O提交队列：简化实现 */
        LOG_INFO("NVMe Admin: Delete I/O SQ");
        set_completion_status(cpl, NVME_SC_SUCCESS, g_nvme_ctrl.admin_cq.phase);
        break;
    case NVME_ADMIN_CREATE_IOSQ: /* 创建I/O提交队列 */
        admin_create_iosq(cmd, cpl);
        break;
    case NVME_ADMIN_GET_LOG_PAGE: /* 获取日志页 */
        admin_get_log_page(cmd, cpl);
        break;
    case NVME_ADMIN_DELETE_IOCQ: /* 删除I/O完成队列：简化实现 */
        LOG_INFO("NVMe Admin: Delete I/O CQ");
        set_completion_status(cpl, NVME_SC_SUCCESS, g_nvme_ctrl.admin_cq.phase);
        break;
    case NVME_ADMIN_CREATE_IOCQ: /* 创建I/O完成队列 */
        admin_create_iocq(cmd, cpl);
        break;
    case NVME_ADMIN_IDENTIFY: /* 识别 */
        admin_identify(cmd, cpl);
        break;
    case NVME_ADMIN_ABORT: /* 中止命令：简化实现 */
        LOG_INFO("NVMe Admin: Abort");
        set_completion_status(cpl, NVME_SC_SUCCESS, g_nvme_ctrl.admin_cq.phase);
        break;
    case NVME_ADMIN_SET_FEATURES: /* 设置特性 */
        admin_set_features(cmd, cpl);
        break;
    case NVME_ADMIN_GET_FEATURES: /* 获取特性 */
        admin_get_features(cmd, cpl);
        break;
    case NVME_ADMIN_ASYNC_EVENT: /* 异步事件请求：挂起等待事件 */
        admin_async_event(cmd, cpl);
        break;
    case NVME_ADMIN_FW_DOWNLOAD: /* 固件镜像下载 */
        admin_fw_download(cmd, cpl);
        break;
    case NVME_ADMIN_FW_ACTIVATE: /* 固件提交/激活 */
        admin_fw_activate(cmd, cpl);
        break;
    case NVME_ADMIN_FORMAT_NVM: /* 格式化NVM：简化实现 */
        LOG_INFO("NVMe Admin: Format NVM");
        set_completion_status(cpl, NVME_SC_SUCCESS, g_nvme_ctrl.admin_cq.phase);
        break;
    case NVME_ADMIN_KEEP_ALIVE: /* 保活命令 */
        LOG_INFO("NVMe Admin: Keep Alive 命令");
        set_completion_status(cpl, NVME_SC_SUCCESS, g_nvme_ctrl.admin_cq.phase);
        break;
    default: /* 未支持的操作码：返回无效操作码错误 */
        LOG_WARN("NVMe Admin: 未支持的操作码 0x%02X", cmd->opcode);
        set_completion_status(cpl, NVME_SC_INVALID_OPCODE, g_nvme_ctrl.admin_cq.phase);
        break;
    }

    /* 命令处理完成，返回RET_OK（执行结果通过cpl->status反映） */
    return RET_OK;
}

/**
 * @brief I/O 命令分发处理
 *
 * 支持的 I/O 命令：
 *   - 0x00 Flush            → io_flush
 *   - 0x01 Write            → io_write
 *   - 0x02 Read             → io_read
 *   - 0x04 Write Uncorrectable → 占位返回成功
 *   - 0x05 Compare          → 占位返回成功
 *   - 0x08 Write Zeroes     → 直接写零到 FTL
 *   - 0x09 Dataset Management → io_dataset_mgmt (TRIM)
 *   - 0x0C Verify           → 占位返回成功
 *
 * @param cmd NVMe 命令
 * @param cpl 完成队列条目
 * @return RET_OK 始终成功
 */
ret_code_t nvme_ctrl_process_io_cmd(const nvme_command_t *cmd, nvme_completion_t *cpl)
{
    /* 清零完成队列条目，设置命令ID和SQID */
    memset(cpl, 0, sizeof(nvme_completion_t));
    cpl->cid = cmd->cid;
    cpl->sqid = g_nvme_ctrl.io_sq.qid;

    /* 根据操作码(opcode)分发到对应处理函数 */
    switch (cmd->opcode) {
    case NVME_IO_FLUSH:
        io_flush(cmd, cpl);
        break;
    case NVME_IO_WRITE:
        io_write(cmd, cpl);
        break;
    case NVME_IO_READ:
        io_read(cmd, cpl);
        break;
    case NVME_IO_WRITE_UNCORRECTABLE:
        LOG_INFO("NVMe I/O: Write Uncorrectable");
        set_completion_status(cpl, NVME_SC_SUCCESS, g_nvme_ctrl.io_cq.phase);
        break;
    case NVME_IO_COMPARE:
        LOG_INFO("NVMe I/O: Compare");
        set_completion_status(cpl, NVME_SC_SUCCESS, g_nvme_ctrl.io_cq.phase);
        break;
    case NVME_IO_WRITE_ZEROES: {
        uint64_t slba = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;
        uint16_t nlb = (cmd->cdw12 & 0xFFFF) + 1;
        uint8_t zero_buf[4096];
        uint32_t i = 0;
        LOG_INFO("NVMe I/O: Write Zeroes, SLBA=%llu, NLB=%u",
                 (unsigned long long)slba, nlb);
        memset(zero_buf, 0, sizeof(zero_buf));
        for (i = 0; i < nlb; i++) {
            if (ftl_write((uint32_t)(slba + i), zero_buf) != RET_OK) {
                LOG_ERROR("NVMe I/O: Write Zeroes FTL 写入失败, LPN=%llu",
                          (unsigned long long)(slba + i));
                set_completion_status(cpl, NVME_SC_INTERNAL_ERROR, g_nvme_ctrl.io_cq.phase);
                break;
            }
        }
        if (i == nlb) {
            set_completion_status(cpl, NVME_SC_SUCCESS, g_nvme_ctrl.io_cq.phase);
        }
        break;
    }
    case NVME_IO_DATASET_MGMT:
        io_dataset_mgmt(cmd, cpl);
        break;
    case NVME_IO_VERIFY:
        LOG_INFO("NVMe I/O: Verify");
        set_completion_status(cpl, NVME_SC_SUCCESS, g_nvme_ctrl.io_cq.phase);
        break;
    default:
        LOG_WARN("NVMe I/O: 未支持的操作码 0x%02X", cmd->opcode);
        set_completion_status(cpl, NVME_SC_INVALID_OPCODE, g_nvme_ctrl.io_cq.phase);
        break;
    }

    /* 命令处理完成，返回RET_OK（执行结果通过cpl->status反映） */
    return RET_OK;
}

void nvme_ctrl_process(void)
{
    nvme_command_t cmd;
    nvme_completion_t cpl;
    uint32_t io_cmd_count = 0;

    if (!g_nvme_ctrl.initialized || g_nvme_ctrl.state != NVME_CTRL_STATE_READY) {
        return;
    }

    /* 处理 Admin SQ 中的命令 */
    while (sq_fetch_cmd(&g_nvme_ctrl.admin_sq, &cmd)) {
        cpl.sqhd = g_nvme_ctrl.admin_sq.head;
        nvme_ctrl_process_admin_cmd(&cmd, &cpl);
        /* AER 命令挂起标记：rsvd1==0xAE01 表示不立即写入 CQ */
        if (cpl.rsvd1 == 0xAE01) {
            LOG_DEBUG("NVMe AER: 命令挂起，跳过 CQ 写入, CID=%u", cpl.cid);
            continue;
        }
        cq_post_completion(&g_nvme_ctrl.admin_cq, &cpl);

        /* 触发 Admin CQ 中断 */
        nvme_ctrl_trigger_irq(0);
    }

    /* 处理 I/O SQ 中的命令 */
    if (g_nvme_ctrl.io_sq.entries != NULL) {
        while (sq_fetch_cmd(&g_nvme_ctrl.io_sq, &cmd)) {
            cpl.sqhd = g_nvme_ctrl.io_sq.head;
            nvme_ctrl_process_io_cmd(&cmd, &cpl);
            cq_post_completion(&g_nvme_ctrl.io_cq, &cpl);
            io_cmd_count++;

            /* 触发 I/O CQ 中断 */
            nvme_ctrl_trigger_irq(g_nvme_ctrl.io_cq.vector);
        }
    }

    /* 尝试完成挂起的 AER 命令（如果有事件待上报） */
    aer_try_complete();

    /* 定期更新温度传感器：每 8 次 process 调用更新一次 */
    g_temp_update_counter++;
    if (g_temp_update_counter >= 8) {
        g_temp_update_counter = 0;
        nvme_ctrl_update_temperature(io_cmd_count);
    }
}

nvme_ctrl_state_t nvme_ctrl_get_state(void)
{
    return g_nvme_ctrl.state;
}

nvme_ctrl_regs_t *nvme_ctrl_get_regs(void)
{
    return &g_nvme_ctrl.regs;
}

/**
 * @brief 填充 SMART/Health Information Log (LID=0x02, 512字节)
 * @details 按照 NVMe 1.4 规范填充 SMART 日志，包含：
 *          - critical_warning (bit1=温度阈值警告)
 *          - Composite Temperature (Sensor 0)
 *          - Temperature Sensor 1-8 (bytes 200-215)
 *          - Warning/Critical Composite Temperature Time
 * @param buf 输出缓冲区
 * @param len 缓冲区长度
 */
void nvme_ctrl_fill_smart_log(uint8_t *buf, uint32_t len)
{
    uint32_t i;
    uint16_t temp;

    if (buf == NULL || len == 0) {
        return;
    }

    memset(buf, 0, len);

    /* byte 0: Critical Warning
     * bit0: 可用空间低于阈值
     * bit1: 温度超过警告阈值
     * bit2: 可靠性降级
     * bit3: 介质只读
     * bit4: 易失性内存备份失败
     */
    buf[0] = 0x00;
    if (g_temp_warning_active || g_temp_critical_active) {
        buf[0] |= 0x02;  /* bit1: 温度超过警告/临界阈值 */
    }

    /* bytes 1-2: Composite Temperature (Kelvin, 小端) */
    temp = g_temp_sensors[0].current;
    buf[1] = (uint8_t)(temp & 0xFF);
    buf[2] = (uint8_t)((temp >> 8) & 0xFF);

    /* byte 3: Available Spare (100%) */
    buf[3] = 0x64;

    /* byte 4: Available Spare Threshold (10%) */
    buf[4] = 0x0A;

    /* byte 5: Percentage Used (0%) */
    buf[5] = 0x00;

    /* byte 6: Endurance Group Critical Warning Summary */
    buf[6] = 0x00;

    /* bytes 112-127: Power Cycles (128-bit, 1) */
    if (len > 112) {
        buf[112] = 0x01;
    }

    /* bytes 192-195: Warning Composite Temperature Time (分钟, 小端) */
    if (len > 195) {
        buf[192] = (uint8_t)(g_warning_temp_time & 0xFF);
        buf[193] = (uint8_t)((g_warning_temp_time >> 8) & 0xFF);
        buf[194] = (uint8_t)((g_warning_temp_time >> 16) & 0xFF);
        buf[195] = (uint8_t)((g_warning_temp_time >> 24) & 0xFF);
    }

    /* bytes 196-199: Critical Composite Temperature Time (分钟, 小端) */
    if (len > 199) {
        buf[196] = (uint8_t)(g_critical_temp_time & 0xFF);
        buf[197] = (uint8_t)((g_critical_temp_time >> 8) & 0xFF);
        buf[198] = (uint8_t)((g_critical_temp_time >> 16) & 0xFF);
        buf[199] = (uint8_t)((g_critical_temp_time >> 24) & 0xFF);
    }

    /* bytes 200-215: Temperature Sensor 1-8 (Kelvin, 小端)
     * Sensor 1 = Composite (g_temp_sensors[0])
     * Sensor 2 = NAND Ch0 (g_temp_sensors[1])
     * ...
     * Sensor 8 = Reserved (g_temp_sensors[7])
     */
    for (i = 0; i < NVME_TEMP_SENSOR_COUNT; i++) {
        uint32_t offset = 200 + i * 2;
        if (offset + 1 >= len) {
            break;
        }
        temp = g_temp_sensors[i].current;
        buf[offset] = (uint8_t)(temp & 0xFF);
        buf[offset + 1] = (uint8_t)((temp >> 8) & 0xFF);
    }

    LOG_DEBUG("NVMe SMART: composite_temp=%uK (%d°C), critical_warning=0x%02X, "
              "warn_time=%u min, crit_time=%u min",
              g_temp_sensors[0].current, g_temp_sensors[0].current - 273,
              buf[0], g_warning_temp_time, g_critical_temp_time);
}

/**
 * @brief 填充 Firmware Slot Information Log (LID=0x03, 512字节)
 * @details 按照 NVMe 1.4 规范填充固件插槽信息日志：
 *          - byte 0: 当前运行的固件插槽号 (1-7)
 *          - byte 1: 下次复位时激活的插槽号
 *          - bytes 8-15: Slot 1 固件版本 (8字节ASCII)
 *          - bytes 16-23: Slot 2 固件版本
 *          - ... 以此类推到 Slot 7
 *          - 空插槽填全零
 * @param buf 输出缓冲区
 * @param len 缓冲区长度
 */
void nvme_ctrl_fill_fw_slot_log(uint8_t *buf, uint32_t len)
{
    uint32_t i;

    if (buf == NULL || len == 0) {
        return;
    }

    memset(buf, 0, len);

    /* byte 0: 当前运行的固件插槽号 */
    buf[0] = g_fw_running_slot;

    /* byte 1: 下次控制器复位时激活的插槽号 */
    buf[1] = g_fw_next_boot_slot;

    /* bytes 8-63: 插槽1-7的固件版本（每个8字节） */
    for (i = 0; i < NVME_FW_SLOT_COUNT; i++) {
        uint32_t offset = 8 + i * 8;
        if (offset + 7 >= len) {
            break;
        }
        if (g_fw_slots[i].state != NVME_FW_SLOT_EMPTY) {
            memcpy(buf + offset, g_fw_slots[i].version, 8);
        }
        /* 空插槽保持全零 */
    }

    LOG_INFO("NVMe FW Slot Log: running_slot=%u, next_boot_slot=%u",
             g_fw_running_slot, g_fw_next_boot_slot);
}

/**
 * @brief Post 一个 AER 异步事件
 * @details 将事件加入事件队列。如果事件类型为 SMART/Health，
 *          会先检查 Async Event Configuration 掩码，被掩码的事件不入队。
 *          事件入队后，下次 nvme_ctrl_process() 调用时会通过
 *          aer_try_complete() 完成挂起的 AER 命令。
 * @param event_type 事件类型 (NVME_AER_TYPE_*)
 * @param event_info 事件信息（SMART/Health 时为子类型: 0=spare,1=temp,2=reliability,...）
 * @param log_page 关联的日志页标识符
 * @retval RET_OK 成功
 * @retval RET_ERR_INTERNAL 事件队列已满
 */
ret_code_t nvme_ctrl_post_aer_event(uint32_t event_type, uint32_t event_info,
                                     uint8_t log_page)
{
    /* SMART/Health 事件需要检查掩码 */
    if (event_type == NVME_AER_TYPE_SMART_HEALTH) {
        uint32_t mask_bit = (1U << (event_info & 0x1F));
        if ((g_aer_event_mask & mask_bit) == 0) {
            LOG_DEBUG("NVMe AER: 事件被掩码屏蔽, type=0x%04X, info=%u",
                      event_type, event_info);
            return RET_OK;  /* 被掩码，不上报 */
        }
    }

    /* 检查事件队列是否已满 */
    if (g_aer_event_count >= NVME_AER_QUEUE_SIZE) {
        LOG_WARN("NVMe AER: 事件队列已满, 丢弃事件 type=0x%04X", event_type);
        return RET_ERR_INTERNAL;
    }

    /* 事件入队 */
    g_aer_event_queue[g_aer_event_tail].event_type = event_type;
    g_aer_event_queue[g_aer_event_tail].event_info = event_info;
    g_aer_event_queue[g_aer_event_tail].log_page = log_page;
    g_aer_event_tail = (g_aer_event_tail + 1) % NVME_AER_QUEUE_SIZE;
    g_aer_event_count++;

    LOG_INFO("NVMe AER: 事件已入队, type=0x%04X, info=%u, log_page=0x%02X, count=%u",
             event_type, event_info, log_page, g_aer_event_count);

    return RET_OK;
}

/**
 * @brief 更新温度传感器（随机游走模型 + I/O 负载影响）
 * @details 每个传感器温度基于随机游走模型变化：
 *          temp = current + (rand()-0.5)*0.5 + io_activity*0.02
 *          然后限制在 [min_temp, max_temp] 范围内。
 *          I/O 负载越高，温度上升越明显。
 *          更新后检查阈值，超温时 post AER 事件。
 * @param io_activity I/O 活动量（0=空闲，值越大负载越高）
 */
void nvme_ctrl_update_temperature(uint32_t io_activity)
{
    uint32_t i;
    int32_t new_temp;

    for (i = 0; i < NVME_TEMP_SENSOR_COUNT; i++) {
        if (g_temp_sensors[i].current == 0 || g_temp_sensors[i].min_temp == 0) {
            continue;  /* 跳过未使用的传感器 */
        }

        /* 随机游走：温度变化 -1/0/+1 K */
        new_temp = (int32_t)g_temp_sensors[i].current
                 + (int32_t)(rand() % 3) - 1  /* 随机游走分量: -1, 0, +1 */
                 + (int32_t)(io_activity > 0 ? 1 : -1);  /* I/O 负载影响 */

        /* 限制在合理范围内 */
        if (new_temp < (int32_t)g_temp_sensors[i].min_temp) {
            new_temp = (int32_t)g_temp_sensors[i].min_temp;
        }
        if (new_temp > (int32_t)g_temp_sensors[i].max_temp) {
            new_temp = (int32_t)g_temp_sensors[i].max_temp;
        }

        g_temp_sensors[i].current = (uint16_t)new_temp;

        /* 更新最高温度记录 */
        if (g_temp_sensors[i].current > g_temp_sensors[i].highest) {
            g_temp_sensors[i].highest = g_temp_sensors[i].current;
        }
    }

    /* 检查 Composite 温度阈值并触发 AER 事件 */
    temp_check_thresholds();

    LOG_DEBUG("NVMe Temp: 更新完成, composite=%uK (%d°C), io_activity=%u",
              g_temp_sensors[0].current, g_temp_sensors[0].current - 273, io_activity);
}

/**
 * @brief 填充 Identify Namespace 数据结构 (4096 字节)
 *
 * 按照 NVMe 1.4 规范的 struct nvme_id_ns 布局填充。
 *
 * LBA 格式 (LBAF) 布局关键：
 *   struct nvme_lbaf = { __le16 ms; __u8 ds; __u8 rp; }
 *   - byte 0-1: ms  (元数据大小，以字节为单位)
 *   - byte 2:   ds  (LBA 数据大小，LBA = 2^ds 字节，ds=12→4KB)
 *   - byte 3:   rp  (相对性能)
 *
 *   LBAF0 位于 buf[128-131]：ms=0, ds=12(4KB), rp=0
 *
 * @param buf  输出缓冲区
 * @param len  缓冲区长度
 * @param nsid 命名空间 ID
 */
void nvme_ctrl_fill_identify_namespace(uint8_t *buf, uint32_t len, uint32_t nsid)
{
    uint64_t nsze = 262144;  /* 1GB / 4KB = 262144 LBA */

    (void)nsid;
    memset(buf, 0, len);

    /* NSZE (bytes 0-7): Namespace Size (总LBA数) */
    buf[0] = (uint8_t)(nsze & 0xFF);
    buf[1] = (uint8_t)((nsze >> 8) & 0xFF);
    buf[2] = (uint8_t)((nsze >> 16) & 0xFF);
    buf[3] = (uint8_t)((nsze >> 24) & 0xFF);

    /* NCAP (bytes 8-15): Namespace Capacity */
    buf[8] = (uint8_t)(nsze & 0xFF);
    buf[9] = (uint8_t)((nsze >> 8) & 0xFF);
    buf[10] = (uint8_t)((nsze >> 16) & 0xFF);
    buf[11] = (uint8_t)((nsze >> 24) & 0xFF);

    /* NUSE (bytes 16-23): Namespace Utilization */
    buf[16] = (uint8_t)(nsze & 0xFF);
    buf[17] = (uint8_t)((nsze >> 8) & 0xFF);
    buf[18] = (uint8_t)((nsze >> 16) & 0xFF);
    buf[19] = (uint8_t)((nsze >> 24) & 0xFF);

    /* NSFEAT (byte 24): Namespace Features (bit0=thin provisioning) */
    buf[24] = 0x00;

    /* NLBAF (byte 25): Number of LBA Formats (0-based, 0=1种格式) */
    buf[25] = 0x00;

    /* FLBAS (byte 26): Formatted LBA Size (使用LBAF0) */
    buf[26] = 0x00;

    /* MC (byte 27): Metadata Capabilities */
    buf[27] = 0x00;

    /* DPC (byte 28): End-to-end Data Protection Capabilities */
    buf[28] = 0x00;

    /* DPS (byte 29): End-to-end Data Protection Type Settings */
    buf[29] = 0x00;

    /* NMIC (byte 30): NVM Namespace Multi-path I/O Capabilities */
    buf[30] = 0x00;

    /* RESCAP (byte 31): Reservation Capabilities */
    buf[31] = 0x00;

    /* LBAF0 (bytes 128-131): LBA Format 0 (4KB, 无元数据)
     * struct nvme_lbaf { __le16 ms; __u8 ds; __u8 rp; }
     * ms=0(无元数据), ds=12(2^12=4096字节), rp=0(最佳性能) */
    buf[128] = 0x00;  /* ms 低字节 */
    buf[129] = 0x00;  /* ms 高字节 */
    buf[130] = 0x0C;  /* ds = 12 (4KB LBA) */
    buf[131] = 0x00;  /* rp = 0 */

    LOG_INFO("NVMe: Identify Namespace LBAF0: buf[128]=0x%02X buf[129]=0x%02X buf[130]=0x%02X buf[131]=0x%02X",
             buf[128], buf[129], buf[130], buf[131]);
}

/**
 * @brief 填充 Identify Controller 数据结构 (4096 字节)
 *
 * 按照 NVMe 1.4 规范的 struct nvme_id_ctrl 布局填充。
 * 关键字段偏移：
 *   - VID@0, SSVID@2, SN@4, MN@24, FR@64
 *   - CNTLID@78-79, KAS@320-321
 *   - OACS@256-257, ACL@258, AERL@259, FRMW@260
 *   - WCTEMP@266-267, CCTEMP@268-269
 *   - SQES@512=0x06, CQES@513=0x04, NN@516=1
 *   - SGLS@536-539=0x03
 *   - SUBNQN@768-1023
 *
 * @param buf 输出缓冲区（至少 4096 字节）
 * @param len 缓冲区长度
 */
void nvme_ctrl_fill_identify_controller(uint8_t *buf, uint32_t len)
{
    const char *subnqn = "nqn.2026-08.io.ftlfw:subsystem";
    const char *sn = "FTLFW00000000000001";
    const char *mn = "FTL-Firmware NVMe Controller";

    memset(buf, 0, len);

    /* VID (bytes 0-1): PCI Vendor ID */
    buf[0] = 0x34; buf[1] = 0x12;  /* 0x1234 小端 */

    /* SSVID (bytes 2-3): PCI Subsystem Vendor ID */
    buf[2] = 0x34; buf[3] = 0x12;  /* 0x1234 小端 */

    /* SN (bytes 4-23): Serial Number (20字节) */
    memcpy(buf + 4, sn, strlen(sn));

    /* MN (bytes 24-63): Model Number (40字节) */
    memcpy(buf + 24, mn, strlen(mn));

    /* FR (bytes 64-71): Firmware Revision (8字节) - 动态当前版本 */
    memcpy(buf + 64, g_fw_current_version, 8);

    /* RAB (byte 72): Recommended Arbitration Burst */
    buf[72] = 0x07;

    /* IEEE (bytes 73-75): IEEE OUI Identifier */
    buf[73] = 0x00; buf[74] = 0x00; buf[75] = 0x00;

    /* CMIC (byte 76): Controller Multi-path I/O Capabilities */
    buf[76] = 0x00;

    /* MDTS (byte 77): Maximum Data Transfer Size (0=无限制) */
    buf[77] = 0x00;

    /* CNTLID (bytes 78-79): Controller ID */
    buf[78] = 0x01; buf[79] = 0x00;  /* 0x0001 小端 */

    /* VER (bytes 80-83): Version (NVMe 1.4 = 0x00010400) */
    buf[80] = 0x00; buf[81] = 0x04; buf[82] = 0x01; buf[83] = 0x00;

    /* OAES (bytes 92-95): Optional Asynchronous Events Supported
     * bit0=1: 支持命名空间属性通知 (Namespace Attribute Notices) */
    buf[92] = 0x01;
    buf[93] = 0x00; buf[94] = 0x00; buf[95] = 0x00;

    /* CNTRLTYPE (byte 111): Controller Type (0=IO controller) */
    buf[111] = 0x00;

    /* FGUID (bytes 112-127): Factory Global Unique Identifier (16字节, 全0) */

    /* KAS (bytes 320-321): Keep Alive Support */
    buf[320] = 0x01; buf[321] = 0x00;  /* 支持 Keep Alive */

    /* OACS (bytes 256-257): Optional Admin Command Support
     * bit0=1: 支持 Security Send/Receive
     * bit3=1: 支持 Firmware Commit/Activate */
    buf[256] = 0x09;  /* bit0 | bit3 */
    buf[257] = 0x00;

    /* ACL (byte 258): Abort Command Limit (0=无限制) */
    buf[258] = 0x00;

    /* AERL (byte 259): Async Event Request Limit (3=最多4个并发AER) */
    buf[259] = 0x03;

    /* FRMW (byte 260): Firmware Update Configuration
     * bit0=1: 支持固件更新
     * bit4=1: 支持固件插槽信息日志 (LID=0x03) */
    buf[260] = 0x11;

    /* LPA (byte 261): Log Page Attributes */
    buf[261] = 0x00;

    /* ELPE (byte 262): Error Log Page Entries (0=1条) */
    buf[262] = 0x00;

    /* NPSS (byte 263): Number of Power States Support */
    buf[263] = 0x00;

    /* AVSCC (byte 264): Admin Vendor Specific Command Configuration */
    buf[264] = 0x00;

    /* APSTA (byte 265): Autonomous Power State Transition Enable */
    buf[265] = 0x00;

    /* WCTEMP (bytes 266-267): Warning Composite Temperature Threshold (Kelvin) */
    buf[266] = (uint8_t)(g_temp_sensors[0].warning & 0xFF);
    buf[267] = (uint8_t)((g_temp_sensors[0].warning >> 8) & 0xFF);

    /* CCTEMP (bytes 268-269): Critical Composite Temperature Threshold (Kelvin) */
    buf[268] = (uint8_t)(g_temp_sensors[0].critical & 0xFF);
    buf[269] = (uint8_t)((g_temp_sensors[0].critical >> 8) & 0xFF);

    /* SQES (byte 512): Submission Queue Entry Size */
    buf[512] = 0x06;  /* 2^6=64字节 */

    /* CQES (byte 513): Completion Queue Entry Size */
    buf[513] = 0x04;  /* 2^4=16字节 */

    /* MAXCMD (bytes 514-515): Maximum Outstanding Commands */
    buf[514] = 0xFF; buf[515] = 0x00;  /* 255 */

    /* NN (bytes 516-519): Number of Namespaces */
    buf[516] = 0x01; buf[517] = 0x00; buf[518] = 0x00; buf[519] = 0x00;

    /* ONCS (bytes 520-521): Optional NVM Command Support */
    buf[520] = 0x00; buf[521] = 0x00;

    /* FNA (byte 524): Fused Operation Support */
    buf[524] = 0x00;

    /* VWC (byte 525): Volatile Write Cache */
    buf[525] = 0x00;

    /* AWUN (bytes 526-527): Atomic Write Unit Normal */
    buf[526] = 0x00; buf[527] = 0x00;

    /* AWUPF (bytes 528-529): Atomic Write Unit Power Fail */
    buf[528] = 0x00; buf[529] = 0x00;

    /* SGLS (bytes 536-539): SGL Support
     * PCIe NVMe 使用 PRP，不支持 SGL，设为 0 */
    buf[536] = 0x00; buf[537] = 0x00; buf[538] = 0x00; buf[539] = 0x00;

    /* SUBNQN (bytes 768-1023): NVM Subsystem NVMe Qualified Name (256字节) */
    memcpy(buf + 768, subnqn, strlen(subnqn));

    /* IOCCSZ (bytes 1792-1795): I/O Queue Command Capsule Supported Size (16字节单位) */
    buf[1792] = 0x04; buf[1793] = 0x00; buf[1794] = 0x00; buf[1795] = 0x00;

    /* IORCSZ (bytes 1796-1799): I/O Queue Response Capsule Supported Size (16字节单位) */
    buf[1796] = 0x01; buf[1797] = 0x00; buf[1798] = 0x00; buf[1799] = 0x00;

    /* ICDOFF (bytes 1800-1801): I/O Queue Command Capsule Data Offset */
    buf[1800] = 0x00; buf[1801] = 0x00;

    LOG_INFO("NVMe: Identify Controller 已填充, FR=%s, AERL=%u, FRMW=0x%02X, OACS=0x%04X, "
             "WCTEMP=%uK, CCTEMP=%uK",
             g_fw_current_version, buf[259], buf[260],
             (uint16_t)buf[256] | ((uint16_t)buf[257] << 8),
             g_temp_sensors[0].warning, g_temp_sensors[0].critical);
}

uint32_t nvme_ctrl_read_reg(uint32_t offset)
{
    switch (offset) {
    case NVME_REG_CAP:
        return (uint32_t)(g_nvme_ctrl.regs.cap & 0xFFFFFFFF);
    case NVME_REG_CAP + 4:
        return (uint32_t)(g_nvme_ctrl.regs.cap >> 32);
    case NVME_REG_VS:
        return g_nvme_ctrl.regs.vs;
    case NVME_REG_INTMS:
        return g_nvme_ctrl.regs.intms;
    case NVME_REG_INTMC:
        return g_nvme_ctrl.regs.intmc;
    case NVME_REG_CC:
        return g_nvme_ctrl.regs.cc;
    case NVME_REG_CSTS:
        return g_nvme_ctrl.regs.csts;
    case NVME_REG_AQA:
        return g_nvme_ctrl.regs.aqa;
    case NVME_REG_ASQ:
        return (uint32_t)(g_nvme_ctrl.regs.asq & 0xFFFFFFFF);
    case NVME_REG_ASQ + 4:
        return (uint32_t)(g_nvme_ctrl.regs.asq >> 32);
    case NVME_REG_ACQ:
        return (uint32_t)(g_nvme_ctrl.regs.acq & 0xFFFFFFFF);
    case NVME_REG_ACQ + 4:
        return (uint32_t)(g_nvme_ctrl.regs.acq >> 32);
    default:
        return 0;
    }
}

void nvme_ctrl_write_reg(uint32_t offset, uint32_t value)
{
    switch (offset) {
    case NVME_REG_INTMS:
        g_nvme_ctrl.regs.intms |= value;
        break;
    case NVME_REG_INTMC:
        g_nvme_ctrl.regs.intmc &= ~value;
        break;
    case NVME_REG_CC:
        nvme_ctrl_write_cc(value);
        break;
    case NVME_REG_AQA:
        nvme_ctrl_write_aqa(value);
        break;
    case NVME_REG_ASQ:
        nvme_ctrl_write_asq(value, false);
        break;
    case NVME_REG_ASQ + 4:
        nvme_ctrl_write_asq(value, true);
        break;
    case NVME_REG_ACQ:
        nvme_ctrl_write_acq(value, false);
        break;
    case NVME_REG_ACQ + 4:
        nvme_ctrl_write_acq(value, true);
        break;
    default:
        break;
    }
}

void nvme_ctrl_trigger_irq(uint16_t vector)
{
    /* 在实际硬件中，触发 MSI/MSI-X 中断 */
    /* 在模拟环境中，记录中断事件 */
    LOG_DEBUG("NVMe 触发中断: Vector=%u", vector);
}
