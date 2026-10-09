# AIC8800D40N Wi-Fi 示例

源码路径：`example/rt_device/wifi_aic8800d40n`

## 概述

本示例演示 **AIC8800D40N**（含 D40LN / D40WN / D80 / D80N）SDIO Wi-Fi 驱动的接入与使用。

采用**路线 1**：驱动**自建 lwIP netif**，不依赖 SDK 的 `rt_wlan` 框架。
因此应用层使用驱动自有 API（`aic_wifi_scan` / `aic_wifi_start_sta` / ...），
而非 `rt_wlan_scan_sync` / `rt_wlan_connect`。

> ⚠️ 与 `example/rt_device/wifi`（SWT6621 模块 + `rt_wlan` 框架）是**两套不同的架构**，
> 代码不通用。本示例对应 `customer/peripherals/wifi/aic8800d40n/` 驱动。

## 驱动结构

```
customer/peripherals/wifi/aic8800d40n/
├── lib/                     host 核心预编译库 (无源码)
│   ├── libaic8800_driver.a    驱动核心 (单库统一, 型号无关)
│   └── libaic_mbedtls.a       加密库 (避免与 SDK mbedtls 符号冲突)
├── port/inc/                移植接口头 (函数签名契约, 不可改)
├── platform/                平台适配层源码 (由 SConscript 编译)
│   ├── hal/sdio_port.c        SDIO 总线适配
│   ├── rtos/rtos_rtt.c        RT-Thread 适配
│   ├── netif/netif_port.c     lwIP netif 对接
│   ├── wifi/wifi_port*.c      STA/AP/WPS/CMD 业务
│   └── fmac/                  固件加载 + 芯片描述表
└── SConscript               构建入口 (DefineGroup 'aic8800d40n')
```

## 芯片型号说明（单库统一架构）

host 库与芯片型号**无关**，所有型号共用同一个 `libaic8800_driver.a`。
型号差异由 `platform/fmac/aic_chip_desc_port.c` 数据表在**运行时**按硬件 VID/PID 分发：

| SDIO PID | 芯片 | 匹配结果 |
|---|---|---|
| `0x0082` | AIC8800D80 | `PRODUCT_ID_AIC8800D80` |
| `0x9081` | AIC8800D80N | `PRODUCT_ID_AIC8800D80N` |
| `0x9082` | AIC8800D80LN | 同上 |
| `0x9083` | AIC8800D80WN | 同上 |
| **`0x9084`** | **AIC8800D40N** | **`PRODUCT_ID_AIC8800D80N`** |
| **`0x9085`** | **AIC8800D40LN** | **同上** |
| **`0x9086`** | **AIC8800D40WN** | **同上（D80WN 路径）** |

即 **D40N 族属 D80N 型号族**，无需改驱动代码，硬件插上即自动识别。

## 支持的开发板

| 开发板 | SOC | SDIO 路径 |
|---|---|---|
| `sf32lb55-lcd_n16r4n4` | SF32LB55X | SDHCI2 |

> **SDIO 路径差异（重要）**：
> - **LB52X / LB57X** → 线性 SDIO，`CONFIG_BSP_USING_SD_LINE=y`
> - **LB55X / LB56X / LB58X** → SDHCI 控制器，`CONFIG_BSP_USING_SDHCI` + `SDHCIx` + `SDIOx_CARD_MODE=2`
>
> 驱动包基准板 `eh-lb525` 属 LB52X；移植到 LB55X 必须改用 SDHCI 配置（本工程已配好）。

## 使用方法

### 编译

```bash
cd example/rt_device/wifi_aic8800d40n/project
scons --board=sf32lb55-lcd_n16r4n4 -j8
```

产物：`build_sf32lb55-lcd_n16r4n4_hcpu/main.elf` 与 `main.bin`

### 烧录

```bash
cd example/rt_device/wifi_aic8800d40n/project
build_sf32lb55-lcd_n16r4n4_hcpu/uart_download.bat
```

### 运行

串口（默认 `uart1`）应见：

```
[aic8800d40n] demo start
[AIC_I]aic_platform_init ...
[AIC_I]detect sdio device
[AIC_I]wifi_driver_init ...
[aic8800d40n] driver up. use 'wifi_test <cmd>' to control
msh />
```

然后通过 MSH 命令控制：

```
wifi_test wifi_on
wifi_test scan
wifi_test connect <ssid> <pwd>
```

## 关键配置项（`proj.conf`）

| 配置 | 值 | 说明 |
|---|---|---|
| `CONFIG_WIFI_USING_AIC8800D40N` | `y` | 启用本驱动（触发 SConscript 的 DefineGroup） |
| `CONFIG_BSP_USING_SDIO` | `y` | 启用 SDIO |
| `CONFIG_BSP_USING_SDMMC2` | `y` | LB55X 用 SDMMC2（走 SDHCI2） |
| `CONFIG_SDIO2_CARD_MODE` | `2` | 2 = SDIO 卡模式（**必须**） |
| `CONFIG_RT_USING_LWIP212` | `y` | lwIP 2.1.2 |
| `CONFIG_RT_USING_WIFI` | **不要开** | 路线 1 不使用 rt_wlan |
| `CONFIG_AIC_WLAN_AIC8800D40L` | **不要开** | 旧驱动，与本驱动符号冲突 |
| `CONFIG_PKG_USING_MBEDTLS` | **不要开** | 驱动自带 `libaic_mbedtls.a`，同开会 `aes_*`/`utils.c` 重复定义 |

## WiFi 电源 / 复位引脚配置（★ 上板必配）

驱动 `platform/hal/plat_port.c` 的电源与复位控制由 Kconfig 配置，
组织方式**对齐旧驱动 `aic8800d40`**（`AIC_WIRELESS_PWR_GPIO` 等）。
电平语义见驱动文档 `docs/4-常见问题说明.md`「如何给 wifi 芯片上电」：

| Kconfig 项 | 对应旧驱动 | 作用 | 有效电平 |
|---|---|---|---|
| `CONFIG_AIC8800D40N_WIFI_PWR_GPIO` | `AIC_WIRELESS_PWR_GPIO` | 使能 WiFi 供电 LDO | **高有效** |
| `CONFIG_AIC8800D40N_WIFI_RST_GPIO` | `AIC_DEV_AIC8800_WLAN0_RST_GPIO` | WiFi pwrkey / 硬复位 | **低有效** |
| `CONFIG_AIC8800D40N_WIFI_HOST_WAKE_GPIO` | — | 主控唤醒 WiFi | — |
| `CONFIG_AIC8800D40N_WIFI_WAKE_HOST_GPIO` | — | WiFi 唤醒主控 | — |

默认全部为 `-1`（不控制）。**若你的模组需要软件控制上电，必须在 `proj.conf` 中填入实际 GPIO 号**，
例如：

```conf
CONFIG_AIC8800D40N_WIFI_PWR_GPIO=124
CONFIG_AIC8800D40N_WIFI_RST_GPIO=-1
```

> ⚠️ 引脚号务必按你的硬件原理图确认，不要直接照抄示例值。

### 上电 / 复位时序

驱动内部按下列时序操作（对齐旧驱动 `wifi_if_drv.c` 的 `wifi_if_sdio_init_my`）：

```
上电:  init() 配置引脚为输出 -> disable() 断电复位 -> enable() 上电
复位:  sdio_pdn_wifi()    = 掉电
       sdio_reset_wifi()  = 掉电 + 延时 10ms + 重新上电
       sdio_reset()       = 同上, 返回 0 表示成功 (异常恢复用)
```

延时长度由 `AICWF_WIFI_PWR_RESET_DELAY_MS` 控制（默认 10ms），
需要时可在 `proj.conf` 中用 `-DAICWF_WIFI_PWR_RESET_DELAY_MS=<ms>` 覆盖。

## 常见编译/运行问题

| 现象 | 根因 | 解决 |
|---|---|---|
| `Board: None` | 未传 `--board` | 命令加 `--board=sf32lb55-lcd_n16r4n4` |
| `Please run set_env.bat ...` | 未初始化 SDK 环境 | 在 SDK 根目录执行 `. ./export.ps1`（Windows）或 `. ./export.sh` |
| `multiple definition of aes_*` / `utils.c` | 同时开了 SDK mbedtls | menuconfig 关闭 `CONFIG_PKG_USING_MBEDTLS` |
| `multiple definition of aicwf_*` / `wpa_*` | 同时开了旧驱动 `aic8800d40` | 关闭 `CONFIG_AIC_WLAN_AIC8800D40L`（Kconfig 已加 `depends on !` 互斥） |
| WiFi 起不来 / 无 FW 日志 | 固件未加载 / SDIO 时序 | 确认 `platform/fmac/fw/` 完整、`CONFIG_LOAD_FW=2`；SDIO 上电时序见下 |
| 卡在 `detect sdio device` | 上电即就绪，无热插拔事件 | 见下方"已知待办" |

## 已知待办（硬件联调）

1. **WiFi 电源/复位引脚**：默认 `-1`（不控制）。若模组需软件上电，
   必须按原理图在 `proj.conf` 填入 `CONFIG_AIC8800D40N_WIFI_PWR_GPIO` /
   `CONFIG_AIC8800D40N_WIFI_RST_GPIO`（见上一节）。

2. **SDIO 卡检测时序**：`platform/wifi/wifi_port_plt.c` 的 `aicwf_aic_wifi_bus_waitready()`
   使用 `mmcsd_wait_cd_changed(RT_WAITING_FOREVER)` 阻塞等卡检测变化。
   若 WiFi 模组焊接在板上（上电即就绪、无热插拔事件），该等待可能不唤醒。
   硬件联调时按需改为「先注册 `sdio_driver` 再轮询 `host->card`」。

## 参考

- 驱动构建细节：`customer/peripherals/wifi/aic8800d40n/BUILD.md`
- 适配流程：`customer/peripherals/wifi/aic8800d40n/docs/aic8800-release-adaptation/SKILL.md`
- lwIP 配置：`customer/peripherals/wifi/aic8800d40n/lwipopts.md`
- 驱动用户文档：`customer/peripherals/wifi/aic8800d40n/docs/1-适配编译说明.md` 等
