# ============================================================================
# AIC8800 WiFi Driver - 顶层 Makefile (双模式: Standalone + SDK Integration)
# ============================================================================
# Standalone 编译 (SDKROOT 未定义):
#   make                                    # 编译 platform
#   make AIC_BUILD_HOST=1                   # 强制从 host 源码编译驱动
#   make clean / make info
#
# SDK 集成编译 (SDKROOT 已定义, 由 SDK 顶层 Makefile 通过 LIBDIRS 调用):
#   自动使用 SDK 的 CC/CFLAGS/INCFLAGS, 编译 platform 层源文件到 $(OUT)/lib/
# ============================================================================

# --- SDK 集成模式: 由 SDK 顶层构建调用 ---
ifdef SDKROOT
  include $(SDKROOT)/build/config.mk

  LIBNAME := aic8800

  # 宏定义
  # 注意: LWIP_DHCPS / LWIP_NETIF_HOSTNAME 必须由平台 lwipopts.h 定义, 勿在此用 -D 注入
  # (SDK lwIP 库不带该宏编译 → struct netif 布局与驱动库错位 → DHCP ACD 阶段野指针 HardFault;
  #  平台未开 LWIP_DHCPS 时驱动带 -D 还会产生 dhcpd_start undefined reference)
  LOCAL_CFLAGS += \
      -DCONFIG_AIC8800D80 -DUSE_5G -DCONFIG_USB_SUPPORT \
      -DCONFIG_AICWF_BUS_USB -DAIC_BUS_USB \
      -DCONFIG_NO_WPA_MSG -DRT_THREAD \
      -DCONFIG_LWIP -DPLATFORM_NO_FILE -DSYS_SOCK_DEFINE \
      -DHAVE_SIGVAL -DHAVE_SIGEVENT -DHAVE_SIGINFO \
      -DRT_VERSION_CHECK\(maj,min,pat\)=\(\(\(maj\)*10000\)+\(\(min\)*100\)+\(pat\)\)

  # 头文件路径（使用相对路径, lib.mk 会基于 CUR_PATH 自动扩充）
  LOCAL_CFLAGS += \
      -Iport/inc \
      -Ihost/driver/drv_api/inc \
      -Ihost/driver/drv_wifi \
      -Ihost/driver/drv_wlan \
      -Ihost/driver/drv_macif \
      -Ihost/driver/drv_fhost \
      -Ihost/driver/utils/inc \
      -Ihost/driver/layer_common/inc \
      -Iplatform/fmac -Iplatform/fmac/fw \
      -Iplatform/hal -Iplatform/netif \
      -Iplatform/rtos -Iplatform/wifi \
      -Iplatform/iperf -Iplatform/uapapi/inc \
      -I../usb/include -I../usb/host

  # P2P: 由 SDK 侧 AIC_FEATURE_P2P 控制 (0=关闭, 未定义默认开启, 与 aic_cfg.mk 一致)
  ifneq ($(AIC_FEATURE_P2P),0)
  LOCAL_CFLAGS += -DAIC_FEATURE_P2P -DCONFIG_P2P_SUPPORT
  endif

  # Platform 层源文件（使用相对路径, lib.mk 会基于 CUR_PATH 自动扩充）
  CSRCS := \
      platform/platform_init.c \
      platform/hal/plat_port.c \
      platform/hal/usb_port.c \
      platform/netif/netif_port.c \
      platform/rtos/rtos_rtt.c \
      platform/fmac/aic_fw_port.c \
      platform/fmac/aic_chip_desc_port.c \
      platform/fmac/userconfig.c \
      platform/wifi/wifi_port.c \
      platform/wifi/wifi_port_sta.c \
      platform/wifi/wifi_port_ap.c \
      platform/wifi/wifi_port_wps.c \
      platform/wifi/wifi_port_cmd.c \
      platform/bt/aic_hci.c \
      platform/bt/aic_bt_sdio.c \
      platform/bt/aic_btusb.c

  include $(SDKROOT)/build/lib.mk

# ============================================================================
# Standalone 模式
# ============================================================================
else

# --- Windows 下强制使用 Git Bash sh (cp/python3/mkdir -p 等 Unix 命令) ---
# 设置 SHELL 后, 下方平台检测的 $(shell uname -s) 也走 sh, 自动进入 Git Bash 分支
ifeq ($(OS),Windows_NT)
  ifneq (,$(wildcard C:/Program Files/Git/usr/bin/sh.exe))
    SHELL := C:/Program Files/Git/usr/bin/sh.exe
  else ifneq (,$(wildcard C:/Program Files/Git/bin/sh.exe))
    SHELL := C:/Program Files/Git/bin/sh.exe
  else ifneq (,$(wildcard C:/Progra~1/Git/usr/bin/sh.exe))
    SHELL := C:/Progra~1/Git/usr/bin/sh.exe
  endif
endif

# --- 平台兼容性处理 ---
ifeq ($(OS),Windows_NT)
    UNAME_S := $(shell uname -s 2>/dev/null)  # 用 /dev/null 而非 NUL: Git Bash 下 2>NUL 会创建字面 NUL 文件
    ifeq ($(UNAME_S),)
        MKDIR = if not exist "$(subst /,\,$1)" mkdir "$(subst /,\,$1)"
        RM_RF = if exist "$(subst /,\,$1)" rmdir /s /q "$(subst /,\,$1)"
    else
        MKDIR = mkdir -p $1
        RM_RF = rm -rf $1
    endif
else
    MKDIR = mkdir -p $1
    RM_RF = rm -rf $1
endif

# --- 目录定义 ---
PACKAGE_PATH     := $(CURDIR)
PLATFORM_PATH    := $(PACKAGE_PATH)/platform
HOST_PATH        := $(PACKAGE_PATH)/host
BUILD_DIR        := $(PACKAGE_PATH)/build

# 发布/diff 交付目录: 收敛到 dist/ (已 gitignore), 不再生成源码同级 *_release/
AIC_DIST_DIR   ?= $(PACKAGE_PATH)/dist
RELEASE_PATH := $(AIC_DIST_DIR)/$(notdir $(PACKAGE_PATH))_release
# 注意: 交付头单一事实源 = lib/include/ (copy_release_headers.py 白名单)。
# 历史遗留的顶层 include/ 副本已移除 — 编译路径 (INC_DRIVER/NimBLE) 只引用 lib/include/。

TARGET := $(BUILD_DIR)/libaic_platform.a

# --- 自动检测 host 编译模式 ---
ifndef AIC_BUILD_HOST
  HAS_HOST_DIR := $(wildcard $(HOST_PATH))
  ifneq (,$(HAS_HOST_DIR))
    AIC_BUILD_HOST := 1
  else
    AIC_BUILD_HOST := 0
  endif
endif

ifeq ($(AIC_BUILD_HOST),1)
  DFLAGS += -DAIC_BUILD_HOST_FROM_SOURCE
endif

# --- 外部 SDK 头文件 ---
EXT_INC_DIRS ?=

# --- 驱动头文件 (注意: 延迟求值, 在 platform_config.mk include 之后) ---
INC_DRIVER =
INC_PLATFORM =

# --- Platform 基础源文件 ---
SRC_PLATFORM := $(PLATFORM_PATH)/platform_init.c

# --- 平台配置 (必须在 INC_* 计算之前) ---
# 默认使用 platform_config.mk；可通过 PLATFORM_CONFIG 变量指定其他配置文件，
# 例如: make PLATFORM_CONFIG=platform_config_mips-gcc540-elf.mk ...
# 注意: 用显式 include + 存在性检查, 拼错文件名立即报错并列出可用配置,
#       避免 -include 静默忽略导致回退宿主机 gcc 编译
PLATFORM_CONFIG ?= platform_config.mk
ifeq (,$(wildcard $(PACKAGE_PATH)/$(PLATFORM_CONFIG)))
$(error PLATFORM_CONFIG=$(PLATFORM_CONFIG) 不存在! 可用配置: $(notdir $(wildcard $(PACKAGE_PATH)/platform_config*.mk)))
endif
include $(PACKAGE_PATH)/$(PLATFORM_CONFIG)
include $(PACKAGE_PATH)/aic_cfg.mk

# --- mbedTLS port 层开关 (host 子构建消费) ---
# 1 = host 自带 TLS port 层 (tls_client/tls_certificate/tls_hardware), 0 = 平台自实现。
# 与 host/Makefile 的 ?= 1 默认保持一致, 且必须显式透传给子 make:
# platform_config.mk 中的赋值不随 MAKEFLAGS 传播, 不传会让 lib-sync 与
# check-src-coverage 看到不同源清单 (实测归档 177 成员 vs 对账 174);
# 未定义时传空值又会覆盖 host 的 ?= 1 默认, 故在此统一定义。
AIC_BUILD_MBEDTLS_PORT ?= 1

# 单库统一: 所有芯片型号共用同一个驱动库 (libaic8800_driver.a),
# host 源码不含型号宏, 型号差异由 aic_chip_desc 数据表运行时分发。

# --- 头文件路径 (platform_config.mk / aic_cfg.mk 之后, EXT_INC_DIRS / AIC_BUS_TYPE 已确定) ---
# 调试与发布统一引用 lib/include/ 公开头 + port/inc 移植接口:
#   - AIC_BUILD_HOST=1: 根 lib/ 由 host lib-sync 生成 (lib/*.a + lib/include/)
#   - AIC_BUILD_HOST=0: 驱动目录内 lib/ 来自 make release 自包含输出
# 统一形态保证平台适配代码只依赖公开 API, 杜绝调试走 host 内部头、
# 发布缺头导致的 diff 包不完整 (见 docs/5-平台移植Diff包说明.md)
INC_DRIVER := \
    $(PACKAGE_PATH)/lib/include/drv_api/inc \
    $(PACKAGE_PATH)/lib/include/drv_wifi \
    $(PACKAGE_PATH)/port/inc \
    $(PACKAGE_PATH)/lib/include/utils/inc

INC_PLATFORM := \
    $(PLATFORM_PATH)/fmac \
    $(PLATFORM_PATH)/fmac/fw \
    $(PLATFORM_PATH)/hal \
    $(PLATFORM_PATH)/netif \
    $(PLATFORM_PATH)/rtos \
    $(PLATFORM_PATH)/wifi \
    $(PLATFORM_PATH)/iperf \
    $(PLATFORM_PATH)/uapapi/inc

# INC_DRIVER (含 port/inc/) 优先于 EXT_INC_DIRS, 确保 port/inc/lwipopts.h 优先
INC_ALL = $(INC_DRIVER) $(INC_PLATFORM) $(EXT_INC_DIRS)
CFLAGS_INC = $(addprefix -I,$(INC_ALL))

# --- 总线平台源文件 (根据 AIC_BUS_TYPE 选择) ---
ifeq ($(AIC_BUS_TYPE),USB)
SRC_PLATFORM += $(PLATFORM_PATH)/hal/usb_port.c
else
SRC_PLATFORM += $(PLATFORM_PATH)/hal/sdio_port.c
endif

# --- mbedTLS crypto 外部实现 (USE_BUILTIN_MBEDTLS=0) ---
# 默认编入转发示例 (标准 mbedtls API 去 aic_ 前缀直接转发):
#   A. 平台已有标准 mbedtls: 直接可用, SDK 链接平台 mbedtls 库
#   B. 硬件加速/自主实现: 设 AIC_MBEDTLS_FWD_EXAMPLE=0 关闭本示例,
#      SRC_PLATFORM += 自建 crypto port 源文件
ifeq ($(USE_BUILTIN_MBEDTLS),0)
ifneq ($(AIC_MBEDTLS_FWD_EXAMPLE),0)
SRC_PLATFORM += $(PLATFORM_PATH)/crypto/aic_mbedtls_port_example.c
endif
endif

# --- 网络接口源文件 ---
SRC_PLATFORM += $(PLATFORM_PATH)/netif/netif_port.c

# --- 功能裁剪: P2P / BT ---
ifeq ($(AIC_FEATURE_P2P),1)
SRC_PLATFORM += $(PLATFORM_PATH)/wifi/wifi_port_p2p.c
endif
ifeq ($(AIC_FEATURE_BT),1)
SRC_PLATFORM += $(PLATFORM_PATH)/bt/aic_hci.c
ifeq ($(AIC_HCI_TRANSPORT),uart)
SRC_PLATFORM += $(PLATFORM_PATH)/bt/aic_bt_uart.c
else ifeq ($(AIC_HCI_TRANSPORT),usb)
SRC_PLATFORM += $(PLATFORM_PATH)/bt/aic_btusb.c
else
SRC_PLATFORM += $(PLATFORM_PATH)/bt/aic_bt_sdio.c
endif
endif

# --- RTOS 选择 ---
AIC_RTOS ?= freertos
ifeq ($(AIC_RTOS),rtthread)
SRC_PLATFORM += $(PLATFORM_PATH)/rtos/rtos_rtt.c
else ifeq ($(AIC_RTOS),rtt)
SRC_PLATFORM += $(PLATFORM_PATH)/rtos/rtos_rtt.c
else ifneq ($(AIC_RTOS),)
SRC_PLATFORM += $(PLATFORM_PATH)/rtos/rtos_freertos.c
endif

# --- WiFi 应用层源文件 ---
SRC_PLATFORM += $(PLATFORM_PATH)/wifi/wifi_port_sta.c
SRC_PLATFORM += $(PLATFORM_PATH)/wifi/wifi_port_ap.c
SRC_PLATFORM += $(PLATFORM_PATH)/wifi/wifi_port.c
SRC_PLATFORM += $(PLATFORM_PATH)/wifi/wifi_port_wps.c
SRC_PLATFORM += $(PLATFORM_PATH)/wifi/wifi_port_cmd.c
SRC_PLATFORM += $(PLATFORM_PATH)/fmac/aic_fw_port.c
SRC_PLATFORM += $(PLATFORM_PATH)/fmac/aic_chip_desc_port.c
SRC_PLATFORM += $(PLATFORM_PATH)/fmac/userconfig.c
ifeq ($(AIC_FEATURE_PLT),1)
SRC_PLATFORM += $(PLATFORM_PATH)/wifi/wifi_port_plt.c
endif

# --- 工具链配置 ---
CC      ?= gcc
AR      ?= ar
ARFLAGS  = rcs

# --- 编译选项 ---
CFLAGS += -std=gnu99
CFLAGS += -Wall -Wno-unused-function -Wno-unused-variable
CFLAGS += -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
CFLAGS += -Wno-incompatible-pointer-types
CFLAGS += -Wno-return-type
CFLAGS += -Wno-implicit-function-declaration
CFLAGS += $(DFLAGS)
CFLAGS += $(PLATFORM_DFLAGS)
CFLAGS += $(CFLAGS_ARCH)
CFLAGS += $(CFLAGS_INC)

# --- 目标文件 ---
OBJ_PLATFORM := $(patsubst $(PACKAGE_PATH)/%,$(BUILD_DIR)/%.o,$(SRC_PLATFORM))
DEP_PLATFORM := $(OBJ_PLATFORM:.o=.d)

# --- 库文件 ---
# AIC_BUILD_HOST=1 (调试模式): host 编译后镜像到根 lib/, platform 链接 lib/
# AIC_BUILD_HOST=0 (发布模式): 直接链接驱动目录内 lib/ 下预编译库
# (diff 包内驱动目录结构与源码仓库一致: lib/ + include/ 均在 <driver>/ 下)
LIB_DRIVER := $(PACKAGE_PATH)/lib/libaic8800_driver.a
# mbedTLS crypto 库 (USE_BUILTIN_MBEDTLS=1 时链接; =0 由平台自实现 aic_mbedtls_* 符号,
# 参见 platform_config.mk "mbedTLS Crypto 库选择" 及 platform/crypto/ 示例)
ifeq ($(USE_BUILTIN_MBEDTLS),0)
LIB_MBEDTLS :=
else
LIB_MBEDTLS := $(PACKAGE_PATH)/lib/libaic_mbedtls.a
endif

# --- 规则 ---
.PHONY: all clean info host-build host-clean lib release dist diff-verify dist-clean
.DEFAULT_GOAL := all

host-build:
ifeq ($(AIC_BUILD_HOST),1)
	@echo "=== Building host driver from source (lib-sync) ==="
	"$(MAKE)" -C "$(HOST_PATH)" CC="$(CC)" AR="$(AR)" \
		CFLAGS_ARCH="$(CFLAGS_ARCH)" \
		DFLAGS="$(DFLAGS)" \
		AIC_BUILD_MBEDTLS_PORT=$(AIC_BUILD_MBEDTLS_PORT) \
		AIC_CHIP_MODEL=$(AIC_CHIP_MODEL) lib-sync
else
	@echo "=== Skipping host build (using precompiled libraries) ==="
endif

host-clean:
ifeq ($(AIC_BUILD_HOST),1)
	@echo "=== Cleaning host driver build ==="
	"$(MAKE)" -C "$(HOST_PATH)" clean
endif

# nimble: 构建 NimBLE host 协议栈独立库 libnimble.a (仅 AIC_FEATURE_NIMBLE=1)
# 编译入口在驱动框架内: 复用 CC/AR/CFLAGS_ARCH, 委托 STACK/nimble/Makefile。
# nimble 独立成库 (tinycrypt utils.c 与 wpa ap/utils.c 同名冲突, 不并入 libaic8800_driver.a);
# 亦不入 release lib/ — dist 仅在 AIC_FEATURE_NIMBLE=1 时携带 STACK/nimble 源码 (见 dist 目标)。
.PHONY: nimble
nimble:
ifeq ($(AIC_FEATURE_NIMBLE),1)
	@echo "=== Building NimBLE host stack (AIC_FEATURE_NIMBLE=1) ==="
	"$(MAKE)" -C "$(PACKAGE_PATH)/STACK/nimble" CC="$(CC)" AR="$(AR)" \
		CFLAGS_ARCH="$(CFLAGS_ARCH)" AIC_BUS_TYPE=$(AIC_BUS_TYPE) all
else
	@echo "=== Skipping NimBLE build (AIC_FEATURE_NIMBLE=0) ==="
endif

.PHONY: nimble-clean
nimble-clean:
	"$(MAKE)" -C "$(PACKAGE_PATH)/STACK/nimble" clean

# lib: 只构建 host 库 + API 公开头 (release 无调试符号) 到 $(PACKAGE_PATH)/lib/
# 供 make dist 组装; 亦可供纯 platform 调试引用预编译库
# 末尾追加 host check-src-coverage 源-归档对账 (make release/diff-verify 经 dist→lib 自动覆盖)
.PHONY: lib
lib:
	@echo "=== Building host driver lib (AIC_CHIP_MODEL=$(AIC_CHIP_MODEL)) ==="
	@if [ -z "$(AIC_HOST_NOCLEAN)" ]; then \
		"$(MAKE)" -C "$(HOST_PATH)" CC="$(CC)" AR="$(AR)" \
			CFLAGS_ARCH="$(CFLAGS_ARCH_RELEASE)" AIC_CHIP_MODEL=$(AIC_CHIP_MODEL) clean; \
	fi
	"$(MAKE)" -C "$(HOST_PATH)" CC="$(CC)" AR="$(AR)" \
		CFLAGS_ARCH="$(CFLAGS_ARCH_RELEASE)" \
		AIC_BUILD_MBEDTLS_PORT=$(AIC_BUILD_MBEDTLS_PORT) \
		AIC_CHIP_MODEL=$(AIC_CHIP_MODEL) lib-sync
	# 源清单 ↔ 归档成员对账 (host/Makefile check-src-coverage): 静态库归档不解析符号,
	# 源清单漏文件只在整镜像链接阶段暴露为 undefined symbol, 此处提前到归档阶段报错。
	# 显式传 AIC_BUS_TYPE/CC/AR: 子 make 必须落在与 lib-sync 同一个 build_<总线> 目录,
	# 否则会因取到空 AIC_BUS_TYPE 而找不到归档并触发错误标志重编。
	@"$(MAKE)" -C "$(HOST_PATH)" CC="$(CC)" AR="$(AR)" \
		CFLAGS_ARCH="$(CFLAGS_ARCH_RELEASE)" AIC_BUS_TYPE="$(AIC_BUS_TYPE)" \
		AIC_BUILD_MBEDTLS_PORT=$(AIC_BUILD_MBEDTLS_PORT) \
		AIC_CHIP_MODEL=$(AIC_CHIP_MODEL) check-src-coverage

# dist: 从驱动目录组装自包含发布包 (host lib + 公开头 + platform/port/config/docs)
# 输出到 $(AIC_DIST_DIR)/<driver>_release/ (与最终 diff 包同级),
# 不再生成驱动源码同级 *_release/。host 库由 make lib 生成到 $(PACKAGE_PATH)/lib/。
.PHONY: dist
dist: lib
	@echo "=== Assembling self-contained release package -> $(RELEASE_PATH) ==="
	@$(RM_RF) $(RELEASE_PATH)
	@$(call MKDIR,$(RELEASE_PATH)/lib)
	@$(call MKDIR,$(RELEASE_PATH)/lib/include)
	@cp -r $(PACKAGE_PATH)/lib/. $(RELEASE_PATH)/lib/
	@$(call MKDIR,$(RELEASE_PATH)/platform)
	@$(call MKDIR,$(RELEASE_PATH)/port/inc)
	@cp -r $(PLATFORM_PATH)/. $(RELEASE_PATH)/platform/
	@cp -r $(PACKAGE_PATH)/port/inc/. $(RELEASE_PATH)/port/inc/
	# port/*.c 平台实现文件 (如 sdio_ccm.c, 由 platform_config.mk 的 SRC_PLATFORM 引用);
	# 基准仓库无 port/*.c 时跳过, 平台集成树有则随包携带 (缺失则 AIC_BUILD_HOST=0 独立编译失败)
ifneq (,$(wildcard $(PACKAGE_PATH)/port/*.c))
	@cp $(PACKAGE_PATH)/port/*.c $(RELEASE_PATH)/port/
endif
	@cp $(PACKAGE_PATH)/Makefile $(RELEASE_PATH)/Makefile
	@cp $(PACKAGE_PATH)/CMakeLists.txt $(RELEASE_PATH)/CMakeLists.txt
	@cp $(PACKAGE_PATH)/aic_cfg.mk $(RELEASE_PATH)/aic_cfg.mk
	# 复制构建时实际使用的平台配置 (跟随 PLATFORM_CONFIG), 使包内默认配置与本次构建一致
	@cp $(PACKAGE_PATH)/$(PLATFORM_CONFIG) $(RELEASE_PATH)/platform_config.mk
	# 独立平台专用配置原版随包携带 (如 platform_config_mips-gcc540-elf.mk); 非默认 platform_config.mk 时复制原名
	@if [ "$(PLATFORM_CONFIG)" != "platform_config.mk" ]; then \
		cp $(PACKAGE_PATH)/$(PLATFORM_CONFIG) $(RELEASE_PATH)/$(PLATFORM_CONFIG); \
	fi
	@cp $(PACKAGE_PATH)/BUILD.md $(RELEASE_PATH)/
	# lwipopts.md: 平台 lwipopts.h 必需配置说明 (客户移植适配文档, 随包交付)
	@cp $(PACKAGE_PATH)/lwipopts.md $(RELEASE_PATH)/
	@$(call MKDIR,$(RELEASE_PATH)/docs)
	@cp -r $(PACKAGE_PATH)/docs/. $(RELEASE_PATH)/docs/
	# 剔除内部 skill 文档目录 (IP 敏感, 含 aic8800-review 提交前 review 环境)、内部 AI 开发指南 (README-forAI.md) 与 aicdoc 中间产物
	@rm -rf $(RELEASE_PATH)/docs/aic8800-porting $(RELEASE_PATH)/docs/aic8800-diff-package $(RELEASE_PATH)/docs/aic8800-sync $(RELEASE_PATH)/docs/aic8800-review $(RELEASE_PATH)/docs/superpowers 2>/dev/null || true
	@rm -rf $(RELEASE_PATH)/docs/README-forAI.md $(RELEASE_PATH)/docs/aicdoc $(RELEASE_PATH)/aicdoc 2>/dev/null || true
# NimBLE 协议栈源码: 仅在 AIC_FEATURE_NIMBLE=1 时随包携带 (默认关闭, 不打包 STACK/nimble)
ifeq ($(AIC_FEATURE_NIMBLE),1)
	@echo "=== Including STACK/nimble (AIC_FEATURE_NIMBLE=1) ==="
	@$(call MKDIR,$(RELEASE_PATH)/STACK)
	@cp -r $(PACKAGE_PATH)/STACK/nimble $(RELEASE_PATH)/STACK/
endif
	@echo "=== Release package ready (self-contained): $(RELEASE_PATH)/ ==="

# release: 兼容别名 (旧脚本引用), 等价于 dist
.PHONY: release
release: dist
	@echo "=== release (= dist) done ==="

# dist-clean: 清理发布/diff 打包的构建副作用 (dist/<driver>_release + 驱动目录 lib/)
# dist/ 与 lib/ 均为 gitignore 的构建产物; 清理后下次 make 会自动重建 lib/
.PHONY: dist-clean
dist-clean:
	@$(RM_RF) $(RELEASE_PATH)
	@$(RM_RF) $(PACKAGE_PATH)/lib
	@echo "=== dist/lib cleaned (release artifacts removed) ==="

# diff-verify: 发布形态独立编译验证 (AIC_BUILD_HOST=0, 直接引用 lib 不经过 host)
# 生成 diff 包前强制校验 (docs/5-平台移植Diff包说明.md 3.2.2):
#   1. release 包结构完整性 (lib/lib/include/port/platform/构建脚本)
#   2. cd $(RELEASE_PATH) && make AIC_BUILD_HOST=0 独立编译 platform
# 注: 第 2 步需 platform_config.mk 已适配目标 SDK (EXT_INC_DIRS 等)
diff-verify: dist
	@echo "=== diff-verify: release 包结构完整性检查 ==="
	@test -f $(RELEASE_PATH)/lib/libaic8800_driver.a
ifeq ($(USE_BUILTIN_MBEDTLS),1)
	@test -f $(RELEASE_PATH)/lib/libaic_mbedtls.a
endif
	@test -d $(RELEASE_PATH)/lib/include
	@test ! -d $(RELEASE_PATH)/include   # 头单一事实源: 不得存在顶层 include/ 冗余副本
	@test -d $(RELEASE_PATH)/port/inc
	@test -d $(RELEASE_PATH)/platform
	@test -f $(RELEASE_PATH)/Makefile
	@echo "=== diff-verify: 独立编译 (AIC_BUILD_HOST=0, 不经过 host) ==="
	# 包内默认 platform_config.mk 已在 release 组装时复制为本次构建用的 $(PLATFORM_CONFIG);
	# 此处显式指定, 隔离外层 command-line PLATFORM_CONFIG 经 MAKEFLAGS 透传导致的文件名错配
	"$(MAKE)" -C "$(RELEASE_PATH)" AIC_BUILD_HOST=0 PLATFORM_CONFIG=platform_config.mk
	@$(RM_RF) $(RELEASE_PATH)/build
	@echo "=== diff-verify PASS: $(RELEASE_PATH)/build/libaic_platform.a ==="

all: host-build nimble
	@$(MAKE) --no-print-directory $(TARGET)
	@echo "=== Platform build complete: $(TARGET) ==="
	@echo "    Platform sources: $(words $(SRC_PLATFORM)) files"
	@echo "    Host build mode:  AIC_BUILD_HOST=$(AIC_BUILD_HOST)"

$(TARGET): $(OBJ_PLATFORM)
	@$(call MKDIR,$(dir $@))
	$(AR) $(ARFLAGS) $@ $^

$(BUILD_DIR)/%.c.o: $(PACKAGE_PATH)/%.c
	@$(call MKDIR,$(dir $@))
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	$(RM_RF) $(BUILD_DIR)
ifeq ($(AIC_BUILD_HOST),1)
	"$(MAKE)" -C "$(HOST_PATH)" clean
endif
ifeq ($(AIC_FEATURE_NIMBLE),1)
	"$(MAKE)" -C "$(PACKAGE_PATH)/STACK/nimble" clean
endif

info:
	@echo "CC                = $(CC)"
	@echo "AR                = $(AR)"
	@echo "TARGET            = $(TARGET)"
	@echo "AIC_BUILD_HOST    = $(AIC_BUILD_HOST)"
	@echo "AIC_BUS_TYPE      = $(AIC_BUS_TYPE)"
	@echo "PLATFORM_CONFIG   = $(PLATFORM_CONFIG)"
	@echo "可用平台配置      = $(notdir $(wildcard $(PACKAGE_PATH)/platform_config*.mk))"
	@echo "Platform sources  = $(words $(SRC_PLATFORM)) files"
	@echo "EXT_INC_DIRS      = $(EXT_INC_DIRS)"

-include $(DEP_PLATFORM)

endif # ifdef SDKROOT
