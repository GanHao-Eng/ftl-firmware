#!/bin/bash
# 从源码编译 QEMU，启用 vfio-user 和 vhost-user-nvme 支持
# 在 Linux VM 中执行，预计需要 30-60 分钟

set -e

WORK_DIR=/mnt/hgfs/Code/ftl-firmware
QEMU_DIR=/tmp/qemu-build
QEMU_VERSION=v8.2.2

echo "=========================================="
echo "  从源码编译 QEMU（启用 vfio-user）"
echo "=========================================="

# 1. 安装编译依赖
echo ""
echo "[1/5] 安装编译依赖..."
sudo apt-get update
sudo apt-get install -y \
    git build-essential pkg-config \
    libglib2.0-dev libpixman-1-dev \
    libfdt-dev libslirp-dev \
    libgtk-3-dev libsdl2-dev \
    libaio-dev libnfs-dev \
    python3 python3-pip \
    ninja-build meson \
    libcap-dev libattr1-dev \
    liburing-dev

echo "  依赖安装完成"

# 2. 下载 QEMU 源码
echo ""
echo "[2/5] 下载 QEMU 源码..."
if [ ! -d $QEMU_DIR ]; then
    git clone --depth 1 --branch $QEMU_VERSION \
        https://gitlab.com/qemu-project/qemu.git \
        $QEMU_DIR
    echo "  源码下载完成"
else
    echo "  源码目录已存在，跳过下载"
fi

cd $QEMU_DIR

# 3. 配置（启用 vfio-user 和 vhost-user）
echo ""
echo "[3/5] 配置 QEMU..."
mkdir -p build
cd build

../configure \
    --target-list=x86_64-softmmu \
    --enable-vfio-user \
    --enable-vhost-user \
    --enable-vhost-kernel \
    --enable-kvm \
    --enable-slirp \
    --enable-linux-aio \
    --enable-attr \
    --enable-cap-ng \
    --disable-docs \
    --disable-werror

echo "  配置完成"

# 4. 编译
echo ""
echo "[4/5] 编译 QEMU（这可能需要 30-60 分钟）..."
make -j$(nproc)

echo "  编译完成"

# 5. 验证
echo ""
echo "[5/5] 验证编译结果..."
./qemu-system-x86_64 --version

echo ""
echo "  检查 vfio-user-pci 设备支持..."
./qemu-system-x86_64 -device help 2>&1 | grep -i "vfio-user"

echo ""
echo "  检查 vhost-user-nvme 设备支持..."
./qemu-system-x86_64 -device help 2>&1 | grep -i "vhost-user-nvme"

echo ""
echo "=========================================="
echo "  QEMU 编译完成！"
echo "=========================================="
echo ""
echo "编译好的 QEMU 路径："
echo "  $QEMU_DIR/build/qemu-system-x86_64"
echo ""
echo "使用方法："
echo "  终端1: 启动固件"
echo "    cd $WORK_DIR"
echo "    rm -f /tmp/ftl-vfio-user.sock"
echo "    /tmp/ftl-firmware-build/ftl_firmware --vfio-user --vfio-socket=/tmp/ftl-vfio-user.sock"
echo ""
echo "  终端2: 启动 QEMU（使用编译好的版本）"
echo "    cd $WORK_DIR"
echo "    $QEMU_DIR/build/qemu-system-x86_64 \\"
echo "      -m 2G -smp 2 \\"
echo "      -cdrom alpine-virt-3.19.0-x86_64.iso \\"
echo "      -boot d \\"
echo "      -chardev socket,id=vfio0,path=/tmp/ftl-vfio-user.sock \\"
echo "      -device vfio-user-pci,chardev=vfio0 \\"
echo "      -nographic"
