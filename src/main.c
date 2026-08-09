/**
 * @file main.c
 * @brief FTL 固件主程序入口
 * @details 企业级 FTL 固件的主程序入口，初始化所有模块并运行主循环
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/common.h"
#include "msg_queue.h"
#include "utils.h"
#include "log.h"
#include "nand.h"
#include "ftl.h"
#include "host_if.h"
#include "manager.h"

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

    printf("\n[固件] 进入主循环...\n\n");

    /* 主循环 */
    while (1) {
        /* 管理模块处理 */
        manager_process();

        /* 主机接口处理 */
        host_if_process();

        /* 打印状态（每 1000 次循环打印一次） */
        loop_count++;
        if (loop_count % 1000U == 0U) {
            printf("[固件] 主循环运行中... 循环次数: %u\n", loop_count);
            manager_print_module_status();
            printf("\n");
        }

        /* 简化实现：运行一定次数后退出 */
        if (loop_count >= 5000U) {
            printf("[固件] 达到最大循环次数，退出\n");
            break;
        }
    }

    return result;
}

/* ============================================================
 *  模块初始化
 * ============================================================ */

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
    log_set_level(LOG_LEVEL_INFO);
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

    /* 打印版本信息 */
    utils_print_version();
    printf("\n");

    /* 打印配置信息 */
    printf("固件配置:\n");
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

    /* 运行基础测试 */
    result = run_basic_test();

    /* 运行主循环 */
    if (result == 0) {
        result = firmware_main_loop();
    }

    /* 反初始化所有模块 */
    deinit_all_modules();

    /* 打印最终状态 */
    printf("\n========================================\n");
    printf("    固件退出，结果: %s\n", result == 0 ? "✅ 正常" : "❌ 异常");
    printf("========================================\n");

    return result;
}
