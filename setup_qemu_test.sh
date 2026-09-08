#!/bin/bash
# QEMU vfio-user NVMe 测试环境准备脚本
# 在 Linux VM 中执行

set -e

WORK_DIR=/mnt/hgfs/Code/ftl-firmware
cd $WORK_DIR

echo "=========================================="
echo "  QEMU vfio-user NVMe 测试环境准备"
echo "=========================================="

# 1. 检查 qemu 是否安装
echo ""
echo "[1/4] 检查 QEMU 安装..."
if ! command -v qemu-system-x86_64 &> /dev/null; then
    echo "  QEMU 未安装，正在安装..."
    sudo apt-get update
    sudo apt-get install -y qemu-system-x86 qemu-utils
else
    echo "  QEMU 已安装: $(qemu-system-x86_64 --version | head -1)"
fi

# 2. 创建虚拟机磁盘镜像
echo ""
echo "[2/4] 创建虚拟机磁盘镜像..."
if [ ! -f guest.img ]; then
    qemu-img create -f qcow2 guest.img 10G
    echo "  已创建 guest.img (10G qcow2)"
else
    echo "  guest.img 已存在，跳过创建"
fi

# 3. 下载 Alpine Linux ISO（轻量级，适合测试，约150MB）
echo ""
echo "[3/4] 下载 Alpine Linux ISO..."
ALPINE_ISO=alpine-virt-3.19.0-x86_64.iso
if [ ! -f $ALPINE_ISO ]; then
    echo "  正在下载 Alpine Linux 3.19.0 (约150MB)..."
    wget -q --show-progress \
        https://dl-cdn.alpinelinux.org/alpine/v3.19/releases/x86_64/$ALPINE_ISO \
        -O $ALPINE_ISO
    echo "  下载完成"
else
    echo "  $ALPINE_ISO 已存在，跳过下载"
fi

# 4. 编译固件
echo ""
echo "[4/4] 编译 ftl-firmware..."
make clean > /dev/null 2>&1
make -j$(nproc)
echo "  编译完成: /tmp/ftl-firmware-build/ftl_firmware"

echo ""
echo "=========================================="
echo "  环境准备完成！"
echo "=========================================="
echo ""
echo "下一步操作："
echo "  终端1: 启动固件（vfio-user 模式）"
echo "    cd /mnt/hgfs/Code/ftl-firmware"
echo "    rm -f /tmp/ftl-vfio-user.sock"
echo "    /tmp/ftl-firmware-build/ftl_firmware --vfio-user --vfio-socket=/tmp/ftl-vfio-user.sock"
echo ""
echo "  终端2: 启动 QEMU（安装 Alpine）"
echo "    cd /mnt/hgfs/Code/ftl-firmware"
echo "    qemu-system-x86_64 \\"
echo "      -m 2G -smp 2 \\"
echo "      -drive file=guest.img,format=qcow2 \\"
echo "      -cdrom alpine-virt-3.19.0-x86_64.iso \\"
echo "      -boot d \\"
echo "      -chardev socket,id=vfio0,path=/tmp/ftl-vfio-user.sock \\"
echo "      -device vfio-user-pci,chardev=vfio0 \\"
echo "      -nographic"
echo ""
echo "  安装完成后，移除 -cdrom 和 -boot d，直接从硬盘启动"
