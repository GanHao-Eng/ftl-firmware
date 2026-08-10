/**
 * @file raid.h
 * @brief RAID 模块头文件
 * @details 企业级固件的 RAID（独立磁盘冗余阵列）模块接口
 *          支持 RAID 0（条带化）和 RAID 1（镜像）
 *          管理多个 FTL 实例，对上层暴露统一的逻辑块地址空间
 */

#ifndef RAID_H
#define RAID_H

#include "common/common.h"
#include "ftl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  宏定义
 * ============================================================ */

/**
 * @brief RAID 最大成员数量
 */
#define RAID_MAX_MEMBERS 8

/**
 * @brief RAID 条带大小（页）
 */
#define RAID_STRIPE_SIZE_PAGES 4

/* ============================================================
 *  枚举定义
 * ============================================================ */

/**
 * @brief RAID 级别枚举
 */
typedef enum {
    RAID_LEVEL_0 = 0,    ///< RAID 0：条带化，无冗余
    RAID_LEVEL_1 = 1,    ///< RAID 1：镜像，完全冗余
    RAID_LEVEL_MAX = 2   ///< RAID 级别最大值
} raid_level_t;

/**
 * @brief RAID 状态枚举
 */
typedef enum {
    RAID_STATE_UNINIT = 0,    ///< 未初始化
    RAID_STATE_READY = 1,     ///< 就绪
    RAID_STATE_DEGRADED = 2,  ///< 降级（有成员故障）
    RAID_STATE_ERROR = 3,     ///< 错误状态
    RAID_STATE_MAX = 4        ///< 状态最大值
} raid_state_t;

/* ============================================================
 *  结构体定义
 * ============================================================ */

/**
 * @brief RAID 成员信息结构体
 */
typedef struct {
    bool is_valid;              ///< 是否有效
    bool is_online;             ///< 是否在线
    uint32_t ftl_instance_id;   ///< FTL 实例ID
    uint64_t total_lpns;        ///< 总逻辑页数量
    uint64_t read_count;        ///< 读取次数
    uint64_t write_count;       ///< 写入次数
    uint64_t error_count;       ///< 错误次数
} raid_member_t;

/**
 * @brief RAID 统计信息结构体
 */
typedef struct {
    uint64_t total_reads;       ///< 总读取次数
    uint64_t total_writes;      ///< 总写入次数
    uint64_t total_read_bytes;  ///< 总读取字节数
    uint64_t total_write_bytes; ///< 总写入字节数
    uint64_t rebuild_count;     ///< 重建次数
    uint64_t error_count;       ///< 总错误次数
} raid_stats_t;

/**
 * @brief RAID 配置结构体
 */
typedef struct {
    raid_level_t level;         ///< RAID 级别
    uint32_t member_count;      ///< 成员数量
    uint32_t stripe_size;       ///< 条带大小（页）
    bool auto_rebuild;          ///< 自动重建
} raid_config_t;

/* ============================================================
 *  接口函数声明
 * ============================================================ */

/**
 * @brief 初始化 RAID 模块
 * @param[in] config RAID 配置
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_INTERNAL 内部错误
 */
ret_code_t raid_init(const raid_config_t *config);

/**
 * @brief 反初始化 RAID 模块
 * @retval RET_OK 成功
 */
ret_code_t raid_deinit(void);

/**
 * @brief 添加 RAID 成员
 * @param[in] member_index 成员索引
 * @param[in] ftl_instance_id FTL 实例ID
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NO_SPACE 空间不足
 */
ret_code_t raid_add_member(uint32_t member_index, uint32_t ftl_instance_id);

/**
 * @brief 移除 RAID 成员
 * @param[in] member_index 成员索引
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t raid_remove_member(uint32_t member_index);

/**
 * @brief RAID 读操作
 * @param[in] lpn 逻辑页号
 * @param[out] buf 数据缓冲区
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_INIT 未初始化
 * @retval RET_ERR_INTERNAL 内部错误
 */
ret_code_t raid_read(uint64_t lpn, uint8_t *buf);

/**
 * @brief RAID 写操作
 * @param[in] lpn 逻辑页号
 * @param[in] buf 数据缓冲区
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_INIT 未初始化
 * @retval RET_ERR_INTERNAL 内部错误
 */
ret_code_t raid_write(uint64_t lpn, const uint8_t *buf);

/**
 * @brief 获取 RAID 状态
 * @return RAID 状态
 */
raid_state_t raid_get_state(void);

/**
 * @brief 获取 RAID 配置
 * @param[out] config RAID 配置
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t raid_get_config(raid_config_t *config);

/**
 * @brief 获取 RAID 统计信息
 * @param[out] stats 统计信息
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t raid_get_stats(raid_stats_t *stats);

/**
 * @brief 获取成员信息
 * @param[in] member_index 成员索引
 * @param[out] member 成员信息
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t raid_get_member_info(uint32_t member_index, raid_member_t *member);

/**
 * @brief 获取逻辑容量（页）
 * @return 逻辑页数量
 */
uint64_t raid_get_logical_capacity(void);

/**
 * @brief 打印 RAID 状态信息
 */
void raid_print_status(void);

#ifdef __cplusplus
}
#endif

#endif /* RAID_H */
