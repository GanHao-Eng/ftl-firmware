# ============================================================
#  FTL 固件 Makefile
# ============================================================

# 编译器配置
CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes -g
CFLAGS += -I include
CFLAGS += -I modules/nand
CFLAGS += -I modules/ftl
CFLAGS += -I modules/log
CFLAGS += -I modules/host_if
CFLAGS += -I modules/manager
CFLAGS += -I modules/thread
CFLAGS += -I modules/dma
CFLAGS += -I modules/raid
CFLAGS += -I ipc
CFLAGS += -I utils
CFLAGS += -DDEBUG

LDFLAGS = -lpthread

# 目录配置
SRC_DIR     = src
MODULES_DIR = modules
IPC_DIR     = ipc
UTILS_DIR   = utils
BUILD_DIR   = /tmp/ftl-firmware-build
TEST_DIR    = tests

# 源文件
CORE_SRCS = $(SRC_DIR)/main.c \
            $(SRC_DIR)/protocol/nvme/nvme_controller.c \
            $(SRC_DIR)/protocol/nvme/nvme_tcp_target.c \
            $(SRC_DIR)/protocol/ufs/ufs_target.c \
            $(SRC_DIR)/hal/os_linux.c \
            $(MODULES_DIR)/nand/nand.c \
            $(MODULES_DIR)/ftl/ftl.c \
            $(MODULES_DIR)/log/log.c \
            $(MODULES_DIR)/host_if/host_if.c \
            $(MODULES_DIR)/manager/manager.c \
            $(MODULES_DIR)/thread/thread.c \
            $(MODULES_DIR)/dma/dma.c \
            $(MODULES_DIR)/raid/raid.c \
            $(IPC_DIR)/msg_queue.c \
            $(UTILS_DIR)/utils.c

# 目标文件
CORE_OBJS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(CORE_SRCS))

# 目标
TARGET = $(BUILD_DIR)/ftl_firmware

# 测试目标
TEST_TARGETS = $(BUILD_DIR)/test_gc_benchmark $(BUILD_DIR)/test_ftl_unit $(BUILD_DIR)/test_plp_recovery

# GC 基准测试源文件（不含 main.c）
GC_BENCH_SRCS = $(TEST_DIR)/test_gc_benchmark.c \
                $(MODULES_DIR)/nand/nand.c \
                $(MODULES_DIR)/ftl/ftl.c \
                $(MODULES_DIR)/log/log.c
GC_BENCH_OBJS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(GC_BENCH_SRCS))

# FTL 单元测试源文件
FTL_UNIT_SRCS = $(TEST_DIR)/test_ftl_unit.c \
                $(MODULES_DIR)/nand/nand.c \
                $(MODULES_DIR)/ftl/ftl.c \
                $(MODULES_DIR)/log/log.c
FTL_UNIT_OBJS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(FTL_UNIT_SRCS))

# PLP 掉电恢复测试源文件
PLP_RECOVERY_SRCS = $(TEST_DIR)/test_plp_recovery.c \
                    $(MODULES_DIR)/nand/nand.c \
                    $(MODULES_DIR)/ftl/ftl.c \
                    $(MODULES_DIR)/log/log.c
PLP_RECOVERY_OBJS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(PLP_RECOVERY_SRCS))

# 默认目标
all: $(BUILD_DIR) $(TARGET)

# 创建构建目录
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(BUILD_DIR)/$(SRC_DIR)
	@mkdir -p $(BUILD_DIR)/$(MODULES_DIR)/nand
	@mkdir -p $(BUILD_DIR)/$(MODULES_DIR)/ftl
	@mkdir -p $(BUILD_DIR)/$(MODULES_DIR)/log
	@mkdir -p $(BUILD_DIR)/$(MODULES_DIR)/host_if
	@mkdir -p $(BUILD_DIR)/$(MODULES_DIR)/manager
	@mkdir -p $(BUILD_DIR)/$(MODULES_DIR)/thread
	@mkdir -p $(BUILD_DIR)/$(MODULES_DIR)/dma
	@mkdir -p $(BUILD_DIR)/$(MODULES_DIR)/raid
	@mkdir -p $(BUILD_DIR)/$(IPC_DIR)
	@mkdir -p $(BUILD_DIR)/$(UTILS_DIR)
	@mkdir -p $(BUILD_DIR)/$(TEST_DIR)
	@mkdir -p $(BUILD_DIR)/$(SRC_DIR)/protocol/nvme
	@mkdir -p $(BUILD_DIR)/$(SRC_DIR)/protocol/ufs
	@mkdir -p $(BUILD_DIR)/$(SRC_DIR)/hal
	@mkdir -p $(BUILD_DIR)/$(SRC_DIR)/protocol/ufs
	@mkdir -p $(BUILD_DIR)/$(SRC_DIR)/hal

# 编译目标文件
$(BUILD_DIR)/%.o: %.c
	@echo "  CC  $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# 链接目标
$(TARGET): $(CORE_OBJS)
	@echo "  LD  $@"
	@$(CC) $(CORE_OBJS) -o $@ $(LDFLAGS)
	@echo ""
	@echo "构建完成: $@"
	@echo ""

# GC 基准测试
$(BUILD_DIR)/test_gc_benchmark: $(GC_BENCH_OBJS)
	@echo "  LD  $@"
	@$(CC) $(GC_BENCH_OBJS) -o $@ $(LDFLAGS)
	@echo ""
	@echo "构建完成: $@"
	@echo ""

# FTL 单元测试
$(BUILD_DIR)/test_ftl_unit: $(FTL_UNIT_OBJS)
	@echo "  LD  $@"
	@$(CC) $(FTL_UNIT_OBJS) -o $@ $(LDFLAGS)
	@echo ""
	@echo "构建完成: $@"
	@echo ""

# PLP 掉电恢复测试
$(BUILD_DIR)/test_plp_recovery: $(PLP_RECOVERY_OBJS)
	@echo "  LD  $@"
	@$(CC) $(PLP_RECOVERY_OBJS) -o $@ $(LDFLAGS)
	@echo ""
	@echo "构建完成: $@"
	@echo ""

# 测试目标
test: $(BUILD_DIR) $(TEST_TARGETS)

# 运行测试
runtest: test
	@echo "运行测试..."
	@for test in $(TEST_TARGETS); do \
		echo ""; \
		echo "========================================"; \
		echo "运行 $$test"; \
		echo "========================================"; \
		./$$test; \
	done

# 清理构建产物
clean:
	@echo "清理构建产物..."
	@rm -rf $(BUILD_DIR)
	@rm -f nand_disk.bin
	@rm -f ftl_wal.log
	@echo "清理完成"

# 打印帮助信息
help:
	@echo "FTL 固件构建系统"
	@echo ""
	@echo "用法:"
	@echo "  make all       - 构建固件（默认）"
	@echo "  make test      - 构建测试"
	@echo "  make runtest   - 运行测试"
	@echo "  make clean     - 清理构建产物"
	@echo "  make help      - 显示帮助信息"
	@echo ""
	@echo "目标:"
	@echo "  $(TARGET)"
	@echo ""

# 伪目标
.PHONY: all test runtest clean help
