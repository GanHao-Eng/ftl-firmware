/*
 * tests/test_gc_benchmark.c
 *
 * FTL GC 算法性能对比测试
 */

#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ftl.h"
#include "nand.h"
#include "log.h"

/* 测试配置 */
#define TEST_PAGE_SIZE      4096
#define TEST_LPNS           15000   /* 逻辑页数 */
#define TEST_FILL_RATIO     0.85    /* 初始填充 85% */
#define TEST_OVERWRITE_RATIO 0.6    /* 随机覆盖 60%（触发 GC） */
#define TEST_SEQ_RATIO      0.4     /* 顺序覆盖 40% */

/* GC 算法名称 */
static const char *gc_algo_names[] = {
    "Greedy",          /* GC_ALGO_GREEDY */
    "Cost-Benefit",    /* GC_ALGO_COST_BENEFIT */
    "CAT",             /* GC_ALGO_CAT */
    "Windowed",        /* GC_ALGO_WINDOWED */
    "d-Choices",       /* GC_ALGO_D_CHOICES */
    "FRA"              /* GC_ALGO_FRA */
};

/* 测试结果 */
typedef struct {
    gc_algo_type_t algo;
    const char *name;
    ftl_stats_t stats;
    double total_time_ms;
} gc_test_result_t;

/* 生成随机 LPN（不超过 max_lpn） */
static uint32_t rand_lpn(uint32_t max_lpn)
{
    return (uint32_t)(rand() % max_lpn);
}

/* 运行测试 workload */
static void run_workload(uint32_t fill_lpns, uint32_t overwrite_lpns,
                         uint32_t seq_lpns)
{
    uint8_t write_buf[TEST_PAGE_SIZE];
    uint32_t i = 0;
    uint32_t lpn = 0;

    /* 初始化写入数据 */
    for (i = 0; i < TEST_PAGE_SIZE; i++) {
        write_buf[i] = (uint8_t)(i & 0xFF);
    }

    /* 阶段1: 顺序写入填满 */
    printf("    阶段1: 顺序写入 %u 页...\n", fill_lpns);
    for (lpn = 0; lpn < fill_lpns; lpn++) {
        if (ftl_write(lpn, write_buf) != RET_OK) {
            printf("    警告: 写入 LPN=%u 失败\n", lpn);
        }
    }

    /* 阶段2: 随机覆盖写入 */
    printf("    阶段2: 随机覆盖写入 %u 页...\n", overwrite_lpns);
    for (i = 0; i < overwrite_lpns; i++) {
        lpn = rand_lpn(fill_lpns);
        write_buf[0] = (uint8_t)(i & 0xFF);  /* 改变数据 */
        if (ftl_write(lpn, write_buf) != RET_OK) {
            printf("    警告: 覆盖写入 LPN=%u 失败\n", lpn);
        }
    }

    /* 阶段3: 顺序覆盖写入 */
    printf("    阶段3: 顺序覆盖写入 %u 页...\n", seq_lpns);
    for (lpn = 0; lpn < seq_lpns; lpn++) {
        write_buf[0] = (uint8_t)(0xAA);  /* 标记数据 */
        if (ftl_write(lpn, write_buf) != RET_OK) {
            printf("    警告: 顺序覆盖 LPN=%u 失败\n", lpn);
        }
    }
}

/* 运行单个 GC 算法测试 */
static gc_test_result_t run_gc_test(gc_algo_type_t algo, uint32_t fill_lpns,
                                    uint32_t overwrite_lpns, uint32_t seq_lpns)
{
    gc_test_result_t result;
    struct timespec start, end;
    double elapsed_ms = 0;

    memset(&result, 0, sizeof(result));
    result.algo = algo;
    result.name = gc_algo_names[algo];

    printf("\n=== 测试算法: %s (algo=%d) ===\n", result.name, algo);

    /* 重新初始化 NAND 和 FTL */
    nand_init("/tmp/gc_test_nand.bin");
    ftl_set_gc_algo(algo);  /* 必须在 init 前设置 */
    ftl_init();

    /* 计时开始 */
    clock_gettime(CLOCK_MONOTONIC, &start);

    /* 运行 workload */
    run_workload(fill_lpns, overwrite_lpns, seq_lpns);

    /* 手动触发 GC（确保 GC 被调用） */
    ftl_trigger_gc();

    /* 计时结束 */
    clock_gettime(CLOCK_MONOTONIC, &end);
    elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0 +
                 (end.tv_nsec - start.tv_nsec) / 1000000.0;
    result.total_time_ms = elapsed_ms;

    /* 获取统计信息 */
    ftl_get_stats(&result.stats);

    printf("    完成: 耗时=%.1fms, WAF=%.2f, GC次数=%u, 搬迁页数=%u\n",
           result.total_time_ms, result.stats.waf,
           result.stats.gc_count, result.stats.gc_moved_pages);

    return result;
}

/* 打印对比表格 */
static void print_comparison(gc_test_result_t *results, int count)
{
    int i = 0;

    printf("\n");
    printf("============================================================\n");
    printf("           FTL GC 算法性能对比结果\n");
    printf("============================================================\n");
    printf("Workload: 顺序填充80%% → 随机覆盖50%% → 顺序覆盖30%%\n");
    printf("逻辑页: %u, 页大小: %d bytes\n", TEST_LPNS, TEST_PAGE_SIZE);
    printf("============================================================\n");
    printf("\n");
    printf("%-14s %8s %8s %10s %10s %12s %10s\n",
           "算法", "WAF", "GC次数", "搬迁页数", "主机写", "NAND写", "耗时(ms)");
    printf("------------------------------------------------------------\n");

    for (i = 0; i < count; i++) {
        printf("%-14s %8.2f %8u %10u %10u %12u %10.1f\n",
               results[i].name,
               results[i].stats.waf,
               results[i].stats.gc_count,
               results[i].stats.gc_moved_pages,
               results[i].stats.host_write_pages,
               results[i].stats.nand_write_pages,
               results[i].total_time_ms);
    }

    printf("------------------------------------------------------------\n");
    printf("\n");

    /* 找出 WAF 最低的算法 */
    int best_waf_idx = 0;
    for (i = 1; i < count; i++) {
        if (results[i].stats.waf < results[best_waf_idx].stats.waf) {
            best_waf_idx = i;
        }
    }
    printf("★ WAF 最低: %s (%.2f)\n",
           results[best_waf_idx].name, results[best_waf_idx].stats.waf);

    /* 找出 GC 次数最少的算法 */
    int best_gc_idx = 0;
    for (i = 1; i < count; i++) {
        if (results[i].stats.gc_count < results[best_gc_idx].stats.gc_count) {
            best_gc_idx = i;
        }
    }
    printf("★ GC次数最少: %s (%u次)\n",
           results[best_gc_idx].name, results[best_gc_idx].stats.gc_count);

    /* 找出耗时最短的算法 */
    int best_time_idx = 0;
    for (i = 1; i < count; i++) {
        if (results[i].total_time_ms < results[best_time_idx].total_time_ms) {
            best_time_idx = i;
        }
    }
    printf("★ 耗时最短: %s (%.1fms)\n",
           results[best_time_idx].name, results[best_time_idx].total_time_ms);

    printf("\n");
}

int main(int argc, char *argv[])
{
    gc_test_result_t results[6];
    int algo_count = 6;
    uint32_t fill_lpns = 0;
    uint32_t overwrite_lpns = 0;
    uint32_t seq_lpns = 0;
    int i = 0;

    (void)argc;
    (void)argv;

    printf("FTL GC 算法性能对比测试\n");
    printf("========================\n\n");

    /* 计算 workload 大小 */
    fill_lpns = (uint32_t)(TEST_LPNS * TEST_FILL_RATIO);
    overwrite_lpns = (uint32_t)(TEST_LPNS * TEST_OVERWRITE_RATIO);
    seq_lpns = (uint32_t)(TEST_LPNS * TEST_SEQ_RATIO);

    printf("测试配置:\n");
    printf("  总逻辑页: %u\n", TEST_LPNS);
    printf("  顺序填充: %u 页 (%.0f%%)\n", fill_lpns, TEST_FILL_RATIO * 100);
    printf("  随机覆盖: %u 页 (%.0f%%)\n", overwrite_lpns, TEST_OVERWRITE_RATIO * 100);
    printf("  顺序覆盖: %u 页 (%.0f%%)\n", seq_lpns, TEST_SEQ_RATIO * 100);

    /* 初始化随机数种子 */
    srand(42);  /* 固定种子，保证可重复 */

    /* 测试所有 6 种算法 */
    for (i = 0; i < algo_count; i++) {
        results[i] = run_gc_test((gc_algo_type_t)i, fill_lpns,
                                 overwrite_lpns, seq_lpns);
    }

    /* 打印对比结果 */
    print_comparison(results, algo_count);

    /* 清理 */
    remove("/tmp/gc_test_nand.bin");

    printf("测试完成。\n");
    return 0;
}
