# ============================================================================
# Platform Configuration — SiFli SF32LB52x (ARM Cortex-M33, arm-none-eabi-gcc 14.2.1)
# ============================================================================
# 目标 SDK : SiFli SF32LB52x release/v2.4 (RT-Thread 3.1.3 + lwIP 2.1.2 + scons)
# 芯片/总线: AIC8800D80 / SDIO / 5G        RTOS: RT-Thread     WiFi-only (BT/nimble off)
#
# 启用:
#   . <SDK>/export.sh                       # 提供 arm-none-eabi-gcc 14.2.1
#   make PLATFORM_CONFIG=platform_config_cortex-m33.mk \
#        AIC_CHIP_MODEL=D80 AIC_BUS_TYPE=SDIO AIC_RTOS=rtthread \
#        AIC_FEATURE_BT=0 AIC_FEATURE_NIMBLE=0 AIC_FEATURE_PLT=1 AIC_FEATURE_P2P=0 \
#        AIC_BUILD_HOST=1 host-build         # 阶段1: 仅 host 库 (平台无关)
#
# ABI 旗标取自 SDK 实测 compile_commands.json (example/get-started/hello_world, 282 C 文件):
#   -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard
#   -funsigned-char -fshort-enums -fshort-wchar -Os
# 注: host/Makefile 会在 CFLAGS_ARCH 之后强制追加 -fsigned-char (host 库恒为 signed-char),
#     与 platform/SDK 的 unsigned-char 存在固有分歧 (仅影响 char 符号扩展, 不改布局), 已知并记录。

# ============================================================================
# 芯片型号 / 总线 / RTOS
# ============================================================================
AIC_CHIP_MODEL := D80N   # [AIC8800-PORT][SiFli] 本板实测 D80N(PID 0x9082); 该宏决定固件表 CINIT/PATCH/CALIB
AIC_BUS_TYPE   := SDIO
AIC_RTOS       := rtthread

# ============================================================================
# 功能开关 (WiFi-only)
# ============================================================================
AIC_FEATURE_BT     := 0     # 不移植 BT
AIC_FEATURE_NIMBLE := 0     # 不移植 nimble (不携带 STACK/)
AIC_FEATURE_PLT    := 1     # 编入 wifi_port_plt.c: SDIO 上电时序 + cmd_test/wifi_test msh 入口
AIC_FEATURE_P2P    := 0     # 回归 SPEC: 基准 d4509b7 的 1d16913 "P2P 编译开关安全化" 已修复根因(wifi_def.h 的 aic_wifi_event_data sizeof 恒 72 不再 ABI 错位; fhost_wpa.c/wpa_supplicant.c 的 P2P 引用补 #ifdef 守卫), P2P=0 host 库编译验证 EXIT=0。WiFi-only 场景不需要 P2P。

# ============================================================================
# 工具链 — SiFli SDK 自带 arm-none-eabi-gcc 14.2.1 (先 . export.sh)
# ============================================================================
ARM_TOOLCHAIN_ROOT ?= $(HOME)/.sifli/tools/arm-none-eabi-gcc/14.2.1
CC := $(ARM_TOOLCHAIN_ROOT)/bin/arm-none-eabi-gcc
AR := $(ARM_TOOLCHAIN_ROOT)/bin/arm-none-eabi-ar

# ============================================================================
# Cortex-M33 架构旗标 (与 SDK 逐字对齐)
# ============================================================================
CFLAGS_ARCH = \
    -mcpu=cortex-m33 \
    -mthumb \
    -mfpu=fpv5-sp-d16 \
    -mfloat-abi=hard \
    -funsigned-char \
    -fshort-enums \
    -fshort-wchar \
    -ffunction-sections \
    -fdata-sections \
    -Os \
    -g -g2
CFLAGS_ARCH_RELEASE := $(filter-out -g -g2,$(CFLAGS_ARCH))

# ============================================================================
# 类型定义宏
# ============================================================================
CFLAGS += -DSYS_SOCK_DEFINE      # 平台 (lwIP) 已提供 POSIX socket 类型
CFLAGS += -DPLATFORM_NO_FILE     # RTOS/newlib 裸机, 无 stdio FILE

# ============================================================================
# 固件加载模式: 0=filemode 1=flashmode 2=innermode(内嵌固件数组, 默认)
# ============================================================================
CFLAGS += -DCONFIG_LOAD_FW=2

# ============================================================================
# SDIO 中断挂起恢复
# ============================================================================
AIC_IRQ_SUS ?= 0
CFLAGS += -DCONFIG_IRQ_SUS_DEFAULT=$(AIC_IRQ_SUS)

# ============================================================================
# 平台功能开关 (rftest / iperf 默认关)
# ============================================================================
AIC_WIFI_MODE_RFTEST ?= 0
ifeq ($(AIC_WIFI_MODE_RFTEST),1)
CFLAGS += -DCONFIG_WIFI_MODE_RFTEST
endif

AIC_FEATURE_IPERF ?= 0
ifeq ($(AIC_FEATURE_IPERF),1)
CFLAGS += -DCONFIG_IPERF
endif

# ============================================================================
# 平台宏 (与 SDK 集成分支一致; RT-Thread 分支代码用)
# ============================================================================
PLATFORM_DFLAGS += -DRT_THREAD

# --- WPA 短名类型冲突保护 ---
# aic_porting_types.h 的 u8/u16/u32/s8/s16/s32 受 #ifndef WPA_TYPES_DEFINED 保护。
# 若链接期报与 SDK (CMSIS/lwIP) typedef 重定义, 取消下行注释复用平台类型:
#PLATFORM_DFLAGS += -DWPA_TYPES_DEFINED

# ============================================================================
# mbedTLS Crypto: 1=使用内置 libaic_mbedtls.a (SDK 链接时需加入)
# ============================================================================
USE_BUILTIN_MBEDTLS ?= 1

# ============================================================================
# 外部 SDK 头文件路径 (RT-Thread / lwIP / SDK HAL) — 仅 platform 库(stage2)使用
# ============================================================================
SIFLI_SDK ?= $(realpath $(PACKAGE_PATH)/../..)
LWIP_ROOT  = $(SIFLI_SDK)/rtos/rtthread/components/net/lwip
EXT_INC_DIRS = \
    $(SIFLI_SDK)/rtos/rtthread/include \
    $(SIFLI_SDK)/rtos/rtthread/components/drivers/include \
    $(SIFLI_SDK)/drivers/Include \
    $(LWIP_ROOT)/lwip-2.1.2/src/include \
    $(LWIP_ROOT)/port
# 注: stage2 编 platform 还需 rtconfig.h — 由 scons 集成工程生成于其 build 目录,
#     届时追加: EXT_INC_DIRS += <SDK>/example/aic8800_d80/project/rtt/project/build_<board>/

# ============================================================================
# SRC_PLATFORM 手动追加
#   Makefile 不自动收集 hal/plat_port.c; 缺失则最终固件链接报 HAL(电源/GPIO/日志)符号未定义
#   (sdio_port.c/netif_port.c/rtos_rtt.c/wifi_port_*.c/userconfig.c 等 Makefile 已自动收集, 勿重复)
# ============================================================================
SRC_PLATFORM += $(PLATFORM_PATH)/hal/plat_port.c
