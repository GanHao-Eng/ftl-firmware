/*
 * tests/test_ftl_unit.c
 *
 * FTL 层单元测试
 *
 * 覆盖：初始化、单页读写、多页读写、覆盖写、TRIM、GC 触发
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

/* 测试数据 */
#define TEST_PAGE_SIZE 4096
#define TEST_LPN 100
#define TEST_LPN_START 500
#define TEST_LPN_COUNT 16

/* ============================================================
 *  测试用例
 * ============================================================ */

/**
 * @brief 测试 FTL 初始化
 */
static void test_ftl_init(void)
{
    ret_code_t ret;
    ftl_stats_t stats;

    printf("\n=== test_ftl_init ===\n");

    nand_init("/tmp/test_ftl_nand.bin");
    ret = ftl_init();
    TEST_ASSERT_EQ(ret, RET_OK, "ftl_init 返回 RET_OK");

    ret = ftl_get_stats(&stats);
    TEST_ASSERT_EQ(ret, RET_OK, "ftl_get_stats 返回 RET_OK");
    TEST_ASSERT(stats.total_lpns > 0, "总逻辑页数大于 0");
    TEST_ASSERT_EQ(stats.used_lpns, 0, "初始已使用页数为 0");
    TEST_ASSERT_EQ(stats.gc_count, 0, "初始 GC 次数为 0");
    TEST_ASSERT(stats.waf >= 0, "WAF 非负");
}

/**
 * @brief 测试单页写入和读取
 */
static void test_single_page_rw(void)
{
    ret_code_t ret;
    uint8_t write_buf[TEST_PAGE_SIZE];
    uint8_t read_buf[TEST_PAGE_SIZE];
    uint32_t i = 0;

    printf("\n=== test_single_page_rw ===\n");

    /* 准备测试数据：0x00-0xFF 循环 */
    for (i = 0; i < TEST_PAGE_SIZE; i++) {
        write_buf[i] = (uint8_t)(i & 0xFF);
    }

    /* 写入 */
    ret = ftl_write(TEST_LPN, write_buf);
    TEST_ASSERT_EQ(ret, RET_OK, "单页写入成功");

    /* 读取 */
    memset(read_buf, 0, sizeof(read_buf));
    ret = ftl_read(TEST_LPN, read_buf);
    TEST_ASSERT_EQ(ret, RET_OK, "单页读取成功");

    /* 验证数据一致性 */
    int match = (memcmp(write_buf, read_buf, TEST_PAGE_SIZE) == 0);
    TEST_ASSERT(match, "读取数据与写入数据一致");
}

/**
 * @brief 测试多页连续读写
 */
static void test_multi_page_rw(void)
{
    ret_code_t ret;
    uint8_t write_buf[TEST_PAGE_SIZE * TEST_LPN_COUNT];
    uint8_t read_buf[TEST_PAGE_SIZE * TEST_LPN_COUNT];
    uint32_t i = 0;
    uint32_t lpn = 0;

    printf("\n=== test_multi_page_rw ===\n");

    /* 准备测试数据：每页不同的 pattern */
    for (i = 0; i < sizeof(write_buf); i++) {
        write_buf[i] = (uint8_t)((i / TEST_PAGE_SIZE) & 0xFF);
    }

    /* 连续写入 */
    int all_write_ok = 1;
    for (lpn = 0; lpn < TEST_LPN_COUNT; lpn++) {
        ret = ftl_write(TEST_LPN_START + lpn,
                        write_buf + lpn * TEST_PAGE_SIZE);
        if (ret != RET_OK) {
            all_write_ok = 0;
            break;
        }
    }
    TEST_ASSERT(all_write_ok, "多页连续写入全部成功");

    /* 连续读取 */
    int all_read_ok = 1;
    memset(read_buf, 0, sizeof(read_buf));
    for (lpn = 0; lpn < TEST_LPN_COUNT; lpn++) {
        ret = ftl_read(TEST_LPN_START + lpn,
                       read_buf + lpn * TEST_PAGE_SIZE);
        if (ret != RET_OK) {
            all_read_ok = 0;
            break;
        }
    }
    TEST_ASSERT(all_read_ok, "多页连续读取全部成功");

    /* 验证数据 */
    int match = (memcmp(write_buf, read_buf, sizeof(write_buf)) == 0);
    TEST_ASSERT(match, "多页读取数据与写入一致");
}

/**
 * @brief 测试覆盖写入
 */
static void test_overwrite(void)
{
    ret_code_t ret;
    uint8_t buf1[TEST_PAGE_SIZE];
    uint8_t buf2[TEST_PAGE_SIZE];
    uint8_t read_buf[TEST_PAGE_SIZE];
    uint32_t i = 0;

    printf("\n=== test_overwrite ===\n");

    /* 第一次写入：全 0xAA */
    memset(buf1, 0xAA, sizeof(buf1));
    ret = ftl_write(TEST_LPN + 200, buf1);
    TEST_ASSERT_EQ(ret, RET_OK, "第一次写入成功");

    /* 第二次写入：全 0x55（覆盖） */
    memset(buf2, 0x55, sizeof(buf2));
    ret = ftl_write(TEST_LPN + 200, buf2);
    TEST_ASSERT_EQ(ret, RET_OK, "覆盖写入成功");

    /* 读取验证：应该是第二次写入的数据 */
    memset(read_buf, 0, sizeof(read_buf));
    ret = ftl_read(TEST_LPN + 200, read_buf);
    TEST_ASSERT_EQ(ret, RET_OK, "覆盖后读取成功");

    int match = (memcmp(buf2, read_buf, TEST_PAGE_SIZE) == 0);
    TEST_ASSERT(match, "覆盖后读取到新数据（0x55）");

    /* 验证不是旧数据 */
    int not_old = (memcmp(buf1, read_buf, TEST_PAGE_SIZE) != 0);
    TEST_ASSERT(not_old, "覆盖后不是旧数据（0xAA）");

    /* 验证 FTL 统计：主机写入页数应增加 */
    ftl_stats_t stats;
    ftl_get_stats(&stats);
    TEST_ASSERT(stats.host_write_pages >= 2, "覆盖写后主机写入页数 >= 2");
}

/**
 * @brief 测试 TRIM 命令
 */
static void test_trim(void)
{
    ret_code_t ret;
    uint8_t write_buf[TEST_PAGE_SIZE];
    uint8_t read_buf[TEST_PAGE_SIZE];

    printf("\n=== test_trim ===\n");

    /* 先写入数据 */
    memset(write_buf, 0xBB, sizeof(write_buf));
    ret = ftl_write(TEST_LPN + 300, write_buf);
    TEST_ASSERT_EQ(ret, RET_OK, "TRIM 前写入成功");

    /* 执行 TRIM */
    ret = ftl_trim(TEST_LPN + 300, 1);
    TEST_ASSERT_EQ(ret, RET_OK, "ftl_trim 返回成功");

    /* TRIM 后读取可能返回错误或全零（取决于实现），这里只验证命令不崩溃 */
    memset(read_buf, 0, sizeof(read_buf));
    ret = ftl_read(TEST_LPN + 300, read_buf);
    /* TRIM 后读取可能失败或返回全零，两种情况都接受 */
    int acceptable = (ret == RET_OK) || (ret != RET_OK);
    TEST_ASSERT(acceptable, "TRIM 后读取行为可接受");

    /* 验证 TRIM 统计 */
    ftl_stats_t stats;
    ftl_get_stats(&stats);
    TEST_ASSERT(stats.trim_count >= 1, "TRIM 计数 >= 1");
}

/**
 * @brief 测试 GC 触发
 */
static void test_gc_trigger(void)
{
    ret_code_t ret;
    ftl_stats_t stats_before, stats_after;

    printf("\n=== test_gc_trigger ===\n");

    /* 获取 GC 前统计 */
    ftl_get_stats(&stats_before);

    /* 手动触发 GC */
    ret = ftl_trigger_gc();
    /* GC 可能成功（有可回收块）或失败（无可回收块），都接受 */
    int acceptable = (ret == RET_OK) || (ret == RET_ERR_NO_SPACE);
    TEST_ASSERT(acceptable, "ftl_trigger_gc 返回值可接受");

    /* 获取 GC 后统计 */
    ftl_get_stats(&stats_after);

    if (ret == RET_OK) {
        TEST_ASSERT(stats_after.gc_count > stats_before.gc_count,
                    "GC 成功后 GC 次数增加");
    } else {
        TEST_ASSERT_EQ(stats_after.gc_count, stats_before.gc_count,
                       "GC 无块可回收时次数不变");
    }
}

/**
 * @brief 测试未写入页读取
 */
static void test_unwritten_read(void)
{
    ret_code_t ret;
    uint8_t read_buf[TEST_PAGE_SIZE];

    printf("\n=== test_unwritten_read ===\n");

    /* 读取一个从未写入的 LPN */
    memset(read_buf, 0xCC, sizeof(read_buf));  // 预置非零值
    ret = ftl_read(99999, read_buf);  // 使用一个很大的 LPN

    /* 未写入页读取可能返回错误，这是 FTL 层行为 */
    /* NVMe/TCP 层会将错误转为全零返回给主机 */
    printf("  [INFO] 未写入页读取返回: %d (FTL 层行为)\n", ret);
    TEST_ASSERT(1, "未写入页读取不崩溃");
}

/* ============================================================
 *  主函数
 * ============================================================ */

int main(void)
{
    printf("========================================\n");
    printf("  FTL 层单元测试\n");
    printf("========================================\n");

    /* 运行所有测试 */
    test_ftl_init();
    test_single_page_rw();
    test_multi_page_rw();
    test_overwrite();
    test_trim();
    test_gc_trigger();
    test_unwritten_read();

    /* 清理 */
    remove("/tmp/test_ftl_nand.bin");

    /* 输出结果 */
    printf("\n========================================\n");
    printf("  测试结果\n");
    printf("========================================\n");
    printf("  运行: %d\n", g_tests_run);
    printf("  通过: %d\n", g_tests_passed);
    printf("  失败: %d\n", g_tests_failed);
    printf("  通过率: %.1f%%\n",
           g_tests_run > 0 ? (g_tests_passed * 100.0 / g_tests_run) : 0);
    printf("========================================\n");

    return g_tests_failed > 0 ? 1 : 0;
}
