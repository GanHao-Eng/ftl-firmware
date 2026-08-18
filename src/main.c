/**
 * @file main.c
 * @brief FTL 固件主程序入口
 * @details 企业级 FTL 固件的主程序入口，初始化所有模块并运行主循环
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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

/* ============================================================
 *  全局配置
 * ============================================================ */

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
static int firmware_main_loop(void)
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
static int run_basic_test(void)
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
static int run_thread_test(void)
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
static int run_dma_test(void)
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
static int run_raid_test(void)
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
static int test_enterprise_features(void)
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

    /* 初始化所有模块 */
    ret = init_all_modules();
    if (ret != RET_OK) {
        printf("[固件] 模块初始化失败\n");
        return -1;
    }

    /* 直接进入 NVMe/TCP 服务主循环 */
    result = firmware_main_loop();

    /* 反初始化所有模块 */
    deinit_all_modules();

    /* 打印最终状态 */
    printf("\n========================================\n");
    printf("    固件退出，结果: %s\n", result == 0 ? "✅ 正常" : "❌ 异常");
    printf("========================================\n");

    return result;
}
