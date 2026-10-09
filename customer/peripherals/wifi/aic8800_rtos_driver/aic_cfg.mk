# ============================================================================
# AIC8800 功能裁剪 — Make 层开关
#
# 用法: 设为 0 即不编译对应功能的源文件
#       配合 port/inc/aic_cfg.h 中的 AIC_DISABLE_* 宏一同使用
#
# 注意: AP 与 STA 为基础功能，不可裁剪
#
# AIC_WPA_LOG: 取消注释即完全关闭 wpa 日志（wpa_printf/wpa_hexdump 变为空操作）
# ============================================================================

# ============================================================================
# 芯片型号选择
#
# 用法: 在 platform_config.mk 中设置 AIC_CHIP_MODEL，此处用 ?= 提供默认值。
# 可选型号（互斥，只能启用一个）:
#   D80  — AIC8800D80 (支持 5G)
#   D80N — AIC8800D80N (支持 5G, u02 子分支)
#   D80N_U02C — AIC8800D80N (支持 5G, u02c 子分支)
#   DW   — AIC8800DW
#   DC   — AIC8800DC
#   DL   — AIC8800DL
#   DC_H — AIC8800DC  (chip sub_id=2, 仅含 _h_u02 固件, 节省 Flash)
#   DW_H — AIC8800DW  (chip sub_id=2, 仅含 _h_u02 固件, 节省 Flash)
#   DL_H — AIC8800DL  (chip sub_id=2, 仅含 _h_u02 固件, 节省 Flash)
#   D80X2 — AIC8800D80X2 (支持 5G, U05 基线, 2T2R)
# ============================================================================
AIC_CHIP_MODEL ?= D80N
# [AIC8800-PORT][SiFli] 本板实测为 D80N (卡级 PID=0x9082, chip_id=3/sub_id=1/rev=U03)。
#   本包 lib/ 版本串 `NV1.1.6 STA AP  gb4036bf 20261013` (厂家 WiFi-only 交付);
#   D80N 主 FMAC 复用 D80 固件 (见 platform/fmac/aic_fw_port.c), 故该库可用。
#   若某板实测为 D80: 此处与 platform_config.mk / platform_config_cortex-m33.mk 一并改 D80。
#   注: port/inc/aic_chip_desc.h 的 msg_fc_bypass 在 D80N 条目已置 1 (否则 boot 卡死)。

# 根据芯片型号设置编译宏
ifeq ($(AIC_CHIP_MODEL),D80)
DFLAGS += -DCONFIG_AIC8800D80
DFLAGS += -DUSE_5G
endif
ifeq ($(AIC_CHIP_MODEL),DW)
DFLAGS += -DCONFIG_AIC8800DW
AIC_FEATURE_BT := 0
endif
ifeq ($(AIC_CHIP_MODEL),DC)
DFLAGS += -DCONFIG_AIC8800DC
endif
ifeq ($(AIC_CHIP_MODEL),DC_H)
DFLAGS += -DCONFIG_AIC8800DC
DFLAGS += -DCONFIG_AIC8800DC_H
endif
ifeq ($(AIC_CHIP_MODEL),DW_H)
DFLAGS += -DCONFIG_AIC8800DW
DFLAGS += -DCONFIG_AIC8800DW_H
AIC_FEATURE_BT := 0
endif
ifeq ($(AIC_CHIP_MODEL),DL)
DFLAGS += -DCONFIG_AIC8800DC
DFLAGS += -DCONFIG_NO_SDIO_FUNC_RELEASE
endif
ifeq ($(AIC_CHIP_MODEL),DL_H)
DFLAGS += -DCONFIG_AIC8800DC
DFLAGS += -DCONFIG_AIC8800DC_H
DFLAGS += -DCONFIG_NO_SDIO_FUNC_RELEASE
endif
ifeq ($(AIC_CHIP_MODEL),D80N)
DFLAGS += -DCONFIG_AIC8800D80N
DFLAGS += -DUSE_5G
AIC_FEATURE_BT ?= 1
endif
ifeq ($(AIC_CHIP_MODEL),D80N_U02C)
DFLAGS += -DCONFIG_AIC8800D80N
DFLAGS += -DCONFIG_AIC8800D80N_U02C
DFLAGS += -DUSE_5G
AIC_FEATURE_BT ?= 1
endif
ifeq ($(AIC_CHIP_MODEL),D80X2)
DFLAGS += -DCONFIG_AIC8800D80X2
DFLAGS += -DUSE_5G
AIC_FEATURE_BT ?= 1
endif

# ============================================================================
# 总线类型选择 (Plan B — 编译期总线选择)
#
# 用法: 选择一个总线类型（互斥，只能启用一个）
#   SDIO (默认) → CONFIG_SDIO_SUPPORT
#   USB         → CONFIG_USB_SUPPORT + CONFIG_AICWF_BUS_USB
#
# 所有芯片型号 (D80/D80N/DC/DW/D81/D81X2/D89X2) 均支持两种总线。
# USB 模式当前为 stub 实现 (usb_co.c / usb_port.c)，包含完整接口定义、
# 参数验证和基础错误处理；真实 USB 传输逻辑 (URB/bulk endpoint) 待
# 完整 USB bring-up 时替换 stub。
# ============================================================================
AIC_BUS_TYPE ?= SDIO

ifeq ($(AIC_BUS_TYPE),SDIO)
DFLAGS += -DCONFIG_SDIO_SUPPORT
else ifeq ($(AIC_BUS_TYPE),USB)
DFLAGS += -DCONFIG_USB_SUPPORT
DFLAGS += -DCONFIG_AICWF_BUS_USB
DFLAGS += -DAIC_BUS_USB
else
$(error Unknown AIC_BUS_TYPE "$(AIC_BUS_TYPE)". Valid values: SDIO | USB)
endif

AIC_FEATURE_P2P ?= 1
AIC_FEATURE_BT  ?= 0
AIC_FEATURE_PLT ?= 0
# 0 = 不编译 PLT 演示入口 (需要平台 SDK: FreeRTOS.h, mmcsd_core.h)

# NimBLE host 协议栈 (STACK/nimble) 开关 — 默认关闭
#   0 = 关闭 (默认): release/diff 包不携带 STACK/nimble, platform 不注入 nimble 宏
#   1 = 开启: 随包携带 STACK/nimble 源码 + platform 注入 -DAIC_FEATURE_NIMBLE
#             (wifi_port_plt.c 据此拉起 nimble_stack_start, 需 BT 同开)
#   注: nimble 协议栈本体编译为独立 libnimble.a (tinycrypt utils.c 与 wpa ap/utils.c
#       同名冲突, 不并入 libaic8800_driver.a)。编译入口在驱动框架内:
#       - Make 流: make nimble (或 all, 本开关=1 时自动) 委托 STACK/nimble/Makefile
#       - CMake 流: -DAIC_FEATURE_NIMBLE=ON, host/CMakeLists 或顶层 add_subdirectory
AIC_FEATURE_NIMBLE ?= 0

# BT 总线接口选择: sdio | usb
# sdio = 传统 SDIO/UART BT 通道 (aic_bt_driver.c)
# usb  = USB HCI BT 通道 (aic_btusb.c, 需要 AIC_BUS_TYPE=USB)
AIC_BT_BUS_TYPE ?= sdio

# BT HCI 传输选择: sdio | uart | usb (未指定时跟随 AIC_BT_BUS_TYPE)
#   sdio = 邮箱 HCI (aic_hci.c + aic_bt_sdio.c, 定义 AIC_HCI_OVER_SDIO)
#   uart = 串口 HCI (aic_hci.c + aic_bt_uart.c, 定义 AIC_HCI_OVER_UART)
#   usb  = USB 蓝牙类 HCI (aic_hci.c + aic_btusb.c, 定义 AIC_HCI_OVER_USB)
AIC_HCI_TRANSPORT ?= $(if $(filter usb,$(AIC_BT_BUS_TYPE)),usb,sdio)

# 将 Make 层变量映射为 C 编译宏
ifeq ($(AIC_FEATURE_P2P),1)
DFLAGS += -DAIC_FEATURE_P2P
DFLAGS += -DCONFIG_P2P_SUPPORT
endif
ifeq ($(AIC_FEATURE_BT),1)
DFLAGS += -DAIC_FEATURE_BT
ifeq ($(AIC_BT_BUS_TYPE),usb)
DFLAGS += -DAIC_BT_BUS_USB
endif
endif
ifeq ($(AIC_FEATURE_NIMBLE),1)
DFLAGS += -DAIC_FEATURE_NIMBLE
endif

# AIC_HCI_OVER_SDIO: SDIO 邮箱 HCI 通道选择, 与 AIC_FEATURE_BT 解耦 —
#   由 AIC_HCI_TRANSPORT=sdio (默认) 直接注入, 驱动 platform 层 BT 固件注册
#   (aic_fw_port.c DC_H 选 hbt 变体 / userconfig.c 设 btport=MB)。
#   uart/usb HCI 通道时不注入 (FW 侧 BT 端口由 patch 配置为 UART)。
#   host 层不依赖此宏: wifisetting BT 共存表项改由 rwnx_main.c 运行时
#   aicbt_user_info.btport 判断, 保证 host 单库兼容所有 HCI 传输。
# host 层 BT 驱动代码本身的编译开关是 host_config.mk 的 AIC_HOST_BT_DRIVER。
ifeq ($(AIC_HCI_TRANSPORT),sdio)
DFLAGS += -DAIC_HCI_OVER_SDIO
else ifeq ($(AIC_HCI_TRANSPORT),uart)
DFLAGS += -DAIC_HCI_OVER_UART
else ifeq ($(AIC_HCI_TRANSPORT),usb)
DFLAGS += -DAIC_HCI_OVER_USB
else
$(error Unknown AIC_HCI_TRANSPORT "$(AIC_HCI_TRANSPORT)". Valid values: sdio | uart | usb)
endif

# WPA 日志开关: 注释掉此行以启用 wpa 日志，取消注释以完全关闭
# DFLAGS += -DCONFIG_NO_STDOUT_DEBUG

# WPA ctrl_iface 消息开关: 默认关闭（节省空间）
# 注释掉此行以启用 wpa_msg 族（事件通知/控制接口消息）
DFLAGS += -DCONFIG_NO_WPA_MSG

# ================================================================
# FW DPD/校准日志 patch (仅 DC/DW, 启动早期写 wifi_setting
# lp_level + debug_mask=0xAF19, FW 刚起来就收 DPD 日志)
# 1 = 开启 (rwnx_main.c patch 表追加 {0x00B0, 0xAF190100})
# 0 = 关闭 (默认); 运行时开日志用 aic_wifi_fw_log_enable() 即可
# ================================================================
AIC_FW_PATCH_DPD_LOG ?= 0
ifeq ($(AIC_FW_PATCH_DPD_LOG),1)
DFLAGS += -DFW_PATCH_DPD_LOG=1
endif

# =============================================================================
# RX 全链路零拷贝
#
# [PLATFORM] 1 = 启用 CONFIG_RX_NOCOPY: SDIO RX node 经引用计数延迟归还,
#       数据帧以 lwip custom pbuf 直指 SDIO 缓冲入栈, 省去 reorder/转发
#       路径的整帧 memcpy; SDIO_RX_BUF_COUNT_DEFAULT 自动加大补偿
#       node 生命周期变长 (见 sdio_co.h)
#       依赖平台 lwip 配置 LWIP_SUPPORT_CUSTOM_PBUF=1;
#       平台未开该宏时 netif_port.c 自动降级为 alloc+memcpy copy 路径
#       (aicwf_platform_rx_zero_copy 恒返回 NULL), 因此可安全默认打开。
#       0 = 关闭, 走旧 alloc+memcpy 路径
# host 库与 platform 层共用此开关 (两层 Makefile 均 include 本文件)
# =============================================================================
AIC_RX_NOCOPY ?= 1
ifeq ($(AIC_RX_NOCOPY),1)
DFLAGS += -DCONFIG_RX_NOCOPY
endif

# ================================================================
# mbedTLS 编译控制
#
# host 层始终构建 libaic_mbedtls.a 并放入 <driver>_release/lib/
# 平台层通过 platform_config.mk 中的 USE_BUILTIN_MBEDTLS 决定是否使用：
#   USE_BUILTIN_MBEDTLS=1（默认）：链接 release 中的内置库
#   USE_BUILTIN_MBEDTLS=0：平台自行实现 35 个 aic_mbedtls_* 符号
#      接口契约见：host/thirdparty/mbedtls_v3_6_2/include/aic_mbedtls_crypto_if.h
#      直接转发示例：platform/crypto/aic_mbedtls_port_example.c
#      (示例实现 = 标准 mbedtls API 去 aic_ 前缀直接转发, 适用于平台
#       SDK 自带 mbedtls 的场景; 硬件加速场景可替换为平台实现)
#
# Port 层（TLS 客户端/证书）独立控制，见 host/Makefile 中 AIC_BUILD_MBEDTLS_PORT
# ================================================================
