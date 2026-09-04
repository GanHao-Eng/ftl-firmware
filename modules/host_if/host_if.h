/**
 * @file host_if.h
 * @brief 主机接口模块
 * @details 主机接口模块，模拟 NVMe 协议接口
 */

#ifndef FIRMWARE_HOST_IF_H
#define FIRMWARE_HOST_IF_H

#include "common/common.h"
#include "msg_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  NVMe 命令定义
 * ============================================================ */

/**
 * @brief NVMe 命令操作码
 */
typedef enum {
    /* I/O 命令 */
    NVME_CMD_FLUSH = 0x00,          ///< Flush 命令
    NVME_CMD_WRITE = 0x01,          ///< Write 命令
    NVME_CMD_READ = 0x02,           ///< Read 命令
    NVME_CMD_WRITE_UNCOR = 0x04,    ///< Write Uncorrectable 命令
    NVME_CMD_COMPARE = 0x05,        ///< Compare 命令
    NVME_CMD_WRITE_ZEROES = 0x08,   ///< Write Zeroes 命令
    NVME_CMD_DATASET_MGMT = 0x09,   ///< Dataset Management (TRIM) 命令
    NVME_CMD_VERIFY = 0x0C,         ///< Verify 命令

    /* Admin 命令 */
    NVME_ADMIN_DELETE_SQ = 0x00,    ///< 删除提交队列
    NVME_ADMIN_CREATE_SQ = 0x01,    ///< 创建提交队列
    NVME_ADMIN_GET_LOG_PAGE = 0x02, ///< 获取日志页
    NVME_ADMIN_DELETE_CQ = 0x04,    ///< 删除完成队列
    NVME_ADMIN_CREATE_CQ = 0x05,    ///< 创建完成队列
    NVME_ADMIN_IDENTIFY = 0x06,     ///< Identify（识别控制器/命名空间）
    NVME_ADMIN_ABORT = 0x08,        ///< 中止命令
    NVME_ADMIN_SET_FEATURE = 0x09,  ///< 设置特性
    NVME_ADMIN_GET_FEATURE = 0x0A,  ///< 获取特性
    NVME_ADMIN_ASYNC_EVENT = 0x0C,  ///< 异步事件请求
    NVME_ADMIN_FW_ACTIVATE = 0x10,  ///< 固件激活
    NVME_ADMIN_FW_DOWNLOAD = 0x11,  ///< 固件下载
    NVME_ADMIN_FORMAT_NVM = 0x80,   ///< 格式化NVM
    NVME_ADMIN_SECURITY_SEND = 0x81,///< 安全发送
    NVME_ADMIN_SECURITY_RECV = 0x82,///< 安全接收

    NVME_CMD_MAX = 0x100            ///< 命令码最大值
} nvme_opcode_t;

/**
 * @brief NVMe 命令状态
 */
typedef enum {
    NVME_STATUS_SUCCESS = 0x00,         ///< 成功
    NVME_STATUS_INVALID_OPCODE = 0x01,  ///< 无效操作码
    NVME_STATUS_INVALID_FIELD = 0x02,   ///< 无效字段
    NVME_STATUS_DATA_TRANSFER = 0x03,   ///< 数据传输错误
    NVME_STATUS_ABORTED = 0x04,         ///< 命令已中止
    NVME_STATUS_INTERNAL_ERROR = 0x05,  ///< 内部错误
    NVME_STATUS_MAX = 0x10              ///< 状态码最大值
} nvme_status_t;

/**
 * @brief NVMe 命令结构体
 */
typedef struct {
    nvme_opcode_t opcode;     ///< 操作码
    uint32_t nsid;            ///< 命名空间ID
    uint64_t slba;            ///< 起始逻辑块地址
    uint32_t nlb;             ///< 逻辑块数量（0 表示 1 个）
    uint16_t cid;             ///< 命令ID
    uint8_t fua;              ///< 强制单元访问
    uint8_t lr;               ///< 受限重试
    uint8_t *data_buf;        ///< 数据缓冲区指针
    uint32_t data_len;        ///< 数据长度
    bool is_admin;            ///< 是否为Admin命令（区分Admin和I/O队列）
} nvme_cmd_t;

/**
 * @brief NVMe 完成队列条目
 */
typedef struct {
    uint32_t dw0;             ///< 命令特定信息
    uint32_t reserved;        ///< 保留
    uint16_t sqhd;            ///< 提交队列头指针
    uint16_t sqid;            ///< 提交队列ID
    uint16_t cid;             ///< 命令ID
    nvme_status_t status;     ///< 状态码
    uint8_t phase_tag;        ///< 阶段标签
} nvme_cqe_t;

/* ============================================================
 *  NVMe Identify 数据结构
 * ============================================================ */

/**
 * @brief Identify CNS（Controller or Namespace Structure）值
 */
typedef enum {
    NVME_IDENTIFY_CNS_NAMESPACE = 0x00,   ///< 识别命名空间
    NVME_IDENTIFY_CNS_CONTROLLER = 0x01,  ///< 识别控制器
    NVME_IDENTIFY_CNS_NS_LIST = 0x02,     ///< 命名空间列表
    NVME_IDENTIFY_CNS_NS_DESCRIPTOR = 0x03 ///< 命名空间描述符列表
} nvme_identify_cns_t;

/**
 * @brief NVMe Identify Controller 数据结构（简化版，4096字节）
 * @details 包含控制器能力、序列号、固件版本、PCIe信息等
 */
typedef struct {
    /* 控制器能力 */
    uint16_t vid;               ///< 厂商ID (PCI Vendor ID)
    uint16_t ssvid;             ///< 子系统厂商ID
    uint8_t  sn[20];            ///< 序列号 (Serial Number)
    uint8_t  mn[40];            ///< 型号 (Model Number)
    uint8_t  fr[8];             ///< 固件版本 (Firmware Revision)
    uint8_t  rab;               ///< 推荐仲裁突发
    uint8_t  ieee[3];           ///< IEEE OUI标识符
    uint8_t  cmic;              ///< 控制器多路径I/O能力
    uint8_t  mdts;              ///< 最大数据传输大小
    uint16_t cntlid;            ///< 控制器ID
    uint32_t ver;               ///< 版本号
    uint32_t rtd3r;             ///< RTD3恢复延迟
    uint32_t rtd3e;             ///< RTD3进入延迟
    uint32_t oaes;              ///< 可选异步事件支持
    uint32_t ctratt;            ///< 控制器属性

    /* 管理能力 */
    uint16_t oacs;              ///< 可选Admin命令支持
    uint8_t  acl;               ///< 中止命令限制
    uint8_t  aerl;              ///< 异步事件请求限制
    uint8_t  frmw;              ///< 固件更新配置
    uint8_t  lpa;               ///< 日志页属性
    uint8_t  elpe;              ///< 错误日志条目数
    uint8_t  npss;              ///< 电源状态数
    uint8_t  avscc;             ///< 管理Vendor特定命令配置
    uint8_t  apsta;             ///< 自动电源状态转换支持
    uint16_t wctemp;            ///< 警告温度阈值
    uint16_t cctemp;            ///< 临界温度阈值
    uint16_t mtfa;              ///< 最大固件激活时间
    uint32_t hmpre;             ///< 主机内存缓冲区首选大小
    uint32_t hmmin;             ///< 主机内存缓冲区最小大小
    uint8_t  tnvmcap[16];       ///< 总NVM容量
    uint8_t  unvmcap[16];       ///< 未分配NVM容量
    uint32_t rpmbs;             ///< 可重放内存块支持
    uint16_t edstt;             ///< 扩展设备自检时间
    uint8_t  dsto;              ///< 设备自检选项
    uint8_t  fwug;              ///< 固件更新粒度
    uint16_t kas;               ///< 保持活动支持
    uint16_t hctma;             ///< 主机控制热管理支持
    uint16_t mntmt;             ///< 最低温度
    uint16_t mxtmt;             ///< 最高温度
    uint32_t sanicap;           ///< 消毒能力
    uint32_t hmminds;           ///< 主机内存缓冲区最小描述符数
    uint16_t hmmaxd;            ///< 主机内存缓冲区最大描述符数
    uint16_t nsetidmax;         ///< 最大NVM Set ID
    uint16_t endgidmax;         ///< 最大Endurance Group ID
    uint8_t  anatt;             ///< 非对称命名空间访问转换时间
    uint8_t  anacap;            ///< 非对称命名空间访问能力
    uint8_t  anagrpmax;         ///< 最大ANA组ID
    uint8_t  nanagrpid;         ///< ANA组ID数
    uint32_t pels;              ///< 持久事件日志支持

    /* 保留 */
    uint8_t  reserved1[156];

    /* 命令集能力 */
    uint8_t  sqes;              ///< 提交队列条目大小
    uint8_t  cqes;              ///< 完成队列条目大小
    uint16_t maxcmd;            ///< 最大命令数
    uint32_t nn;                ///< 命名空间数
    uint16_t oncs;              ///< 可选NVM命令支持
    uint16_t fuses;             ///< 融合操作支持
    uint8_t  fna;               ///< 格式化NVM属性
    uint8_t  vwc;               ///< 易失性写缓存
    uint16_t awun;              ///< 原子写单位NVM
    uint16_t awupf;             ///< 原子写单位电源故障
    uint8_t  nvscc;             ///< NVM Vendor特定命令配置
    uint8_t  nwpc;              ///< 命名空间写保护能力
    uint16_t acwu;              ///< 原子比较写单位
    uint8_t  reserved2[2];
    uint32_t sgls;              ///< SGL支持
    uint32_t mnan;              ///< 最大命名空间数

    /* 保留 */
    uint8_t  reserved3[224];

    /* 电源状态描述符 */
    uint8_t  psd[32][32];       ///< 电源状态描述符（最多32个状态）

    /* 厂商特定 */
    uint8_t  vs[1024];          ///< 厂商特定区域
} nvme_id_ctrl_t;

/**
 * @brief NVMe Identify Namespace 数据结构（简化版，4096字节）
 */
typedef struct {
    uint64_t nsze;              ///< 命名空间大小（LBA数）
    uint64_t ncap;              ///< 命名空间容量
    uint64_t nuse;              ///< 命名空间已使用
    uint8_t  nsfeat;            ///< 命名空间特性
    uint8_t  nlbaf;             ///< LBA格式数
    uint8_t  flbas;             ///< 格式化LBA大小
    uint8_t  mc;                ///< 元数据能力
    uint8_t  dpc;               ///< 端到端数据保护能力
    uint8_t  dps;               ///< 端到端数据保护设置
    uint8_t  nmic;              ///< 命名空间多路径I/O能力
    uint8_t  rescap;            ///< 预留能力
    uint8_t  fpi;               ///< 格式化进度指示器
    uint8_t  dlfeat;            ///< 去重特性
    uint16_t nawun;             ///< 命名空间原子写单位
    uint16_t nawupf;            ///< 命名空间原子写单位电源故障
    uint16_t nacwu;             ///< 命名空间原子比较写单位
    uint16_t nabsn;             ///< 命名空间原子边界大小NVM
    uint16_t nabo;              ///< 命名空间原子边界偏移
    uint16_t nabspf;            ///< 命名空间原子边界大小电源故障
    uint16_t noiob;             ///< 最佳I/O边界
    uint64_t nvmcap[2];         ///< NVM容量
    uint16_t npwg;              ///< 最佳写粒度
    uint16_t npwa;              ///< 最佳写对齐
    uint16_t npdg;              ///< 最佳去重粒度
    uint16_t npda;              ///< 最佳去重对齐
    uint16_t nows;              ///< 最佳写大小
    uint16_t mssrl;             ///< 最大单源范围长度
    uint32_t mcl;               ///< 最大拷贝长度
    uint8_t  msrc;              ///< 最大单源范围数
    uint8_t  reserved1[11];
    uint32_t anagrpid;          ///< ANA组ID
    uint8_t  reserved2[3];
    uint8_t  nsattr;            ///< 命名空间属性
    uint16_t nvmsetid;          ///< NVM Set ID
    uint16_t endgid;            ///< Endurance Group ID
    uint16_t nguid[8];          ///< 命名空间全局唯一标识符
    uint8_t  eui64[8];          ///< IEEE扩展唯一标识符
    uint8_t  lbaf[64][4];       ///< LBA格式支持（最多64种）

    /* 保留和厂商特定 */
    uint8_t  reserved3[192];
    uint8_t  vs[3712];          ///< 厂商特定区域
} nvme_id_ns_t;

/* ============================================================
 *  NVMe Log Page 定义
 * ============================================================ */

/**
 * @brief NVMe Log Page ID
 */
typedef enum {
    NVME_LOG_ERROR = 0x01,          ///< 错误信息日志
    NVME_LOG_SMART = 0x02,          ///< SMART/健康信息日志
    NVME_LOG_FW_SLOT = 0x03,        ///< 固件插槽信息日志
    NVME_LOG_CHANGED_NS = 0x04,     ///< 已更改命名空间列表
    NVME_LOG_CMD_EFFECTS = 0x05,    ///< 命令效果日志
    NVME_LOG_DEVICE_SELF_TEST = 0x06,///< 设备自检日志
    NVME_LOG_TELEMETRY_HOST = 0x07, ///< 遥测主机发起日志
    NVME_LOG_TELEMETRY_CTRL = 0x08, ///< 遥测控制器发起日志
    NVME_LOG_ENDURANCE_GROUP = 0x09,///< Endurance Group信息日志
    NVME_LOG_PREDICTABLE_LATENCY = 0x0A, ///< 可预测延迟日志
    NVME_LOG_PERSISTENT_EVENT = 0x0D,    ///< 持久事件日志
    NVME_LOG_ANA = 0x0C,            ///< 非对称命名空间访问日志
    NVME_LOG_DISC_CHANGE = 0x70,    ///< 发现变更日志
    NVME_LOG_RESERVATION = 0x80     ///< 预留通知日志
} nvme_log_page_id_t;

/**
 * @brief NVMe SMART/Health Information Log Page（512字节）
 * @details 包含温度、可用空间、寿命、错误统计等健康信息
 */
typedef struct {
    uint8_t  critical_warning;    ///< 严重警告（位0=可用空间低，位1=温度过高，位2=可靠性降级，位3=只读，位4=易失性内存备份失败）
    uint16_t temperature;         ///< 当前温度（开尔文）
    uint8_t  avail_spare;         ///< 可用备用空间（百分比）
    uint8_t  spare_thresh;        ///< 可用备用阈值
    uint8_t  percent_used;        ///< 已使用百分比（寿命估算）
    uint8_t  endu_grp_crit_warn;  ///< Endurance Group严重警告摘要
    uint8_t  reserved1[25];
    uint8_t  data_units_read[16]; ///< 已读取数据单元数（每512KB为1单位，128位）
    uint8_t  data_units_written[16]; ///< 已写入数据单元数（128位）
    uint8_t  host_read_cmds[16];  ///< 主机读命令数（128位）
    uint8_t  host_write_cmds[16]; ///< 主机写命令数（128位）
    uint8_t  ctrl_busy_time[16];  ///< 控制器忙碌时间（分钟，128位）
    uint8_t  power_cycles[16];    ///< 上电循环次数（128位）
    uint8_t  power_on_hours[16];  ///< 上电时间（小时，128位）
    uint8_t  unsafe_shutdowns[16];///< 不安全关机次数（128位）
    uint8_t  media_errors[16];    ///< 媒体错误数（128位）
    uint8_t  num_err_log_entries[16]; ///< 错误日志条目数（128位）
    uint32_t warning_temp_time;   ///< 警告温度时间（分钟）
    uint32_t critical_temp_time;  ///< 临界温度时间（分钟）
    uint16_t temp_sensor[8];      ///< 温度传感器1-8读数（开尔文）
    uint32_t thm_temp1_trans_count; ///< 热管理温度1转换计数
    uint32_t thm_temp2_trans_count; ///< 热管理温度2转换计数
    uint32_t thm_temp1_total_time;   ///< 热管理温度1总时间
    uint32_t thm_temp2_total_time;   ///< 热管理温度2总时间
    uint8_t  reserved2[280];
} nvme_smart_log_t;

/* ============================================================
 *  NVMe Feature 定义
 * ============================================================ */

/**
 * @brief NVMe Feature ID
 */
typedef enum {
    NVME_FEAT_ARBITRATION = 0x01,      ///< 仲裁
    NVME_FEAT_POWER_MGMT = 0x02,       ///< 电源管理
    NVME_FEAT_LBA_RANGE = 0x03,        ///< LBA范围类型
    NVME_FEAT_TEMP_THRESHOLD = 0x04,   ///< 温度阈值
    NVME_FEAT_ERROR_RECOVERY = 0x05,    ///< 错误恢复
    NVME_FEAT_VOLATILE_WC = 0x06,       ///< 易失性写缓存
    NVME_FEAT_NUM_QUEUES = 0x07,        ///< 队列数
    NVME_FEAT_IRQ_COALESCE = 0x08,      ///< 中断聚合
    NVME_FEAT_IRQ_CONFIG = 0x09,        ///< 中断配置
    NVME_FEAT_WRITE_ATOMICITY = 0x0A,   ///< 写原子性
    NVME_FEAT_ASYNC_EVENT = 0x0B,       ///< 异步事件配置
    NVME_FEAT_AUTO_PST = 0x0C,          ///< 自动电源状态转换
    NVME_FEAT_HOST_MEM_BUF = 0x0D,      ///< 主机内存缓冲区
    NVME_FEAT_TIMESTAMP = 0x0E,         ///< 时间戳
    NVME_FEAT_KEEP_ALIVE = 0x0F,        ///< 保持活动
    NVME_FEAT_HCTM = 0x10,              ///< 主机控制热管理
    NVME_FEAT_NOPSC = 0x11,             ///< 非操作电源状态配置
    NVME_FEAT_RRL = 0x12,               ///< 可恢复重试延迟
    NVME_FEAT_PLM_CONFIG = 0x13,        ///< 预测延迟管理配置
    NVME_FEAT_PLM_WINDOW = 0x14,        ///< 预测延迟管理窗口
    NVME_FEAT_LBA_STS_INTERVAL = 0x15,  ///< LBA状态信息间隔
    NVME_FEAT_HOST_BEHAVIOR = 0x16,     ///< 主机行为支持
    NVME_FEAT_SANITIZE = 0x17,          ///< 消毒
    NVME_FEAT_ENDURANCE_EVT_CFG = 0x18, ///< Endurance Group事件配置
    NVME_FEAT_IO_GUARD = 0x19           ///< I/O保护
} nvme_feature_id_t;

/* ============================================================
 *  端到端数据保护（DIF/DIX）
 * ============================================================ */

/**
 * @brief DIF 保护信息类型
 */
typedef enum {
    DIF_TYPE_DISABLED = 0,    ///< 禁用DIF
    DIF_TYPE1 = 1,            ///< Type 1：参考标签=LBA，应用标签可配置
    DIF_TYPE2 = 2,            ///< Type 2：参考标签=0，应用标签可配置
    DIF_TYPE3 = 3             ///< Type 3：参考标签和应用标签均不检查
} dif_type_t;

/**
 * @brief DIF 保护信息结构（8字节，每个LBA附加）
 * @details 包含CRC校验、应用标签和参考标签，用于端到端数据完整性保护
 */
typedef struct {
    uint16_t crc;             ///< CRC-16校验值（T10 PI标准）
    uint16_t app_tag;         ///< 应用标签（Application Tag）
    uint32_t ref_tag;         ///< 参考标签（Reference Tag，通常为LBA号）
} dif_protection_t;

/**
 * @brief DIF 配置结构
 */
typedef struct {
    dif_type_t type;          ///< DIF类型
    bool guard_check_enable;  ///< CRC校验使能
    bool app_tag_check_enable;///< 应用标签校验使能
    bool ref_tag_check_enable;///< 参考标签校验使能
    uint16_t app_tag;         ///< 默认应用标签
    uint16_t app_tag_mask;    ///< 应用标签掩码（用于部分校验）
} dif_config_t;

/**
 * @brief DIF 错误统计
 */
typedef struct {
    uint64_t crc_errors;      ///< CRC校验错误数
    uint64_t app_tag_errors;  ///< 应用标签错误数
    uint64_t ref_tag_errors;  ///< 参考标签错误数
    uint64_t total_checks;    ///< 总校验次数
} dif_stats_t;

/* ============================================================
 *  主机接口配置
 * ============================================================ */

/**
 * @brief 主机接口配置结构体
 */
typedef struct {
    uint32_t queue_size;      ///< 命令队列大小
    uint32_t max_cmd;         ///< 最大并发命令数
    uint32_t lba_size;        ///< LBA 大小（字节）
    uint64_t total_lbas;      ///< 总 LBA 数量
    bool is_nvm;              ///< 是否为 NVM 命名空间
} host_if_config_t;

/* ============================================================
 *  主机接口统计信息
 * ============================================================ */

/**
 * @brief 主机接口统计结构体
 */
typedef struct {
    uint64_t total_cmds;        ///< 总命令数
    uint64_t read_cmds;         ///< 读命令数
    uint64_t write_cmds;        ///< 写命令数
    uint64_t trim_cmds;         ///< TRIM 命令数
    uint64_t completed_cmds;    ///< 已完成命令数
    uint64_t failed_cmds;       ///< 失败命令数
    uint64_t total_read_bytes;  ///< 总读取字节数
    uint64_t total_write_bytes; ///< 总写入字节数
    uint32_t avg_latency_us;    ///< 平均延迟（微秒）
} host_if_stats_t;

/**
 * @brief 性能统计结构体
 */
typedef struct {
    /* IOPS 统计 */
    uint64_t read_iops;         ///< 读 IOPS
    uint64_t write_iops;        ///< 写 IOPS
    uint64_t total_iops;        ///< 总 IOPS

    /* 带宽统计 */
    uint64_t read_bw_bps;       ///< 读带宽（字节/秒）
    uint64_t write_bw_bps;      ///< 写带宽（字节/秒）
    uint64_t total_bw_bps;      ///< 总带宽（字节/秒）

    /* 延迟统计 */
    uint64_t min_latency_us;    ///< 最小延迟（微秒）
    uint64_t max_latency_us;    ///< 最大延迟（微秒）
    uint64_t avg_latency_us;    ///< 平均延迟（微秒）
    uint64_t total_latency_us;  ///< 总延迟（微秒）
    uint64_t latency_count;     ///< 延迟统计次数

    /* 时间窗口统计 */
    uint64_t window_start_ms;   ///< 统计窗口开始时间
    uint64_t window_read_cmds;  ///< 窗口内读命令数
    uint64_t window_write_cmds; ///< 窗口内写命令数
    uint64_t window_read_bytes; ///< 窗口内读字节数
    uint64_t window_write_bytes;///< 窗口内写字节数
} performance_stats_t;

/* ============================================================
 *  主机接口
 * ============================================================ */

/**
 * @brief 初始化主机接口模块
 * @param[in] config 配置指针
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_INTERNAL 内部错误
 */
ret_code_t host_if_init(const host_if_config_t *config);

/**
 * @brief 反初始化主机接口模块
 * @retval RET_OK 成功
 */
ret_code_t host_if_deinit(void);

/**
 * @brief 提交 NVMe 命令
 * @param[in] cmd 命令指针
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NO_SPACE 队列已满
 */
ret_code_t host_if_submit_cmd(const nvme_cmd_t *cmd);

/**
 * @brief 轮询完成队列
 * @param[out] cqe 完成队列条目指针
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_BUSY 队列为空
 */
ret_code_t host_if_poll_cq(nvme_cqe_t *cqe);

/**
 * @brief 主机接口主循环处理
 * @details 处理接收到的命令，转发给 FTL 模块
 * @retval RET_OK 成功
 * @retval RET_ERR_NOT_INIT 未初始化
 */
ret_code_t host_if_process(void);

/**
 * @brief 获取主机接口统计信息
 * @param[out] stats 统计信息指针
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t host_if_get_stats(host_if_stats_t *stats);

/**
 * @brief 重置主机接口统计信息
 * @retval RET_OK 成功
 */
ret_code_t host_if_reset_stats(void);

/**
 * @brief 打印主机接口统计信息
 */
void host_if_print_stats(void);

/* ============================================================
 *  性能监控接口
 * ============================================================ */

/**
 * @brief 获取性能统计信息
 * @param[out] stats 性能统计信息指针
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_INIT 未初始化
 * @note 性能统计包括 IOPS、带宽、延迟等指标
 */
ret_code_t host_if_get_performance_stats(performance_stats_t *stats);

/**
 * @brief 重置性能统计信息
 * @retval RET_OK 成功
 * @retval RET_ERR_NOT_INIT 未初始化
 */
ret_code_t host_if_reset_performance_stats(void);

/**
 * @brief 打印性能统计信息
 */
void host_if_print_performance_stats(void);

/* ============================================================
 *  NVMe Admin 命令对外接口
 * ============================================================ */

/**
 * @brief 获取 Identify Controller 数据
 * @param[out] id_ctrl 输出缓冲区（大小 >= sizeof(nvme_id_ctrl_t)）
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_INIT 未初始化
 */
ret_code_t host_if_get_id_ctrl(nvme_id_ctrl_t *id_ctrl);

/**
 * @brief 获取 Identify Namespace 数据
 * @param[out] id_ns 输出缓冲区（大小 >= sizeof(nvme_id_ns_t)）
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_INIT 未初始化
 */
ret_code_t host_if_get_id_ns(nvme_id_ns_t *id_ns);

/**
 * @brief 获取 SMART/健康日志
 * @param[out] smart_log 输出缓冲区（大小 >= sizeof(nvme_smart_log_t)）
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_INIT 未初始化
 */
ret_code_t host_if_get_smart_log(nvme_smart_log_t *smart_log);

/**
 * @brief 打印 SMART/健康信息
 * @details 以可读格式打印温度、寿命、错误统计等健康信息
 */
void host_if_print_smart_info(void);

/* ============================================================
 *  断电保护（PLP）接口
 * ============================================================ */

/**
 * @brief 模拟断电事件
 * @details 模拟突然断电，触发WAL日志保护机制
 * @retval RET_OK 成功
 * @retval RET_ERR_NOT_INIT 未初始化
 */
ret_code_t host_if_plp_simulate_power_loss(void);

/**
 * @brief 执行断电恢复
 * @details 从WAL日志中恢复未完成的写入，确保数据一致性
 * @retval RET_OK 成功
 * @retval RET_ERR_NOT_INIT 未初始化
 */
ret_code_t host_if_plp_recovery(void);

/**
 * @brief 获取电源状态信息
 * @param[out] power_cycles 上电循环次数（可为NULL）
 * @param[out] unsafe_shutdowns 不安全关机次数（可为NULL）
 * @param[out] power_on_hours 上电时间（小时，可为NULL）
 * @retval RET_OK 成功
 * @retval RET_ERR_NOT_INIT 未初始化
 */
ret_code_t host_if_get_power_status(uint64_t *power_cycles,
                                     uint64_t *unsafe_shutdowns,
                                     uint64_t *power_on_hours);

/* ============================================================
 *  端到端数据保护（DIF/DIX）接口
 * ============================================================ */

/**
 * @brief 初始化DIF保护模块
 * @param[in] config DIF配置指针
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t dif_init(const dif_config_t *config);

/**
 * @brief 为数据生成DIF保护信息
 * @param[in] data 数据缓冲区
 * @param[in] data_len 数据长度（必须是LBA大小的整数倍）
 * @param[in] lba 起始LBA号（用于参考标签）
 * @param[out] protection 输出的保护信息数组（大小 = data_len / lba_size）
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 * @retval RET_ERR_NOT_INIT 未初始化
 */
ret_code_t dif_generate(const uint8_t *data, uint32_t data_len,
                        uint64_t lba, dif_protection_t *protection);

/**
 * @brief 校验数据的DIF保护信息
 * @param[in] data 数据缓冲区
 * @param[in] data_len 数据长度
 * @param[in] lba 起始LBA号
 * @param[in] protection 保护信息数组
 * @param[out] error_lba 出错的LBA号（可为NULL）
 * @retval RET_OK 校验通过
 * @retval RET_ERR_DIF_CRC CRC校验失败
 * @retval RET_ERR_DIF_APP_TAG 应用标签校验失败
 * @retval RET_ERR_DIF_REF_TAG 参考标签校验失败
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t dif_verify(const uint8_t *data, uint32_t data_len,
                      uint64_t lba, const dif_protection_t *protection,
                      uint64_t *error_lba);

/**
 * @brief 获取DIF配置
 * @param[out] config 输出配置
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t dif_get_config(dif_config_t *config);

/**
 * @brief 获取DIF错误统计
 * @param[out] stats 输出统计
 * @retval RET_OK 成功
 * @retval RET_ERR_PARAM 参数错误
 */
ret_code_t dif_get_stats(dif_stats_t *stats);

/**
 * @brief 重置DIF错误统计
 * @retval RET_OK 成功
 */
ret_code_t dif_reset_stats(void);

/**
 * @brief 计算CRC-16（T10 PI标准，多项式0x8BB7）
 * @param[in] data 数据缓冲区
 * @param[in] len 数据长度
 * @return CRC-16校验值
 */
uint16_t dif_crc16(const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* FIRMWARE_HOST_IF_H */
