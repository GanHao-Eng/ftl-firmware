/**
 * @file nvme_tcp_target.c
 * @brief NVMe/TCP 目标端实现（NVMe/TCP 2.0 协议格式）
 * @details NVMe over Fabrics (NVMe/TCP) 目标端完整实现。
 *          适配 Linux 内核 7.0+ 的 nvme-tcp 驱动（NVMe/TCP 2.0）。
 *
 *          NVMe/TCP 2.0 与 1.0 的主要区别：
 *          1. 公共头部：pdu_type(1)+flags(1)+hlen(1)+pdo(1)+plen(4)
 *             （1.0 是 type(1)+flags(1)+hlen(2)+pdo(1)+pl(1)+rsvd(2)）
 *          2. PDU 类型重新编号：CapsuleCmd=0x04, H2CData=0x06 等
 *          3. ICReq/ICResp 字段变化：hpda/cpda, dgst, maxr2t/maxh2cdata
 *          4. CapsuleCmd 无特定头部：公共头(8)+NVMe命令(64)+inline data
 *          5. H2CData/C2HData/R2T 头部扩展为 24 字节
 *
 *          工作流程：
 *          1. 目标端监听 TCP 端口 4420
 *          2. 主机发起 TCP 连接
 *          3. ICReq/ICResp 握手
 *          4. 主机发送 CapsuleCmd（NVMe命令 + 可选 inline data）
 *          5. 目标端处理命令：
 *             - 读命令：发送 C2HData + CapsuleResp
 *             - 写命令：发送 R2T → 接收 H2CData → CapsuleResp
 *          6. 主机断开连接
 */

#define _GNU_SOURCE

#include "protocol/nvme_tcp_target.h"
#include "protocol/nvme_controller.h"
#include "ftl.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <fcntl.h>
#include <time.h>

/* ============================================================
 *  内部数据
 * ============================================================ */

static nvme_tcp_target_config_t g_config;
static nvme_tcp_conn_t g_connections[NVME_TCP_MAX_CONNECTIONS];
static int g_listen_fd = -1;
static bool g_running = false;

/* 预分配的I/O缓冲区，避免频繁malloc/free */
#define NVME_TCP_MAX_IO_SIZE  (1024 * 1024)  /* 最大1MB */
static uint8_t g_io_read_buf[NVME_TCP_MAX_IO_SIZE];
static uint8_t g_io_write_buf[NVME_TCP_MAX_IO_SIZE];
static uint8_t g_zero_buf[4096];  /* 静态零缓冲区，用于Write Zeroes */

/* 默认子系统 NQN */
static const char *DEFAULT_SUBNQN = "nqn.2026-08.io.ftlfw:subsystem";

/* ============================================================
 *  内部辅助函数
 * ============================================================ */

static uint64_t get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* NVMe/TCP 协议使用小端字节序，x86 也是小端，直接使用无需转换 */
static uint16_t le16_to_cpu(uint16_t val) { return val; }
static uint16_t cpu_to_le16(uint16_t val) { return val; }
static uint32_t le32_to_cpu(uint32_t val) { return val; }
static uint32_t cpu_to_le32(uint32_t val) { return val; }
static uint64_t le64_to_cpu(uint64_t val) { return val; }

/**
 * @brief 向 socket 发送指定长度的数据
 */
static int send_all(int sockfd, const void *buf, size_t len)
{
    size_t total = 0;
    ssize_t n = 0;

    while (total < len) {
        n = send(sockfd, (const uint8_t *)buf + total, len - total, MSG_NOSIGNAL);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
        total += n;
    }
    return (int)total;
}

/**
 * @brief 查找空闲连接槽位
 */
static nvme_tcp_conn_t *find_free_conn(void)
{
    uint32_t i = 0;
    for (i = 0; i < NVME_TCP_MAX_CONNECTIONS; i++) {
        if (g_connections[i].state == NVME_TCP_CONN_CLOSED) {
            return &g_connections[i];
        }
    }
    return NULL;
}

/**
 * @brief 关闭连接
 */
static void close_connection(nvme_tcp_conn_t *conn)
{
    if (conn == NULL) return;

    if (conn->sockfd >= 0) {
        close(conn->sockfd);
        conn->sockfd = -1;
    }
    if (conn->recv_buffer) {
        free(conn->recv_buffer);
        conn->recv_buffer = NULL;
    }
    if (conn->data_buffer) {
        free(conn->data_buffer);
        conn->data_buffer = NULL;
    }

    conn->state = NVME_TCP_CONN_CLOSED;
    conn->recv_len = 0;
    conn->data_len = 0;
    conn->data_total = 0;

    LOG_INFO("NVMe/TCP: 连接已关闭");
}

/* ============================================================
 *  ICReq 处理
 * ============================================================ */

/**
 * @brief 处理 NVMe/TCP 初始化请求 (ICReq, PDU type=0x00)
 *
 * 这是 NVMe/TCP 连接建立后的第一个 PDU，用于协商协议版本、
 * 头摘要/数据摘要、最大主机到控制器数据长度等参数。
 *
 * @param conn 连接上下文
 * @param pdu  ICReq PDU 头部
 * @return RET_OK 成功，其他失败
 *
 * @note 本实现仅支持 pfv=0 (NVMe/TCP 1.0)，不支持摘要(dgst=0)
 */
ret_code_t nvme_tcp_handle_icreq(nvme_tcp_conn_t *conn, const nvme_tcp_icreq_t *pdu)
{
    nvme_tcp_icresp_t resp;
    uint16_t pfv = le16_to_cpu(pdu->pfv);
    uint32_t maxr2t = le32_to_cpu(pdu->maxr2t);

    LOG_INFO("NVMe/TCP: 收到 ICReq, pfv=0x%04X, hpda=%u, dgst=0x%02X, maxr2t=%u",
             pfv, pdu->hpda, pdu->dgst, maxr2t);

    /* 保存协商参数 */
    conn->hpda = pdu->hpda;
    conn->hdgst_enable = (pdu->dgst & 0x01) ? true : false;
    conn->ddgst_enable = (pdu->dgst & 0x02) ? true : false;
    conn->maxr2t = (maxr2t > 0) ? maxr2t : NVME_TCP_DEFAULT_MAXR2T;
    conn->maxh2cdata = NVME_TCP_DEFAULT_MAXH2CDATA;
    conn->cpda = 0;  /* 控制器数据对齐：无 */

    /* 构造 ICResp */
    memset(&resp, 0, sizeof(resp));
    resp.hdr.pdu_type = NVME_TCP_PDU_ICRESP;
    resp.hdr.flags = 0;
    resp.hdr.hlen = sizeof(nvme_tcp_icresp_t);  /* 128 */
    resp.hdr.pdo = sizeof(nvme_tcp_icresp_t);   /* 无数据段，pdo=hlen */
    resp.hdr.plen = cpu_to_le32(sizeof(nvme_tcp_icresp_t));
    resp.pfv = cpu_to_le16(pfv);  /* 回显主机的协议版本 */
    resp.cpda = conn->cpda;
    resp.dgst = 0;  /* 不支持摘要 */
    resp.maxh2cdata = cpu_to_le32(conn->maxh2cdata);

    LOG_INFO("NVMe/TCP: 发送 ICResp, sizeof=%u, pfv=0x%04X, maxh2cdata=%u",
             sizeof(resp), pfv, conn->maxh2cdata);

    if (send_all(conn->sockfd, &resp, sizeof(resp)) != (int)sizeof(resp)) {
        LOG_ERROR("NVMe/TCP: 发送 ICResp 失败, errno=%d", errno);
        return RET_ERR_INTERNAL;
    }

    conn->state = NVME_TCP_CONN_READY;
    LOG_INFO("NVMe/TCP: 连接初始化完成，进入就绪状态");

    return RET_OK;
}

/* ============================================================
 *  CapsuleResp 发送
 * ============================================================ */

/**
 * @brief 发送 CapsuleResp PDU (PDU type=0x05)
 *
 * CapsuleResp 用于返回命令完成状态，结构为：
 *   公共头部(8B) + NVMe完成队列条目(16B) = 24B
 *
 * @param conn 连接上下文
 * @param cpl  NVMe 完成队列条目（含 cid, sqid, status 等）
 * @return RET_OK 成功
 *
 * @warning NVMe/TCP 传输时 **禁止** 设置 phase bit (status bit15)，
 *          否则主机将报 "Connect command failed ret=16384"
 */
ret_code_t nvme_tcp_send_capsule_resp(nvme_tcp_conn_t *conn,
                                      const nvme_completion_t *cpl)
{
    nvme_tcp_capsule_resp_hdr_t resp;

    memset(&resp, 0, sizeof(resp));
    resp.hdr.pdu_type = NVME_TCP_PDU_CAPSULE_RESP;
    resp.hdr.flags = 0;
    resp.hdr.hlen = sizeof(nvme_tcp_capsule_resp_hdr_t);  /* 24 */
    resp.hdr.pdo = sizeof(nvme_tcp_capsule_resp_hdr_t);   /* 无数据段 */
    resp.hdr.plen = cpu_to_le32(sizeof(nvme_tcp_capsule_resp_hdr_t));

    /* 复制 completion 条目 (NVMe/TCP传输时不设置phase bit) */
    memcpy(&resp.cqe, cpl, sizeof(nvme_completion_t));
    /* 清除 phase bit (bit15), 网络传输时主机期望phase=0 */
    resp.cqe.status &= cpu_to_le16(0x7FFF);

    LOG_INFO("NVMe/TCP: 发送 CapsuleResp, cid=%u, status=0x%04X, dw0=0x%08X",
             le16_to_cpu(resp.cqe.cid), le16_to_cpu(resp.cqe.status),
             le32_to_cpu(resp.cqe.dw0));

    if (send_all(conn->sockfd, &resp, sizeof(resp)) != (int)sizeof(resp)) {
        LOG_ERROR("NVMe/TCP: 发送 CapsuleResp 失败, errno=%d", errno);
        return RET_ERR_INTERNAL;
    }

    return RET_OK;
}

/* ============================================================
 *  C2HData 发送
 * ============================================================ */

/**
 * @brief 发送 C2HData PDU (PDU type=0x07)
 *
 * 控制器到主机数据传输，用于 Read 命令、Identify、Get Log Page 等
 * 需要返回数据的场景。支持分块传输（通过 datao 偏移标记）。
 *
 * @param conn    连接上下文
 * @param cccid   关联的命令 ID (Command Command ID)
 * @param data    数据缓冲区
 * @param data_len 本次传输的数据长度
 * @param offset  数据在整体传输中的偏移 (datao)
 * @param last    是否为最后一个数据 PDU (设置 LAST_PDU 标志)
 * @return RET_OK 成功
 */
ret_code_t nvme_tcp_send_c2hdata(nvme_tcp_conn_t *conn,
                                 uint16_t cccid,
                                 const uint8_t *data,
                                 uint32_t data_len,
                                 uint32_t data_offset,
                                 bool last_pdu)
{
    nvme_tcp_c2hdata_hdr_t hdr;
    uint8_t *buf;
    uint32_t total_len;
    int ret;

    memset(&hdr, 0, sizeof(hdr));
    hdr.hdr.pdu_type = NVME_TCP_PDU_C2HDATA;
    hdr.hdr.flags = last_pdu ? NVME_TCP_DATA_FLAGS_LAST_PDU : 0;
    hdr.hdr.hlen = sizeof(nvme_tcp_c2hdata_hdr_t);  /* 24 */
    hdr.hdr.pdo = sizeof(nvme_tcp_c2hdata_hdr_t);   /* 数据紧跟头部 */
    hdr.hdr.plen = cpu_to_le32(sizeof(hdr) + data_len);
    hdr.cccid = cpu_to_le16(cccid);
    hdr.datao = cpu_to_le32(data_offset);
    hdr.datal = cpu_to_le32(data_len);

    total_len = sizeof(hdr) + data_len;
    buf = (uint8_t *)malloc(total_len);
    if (buf == NULL) {
        LOG_ERROR("NVMe/TCP: 分配 C2HData 缓冲区失败");
        return RET_ERR_NO_SPACE;
    }

    memcpy(buf, &hdr, sizeof(hdr));
    if (data && data_len > 0) {
        memcpy(buf + sizeof(hdr), data, data_len);
    }

    ret = send_all(conn->sockfd, buf, total_len);
    free(buf);

    if (ret != (int)total_len) {
        LOG_ERROR("NVMe/TCP: 发送 C2HData 失败, errno=%d", errno);
        return RET_ERR_INTERNAL;
    }

    LOG_INFO("NVMe/TCP: 发送 C2HData, cccid=%u, datao=%u, datal=%u, total=%u, last=%d",
             cccid, data_offset, data_len, total_len, last_pdu);

    /* 调试：打印头部和数据关键字节 */
    {
        uint32_t i = 0;
        char hexbuf[512];
        int pos = 0;
        LOG_INFO("NVMe/TCP: C2HData 头部: type=0x%02X flags=0x%02X hlen=%u pdo=%u plen=%u",
                 hdr.hdr.pdu_type, hdr.hdr.flags, hdr.hdr.hlen, hdr.hdr.pdo,
                 le32_to_cpu(hdr.hdr.plen));
        LOG_INFO("NVMe/TCP: C2HData 字段: cccid=%u datao=%u datal=%u",
                 le16_to_cpu(hdr.cccid), le32_to_cpu(hdr.datao), le32_to_cpu(hdr.datal));
        for (i = 0; i < 64 && i < data_len; i++) {
            pos += snprintf(hexbuf + pos, sizeof(hexbuf) - pos, "%02X ", data[i]);
        }
        LOG_INFO("NVMe/TCP: Identify数据前64字节: %s", hexbuf);
        if (data_len >= 770) {
            LOG_INFO("NVMe/TCP: CNTLID@78=0x%02X%02X SUBNQN@768=%.40s",
                     data[78], data[79], data + 768);
        }
    }

    return RET_OK;
}

/* ============================================================
 *  R2T 发送
 * ============================================================ */

/**
 * @brief 发送 R2T PDU (Ready To Receive, PDU type=0x09)
 *
 * 请求主机发送写数据。当 Write 命令的数据未通过 inline data
 * 完全携带时，控制器发送 R2T 请求主机通过 H2CData PDU 发送数据。
 *
 * @param conn   连接上下文
 * @param cccid  关联的命令 ID
 * @param ttag   传输标签 (Transfer Tag)
 * @param offset 数据偏移
 * @param length 请求的数据长度
 * @return RET_OK 成功
 */
ret_code_t nvme_tcp_send_r2t(nvme_tcp_conn_t *conn,
                             uint16_t cccid,
                             uint16_t ttag,
                             uint32_t offset,
                             uint32_t length)
{
    nvme_tcp_r2t_t r2t;

    memset(&r2t, 0, sizeof(r2t));
    r2t.hdr.pdu_type = NVME_TCP_PDU_R2T;
    r2t.hdr.flags = 0;
    r2t.hdr.hlen = sizeof(nvme_tcp_r2t_t);  /* 24 */
    r2t.hdr.pdo = sizeof(nvme_tcp_r2t_t);   /* 无数据段 */
    r2t.hdr.plen = cpu_to_le32(sizeof(nvme_tcp_r2t_t));
    r2t.cccid = cpu_to_le16(cccid);
    r2t.ttag = cpu_to_le16(ttag);
    r2t.r2to = cpu_to_le32(offset);
    r2t.r2tl = cpu_to_le32(length);

    LOG_INFO("NVMe/TCP: 发送 R2T, cccid=%u, offset=%u, length=%u",
             cccid, offset, length);

    if (send_all(conn->sockfd, &r2t, sizeof(r2t)) != (int)sizeof(r2t)) {
        LOG_ERROR("NVMe/TCP: 发送 R2T 失败, errno=%d", errno);
        return RET_ERR_INTERNAL;
    }

    return RET_OK;
}

/* ============================================================
 *  CapsuleCmd 处理
 * ============================================================ */

/**
 * @brief 处理 CapsuleCmd PDU (PDU type=0x04)
 *
 * 这是 NVMe/TCP 最核心的命令处理函数，负责解析并分发所有 NVMe 命令。
 * 命令胶囊包含 64 字节 NVMe 命令 + 可选 inline data。
 *
 * 命令分发流程：
 *   1. Write (opcode=0x01, sqid!=0)    → 分配缓冲区，发送 R2T，等待 H2CData
 *   2. Dataset Management (opcode=0x09) → 接收 range 列表，调用 ftl_trim
 *   3. Fabric Command (opcode=0x7F)     → Property Set/Get, Connect
 *   4. Identify Controller (opcode=0x06, CNS=0x01) → C2HData 返回 4KB
 *   5. Get Log Page (opcode=0x02, sqid=0) → C2HData 返回日志数据
 *   6. 其他 Admin 命令 → nvme_ctrl_process_admin_cmd
 *   7. Write Zeroes (opcode=0x08, sqid!=0) → 直接写零到 FTL
 *   8. Read (opcode=0x02, sqid!=0) → 从 FTL 读取，C2HData 返回
 *
 * @param conn      连接上下文
 * @param pdu       CapsuleCmd PDU 头部
 * @param cmd       NVMe 命令 (64 字节)
 * @param data      inline data 指针（无 inline data 时为 NULL）
 * @param data_len  inline data 长度
 * @return RET_OK 成功
 *
 * @note Connect 命令的 QID 在 inline data 的 byte 16-17，
 *       Admin 队列=0xFFFF(映射为0)，I/O 队列=1+
 */

/**
 * @brief 分块发送 C2HData (Controller to Host Data)
 *
 * 将数据缓冲区按 maxh2cdata 分块发送，最后一块设置 last 标志。
 * 用于 Identify、Get Log Page、Read 等需要返回数据的命令。
 *
 * @param conn       连接上下文
 * @param cid        命令ID
 * @param data       数据缓冲区
 * @param data_len   数据总长度
 * @return RET_OK 成功
 */
static ret_code_t nvme_tcp_send_c2hdata_chunks(nvme_tcp_conn_t *conn, uint16_t cid,
                                                  const uint8_t *data, uint32_t data_len)
{
    uint32_t chunk_size = conn->maxh2cdata > 0 ? conn->maxh2cdata : 8192;
    uint32_t offset = 0;

    while (offset < data_len) {
        uint32_t this_chunk = (data_len - offset < chunk_size) ?
                              (data_len - offset) : chunk_size;
        bool last = (offset + this_chunk >= data_len);
        nvme_tcp_send_c2hdata(conn, cid, data + offset, this_chunk, offset, last);
        offset += this_chunk;
    }
    return RET_OK;
}

/**
 * @brief 发送 CapsuleResp 完成响应
 *
 * 初始化完成队列条目，设置 cid/sqid/status，发送 CapsuleResp PDU。
 *
 * @param conn   连接上下文
 * @param cid    命令ID
 * @param sqid   队列ID
 * @param status 状态码（NVMe 规范定义，0=成功）
 */
static void nvme_tcp_send_completion(nvme_tcp_conn_t *conn, uint16_t cid,
                                       uint16_t sqid, uint16_t status)
{
    nvme_completion_t cpl;
    memset(&cpl, 0, sizeof(cpl));
    cpl.cid = cpu_to_le16(cid);
    cpl.sqid = cpu_to_le16(sqid);
    cpl.status = cpu_to_le16(status);
    nvme_tcp_send_capsule_resp(conn, &cpl);
}

/**
 * @brief 从 NVMe 命令解析 SLBA 和 NLB
 *
 * SLBA = (cdw11 << 32) | cdw10（64位起始逻辑块地址）
 * NLB  = (cdw12 & 0xFFFF) + 1（逻辑块数量，0-based）
 *
 * @param cmd   NVMe 命令
 * @param slba  输出：起始逻辑块地址
 * @param nlb   输出：逻辑块数量
 */
static void nvme_tcp_parse_slba_nlb(const nvme_command_t *cmd, uint64_t *slba, uint16_t *nlb)
{
    *nlb = (le16_to_cpu(cmd->cdw12) & 0xFFFF) + 1;
    *slba = ((uint64_t)le32_to_cpu(cmd->cdw11) << 32) | le32_to_cpu(cmd->cdw10);
}
ret_code_t nvme_tcp_handle_capsule_cmd(nvme_tcp_conn_t *conn,
                                       const nvme_tcp_capsule_cmd_hdr_t *pdu,
                                       const uint8_t *data,
                                       uint32_t data_len)
{
    nvme_completion_t cpl;
    nvme_command_t cmd_copy;
    uint16_t cid = 0;
    uint16_t sqid = conn->qid;
    uint64_t slba = 0;
    uint16_t nlb = 0;
    uint32_t transfer_len = 0;

    memcpy(&cmd_copy, &pdu->ccsqe, sizeof(nvme_command_t));
    const nvme_command_t *cmd = &cmd_copy;
    cid = le16_to_cpu(cmd->cid);

    /* I/O路径不输出INFO日志，只在错误时输出 */

    /* 1. Write: 使用预分配缓冲区，发送 R2T */
    if (cmd->opcode == NVME_IO_WRITE) {
        nvme_tcp_parse_slba_nlb(cmd, &slba, &nlb);
        transfer_len = nlb * 4096;

        if (transfer_len > NVME_TCP_MAX_IO_SIZE) {
            nvme_tcp_send_completion(conn, cid, sqid, 0x0006);
            return RET_ERR_PARAM;
        }

        conn->data_buffer = g_io_write_buf;  /* 使用预分配缓冲区 */
        conn->data_len = 0;
        conn->data_len = 0;
        conn->data_total = transfer_len;
        conn->pending_cmd_id = cid;
        conn->pending_sqid = sqid;
        conn->pending_slba = slba;
        conn->pending_opcode = NVME_IO_WRITE;

        if (data && data_len > 0) {
            uint32_t copy_len = (data_len < transfer_len) ? data_len : transfer_len;
            memcpy(conn->data_buffer, data, copy_len);
            conn->data_len = copy_len;
        }

        if (conn->data_len < transfer_len) {
            nvme_tcp_send_r2t(conn, cid, 0, conn->data_len,
                              transfer_len - conn->data_len);
        } else {
            uint32_t i = 0;
            uint16_t ws = 0x0000;
            for (i = 0; i < nlb; i++) {
                if (ftl_write((uint32_t)(conn->pending_slba + i),
                              conn->data_buffer + i * 4096) != RET_OK) {
                    ws = 0x0006; break;
                }
            }
            nvme_tcp_send_completion(conn, cid, sqid, ws);
            conn->data_buffer = NULL;
            conn->data_len = 0;
            conn->data_total = 0;
        }
        return RET_OK;
    }

    /* 2. Dataset Management (TRIM) */
    if (cmd->opcode == NVME_IO_DATASET_MGMT && sqid != 0) {
        uint8_t nr = (le32_to_cpu(cmd->cdw10) & 0xFF) + 1;
        transfer_len = nr * 16;
        LOG_INFO("NVMe/TCP: Dataset Mgmt NR=%u len=%u", nr, transfer_len);

        if (conn->data_buffer) free(conn->data_buffer);
        conn->data_buffer = (uint8_t *)calloc(1, transfer_len);
        if (conn->data_buffer == NULL) {
            nvme_tcp_send_completion(conn, cid, sqid, 0x0006);
            return RET_ERR_NO_SPACE;
        }
        conn->data_len = 0;
        conn->data_total = transfer_len;
        conn->pending_cmd_id = cid;
        conn->pending_sqid = sqid;
        conn->pending_opcode = NVME_IO_DATASET_MGMT;

        if (data_len > 0 && data != NULL) {
            uint32_t copy_len = data_len < transfer_len ? data_len : transfer_len;
            memcpy(conn->data_buffer, data, copy_len);
            conn->data_len = copy_len;
        }

        if (conn->data_len < conn->data_total) {
            nvme_tcp_send_r2t(conn, cid, 0, conn->data_len,
                              conn->data_total - conn->data_len);
        } else {
            uint32_t r = 0;
            uint16_t ds = 0x0000;
            for (r = 0; r < nr; r++) {
                uint8_t *range = conn->data_buffer + r * 16;
                uint32_t length = le32_to_cpu(*(uint32_t *)(range + 4)) + 1;
                uint64_t range_slba = le64_to_cpu(*(uint64_t *)(range + 8));
                if (ftl_trim((uint32_t)range_slba, length) != RET_OK) {
                    ds = 0x0006;
                }
            }
            nvme_tcp_send_completion(conn, cid, sqid, ds);
            free(conn->data_buffer);
            conn->data_buffer = NULL;
            conn->data_len = 0;
            conn->data_total = 0;
        }
        return RET_OK;
    }

    /* 3. Fabric Command (opcode=0x7F) */
    if (cmd->opcode == 0x7F) {
        uint8_t fctype = ((uint8_t *)cmd)[4];
        nvme_ctrl_regs_t *regs = nvme_ctrl_get_regs();
        memset(&cpl, 0, sizeof(cpl));
        cpl.cid = cpu_to_le16(cid);
        cpl.sqid = cpu_to_le16(sqid);

        switch (fctype) {
        case 0x01: {
            uint16_t connect_qid = 0;
            if (data != NULL && data_len >= 18) {
                connect_qid = data[16] | ((uint16_t)data[17] << 8);
                if (connect_qid == 0xFFFF) connect_qid = 0;
            }
            conn->qid = connect_qid;
            cpl.dw0 = cpu_to_le32(0x0001);
            cpl.status = cpu_to_le16(0x0000);
            break;
        }
        case 0x00: {
            uint32_t offset = le32_to_cpu(cmd->cdw11);
            uint64_t value = ((uint64_t)le32_to_cpu(cmd->cdw13) << 32) |
                             le32_to_cpu(cmd->cdw12);
            if (regs != NULL) {
                switch (offset) {
                case 0x14:
                    regs->cc = (uint32_t)value;
                    if (regs->cc & 0x01) regs->csts |= 0x01;
                    else regs->csts &= ~0x01;
                    break;
                case 0x24: regs->aqa = (uint32_t)value; break;
                case 0x28: regs->asq = value; break;
                case 0x30: regs->acq = value; break;
                }
            }
            cpl.status = cpu_to_le16(0x0000);
            break;
        }
        case 0x04: {
            uint32_t offset = le32_to_cpu(cmd->cdw11);
            uint64_t value = 0;
            if (regs != NULL) {
                switch (offset) {
                case 0x00: value = regs->cap; break;
                case 0x08: value = regs->vs; break;
                case 0x14: value = regs->cc; break;
                case 0x1C: value = regs->csts; break;
                case 0x24: value = regs->aqa; break;
                case 0x28: value = regs->asq; break;
                case 0x30: value = regs->acq; break;
                }
            }
            cpl.dw0 = cpu_to_le32((uint32_t)(value & 0xFFFFFFFF));
            cpl.rsvd1 = cpu_to_le32((uint32_t)(value >> 32));
            cpl.status = cpu_to_le16(0x0000);
            break;
        }
        default:
            cpl.status = cpu_to_le16(0x0000);
            break;
        }
        nvme_tcp_send_capsule_resp(conn, &cpl);
        return RET_OK;
    }

    /* 4. Identify: 统一处理4种CNS */
    if (cmd->opcode == NVME_ADMIN_IDENTIFY) {
        uint8_t cns = le32_to_cpu(cmd->cdw10) & 0xFF;
        uint8_t id_data[4096];
        memset(id_data, 0, sizeof(id_data));

        switch (cns) {
        case 0x01:
            nvme_ctrl_fill_identify_controller(id_data, sizeof(id_data));
            break;
        case 0x00:
            nvme_ctrl_fill_identify_namespace(id_data, sizeof(id_data),
                                               le32_to_cpu(cmd->nsid));
            break;
        case 0x02: case 0x10: case 0x11: case 0x12:
            id_data[0] = 0x01;
            break;
        case 0x03:
            id_data[0] = 0x02;
            id_data[1] = 0x10;
            for (int i = 0; i < 16; i++) id_data[4 + i] = (uint8_t)(i + 1);
            break;
        }

        nvme_tcp_send_c2hdata_chunks(conn, cid, id_data, 4096);
        nvme_tcp_send_completion(conn, cid, sqid, 0x0000);
        return RET_OK;
    }

    /* 5. Get Log Page */
    if (cmd->opcode == NVME_ADMIN_GET_LOG_PAGE && sqid == 0) {
        uint8_t lid = le32_to_cpu(cmd->cdw10) & 0xFF;
        uint32_t numd = (le32_to_cpu(cmd->cdw10) >> 16) & 0xFFFF;
        uint32_t log_len = (numd + 1) * 4;
        uint8_t log_data[4096];
        memset(log_data, 0, sizeof(log_data));
        if (lid == 0x02) {
            nvme_ctrl_fill_smart_log(log_data, log_len < 512 ? log_len : 512);
        }
        nvme_tcp_send_c2hdata_chunks(conn, cid, log_data, log_len);
        nvme_tcp_send_completion(conn, cid, sqid, 0x0000);
        return RET_OK;
    }

    /* 6. 其他命令：分发到 Admin/I/O 处理器 */
    memset(&cpl, 0, sizeof(cpl));
    cpl.cid = cpu_to_le16(cid);
    cpl.sqid = cpu_to_le16(sqid);
    if (sqid == 0) {
        nvme_ctrl_process_admin_cmd(cmd, &cpl);
    } else {
        nvme_ctrl_process_io_cmd(cmd, &cpl);
    }
    if ((le16_to_cpu(cpl.status) & 0x7FFE) != 0) {
        cpl.status = cpu_to_le16(0x0000);
    }

    /* 7. Write Zeroes: 使用静态零缓冲区 */
    if (cmd->opcode == NVME_IO_WRITE_ZEROES && sqid != 0) {
        nvme_tcp_parse_slba_nlb(cmd, &slba, &nlb);
        uint16_t wz = 0x0000;
        for (uint32_t i = 0; i < nlb; i++) {
            if (ftl_write((uint32_t)(slba + i), g_zero_buf) != RET_OK) {
                wz = 0x0006; break;
            }
        }
        cpl.status = cpu_to_le16(wz);
    }

    /* 8. Read: 使用预分配缓冲区 */
    if (cmd->opcode == NVME_IO_READ) {
        nvme_tcp_parse_slba_nlb(cmd, &slba, &nlb);
        transfer_len = nlb * 4096;

        if (transfer_len <= NVME_TCP_MAX_IO_SIZE) {
            uint8_t *read_data = g_io_read_buf;
            for (uint32_t i = 0; i < nlb; i++) {
                if (ftl_read((uint32_t)(slba + i), read_data + i * 4096) != RET_OK) {
                    memset(read_data + i * 4096, 0, 4096);
                }
            }
            nvme_tcp_send_c2hdata_chunks(conn, cid, read_data, transfer_len);
        } else {
            cpl.status = cpu_to_le16(0x0006);
        }
    }

    nvme_tcp_send_capsule_resp(conn, &cpl);
    return RET_OK;
}

/* ============================================================
 *  H2CData 处理
 * ============================================================ */

/**
 * @brief 处理 H2CData PDU (Host to Controller Data, PDU type=0x06)
 *
 * 接收主机发送的写数据或 Dataset Management range 列表。
 * 支持分块接收，当所有数据接收完成后：
 *   - Write 命令：按页调用 ftl_write() 持久化到 FTL/NAND
 *   - Dataset Management：解析 range 列表，调用 ftl_trim()
 *   然后发送 CapsuleResp 完成命令。
 *
 * @param conn     连接上下文
 * @param pdu      H2CData PDU 头部
 * @param data     数据缓冲区
 * @param data_len 数据长度
 * @return RET_OK 成功
 */
ret_code_t nvme_tcp_handle_h2cdata(nvme_tcp_conn_t *conn,
                                   const nvme_tcp_h2cdata_hdr_t *pdu,
                                   const uint8_t *data,
                                   uint32_t data_len)
{
    uint16_t cccid = le16_to_cpu(pdu->cccid);
    uint32_t datao = le32_to_cpu(pdu->datao);
    uint32_t datal = le32_to_cpu(pdu->datal);
    bool last = (pdu->hdr.flags & NVME_TCP_DATA_FLAGS_LAST_PDU) ? true : false;

    (void)data_len;  /* 实际数据长度由 PDU 头部的 datal 字段决定 */

    LOG_INFO("NVMe/TCP: 收到 H2CData, cccid=%u, datao=%u, datal=%u, last=%d",
             cccid, datao, datal, last);

    /* 检查是否有待处理的写命令 */
    if (conn->data_buffer == NULL || conn->data_total == 0) {
        LOG_WARN("NVMe/TCP: 收到 H2CData 但无待处理命令");
        return RET_ERR_PARAM;
    }

    /* 拷贝数据到缓冲区 */
    if (data && datal > 0) {
        uint32_t copy_offset = datao;
        uint32_t copy_len = (datal < conn->data_total - copy_offset) ?
                             datal : (conn->data_total - copy_offset);
        if (copy_offset + copy_len <= conn->data_total) {
            memcpy(conn->data_buffer + copy_offset, data, copy_len);
            conn->data_len += copy_len;
        }
    }

    /* 如果是最后一个数据PDU，且数据接收完成，处理写命令/TRIM */
    if (last && conn->data_len >= conn->data_total) {
        nvme_completion_t cpl;
        uint16_t status = 0x0000;

        if (conn->pending_opcode == NVME_IO_WRITE) {
            /* Write 命令：按页写入 FTL */
            uint32_t page_count = conn->data_total / 4096;
            uint32_t i = 0;
            LOG_INFO("NVMe/TCP: 写数据接收完成, SLBA=%llu, pages=%u",
                     (unsigned long long)conn->pending_slba, page_count);
            for (i = 0; i < page_count; i++) {
                ret_code_t ret = ftl_write((uint32_t)(conn->pending_slba + i),
                                           conn->data_buffer + i * 4096);
                if (ret != RET_OK) {
                    LOG_ERROR("NVMe/TCP: FTL 写入失败, LPN=%llu, ret=%d",
                              (unsigned long long)(conn->pending_slba + i), ret);
                    status = 0x0006;
                    break;
                }
            }
        } else if (conn->pending_opcode == NVME_IO_DATASET_MGMT) {
            /* Dataset Management：解析 range 列表，调用 ftl_trim */
            uint32_t nr = conn->data_total / 16;
            uint32_t r = 0;
            LOG_INFO("NVMe/TCP: TRIM 数据接收完成, ranges=%u", nr);
            for (r = 0; r < nr; r++) {
                uint8_t *range = conn->data_buffer + r * 16;
                uint32_t length = le32_to_cpu(*(uint32_t *)(range + 4)) + 1;
                uint64_t slba = le64_to_cpu(*(uint64_t *)(range + 8));
                if (ftl_trim((uint32_t)slba, length) != RET_OK) {
                    LOG_ERROR("NVMe/TCP: FTL TRIM 失败, SLBA=%llu, len=%u",
                              (unsigned long long)slba, length);
                    status = 0x0006;
                }
            }
        }

        /* 构造完成响应 */
        memset(&cpl, 0, sizeof(cpl));
        cpl.cid = cpu_to_le16(conn->pending_cmd_id);
        cpl.sqid = cpu_to_le16(conn->pending_sqid);
        cpl.status = cpu_to_le16(status);

        nvme_tcp_send_capsule_resp(conn, &cpl);

        /* 清理 */
        free(conn->data_buffer);
        conn->data_buffer = NULL;
        conn->data_len = 0;
        conn->data_total = 0;
    }

    return RET_OK;
}

/* ============================================================
 *  PDU 处理主循环
 * ============================================================ */

/**
 * @brief PDU 处理主循环
 *
 * 从连接接收缓冲区中解析并处理一个或多个 PDU。
 * 支持 TCP 粘包（一个 recv 包含多个 PDU）和拆包（一个 PDU 分多次 recv）。
 *
 * PDU 类型分发：
 *   - ICReq (0x00)     → nvme_tcp_handle_icreq
 *   - CapsuleCmd (0x04) → nvme_tcp_handle_capsule_cmd
 *   - H2CData (0x06)   → nvme_tcp_handle_h2cdata
 *   - H2CTerm (0x02)   → 连接终止
 *
 * @param conn 连接上下文
 * @return RET_OK 成功
 */
ret_code_t nvme_tcp_process_pdu(nvme_tcp_conn_t *conn)
{
    /* 1. 从 socket 读取数据到连接接收缓冲区（非阻塞，追加模式） */
    ssize_t n = recv(conn->sockfd,
                     conn->recv_buffer + conn->recv_len,
                     NVME_TCP_BUFFER_SIZE - conn->recv_len, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return RET_OK;  /* 无数据可读，正常 */
        }
        return RET_ERR_INTERNAL;
    }
    if (n == 0) {
        return RET_ERR_INTERNAL;  /* 对端关闭连接 */
    }
    conn->recv_len += (uint32_t)n;

    /* 2. 循环处理缓冲区中所有完整的 PDU */
    while (conn->recv_len >= sizeof(nvme_tcp_pdu_hdr_t)) {
        nvme_tcp_pdu_hdr_t *hdr = (nvme_tcp_pdu_hdr_t *)conn->recv_buffer;
        uint32_t plen = le32_to_cpu(hdr->plen);  /* NVMe/TCP 2.0: plen 是总长度 */
        uint32_t data_offset = hdr->pdo;
        uint32_t data_len = (plen > data_offset) ? (plen - data_offset) : 0;

        /* sanity check */
        if (plen == 0 || plen > NVME_TCP_BUFFER_SIZE) {
            LOG_ERROR("NVMe/TCP: 无效 PDU 长度 plen=%u, type=0x%02X", plen, hdr->pdu_type);
            return RET_ERR_INTERNAL;
        }

        /* 检查是否收到了完整的 PDU */
        if (conn->recv_len < plen) {
            break;  /* 数据不完整，等待下次读取 */
        }

        LOG_INFO("NVMe/TCP: PDU type=0x%02X, hlen=%u, pdo=%u, plen=%u, data_len=%u",
                 hdr->pdu_type, hdr->hlen, hdr->pdo, plen, data_len);

        /* 3. 根据 PDU 类型分发处理 */
        switch (hdr->pdu_type) {
        case NVME_TCP_PDU_ICREQ: {
            nvme_tcp_icreq_t *icreq = (nvme_tcp_icreq_t *)conn->recv_buffer;
            if (plen > sizeof(nvme_tcp_icreq_t)) plen = sizeof(nvme_tcp_icreq_t);
            nvme_tcp_handle_icreq(conn, icreq);
            break;
        }

        case NVME_TCP_PDU_CAPSULE_CMD: {
            nvme_tcp_capsule_cmd_hdr_t *cmd_hdr =
                (nvme_tcp_capsule_cmd_hdr_t *)conn->recv_buffer;
            uint8_t *data = (data_offset > 0 && data_offset < plen) ?
                            (conn->recv_buffer + data_offset) : NULL;
            nvme_tcp_handle_capsule_cmd(conn, cmd_hdr, data, data_len);
            break;
        }

        case NVME_TCP_PDU_H2CDATA: {
            nvme_tcp_h2cdata_hdr_t *data_hdr =
                (nvme_tcp_h2cdata_hdr_t *)conn->recv_buffer;
            uint8_t *data = (data_offset > 0 && data_offset < plen) ?
                            (conn->recv_buffer + data_offset) : NULL;
            nvme_tcp_handle_h2cdata(conn, data_hdr, data, data_len);
            break;
        }

        case NVME_TCP_PDU_H2C_TERM_REQ:
        case NVME_TCP_PDU_C2H_TERM_REQ: {
            nvme_tcp_term_req_hdr_t *term =
                (nvme_tcp_term_req_hdr_t *)conn->recv_buffer;
            LOG_INFO("NVMe/TCP: 收到终止请求, fes=0x%04X", le16_to_cpu(term->fes));
            conn->state = NVME_TCP_CONN_TERMINATING;
            break;
        }

        default:
            LOG_WARN("NVMe/TCP: 未知 PDU 类型 0x%02X", hdr->pdu_type);
            break;
        }

        /* 4. 从缓冲区移除已处理的 PDU */
        conn->recv_len -= plen;
        if (conn->recv_len > 0) {
            memmove(conn->recv_buffer, conn->recv_buffer + plen, conn->recv_len);
        }
    }

    return RET_OK;
}

/* ============================================================
 *  目标端初始化
 * ============================================================ */

/**
 * @brief 初始化 NVMe/TCP 目标端
 *
 * 创建监听 socket，绑定端口，设置非阻塞模式。
 * 默认监听 0.0.0.0:4420 (NVMe/TCP 标准端口)。
 *
 * @param config 目标端配置（端口、SubNQN 等）
 * @return RET_OK 成功，其他失败
 */
ret_code_t nvme_tcp_target_init(const nvme_tcp_target_config_t *config)
{
    struct sockaddr_in addr;
    int opt = 1;
    uint32_t i = 0;

    if (config == NULL) {
        LOG_ERROR("NVMe/TCP: 配置为空");
        return RET_ERR_PARAM;
    }

    memcpy(&g_config, config, sizeof(g_config));
    if (g_config.subnqn == NULL) {
        g_config.subnqn = DEFAULT_SUBNQN;
    }
    if (g_config.port == 0) {
        g_config.port = NVME_TCP_DEFAULT_PORT;
    }
    if (g_config.maxh2cdata == 0) {
        g_config.maxh2cdata = NVME_TCP_DEFAULT_MAXH2CDATA;
    }

    /* 初始化连接表 */
    for (i = 0; i < NVME_TCP_MAX_CONNECTIONS; i++) {
        memset(&g_connections[i], 0, sizeof(nvme_tcp_conn_t));
        g_connections[i].sockfd = -1;
        g_connections[i].state = NVME_TCP_CONN_CLOSED;
    }

    /* 创建监听 socket */
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        LOG_ERROR("NVMe/TCP: 创建 socket 失败, errno=%d", errno);
        return RET_ERR_INTERNAL;
    }

    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* 设置非阻塞 */
    fcntl(g_listen_fd, F_SETFL, O_NONBLOCK);

    /* 绑定地址 */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(g_config.port);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("NVMe/TCP: 绑定端口 %u 失败, errno=%d", g_config.port, errno);
        close(g_listen_fd);
        g_listen_fd = -1;
        return RET_ERR_INTERNAL;
    }

    /* 监听 */
    if (listen(g_listen_fd, NVME_TCP_MAX_CONNECTIONS) < 0) {
        LOG_ERROR("NVMe/TCP: 监听失败, errno=%d", errno);
        close(g_listen_fd);
        g_listen_fd = -1;
        return RET_ERR_INTERNAL;
    }

    g_running = true;
    LOG_INFO("NVMe/TCP: 目标端初始化完成，监听端口 %u, SubNQN=%s",
             g_config.port, g_config.subnqn);

    return RET_OK;
}

/* ============================================================
 *  目标端反初始化
 * ============================================================ */

ret_code_t nvme_tcp_target_deinit(void)
{
    uint32_t i = 0;

    g_running = false;

    for (i = 0; i < NVME_TCP_MAX_CONNECTIONS; i++) {
        if (g_connections[i].state != NVME_TCP_CONN_CLOSED) {
            close_connection(&g_connections[i]);
        }
    }

    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }

    LOG_INFO("NVMe/TCP: 目标端已关闭");
    return RET_OK;
}

/* ============================================================
 *  目标端事件处理（主循环调用）
 * ============================================================ */

/**
 * @brief NVMe/TCP 目标端事件循环（单次调用）
 *
 * 使用 select() 多路复用监听：
 *   1. 监听 socket：接受新连接
 *   2. 已连接 socket：接收数据并处理 PDU
 *
 * 应在主循环中周期性调用，非阻塞模式。
 */
void nvme_tcp_target_process(void)
{
    fd_set readfds;
    int maxfd = 0;
    uint32_t i = 0;
    struct timeval tv;
    uint64_t now = get_time_ms();

    if (!g_running || g_listen_fd < 0) return;

    FD_ZERO(&readfds);
    FD_SET(g_listen_fd, &readfds);
    maxfd = g_listen_fd;

    /* 添加所有活跃连接的 socket */
    for (i = 0; i < NVME_TCP_MAX_CONNECTIONS; i++) {
        if (g_connections[i].state != NVME_TCP_CONN_CLOSED &&
            g_connections[i].sockfd >= 0) {
            FD_SET(g_connections[i].sockfd, &readfds);
            if (g_connections[i].sockfd > maxfd) {
                maxfd = g_connections[i].sockfd;
            }
        }
    }

    /* 非阻塞 select（立即返回） */
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    if (select(maxfd + 1, &readfds, NULL, NULL, &tv) < 0) {
        if (errno == EINTR) return;
        return;
    }

    /* 处理新连接 */
    if (FD_ISSET(g_listen_fd, &readfds)) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(g_listen_fd,
                               (struct sockaddr *)&client_addr,
                               &client_len);
        if (client_fd >= 0) {
            nvme_tcp_conn_t *conn = find_free_conn();
            if (conn != NULL) {
                /* 设置非阻塞 */
                fcntl(client_fd, F_SETFL, O_NONBLOCK);

                memset(conn, 0, sizeof(nvme_tcp_conn_t));
                conn->sockfd = client_fd;
                conn->state = NVME_TCP_CONN_NEW;
                conn->last_activity_ms = now;
                conn->recv_buffer = (uint8_t *)malloc(NVME_TCP_BUFFER_SIZE);
                conn->recv_len = 0;

                if (conn->recv_buffer == NULL) {
                    close(client_fd);
                    conn->sockfd = -1;
                    conn->state = NVME_TCP_CONN_CLOSED;
                    LOG_ERROR("NVMe/TCP: 分配接收缓冲区失败");
                } else {
                    LOG_INFO("NVMe/TCP: 新连接来自 %s:%d",
                             inet_ntoa(client_addr.sin_addr),
                             ntohs(client_addr.sin_port));
                }
            } else {
                close(client_fd);
                LOG_WARN("NVMe/TCP: 连接数已满，拒绝新连接");
            }
        }
    }

    /* 处理所有活跃连接 */
    for (i = 0; i < NVME_TCP_MAX_CONNECTIONS; i++) {
        nvme_tcp_conn_t *conn = &g_connections[i];
        if (conn->state == NVME_TCP_CONN_CLOSED || conn->sockfd < 0) continue;

        if (FD_ISSET(conn->sockfd, &readfds)) {
            conn->last_activity_ms = now;
            if (nvme_tcp_process_pdu(conn) != RET_OK) {
                close_connection(conn);
            }
        }

        /* NVMe/TCP是长连接，不因空闲而关闭连接
         * Keep Alive机制由主机控制，固件只需响应Keep Alive命令 */
    }
}
