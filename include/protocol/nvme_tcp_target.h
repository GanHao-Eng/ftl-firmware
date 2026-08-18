/**
 * @file nvme_tcp_target.h
 * @brief NVMe/TCP 目标端接口（NVMe/TCP 2.0 协议格式）
 * @details NVMe over Fabrics (NVMe/TCP) 目标端实现，使固件能够通过
 *          TCP 网络提供 NVMe 服务。Linux 内核 7.0+ 的 nvme-tcp 驱动
 *          使用 NVMe/TCP 2.0 协议格式。
 *
 *          数据链路：
 *          Linux(nvme-tcp) → TCP网络 → NVMe/TCP目标端 → NVMe控制器 → FTL → NAND
 *
 *          NVMe/TCP 2.0 PDU 类型：
 *          - 0x00 ICReq / 0x01 ICResp: 连接初始化握手
 *          - 0x02 H2CTermReq / 0x03 C2HTermReq: 连接终止
 *          - 0x04 CapsuleCmd / 0x05 CapsuleResp: 命令封装和完成
 *          - 0x06 H2CData / 0x07 C2HData: 数据传输
 *          - 0x09 R2T: 准备接收数据
 *
 *          NVMe/TCP 2.0 公共头部（8字节）：
 *          - pdu_type(1) + flags(1) + hlen(1) + pdo(1) + plen(4)
 *          注意：与 1.0 不同，hlen 是 uint8_t，plen 是 uint32_t（总长度）
 */

#ifndef NVME_TCP_TARGET_H
#define NVME_TCP_TARGET_H

#include "common/common.h"
#include "protocol/nvme_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  NVMe/TCP 常量
 * ============================================================ */

#define NVME_TCP_DEFAULT_PORT     4420    ///< NVMe/TCP 默认端口
#define NVME_TCP_MAX_CONNECTIONS  4       ///< 最大并发连接数
#define NVME_TCP_BUFFER_SIZE      65536   ///< 接收缓冲区大小
#define NVME_TCP_PDU_HDR_SIZE     8       ///< 公共头部大小
#define NVME_TCP_IC_PDU_SIZE      128     ///< ICReq/ICResp 总大小
#define NVME_TCP_CMD_HDR_SIZE     72      ///< CapsuleCmd头部大小(8+64)
#define NVME_TCP_RSP_HDR_SIZE     24      ///< CapsuleResp头部大小(8+16)
#define NVME_TCP_DATA_HDR_SIZE    24      ///< H2CData/C2HData/R2T头部大小
#define NVME_TCP_DEFAULT_MAXH2CDATA 8192  ///< 默认最大H2C数据突发
#define NVME_TCP_DEFAULT_MAXR2T    1      ///< 默认最大R2T数

/* ============================================================
 *  NVMe/TCP 2.0 PDU 类型
 * ============================================================ */

typedef enum {
    NVME_TCP_PDU_ICREQ          = 0x00,  ///< 初始化连接请求
    NVME_TCP_PDU_ICRESP         = 0x01,  ///< 初始化连接响应
    NVME_TCP_PDU_H2C_TERM_REQ   = 0x02,  ///< 主机到控制器终止请求
    NVME_TCP_PDU_C2H_TERM_REQ   = 0x03,  ///< 控制器到主机终止请求
    NVME_TCP_PDU_CAPSULE_CMD    = 0x04,  ///< 封装命令
    NVME_TCP_PDU_CAPSULE_RESP   = 0x05,  ///< 封装响应
    NVME_TCP_PDU_H2CDATA        = 0x06,  ///< 主机到控制器数据
    NVME_TCP_PDU_C2HDATA        = 0x07,  ///< 控制器到主机数据
    NVME_TCP_PDU_R2T            = 0x09,  ///< 准备接收
    NVME_TCP_PDU_MAX
} nvme_tcp_pdu_type_t;

/* ============================================================
 *  NVMe/TCP 2.0 公共头部 (8字节)
 * ============================================================ */

typedef struct {
    uint8_t  pdu_type;    ///< PDU 类型
    uint8_t  flags;       ///< PDU 类型特定标志
    uint8_t  hlen;        ///< PDU 头部长度（不含 Header Digest）
    uint8_t  pdo;         ///< PDU 数据偏移（从PDU开头到数据段的字节数）
    uint32_t plen;        ///< PDU 总长度（含头部，小端）
} __attribute__((packed)) nvme_tcp_pdu_hdr_t;

/* 公共头部标志 */
#define NVME_TCP_CH_FLAGS_HDGSTF   (1u << 0)  ///< 头部摘要使能
#define NVME_TCP_CH_FLAGS_DDGSTF   (1u << 1)  ///< 数据摘要使能

/* H2CData/C2HData 标志 */
#define NVME_TCP_DATA_FLAGS_LAST_PDU  (1u << 2)  ///< 最后一个数据PDU
#define NVME_TCP_DATA_FLAGS_SUCCESS   (1u << 3)  ///< 成功

/* NVMe/TCP 协议格式版本 */
#define NVME_TCP_PFV_1_0    0x0001  ///< NVMe/TCP 1.0
#define NVME_TCP_PFV_2_0    0x0002  ///< NVMe/TCP 2.0

/* ============================================================
 *  ICReq PDU (初始化连接请求, 128字节)
 * ============================================================ */

typedef struct {
    nvme_tcp_pdu_hdr_t hdr;        ///< 公共头部 (8字节)
    uint16_t pfv;                   ///< 协议格式版本
    uint8_t  hpda;                  ///< 主机PDU数据对齐（4字节单位，0=无对齐）
    uint8_t  dgst;                  ///< 摘要使能 (bit0=hdgst, bit1=ddgst)
    uint32_t maxr2t;                ///< 最大R2T数
    uint8_t  rsvd[112];             ///< 保留（总PDU长度=128字节）
} __attribute__((packed)) nvme_tcp_icreq_t;

/* ============================================================
 *  ICResp PDU (初始化连接响应, 128字节)
 * ============================================================ */

typedef struct {
    nvme_tcp_pdu_hdr_t hdr;        ///< 公共头部 (8字节)
    uint16_t pfv;                   ///< 协议格式版本
    uint8_t  cpda;                  ///< 控制器PDU数据对齐
    uint8_t  dgst;                  ///< 摘要使能 (bit0=hdgst, bit1=ddgst)
    uint32_t maxh2cdata;            ///< 最大H2C数据突发（字节）
    uint8_t  rsvd[112];             ///< 保留（总PDU长度=128字节）
} __attribute__((packed)) nvme_tcp_icresp_t;

/* ============================================================
 *  CapsuleCmd PDU (封装命令, 头部72字节 + inline data)
 * ============================================================ */

typedef struct {
    nvme_tcp_pdu_hdr_t hdr;        ///< 公共头部 (8字节)
    nvme_command_t ccsqe;           ///< NVMe 命令 (64字节)
    /* 后面可跟随 inline data（从 hdr.pdo 偏移开始） */
} __attribute__((packed)) nvme_tcp_capsule_cmd_hdr_t;

/* ============================================================
 *  CapsuleResp PDU (封装响应, 24字节)
 *  Linux内核使用标准 nvme_completion 结构:
 *  hdr(8) + result(4) + rsvd(4) + sq_head(2) + sq_id(2) + command_id(2) + status(2)
 * ============================================================ */

typedef struct {
    nvme_tcp_pdu_hdr_t hdr;        ///< 公共头部 (8字节)
    nvme_completion_t cqe;         ///< NVMe 完成队列项 (16字节)
} __attribute__((packed)) nvme_tcp_capsule_resp_hdr_t;

/* ============================================================
 *  H2CData PDU (主机到控制器数据, 头部24字节 + 数据)
 * ============================================================ */

typedef struct {
    nvme_tcp_pdu_hdr_t hdr;        ///< 公共头部 (8字节)
    uint16_t cccid;                 ///< 命令ID (Command Capsule Command ID)
    uint16_t ttag;                  ///< 传输标签 (Transfer Tag)
    uint32_t datao;                 ///< 数据偏移 (Data Offset)
    uint32_t datal;                 ///< 数据长度 (Data Length)
    uint8_t  rsvd[4];               ///< 保留
    /* 后面跟随数据（从 hdr.pdo 偏移开始） */
} __attribute__((packed)) nvme_tcp_h2cdata_hdr_t;

/* ============================================================
 *  C2HData PDU (控制器到主机数据, 头部24字节 + 数据)
 * ============================================================ */

typedef struct {
    nvme_tcp_pdu_hdr_t hdr;        ///< 公共头部 (8字节)
    uint16_t cccid;                 ///< 命令ID
    uint8_t  rsvd1[2];              ///< 保留
    uint32_t datao;                 ///< 数据偏移
    uint32_t datal;                 ///< 数据长度
    uint8_t  rsvd2[4];              ///< 保留
    /* 后面跟随数据（从 hdr.pdo 偏移开始） */
} __attribute__((packed)) nvme_tcp_c2hdata_hdr_t;

/* ============================================================
 *  R2T PDU (准备接收, 24字节)
 * ============================================================ */

typedef struct {
    nvme_tcp_pdu_hdr_t hdr;        ///< 公共头部 (8字节)
    uint16_t cccid;                 ///< 命令ID
    uint16_t ttag;                  ///< 传输标签
    uint32_t r2to;                  ///< R2T 偏移
    uint32_t r2tl;                  ///< R2T 长度
    uint8_t  rsvd[4];               ///< 保留
} __attribute__((packed)) nvme_tcp_r2t_t;

/* ============================================================
 *  TermReq PDU (终止请求)
 * ============================================================ */

typedef struct {
    nvme_tcp_pdu_hdr_t hdr;        ///< 公共头部 (8字节)
    uint16_t fes;                   ///< 致命错误状态 (Fatal Error Status)
    uint8_t  fei[4];                ///< 错误信息 (Fatal Error Information)
    uint8_t  rsvd[10];              ///< 保留
} __attribute__((packed)) nvme_tcp_term_req_hdr_t;

/* 终止错误状态 */
#define NVME_TCP_TERM_FES_INVALID_HDR     0x01  ///< 无效PDU头部字段
#define NVME_TCP_TERM_FES_PDU_SEQ_ERR     0x02  ///< PDU序列错误
#define NVME_TCP_TERM_FES_HDGST_ERR       0x03  ///< 头部摘要错误
#define NVME_TCP_TERM_FES_DATA_OUT_RANGE  0x04  ///< 数据传输越界
#define NVME_TCP_TERM_FES_DATA_LIMIT_EXC  0x05  ///< 数据传输超限
#define NVME_TCP_TERM_FES_R2T_LIMIT_EXC   0x05  ///< R2T超限
#define NVME_TCP_TERM_FES_INVALID_DATA    0x06  ///< 无效数据/不支持参数

/* ============================================================
 *  连接状态
 * ============================================================ */

typedef enum {
    NVME_TCP_CONN_NEW = 0,      ///< 新连接（等待ICReq）
    NVME_TCP_CONN_READY,        ///< 连接就绪（IC完成）
    NVME_TCP_CONN_ACTIVE,       ///< 连接活跃（正在处理命令）
    NVME_TCP_CONN_TERMINATING,  ///< 连接终止中
    NVME_TCP_CONN_CLOSED        ///< 连接已关闭
} nvme_tcp_conn_state_t;

/* ============================================================
 *  连接上下文
 * ============================================================ */

typedef struct {
    int sockfd;                         ///< Socket 文件描述符
    nvme_tcp_conn_state_t state;        ///< 连接状态
    uint32_t maxh2cdata;                ///< 最大H2C数据突发（字节）
    uint32_t maxr2t;                    ///< 最大R2T数
    bool hdgst_enable;                  ///< 头部摘要使能
    bool ddgst_enable;                  ///< 数据摘要使能
    uint8_t  cpda;                      ///< 控制器PDU数据对齐
    uint8_t  hpda;                      ///< 主机PDU数据对齐
    uint8_t *recv_buffer;               ///< 接收缓冲区
    uint32_t recv_len;                  ///< 接收数据长度
    uint8_t *data_buffer;               ///< 数据缓冲区（写命令数据）
    uint32_t data_len;                  ///< 已接收数据长度
    uint32_t data_total;                ///< 期望数据总长度
    uint16_t pending_cmd_id;            ///< 待完成命令ID
    uint16_t pending_sqid;              ///< 待完成SQ ID
    uint64_t pending_slba;              ///< 待完成写命令的起始LBA
    uint8_t  pending_opcode;            ///< 待完成命令操作码 (Write=0x01, DatasetMgmt=0x09)
    uint16_t qid;                       ///< 该连接对应的队列ID (0=Admin, 1+=I/O)
    uint64_t last_activity_ms;          ///< 最后活动时间
} nvme_tcp_conn_t;

/* ============================================================
 *  目标端配置
 * ============================================================ */

typedef struct {
    uint16_t port;                      ///< 监听端口
    const char *subnqn;                 ///< 子系统NQN
    uint32_t maxh2cdata;                ///< 最大H2C数据突发
} nvme_tcp_target_config_t;

/* ============================================================
 *  函数接口
 * ============================================================ */

/**
 * @brief 初始化 NVMe/TCP 目标端
 * @param config 目标端配置
 * @retval RET_OK 成功
 * @retval RET_ERR_* 失败
 */
ret_code_t nvme_tcp_target_init(const nvme_tcp_target_config_t *config);

/**
 * @brief 反初始化 NVMe/TCP 目标端
 */
ret_code_t nvme_tcp_target_deinit(void);

/**
 * @brief 处理 NVMe/TCP 事件（非阻塞，应在主循环中调用）
 */
void nvme_tcp_target_process(void);

/**
 * @brief 处理连接上的 PDU
 * @param conn 连接上下文
 * @retval RET_OK 成功
 * @retval RET_ERR_INTERNAL 连接错误，应关闭
 */
ret_code_t nvme_tcp_process_pdu(nvme_tcp_conn_t *conn);

/**
 * @brief 处理 ICReq
 */
ret_code_t nvme_tcp_handle_icreq(nvme_tcp_conn_t *conn,
                                 const nvme_tcp_icreq_t *pdu);

/**
 * @brief 处理 CapsuleCmd
 */
ret_code_t nvme_tcp_handle_capsule_cmd(nvme_tcp_conn_t *conn,
                                       const nvme_tcp_capsule_cmd_hdr_t *pdu,
                                       const uint8_t *data,
                                       uint32_t data_len);

/**
 * @brief 处理 H2CData
 */
ret_code_t nvme_tcp_handle_h2cdata(nvme_tcp_conn_t *conn,
                                   const nvme_tcp_h2cdata_hdr_t *pdu,
                                   const uint8_t *data,
                                   uint32_t data_len);

/**
 * @brief 发送 CapsuleResp
 */
ret_code_t nvme_tcp_send_capsule_resp(nvme_tcp_conn_t *conn,
                                      const nvme_completion_t *cpl);

/**
 * @brief 发送 C2HData
 */
ret_code_t nvme_tcp_send_c2hdata(nvme_tcp_conn_t *conn,
                                 uint16_t cccid,
                                 const uint8_t *data,
                                 uint32_t data_len,
                                 uint32_t data_offset,
                                 bool last_pdu);

/**
 * @brief 发送 R2T
 */
ret_code_t nvme_tcp_send_r2t(nvme_tcp_conn_t *conn,
                             uint16_t cccid,
                             uint16_t ttag,
                             uint32_t offset,
                             uint32_t length);

#ifdef __cplusplus
}
#endif

#endif /* NVME_TCP_TARGET_H */
