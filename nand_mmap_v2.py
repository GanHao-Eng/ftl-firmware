#!/usr/bin/env python3
"""NAND层mmap优化脚本 - 完整版"""
import sys

file_path = r"E:\Learn\Code\ftl-firmware\modules\nand\nand.c"

with open(file_path, 'r', encoding='utf-8') as f:
    content = f.read()

def replace(old, new, desc):
    global content
    if old in content:
        content = content.replace(old, new, 1)
        print(f"[OK] {desc}")
        return True
    else:
        print(f"[FAIL] {desc} - pattern not found!")
        return False

# 1. 添加头文件
replace(
    '#include <pthread.h>',
    '#include <pthread.h>\n#include <sys/mman.h>\n#include <sys/stat.h>\n#include <fcntl.h>\n#include <unistd.h>',
    '添加头文件'
)

# 2. 修改 nand_dev_t 结构体
replace(
    '    FILE *media_file;             ///< 模拟介质的文件句柄\n    bool is_initialized;',
    '    FILE *media_file;             ///< 模拟介质的文件句柄（保留用于兼容性）\n    int   media_fd;               ///< 文件描述符（用于 mmap）\n    uint8_t *mmap_base;           ///< mmap 映射基地址（直接内存读写，无需系统调用）\n    size_t mmap_size;             ///< mmap 映射大小\n    bool is_initialized;',
    '修改nand_dev_t结构体'
)

# 3. 修改 g_nand_dev 初始化
replace(
    'static nand_dev_t g_nand_dev = {0};',
    'static nand_dev_t g_nand_dev = {\n    .media_fd = -1,\n    .mmap_base = NULL,\n    .mmap_size = 0,\n};',
    '修改g_nand_dev初始化'
)

# 4. 修改 nand_init() 中的文件打开和预分配
old_file_open = '''    /* 打开模拟介质文件
     * - 文件不存在：使用 "w+b" 全新创建并格式化（模拟出厂空NAND）
     * - 文件已存在：使用 "r+b" 保留已有数据（模拟掉电后重新上电）
     * 这样掉电恢复测试中，NAND数据可以正确保留 */
    g_nand_dev.media_file = fopen(file_path, "r+b");
    if (g_nand_dev.media_file == NULL) {
        /* 文件不存在，全新创建 */
        g_nand_dev.media_file = fopen(file_path, "w+b");
        if (g_nand_dev.media_file == NULL) {
            return RET_ERR_INTERNAL;
        }
        g_nand_dev.is_new_media = true;
    } else {
        g_nand_dev.is_new_media = false;
    }

    /* 预分配完整介质空间，模拟真实NAND容量（包含数据区和OOB区） */
    total_size = (long)NAND_TOTAL_BLOCKS * NAND_PAGES_PER_BLOCK * (NAND_PAGE_SIZE + NAND_OOB_SIZE);
    if (fseek(g_nand_dev.media_file, total_size - 1, SEEK_SET) != 0) {
        fclose(g_nand_dev.media_file);
        return RET_ERR_INTERNAL;
    }
    /* 写入一个字节以确保文件大小 */
    (void)fputc(0, g_nand_dev.media_file);'''

new_file_open = '''    /* 打开模拟介质文件（使用文件描述符以便mmap映射）
     * - 文件不存在：O_CREAT 全新创建
     * - 文件已存在：保留已有数据 */
    g_nand_dev.media_fd = open(file_path, O_RDWR | O_CREAT, 0644);
    if (g_nand_dev.media_fd < 0) {
        return RET_ERR_INTERNAL;
    }

    /* 检查文件是否为全新创建 */
    {
        struct stat st;
        if (fstat(g_nand_dev.media_fd, &st) == 0 && st.st_size == 0) {
            g_nand_dev.is_new_media = true;
        } else {
            g_nand_dev.is_new_media = false;
        }
    }

    /* 同时打开 FILE* 句柄，保留用于兼容性 */
    g_nand_dev.media_file = fdopen(g_nand_dev.media_fd, "r+b");
    if (g_nand_dev.media_file == NULL) {
        close(g_nand_dev.media_fd);
        return RET_ERR_INTERNAL;
    }

    /* 预分配完整介质空间 */
    total_size = (long)NAND_TOTAL_BLOCKS * NAND_PAGES_PER_BLOCK * (NAND_PAGE_SIZE + NAND_OOB_SIZE);
    if (ftruncate(g_nand_dev.media_fd, total_size) != 0) {
        fclose(g_nand_dev.media_file);
        close(g_nand_dev.media_fd);
        return RET_ERR_INTERNAL;
    }
    g_nand_dev.mmap_size = (size_t)total_size;

    /* mmap 映射文件到内存 */
    g_nand_dev.mmap_base = (uint8_t *)mmap(NULL, g_nand_dev.mmap_size,
                                              PROT_READ | PROT_WRITE, MAP_SHARED,
                                              g_nand_dev.media_fd, 0);
    if (g_nand_dev.mmap_base == MAP_FAILED) {
        fclose(g_nand_dev.media_file);
        close(g_nand_dev.media_fd);
        return RET_ERR_INTERNAL;
    }'''

replace(old_file_open, new_file_open, '修改nand_init文件打开')

# 5. 修改恢复模式 OOB 扫描
old_oob_scan = '''            oob_offset = nand_calc_oob_offset(blk, pg);
            (void)fseek(g_nand_dev.media_file, oob_offset, SEEK_SET);
            if (fread(&oob, sizeof(nand_oob_t), 1, g_nand_dev.media_file) == 1) {
                if (oob.magic == NAND_OOB_MAGIC) {'''

new_oob_scan = '''            oob_offset = nand_calc_oob_offset(blk, pg);
            /* 直接从 mmap 内存读取 OOB */
            (void)memcpy(&oob, g_nand_dev.mmap_base + oob_offset, sizeof(nand_oob_t));
            if (oob.magic == NAND_OOB_MAGIC) {'''

replace(old_oob_scan, new_oob_scan, '修改恢复模式OOB扫描')

# 6. 修改 nand_deinit()
old_deinit = '''{
    /* 关闭介质文件前flush所有缓冲数据，确保持久化 */
    if (g_nand_dev.media_file != NULL) {
        (void)fflush(g_nand_dev.media_file);
        (void)fclose(g_nand_dev.media_file);
        g_nand_dev.media_file = NULL;
    }

    /* 清除初始化标志 */
    g_nand_dev.is_initialized = false;
}'''

new_deinit = '''{
    /* mmap 映射内存同步到文件，确保所有修改持久化 */
    if (g_nand_dev.mmap_base != NULL) {
        (void)msync(g_nand_dev.mmap_base, g_nand_dev.mmap_size, MS_SYNC);
        (void)munmap(g_nand_dev.mmap_base, g_nand_dev.mmap_size);
        g_nand_dev.mmap_base = NULL;
    }

    /* 关闭介质文件 */
    if (g_nand_dev.media_file != NULL) {
        (void)fclose(g_nand_dev.media_file);
        g_nand_dev.media_file = NULL;
    }
    if (g_nand_dev.media_fd >= 0) {
        (void)close(g_nand_dev.media_fd);
        g_nand_dev.media_fd = -1;
    }

    /* 清除初始化标志 */
    g_nand_dev.is_initialized = false;
}'''

replace(old_deinit, new_deinit, '修改nand_deinit')

# 7. 修改 nand_page_read()
old_read = '''    /* 定位到指定页的偏移位置 */
    offset = nand_calc_page_offset(block, page);
    (void)fseek(g_nand_dev.media_file, offset, SEEK_SET);

    /* 读取一页数据 */
    (void)fread(buf, 1U, NAND_PAGE_SIZE, g_nand_dev.media_file);

    /* 读取OOB中的ECC校验值并进行纠错 */
    {
        nand_oob_t oob;
        ecc_result_t ecc_ret;
        (void)fseek(g_nand_dev.media_file, offset + NAND_PAGE_SIZE, SEEK_SET);
        if (fread(&oob, sizeof(nand_oob_t), 1, g_nand_dev.media_file) == 1) {
            if (oob.magic == NAND_OOB_MAGIC) {'''

new_read = '''    /* 定位到指定页的偏移位置（mmap 内存直接寻址，无需 fseek 系统调用） */
    offset = nand_calc_page_offset(block, page);

    /* 从 mmap 映射内存读取一页数据（直接内存拷贝，无需 fread 系统调用） */
    (void)memcpy(buf, g_nand_dev.mmap_base + offset, NAND_PAGE_SIZE);

    /* 读取OOB中的ECC校验值并进行纠错（直接从 mmap 内存读取） */
    {
        nand_oob_t oob;
        ecc_result_t ecc_ret;
        (void)memcpy(&oob, g_nand_dev.mmap_base + offset + NAND_PAGE_SIZE, sizeof(nand_oob_t));
        if (oob.magic == NAND_OOB_MAGIC) {'''

replace(old_read, new_read, '修改nand_page_read')

# 8. 修改 nand_page_write()
old_write = '''    /* 定位到指定页的偏移位置 */
    offset = nand_calc_page_offset(block, page);
    (void)fseek(g_nand_dev.media_file, offset, SEEK_SET);

    /* 写入一页数据 */
    (void)fwrite(buf, 1U, NAND_PAGE_SIZE, g_nand_dev.media_file);

    /* 写入 OOB 区域（标记页已写入，用于掉电恢复时重建页状态） */
    {
        nand_oob_t oob;
        uint32_t ecc_value = 0;
        (void)memset(&oob, 0, sizeof(nand_oob_t));
        oob.magic = NAND_OOB_MAGIC;
        oob.bad_block_mark = 0xFF;  /* 0xFF 表示正常块 */

        /* 计算数据的ECC校验值并写入OOB */
        (void)nand_ecc_hamming_encode(buf, NAND_PAGE_SIZE, &ecc_value);
        oob.ecc = ecc_value;

        (void)fseek(g_nand_dev.media_file, offset + NAND_PAGE_SIZE, SEEK_SET);
        (void)fwrite(&oob, sizeof(nand_oob_t), 1, g_nand_dev.media_file);
    }

    /* 注意：模拟器场景下不每次写入都fflush，由操作系统缓冲区管理
     * nand_deinit时统一flush，大幅提升写入性能
     * 真实SSD中数据写入后即持久化，模拟器通过文件系统缓冲模拟 */'''

new_write = '''    /* 定位到指定页的偏移位置（mmap 内存直接寻址，无需 fseek 系统调用） */
    offset = nand_calc_page_offset(block, page);

    /* 写入一页数据到 mmap 映射内存（直接内存拷贝，无需 fwrite 系统调用） */
    (void)memcpy(g_nand_dev.mmap_base + offset, buf, NAND_PAGE_SIZE);

    /* 写入 OOB 区域（标记页已写入，用于掉电恢复时重建页状态） */
    {
        nand_oob_t oob;
        uint32_t ecc_value = 0;
        (void)memset(&oob, 0, sizeof(nand_oob_t));
        oob.magic = NAND_OOB_MAGIC;
        oob.bad_block_mark = 0xFF;  /* 0xFF 表示正常块 */

        /* 计算数据的ECC校验值并写入OOB */
        (void)nand_ecc_hamming_encode(buf, NAND_PAGE_SIZE, &ecc_value);
        oob.ecc = ecc_value;

        /* 直接写入 mmap 映射内存的 OOB 区域 */
        (void)memcpy(g_nand_dev.mmap_base + offset + NAND_PAGE_SIZE, &oob, sizeof(nand_oob_t));
    }

    /* mmap MAP_SHARED 模式下，修改会由内核自动写回文件
     * 无需每次写入都 fflush，大幅提升写入性能
     * nand_deinit 时调用 msync 确保数据持久化 */'''

replace(old_write, new_write, '修改nand_page_write')

# 9. 修改 nand_mark_block_bad() 中的 OOB 写入
old_bad_block = '''        /* 定位到第一页的 OOB 区域 */
        offset = nand_calc_oob_offset(block, 0);
        (void)fseek(g_nand_dev.media_file, offset, SEEK_SET);
        (void)fwrite(&oob, 1U, sizeof(nand_oob_t), g_nand_dev.media_file);
        (void)fflush(g_nand_dev.media_file);'''

new_bad_block = '''        /* 定位到第一页的 OOB 区域（mmap 内存直接写入） */
        offset = nand_calc_oob_offset(block, 0);
        (void)memcpy(g_nand_dev.mmap_base + offset, &oob, sizeof(nand_oob_t));'''

replace(old_bad_block, new_bad_block, '修改nand_mark_block_bad')

# 10. 修改 nand_inject_retention_errors() 中的数据读取
old_inject_read = '''        /* 读取该页数据 */
        offset = nand_calc_page_offset(block, page);
        (void)fseek(g_nand_dev.media_file, offset, SEEK_SET);
        if (fread(buf, 1U, NAND_PAGE_SIZE, g_nand_dev.media_file) != NAND_PAGE_SIZE) {
            continue;
        }'''

new_inject_read = '''        /* 从 mmap 内存读取该页数据 */
        offset = nand_calc_page_offset(block, page);
        (void)memcpy(buf, g_nand_dev.mmap_base + offset, NAND_PAGE_SIZE);'''

replace(old_inject_read, new_inject_read, '修改nand_inject数据读取')

# 11. 修改 nand_inject_retention_errors() 中的数据写回
old_inject_write = '''        /* 写回错误数据（不更新OOB的ECC，模拟自然发生的位翻转） */
        (void)fseek(g_nand_dev.media_file, offset, SEEK_SET);
        (void)fwrite(buf, 1U, NAND_PAGE_SIZE, g_nand_dev.media_file);
    }

    (void)fflush(g_nand_dev.media_file);'''

new_inject_write = '''        /* 写回错误数据到 mmap 内存（不更新OOB的ECC，模拟自然发生的位翻转） */
        (void)memcpy(g_nand_dev.mmap_base + offset, buf, NAND_PAGE_SIZE);
    }'''

replace(old_inject_write, new_inject_write, '修改nand_inject数据写回')

# 12. 修改 nand_oob_read()
old_oob_read = '''    /* 定位到 OOB 区域的偏移位置 */
    offset = nand_calc_oob_offset(block, page);
    (void)fseek(g_nand_dev.media_file, offset, SEEK_SET);

    /* 读取 OOB 数据 */
    (void)fread(oob, 1U, sizeof(nand_oob_t), g_nand_dev.media_file);

    return RET_OK;
}'''

new_oob_read = '''    /* 从 mmap 内存读取 OOB 数据 */
    offset = nand_calc_oob_offset(block, page);
    (void)memcpy(oob, g_nand_dev.mmap_base + offset, sizeof(nand_oob_t));

    return RET_OK;
}'''

replace(old_oob_read, new_oob_read, '修改nand_oob_read')

# 13. 修改 nand_oob_write()
old_oob_write = '''    /* 定位到 OOB 区域的偏移位置 */
    offset = nand_calc_oob_offset(block, page);
    (void)fseek(g_nand_dev.media_file, offset, SEEK_SET);

    /* 写入 OOB 数据 */
    (void)fwrite(oob, 1U, sizeof(nand_oob_t), g_nand_dev.media_file);
    (void)fflush(g_nand_dev.media_file);

    return RET_OK;
}'''

new_oob_write = '''    /* 写入 OOB 数据到 mmap 内存 */
    offset = nand_calc_oob_offset(block, page);
    (void)memcpy(g_nand_dev.mmap_base + offset, oob, sizeof(nand_oob_t));

    return RET_OK;
}'''

replace(old_oob_write, new_oob_write, '修改nand_oob_write')

with open(file_path, 'w', encoding='utf-8') as f:
    f.write(content)

print("\nNAND mmap optimization completed!")
