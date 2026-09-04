/**
 * @file main.c
 * @brief FTL 固件主程序入口
 * @details 企业级 FTL 固件的主程序入口，初始化所有模块并运行主循环
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include "common/common.h"
#include "msg_queue.h"
#include "utils.h"
#include "log.h"
#include "nand.h"
#include "ftl.h"
#include "host_if.h"
#include "manager.h"
#include "thread.h"
#include "dma.h"
#include "raid.h"
#include "protocol/nvme_tcp_target.h"
#include "protocol/nvme_controller.h"
#include "protocol/ufs_target.h"
#include "hal/os_abstract.h"

/* ============================================================
 *  全局配置
 * ============================================================ */

/** @brief FTL 元数据快照文件路径（掉电保护持久化） */
#define FTL_SNAPSHOT_FILE  "ftl_snapshot.bin"



/* ============================================================
 *  任务函数前向声明（FreeRTOS 风格任务入口）
 *  每个任务独立线程，通过消息队列通信，避免共享数据竞争
 * ============================================================ */
static void task_nvme_tcp_service(void *arg);    ///< NVMe/TCP 前端接口服务任务（高优先级）
static void task_heartbeat_monitor(void *arg);     ///< 心跳与健康监控任务（普通优先级）
static void task_ftl_unit_test(void *arg);         ///< FTL 单元测试任务（低优先级，后台验证）
static void task_gc_benchmark(void *arg);          ///< GC 算法基准测试任务（低优先级，后台分析）
static void task_status_monitor(void *arg);         ///< 任务状态监控任务（空闲优先级，调试用）
/* ============================================================
 *  任务管理框架（FreeRTOS 风格）
 *
 *  设计参考 FreeRTOS 任务调度：
 *  - 每个任务有独立的栈、优先级、名称
 *  - 任务通过 os_thread_create 创建，由 OS 调度器调度
 *  - 任务间通过消息队列(msg_queue)通信，避免共享数据竞争
 *  - 任务状态：READY/RUNNING/BLOCKED/SUSPENDED
 * ============================================================ */

/** @brief 任务优先级定义（数值越大优先级越高，参考 FreeRTOS） */
typedef enum {
    TASK_PRIORITY_IDLE      = 0,   ///< 空闲任务（最低）
    TASK_PRIORITY_LOW       = 1,   ///< 低优先级（后台任务、测试任务）
    TASK_PRIORITY_NORMAL    = 2,   ///< 普通优先级（常规业务）
    TASK_PRIORITY_HIGH      = 3,   ///< 高优先级（NVMe/TCP 服务）
    TASK_PRIORITY_REALTIME  = 4,   ///< 实时优先级（中断处理、看门狗）
} task_priority_t;

/** @brief 任务状态 */
typedef enum {
    TASK_STATE_READY = 0,    ///< 就绪，等待调度
    TASK_STATE_RUNNING,      ///< 运行中
    TASK_STATE_BLOCKED,      ///< 阻塞（等待消息/延时）
    TASK_STATE_SUSPENDED,    ///< 挂起
    TASK_STATE_FINISHED,     ///< 已结束
} task_state_t;

/** @brief 任务控制块（TCB, Task Control Block），参考 FreeRTOS TCB */
typedef struct {
    const char        *name;          ///< 任务名称（调试用）
    os_thread_func_t  entry;          ///< 任务入口函数
    void              *arg;           ///< 任务参数
    task_priority_t   priority;       ///< 任务优先级
    uint32_t          stack_size;     ///< 栈大小（字节）
    os_thread_t       handle;         ///< 线程句柄（OS 抽象层）
    task_state_t      state;          ///< 任务状态
    uint32_t          run_count;      ///< 运行次数统计（调试用）
} task_tcb_t;

/** @brief 全局任务表（静态分配，避免动态内存碎片） */
#define MAX_TASKS  16
static task_tcb_t g_task_table[MAX_TASKS];
static uint32_t   g_task_count = 0;

/**
 * @brief 注册任务到任务表
 * @param[in] name       任务名称
 * @param[in] entry      任务入口函数
 * @param[in] arg        任务参数
 * @param[in] priority   任务优先级
 * @param[in] stack_size 栈大小
 * @return 任务索引，失败返回 -1
 */
static int task_register(const char *name, os_thread_func_t entry, void *arg,
                          task_priority_t priority, uint32_t stack_size)
{
    if (g_task_count >= MAX_TASKS) {
        printf("[任务管理] 任务表已满，无法注册任务 %s\n", name);
        return -1;
    }
    task_tcb_t *tcb = &g_task_table[g_task_count];
    tcb->name       = name;
    tcb->entry      = entry;
    tcb->arg        = arg;
    tcb->priority   = priority;
    tcb->stack_size = stack_size;
    tcb->handle     = NULL;
    tcb->state      = TASK_STATE_READY;
    tcb->run_count  = 0;
    printf("[任务管理] 注册任务: %s (优先级=%d, 栈=%u字节)\n",
           name, priority, stack_size);
    return (int)g_task_count++;
}

/**
 * @brief 启动所有已注册任务（创建线程）
 * @return 成功启动的任务数
 */
static uint32_t task_start_all(void)
{
    uint32_t started = 0;
    uint32_t i = 0;
    printf("\n[任务管理] 启动 %u 个任务...\n", g_task_count);
    for (i = 0; i < g_task_count; i++) {
        task_tcb_t *tcb = &g_task_table[i];
        tcb->handle = os_thread_create(tcb->entry, tcb->arg, tcb->name,
                                        (uint32_t)tcb->priority, tcb->stack_size);
        if (tcb->handle != NULL) {
            tcb->state = TASK_STATE_RUNNING;
            started++;
            printf("[任务管理] 任务 %s 启动成功\n", tcb->name);
        } else {
            printf("[任务管理] 任务 %s 启动失败\n", tcb->name);
        }
    }
    printf("[任务管理] 成功启动 %u/%u 个任务\n\n", started, g_task_count);
    return started;
}

/**
 * @brief 打印所有任务状态（调试用，类似 FreeRTOS task list）
 */
static void task_print_status(void)
{
    uint32_t i = 0;
    printf("\n========================================\n");
    printf("  任务状态表\n");
    printf("========================================\n");
    printf("  %-20s %-8s %-10s %-8s\n", "任务名", "优先级", "状态", "运行次数");
    printf("  ----------------------------------------\n");
    for (i = 0; i < g_task_count; i++) {
        task_tcb_t *tcb = &g_task_table[i];
        const char *state_str = "UNKNOWN";
        switch (tcb->state) {
        case TASK_STATE_READY:     state_str = "READY"; break;
        case TASK_STATE_RUNNING:   state_str = "RUNNING"; break;
        case TASK_STATE_BLOCKED:   state_str = "BLOCKED"; break;
        case TASK_STATE_SUSPENDED: state_str = "SUSPENDED"; break;
        case TASK_STATE_FINISHED:  state_str = "FINISHED"; break;
        }
        printf("  %-20s %-8d %-10s %-8u\n",
               tcb->name, tcb->priority, state_str, tcb->run_count);
    }
    printf("========================================\n\n");
}
/** @brief 快照保存间隔（每 N 次主循环保存一次） */
#define FTL_SNAPSHOT_INTERVAL  5000U

/**
 * @brief 固件默认配置
 */
static const firmware_config_t g_default_fw_config = {
    .heartbeat_interval_ms = 1000U,
    .health_check_interval_ms = 5000U,
    .watchdog_timeout_ms = 10000U,
    .max_error_count = 10U,
    .auto_recovery = true
};

/**
 * @brief 主机接口默认配置
 */
static const host_if_config_t g_default_host_config = {
    .queue_size = 64U,
    .max_cmd = 32U,
    .lba_size = 4096U,
    .total_lbas = 1024U * 1024U,
    .is_nvm = true
};

/* ============================================================
 *  主循环
 * ============================================================ */

/**
 * @brief 固件主循环
 * @return 0 正常退出，-1 异常退出
 */
static int __attribute__((unused)) firmware_main_loop(void)
{
    int result = 0;
    uint32_t loop_count = 0U;

    printf("\n[固件] 进入主循环（NVMe/TCP 服务模式）...\n\n");

    /* 主循环：无限运行，直到收到信号 */
    while (1) {
        /* 管理模块处理 */
        manager_process();

        /* 主机接口处理（包含 NVMe/TCP 目标端） */
        host_if_process();

        /* NVMe/TCP 目标端处理 */
        nvme_tcp_target_process();

        /* 更新模块心跳（防止看门狗超时） */
        manager_send_heartbeat(MODULE_NAND);
        manager_send_heartbeat(MODULE_FTL);
        manager_send_heartbeat(MODULE_HOST_IF);
        manager_send_heartbeat(MODULE_LOG);

        /* 打印状态（每 10000 次循环打印一次） */
        loop_count++;
        if (loop_count % 10000U == 0U) {
            printf("[固件] 主循环运行中... 循环次数: %u\n", loop_count);
            manager_print_module_status();
            printf("\n");
        }

        /* 掉电保护：定期保存 FTL 元数据快照
         * 每 FTL_SNAPSHOT_INTERVAL 次循环保存一次，确保掉电后能恢复 */
        if (loop_count % FTL_SNAPSHOT_INTERVAL == 0U) {
            ret_code_t snap_ret = ftl_save_snapshot(FTL_SNAPSHOT_FILE);
            if (snap_ret != RET_OK) {
                LOG_WARN("FTL 快照保存失败: ret=%d", snap_ret);
            }
        }

        /* 短暂休眠，减少 CPU 占用 */
        usleep(100);  /* 100 微秒 */
    }

    return result;
}

/* ============================================================
 *  模块初始化
 * ============================================================ */

/* 全局日志级别（默认 WARN，高性能模式） */
static log_level_t g_log_level = LOG_LEVEL_WARN;

/* ============================================================
 *  任务函数实现（FreeRTOS 风格任务入口）
 *  每个任务独立线程，通过消息队列通信，避免共享数据竞争
 * ============================================================ */

/**
 * @brief NVMe/TCP 前端接口服务任务（高优先级，核心业务）
 * @param[in] arg 任务参数（未使用）
 * @details 持续调用 nvme_tcp_target_process 处理主机命令，
 *          这是固件的核心业务任务，优先级最高
 */
static void task_nvme_tcp_service(void *arg)
{
    (void)arg;
    printf("[任务] NVMe-TCP-Service 启动 (优先级=HIGH)\n");

    /* 持续处理 NVMe/TCP 主机命令，直到程序退出
     * 采用忙轮询模式（Busy Polling），与真实 SSD 固件前端处理器一致：
     * 真实 SSD 固件的 NVMe 前端控制器持续轮询提交队列，不会主动休眠，
     * 以确保最低延迟和最高 IOPS。多线程环境下，OS 调度器会自动
     * 在该任务和其他低优先级任务之间切换 CPU 时间。 */
    while (1) {
        /* 管理模块处理（健康监控、错误恢复、看门狗） */
        manager_process();

        /* 主机接口处理（提交队列/完成队列命令处理，性能关键路径）
         * 必须与nvme_tcp_target_process()配合调用，否则命令处理不完整 */
        host_if_process();

        /* NVMe/TCP 目标端处理（TCP连接、PDU收发、Capsule解析） */
        nvme_tcp_target_process();

        /* 更新模块心跳（防止看门狗超时） */
        manager_send_heartbeat(MODULE_NAND);
        manager_send_heartbeat(MODULE_FTL);
        manager_send_heartbeat(MODULE_HOST_IF);
        manager_send_heartbeat(MODULE_LOG);

        /* 短暂休眠100微秒，减少CPU占用，与单线程版本一致
         * 100us休眠对性能影响极小，同时避免CPU 100%占用导致
         * 内核网络软中断(ksoftirqd)得不到调度 */
        usleep(100);
    }
}

/**
 * @brief 心跳与健康监控任务（普通优先级，后台管理）
 * @param[in] arg 任务参数（未使用）
 * @details 定期输出 FTL 统计信息（WAF、GC次数、已使用页），
 *          每10秒保存一次 PLP 快照（掉电保护），每5秒输出一次心跳
 */
static void task_heartbeat_monitor(void *arg)
{
    (void)arg;
    uint32_t tick = 0;
    ftl_stats_t stats;

    printf("[任务] Heartbeat-Monitor 启动 (优先级=NORMAL)\n");

    while (1) {
        tick++;

        /* 每5秒输出一次 FTL 统计信息 */
        if (tick % 5 == 0) {
            if (ftl_get_stats(&stats) == RET_OK) {
                printf("[心跳] FTL统计: 已用页=%u/%u, GC次数=%u, WAF=%.2f, 主机写=%u, NAND写=%u\n",
                       stats.used_lpns, stats.total_lpns,
                       stats.gc_count, stats.waf,
                       stats.host_write_pages, stats.nand_write_pages);
            }
        }

        /* 每10秒保存一次 PLP 快照（掉电保护持久化） */
        if (tick % 10 == 0) {
            ret_code_t ret = ftl_save_snapshot(FTL_SNAPSHOT_FILE);
            if (ret != RET_OK) {
                printf("[心跳] 警告: PLP快照保存失败 (ret=%d)\n", ret);
            }
        }

        /* 休眠1秒 */
        os_delay_ms(1000);
    }
}

/**
 * @brief FTL 单元测试任务（低优先级，后台验证）
 * @param[in] arg 任务参数（未使用）
 * @details 从 tests/test_ftl_unit.c 提取核心测试项，
 *          在后台低优先级运行，验证 FTL 层功能正确性：
 *          1. 初始化状态检查
 *          2. 单页读写一致性
 *          3. 覆盖写验证（新数据覆盖旧数据）
 */
static void task_ftl_unit_test(void *arg)
{
    (void)arg;
    uint8_t write_buf[4096];
    uint8_t read_buf[4096];
    int pass = 1;
    ftl_stats_t stats;

    printf("[任务] FTL-Unit-Test 启动 (优先级=LOW)\n");

    /* ---------- 测试1: 初始化状态检查 ---------- */
    printf("[FTL测试] 1. 初始化状态检查\n");
    if (ftl_get_stats(&stats) != RET_OK) {
        printf("  [FAIL] ftl_get_stats 失败\n");
        pass = 0;
    } else if (stats.total_lpns == 0) {
        printf("  [FAIL] 总逻辑页数为0\n");
        pass = 0;
    } else {
        printf("  [PASS] 初始化正常: 总LPN=%u\n", stats.total_lpns);
    }

    /* ---------- 测试2: 单页读写一致性 ---------- */
    printf("[FTL测试] 2. 单页读写一致性\n");
    /* 填充测试数据：0xA5 模式 */
    memset(write_buf, 0xA5, sizeof(write_buf));
    memset(read_buf, 0x00, sizeof(read_buf));

    if (ftl_write(100, write_buf) != RET_OK) {
        printf("  [FAIL] 写入 LPN=100 失败\n");
        pass = 0;
    } else if (ftl_read(100, read_buf) != RET_OK) {
        printf("  [FAIL] 读取 LPN=100 失败\n");
        pass = 0;
    } else if (memcmp(write_buf, read_buf, sizeof(write_buf)) != 0) {
        printf("  [FAIL] 读写数据不一致\n");
        pass = 0;
    } else {
        printf("  [PASS] 单页读写一致 (LPN=100)\n");
    }

    /* ---------- 测试3: 覆盖写验证 ---------- */
    printf("[FTL测试] 3. 覆盖写验证\n");
    /* 用新数据 0x55 覆盖旧数据 0xA5 */
    memset(write_buf, 0x55, sizeof(write_buf));
    memset(read_buf, 0x00, sizeof(read_buf));

    if (ftl_write(100, write_buf) != RET_OK) {
        printf("  [FAIL] 覆盖写入 LPN=100 失败\n");
        pass = 0;
    } else if (ftl_read(100, read_buf) != RET_OK) {
        printf("  [FAIL] 覆盖后读取 LPN=100 失败\n");
        pass = 0;
    } else if (read_buf[0] != 0x55) {
        printf("  [FAIL] 覆盖后读取到旧数据 (expected 0x55, got 0x%02X)\n", read_buf[0]);
        pass = 0;
    } else {
        printf("  [PASS] 覆盖写验证通过 (新数据=0x55)\n");
    }

    /* 输出测试结果 */
    printf("[FTL测试] 结果: %s\n\n", pass ? "全部通过" : "存在失败");

    /* 测试任务完成后进入空闲循环（不退出线程） */
    while (1) {
        os_delay_ms(60000);  /* 每60秒唤醒一次，保持线程存活 */
    }
}

/**
 * @brief GC 算法基准测试任务（低优先级，后台性能分析）
 * @param[in] arg 任务参数（未使用）
 * @details 从 tests/test_gc_benchmark.c 提取核心逻辑，
 *          在后台低优先级运行，测试 GC 算法性能：
 *          1. 顺序写入100页填充数据
 *          2. 触发一次 GC
 *          3. 统计 GC 次数和已使用页变化
 */
static void task_gc_benchmark(void *arg)
{
    (void)arg;
    uint8_t write_buf[4096];
    uint32_t i = 0;
    uint32_t gc_before = 0;
    uint32_t gc_after = 0;
    ftl_stats_t stats;

    printf("[任务] GC-Benchmark 启动 (优先级=LOW)\n");

    /* 填充测试数据 */
    memset(write_buf, 0xCC, sizeof(write_buf));

    /* ---------- 阶段1: 顺序写入100页 ---------- */
    printf("[GC测试] 阶段1: 顺序写入 100 页...\n");
    for (i = 0; i < 100; i++) {
        if (ftl_write(i, write_buf) != RET_OK) {
            printf("  [WARN] 写入 LPN=%u 失败\n", i);
        }
    }
    printf("  完成: 写入 100 页\n");

    /* 记录 GC 前的统计 */
    if (ftl_get_stats(&stats) == RET_OK) {
        gc_before = stats.gc_count;
        printf("  GC前: GC次数=%u, 已用页=%u\n", stats.gc_count, stats.used_lpns);
    }

    /* ---------- 阶段2: 触发 GC ---------- */
    printf("[GC测试] 阶段2: 触发 GC...\n");
    ret_code_t ret = ftl_trigger_gc();
    if (ret == RET_OK) {
        printf("  GC 触发成功\n");
    } else {
        printf("  GC 触发返回: ret=%d (可能无需GC)\n", ret);
    }

    /* 记录 GC 后的统计 */
    if (ftl_get_stats(&stats) == RET_OK) {
        gc_after = stats.gc_count;
        printf("  GC后: GC次数=%u, 已用页=%u, 搬迁页数=%u\n",
               stats.gc_count, stats.used_lpns, stats.gc_moved_pages);
    }

    /* 输出基准测试结果 */
    printf("[GC测试] 结果: GC执行次数=%u, 搬迁页数=%u\n\n",
           gc_after - gc_before,
           (ftl_get_stats(&stats) == RET_OK) ? stats.gc_moved_pages : 0);

    /* 测试任务完成后进入空闲循环 */
    while (1) {
        os_delay_ms(60000);
    }
}

/**
 * @brief 任务状态监控任务（空闲优先级，调试用）
 * @param[in] arg 任务参数（未使用）
 * @details 每30秒打印一次所有任务的状态表，
 *          包括任务名、优先级、状态、运行次数，
 *          用于调试和监控系统运行状态
 */
static void task_status_monitor(void *arg)
{
    (void)arg;
    uint32_t tick = 0;

    printf("[任务] Task-Monitor 启动 (优先级=IDLE)\n");

    while (1) {
        tick++;

        /* 每30秒打印一次任务状态表 */
        if (tick % 30 == 0) {
            task_print_status();
        }

        os_delay_ms(1000);
    }
}

/**
 * @brief 初始化所有模块
 * @retval RET_OK 成功
 * @retval RET_ERR_INTERNAL 内部错误
 */
static ret_code_t init_all_modules(void)
{
    ret_code_t ret = RET_OK;

    printf("[固件] 开始初始化所有模块...\n\n");

    /* 初始化消息队列 */
    printf("[固件] 初始化消息队列...\n");
    msg_queue_init(MODULE_NAND, 64U);
    msg_queue_init(MODULE_FTL, 64U);
    msg_queue_init(MODULE_HOST_IF, 64U);
    msg_queue_init(MODULE_MANAGER, 64U);
    msg_queue_init(MODULE_LOG, 64U);
    printf("[固件] 消息队列初始化完成\n\n");

    /* 初始化日志模块 */
    printf("[固件] 初始化日志模块...\n");
    log_set_level(g_log_level);
    printf("[固件] 日志模块初始化完成\n\n");

    /* 初始化 NAND 模块 */
    printf("[固件] 初始化 NAND 模块...\n");
    ret = nand_init("nand_disk.bin");
    if (ret != RET_OK) {
        printf("[固件] NAND 模块初始化失败\n");
        return RET_ERR_INTERNAL;
    }
    printf("[固件] NAND 模块初始化完成\n\n");

    /* 初始化 FTL 模块 */
    printf("[固件] 初始化 FTL 模块...\n");
    ret = ftl_init();
    if (ret != RET_OK) {
        printf("[固件] FTL 模块初始化失败\n");
        nand_deinit();
        return RET_ERR_INTERNAL;
    }
    printf("[固件] FTL 模块初始化完成\n\n");

    /* 掉电恢复：尝试从快照文件恢复元数据
     * 如果快照存在且校验通过，则恢复映射表和统计信息
     * 如果快照不存在或校验失败，则保持全新初始化状态 */
    printf("[固件] 检查掉电恢复快照...\n");
    ret = ftl_load_snapshot(FTL_SNAPSHOT_FILE);
    if (ret == RET_OK) {
        printf("[固件] 掉电恢复成功: 从 %s 恢复元数据\n", FTL_SNAPSHOT_FILE);
    } else {
        printf("[固件] 无有效快照，全新启动 (ret=%d)\n", ret);
    }
    printf("\n");

    /* 初始化主机接口模块 */
    printf("[固件] 初始化主机接口模块...\n");
    ret = host_if_init(&g_default_host_config);
    if (ret != RET_OK) {
        printf("[固件] 主机接口模块初始化失败\n");
        ftl_deinit();
        nand_deinit();
        return RET_ERR_INTERNAL;
    }
    printf("[固件] 主机接口模块初始化完成\n\n");

    /* 初始化 NVMe 控制器 */
    printf("[固件] 初始化 NVMe 控制器...\n");
    ret = nvme_ctrl_init();
    if (ret != RET_OK) {
        printf("[固件] NVMe 控制器初始化失败\n");
        host_if_deinit();
        ftl_deinit();
        nand_deinit();
        return RET_ERR_INTERNAL;
    }
    printf("[固件] NVMe 控制器初始化完成\n\n");

    /* 初始化 NVMe/TCP 目标端 */
    printf("[固件] 初始化 NVMe/TCP 目标端...\n");
    nvme_tcp_target_config_t tcp_config;
    memset(&tcp_config, 0, sizeof(tcp_config));
    tcp_config.port = 4420;
    tcp_config.subnqn = "nqn.2026-08.io.ftlfw:subsystem";
    tcp_config.maxh2cdata = 65536;
    ret = nvme_tcp_target_init(&tcp_config);
    if (ret != RET_OK) {
        printf("[固件] NVMe/TCP 目标端初始化失败\n");
        host_if_deinit();
        ftl_deinit();
        nand_deinit();
        return RET_ERR_INTERNAL;
    }
    printf("[固件] NVMe/TCP 目标端初始化完成，监听端口 %u\n\n", tcp_config.port);


    /* 初始化 UFS 目标端（Universal Flash Storage）
     * UFS 基于 SCSI 命令集，通过 UPIU 协议与主机通信
     * 当前实现应用层和传输层框架，链路层/物理层由硬件实现 */
    printf("[固件] 初始化 UFS 目标端...\n");
    ret = ufs_target_init();
    if (ret != RET_OK) {
        printf("[固件] UFS 目标端初始化失败（非致命，继续运行）\n");
    } else {
        uint64_t total_sectors = 0;
        uint32_t sector_size = 0;
        ufs_target_get_capacity(&total_sectors, &sector_size);
        printf("[固件] UFS 目标端初始化完成: 容量=%llu扇区, 扇区大小=%u字节\n",
               (unsigned long long)total_sectors, sector_size);
    }
    printf("\n");
    /* 初始化管理模块 */
    printf("[固件] 初始化管理模块...\n");
    ret = manager_init(&g_default_fw_config);
    if (ret != RET_OK) {
        printf("[固件] 管理模块初始化失败\n");
        host_if_deinit();
        ftl_deinit();
        nand_deinit();
        return RET_ERR_INTERNAL;
    }
    printf("[固件] 管理模块初始化完成\n\n");

    /* 注册所有模块到管理模块 */
    printf("[固件] 注册模块到管理模块...\n");
    manager_init_all_modules();
    manager_start_all_modules();
    printf("[固件] 模块注册完成\n\n");

    printf("[固件] 所有模块初始化完成\n");

    return RET_OK;
}

/**
 * @brief 反初始化所有模块
 */
static void deinit_all_modules(void)
{
    printf("\n[固件] 开始反初始化所有模块...\n\n");

    /* 停止所有模块 */
    manager_stop_all_modules();

    /* 反初始化管理模块 */
    printf("[固件] 反初始化管理模块...\n");
    manager_deinit();
    printf("[固件] 管理模块反初始化完成\n\n");

    /* 反初始化主机接口模块 */
    printf("[固件] 反初始化主机接口模块...\n");
    host_if_deinit();
    printf("[固件] 主机接口模块反初始化完成\n\n");

    /* 反初始化 NVMe/TCP 目标端 */
    printf("[固件] 反初始化 NVMe/TCP 目标端...\n");
    nvme_tcp_target_deinit();
    printf("[固件] NVMe/TCP 目标端反初始化完成\n\n");

    /* 反初始化 FTL 模块前，保存元数据快照（掉电保护） */
    printf("[固件] 保存 FTL 元数据快照...\n");
    {
        ret_code_t snap_ret = ftl_save_snapshot(FTL_SNAPSHOT_FILE);
        if (snap_ret == RET_OK) {
            printf("[固件] 快照保存成功: %s\n", FTL_SNAPSHOT_FILE);
        } else {
            printf("[固件] 快照保存失败: ret=%d\n", snap_ret);
        }
    }
    printf("\n");

    /* 反初始化 FTL 模块 */
    printf("[固件] 反初始化 FTL 模块...\n");
    ftl_deinit();
    printf("[固件] FTL 模块反初始化完成\n\n");

    /* 反初始化 NAND 模块 */
    printf("[固件] 反初始化 NAND 模块...\n");
    nand_deinit();
    printf("[固件] NAND 模块反初始化完成\n\n");

    /* 销毁消息队列 */
    printf("[固件] 销毁消息队列...\n");
    msg_queue_deinit(MODULE_NAND);
    msg_queue_deinit(MODULE_FTL);
    msg_queue_deinit(MODULE_HOST_IF);
    msg_queue_deinit(MODULE_MANAGER);
    msg_queue_deinit(MODULE_LOG);
    printf("[固件] 消息队列销毁完成\n\n");

    printf("[固件] 所有模块反初始化完成\n");
}

/* ============================================================
 *  测试功能
 * ============================================================ */

/**
 * @brief 运行基础测试
 * @return 0 成功，-1 失败
 */
static int __attribute__((unused)) run_basic_test(void)
{
    uint8_t write_buf[NAND_PAGE_SIZE];
    uint8_t read_buf[NAND_PAGE_SIZE];
    nvme_cmd_t cmd;
    nvme_cqe_t cqe;
    int pass = 1;

    printf("\n========================================\n");
    printf("    基础功能测试\n");
    printf("========================================\n\n");

    /* 准备测试数据 */
    memset(write_buf, 0xAA, NAND_PAGE_SIZE);

    /* 测试 NVMe 写命令 */
    printf("[测试] NVMe 写命令\n");
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_CMD_WRITE;
    cmd.slba = 100U;
    cmd.nlb = 0U;  /* 1 个块 */
    cmd.data_buf = write_buf;
    cmd.data_len = NAND_PAGE_SIZE;

    if (host_if_submit_cmd(&cmd) != RET_OK) {
        printf("  提交写命令: ❌ 失败\n");
        pass = 0;
    } else {
        printf("  提交写命令: ✅ 通过\n");
    }

    /* 处理命令 */
    host_if_process();

    /* 检查完成队列 */
    if (host_if_poll_cq(&cqe) == RET_OK) {
        printf("  写命令完成: %s (状态=%u)\n",
               cqe.status == NVME_STATUS_SUCCESS ? "✅ 通过" : "❌ 失败",
               cqe.status);
        if (cqe.status != NVME_STATUS_SUCCESS) {
            pass = 0;
        }
    } else {
        printf("  写命令完成: ❌ 未完成\n");
        pass = 0;
    }

    /* 测试 NVMe 读命令 */
    printf("\n[测试] NVMe 读命令\n");
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_CMD_READ;
    cmd.slba = 100U;
    cmd.nlb = 0U;  /* 1 个块 */
    cmd.data_buf = read_buf;
    cmd.data_len = NAND_PAGE_SIZE;

    if (host_if_submit_cmd(&cmd) != RET_OK) {
        printf("  提交读命令: ❌ 失败\n");
        pass = 0;
    } else {
        printf("  提交读命令: ✅ 通过\n");
    }

    /* 处理命令 */
    host_if_process();

    /* 检查完成队列 */
    if (host_if_poll_cq(&cqe) == RET_OK) {
        printf("  读命令完成: %s (状态=%u)\n",
               cqe.status == NVME_STATUS_SUCCESS ? "✅ 通过" : "❌ 失败",
               cqe.status);
        if (cqe.status != NVME_STATUS_SUCCESS) {
            pass = 0;
        }
    } else {
        printf("  读命令完成: ❌ 未完成\n");
        pass = 0;
    }

    /* 验证数据 */
    printf("\n[测试] 数据一致性验证\n");
    if (memcmp(write_buf, read_buf, NAND_PAGE_SIZE) == 0) {
        printf("  数据一致性: ✅ 通过\n");
    } else {
        printf("  数据一致性: ❌ 失败\n");
        pass = 0;
    }

    /* 打印统计信息 */
    printf("\n");
    host_if_print_stats();
    printf("\n");
    manager_print_module_status();
    printf("\n");
    manager_print_stats();

    printf("\n========================================\n");
    printf("    测试结果: %s\n", pass ? "✅ 全部通过" : "❌ 部分失败");
    printf("========================================\n");

    return pass ? 0 : -1;
}

/* ============================================================
 *  多线程测试
 * ============================================================ */

/**
 * @brief 测试线程函数
 * @param[in] arg 线程参数
 * @return 线程返回值
 */
static void *test_thread_func(void *arg)
{
    uint32_t thread_id = 0;
    uint32_t i = 0;

    thread_id = thread_get_current_id();
    printf("[多线程测试] 线程 %u 开始运行，参数=%p\n", thread_id, arg);

    /* 模拟工作 */
    for (i = 0; i < 5; i++) {
        printf("[多线程测试] 线程 %u 运行中... 第 %u 次\n", thread_id, i + 1);
        thread_sleep(10);
    }

    printf("[多线程测试] 线程 %u 结束运行\n", thread_id);

    return NULL;
}

/**
 * @brief NAND 多线程测试函数
 * @param[in] arg 线程参数，指向要操作的块号（uint32_t*）
 * @return NULL
 * @details 每个线程操作独立的 NAND 块，测试并发读写安全性：
 *          1. 擦除指定块
 *          2. 写入前4页数据（每页数据不同）
 *          3. 读取并验证数据一致性
 *          4. 统计成功/失败次数
 */
static void *nand_test_thread_func(void *arg)
{
    uint32_t thread_id = 0;
    uint32_t block = 0;
    uint32_t page = 0;
    uint8_t write_buf[NAND_PAGE_SIZE];
    uint8_t read_buf[NAND_PAGE_SIZE];
    ret_code_t ret = RET_OK;
    uint32_t success_count = 0;
    uint32_t fail_count = 0;

    /* 获取线程ID和操作的块号 */
    thread_id = thread_get_current_id();
    block = *(uint32_t *)arg;

    printf("[NAND多线程] 线程 %u 开始，操作块 %u\n", thread_id, block);

    /* 步骤1：擦除块（NAND特性：写前必须擦除） */
    ret = nand_block_erase(block);
    if (ret != RET_OK) {
        printf("[NAND多线程] 线程 %u 擦除块 %u 失败，错误码=%d\n", thread_id, block, ret);
        return NULL;
    }
    printf("[NAND多线程] 线程 %u 擦除块 %u 成功\n", thread_id, block);

    /* 步骤2：写入前4页数据，每页使用不同的数据模式 */
    for (page = 0; page < 4; page++) {
        /* 填充测试数据：每页第一个字节为页号，其余为页号的反码 */
        memset(write_buf, (uint8_t)(~page & 0xFF), NAND_PAGE_SIZE);
        write_buf[0] = (uint8_t)page;

        ret = nand_page_write(block, page, write_buf);
        if (ret != RET_OK) {
            printf("[NAND多线程] 线程 %u 写入页 %u 失败，错误码=%d\n", thread_id, page, ret);
            fail_count++;
        } else {
            success_count++;
        }
    }

    /* 步骤3：读取并验证数据一致性 */
    for (page = 0; page < 4; page++) {
        memset(read_buf, 0, NAND_PAGE_SIZE);

        ret = nand_page_read(block, page, read_buf);
        if (ret != RET_OK) {
            printf("[NAND多线程] 线程 %u 读取页 %u 失败，错误码=%d\n", thread_id, page, ret);
            fail_count++;
            continue;
        }

        /* 验证数据：第一个字节应为页号，其余应为页号的反码 */
        memset(write_buf, (uint8_t)(~page & 0xFF), NAND_PAGE_SIZE);
        write_buf[0] = (uint8_t)page;

        if (memcmp(write_buf, read_buf, NAND_PAGE_SIZE) != 0) {
            printf("[NAND多线程] 线程 %u 页 %u 数据验证失败\n", thread_id, page);
            fail_count++;
        } else {
            success_count++;
        }
    }

    printf("[NAND多线程] 线程 %u 结束，成功=%u, 失败=%u\n",
           thread_id, success_count, fail_count);

    return NULL;
}

/**
 * @brief 运行多线程测试
 * @return 0 成功，-1 失败
 */
static int __attribute__((unused)) run_thread_test(void)
{
    uint32_t thread1 = 0;
    uint32_t thread2 = 0;
    uint32_t thread3 = 0;
    uint32_t nand_thread1 = 0;
    uint32_t nand_thread2 = 0;
    uint32_t nand_thread3 = 0;
    /* NAND测试线程的块号参数（静态变量，确保线程运行期间有效） */
    static uint32_t nand_block1 = 10;
    static uint32_t nand_block2 = 11;
    static uint32_t nand_block3 = 12;
    int pass = 1;

    printf("\n========================================\n");
    printf("    多线程功能测试\n");
    printf("========================================\n\n");

    /* 初始化线程管理模块 */
    printf("[测试] 初始化线程管理模块\n");
    if (thread_manager_init() != RET_OK) {
        printf("  初始化: ❌ 失败\n");
        return -1;
    }
    printf("  初始化: ✅ 通过\n");

    /* 创建普通测试线程 */
    printf("\n[测试] 创建普通测试线程\n");
    thread1 = thread_create("test_thread_1", test_thread_func, NULL, THREAD_PRIORITY_NORMAL);
    thread2 = thread_create("test_thread_2", test_thread_func, NULL, THREAD_PRIORITY_HIGH);
    thread3 = thread_create("test_thread_3", test_thread_func, NULL, THREAD_PRIORITY_LOW);

    if (thread1 == 0 || thread2 == 0 || thread3 == 0) {
        printf("  创建线程: ❌ 失败\n");
        pass = 0;
    } else {
        printf("  创建线程: ✅ 通过 (ID=%u, %u, %u)\n", thread1, thread2, thread3);
    }

    /* 创建 NAND 测试线程（每个线程操作不同的块，测试并发安全性） */
    printf("\n[测试] 创建 NAND 多线程测试\n");
    nand_thread1 = thread_create("nand_test_1", nand_test_thread_func, &nand_block1, THREAD_PRIORITY_NORMAL);
    nand_thread2 = thread_create("nand_test_2", nand_test_thread_func, &nand_block2, THREAD_PRIORITY_NORMAL);
    nand_thread3 = thread_create("nand_test_3", nand_test_thread_func, &nand_block3, THREAD_PRIORITY_NORMAL);

    if (nand_thread1 == 0 || nand_thread2 == 0 || nand_thread3 == 0) {
        printf("  创建NAND线程: ❌ 失败\n");
        pass = 0;
    } else {
        printf("  创建NAND线程: ✅ 通过 (ID=%u, %u, %u, 操作块=%u,%u,%u)\n",
               nand_thread1, nand_thread2, nand_thread3,
               nand_block1, nand_block2, nand_block3);
    }

    /* 启动所有线程 */
    printf("\n[测试] 启动所有线程\n");
    if (thread_start(thread1) != RET_OK ||
        thread_start(thread2) != RET_OK ||
        thread_start(thread3) != RET_OK ||
        thread_start(nand_thread1) != RET_OK ||
        thread_start(nand_thread2) != RET_OK ||
        thread_start(nand_thread3) != RET_OK) {
        printf("  启动线程: ❌ 失败\n");
        pass = 0;
    } else {
        printf("  启动线程: ✅ 通过\n");
    }

    /* 等待所有线程结束 */
    printf("\n[测试] 等待所有线程结束\n");
    thread_join(thread1, NULL);
    thread_join(thread2, NULL);
    thread_join(thread3, NULL);
    thread_join(nand_thread1, NULL);
    thread_join(nand_thread2, NULL);
    thread_join(nand_thread3, NULL);
    printf("  等待线程: ✅ 通过\n");

    /* 打印线程状态 */
    printf("\n");
    thread_manager_print_status();

    /* 销毁所有线程 */
    printf("\n[测试] 销毁所有线程\n");
    thread_destroy(thread1);
    thread_destroy(thread2);
    thread_destroy(thread3);
    thread_destroy(nand_thread1);
    thread_destroy(nand_thread2);
    thread_destroy(nand_thread3);
    printf("  销毁线程: ✅ 通过\n");

    /* 反初始化线程管理模块 */
    thread_manager_deinit();

    printf("\n========================================\n");
    printf("    测试结果: %s\n", pass ? "✅ 全部通过" : "❌ 部分失败");
    printf("========================================\n");

    return pass ? 0 : -1;
}

/* ============================================================
 *  DMA 测试
 * ============================================================ */

/**
 * @brief DMA 传输完成回调
 * @param[in] channel DMA 通道号
 * @param[in] success 是否成功
 * @param[in] transferred 已传输字节数
 * @param[in] user_data 用户数据指针
 */
static void dma_test_callback(uint32_t channel, bool success,
                              uint32_t transferred, void *user_data)
{
    /* 消除未使用参数警告 */
    (void)user_data;

    printf("[DMA测试] 回调: 通道=%u, 成功=%s, 已传输=%u 字节\n",
           channel, success ? "是" : "否", transferred);
}

/**
 * @brief 运行 DMA 测试
 * @return 0 成功，-1 失败
 */
static int __attribute__((unused)) run_dma_test(void)
{
    uint8_t *src_buf = NULL;
    uint8_t *dst_buf = NULL;
    dma_transfer_desc_t desc;
    uint32_t transferred = 0;
    uint32_t channel = 0;
    int pass = 1;
    uint32_t i = 0;

    printf("\n========================================\n");
    printf("    DMA 功能测试\n");
    printf("========================================\n\n");

    /* 初始化 DMA 控制器 */
    printf("[测试] 初始化 DMA 控制器\n");
    if (dma_init() != RET_OK) {
        printf("  初始化: ❌ 失败\n");
        return -1;
    }
    printf("  初始化: ✅ 通过\n");

    /* 分配缓冲区 */
    src_buf = (uint8_t *)malloc(4096);
    dst_buf = (uint8_t *)malloc(4096);
    if (src_buf == NULL || dst_buf == NULL) {
        printf("  分配缓冲区: ❌ 失败\n");
        dma_deinit();
        return -1;
    }

    /* 初始化源缓冲区 */
    for (i = 0; i < 4096; i++) {
        src_buf[i] = (uint8_t)(i & 0xFF);
    }
    memset(dst_buf, 0, 4096);

    /* 测试同步 DMA 传输 */
    printf("\n[测试] 同步 DMA 传输\n");
    memset(&desc, 0, sizeof(desc));
    desc.src_addr = src_buf;
    desc.dst_addr = dst_buf;
    desc.length = 4096;
    desc.direction = DMA_DIR_MEM_TO_MEM;
    desc.src_width = DMA_WIDTH_WORD;
    desc.dst_width = DMA_WIDTH_WORD;
    desc.src_increment = true;
    desc.dst_increment = true;
    desc.interrupt_enable = true;

    if (dma_transfer_sync(&desc, &transferred) != RET_OK) {
        printf("  同步传输: ❌ 失败\n");
        pass = 0;
    } else {
        printf("  同步传输: ✅ 通过 (已传输=%u 字节)\n", transferred);
    }

    /* 验证数据 */
    printf("\n[测试] 数据一致性验证\n");
    if (memcmp(src_buf, dst_buf, 4096) == 0) {
        printf("  数据一致性: ✅ 通过\n");
    } else {
        printf("  数据一致性: ❌ 失败\n");
        pass = 0;
    }

    /* 测试异步 DMA 传输 */
    printf("\n[测试] 异步 DMA 传输\n");
    memset(dst_buf, 0, 4096);

    channel = dma_alloc_channel();
    if (channel == 0xFFFFFFFF) {
        printf("  分配通道: ❌ 失败\n");
        pass = 0;
    } else {
        printf("  分配通道: ✅ 通过 (通道=%u)\n", channel);

        /* 配置通道 */
        dma_config_channel(channel, &desc);

        /* 设置回调 */
        dma_set_callback(channel, dma_test_callback, NULL);

        /* 启动传输 */
        if (dma_start_transfer(channel) != RET_OK) {
            printf("  启动传输: ❌ 失败\n");
            pass = 0;
        } else {
            printf("  启动传输: ✅ 通过\n");
        }

        /* 等待传输完成 */
        if (dma_wait_complete(channel, 5000) != RET_OK) {
            printf("  等待完成: ❌ 超时\n");
            pass = 0;
        } else {
            printf("  等待完成: ✅ 通过\n");
        }

        /* 验证数据 */
        if (memcmp(src_buf, dst_buf, 4096) == 0) {
            printf("  数据一致性: ✅ 通过\n");
        } else {
            printf("  数据一致性: ❌ 失败\n");
            pass = 0;
        }

        /* 释放通道 */
        dma_free_channel(channel);
    }

    /* 打印 DMA 状态 */
    printf("\n");
    dma_print_status();

    /* 释放缓冲区 */
    free(src_buf);
    free(dst_buf);

    /* 反初始化 DMA 控制器 */
    dma_deinit();

    printf("\n========================================\n");
    printf("    测试结果: %s\n", pass ? "✅ 全部通过" : "❌ 部分失败");
    printf("========================================\n");

    return pass ? 0 : -1;
}

/* ============================================================
 *  RAID 功能测试
 * ============================================================ */

/**
 * @brief 运行 RAID 功能测试
 * @return 0 成功，-1 失败
 */
static int __attribute__((unused)) run_raid_test(void)
{
    int pass = 1;
    raid_config_t config;
    uint8_t write_buf[NAND_PAGE_SIZE];
    uint8_t read_buf[NAND_PAGE_SIZE];
    uint32_t i = 0;
    uint64_t lpn = 0;

    printf("\n========================================\n");
    printf("    RAID 功能测试\n");
    printf("========================================\n");

    /* 测试 RAID 0 */
    printf("\n--- RAID 0 (条带化) 测试 ---\n");

    /* 初始化 RAID 0 */
    memset(&config, 0, sizeof(config));
    config.level = RAID_LEVEL_0;
    config.member_count = 2;
    config.stripe_size = RAID_STRIPE_SIZE_PAGES;
    config.auto_rebuild = true;

    printf("[测试] 初始化 RAID 0\n");
    if (raid_init(&config) != RET_OK) {
        printf("  初始化: ❌ 失败\n");
        return -1;
    }
    printf("  初始化: ✅ 通过\n");

    /* 添加成员 */
    printf("[测试] 添加 RAID 成员\n");
    for (i = 0; i < config.member_count; i++) {
        if (raid_add_member(i, i) != RET_OK) {
            printf("  添加成员 %u: ❌ 失败\n", i);
            pass = 0;
        } else {
            printf("  添加成员 %u: ✅ 通过\n", i);
        }
    }

    printf("  逻辑容量: %llu 页\n", (unsigned long long)raid_get_logical_capacity());

    /* 写入测试数据 */
    printf("[测试] RAID 0 写入测试\n");
    for (i = 0; i < NAND_PAGE_SIZE; i++) {
        write_buf[i] = (uint8_t)(i & 0xFF);
    }

    for (lpn = 0; lpn < 8; lpn++) {
        write_buf[0] = (uint8_t)lpn;
        if (raid_write(lpn, write_buf) != RET_OK) {
            printf("  写入 LPN %llu: ❌ 失败\n", (unsigned long long)lpn);
            pass = 0;
        }
    }
    printf("  写入 8 页: ✅ 通过\n");

    /* 读取并验证数据 */
    printf("[测试] RAID 0 读取验证\n");
    for (lpn = 0; lpn < 8; lpn++) {
        memset(read_buf, 0, sizeof(read_buf));
        if (raid_read(lpn, read_buf) != RET_OK) {
            printf("  读取 LPN %llu: ❌ 失败\n", (unsigned long long)lpn);
            pass = 0;
            continue;
        }

        write_buf[0] = (uint8_t)lpn;
        if (memcmp(write_buf, read_buf, NAND_PAGE_SIZE) != 0) {
            printf("  数据验证 LPN %llu: ❌ 失败\n", (unsigned long long)lpn);
            pass = 0;
        }
    }
    printf("  读取验证 8 页: ✅ 通过\n");

    /* 打印状态 */
    raid_print_status();

    /* 反初始化 */
    raid_deinit();

    /* 测试 RAID 1 */
    printf("\n--- RAID 1 (镜像) 测试 ---\n");

    /* 初始化 RAID 1 */
    memset(&config, 0, sizeof(config));
    config.level = RAID_LEVEL_1;
    config.member_count = 2;
    config.stripe_size = 1;
    config.auto_rebuild = true;

    printf("[测试] 初始化 RAID 1\n");
    if (raid_init(&config) != RET_OK) {
        printf("  初始化: ❌ 失败\n");
        return -1;
    }
    printf("  初始化: ✅ 通过\n");

    /* 添加成员 */
    printf("[测试] 添加 RAID 成员\n");
    for (i = 0; i < config.member_count; i++) {
        if (raid_add_member(i, i + 10) != RET_OK) {
            printf("  添加成员 %u: ❌ 失败\n", i);
            pass = 0;
        } else {
            printf("  添加成员 %u: ✅ 通过\n", i);
        }
    }

    printf("  逻辑容量: %llu 页\n", (unsigned long long)raid_get_logical_capacity());

    /* 写入测试数据 */
    printf("[测试] RAID 1 写入测试\n");
    for (i = 0; i < NAND_PAGE_SIZE; i++) {
        write_buf[i] = (uint8_t)(0xAA + (i & 0xFF));
    }

    for (lpn = 0; lpn < 4; lpn++) {
        write_buf[0] = (uint8_t)(lpn + 0x10);
        if (raid_write(lpn, write_buf) != RET_OK) {
            printf("  写入 LPN %llu: ❌ 失败\n", (unsigned long long)lpn);
            pass = 0;
        }
    }
    printf("  写入 4 页: ✅ 通过\n");

    /* 读取并验证数据 */
    printf("[测试] RAID 1 读取验证\n");
    for (lpn = 0; lpn < 4; lpn++) {
        memset(read_buf, 0, sizeof(read_buf));
        if (raid_read(lpn, read_buf) != RET_OK) {
            printf("  读取 LPN %llu: ❌ 失败\n", (unsigned long long)lpn);
            pass = 0;
            continue;
        }

        write_buf[0] = (uint8_t)(lpn + 0x10);
        if (memcmp(write_buf, read_buf, NAND_PAGE_SIZE) != 0) {
            printf("  数据验证 LPN %llu: ❌ 失败\n", (unsigned long long)lpn);
            pass = 0;
        }
    }
    printf("  读取验证 4 页: ✅ 通过\n");

    /* 打印状态 */
    raid_print_status();

    /* 反初始化 */
    raid_deinit();

    printf("\n========================================\n");
    printf("    测试结果: %s\n", pass ? "✅ 全部通过" : "❌ 部分失败");
    printf("========================================\n");

    return pass ? 0 : -1;
}

/* ============================================================
 *  企业级特性测试（NVMe Admin、SMART、PLP、DIF）
 * ============================================================ */

/**
 * @brief 运行企业级特性测试
 * @return 0 成功，-1 失败
 */
static int __attribute__((unused)) test_enterprise_features(void)
{
    int pass = 1;
    ret_code_t ret;

    printf("\n========================================\n");
    printf("    企业级特性测试\n");
    printf("========================================\n");

    /* ---------- 1. NVMe Admin 命令测试 ---------- */
    printf("\n--- 1. NVMe Admin 命令测试 ---\n");

    /* 测试 Identify Controller */
    printf("[测试] Identify Controller\n");
    nvme_id_ctrl_t id_ctrl;
    ret = host_if_get_id_ctrl(&id_ctrl);
    if (ret == RET_OK) {
        printf("  厂商ID:       0x%04X\n", id_ctrl.vid);
        printf("  序列号:       %.20s\n", id_ctrl.sn);
        printf("  型号:         %.40s\n", id_ctrl.mn);
        printf("  固件版本:     %.8s\n", id_ctrl.fr);
        printf("  NVMe版本:     0x%08X\n", id_ctrl.ver);
        printf("  命名空间数:   %u\n", id_ctrl.nn);
        printf("  最大命令数:   %u\n", id_ctrl.maxcmd);
        printf("  Identify Controller: ✅ 通过\n");
    } else {
        printf("  Identify Controller: ❌ 失败 (ret=%d)\n", ret);
        pass = 0;
    }

    /* 测试 Identify Namespace */
    printf("[测试] Identify Namespace\n");
    nvme_id_ns_t id_ns;
    ret = host_if_get_id_ns(&id_ns);
    if (ret == RET_OK) {
        printf("  命名空间大小: %llu LBA\n", (unsigned long long)id_ns.nsze);
        printf("  命名空间容量: %llu LBA\n", (unsigned long long)id_ns.ncap);
        printf("  已使用:       %llu LBA\n", (unsigned long long)id_ns.nuse);
        printf("  LBA格式数:    %u\n", id_ns.nlbaf + 1);
        printf("  格式化LBA:    %u (2^%u = %u字节)\n",
               id_ns.flbas, id_ns.lbaf[0][0], 1 << id_ns.lbaf[0][0]);
        printf("  端到端保护:   能力=0x%02X 设置=0x%02X\n", id_ns.dpc, id_ns.dps);
        printf("  Identify Namespace: ✅ 通过\n");
    } else {
        printf("  Identify Namespace: ❌ 失败 (ret=%d)\n", ret);
        pass = 0;
    }

    /* 测试 Get Log Page (SMART) */
    printf("[测试] Get Log Page (SMART/Health)\n");
    nvme_smart_log_t smart_log;
    ret = host_if_get_smart_log(&smart_log);
    if (ret == RET_OK) {
        printf("  温度:         %u °C\n", smart_log.temperature - 273);
        printf("  可用备用:     %u%%\n", smart_log.avail_spare);
        printf("  寿命已使用:   %u%%\n", smart_log.percent_used);
        printf("  严重警告:     0x%02X\n", smart_log.critical_warning);
        printf("  Get Log Page: ✅ 通过\n");
    } else {
        printf("  Get Log Page: ❌ 失败 (ret=%d)\n", ret);
        pass = 0;
    }

    /* 测试 Set Feature / Get Feature */
    printf("[测试] Set Feature / Get Feature\n");
    nvme_cmd_t feature_cmd;
    memset(&feature_cmd, 0, sizeof(feature_cmd));
    feature_cmd.is_admin = true;
    feature_cmd.opcode = NVME_ADMIN_SET_FEATURE;
    feature_cmd.nsid = NVME_FEAT_TEMP_THRESHOLD;
    feature_cmd.slba = 350;  /* 设置温度阈值为77°C */
    feature_cmd.data_buf = NULL;
    feature_cmd.data_len = 0;
    ret = host_if_submit_cmd(&feature_cmd);
    host_if_process();
    if (ret == RET_OK) {
        printf("  Set Feature (温度阈值): ✅ 通过\n");
    } else {
        printf("  Set Feature: ❌ 失败 (ret=%d)\n", ret);
        pass = 0;
    }

    /* 打印 SMART 信息 */
    printf("\n[测试] 打印 SMART/健康信息\n");
    host_if_print_smart_info();

    /* ---------- 2. 断电保护（PLP）测试 ---------- */
    printf("\n--- 2. 断电保护（PLP）测试 ---\n");

    /* 模拟断电 */
    printf("[测试] 模拟断电事件\n");
    ret = host_if_plp_simulate_power_loss();
    if (ret == RET_OK) {
        printf("  模拟断电: ✅ 通过\n");
    } else {
        printf("  模拟断电: ❌ 失败 (ret=%d)\n", ret);
        pass = 0;
    }

    /* 断电恢复 */
    printf("[测试] 断电恢复\n");
    ret = host_if_plp_recovery();
    if (ret == RET_OK) {
        printf("  断电恢复: ✅ 通过\n");
    } else {
        printf("  断电恢复: ❌ 失败 (ret=%d)\n", ret);
        pass = 0;
    }

    /* 获取电源状态 */
    printf("[测试] 获取电源状态\n");
    uint64_t power_cycles, unsafe_shutdowns, power_on_hours;
    ret = host_if_get_power_status(&power_cycles, &unsafe_shutdowns, &power_on_hours);
    if (ret == RET_OK) {
        printf("  上电循环:     %llu\n", (unsigned long long)power_cycles);
        printf("  不安全关机:   %llu\n", (unsigned long long)unsafe_shutdowns);
        printf("  上电时间:     %llu 小时\n", (unsigned long long)power_on_hours);
        printf("  获取电源状态: ✅ 通过\n");
    } else {
        printf("  获取电源状态: ❌ 失败 (ret=%d)\n", ret);
        pass = 0;
    }

    /* ---------- 3. 端到端数据保护（DIF）测试 ---------- */
    printf("\n--- 3. 端到端数据保护（DIF）测试 ---\n");

    /* 初始化 DIF */
    printf("[测试] DIF 初始化\n");
    dif_config_t dif_config;
    memset(&dif_config, 0, sizeof(dif_config));
    dif_config.type = DIF_TYPE1;
    dif_config.guard_check_enable = true;
    dif_config.app_tag_check_enable = true;
    dif_config.ref_tag_check_enable = true;
    dif_config.app_tag = 0x1234;
    dif_config.app_tag_mask = 0xFFFF;
    ret = dif_init(&dif_config);
    if (ret == RET_OK) {
        printf("  DIF 初始化: ✅ 通过\n");
    } else {
        printf("  DIF 初始化: ❌ 失败 (ret=%d)\n", ret);
        pass = 0;
    }

    /* 测试 DIF 生成和校验（正常情况） */
    printf("[测试] DIF 生成和校验（正常数据）\n");
    uint8_t test_data[NAND_PAGE_SIZE * 2];
    dif_protection_t test_protection[2];
    for (uint32_t i = 0; i < sizeof(test_data); i++) {
        test_data[i] = (uint8_t)(i & 0xFF);
    }
    ret = dif_generate(test_data, sizeof(test_data), 100, test_protection);
    if (ret == RET_OK) {
        printf("  DIF 生成: ✅ 通过 (CRC[0]=0x%04X, RefTag[0]=0x%08X)\n",
               test_protection[0].crc, test_protection[0].ref_tag);
    } else {
        printf("  DIF 生成: ❌ 失败 (ret=%d)\n", ret);
        pass = 0;
    }

    uint64_t error_lba = 0;
    ret = dif_verify(test_data, sizeof(test_data), 100, test_protection, &error_lba);
    if (ret == RET_OK) {
        printf("  DIF 校验（正常）: ✅ 通过\n");
    } else {
        printf("  DIF 校验（正常）: ❌ 失败 (ret=%d, error_lba=%llu)\n",
               ret, (unsigned long long)error_lba);
        pass = 0;
    }

    /* 测试 DIF 校验（数据损坏情况） */
    printf("[测试] DIF 校验（数据损坏）\n");
    test_data[10] = 0xFF;  /* 篡改数据 */
    ret = dif_verify(test_data, sizeof(test_data), 100, test_protection, &error_lba);
    if (ret == RET_ERR_DIF_CRC) {
        printf("  DIF 校验（损坏）: ✅ 正确检测到CRC错误 (LBA=%llu)\n",
               (unsigned long long)error_lba);
    } else {
        printf("  DIF 校验（损坏）: ❌ 未检测到错误 (ret=%d)\n", ret);
        pass = 0;
    }
    test_data[10] = 10;  /* 恢复数据 */

    /* 测试 DIF 参考标签错误 */
    printf("[测试] DIF 校验（参考标签错误）\n");
    ret = dif_verify(test_data, sizeof(test_data), 200, test_protection, &error_lba);
    if (ret == RET_ERR_DIF_REF_TAG) {
        printf("  DIF 校验（参考标签）: ✅ 正确检测到参考标签错误 (LBA=%llu)\n",
               (unsigned long long)error_lba);
    } else {
        printf("  DIF 校验（参考标签）: ❌ 未检测到错误 (ret=%d)\n", ret);
        pass = 0;
    }

    /* 获取 DIF 统计 */
    printf("[测试] 获取 DIF 统计\n");
    dif_stats_t dif_stats;
    ret = dif_get_stats(&dif_stats);
    if (ret == RET_OK) {
        printf("  总校验次数:   %llu\n", (unsigned long long)dif_stats.total_checks);
        printf("  CRC错误数:    %llu\n", (unsigned long long)dif_stats.crc_errors);
        printf("  应用标签错误: %llu\n", (unsigned long long)dif_stats.app_tag_errors);
        printf("  参考标签错误: %llu\n", (unsigned long long)dif_stats.ref_tag_errors);
        printf("  获取 DIF 统计: ✅ 通过\n");
    } else {
        printf("  获取 DIF 统计: ❌ 失败 (ret=%d)\n", ret);
        pass = 0;
    }

    printf("\n========================================\n");
    printf("    企业级特性测试结果: %s\n", pass ? "✅ 全部通过" : "❌ 部分失败");
    printf("========================================\n");

    return pass ? 0 : -1;
}

/* ============================================================
 *  主函数
 * ============================================================ */

/**
 * @brief 主函数
 * @param[in] argc 参数个数
 * @param[in] argv 参数数组
 * @return 0 成功，-1 失败
 */
int main(int argc, char *argv[])
{
    ret_code_t ret = RET_OK;
    int result = 0;
    int i = 0;

    /* 解析命令行参数 */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            g_log_level = LOG_LEVEL_INFO;
        } else if (strcmp(argv[i], "--trace") == 0) {
            g_log_level = LOG_LEVEL_DEBUG;
        }
    }

    /* 打印版本信息 */
    utils_print_version();
    printf("\n");

    /* 打印配置信息 */
    printf("固件配置:\n");
    printf("  日志级别:     %s\n",
           g_log_level == LOG_LEVEL_DEBUG ? "DEBUG" :
           g_log_level == LOG_LEVEL_INFO ? "INFO" :
           g_log_level == LOG_LEVEL_WARN ? "WARN" : "ERROR");
    printf("  心跳间隔:     %u ms\n", g_default_fw_config.heartbeat_interval_ms);
    printf("  健康检查间隔: %u ms\n", g_default_fw_config.health_check_interval_ms);
    printf("  看门狗超时:   %u ms\n", g_default_fw_config.watchdog_timeout_ms);
    printf("  最大错误数:   %u\n", g_default_fw_config.max_error_count);
    printf("  自动恢复:     %s\n", g_default_fw_config.auto_recovery ? "启用" : "禁用");
    printf("\n");

    /* 初始化所有模块（含 UFS 协议栈） */
    ret = init_all_modules();
    if (ret != RET_OK) {
        printf("[固件] 模块初始化失败\n");
        return -1;
    }

    /* ============================================================
     *  任务注册（FreeRTOS 风格，按优先级从高到低）
     * ============================================================ */
    printf("\n[固件] 注册系统任务...\n");
//    task_register("NVMe-TCP-Service", task_nvme_tcp_service, NULL, TASK_PRIORITY_HIGH, 64 * 1024);
//    task_register("Heartbeat-Monitor", task_heartbeat_monitor, NULL, TASK_PRIORITY_NORMAL, 16 * 1024);
//    task_register("FTL-Unit-Test", task_ftl_unit_test, NULL, TASK_PRIORITY_LOW, 32 * 1024);
//    task_register("GC-Benchmark", task_gc_benchmark, NULL, TASK_PRIORITY_LOW, 32 * 1024);
//    task_register("Task-Monitor", task_status_monitor, NULL, TASK_PRIORITY_IDLE, 8 * 1024);

    uint32_t started = task_start_all();
    if (started == 0) {
        printf("[固件] 警告：没有辅助任务启动（核心业务在主线程运行）\n");
    }
    task_print_status();

    printf("[固件] 进入多任务运行模式（NVMe/TCP核心业务在主线程，辅助任务在子线程）...\n\n");
    uint32_t main_loop_count = 0;
    while (1) {
        /* 核心业务：NVMe/TCP 前端接口处理（在主线程运行，确保最低延迟） */
        manager_process();
        host_if_process();
        nvme_tcp_target_process();

        /* 更新模块心跳（防止看门狗超时） */
        manager_send_heartbeat(MODULE_NAND);
        manager_send_heartbeat(MODULE_FTL);
        manager_send_heartbeat(MODULE_HOST_IF);
        manager_send_heartbeat(MODULE_LOG);

        main_loop_count++;

        /* 每10000次循环打印一次状态 */
        if (main_loop_count % 10000U == 0U) {
            printf("[主线程] 运行中: 循环次数=%u, 任务数=%u\n", main_loop_count, g_task_count);
            manager_print_module_status();
        }

        /* 掉电保护：每5000次循环保存一次 FTL 元数据快照 */
        if (main_loop_count % 5000U == 0U) {
            ret_code_t snap_ret = ftl_save_snapshot(FTL_SNAPSHOT_FILE);
            if (snap_ret != RET_OK) {
                LOG_WARN("FTL 快照保存失败: ret=%d", snap_ret);
            }
        }

        /* 短暂休眠100微秒，减少CPU占用 */
        usleep(100);
    }

    deinit_all_modules();

    /* 打印最终状态 */
    printf("\n========================================\n");
    printf("    固件退出，结果: %s\n", result == 0 ? "✅ 正常" : "❌ 异常");
    printf("========================================\n");

    return result;
}
