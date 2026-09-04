/**
 * @file test_ufs.c
 * @brief UFS目标端单元测试
 * @details 覆盖UFS协议栈核心功能：初始化、INQUIRY、TEST_UNIT_READY、
 *          READ_CAPACITY、WRITE、READ、UNMAP、写保护、电源管理、健康监控、错误统计
 * @note UFS基于SCSI命令集，CDB为大端格式，扇区大小512字节
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "protocol/ufs_target.h"
#include "common/common.h"
#include "nand.h"
#include "ftl.h"

/* ============================================================
 *  简单测试框架
 * ============================================================ */

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    g_tests_run++; \
    if (cond) { \
        g_tests_passed++; \
        printf("  [PASS] %s\n", msg); \
    } else { \
        g_tests_failed++; \
        printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
    } \
} while (0)

#define TEST_ASSERT_EQ(a, b, msg) do { \
    g_tests_run++; \
    if ((a) == (b)) { \
        g_tests_passed++; \
        printf("  [PASS] %s\n", msg); \
    } else { \
        g_tests_failed++; \
        printf("  [FAIL] %s: expected %ld, got %ld (line %d)\n", \
               msg, (long)(b), (long)(a), __LINE__); \
    } \
} while (0)

/* ============================================================
 *  UFS测试辅助函数
 * ============================================================ */

#define UFS_TEST_SECTOR_SIZE  512
#define UFS_TEST_LBA_START    100
#define UFS_TEST_SECTOR_COUNT 8  /* 8个扇区 = 4KB = 1个FTL页 */

/**
 * @brief 构造10字节SCSI CDB（大端格式）
 */
static void build_cdb_10(uint8_t *cdb, uint8_t opcode, uint32_t lba, uint16_t length)
{
    memset(cdb, 0, 16);
    cdb[0] = opcode;
    cdb[2] = (uint8_t)((lba >> 24) & 0xFF);
    cdb[3] = (uint8_t)((lba >> 16) & 0xFF);
    cdb[4] = (uint8_t)((lba >> 8) & 0xFF);
    cdb[5] = (uint8_t)(lba & 0xFF);
    cdb[7] = (uint8_t)((length >> 8) & 0xFF);
    cdb[8] = (uint8_t)(length & 0xFF);
}

/**
 * @brief 发送UFS命令并检查响应状态
 */
static ret_code_t send_ufs_cmd(uint8_t opcode, uint32_t lba, uint16_t length,
                                uint8_t *data, uint32_t data_len)
{
    ufs_cmd_request_t request;
    ufs_cmd_response_t response;

    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));
    request.header.trans_type = UPIU_TYPE_COMMAND;
    request.header.lun = 0;
    request.header.task_id = 1;

    build_cdb_10(request.cdb, opcode, lba, length);

    return ufs_target_process_cmd(&request, &response, data, data_len);
}

/* ============================================================
 *  测试用例
 * ============================================================ */

/**
 * @brief 测试UFS初始化
 */
static void test_ufs_init(void)
{
    ret_code_t ret;
    uint64_t total_sectors = 0;
    uint32_t sector_size = 0;

    printf("\n=== test_ufs_init ===\n");

    /* UFS依赖NAND和FTL层，必须先初始化 */
    ret = nand_init("/tmp/test_ufs_nand.bin");
    TEST_ASSERT_EQ(ret, RET_OK, "nand_init 返回 RET_OK");

    ret = ftl_init();
    TEST_ASSERT_EQ(ret, RET_OK, "ftl_init 返回 RET_OK");

    ret = ufs_target_init();
    TEST_ASSERT_EQ(ret, RET_OK, "ufs_target_init 返回 RET_OK");

    ret = ufs_target_get_capacity(&total_sectors, &sector_size);
    TEST_ASSERT_EQ(ret, RET_OK, "ufs_target_get_capacity 返回 RET_OK");
    TEST_ASSERT(total_sectors > 0, "总扇区数大于0");
    TEST_ASSERT_EQ(sector_size, UFS_TEST_SECTOR_SIZE, "扇区大小为512字节");

    printf("  [INFO] UFS容量: %llu 扇区 x %u 字节 = %.2f MB\n",
           (unsigned long long)total_sectors, sector_size,
           (double)(total_sectors * sector_size) / (1024 * 1024));
}

/**
 * @brief 测试TEST UNIT READY命令
 */
static void test_test_unit_ready(void)
{
    ret_code_t ret;
    ufs_cmd_request_t request;
    ufs_cmd_response_t response;

    printf("\n=== test_test_unit_ready ===\n");

    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));
    request.header.trans_type = UPIU_TYPE_COMMAND;
    request.header.lun = 0;
    request.cdb[0] = SCSI_OP_TEST_UNIT_READY;

    ret = ufs_target_process_cmd(&request, &response, NULL, 0);
    TEST_ASSERT_EQ(ret, RET_OK, "TEST UNIT READY 命令处理成功");
    TEST_ASSERT_EQ(response.header.scsi_status, UFS_STATUS_GOOD, "TEST UNIT READY 返回GOOD状态");
}

/**
 * @brief 测试INQUIRY命令
 */
static void test_inquiry(void)
{
    ret_code_t ret;
    uint8_t data[256];
    ufs_inquiry_data_t *inquiry = (ufs_inquiry_data_t *)data;

    printf("\n=== test_inquiry ===\n");

    memset(data, 0, sizeof(data));
    ret = send_ufs_cmd(SCSI_OP_INQUIRY, 0, 36, data, sizeof(data));
    TEST_ASSERT_EQ(ret, RET_OK, "INQUIRY 命令处理成功");

    TEST_ASSERT_EQ(inquiry->peripheral_type, 0x00, "外设类型为直接访问设备");
    TEST_ASSERT_EQ(inquiry->response_format, 0x02, "响应格式为2");
    TEST_ASSERT(inquiry->additional_length >= 31, "附加长度>=31");
    TEST_ASSERT(strncmp(inquiry->vendor, "FTLFW", 5) == 0, "厂商ID为FTLFW");

    printf("  [INFO] 厂商: %.8s 产品: %.16s 版本: %.4s\n",
           inquiry->vendor, inquiry->product, inquiry->revision);
}

/**
 * @brief 测试READ CAPACITY命令
 */
static void test_read_capacity(void)
{
    ret_code_t ret;
    uint8_t data[16];
    uint32_t returned_lba;
    uint32_t block_size;

    printf("\n=== test_read_capacity ===\n");

    memset(data, 0, sizeof(data));
    ret = send_ufs_cmd(SCSI_OP_READ_CAPACITY_10, 0, 0, data, sizeof(data));
    TEST_ASSERT_EQ(ret, RET_OK, "READ CAPACITY 命令处理成功");

    /* READ CAPACITY返回：最后LBA(4B大端) + 块大小(4B大端) */
    returned_lba = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                   ((uint32_t)data[2] << 8) | data[3];
    block_size = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
                 ((uint32_t)data[6] << 8) | data[7];

    TEST_ASSERT(returned_lba > 0, "返回LBA大于0");
    TEST_ASSERT_EQ(block_size, UFS_TEST_SECTOR_SIZE, "块大小为512字节");

    printf("  [INFO] 最后LBA: %u, 块大小: %u\n", returned_lba, block_size);
}

/**
 * @brief 测试WRITE命令
 */
static void test_write(void)
{
    ret_code_t ret;
    uint8_t write_buf[UFS_TEST_SECTOR_SIZE * UFS_TEST_SECTOR_COUNT];

    printf("\n=== test_write ===\n");

    /* 填充测试数据 */
    for (uint32_t i = 0; i < sizeof(write_buf); i++) {
        write_buf[i] = (uint8_t)(i & 0xFF);
    }

    ret = send_ufs_cmd(SCSI_OP_WRITE_10, UFS_TEST_LBA_START,
                        UFS_TEST_SECTOR_COUNT, write_buf, sizeof(write_buf));
    TEST_ASSERT_EQ(ret, RET_OK, "WRITE 命令处理成功");
}

/**
 * @brief 测试READ命令（读写一致性验证）
 */
static void test_read(void)
{
    ret_code_t ret;
    uint8_t read_buf[UFS_TEST_SECTOR_SIZE * UFS_TEST_SECTOR_COUNT];
    bool data_match = true;

    printf("\n=== test_read ===\n");

    memset(read_buf, 0, sizeof(read_buf));
    ret = send_ufs_cmd(SCSI_OP_READ_10, UFS_TEST_LBA_START,
                        UFS_TEST_SECTOR_COUNT, read_buf, sizeof(read_buf));
    TEST_ASSERT_EQ(ret, RET_OK, "READ 命令处理成功");

    /* 验证数据一致性 */
    for (uint32_t i = 0; i < sizeof(read_buf); i++) {
        if (read_buf[i] != (uint8_t)(i & 0xFF)) {
            data_match = false;
            printf("  [FAIL] 数据不匹配 at offset %u: expected 0x%02X, got 0x%02X\n",
                   i, (uint8_t)(i & 0xFF), read_buf[i]);
            break;
        }
    }
    TEST_ASSERT(data_match, "读写数据一致性验证通过");
}

/**
 * @brief 测试UNMAP（TRIM）命令
 */
static void test_unmap(void)
{
    ret_code_t ret;
    uint8_t data[16];

    printf("\n=== test_unmap ===\n");

    /* UNMAP参数列表：参数长度(2B) + 块描述符(8B) */
    memset(data, 0, sizeof(data));
    data[0] = 0x00;  /* 参数长度高字节 */
    data[1] = 0x08;  /* 参数长度低字节 = 8 */
    /* 块描述符：LBA(4B大端) + 块数(4B大端) */
    data[2] = (uint8_t)((UFS_TEST_LBA_START >> 24) & 0xFF);
    data[3] = (uint8_t)((UFS_TEST_LBA_START >> 16) & 0xFF);
    data[4] = (uint8_t)((UFS_TEST_LBA_START >> 8) & 0xFF);
    data[5] = (uint8_t)(UFS_TEST_LBA_START & 0xFF);
    data[6] = 0x00;
    data[7] = 0x00;
    data[8] = 0x00;
    data[9] = UFS_TEST_SECTOR_COUNT;

    ret = send_ufs_cmd(SCSI_OP_UNMAP, 0, 0, data, sizeof(data));
    TEST_ASSERT_EQ(ret, RET_OK, "UNMAP 命令处理成功");
}

/**
 * @brief 测试写保护功能
 */
static void test_write_protect(void)
{
    ret_code_t ret;
    uint8_t write_buf[UFS_TEST_SECTOR_SIZE];

    printf("\n=== test_write_protect ===\n");

    /* 启用写保护 */
    ret = ufs_target_set_write_protect(true);
    TEST_ASSERT_EQ(ret, RET_OK, "启用写保护成功");
    TEST_ASSERT(ufs_target_get_write_protect() == true, "写保护状态为启用");

    /* 写保护状态下写入应该失败（返回CHECK_CONDITION） */
    memset(write_buf, 0xAA, sizeof(write_buf));
    ufs_cmd_request_t request;
    ufs_cmd_response_t response;
    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));
    request.header.trans_type = UPIU_TYPE_COMMAND;
    build_cdb_10(request.cdb, SCSI_OP_WRITE_10, UFS_TEST_LBA_START + 100, 1);
    ret = ufs_target_process_cmd(&request, &response, write_buf, sizeof(write_buf));
    /* 写保护下写入应返回CHECK_CONDITION */
    TEST_ASSERT(response.header.scsi_status == UFS_STATUS_CHECK_CONDITION,
                "写保护状态下写入返回CHECK_CONDITION");

    /* 禁用写保护 */
    ret = ufs_target_set_write_protect(false);
    TEST_ASSERT_EQ(ret, RET_OK, "禁用写保护成功");
    TEST_ASSERT(ufs_target_get_write_protect() == false, "写保护状态为禁用");
}

/**
 * @brief 测试电源管理
 */
static void test_power_management(void)
{
    ret_code_t ret;

    printf("\n=== test_power_management ===\n");

    /* 切换到IDLE模式 */
    ret = ufs_target_set_power_mode(UFS_POWER_MODE_IDLE);
    TEST_ASSERT_EQ(ret, RET_OK, "切换到IDLE模式成功");
    TEST_ASSERT_EQ(ufs_target_get_power_mode(), UFS_POWER_MODE_IDLE, "当前模式为IDLE");

    /* 切换到SLEEP模式 */
    ret = ufs_target_set_power_mode(UFS_POWER_MODE_SLEEP);
    TEST_ASSERT_EQ(ret, RET_OK, "切换到SLEEP模式成功");
    TEST_ASSERT_EQ(ufs_target_get_power_mode(), UFS_POWER_MODE_SLEEP, "当前模式为SLEEP");

    /* 切换回ACTIVE模式 */
    ret = ufs_target_set_power_mode(UFS_POWER_MODE_ACTIVE);
    TEST_ASSERT_EQ(ret, RET_OK, "切换到ACTIVE模式成功");
    TEST_ASSERT_EQ(ufs_target_get_power_mode(), UFS_POWER_MODE_ACTIVE, "当前模式为ACTIVE");
}

/**
 * @brief 测试健康监控
 */
static void test_health_monitor(void)
{
    ret_code_t ret;
    ufs_health_info_t health;

    printf("\n=== test_health_monitor ===\n");

    ret = ufs_target_get_health_info(&health);
    TEST_ASSERT_EQ(ret, RET_OK, "获取健康信息成功");
    TEST_ASSERT(health.temperature >= -40 && health.temperature <= 125, "温度在合理范围");
    TEST_ASSERT(health.lifetime_used_percent <= 100, "寿命百分比<=100");
    TEST_ASSERT(health.power_on_count <= 0xFFFFFFFF, "上电次数在合理范围");

    printf("  [INFO] 温度: %d°C, 寿命已用: %u%%, 上电次数: %u\n",
           health.temperature, health.lifetime_used_percent, health.power_on_count);

    /* 测试健康监控周期处理 */
    ret = ufs_target_health_monitor_process();
    TEST_ASSERT_EQ(ret, RET_OK, "健康监控周期处理成功");
}

/**
 * @brief 测试错误统计
 */
static void test_error_stats(void)
{
    ret_code_t ret;
    ufs_error_stats_t stats;

    printf("\n=== test_error_stats ===\n");

    ret = ufs_target_get_error_stats(&stats);
    TEST_ASSERT_EQ(ret, RET_OK, "获取错误统计成功");
    TEST_ASSERT(stats.total_cmd_count <= 0xFFFFFFFF, "总命令数在合理范围");
    TEST_ASSERT(stats.success_count <= stats.total_cmd_count, "成功数<=总命令数");
    TEST_ASSERT(stats.retry_count <= 0xFFFFFFFF, "重试次数在合理范围");

    printf("  [INFO] 总命令: %u, 成功: %u, 重试: %u, 失败: %u\n",
           stats.total_cmd_count, stats.success_count,
           stats.retry_count, stats.failure_count);

    /* 重置错误统计 */
    ret = ufs_target_reset_error_stats();
    TEST_ASSERT_EQ(ret, RET_OK, "重置错误统计成功");

    ret = ufs_target_get_error_stats(&stats);
    TEST_ASSERT_EQ(stats.total_cmd_count, 0, "重置后总命令数为0");
}

/**
 * @brief 测试后台操作（BKOPS）
 */
static void test_background_ops(void)
{
    ret_code_t ret;

    printf("\n=== test_background_ops ===\n");

    ret = ufs_target_trigger_background_ops();
    TEST_ASSERT_EQ(ret, RET_OK, "触发后台操作成功");
}

/* ============================================================
 *  主函数
 * ============================================================ */

int main(void)
{
    printf("========================================\n");
    printf("  UFS 目标端单元测试\n");
    printf("========================================\n");

    /* 运行所有测试用例 */
    test_ufs_init();
    test_test_unit_ready();
    test_inquiry();
    test_read_capacity();
    test_write();
    test_read();
    test_unmap();
    test_write_protect();
    test_power_management();
    test_health_monitor();
    test_error_stats();
    test_background_ops();

    /* 反初始化 */
    ufs_target_deinit();
    ftl_deinit();
    nand_deinit();

    /* 打印测试结果 */
    printf("\n========================================\n");
    printf("  测试结果汇总\n");
    printf("========================================\n");
    printf("  总测试数: %d\n", g_tests_run);
    printf("  通过:     %d\n", g_tests_passed);
    printf("  失败:     %d\n", g_tests_failed);
    printf("  通过率:   %.1f%%\n",
           g_tests_run > 0 ? (double)g_tests_passed / g_tests_run * 100 : 0);
    printf("========================================\n");

    return g_tests_failed > 0 ? 1 : 0;
}
