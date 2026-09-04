# UFS系统特性设计文档

> 本文档详细描述ftl-firmware项目中UFS（Universal Flash Storage）目标端的系统特性设计与实现，涵盖电源管理、错误恢复、健康监控、安全特性等核心系统功能。

---

## 1. UFS协议栈架构

### 1.1 协议层次

```
┌─────────────────────────────────────────────────┐
│  应用层：SCSI命令集（SAM-5/SPC-4/SBC-4）        │
│  INQUIRY/READ/WRITE/UNMAP/MODE_SENSE/LOG_SENSE  │
├─────────────────────────────────────────────────┤
│  传输层：UPIU（UFS Protocol Information Unit）   │
│  命令UPIU/响应UPIU/数据IN/数据OUT/任务管理       │
├─────────────────────────────────────────────────┤
│  链路层：UniPro（MIPI UniPro Specification）     │
│  连接管理/流量控制/错误检测/重传                 │
├─────────────────────────────────────────────────┤
│  物理层：MIPI M-PHY                              │
│  HS-Gear1/2/3/4/5, PWM-Gear1/2/3/4/5/6/7      │
└─────────────────────────────────────────────────┘
```

### 1.2 UFS与NVMe对比

| 特性 | UFS | NVMe |
|------|-----|------|
| 命令集 | SCSI（SAM-5） | NVMe原生命令集 |
| 传输层 | UPIU over UniPro | PCIe/NVMe-oF(TCP/RDMA) |
| 物理层 | MIPI M-PHY | PCIe PHY |
| 队列深度 | 最多32（单队列） | 最多64K（65535队列） |
| 典型应用 | 手机/移动设备/汽车 | PC/服务器/数据中心 |
| 电源管理 | 精细（4种电源模式） | 相对简单（APST） |
| FTL层 | 通用（算法完全一致） | 通用（算法完全一致） |

> **关键洞察**：UFS和NVMe的FTL层算法完全通用，仅前端协议不同。本项目同时实现UFS和NVMe双协议栈，证明了FTL算法的协议无关性。

---

## 2. 电源管理设计

### 2.1 四种电源模式

| 模式 | 功耗 | 唤醒延迟 | 适用场景 |
|------|------|---------|---------|
| ACTIVE | 最高 | 0 | 正常读写操作 |
| IDLE | 中 | <10μs | 短暂空闲，快速响应 |
| SLEEP | 低 | <1ms | 较长时间空闲 |
| POWER_DOWN | 最低 | 需重新初始化 | 系统关机 |

### 2.2 电源模式切换流程

```
主机发送START STOP UNIT命令
         │
         ▼
  检查当前电源模式
         │
    ┌────┴────┐
    ▼         ▼
  进入低功耗  唤醒
    │         │
    ▼         ▼
  保存上下文  恢复上下文
  关闭时钟    使能时钟
  关闭电路    使能电路
    │         │
    └────┬────┘
         ▼
  更新电源模式状态
  发送响应给主机
```

### 2.3 关键实现

```c
ret_code_t ufs_target_set_power_mode(ufs_power_mode_t mode)
{
    /* 记录电源模式切换（用于功耗分析） */
    LOG_INFO("UFS: 电源模式切换 %d -> %d", g_power_mode, mode);

    g_power_mode = mode;

    switch (mode) {
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
    }
    return RET_OK;
}
```

---

## 3. 错误处理与恢复设计

### 3.1 错误分类

| 错误类型 | 可重试 | 典型场景 | 处理策略 |
|---------|-------|---------|---------|
| 介质错误 | 是 | NAND读失败/比特翻转 | 自动重试+ECC纠错 |
| 超时错误 | 是 | 命令执行超时 | 重试+任务管理取消 |
| 参数错误 | 否 | 非法LBA/非法命令 | 直接返回CHECK_CONDITION |
| 写保护错误 | 否 | 写保护状态下写入 | 返回WRITE_PROTECTED |
| 设备忙 | 是 | 后台操作进行中 | 等待后重试 |

### 3.2 命令重试机制

```
命令执行失败
     │
     ▼
  检查错误类型
     │
  ┌──┴──┐
  ▼     ▼
可重试  不可重试
  │     │
  ▼     ▼
重试次数<3?  返回错误
  │
  ├─是→ 重试命令，重试计数+1
  │
  └─否→ 返回错误，记录失败统计
```

### 3.3 错误统计

```c
typedef struct {
    uint32_t total_cmd_count;       /* 总命令数 */
    uint32_t success_count;         /* 成功完成数 */
    uint32_t retry_count;           /* 重试次数 */
    uint32_t failure_count;         /* 失败次数 */
    uint32_t media_error_count;     /* 介质错误数 */
    uint32_t timeout_error_count;   /* 超时错误数 */
} ufs_error_stats_t;
```

> **实际价值**：错误统计是固件质量的关键指标，可用于：
> - 量产良率分析
> - 现场故障诊断
> - 固件版本质量对比
> - 客户问题定位

---

## 4. 健康监控设计

### 4.1 健康状态指标

| 指标 | 单位 | 告警阈值 | 严重阈值 | 说明 |
|------|------|---------|---------|------|
| 温度 | °C | 70 | 85 | 超过严重阈值触发降速保护 |
| 寿命已用 | % | 80 | 95 | 超过告警阈值建议备份数据 |
| 总擦除次数 | 次 | - | - | NAND磨损程度指标 |
| 上电次数 | 次 | - | - | 设备使用频率 |
| 累计上电时间 | 分钟 | - | - | 设备总运行时间 |
| 非正常关机次数 | 次 | - | - | PLP保护有效性指标 |

### 4.2 温度保护机制

```
温度监控周期处理
       │
       ▼
  读取当前温度
       │
  ┌────┼────┐
  ▼    ▼    ▼
 <70°C 70-85°C >85°C
  │     │      │
  ▼     ▼      ▼
正常  温度告警  严重过热
      上报主机  触发降速
              限制写入速率
              启动散热策略
```

### 4.3 健康监控周期处理

```c
ret_code_t ufs_target_health_monitor_process(void)
{
    /* 更新上电时间 */
    g_health_info.power_on_minutes++;

    /* 温度监控 */
    if (g_health_info.temperature >= UFS_TEMP_CRITICAL_THRESHOLD) {
        g_health_info.temperature_warning = true;
        LOG_WARN("UFS: 温度严重过高 %d°C，触发降速保护",
                 g_health_info.temperature);
    } else if (g_health_info.temperature >= UFS_TEMP_WARNING_THRESHOLD) {
        g_health_info.temperature_warning = true;
        LOG_WARN("UFS: 温度过高 %d°C，建议降低负载",
                 g_health_info.temperature);
    }

    /* 寿命监控 */
    if (g_health_info.lifetime_used_percent >= UFS_LIFETIME_WARNING_PERCENT) {
        g_health_info.lifetime_warning = true;
        LOG_WARN("UFS: 寿命已使用 %u%%，建议备份数据",
                 g_health_info.lifetime_used_percent);
    }

    return RET_OK;
}
```

---

## 5. 安全特性设计

### 5.1 写保护

**应用场景**：
- 取证场景：防止证据被篡改
- 固件更新：更新过程中保护关键区域
- 数据保护：重要数据只读保护

**实现机制**：
```c
ret_code_t ufs_target_set_write_protect(bool enable)
{
    g_health_info.write_protected = enable;
    LOG_INFO("UFS: 写保护 %s", enable ? "启用" : "禁用");
    return RET_OK;
}
```

写命令处理时检查：
```c
if (g_health_info.write_protected) {
    /* 返回WRITE_PROTECTED错误 */
    set_response_check_condition(response, 0x07, 0x27, 0x00);
    return RET_OK;
}
```

### 5.2 安全协议框架

支持SCSI SECURITY PROTOCOL IN/OUT命令框架，可扩展：
- TCG Opal自加密驱动器（SED）
- 安全擦除（Cryptographic Erase）
- 固件签名验证
- 调试端口锁定

---

## 6. 后台操作（BKOPS）设计

### 6.1 BKOPS触发时机

| 触发方式 | 说明 | 优先级 |
|---------|------|-------|
| 主机主动触发 | 主机发送BKOPS命令 | 高 |
| 空闲自动触发 | 系统空闲超过阈值 | 中 |
| 空间不足触发 | 空闲块低于阈值 | 高 |

### 6.2 BKOPS操作内容

1. **垃圾回收（GC）**：回收无效页，释放空闲块
2. **磨损均衡**：均衡各块擦除次数，延长寿命
3. **读干扰处理**：检测并修复读干扰错误
4. **数据刷新**：刷新保留时间过长的数据
5. **ECC校验**：后台扫描数据完整性

### 6.3 BKOPS实现

```c
ret_code_t ufs_target_trigger_background_ops(void)
{
    LOG_INFO("UFS: 触发后台操作（BKOPS）");

    /* 调用FTL层后台操作接口
     * - ftl_trigger_gc(): 垃圾回收
     * - ftl_trigger_wear_leveling(): 磨损均衡
     * - ftl_read_disturb_check(): 读干扰检测
     */

    return RET_OK;
}
```

---

## 7. 命令队列设计

### 7.1 队列结构

```c
typedef struct {
    bool     valid;           /* 条目有效标志 */
    uint8_t  task_id;         /* 任务ID */
    uint8_t  retry_count;     /* 已重试次数 */
    ufs_cmd_request_t request;/* 命令请求 */
} ufs_cmd_queue_entry_t;

static ufs_cmd_queue_entry_t g_cmd_queue[UFS_MAX_CMD_QUEUE];
```

### 7.2 队列深度

UFS 2.x/3.x/4.x规范支持最多32个命令队列深度（单队列），本项目实现完整的命令队列管理。

> **对比NVMe**：NVMe支持最多65535个队列，每个队列最多65535个命令。UFS队列深度较小，但对于移动设备场景已足够。

---

## 8. 完整项目经验总结

### 8.1 本项目UFS实现的系统特性

| 特性 | 实现状态 | 说明 |
|------|---------|------|
| SCSI命令集 | ✅ 完整 | INQUIRY/READ/WRITE/UNMAP/TEST_UNIT_READY等 |
| 电源管理 | ✅ 框架 | 4种电源模式切换 |
| 错误恢复 | ✅ 完整 | 命令重试+错误统计 |
| 健康监控 | ✅ 框架 | 温度/寿命/磨损监控 |
| 写保护 | ✅ 完整 | 软件写保护 |
| 安全协议 | ⚠️ 框架 | SECURITY PROTOCOL IN/OUT命令框架 |
| 后台操作 | ✅ 框架 | BKOPS触发接口 |
| 命令队列 | ✅ 完整 | 32深度命令队列 |

### 8.2 UFS开发的关键技术点

1. **大小端转换**：UFS/SCSI命令字段为大端，需正确转换
2. **SCSI感知数据**：CHECK_CONDITION时需填充正确的Sense Key/ASC/ASCQ
3. **电源状态机**：4种电源模式的状态切换和唤醒延迟
4. **错误恢复流程**：命令重试、任务管理、错误统计
5. **健康监控周期**：温度、寿命、磨损的周期监控和告警

### 8.3 与真实UFS固件开发的差距

| 方面 | 本项目（模拟器） | 真实UFS固件 |
|------|----------------|------------|
| 物理层 | 文件模拟NAND | 真实NAND芯片+M-PHY |
| 链路层 | 无（直接调用） | UniPro协议栈 |
| 时序 | 无真实延迟 | 符合UFS规范时序 |
| 电源 | 软件状态 | 真实硬件电源域控制 |
| 温度 | 模拟值 | 真实温度传感器 |
| ECC | 简化实现 | 硬件BCH/LDPC纠错 |

> **项目技术特点**：本项目同时实现UFS和NVMe双协议栈，体现了：
> 1. 对存储协议栈的深入理解（SCSI/NVMe/UPIU）
> 2. FTL算法的协议无关性设计能力
> 3. 系统特性（电源管理/错误恢复/健康监控）的设计经验
> 4. 跨协议栈的架构设计能力

---

## 9. 参考资料

- [UFS Specification v4.0](https://www.jedec.org/standards-documents/docs/jesd220f)
- [SCSI Primary Commands - 4 (SPC-4)](https://www.t10.org/cgi-bin/ac.pl?t=f&f=spc4r37.pdf)
- [SCSI Block Commands - 4 (SBC-4)](https://www.t10.org/cgi-bin/ac.pl?t=f&f=sbc4r08.pdf)
- [MIPI UniPro Specification](https://www.mipi.org/specifications/unipro)
- [MIPI M-PHY Specification](https://www.mipi.org/specifications/m-phy)
