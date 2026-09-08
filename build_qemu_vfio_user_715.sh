#!/bin/bash
# 使用 Oracle vfio-user-7.1.5 分支编译 QEMU
# 这是 Oracle 仓库中最新的 vfio-user 分支

set -e

QEMU_DIR=/tmp/qemu-vfio-user
QEMU_BRANCH=vfio-user-7.1.5

echo "=========================================="
echo "  编译 QEMU（Oracle vfio-user-7.1.5 分支）"
echo "=========================================="

# 1. 清理旧目录，克隆新分支
echo ""
echo "[1/4] 克隆 Oracle vfio-user-7.1.5 分支..."
rm -rf $QEMU_DIR
git clone --depth 1 --branch $QEMU_BRANCH \
    https://github.com/oracle/qemu.git \
    $QEMU_DIR
echo "  源码克隆完成"

cd $QEMU_DIR

# 2. 检查 vfio-user 配置选项
echo ""
echo "[2/4] 检查 vfio-user 配置选项..."
./configure --help 2>&1 | grep -i vfio || echo "  （无 vfio 选项，可能默认启用）"

# 3. 配置并编译
echo ""
echo "[3/4] 配置并编译 QEMU..."
mkdir -p build
cd build

# 尝试启用 vfio-user
../configure \
    --target-list=x86_64-softmmu \
    --enable-vfio-user \
    --enable-vhost-user \
    --disable-docs \
    --disable-werror 2>&1 | tail -20

echo ""
echo "  配置完成，开始编译（预计 30-60 分钟）..."
make -j$(nproc)

echo "  编译完成"

# 4. 验证
echo ""
echo "[4/4] 验证编译结果..."
./qemu-system-x86_64 --version

echo ""
echo "  检查 vfio-user-pci 设备支持..."
./qemu-system-x86_64 -device help 2>&1 | grep -i "vfio-user"

echo ""
echo "  检查 vhost-user-nvme 设备支持..."
./qemu-system-x86_64 -device help 2>&1 | grep -i "vhost-user-nvme"

echo ""
echo "=========================================="
echo "  编译完成！"
echo "=========================================="
echo ""
echo "QEMU 路径：$QEMU_DIR/build/qemu-system-x86_64"
echo ""
echo "测试步骤："
echo "  终端1: 启动固件"
echo "    cd /mnt/hgfs/Code/ftl-firmware"
echo "    rm -f /tmp/ftl-vfio-user.sock"
echo "    /tmp/ftl-firmware-build/ftl_firmware --vfio-user --vfio-socket=/tmp/ftl-vfio-user.sock"
echo ""
echo "  终端2: 启动 QEMU"
echo "    cd /mnt/hgfs/Code/ftl-firmware"
echo "    $QEMU_DIR/build/qemu-system-x86_64 \\"
echo "      -m 2G -smp 2 \\"
echo "      -cdrom alpine-virt-3.19.0-x86_64.iso \\"
echo "      -boot d \\"
echo "      -chardev socket,id=vfio0,path=/tmp/ftl-vfio-user.sock \\"
echo "      -device vfio-user-pci,chardev=vfio0 \\"
echo "      -nographic"
