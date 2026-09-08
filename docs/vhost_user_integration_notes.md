# vhost-user NVMe 后端集成说明

## 概述

本文档说明如何将 vhost-user NVMe 后端集成到 ftl-firmware 项目中，使固件可以作为 QEMU 的 NVMe 设备后端运行。

## 新增文件

| 文件 | 说明 |
|------|------|
| `include/protocol/vhost_user_nvme.h` | vhost-user 协议头文件（消息ID、数据结构、接口声明） |
| `src/protocol/nvme/vhost_user_nvme.c` | vhost-user NVMe 后端完整实现 |

## 一、Makefile 修改

### 1.1 添加源文件到 CORE_SRCS

在 `Makefile` 的 `CORE_SRCS` 变量中添加新源文件：

```makefile
CORE_SRCS = $(SRC_DIR)/main.c \
            $(SRC_DIR)/protocol/nvme/nvme_controller.c \
            $(SRC_DIR)/protocol/nvme/nvme_tcp_target.c \
            $(SRC_DIR)/protocol/nvme/vhost_user_nvme.c \   # <-- 新增此行
            $(SRC_DIR)/protocol/ufs/ufs_target.c \
            ...
```

### 1.2 构建目录

构建目录 `$(BUILD_DIR)/$(SRC_DIR)/protocol/nvme` 已在 Makefile 中创建（第104行），无需额外修改。

## 二、main.c 修改

### 2.1 添加头文件包含

在 `main.c` 顶部添加：

```c
#include "protocol/vhost_user_nvme.h"
```

### 2.2 添加全局配置变量

在全局变量区域添加：

```c
static bool g_vhost_user_enable = false;
static char g_vhost_socket_path[256] = VHOST_USER_NVME_DEFAULT_SOCKET;
```

### 2.3 命令行参数解析

在 `main()` 函数的参数解析循环中添加：

```c
else if (strcmp(argv[i], "--vhost-user") == 0) {
    g_vhost_user_enable = true;
}
else if (strncmp(argv[i], "--vhost-socket=", 15) == 0) {
    strncpy(g_vhost_socket_path, argv[i] + 15, sizeof(g_vhost_socket_path) - 1);
}
```

同时在帮助信息中添加：

```c
printf("  --vhost-user              启用 vhost-user NVMe 后端模式\n");
printf("  --vhost-socket=<path>     指定 vhost-user Unix socket 路径\n");
```

### 2.4 在 init_all_modules() 中初始化

在 NVMe 控制器初始化（`nvme_ctrl_init()`）之后添加：

```c
if (g_vhost_user_enable) {
    vhost_user_nvme_config_t vu_config;
    memset(&vu_config, 0, sizeof(vu_config));
    strncpy(vu_config.socket_path, g_vhost_socket_path, sizeof(vu_config.socket_path) - 1);
    vu_config.max_queues = 2;  /* Admin + 1 I/O 队列 */
    if (vhost_user_nvme_init(&vu_config) != RET_OK) {
        LOG_ERROR("vhost-user NVMe 后端初始化失败");
        return RET_ERR_INTERNAL;
    }
}
```

### 2.5 在主循环中调用 process

在主循环（`while (!g_should_exit)`）中添加：

```c
if (g_vhost_user_enable) {
    vhost_user_nvme_process();
}
```

建议与 `nvme_tcp_target_process()` 放在同一层级，每次循环调用一次。

### 2.6 在 deinit_all_modules() 中反初始化

在 `nvme_ctrl_deinit()` 之前或之后添加：

```c
if (g_vhost_user_enable) {
    vhost_user_nvme_deinit();
}
```

## 三、编译验证

### 3.1 单文件编译验证

```bash
cd /mnt/hgfs/Code/ftl-firmware
gcc -std=c99 -Wall -Wextra -I include -I modules/nand -I modules/ftl \
    -I modules/log -I modules/host_if -I modules/manager -I modules/thread \
    -I modules/dma -I modules/raid -I ipc -I utils \
    -c src/protocol/nvme/vhost_user_nvme.c -o /tmp/vhost_user_nvme.o
```

### 3.2 完整项目编译

```bash
cd /mnt/hgfs/Code/ftl-firmware
make clean && make
```

## 四、QEMU 启动命令

### 4.1 启动固件（vhost-user 模式）

```bash
/tmp/ftl-firmware-build/ftl_firmware --vhost-user \
    --vhost-socket=/tmp/ftl-vhost-user.sock
```

固件启动后会监听 `/tmp/ftl-vhost-user.sock`，等待 QEMU 连接。

### 4.2 启动 QEMU 虚拟机

```bash
qemu-system-x86_64 \
    -machine q35,accel=kvm \
    -cpu host \
    -m 2048 \
    -smp 2 \
    -drive file=/path/to/guest.img,format=qcow2 \
    -chardev socket,id=vu0,path=/tmp/ftl-vhost-user.sock \
    -device vhost-user-nvme,chardev=vu0,num-queues=2 \
    -net nic -net user,hostfwd=tcp::2222-:22 \
    -nographic
```

**参数说明：**
- `-chardev socket,id=vu0,path=...`：创建指向 vhost-user socket 的字符设备
- `-device vhost-user-nvme,chardev=vu0,num-queues=2`：创建 vhost-user NVMe 设备，2个队列（Admin + 1 I/O）
- QEMU 8.0+ 版本支持 `vhost-user-nvme` 设备

### 4.3 多队列配置

如需更多 I/O 队列：

```bash
# 固件端
/tmp/ftl-firmware-build/ftl_firmware --vhost-user --vhost-socket=/tmp/ftl-vhost-user.sock
# （max_queues 在代码中配置，默认2，可修改为4或更多）

# QEMU 端
-device vhost-user-nvme,chardev=vu0,num-queues=4
```

## 五、虚拟机内验证

### 5.1 识别 NVMe 设备

```bash
# 查看 NVMe 设备列表
nvme list

# 预期输出：
# Node             SN                  Model               Namespace  Usage
# /dev/nvme0n1     ...                 ftl-firmware        1          ...
```

### 5.2 查看设备信息

```bash
# 查看控制器信息
nvme id-ctrl /dev/nvme0

# 查看命名空间信息
nvme id-ns /dev/nvme0n1

# 查看 SMART 日志
nvme smart-log /dev/nvme0
```

### 5.3 写入测试

```bash
# 写入 400KB 数据（100个4K块）
dd if=/dev/zero of=/dev/nvme0n1 bs=4k count=100 oflag=direct

# 写入随机数据
dd if=/dev/urandom of=/dev/nvme0n1 bs=4k count=100 oflag=direct
```

### 5.4 读取验证

```bash
# 读取并校验
dd if=/dev/nvme0n1 of=/tmp/test_read.bin bs=4k count=100 iflag=direct

# 对比写入和读取的数据（如果之前写入的是已知模式）
md5sum /tmp/test_write.bin /tmp/test_read.bin
```

### 5.5 性能测试

```bash
# 使用 fio 进行随机读测试
fio --name=randread --ioengine=libaio --direct=1 --rw=randread \
    --bs=4k --numjobs=1 --iodepth=32 --runtime=30 \
    --filename=/dev/nvme0n1 --group_reporting

# 顺序写测试
fio --name=seqwrite --ioengine=libaio --direct=1 --rw=write \
    --bs=128k --numjobs=1 --iodepth=32 --runtime=30 \
    --filename=/dev/nvme0n1 --group_reporting
```

### 5.6 TRIM 测试

```bash
# 查看设备是否支持 TRIM
nvme id-ns /dev/nvme0n1 | grep -i per

# 执行 TRIM
blkdiscard /dev/nvme0n1
```

## 六、协议握手流程

vhost-user 协议握手顺序：

```
QEMU (前端)                          固件 (后端)
    |                                    |
    |---- GET_FEATURES ----------------->|
    |<--- features (bit30) --------------|
    |---- SET_FEATURES ----------------->|
    |---- GET_PROTOCOL_FEATURES -------->|
    |<--- protocol_features -------------|
    |---- SET_PROTOCOL_FEATURES -------->|
    |---- SET_OWNER -------------------->|
    |---- SET_MEM_TABLE (+FDs) --------->|  mmap 所有内存区域
    |---- SET_VRING_NUM ---------------->|
    |---- SET_VRING_ADDR --------------->|
    |---- SET_VRING_BASE --------------->|
    |---- SET_VRING_KICK (+eventfd) ---->|
    |---- SET_VRING_CALL (+eventfd) ---->|
    |---- SET_VRING_ENABLE ------------->|
    |---- SET_CONFIG (CC.EN=1) --------->|  CSTS.RDY=1
    |                                    |
    |<=== 运行时数据传输 ===>           |
    |---- SET_CONFIG (doorbell) -------->|
    |                                    |  从 vring 取命令
    |                                    |  处理命令 + PRP 数据传输
    |                                    |  写 used ring
    |<--- call eventfd (中断) -----------|
```

## 七、架构设计说明

### 7.1 vring 缓冲区布局

每个 available ring 描述符指向一块 80 字节缓冲区：

```
偏移 0-63:   nvme_command_t (64字节，QEMU写入，固件读取)
偏移 64-79:  nvme_completion_t (16字节，固件写入，QEMU读取)
```

数据传输通过命令中的 `dptr_prp1` / `dptr_prp2` 指针指向 Guest 物理内存，固件通过 `SET_MEM_TABLE` 建立的 GPA→HVA 映射访问。

### 7.2 命令处理路径

- **Admin 命令**（vring0）：
  - Identify / Get Log Page：调用 `nvme_ctrl_fill_*` 填充数据，通过 PRP 写入共享内存
  - 其他：调用 `nvme_ctrl_process_admin_cmd()`

- **I/O 命令**（vring1+）：
  - Read：调用 `ftl_read()` 读取数据，通过 PRP 写入共享内存
  - Write：通过 PRP 从共享内存读取数据，调用 `ftl_write()`
  - Write Zeroes：调用 `ftl_write()` 写零
  - Dataset Mgmt (TRIM)：通过 PRP 读取 range 列表，调用 `ftl_trim()`
  - 其他：调用 `nvme_ctrl_process_io_cmd()`

### 7.3 中断机制

命令完成后，固件向 `call eventfd` 写入 1，QEMU 检测到 eventfd 可读后触发虚拟机中断，Guest 驱动从 used ring 读取完成条目。

## 八、注意事项

1. **QEMU 版本**：需要 QEMU 8.0+ 以支持 `vhost-user-nvme` 设备
2. **权限**：运行固件的用户需要有权限创建和访问 `/tmp/ftl-vhost-user.sock`
3. **大页内存**：如果 QEMU 使用大页内存（`-mem-path`），vhost-user 后端需要能 mmap 大页文件
4. **SELinux/AppArmor**：可能需要配置安全策略允许 QEMU 访问 socket
5. **调试**：可设置 `DEBUG` 宏查看详细协议日志，固件会输出每条 vhost-user 消息的处理信息
6. **单连接**：当前实现仅支持一个 QEMU 连接，新连接会被拒绝
