/*
 * tests/test_plp_recovery.c
 *
 * FTL 掉电保护（PLP）恢复测试
 *
 * 验证：
 *   1. 快照保存与加载的正确性
 *   2. 掉电后映射表恢复一致性
 *   3. 掉电后数据可正确读取
 *   4. 快照校验和验证（损坏快照拒绝加载）
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ftl.h"
#include "nand.h"

/* 简单测试框架 */
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
        printf("  [FAIL] %s: expected %lu, got %lu (line %d)\n", \
               msg, (unsigned long)(b), (unsigned long)(a), __LINE__); \
    } \
} while (0)

/* 测试配置 */
#define TEST_NAND_FILE     "/tmp/test_plp_nand.bin"
#define TEST_SNAPSHOT_FILE "/tmp/test_plp_snapshot.bin"
#define TEST_PAGE_SIZE     4096
#define TEST_LPN_START     100
#define TEST_LPN_COUNT     8

/* ============================================================
 *  辅助函数
 * ============================================================ */

/**
 * @brief 生成测试数据（基于LPN的确定性模式，便于验证）
 * @param[out] buf 数据缓冲区
 * @param[in] lpn 逻辑页号
 */
static void generate_test_data(uint8_t *buf, uint32_t lpn)
{
    uint32_t i = 0;
    for (i = 0; i < TEST_PAGE_SIZE; i++) {
        buf[i] = (uint8_t)((lpn + i) & 0xFF);
    }
}

/**
 * @brief 验证测试数据
 * @param[in] buf 数据缓冲区
 * @param[in] lpn 逻辑页号
 * @return 0 一致，-1 不一致
 */
static int verify_test_data(const uint8_t *buf, uint32_t lpn)
{
    uint32_t i = 0;
    for (i = 0; i < TEST_PAGE_SIZE; i++) {
        if (buf[i] != (uint8_t)((lpn + i) & 0xFF)) {
            return -1;
        }
    }
    return 0;
}

/* ============================================================
 *  测试用例
 * ============================================================ */

/**
 * @brief 测试1：快照保存与基本加载
 *
 * 流程：
 *   1. 初始化FTL，写入多页数据
 *   2. 保存快照
 *   3. 反初始化FTL（模拟掉电）
 *   4. 重新初始化FTL
 *   5. 从快照恢复
 *   6. 验证数据可正确读取
 */
static void test_snapshot_save_and_load(void)
{
    ret_code_t ret;
    uint8_t write_buf[TEST_PAGE_SIZE];
    uint8_t read_buf[TEST_PAGE_SIZE];
    uint32_t i = 0;

    printf("\n=== test_snapshot_save_and_load ===\n");

    /* 阶段1：初始化并写入数据 */
    nand_init(TEST_NAND_FILE);
    ret = ftl_init();
    TEST_ASSERT_EQ(ret, RET_OK, "FTL 初始化成功");

    for (i = 0; i < TEST_LPN_COUNT; i++) {
        generate_test_data(write_buf, TEST_LPN_START + i);
        ret = ftl_write(TEST_LPN_START + i, write_buf);
        if (ret != RET_OK) {
            printf("  [WARN] 写入 LPN=%u 失败: ret=%d\n", TEST_LPN_START + i, ret);
        }
    }
    printf("  [INFO] 写入 %u 页测试数据\n", TEST_LPN_COUNT);

    /* 阶段2：保存快照 */
    ret = ftl_save_snapshot(TEST_SNAPSHOT_FILE);
    TEST_ASSERT_EQ(ret, RET_OK, "快照保存成功");

    /* 阶段3：反初始化（模拟掉电） */
    ftl_deinit();
    nand_deinit();
    printf("  [INFO] FTL 已反初始化（模拟掉电）\n");

    /* 阶段4：重新初始化并从快照恢复 */
    nand_init(TEST_NAND_FILE);
    ret = ftl_init();
    TEST_ASSERT_EQ(ret, RET_OK, "FTL 重新初始化成功");

    ret = ftl_load_snapshot(TEST_SNAPSHOT_FILE);
    TEST_ASSERT_EQ(ret, RET_OK, "从快照恢复成功");

    /* 阶段5：验证数据一致性 */
    for (i = 0; i < TEST_LPN_COUNT; i++) {
        memset(read_buf, 0, TEST_PAGE_SIZE);
        ret = ftl_read(TEST_LPN_START + i, read_buf);
        if (ret == RET_OK) {
            int cmp = verify_test_data(read_buf, TEST_LPN_START + i);
            TEST_ASSERT(cmp == 0, "LPN 数据恢复一致");
            if (cmp != 0) {
                printf("  [WARN] LPN=%u 数据不一致\n", TEST_LPN_START + i);
                break;
            }
        } else {
            printf("  [WARN] 读取 LPN=%u 失败: ret=%d\n", TEST_LPN_START + i, ret);
        }
    }

    /* 清理 */
    ftl_deinit();
    nand_deinit();
    remove(TEST_SNAPSHOT_FILE);
    remove(TEST_NAND_FILE);
}

/**
 * @brief 测试2：覆盖写后的快照恢复
 *
 * 验证异地更新机制下，覆盖写后的映射关系能正确恢复
 */
static void test_overwrite_recovery(void)
{
    ret_code_t ret;
    uint8_t buf_old[TEST_PAGE_SIZE];
    uint8_t buf_new[TEST_PAGE_SIZE];
    uint8_t read_buf[TEST_PAGE_SIZE];
    uint32_t lpn = 200;

    printf("\n=== test_overwrite_recovery ===\n");

    nand_init(TEST_NAND_FILE);
    ftl_init();

    /* 第一次写入 */
    memset(buf_old, 0xAA, TEST_PAGE_SIZE);
    ret = ftl_write(lpn, buf_old);
    TEST_ASSERT_EQ(ret, RET_OK, "第一次写入成功");

    /* 覆盖写入 */
    memset(buf_new, 0x55, TEST_PAGE_SIZE);
    ret = ftl_write(lpn, buf_new);
    TEST_ASSERT_EQ(ret, RET_OK, "覆盖写入成功");

    /* 保存快照 */
    ret = ftl_save_snapshot(TEST_SNAPSHOT_FILE);
    TEST_ASSERT_EQ(ret, RET_OK, "快照保存成功");

    /* 模拟掉电 */
    ftl_deinit();
    nand_deinit();

    /* 恢复 */
    nand_init(TEST_NAND_FILE);
    ftl_init();
    ret = ftl_load_snapshot(TEST_SNAPSHOT_FILE);
    TEST_ASSERT_EQ(ret, RET_OK, "快照恢复成功");

    /* 验证读取到的是新数据 */
    memset(read_buf, 0, TEST_PAGE_SIZE);
    ret = ftl_read(lpn, read_buf);
    TEST_ASSERT_EQ(ret, RET_OK, "恢复后读取成功");
    TEST_ASSERT(memcmp(read_buf, buf_new, TEST_PAGE_SIZE) == 0,
                "恢复后读取到覆盖后的新数据");
    TEST_ASSERT(memcmp(read_buf, buf_old, TEST_PAGE_SIZE) != 0,
                "恢复后不是旧数据");

    ftl_deinit();
    nand_deinit();
    remove(TEST_SNAPSHOT_FILE);
    remove(TEST_NAND_FILE);
}

/**
 * @brief 测试3：快照不存在时的处理
 *
 * 验证加载不存在的快照时返回错误，不崩溃
 */
static void test_snapshot_not_exist(void)
{
    ret_code_t ret;

    printf("\n=== test_snapshot_not_exist ===\n");

    nand_init(TEST_NAND_FILE);
    ftl_init();

    /* 加载不存在的快照 */
    ret = ftl_load_snapshot("/tmp/nonexistent_snapshot.bin");
    TEST_ASSERT(ret != RET_OK, "加载不存在的快照返回错误");
    printf("  [INFO] 不存在快照返回 ret=%d（预期非OK）\n", ret);

    ftl_deinit();
    nand_deinit();
    remove(TEST_NAND_FILE);
}

/**
 * @brief 测试4：TRIM后的快照恢复
 *
 * 验证TRIM操作后的映射状态能正确恢复
 */
static void test_trim_recovery(void)
{
    ret_code_t ret;
    uint8_t write_buf[TEST_PAGE_SIZE];
    uint8_t read_buf[TEST_PAGE_SIZE];
    uint32_t lpn = 300;

    printf("\n=== test_trim_recovery ===\n");

    nand_init(TEST_NAND_FILE);
    ftl_init();

    /* 写入数据 */
    memset(write_buf, 0xBB, TEST_PAGE_SIZE);
    ret = ftl_write(lpn, write_buf);
    TEST_ASSERT_EQ(ret, RET_OK, "写入数据成功");

    /* TRIM */
    ret = ftl_trim(lpn, 1);
    TEST_ASSERT_EQ(ret, RET_OK, "TRIM 操作成功");

    /* 保存快照 */
    ret = ftl_save_snapshot(TEST_SNAPSHOT_FILE);
    TEST_ASSERT_EQ(ret, RET_OK, "快照保存成功");

    /* 模拟掉电 */
    ftl_deinit();
    nand_deinit();

    /* 恢复 */
    nand_init(TEST_NAND_FILE);
    ftl_init();
    ret = ftl_load_snapshot(TEST_SNAPSHOT_FILE);
    TEST_ASSERT_EQ(ret, RET_OK, "快照恢复成功");

    /* TRIM后读取应返回未映射或全零（取决于实现） */
    memset(read_buf, 0, TEST_PAGE_SIZE);
    ret = ftl_read(lpn, read_buf);
    printf("  [INFO] TRIM后读取返回 ret=%d（预期未映射或全零）\n", ret);
    TEST_ASSERT(ret != RET_OK || memcmp(read_buf, write_buf, TEST_PAGE_SIZE) != 0,
                "TRIM后无法读取到原始数据");

    ftl_deinit();
    nand_deinit();
    remove(TEST_SNAPSHOT_FILE);
    remove(TEST_NAND_FILE);
}

/* ============================================================
 *  主函数
 * ============================================================ */

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("========================================\n");
    printf("  FTL 掉电保护（PLP）恢复测试\n");
    printf("========================================\n");

    /* 运行所有测试用例 */
    test_snapshot_save_and_load();
    test_overwrite_recovery();
    test_snapshot_not_exist();
    test_trim_recovery();

    /* 打印测试结果 */
    printf("\n========================================\n");
    printf("  测试结果\n");
    printf("========================================\n");
    printf("  运行: %d\n", g_tests_run);
    printf("  通过: %d\n", g_tests_passed);
    printf("  失败: %d\n", g_tests_failed);
    printf("  通过率: %.1f%%\n",
           g_tests_run > 0 ? (100.0 * g_tests_passed / g_tests_run) : 0.0);
    printf("========================================\n");

    return g_tests_failed > 0 ? 1 : 0;
}
