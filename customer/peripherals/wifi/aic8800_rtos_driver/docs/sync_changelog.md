# SYNC_CHANGELOG — AIC8800 RTOS 驱动发布变更记录

> 本文件随 `docs/` 随发布包（release / diff 包）交付，面向**集成与移植客户**，记录每个发布版本的
> 变更摘要、影响面与**平台侧必做动作**。
>
> - 维护规则：每次出包前**追加一节**；节内版本串须与包内库一致，可用
>   `strings lib/libaic8800_driver.a | grep NV1.1.6` 核对。
> - 本文件只写客户可见信息；平台工程内部记录（同步台账、平台定制说明）不在此处。
> - 「必做动作」= 不执行会导致**编译/链接失败或功能异常**的项；仅性能/日志类优化不列入。

## 版本对照

| 版本串（包内 `lib/libaic8800_driver.a`） | 日期 | 备注 |
|---|---|---|
| `NV1.1.6 STA AP P2P gf5d2aef 20261013` | 2026-10-13 | 当前版本，含 **3 项契约变更**，升级必读 |
| `NV1.1.6 STA AP P2P gcb27c2b 20261009` | 2026-10-09 | 含 host 库对平台符号的**新依赖**，升级必读 |
| `NV1.1.6 STA AP P2P g0e6ddbe 20260929` | 2026-09-29 | 本记录起始版本 |

> 交付包文件名后缀日期与上表日期一致（如 `…_20261013.tar.gz`）；版本串中的 `g<hash>` 为驱动源码提交短 hash。

---

## 20261013（`gf5d2aef`）— 相对 `20261009`

### 一、平台侧必做动作

1. **D80N 型号：`msg_fc_bypass` 必须置 1**
   D80N 固件不维护 msg 通道 flow-control credit，若按流控路径发送，boot 序列会卡在首条
   `system_config`（表现为周期性 `msg fc:0` 重试，设备起不来）。
   - `port/inc/aic_chip_desc.h` 新增字段 `uint8_t msg_fc_bypass`（**缺省 0 = 流控 = 原行为**）；
   - 包内参考实现 `platform/fmac/aic_chip_desc_port.c` 已按型号正确填值（仅供 D80N 为 1）；
   - **自维护 chip_desc 数据表的平台**请同步该字段，D80N 条目置 1，其余型号保持 0。

2. **扫描结果结构变更（公开头，需用新头重编平台层）**
   `lib/include/drv_api/inc/wifi_api.h` 中 `wifi_ap_info_t`：
   - `ssid[32]` → `ssid[WIFI_MAX_SSID_LEN + 1]`（恒以 `'\0'` 结尾，修满 32 字节 SSID 拷贝越界 1 字节）；
   - 新增 `ssid_len`（SSID 实际字节数，隐藏 AP = 0）与 `auth_mode`（新枚举 `aic_wifi_auth_mode_e`）。
   → `wifi_ap_list_t` 的布局与 `sizeof` 随之变化；**平台层与自备该结构副本的工程必须用新头重编**
   （混用新旧头会出现内存错位）。

3. **平台层必须提供 `aicwf_dbg_snprintf`**
   host 预编译库直接引用该平台符号（`port/inc/plat_port.h` 声明、`platform/hal/plat_port.c` 实现，
   强符号、无弱符号兜底）。
   - 使用包内 `plat_port.c` 的工程无需改动；
   - **整体替换/重写过 `plat_port.c` 的工程**须保留该函数，否则最终链接报
     `undefined reference to 'aicwf_dbg_snprintf'`。

4. **AT 响应输出通道变化**
   参考实现 `aicwf_ATRESP()` 改经 AIC 调试日志输出（`DBG_APP_INF`，串口抓包只采集 `[AIC][*]`）。
   若平台依赖原控制台输出（如 RT-Thread `rt_kprintf`），请在该函数内自行追加。

5. **GPIO 触发接口改为 id→pin 映射表**
   `aicwf_plat_gpio_trigger()`：`DRV_TRG_*` 逻辑触发 id 与板级引脚（物理概念）解耦，平台只需维护
   映射表并实现 `[PLATFORM]` 标注处的实际输出调用。

6. **SDIO 默认值可覆盖（`port/inc/sdio_port.h`）**
   `CONFIG_RXTASK_INSDIO_DEFAULT` / `CONFIG_RX_POLLING_DEFAULT` 等默认值已加 `#ifndef` 包裹，
   可用 `-D` 正常覆盖。默认值为 **0（独立 RX task；中断驱动）**，轮询模式非必要不启用。

7. **使用 CMake 集成的工程**
   本版将 CMake 清单/门控与 Makefile 对齐并显式化开关：`AIC_FEATURE_PLT`（默认 0）、
   `AIC_FEATURE_P2P`（默认 1）、`AIC_BUILD_MBEDTLS_PORT`（默认 1）、`AIC_FEATURE_BT`（默认 0）、
   `AIC_HOST_BT_DRIVER`（默认 1）、`AIC_MBEDTLS_FWD_EXAMPLE`；同时修复引用模式下 BT 平台层与
   HCI 传输宏缺失、全新 configure 时 P2P 源码被静默丢弃等问题。默认行为与旧版有差异，可按需 `-D` 固定。

### 二、功能修复

- **D80N**：msg 通道免流控（修 boot 卡死）；chipmatch 失败时终止初始化并释放资源，避免后续异常。
- **D80N STA 信息查询**：恢复 `chip_desc` 的 `sta_info_more_info = 1`，`mm_get_sta_info_req`
  变长打包与固件消息格式重新对齐。
- **隐藏 AP 连接**：`max_scan_ssids` 由 1 提升到 3（wpa 仅在 `max_ssids > 1` 时才下发定向 SSID，
  原值使 `scan_ssid` 配置失效，隐藏 AP 永不被发现 → 连接超时）；连接配置追加 `scan_ssid 1`。
- **WEP 连接回退**：`OPEN|SHARED` 组合优先尝试 OPEN，认证被拒（status 13 / 不支持该算法）时正确
  剔除 OPEN 回退到 SHARED；修对端不回 status 13 时卡在 shared 交换、无法回退的连接失败。
- **扫描列表隐藏 SSID 占位**：修首条隐藏 AP 只拷贝 1 字节、以及隐藏标志跨条目粘滞的问题。
- **链路关闭竞态**：`link_close` 关 fd 前清空各 vif 指向该链路的 socket 引用；两处迟到事件断言改为
  丢弃并记日志（修 wep/wps 循环、teardown 竞态下的崩溃）。
- **越界修复**：满 32 字节 SSID 拷贝越界 1 字节；`getscan` 写 `ap_info[64]` 越界；
  AT 扫描输出 `index[32]` 栈越界。
- **日志级别**：无匹配命令的 e2a 消息由 ERR 降为 VRB（扫描结果、survey 等 IND 走此路径属正常）。

### 三、构建与交付

- `docs/aic8800-release-adaptation`：补充 `plat_port.c` 必加依据与 `platform_config.mk` 关键项说明；
  `BUILD.md` 增补 CMake 开关表。

---

## 20261009（`gcb27c2b`）— 相对 `20260929`

### 一、平台侧必做动作

1. **平台层必须提供 `aicwf_dbg_snprintf`（本版起）**
   host 侧格式化输出统一收敛到平台 porting 接口（删除 `#define aicwf_dbg_snprintf snprintf` 宏遮蔽
   与裸 libc 调用），因此 `fhost.o` / `wlan_if.o` / `fhost_cntrl.o` 均引用该平台符号。
   用包内 `plat_port.c` 即可；**整体替换过 `plat_port.c` 的工程须补该实现**，否则链接失败。

2. **模式设置 API 迁移**：platform 层 13 处 `aic_wifi_set_mode()` → 公开 API `aic_wifi_mode_set()`。

3. **版本串锚点**：锚点文件中的 commit 若不可解析，版本串将正确降级为 HEAD（此前不校验存在性）。

4. **`platform_config.mk`（RISC-V）**：补 `CFLAGS_ARCH_RELEASE`；此前该变量为空会编出 rv64 目标库，
   与 SDK 的 rv32 库链接出现 ABI 冲突。

5. **SDIO 默认值**：`port/inc/sdio_port.h` 默认值加 `#ifndef` 包裹（可用 `-D` 覆盖）。

6. **CMake 集成（如使用）**：清单/门控与 Makefile 对齐，详见下方「功能修复」与上一节第 7 条。

### 二、功能修复

- **隐藏网络扫描列表**：修 `wlan_if_getscan` 隐藏 SSID 占位逻辑反置与标志粘滞。
- **WEP / 隐藏网络连接**：WEP `OPEN|SHARED` 优先 OPEN 并支持回退；连接配置追加 `scan_ssid 1`。
- **链路 teardown 竞态**：`link_close` 清理 socket 引用 + 迟到事件断言改丢弃。
- **macif 发送队列**：fire-and-forget 判据通用化（CFM 无人消费即发完即焚），避免固件不回 CFM 时
  命令队列泄漏；`ME_TRAFFIC_IND_REQ` 恢复 `reqcfm` 原始语义。
- **BLE 固件加载**：ext_patch2 灌包判据按表内 id（修第二条 ext patch 漏灌）；新增 `DBG_BT_*` 日志模块。
- **RTOS 端口**：任务状态判据改用 RTOS 原生状态值（修 AP stop 卡死）。

### 三、构建与交付

- 顶层 `AIC_BUILD_MBEDTLS_PORT` 统一默认值并透传到子构建；`lib` 目标接入源清单↔归档自动对账
  （`check-src-coverage`，缺对象即报错）。
- 源-归档对账脚本支持 IAR `iarchive --toc`、`@响应文件`、路径含空格。
- CMake：mbedtls port 源漏列、`wifi_port_plt.c` 门控、`plat_port.c` 口径、引用模式 SDIO-only 声明等
  与 Makefile 对齐；`docs/aic8800-release-adaptation` 同步更新。

---

## 20260929（`g0e6ddbe`）— 记录起点

本版本作为本变更记录的起始基线，内容以该版交付包为准。此后所有版本变更按上文体例追加。
