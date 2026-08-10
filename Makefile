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
CFLAGS += -I ipc
CFLAGS += -I utils
CFLAGS += -DDEBUG

LDFLAGS = -lpthread

# 目录配置
SRC_DIR     = src
MODULES_DIR = modules
IPC_DIR     = ipc
UTILS_DIR   = utils
BUILD_DIR   = build
TEST_DIR    = tests

# 源文件
CORE_SRCS = $(SRC_DIR)/main.c \
            $(MODULES_DIR)/nand/nand.c \
            $(MODULES_DIR)/ftl/ftl.c \
            $(MODULES_DIR)/log/log.c \
            $(MODULES_DIR)/host_if/host_if.c \
            $(MODULES_DIR)/manager/manager.c \
            $(MODULES_DIR)/thread/thread.c \
            $(MODULES_DIR)/dma/dma.c \
            $(IPC_DIR)/msg_queue.c \
            $(UTILS_DIR)/utils.c

# 目标文件
CORE_OBJS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(CORE_SRCS))

# 目标
TARGET = $(BUILD_DIR)/ftl_firmware

# 测试目标
TEST_TARGETS =

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
	@mkdir -p $(BUILD_DIR)/$(IPC_DIR)
	@mkdir -p $(BUILD_DIR)/$(UTILS_DIR)

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
