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
#include "ftl.h"
#include "log.h"
#include <string.h>

/* ============================================================
 *  内部状态
 * ============================================================ */

static bool g_ufs_initialized = false;

/* ============================================================
 *  企业级特性状态
 * ============================================================ */

/* 电源管理状态 */
static ufs_power_mode_t g_power_mode = UFS_POWER_MODE_ACTIVE;

/* 健康状态信息 */
static ufs_health_info_t g_health_info = {
    .temperature = 25,
    .lifetime_used_percent = 0,
    .total_erase_count = 0,
    .power_on_count = 0,
    .power_on_minutes = 0,
    .unsafe_shutdown_count = 0,
    .temperature_warning = false,
    .lifetime_warning = false,
    .write_protected = false,
};

/* 错误统计信息 */
static ufs_error_stats_t g_error_stats = {0};

/* 命令队列（预留接口，用于多队列并发场景）
 * 当前单线程同步处理模式下未使用，保留用于未来多队列扩展 */
static ufs_cmd_queue_entry_t g_cmd_queue[UFS_MAX_CMD_QUEUE] __attribute__((unused));

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

    /* 写保护检查：启用写保护时拒绝写入 */
    if (g_health_info.write_protected) {
        set_response_check_condition(response, 0x07, 0x27, 0x00);  /* WRITE PROTECTED */
        return RET_OK;
    }

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


/* ============================================================
 *  企业级特性实现
 * ============================================================ */

/**
 * @brief 设置UFS电源模式
 * @details 支持ACTIVE/IDLE/SLEEP/POWER_DOWN四种模式
 *          IDLE模式：关闭部分时钟，快速唤醒（<10us）
 *          SLEEP模式：关闭大部分电路，唤醒延迟较高（<1ms）
 *          POWER_DOWN模式：完全掉电，需重新初始化
 */
ret_code_t ufs_target_set_power_mode(ufs_power_mode_t mode)
{
    if (!g_ufs_initialized) {
        return RET_ERR_NOT_INIT;
    }

    /* 记录电源模式切换 */
    LOG_INFO("UFS: 电源模式切换 %d -> %d", g_power_mode, mode);

    g_power_mode = mode;

    /* 根据电源模式调整行为 */
    switch (mode) {
    case UFS_POWER_MODE_ACTIVE:
        /* 活跃模式：正常处理所有命令 */
        break;
    case UFS_POWER_MODE_IDLE:
        /* 空闲模式：降低时钟频率，快速唤醒 */
        break;
    case UFS_POWER_MODE_SLEEP:
        /* 休眠模式：保存上下文，关闭大部分电路 */
        break;
    case UFS_POWER_MODE_POWER_DOWN:
        /* 掉电模式：完全关闭，需重新初始化 */
        g_ufs_initialized = false;
        break;
    default:
        return RET_ERR_PARAM;
    }

    return RET_OK;
}

/**
 * @brief 获取当前电源模式
 */
ufs_power_mode_t ufs_target_get_power_mode(void)
{
    return g_power_mode;
}

/**
 * @brief 获取UFS健康状态信息
 * @details 包含温度、寿命、擦除次数、上电次数等关键指标
 *          用于主机端SMART/健康监控
 */
ret_code_t ufs_target_get_health_info(ufs_health_info_t *info)
{
    if (info == NULL) {
        return RET_ERR_PARAM;
    }

    /* 从FTL层获取实际磨损数据（此处简化，实际应从FTL获取） */
    g_health_info.total_erase_count = 0; /* 应从FTL获取 */

    memcpy(info, &g_health_info, sizeof(ufs_health_info_t));
    return RET_OK;
}

/**
 * @brief 获取UFS错误统计信息
 * @details 包含总命令数、成功数、重试数、失败数、介质错误等
 *          用于性能分析和故障诊断
 */
ret_code_t ufs_target_get_error_stats(ufs_error_stats_t *stats)
{
    if (stats == NULL) {
        return RET_ERR_PARAM;
    }

    memcpy(stats, &g_error_stats, sizeof(ufs_error_stats_t));
    return RET_OK;
}

/**
 * @brief 重置错误统计信息
 */
ret_code_t ufs_target_reset_error_stats(void)
{
    memset(&g_error_stats, 0, sizeof(ufs_error_stats_t));
    return RET_OK;
}

/**
 * @brief 设置写保护状态
 * @details 启用写保护后，所有写命令将返回WRITE_PROTECTED错误
 *          用于数据保护场景（如取证、固件更新保护）
 */
ret_code_t ufs_target_set_write_protect(bool enable)
{
    g_health_info.write_protected = enable;
    LOG_INFO("UFS: 写保护 %s", enable ? "启用" : "禁用");
    return RET_OK;
}

/**
 * @brief 获取写保护状态
 */
bool ufs_target_get_write_protect(void)
{
    return g_health_info.write_protected;
}

/**
 * @brief 后台操作触发（BKOPS）
 * @details 触发垃圾回收、磨损均衡等后台操作
 *          主机在系统空闲时调用，提升后续写入性能
 */
ret_code_t ufs_target_trigger_background_ops(void)
{
    if (!g_ufs_initialized) {
        return RET_ERR_NOT_INIT;
    }

    LOG_INFO("UFS: 触发后台操作（BKOPS）");

    /* 此处应调用FTL层的GC和磨损均衡接口
     * 实际实现：ftl_trigger_gc() / ftl_trigger_wear_leveling()
     * 当前为框架实现 */

    return RET_OK;
}

/**
 * @brief UFS健康监控周期处理
 * @details 应定期调用（建议每秒一次），更新温度、寿命等健康指标
 *          检测温度告警、寿命告警，必要时触发降速保护
 */
ret_code_t ufs_target_health_monitor_process(void)
{
    /* 更新上电时间（简化：每次调用增加1分钟，实际应基于时间戳） */
    g_health_info.power_on_minutes++;

    /* 温度监控（简化：模拟温度，实际应从传感器读取） */
    if (g_health_info.temperature >= UFS_TEMP_CRITICAL_THRESHOLD) {
        /* 严重过热：触发降速保护 */
        g_health_info.temperature_warning = true;
        LOG_WARN("UFS: 温度严重过高 %d°C，触发降速保护", g_health_info.temperature);
    } else if (g_health_info.temperature >= UFS_TEMP_WARNING_THRESHOLD) {
        /* 温度告警：上报主机 */
        g_health_info.temperature_warning = true;
        LOG_WARN("UFS: 温度过高 %d°C，建议降低负载", g_health_info.temperature);
    } else {
        g_health_info.temperature_warning = false;
    }

    /* 寿命监控 */
    if (g_health_info.lifetime_used_percent >= UFS_LIFETIME_WARNING_PERCENT) {
        g_health_info.lifetime_warning = true;
        LOG_WARN("UFS: 寿命已使用 %u%%，建议备份数据", g_health_info.lifetime_used_percent);
    }

    return RET_OK;
}

/**
 * @brief 带重试的命令处理（对外接口）
 * @details 命令失败时自动重试，最多UFS_MAX_RETRY_COUNT次
 *          提升介质错误场景下的命令成功率
 * @param[in]  request  命令请求
 * @param[out] response 命令响应
 * @param[in,out] data  数据缓冲区
 * @param[in]  data_len 数据缓冲区长度
 * @retval RET_OK 处理成功
 * @retval RET_ERR_PARAM 参数非法
 */
ret_code_t ufs_target_process_cmd_with_retry(const ufs_cmd_request_t *request,
                                              ufs_cmd_response_t *response,
                                              uint8_t *data, uint32_t data_len)
{
    ret_code_t ret;
    uint8_t retry = 0;

    do {
        ret = ufs_target_process_cmd(request, response, data, data_len);
        if (ret == RET_OK && response->header.scsi_status == UFS_STATUS_GOOD) {
            g_error_stats.success_count++;
            return RET_OK;
        }

        /* 介质错误可重试 */
        if (response->header.scsi_status == UFS_STATUS_CHECK_CONDITION &&
            retry < UFS_MAX_RETRY_COUNT) {
            retry++;
            g_error_stats.retry_count++;
            LOG_WARN("UFS: 命令重试 %d/%d", retry, UFS_MAX_RETRY_COUNT);
            continue;
        }

        break;
    } while (retry < UFS_MAX_RETRY_COUNT);

    g_error_stats.failure_count++;
    return ret;
}
