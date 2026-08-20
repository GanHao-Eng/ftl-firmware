/**
 * @file ufs_target.c
 * @brief UFS（Universal Flash Storage）目标端简化实现
 * @details 实现UFS基本命令集框架，展示UFS协议架构理解
 *          支持SCSI命令集：INQUIRY/READ/WRITE/READ_CAPACITY/TEST_UNIT_READY/UNMAP
 *          数据存储对接FTL层（通过ftl_read/ftl_write接口）
 * @note UFS与NVMe的区别：
 *       - UFS基于SCSI命令集，NVMe是原生NVMe命令集
 *       - UFS使用UPIU传输，NVMe使用PCIe/NVMe-oF
 *       - UFS主要用于移动设备（手机），NVMe主要用于PC/服务器
 *       - 两者FTL层算法通用，仅前端协议不同
 */
#include "protocol/ufs_target.h"
#include "ftl/ftl.h"
#include "log/log.h"
#include <string.h>

/* ============================================================
 *  内部状态
 * ============================================================ */

static bool g_ufs_initialized = false;

/* UFS设备标识信息 */
static const ufs_inquiry_data_t g_ufs_inquiry = {
    .peripheral_type = 0x00,        /* 直接访问设备（磁盘） */
    .rmb = 0x00,                     /* 不可移动介质 */
    .version = 0x06,                 /* SPC-4版本 */
    .response_format = 0x02,         /* 响应格式2 */
    .additional_length = 31,         /* 附加长度=36-5 */
    .vendor = "FTLFW  ",             /* 厂商ID（8字节） */
    .product = "UFS-SSD-SIM   ",    /* 产品ID（16字节） */
    .revision = "1.0 "               /* 版本号（4字节） */
};

/* ============================================================
 *  内部辅助函数
 * ============================================================ */

/**
 * @brief 从10字节CDB中提取LBA（大端转主机字节序）
 */
static uint32_t cdb_get_lba_10(const uint8_t *cdb)
{
    return ((uint32_t)cdb[2] << 24) |
           ((uint32_t)cdb[3] << 16) |
           ((uint32_t)cdb[4] << 8)  |
           ((uint32_t)cdb[5]);
}

/**
 * @brief 从10字节CDB中提取传输长度（大端转主机字节序）
 */
static uint16_t cdb_get_length_10(const uint8_t *cdb)
{
    return ((uint16_t)cdb[7] << 8) | ((uint16_t)cdb[8]);
}

/**
 * @brief 设置响应状态为GOOD
 */
static void set_response_good(ufs_cmd_response_t *response)
{
    response->header.scsi_status = UFS_STATUS_GOOD;
    memset(response->sense_data, 0, sizeof(response->sense_data));
}

/**
 * @brief 设置响应状态为CHECK_CONDITION并填充感知数据
 */
static void set_response_check_condition(ufs_cmd_response_t *response,
                                          uint8_t sense_key, uint8_t asc, uint8_t ascq)
{
    response->header.scsi_status = UFS_STATUS_CHECK_CONDITION;
    memset(response->sense_data, 0, sizeof(response->sense_data));
    response->sense_data[0] = 0x70;         /* 响应代码=当前错误 */
    response->sense_data[2] = sense_key;     /* 感知键 */
    response->sense_data[7] = 10;            /* 附加感知长度 */
    response->sense_data[12] = asc;          /* 附加感知代码 */
    response->sense_data[13] = ascq;         /* 附加感知代码限定符 */
}

/* ============================================================
 *  SCSI 命令处理函数
 * ============================================================ */

/**
 * @brief 处理TEST UNIT READY命令（测试设备是否就绪）
 */
static ret_code_t handle_test_unit_ready(const ufs_cmd_request_t *request,
                                          ufs_cmd_response_t *response)
{
    (void)request;
    if (g_ufs_initialized) {
        set_response_good(response);
    } else {
        set_response_check_condition(response, 0x02, 0x04, 0x01);  /* 未就绪 */
    }
    return RET_OK;
}

/**
 * @brief 处理INQUIRY命令（查询设备信息）
 */
static ret_code_t handle_inquiry(const ufs_cmd_request_t *request,
                                  ufs_cmd_response_t *response,
                                  uint8_t *data, uint32_t data_len)
{
    (void)request;
    uint32_t copy_len = sizeof(ufs_inquiry_data_t);
    if (copy_len > data_len) {
        copy_len = data_len;
    }
    memcpy(data, &g_ufs_inquiry, copy_len);
    response->header.data_segment_len = copy_len;
    set_response_good(response);
    return RET_OK;
}

/**
 * @brief 处理READ CAPACITY命令（读取设备容量）
 */
static ret_code_t handle_read_capacity(const ufs_cmd_request_t *request,
                                        ufs_cmd_response_t *response,
                                        uint8_t *data, uint32_t data_len)
{
    (void)request;
    uint32_t lpn_size = 4096;  /* FTL页大小4KB */
    ftl_stats_t stats;

    /* 获取FTL总逻辑页数 */
    if (ftl_get_stats(&stats) != RET_OK) {
        set_response_check_condition(response, 0x04, 0x00, 0x00);
        return RET_ERR_INTERNAL;
    }
    uint64_t total_lpns = stats.total_lpns;

    /* UFS扇区大小512字节，FTL页4KB = 8个扇区 */
    uint64_t total_sectors = total_lpns * (lpn_size / UFS_SECTOR_SIZE);

    /* 填充READ CAPACITY响应（8字节：最后LBA(4) + 块大小(4)） */
    if (data_len >= 8) {
        uint32_t last_lba = (uint32_t)(total_sectors - 1);
        data[0] = (uint8_t)(last_lba >> 24);
        data[1] = (uint8_t)(last_lba >> 16);
        data[2] = (uint8_t)(last_lba >> 8);
        data[3] = (uint8_t)(last_lba);
        data[4] = 0;
        data[5] = 0;
        data[6] = (uint8_t)(UFS_SECTOR_SIZE >> 8);
        data[7] = (uint8_t)(UFS_SECTOR_SIZE);
        response->header.data_segment_len = 8;
    }
    set_response_good(response);
    return RET_OK;
}

/**
 * @brief 处理READ(10)命令（读取数据）
 */
static ret_code_t handle_read_10(const ufs_cmd_request_t *request,
                                  ufs_cmd_response_t *response,
                                  uint8_t *data, uint32_t data_len)
{
    uint32_t lba = cdb_get_lba_10(request->cdb);
    uint16_t sectors = cdb_get_length_10(request->cdb);
    uint32_t bytes_to_read = sectors * UFS_SECTOR_SIZE;
    uint32_t lpn = lba / 8;  /* 512字节扇区 → 4KB页（8扇区=1页） */
    uint32_t lpns = (bytes_to_read + 4095) / 4096;

    if (bytes_to_read > data_len) {
        set_response_check_condition(response, 0x05, 0x20, 0x00);  /* 无效命令操作码 */
        return RET_ERR_PARAM;
    }

    /* 逐页读取（对接FTL层） */
    for (uint32_t i = 0; i < lpns; i++) {
        ret_code_t ret = ftl_read(lpn + i, data + i * 4096);
        if (ret != RET_OK) {
            LOG_WARN("UFS READ: FTL读取失败, LPN=%u, ret=%d", lpn + i, ret);
            set_response_check_condition(response, 0x03, 0x11, 0x00);  /* 未恢复的读取错误 */
            return ret;
        }
    }

    response->header.data_segment_len = bytes_to_read;
    set_response_good(response);
    return RET_OK;
}

/**
 * @brief 处理WRITE(10)命令（写入数据）
 */
static ret_code_t handle_write_10(const ufs_cmd_request_t *request,
                                   ufs_cmd_response_t *response,
                                   uint8_t *data, uint32_t data_len)
{
    uint32_t lba = cdb_get_lba_10(request->cdb);
    uint16_t sectors = cdb_get_length_10(request->cdb);
    uint32_t bytes_to_write = sectors * UFS_SECTOR_SIZE;
    uint32_t lpn = lba / 8;
    uint32_t lpns = (bytes_to_write + 4095) / 4096;

    if (bytes_to_write > data_len) {
        set_response_check_condition(response, 0x05, 0x20, 0x00);
        return RET_ERR_PARAM;
    }

    /* 逐页写入（对接FTL层） */
    for (uint32_t i = 0; i < lpns; i++) {
        ret_code_t ret = ftl_write(lpn + i, data + i * 4096);
        if (ret != RET_OK) {
            LOG_WARN("UFS WRITE: FTL写入失败, LPN=%u, ret=%d", lpn + i, ret);
            set_response_check_condition(response, 0x03, 0x0C, 0x00);  /* 写入错误 */
            return ret;
        }
    }

    set_response_good(response);
    return RET_OK;
}

/**
 * @brief 处理SYNCHRONIZE CACHE命令（Flush缓存）
 */
static ret_code_t handle_synchronize_cache(const ufs_cmd_request_t *request,
                                             ufs_cmd_response_t *response)
{
    (void)request;
    /* 模拟器中数据直接写入NAND文件，无需额外flush */
    set_response_good(response);
    return RET_OK;
}

/**
 * @brief 处理UNMAP命令（TRIM/取消映射）
 */
static ret_code_t handle_unmap(const ufs_cmd_request_t *request,
                                ufs_cmd_response_t *response,
                                uint8_t *data, uint32_t data_len)
{
    (void)request;
    (void)data;
    (void)data_len;
    /* 简化实现：UNMAP命令直接返回成功 */
    /* 实际实现需要解析UNMAP参数列表，调用ftl_trim */
    set_response_good(response);
    return RET_OK;
}

/* ============================================================
 *  公共接口实现
 * ============================================================ */

ret_code_t ufs_target_init(void)
{
    if (g_ufs_initialized) {
        return RET_OK;
    }
    g_ufs_initialized = true;
    LOG_INFO("UFS目标端初始化完成");
    return RET_OK;
}

void ufs_target_deinit(void)
{
    g_ufs_initialized = false;
    LOG_INFO("UFS目标端反初始化完成");
}

ret_code_t ufs_target_process_cmd(const ufs_cmd_request_t *request,
                                   ufs_cmd_response_t *response,
                                   uint8_t *data, uint32_t data_len)
{
    if (request == NULL || response == NULL) {
        return RET_ERR_PARAM;
    }

    uint8_t opcode = request->cdb[0];
    ret_code_t ret = RET_OK;

    switch (opcode) {
    case SCSI_OP_TEST_UNIT_READY:
        ret = handle_test_unit_ready(request, response);
        break;
    case SCSI_OP_INQUIRY:
        ret = handle_inquiry(request, response, data, data_len);
        break;
    case SCSI_OP_READ_CAPACITY_10:
        ret = handle_read_capacity(request, response, data, data_len);
        break;
    case SCSI_OP_READ_10:
        ret = handle_read_10(request, response, data, data_len);
        break;
    case SCSI_OP_WRITE_10:
        ret = handle_write_10(request, response, data, data_len);
        break;
    case SCSI_OP_SYNCHRONIZE_CACHE:
        ret = handle_synchronize_cache(request, response);
        break;
    case SCSI_OP_UNMAP:
        ret = handle_unmap(request, response, data, data_len);
        break;
    default:
        LOG_WARN("UFS: 不支持的SCSI命令, opcode=0x%02X", opcode);
        set_response_check_condition(response, 0x05, 0x20, 0x00);  /* 无效命令操作码 */
        ret = RET_ERR_PARAM;
        break;
    }

    return ret;
}

ret_code_t ufs_target_get_capacity(uint64_t *total_sectors, uint32_t *sector_size)
{
    if (total_sectors == NULL || sector_size == NULL) {
        return RET_ERR_PARAM;
    }
    ftl_stats_t stats;
    if (ftl_get_stats(&stats) != RET_OK) {
        return RET_ERR_INTERNAL;
    }
    *total_sectors = (uint64_t)stats.total_lpns * (4096 / UFS_SECTOR_SIZE);
    *sector_size = UFS_SECTOR_SIZE;
    return RET_OK;
}
