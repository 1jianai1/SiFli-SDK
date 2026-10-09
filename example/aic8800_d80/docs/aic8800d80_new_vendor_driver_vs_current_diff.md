# 厂家新版 AIC8800 WiFi 驱动 vs 当前工程（含历次本地修改）差异扫描报告

> 扫描对象
> - **新版（厂家 diff 包）**：`G:\Code\VScode\SF32_Code\SiFli_demos\aic8800_diff_sifli\aic8800_diff_sf32lb52x_sifli_gb4036bf_20261013\sdk_patch\platform\aic8800_rtos_driver`
> - **当前（已集成并改过很多轮的副本）**：`D:\SiFli\SDK\SiFli-SDK-WiFi\SiFli-SDK\customer\peripherals\wifi\aic8800_rtos_driver`
> - 补充对照：本工程的 12 份修改记录（`example/aic8800_d80/docs/*.md`）与 SDK 公共代码侧未提交改动（`git status`）

---

## 0. 一句话结论

新版是**在厂家干净基线上重出的一版 WiFi-only 交付包**（`NV1.1.6 STA AP  gb4036bf 20261013`，去掉 P2P，162 个归档对象），
它把本地往厂家回流的**共性修复（netif 生命周期/vif 反查/MAC 单一来源/接口名唯一/APSTA vif/uintptr_t/扫描数组越界）都吸收进去了**，
同时**新增了 rt_wlan 框架接入（`AIC8800_USE_RT_WLAN`）、ADMA 契约、`msg_fc_bypass`、扫描结构扩展、BT 日志模块**。

但它**没有**包含本工程 SDIO/上电/netdev 三条主线的本地 bring-up 修复，而且把好几个"必须保留"的本地改动**回退成了基线原样**：

| 类别 | 数量 | 后果 |
|---|---|---|
| 本地修改被厂家吸收（无需再打） | 11 项 | 直接受益 |
| 新版回退/缺失、**必须重新打回** | **17 项**（+6 项未闭环遗留，见 §10.1） | 其中 8 项会导致**起不来 / 卡死 / 链接失败** |
| 新版新增能力 | 11 项 | 可选采纳 |
| 集成布局/构建差异 | 6 项 | 直接覆盖会**双份驱动或编不过** |
| SDK 公共代码侧本地补丁（新包完全不含） | 5 个文件 | 覆盖/回滚会退化 |
| 文档 vs 代码不一致（会误导合并） | 5 项（§10.3） | 例如被文档声称"已实现"的 `nettest` 实际不在树里 |

**结论：不能把 `sdk_patch/` 直接对比覆盖。** 必须按"厂家上游修复留新版 + 本地 bring-up 修复重新贴回"的方式做**双向合并**。

---

## 1. 基线与版本确认（硬证据）

| 项 | 当前工程（旧） | 新版 diff 包 |
|---|---|---|
| 库内版本串 | `NV1.1.6 STA AP P2P ggd4509b7 20260915` | `NV1.1.6 STA AP  gb4036bf 20261013` |
| `libaic8800_driver.a` | 2,331,708 B | 1,891,782 B |
| 归档成员（`.o`） | 177 | 162 |
| P2P 痕迹 | 645 处 | 17 处 |
| `libaic_mbedtls.a` | 186,318 B | 186,318 B（仅 24 字节归档元数据差异，**内容等价**） |
| 芯片型号宏 | `CONFIG_AIC8800D80N` / `AIC_CHIP_MODEL := D80N` | `CONFIG_AIC8800D80` / `AIC_CHIP_MODEL := D80` |
| 驱动内 `.baseline_anchor` | — | `baseline=d4509b7 -> f5d2aef`，追加 `b4036bf`；`sync_date=2026-10-13` |

> 关键闭环：新版 `.baseline_anchor` 里写的起点 **`d4509b7`** 正是当前工程库版本串里的 `gd4509b7`。
> 也就是说，新版的同步链就是"从你现在这一版往上走"，中间经过 `f5d2aef`，最后到 `b4036bf`。
> `docs/sync_changelog.md` 覆盖了 `20260929 (g0e6ddbe)` → `20261009 (gcb27c2b)` → `20261013 (gf5d2aef)` 三节。

---

## 2. 文件级差异总览（161 同 / 44 改 / 35 仅旧 / 1 仅新）

### 2.1 仅存在于当前工程（新版没有）

| 类型 | 明细 |
|---|---|
| 顶层 `include/` 公开头兼容副本（20 个） | `include/aic_chip_desc.h`、`include/drv_api/inc/*.h`(11)、`include/drv_wifi/wifi_if.h`、`include/utils/inc/*.h`(6) |
| in-source 编译杂物（14 个） | `platform/**/*.o` |
| 本地文档（1 个） | `docs/sdio_detect_card.md` ← **本工程 bring-up 依据文档，别丢** |

> 顶层 `include/` 的移除是厂家**有意为之**（"公开头单一事实源 = `lib/include/`"），属改进，可接受。

### 2.2 仅存在于新版

| 文件 | 说明 |
|---|---|
| `platform/wlan/aic8800_wlan_dev.c` | **新增**：AIC8800 → RT-Thread `rt_wlan` 管理框架对接层（`rt_wlan_dev_ops`），仅在 `CONFIG_AIC8800_USE_RT_WLAN=y` 时由 SConscript 条件纳入 |

### 2.3 内容有变化的文件（44 个，按影响排序）

| 文件 | 旧→新 变化 | 判定 |
|---|---|---|
| `platform/hal/sdio_port.c` | −147 行 | ⛔ 本地 5 项 SDIO 修复被回退 |
| `platform/netif/netif_port.c` | −229 行 | ⛔ netdev 桥接被删 |
| `platform/hal/plat_port.c` | −90 行 | ⛔ 上电 GPIO / 复位被清空 |
| `platform/platform_init.c` | −37 行 | ⛔ 板级上电时序被删 |
| `port/inc/plat_port.h` | 栈大小回退 | ⛔ WPA 栈 8192→3072 |
| `platform/wifi/wifi_port_plt.c` | 轮询→阻塞等卡 | ⛔ 可能永不返回 |
| `platform/wifi/wifi_port_sta.c` | +auth_mode/ssid_len；删 netdev_sync | ✅/⛔ 混合 |
| `platform/wifi/wifi_port.c` | API 改名 + 事件扩展钩子 | ✅/新增 |
| `lib/include/drv_api/inc/wifi_api.h` | 扫描结构扩展 | ⚠️ ABI 契约变更 |
| `platform/fmac/aic_chip_desc_port.c`、`port/inc/aic_chip_desc.h` | +`msg_fc_bypass` | ✅ 新增 |
| `SConscript` | 芯片宏/上电引脚/rt_wlan | ⛔ 丢 Kconfig 注入 |
| `customer/peripherals/wifi/Kconfig`、`project/proj.conf` | 见 §5 | ⛔ 勿覆盖 |
| `Makefile`/`CMakeLists.txt`/`BUILD.md`/`lwipopts.md`/`aic_cfg.mk`/`platform_config*.mk`/`rtos_*.c`/`docs/*` | 见 §3、§4 | 逐项 |

---

## 3. ✅ 已被新版吸收的本地修改（无需再打回）

从注释标记 `[AIC8800-PORT][BUGFIX]` → `[BUGFIX] (共性: xxx)` 可以逐条对上：

| # | 内容 | 新版位置 |
|---|---|---|
| U1 | netif 生命周期：`aicwf_net_if_release_all()` 回收槽位；`down(NULL)` 语义改为下线全部活跃 netif | `netif_port.c` |
| U2 | 按真实 STA vif(idx 0) 反查：`netif_find_from_wifi_idx(0)` 用于 `get_ip` / `dhcp_done` / `netif_mode_switch` | `netif_port.c` |
| U3 | MAC 单一来源：镜像 `fhost_vif->mac_addr`，不再重复计算 | `netif_port.c` |
| U4 | 接口名唯一：不覆盖 `net_if->num`（防 APSTA 两个 `wl2` 错绑） | `netif_port.c` |
| U5 | 并发 APSTA 用真实 AP vif idx（否则 Data Abort） | `wifi_port_ap.c`、`wifi_port_wps.c` |
| U6 | ABI 宽度：`OsResSt.para0/para1`、`aic_tx_cb.frame_id` → `uintptr_t` | `rtos_port.h`、`rtos_rtt.c`、`rtos_freertos.c`、`netif_api.h` |
| U7 | 扫描结果栈越界：`uint8_t index[32]` → `index[MAX_AP_COUNT]` | `wifi_port_sta.c` |
| U8 | `CONFIG_RXTASK_INSDIO_DEFAULT` / `CONFIG_RX_POLLING_DEFAULT` / `CONFIG_IRQ_SUS_DEFAULT` 加 `#ifndef` 包裹（可 `-D` 覆盖） | `port/inc/sdio_port.h` |
| U9 | `sdio_sg_list.addr` → `.buf`；新增 `AICWF_SDIO_SG_LIST_EXTERNAL` 逃生口 | `port/inc/aic_sdio_types.h` |
| U10 | RTOS 原生任务状态判据（修 AP stop 卡死）+ `wait_task_suspend` 修嵌套挂起 | `rtos_rtt.c`、`rtos_freertos.c`、`rtos_port.h` |
| U11 | `aicwf_dbg_snprintf` 平台 porting 契约（两边都已实现，一致） | `port/inc/plat_port.h`、`platform/hal/plat_port.c` |

---

## 4. ⛔ 新版回退/缺失、必须重新打回的本地修改

> 等级：**P0 = 编不过/链接失败/起不来/卡死**；**P1 = 功能异常**；**P2 = 观测性/体验**

### P0-1 `plat_port.c` + `platform_init.c`：WiFi 上电 GPIO 与上电时序被整体清空
- 旧：`aicwf_platform_pwr_wifi_pin_init/enable/disable()` 走 `rt_pin_mode/rt_pin_write`，引脚号来自 Kconfig
  `AIC8800_RTOS_WIRELESS_PWR_GPIO`（本工程 `=124`，即 WL_REG_ON）；`platform_init.c` 里
  `INIT_BOARD_EXPORT(aicwf_wifi_power_on)` 做 拉低→10ms→拉高→200ms 的**忙等**上电（调度器未起，必须 `HAL_Delay`）。
- 新：全部变成 `// To add` 空实现，`platform_init.c` 只剩空 `aic_platform_init()`。
- 附加丢掉的保护：`aicwf_wifi_pwr_locked` —— 预编译库 `aic_wifi_open()` 内部还会再走一次
  `pin_init/disable/10ms/enable`，此时 SDIO 已枚举完（4bit 高速、RCA 已分配），一掉电就丢 RCA/总线宽度，
  主机不会再枚举 → 后续 CMD52 全部超时（实机日志 **0x18001**）。旧代码用锁 + 注释说明屏蔽之（参照 aic8800d40 被 `#if 0` 掉的同类代码）。
- 同时丢：`aicwf_sdio_pdn_wifi()` / `aicwf_sdio_reset_wifi()`（无独立复位脚 → 用电源循环等效复位，会先解锁）。

### P0-2 `port/inc/plat_port.h`：任务栈大小被回退
| 宏 | 旧 | 新 | 说明 |
|---|---|---|---|
| `FHOST_WPA_STACK_SIZE` | **8192** | 3072 | 旧注释：3072 时 `sta conn` 期间 fhost_wpa_task 用量 95%，触发 usage fault `STKOF(SCB_CFSR_UFSR:0x10)`，崩在 `fhost_wpa_create_network/fhost_wpa_start` |
| `RWNX_WIFI_STACK_SIZE` | **4096** | 1024 | 同步下调 |

> 这是**纯本地修复**（厂家 changelog 里没有对应项），回退后 WPA 握手阶段极易复现 HardFault。

### P0-3 `netif_port.c` + `netif_port.h` + `wifi_port_sta.c`：RT-Thread netdev 桥接被整块删除
旧版新增并调用的三个函数在新版**完全不存在**：
- `aicwf_net_if_netdev_add(netif)` / `_del(netif)` / `aicwf_net_if_netdev_sync(net_if)`
- `netif_port.h` 里的 `void aicwf_net_if_netdev_sync(net_if_t);` 声明也已删除
- `wifi_port_sta.c` 里 DHCP 成功后的 `aicwf_net_if_netdev_sync(NULL)` 调用被删

为什么这是 P0（旧文档已定位三条连锁后果）：
1. 本 SDK 的 lwIP 把 DNS 存在 **netdev** 上：`dhcp_handle_ack()` → `netdev_set_dns_server(netdev_get_by_name(netif->name), ...)`
   查不到就 `RT_ASSERT(netdev)` 直接断言 → **DHCP ACK 到达瞬间 tcpip 线程 fatal error**（实测现象）。
2. `netdev_register()` 在 `netdev_default==NULL` 时内部 `netdev_set_default()` → `ops->set_default()` → `netif_set_default()`；
   若注册时 `nd->ops` 还是 NULL 这一步被静默跳过 → `netif_default` 恒 NULL →
   `ip4_route()` 对**任何跨网段目的地**返回 NULL，`sendto()/connect()` 0ms 失败、`errno=118 (EHOSTUNREACH)`。
   （典型误判："能 ping 通网关，但任何公网地址 0ms 失败"，且 DHCP/DNS 到网关都正常。）
3. `aicwf_net_if_init()` 是**直写** `NETIF_FLAG_LINK_UP`，绕过 `netif_set_link_up()`，
   本 SDK 只在后者的路径里调 `netdev_low_level_set_link_status()` → netdev 侧恒 LINK_DOWN →
   netdev 版 ping 一个包都不发就报 `ping: network interface device(wl) status error.`；`ifconfig` 显示 LINK_DOWN / 0.0.0.0。

> 新版把这条路换成了 `#ifdef AIC8800_USE_RT_WLAN`（netif 交给 rt_wlan 框架创建）。
> 但新版 `proj.conf` **仍然保留 `CONFIG_RT_USING_NETDEV=y`**，而默认（`AIC8800_USE_RT_WLAN` 未开）仍是"路线1 驱动自建 netif"。
> 也就是说：**默认配置下 netdev 桥接必须是存在的那一份**，新版把它拿掉了。

### P0-4 `sdio_port.c`：SDIO(DAT1) 中断风暴规避被删
- 旧：全局 `uint32_t g_sdio_irq_mask_forced`，在 `aicwf_sdio_isr()` 与 `aicwf_sdio_host_enable_isr()` 里拦截
  `enable=1`，即"不放开 SDIO 中断"，收包改走库内 10ms 轮询；配套 `aicwifi_sdio_rxdata_mode=1`
  （旧 `example/.../src/main.c` 的 `AIC_SDIO_IRQ_MASK_FORCED` 开关 + `aic_sdio_irq_policy_apply()`）。
- 新：`g_sdio_irq_mask_forced` 与两处拦截分支全删；新 `main.c` 里也没有这个开关。
- 症状（旧文档明确）：`SDHCI_INT_CARD_INT` 是**电平**中断，库 `aicwf_sdio_rx_task` 每轮结束无条件
  `sdio_host_enable_isr(TRUE)` → "中断→唤醒RX→读 intstatus=0→再放开→立刻又中断"死循环；
  该中断 NVIC 优先级高于串口 → 串口 RX 永远得不到服务 → **只在串口打印一次 `msh />`，之后回车无回显（"整机卡死"）**。

### P0-5 `sdio_port.c`：D80N（卡级 PID 0x9082）flow-ctrl Q1→Q2 读重定向被删
- 旧：`aicwf_sdio_readb_cmd52()` 里对 `SDIOWIFI_FLOW_CTRL_Q1_REG_V3(0x03)` 做 0x03→0x09 读侧重定向，
  并带 `AIC_FC_Q1_AFTER_FWBOOT`（补丁 FW 起来后停止重定向、直读真实 Q1）、
  `AIC_FC_RAW_DBG` 探针、以及**前 5 次必须先哑读 Q1/Q2**（旧注释强调这是 ROM/CINIT 阶段 Q2 给出信用的必要条件）；
  由 `aicwf_sdio_enable_irq_after_fw_boot()` 清零 `g_fc_redirect_q2`。
- 新：`sdio_port.c` 中 **`flow_ctrl_reg` / `FLOW_CONTROL_Q*` / `g_fc_redirect_q2` 全部消失**（grep 零命中）。
- 厂家替代方案是**另一条路径**：`aic_chip_desc.msg_fc_bypass`（仅 D80N=1，走 host 库内部 `sdio_co.c aicwf_sdio_tx_msg` 免流控）。
- ⚠️ **静态无法判定**：新 host 库是预编译的，看不到源码。旧文档记录的问题不仅是 msg 通路，
  还有**数据通路**判据 `fc_reg > 2`（Q2=2 恒不通过 → EAPOL msg2/4 发不出 → 4WAY_HANDSHAKE_TIMEOUT、reason=15）。
  `msg_fc_bypass` 只覆盖 msg 通路。**必须在实机上重新验证握手/连接**（见 §8 待验证项）。

### P0-6 `sdio_port.c`：`aicwf_sdiov3_func_init()` 的 func 查找被回退
- 旧（本平台必要）：
  ```c
  /* 本卡 func1 的 CIS 只有 FUNCID/FUNCE, 没有 CISTPL_MANFID, product 恒为 0,
     __aicwf_sdio_find_func() 按型号必然查不到 */
  func = sdio_function_local[FUNC_1];
  if (!func) { ret = -ENODEV; ... }
  ```
- 新（基线原样）：
  ```c
  func = __aicwf_sdio_find_func(0x7, SDIO_VENDOR_ID_AIC8800D80,
              (chipid == PRODUCT_ID_AIC8800D80N) ? SDIO_DEVICE_ID_AIC8800D80N
                                                  : SDIO_DEVICE_ID_AIC8800D80);
  if (!func) { ret = -ENODEV; DBG_SDIO_ERR("%s: __aicwf_sdio_find_func func0 failed!!(%d)\n", ...); }
  ```
- 本平台 card function 级没有 MANFID → `product == 0` → 该查找必然失败 → **probe 直接 `-ENODEV`**。

### P0-7 `sdio_port.c`：SDIO 功能号数组下标上界被回退
- 旧：`num = co_min(card->sdio_function_num, (uint8_t)(SDIOM_MAX_FUNCS - 1));` 且 `for (i = 0; i <= num; i++)`
  （注释：平台数组下标即功能号，`[0]=FUNC0`，合法上界含 `sdio_function_num`）。
- 新：`num = co_min(card->sdio_function_num, SDIOM_MAX_FUNCS);` 且 `for (i = 0; i < num; i++)`。
- 旧版是**修正过的**，新版是回退。

### P0-8 `sdio_port.c` + SDK `sdio.c`：卡识别表首条 `SDIO_ANY_PROD_ID` 被回退
- 旧：`{DEVICE_FUNC_CODE, SDIO_VENDOR_ID_AIC8800D80, SDIO_ANY_PROD_ID}` + 注释说明
  "平台 `sdio_match_card()` 只比卡级(func 0)公共 CIS 的 product，且只取本表首条；具体型号由 probe 内 `aicwf_sdio_chipmatch()` 判定"。
- 新：改回 `SDIO_DEVICE_ID_AIC8800D80`。
- 这条是**成对**的：SDK 侧 `rtos/rtthread/components/drivers/sdio/sdio.c` 被本地改过以支持 `SDIO_ANY_PROD_ID`
  并在 function CIS 无 MANFID 时回退用卡级 product（见 §7）。新版 diff 包**不含任何 SDK 侧改动**，
  所以这两个必须一起看，不能只改一边。

### P1-9 `wifi_port_plt.c`：等卡方式从"轮询"回退成"永久阻塞"
- 旧（本地修复）：
  ```c
  extern uint8_t mmcsd_get_stat(void);   /* mmcsd_core.c:632，头里没声明，局部声明 */
  int timeout = 50;                      /* 最多等 5s */
  while (timeout-- > 0) { if (mmcsd_get_stat()) break; aicwf_rtos_msleep(100); }
  ```
  注释明确：`mmcsd_core.c` 只在 **SD 卡/eMMC** 探测成功和拔卡分支向 `mmcsd_hotpluge_mb` 投递消息，
  **SDIO 探测分支不投递** → 纯 SDIO 场景下 `mmcsd_wait_cd_changed()` 永不返回，主流程卡死。
  做法对齐 `customer/peripherals/wifi/swt6621s/wifi_device.c` 的 `rt_check_sdio_ready()`。
- 新（基线原样）：
  ```c
  for (;;) {
      status = mmcsd_wait_cd_changed(RT_WAITING_FOREVER);
      if (status == MMCSD_HOST_PLUGED) { DBG_SDIO_INF("detect sdio device\r\n"); break; }
  }
  ```
  新版自己的注释就写着 `TODO[HW]: ...若上电卡已就绪而无新热插拔事件此等待或不唤醒...`（README 注意事项 4 同）。
- **等于厂家把这个已知坑又放回来了。**

### P1-10 `plat_port.c`：`aicwf_ATRESP()` 输出通道变更（厂家列为"必做动作 #4"）
- 旧：`#ifdef RT_THREAD  rt_kprintf("%s\n", buf);`（始终打到控制台）
- 新：`DBG_APP_INF("%s\n", buf);`（经 AIC 调试日志，串口抓包只采 `[AIC][*]`）
- 后果：扫描/连接结果（`+OK`、`[n]:rssi=...`）在控制台看不到。需在该函数内自行补 `rt_kprintf`。
- 顺带：`aicwf_log_printf()` 里旧版被注释掉的 `"Interrupt but no data"` 过滤在新版被解开（无过滤），会把串口刷屏。

### P1-11 `workdiff`：GPIO 触发接口改为 id→pin 映射表（厂家"必做动作 #5"）
- 新版 `aicwf_plat_gpio_trigger()` 新增 `AIC_TRG_GPIO_*` → `DRV_TRG_*` 映射表，但**实际输出调用仍是 `(void)pin; (void)value;`（未实现）**。
- 这是**新增的待办**，不是回退：如果本工程有 GPIO 调试触发需求，需要按 `[PLATFORM]` 标注处补实现。

### P1-12 公共 API 契约变更：`aic_wifi_set_mode` → `aic_wifi_mode_set`
- 旧平台层 13 处调用 `aic_wifi_set_mode()`，新版全部改为 `aic_wifi_mode_set()`；
  同时新版删掉了 `wifi_port.c/-ap/-sta/-p2p` 里本工程为基线补的 `#include "wifi_if.h"`。
- 这是**host 库与 platform 层必须同版本重编**的契约变更：**不能只换 `.a` 或只换 platform 源码**。
- 若保留旧 `wifi_if.h` 又混用旧 platform 源码，会报 `undefined reference to aic_wifi_set_mode`（README 排查表已列）。

### P1-13 芯片型号目标变更：`D80N` → `D80`（**不是化妆品，会换固件**）
- `CONFIG_AIC8800D80N` → `CONFIG_AIC8800D80`；`AIC_CHIP_MODEL := D80N` → `D80`（`platform_config*.mk`、`aic_cfg.mk`）。
- `platform/fmac/aic_fw_port.c`（新旧**完全一致**）里，这两个宏选择**不同的固件头文件表**
  （`#if defined(CONFIG_AIC8800D80)` / `#if defined(CONFIG_AIC8800D80N)` 各一套 `FW_PATCH`/`FMACFW` 指针）。
- 本板实测卡级 PID = `0x9082` → 新代码映射为 `SDIO_DEVICE_ID_AIC8800D80LN` → `PRODUCT_ID_AIC8800D80N`。
  **即运行时型号是 D80N，但新版编译期按 D80 选固件** → 型号与固件不匹配。
- ⚠️ 合并时**必须把芯片型号宏改回 D80N**（或明确确认本板换成了 D80）。

### P1-14 `wifi_port.c`：`CONFIG_AIC8800D80N` 的 WPS ModelName 分支被删
- 旧：`#elif defined(CONFIG_AIC8800D80N)` → 返回 `"AIC-8800-D80N"`。
- 新：只保留 D80 分支 → `"AIC-8800-D80"`。D80N 型号下 WPS 上报型号名会错。

### P1-15 `SConscript`：`AIC8800_RTOS_WIRELESS_PWR_GPIO` Kconfig 注入被删
```python
# 旧
pwr_gpio = GetConfigValue('AIC8800_RTOS_WIRELESS_PWR_GPIO')
if pwr_gpio != '':
    CPPDEFINES += ['AIC_WIRELESS_PWR_GPIO=' + str(pwr_gpio)]
# 新 → 整段替换为 rt_wlan 条件纳入
if GetDepend('AIC8800_USE_RT_WLAN'):
    src.append(cwd + '/platform/wlan/aic8800_wlan_dev.c')
    CPPDEFINES.append('AIC8800_USE_RT_WLAN')
```
- **两段互不冲突，应该并存**。删掉上电引脚注入 = 上电功能彻底不可用（与 P0-1 叠加）。

### P2-16 `SConscript`：移除 `FW_LOG_AUTO_EN=1`
- 这是**厂家有意的策略变更**：host 侧不再自动使能 FW 日志，改由平台在 `wifi_port_plt.c` init 后显式
  `aic_wifi_fw_log_enable()`。本工程旧 `main.c`/`wifi_port_plt.c` 本来就显式调了，所以**行为兼容**，
  但新版在该调用外层**新增了 DC/DW 门控**（`aic_chip_desc_chipid_get() != AIC_CHIP_ID_8800DC/8800DW`），值得抄过来。
- 同时 `aic_cfg.mk` 删掉了 `AIC_FW_LOG_AUTO_EN` 开关块；`Makefile` 相关段用于重编 host，用预编译库时无影响。

### P2-17 `wifi_port_plt.c`：`aicwf_net_if_plat_init()` 调用被打开
- 旧：`// aicwf_net_if_plat_init();`（注释掉）
- 新：`aicwf_net_if_plat_init();`（启用）
- 需要确认这是**有意修复**还是与本地 netif 流程冲突；建议保留新版（启用）并在实机验证。

---

## 5. 集成布局与构建差异（直接覆盖会出事）

| # | 项 | 当前工程 | 新版 diff 包 | 风险 |
|---|---|---|---|---|
| L1 | 驱动目录位置 | `customer/peripherals/wifi/aic8800_rtos_driver/` | `platform/aic8800_rtos_driver/` | **同时存在 = 两份驱动 = 重复符号/两份 netif 逻辑** |
| L2 | SDK 顶层 `platform/` | **不存在** | 新增 `platform/SConscript`（模块遍历入口） | 需要新建顶层目录；若不想要新布局，用 `customer/peripherals/wifi/SConscript` 走现有遍历即可（它已经会遍历子目录 `SConscript`） |
| L3 | `customer/peripherals/wifi/Kconfig` | 3671 B，含 `AIC_WLAN_AIC8800D40L`、`BSP_WIFI_SWT6621`、`AIC8800_RTOS_WIRELESS_PWR_GPIO` | 1816 B，**是子集**：只剩 `WIFI_USING_AIC8800_RTOS` + 新增 `AIC8800_USE_RT_WLAN` | ⛔ **整文件覆盖会删掉现有其它 WiFi 选项**；README/manifest 也写明"手动合并，勿覆盖" |
| L4 | `example/aic8800_d80/project/proj.conf` | 25 行，含 SDMMC2/SDIO2_CARD_MODE=2/SD2_MAX_FREQ=24M/RT_SDIO_THREAD_PRIORITY=9/RT_SDIO_DEBUG=y/PWR_GPIO=124 | 50 行，含 `BSP_USING_SD_LINE=y`、`RT_LWIP_DHCP/DNS=y`，**不含**上面 6 项 | ⛔ 覆盖会丢掉 SDIO 控制器选型与上电引脚 |
| L5 | SDIO 控制器路线 | **SDMMC2 / SDHCI**（`BSP_USING_SDMMC2=y` + `SDIO2_CARD_MODE=2` + `BSP_USING_SDHCI2`），依据 `docs/sdio_detect_card.md`，且 SDK 侧 `drv_sdhci.c`/`mmcsd_core.c`/`sdio.c` 已按此打过补丁 | **线性 SDIO**（`BSP_USING_SD_LINE=y`），README 称"LB52X 无 SDHCI" | ⚠️ 两条路线**互斥**，必须选一条。现有工作配置走 SDHCI-and-proven；新包走线性 SDIO 且**未验证** |
| L6 | 示例 `src/main.c` | 带 `AIC_SDIO_IRQ_MASK_FORCED` 开关、`nettest` socket 相关 include | 精简版（55 行） | 覆盖会丢 P0-4 的开关（与 P0-4 叠加） |

> `customer/boards/eh-lb525/hcpu/board.conf` 里**没有任何 SDIO 配置**，所以 SDIO 选型完全由 `proj.conf` 决定 —— L4/L5 的差异是实打实的行为差异。

---

## 6. 新版新增能力（可选采纳，但要知道代价）

| # | 能力 | 位置 | 备注 |
|---|---|---|---|
| N1 | **rt_wlan 框架接入**（路线2） | 新文件 `platform/wlan/aic8800_wlan_dev.c`；`SConscript` 条件纳入；`wifi_port.c` 新增 `aic_wifi_ext_event_register()` + `m_ext_event_cb`；`netif_port.c` RX 走 `rt_wlan_dev_report_data()`、TX 走 `ops->wlan_send` | 开 `CONFIG_AIC8800_USE_RT_WLAN=y`（自动 `select RT_USING_WIFI`）。**边界明确：仅当关闭时才回到"路线1 驱动自建 netif"**，而路线1 恰恰缺 netdev 桥接 → 这就是 P0-3 的根因 |
| N2 | `msg_fc_bypass` chip_desc 字段 | `port/inc/aic_chip_desc.h`、`lib/include/aic_chip_desc.h`、`platform/fmac/aic_chip_desc_port.c` | 表里 6 条：8801/DC/DW/D80/D80X2 = 0，**D80N = 1**。厂家列为 D80N 必做（否则 boot 卡死） |
| N3 | 扫描结构扩展（契约变更） | `lib/include/drv_api/inc/wifi_api.h` | `ssid[32]`→`ssid[WIFI_MAX_SSID_LEN+1]`（33）、新增 `ssid_len`、新增 `auth_mode`（新枚举 `aic_wifi_auth_mode_e`）；`wifi_ap_list_t` 布局/`sizeof` 随之变化 |
| N4 | AT 扫描输出加 `AUTH=` / `SSID_LEN=` | `wifi_port_sta.c` | `AUTH=` 放在 SSID 前（SSID 可含空格）；`SSID_LEN=0` 表示隐藏 AP |
| N5 | **ADMA 契约显式化** | `port/inc/sdio_port.h`（大段契约注释）、`port/inc/aic_netbuf.h`（`data_oprt`）、`port/inc/aic_sdio_types.h`（`.buf`/`AICWF_SDIO_SG_LIST_EXTERNAL`）、`sdio_port.c` 里两处 `#error "CONFIG_SDIO_ADMA: implement ..."` | 未开 `CONFIG_SDIO_ADMA` 时是死代码 + 防误配守卫，**对本平台无影响** |
| N6 | BT 日志模块 | `port/inc/aic_log.h`：`AIC_MOD_BT (1<<14)`、`DBG_MOD_IDX_BT`、`DBG_BT_CRT/ERR/WRN/INF/VRB` | WiFi-only 下无影响 |
| N7 | DC/DW FW 日志门控 | `wifi_port_plt.c`：`#include "aic_chip_desc.h"` + `if (chipid != AIC_CHIP_ID_8800DC && != 8800DW) aic_wifi_fw_log_enable(...)` | 建议抄回（旧版是无条件调用） |
| N8 | 头单一事实源 + `port/*.c` 随包 | 顶层 `include/` 移除；`Makefile` 里 `check-src-coverage` 接入 `lib` 目标；`dist` 支持携带 `port/*.c` 且断言 `! -d release/include` | 构建期改进，无运行影响 |
| N9 | `AIC_BUILD_MBEDTLS_PORT` 透传 | `Makefile`（`?= 1` 并显式传给子 make） | 修"归档 177 成员 vs 对账 174"的不一致；**用预编译库时无影响** |
| N10 | CMake 开关表对齐 | `CMakeLists.txt`（+155 行）、`BUILD.md` | 本工程走 scons，参考即可 |
| N11 | `docs/SYNC_CHANGELOG.md`（客户可见发布记录） | 新版 `docs/`，含 20260929/20261009/20261013 三节的"平台侧必做动作" | **必读**，是本报告 §4 的重要交叉来源 |

> ⚠️ 大小写陷阱：`docs/SYNC_CHANGELOG.md` 与 `docs/sync_changelog.md` 仅大小写不同。
> 当前 Windows 文件系统上解压/合并会**互相覆盖**，需要保留两份内容。

---

## 7. SDK 公共代码侧本地补丁（新包完全不含，必须保留）

`git status` 显示工作区有 4 处**未提交**的 SDK 公共代码改动，它们是 AIC8800 SDIO bring-up 的必要条件：

| 文件 | 改动 | 性质 |
|---|---|---|
| `rtos/rtthread/components/drivers/sdio/sdio.c` | **`sdio_match_card()` 产品号匹配逻辑重写**：支持 `SDIO_ANY_PROD_ID` 直通；产品号默认取卡级公共 CIS；function CIS 无 `CISTPL_MANFID`（product==0）时回退用卡级 product；只有确实填了 MANFID 才以 function 级为准。另加 `[TEMP-DIAG]` 的 `card registered: man/prod/f1prod/funcs` 日志 | **功能性必需**（与驱动侧 P0-8 成对） + 诊断 |
| `rtos/rtthread/components/drivers/sdio/mmcsd_core.c` | `[TEMP-DIAG]`：`init_sdio()` 返回值打印 `LOG_I("init_sdio ret=%d", ret)` | 诊断，**可清理** |
| `rtos/rtthread/bsp/sifli/drivers/drv_sdhci.c` | 只把 `//#define DRV_DEBUG` 改成 `// #define DRV_DEBUG`（格式化，无功能变化） | 噪音，**可回滚** |
| `rtos/rtthread/components/utilities/ulog/ulog.c` | `[FORENSIC]` `ULOG_FORENSIC_REENT`：`output_lock()` 重入时 dump `ICSR.VECTACTIVE`/nest/锁持有者/栈上 Thumb 返回地址，直接写 console 设备避重入 | 临时取证，**定位到重入方后应移除** |
| `customer/boards/sf32lb55-lcd_base/bsp_pinmux.c` | 注释掉 `HAL_PIN_Set(PAD_PA44, GPIO_A44, PIN_NOPULL, 1)` | 与 LB55x 板调试相关，确认是否还需要 |

> 另外 `middleware/bluetooth` 显示为 modified（子模块状态），与本驱动无关。

---

## 8. 合并操作建议（按顺序）

**准备**
1. 备份：`platform/`（若存在）、`customer/peripherals/wifi/aic8800_rtos_driver`、`customer/peripherals/wifi/Kconfig`、
   `example/aic8800_d80/{project,src,docs}`，以及 §7 的 5 个 SDK 文件。
2. 决定**驱动落点**：继续用 `customer/peripherals/wifi/aic8800_rtos_driver/`（推荐，SDK 现有遍历已覆盖），
   还是迁到新版 `platform/aic8800_rtos_driver/`。**二选一，绝不并存**（L1）。
3. 决定**SDIO 路线**：保留现有 SDMMC2/SDHCI（已验证），**不要**采纳新版的 `BSP_USING_SD_LINE`（L5）。

**合并（先取厂家上游，再贴回本地）**
4. 把新版驱动目录的**源码/头文件/库/docs**覆盖到现有驱动目录 —— 但**跳过**：
   `SConscript`、`customer/peripherals/wifi/Kconfig`、`proj.conf`、`main.c`（这 4 个手工合并）。
5. `SConscript` 合并要点：保留新版（`CONFIG_AIC8800D80`→**改回 D80N**、`FW_LOG_AUTO_EN` 已在 `aic_cfg.mk` 删除、
   `AIC8800_USE_RT_WLAN` 条件块、wlan CPPPATH），**并把旧的 `AIC8800_RTOS_WIRELESS_PWR_GPIO` 注入段加回**。
6. `Kconfig` 合并要点：在现有文件 `endmenu` 之前/之内加入 `WIFI_USING_AIC8800_RTOS` + 新增子项 `AIC8800_USE_RT_WLAN`
   （`select RT_USING_WIFI`），**保留** `AIC8800_RTOS_WIRELESS_PWR_GPIO` 与 `AIC_WLAN_AIC8800D40L`/`BSP_WIFI_SWT6621`。
7. `proj.conf` 合并要点：保留 SDMMC2/SDIO2_CARD_MODE/SD2_MAX_FREQ/RT_SDIO_THREAD_PRIORITY/RT_SDIO_DEBUG/PWR_GPIO=124，
   采纳新版的 `RT_LWIP_DHCP=y`/`RT_LWIP_DNS=y` 与 `# CONFIG_AIC8800_USE_RT_WLAN is not set`。
8. **逐条贴回 §4 的 P0/P1 项**（建议顺序：P0-1/2 上电与栈 → P0-3 netdev 桥接 → P0-6/7/8 SDIO 探测与匹配 →
   P0-4/P0-5 中断风暴与 flow-ctrl → P1-9 轮询等卡 → P1-10 ATRESP 控制台 → P1-12 至 P1-15）。
9. `main.c` 合并要点：保留 `AIC_SDIO_IRQ_MASK_FORCED` + `aic_sdio_irq_policy_apply()`（P0-4 的运行期开关），
   其余可采纳新版精简骨架。
10. §7 的 SDK 公共代码改动**全部保留**；其中 `mmcsd_core.c`/`ulog.c` 的 `[TEMP-DIAG]`/`[FORENSIC]` 待问题闭环后单独清理。

**验证**
11. `CONFIG_PKG_USING_MBEDTLS` 保持关闭（避免 `aes_*`/`utils.c` 符号冲突）。
12. lwIP 强制项（新版 `lwipopts.md` §8 新增，**建议照做**）：
    `CHECKSUM_BY_HARDWARE` **不定义**（否则 DHCP 零 OFFER）；`LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS=1`（否则 `htons` 垫片被宏化、链接失败）。
13. 编译：`. ./export.sh` → `scons --board=<board> -j8`（`rtconfig.py` 是空占位，**必须带 `--board`**）。
14. 实机：串口 shell 可交互（验 P0-4）→ 驱动版本串（验库替换）→ 扫描（验 N3/N4）→ 连接 + DHCP + 公网 ping（验 P0-3/P0-5）。

---

## 9. 待实机验证项（静态不可判定）

| # | 项 | 为什么静态判不了 | 验证方法 |
|---|---|---|---|
| V1 | 新版 host 库是否已自行解决 D80N **数据通路** flow-ctrl（旧 `fc_reg > 2` 问题） | host 库是预编译的，无源码；`msg_fc_bypass` 只覆盖 msg 通路 | 连接 WPA2 AP，观察是否 `4WAY_HANDSHAKE_TIMEOUT / reason=15`；必要时把旧的 `AIC_FC_Q1_AFTER_FWBOOT` 重定向贴回 |
| V2 | `mmcsd_wait_cd_changed(RT_WAITING_FOREVER)` 在本板是否真的会唤醒 | 取决于 MMC 核心是否在 SDIO attach 时投递热插拔消息 | 直接跑，看是否卡在 `aicwf_aic_wifi_bus_waitready()` |
| V3 | `aicwf_sdiov3_func_init()` 回退后本卡 func1 是否真查不到 | 取决于卡的 CIS 内容 | 看是否打印 `__aicwf_sdio_find_func func0 failed!!` / `-ENODEV` |
| V4 | `CONFIG_AIC8800D80` 固件能否驱动 D80N 芯片 | 取决于固件与芯片兼容性 | 要么改回 D80N，要么实机看 boot 是否成功 |
| V5 | `aicwf_net_if_plat_init()` 启用是否与本地 netif 流程冲突 | 需运行期观察 | 打开后看 netif 是否重复注册 / ifconfig 是否正常 |
| V6 | 去 P2P 后是否有上层功能依赖 P2P | 本工程是 WiFi-only 场景 | 核对应用需求 |

---

## 10. 补充：把 12 份修改记录逐条对照后的增量发现

> 本节是对 §4/§5 的补充（对 12 份 `docs/*.md` 逐条对照 + 在当前树中实地复核）。

### 10.1 §4 需要补充的"新版同样缺失"的本地修复

| 编号 | 内容 | 位置 | 为什么重要 |
|---|---|---|---|
| P1-18 | **`AIC_FC_FORCE_Q2` + `aic_fc_fix_thread`（已删除）** | 旧 `example/.../src/main.c` | 该线程每 500ms 重写 `sdio_dev[8]`，只有 ~ms 余量，**已被判定为不可靠并删除**。说明 B5/P0-5 这条线**不能靠"把旧 main.c 整体贴回"解决**，必须用官方 `msg_fc_bypass` + 实机验证数据通路 |
| P1-19 | **被否决的写侧方案**：在 `aicwf_sdiov3_func_init()` 里写 `sdiodev->sdio_reg.flow_ctrl_reg = Q2` | 旧 `sdio_port.c` | 已试验并否决（库始终把该字段保持 0x03）。**不要**在合并时"顺手补回"写侧代码，只能用读侧重定向 |
| P1-20 | `CONFIG_PRIO_HIGH_IS_LARGE` 默认 = 1（FreeRTOS 语义，与 RT-Thread 相反） | 驱动 `userconfig.h`/配置 | 导致驱动任务优先级落在 19~31（`aicwf_sd=30`、`aicwf_wi=31`、idle=31）。记录里**建议置 0**，但至今未改；新版也没有改。属**未闭环遗留项** |
| P1-21 | `plat_port.c` 的 `case 0x04` 只 `AIC_ASSERT_ERR(0)` → `while(1)` 永久死机（**仅提议，未实现**） | `platform/hal/plat_port.c` | 链路：`sdio_port.c` `SDIO_WR_FAIL if(wr_fail>15) aicwf_driver_unexcepted(0x04)` → 恰好 15 行 `fail -116!` 后第 16 次进断言死循环。记录里提议改为 `aicwf_aic_driver_reboot(0x03)`，**新版也没做** |
| P1-22 | `aicwf_sdio_readb_cmd52_by_num()` 在 `wifi_chip_status_get()==0` 时返回 false 且**不写** `*data` | `sdio_port.c` | 会**伪造出 `msg fc:0`** 的假象，排查时极易误判。新旧一致，排查时务必知道 |
| P1-23 | `CONFIG_SDMMC2_BUS_WIDTH_1_ONLY` 必须保持 **n（4-bit）** | `proj.conf` / Kconfig 默认 | `=y` 会让 `drv_sdhci.c` 跳过 `MMCSD_BUSWIDTH_4`、`sdio.c::sdio_set_bus_wide()` 跳过 CCCR 0x07 → 退回 1-bit。当前文件里该行**本就不存在**（走默认 n），**合并时别手贱加进去** |

### 10.2 §7 需要补充的 SDK/板级改动

| 项 | 事实（已实地核对） |
|---|---|
| `customer/boards/sf32lb55-lcd_base/bsp_pinmux.c` | SDIO2 引脚组（134–139 行）为 `SD2_CLK` **PIN_NOPULL**、`SD2_CMD`/`SD2_DIO0..DIO3` **PIN_PULLUP**；未提交的本地改动**只有第 169 行**：`// HAL_PIN_Set(PAD_PA44, GPIO_A44, PIN_NOPULL, 1);`（注释掉）。即"4-bit @24MHz 数据完整性"的引脚配置属 bring-up 必备，别当作噪音回滚 |
| `rtos/rtthread/components/drivers/sdio/sdio.c` | 除 §7 所述 `sdio_match_card()` 重写，还附带 `[TEMP-DIAG]` 的 `card registered: man=.. prod=.. f1prod=.. funcs=..` 日志（保留，用于验 P0-8/P0-6） |
| `rtos/rtthread/bsp/sifli/drivers/drv_sdhci.c` | 除 §7 的格式噪音，记录中还指出**真正的根治点未做**：`sdhci_irq()` 对 `SDHCI_INT_CARD_INT` 应是 **mask → 处理 → 写 SDHCI_INT_STATUS 清中断 → unmask**；现状既不 clear 也不 mask，才需要 P0-4 的软件规避 |

### 10.3 文档 vs 代码不一致（会在合并/验收时误导人，务必先对齐）

| # | 不一致 | 实情 |
|---|---|---|
| X1 | 三份文档（`aic8800d80_full_modification_summary.md` 附录 A、`aic8800d80_netif_netdev_final_changes.md` §3.4、`aic8800d80_network_connectivity_test_guide.md` §5）把 **`nettest` 命令描述为已实现** | 当前 `example/aic8800_d80/src/main.c` 只有第 17–22 行的 socket 头 include（注释写着"nettest 命令所需的 socket 头"）与 `MSH_CMD_EXPORT(wifi_test, ...)`，**没有 `nettest_worker()` / `nettest()` / `MSH_CMD_EXPORT(nettest, ...)`**。而 `nettest` 正是当初定位 `netif_default` 为 NULL 的工具 → **合并时建议一并补齐，否则 V/P0-3 的复验没有抓手** |
| X2 | 旧 `main.c` 头部注释声称"Q2 适配已在 `aicwf_sdiov3_func_init()` 中一次性设置 `sdio_reg.flow_ctrl_reg`" | 那是**已否决的写侧方案**；实际生效的是 `aicwf_sdio_readb_cmd52()` 里的**读侧重定向**（见 P0-5）。照着注释改会走错路 |
| X3 | `AIC_SDIO_REG_PROBE` / `AIC_SDIO_IRQ_MASK_TEST` / `AIC_SDIO_D1_ISOLATE` / `AIC_SDIO_D1_BIAS` 在 `aic8800d80_sdio_bringup_handoff.md` 里像现役开关 | 当前 `main.c` 中**都不存在**，那份文档是历史状态快照。现役只有 `AIC_SDIO_IRQ_MASK_FORCED` |
| X4 | 板名 | 新包 README/manifest 一律写 `--board=eh-lb525`；而旧的 bring-up 文档写的是 `sf32lb55-lcd_n16r4n4_hcpu`（`docs/sdio_detect_card.md`、工程记录）。工程自带 `SConstruct` 新旧**完全一致**、不绑定板名 → **必须与用户确认到底用哪块板**，否则 `scons --board=` 会编出不同板级/引脚配置 |
| X5 | `docs/sync_changelog.md` 内部台账里提到 `20260929` | 在 12 份工程文档 + 当前树里**搜不到任何 20260929 引用**；本工程库里能对上的只有 `NV1.1.6 STA AP P2P ggd4509b7 20260915` / `d4509b7f99235130ffc525aaf63f28caa3f047bf` / `1d16913`（P2P ABI sizeof 64→72 修复）/ ROM `g38f9137` / FW `g9f0ce24` |

### 10.4 与 §4 的关系（重要提醒）

`P0-5`（flow-ctrl）在记录里是**两段式**的：
1. **读侧重定向**（含"前 5 次哑读 Q1 再读 Q2"这个看似玄学但被 A/B 证明必要的步骤）；
2. **补丁 FW 起来后收回重定向**（`AIC_FC_Q1_AFTER_FWBOOT=1`），否则数据通路 `fc_reg > 2` 不成立 → `data fc:-2` → EAPOL msg2/4 发不出 → AP 重发 4 次 → `deauth reason=15`。

新版用 `msg_fc_bypass` 解决的是**第 1 段对应的 msg 通路**；**第 2 段的数据通路没有对应实现**。
因此合并时建议：**先只贴回第 2 段（`AIC_FC_Q1_AFTER_FWBOOT` 语义）做 A/B 对比**，用扫描/连接结果决定是否也要贴回第 1 段，
不要一次全贴（否则无法区分是厂家修复生效还是本地规避生效）。

---

## 11. 附录：扫描方法与原始证据

生成物：`example/aic8800_d80/.aic_diff/driver_src.diff`（165 KB，34 个文件的 unified diff；`-` = 当前工程，`+` = 新版）。

```powershell
# 1) 文件级三方比对（大小 + MD5）
$new='G:\...\sdk_patch\platform\aic8800_rtos_driver'
$old='D:\...\customer\peripherals\wifi\aic8800_rtos_driver'
# → 161 same / 44 changed / 35 only-old / 1 only-new
git diff --no-index --unified=3 --ignore-cr-at-eol -- <old-file> <new-file>   # 逐文件

# 2) 库版本串（把 .a 当 ASCII 读）
[regex]::Matches([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($a)),'NV1\.1\.[0-9A-Za-z \.\-_]{0,40}')

# 3) 确认 flow-ctrl 修复是否还在
Select-String -Path "$new\platform\hal\sdio_port.c" -Pattern 'flow_ctrl_reg|FLOW_CTRL_Q'

# 4) SDK 公共代码侧补丁
git -C <SDK> status --short
git -C <SDK> diff -- rtos/rtthread/components/drivers/sdio/sdio.c ... 
```

关键计数复核：`libaic_mbedtls.a` 新旧同长度、仅 24 字节差异（归档元数据）；
`libaic8800_driver.a` P2P 字符串 645→17、归档成员 177→162；
新旧 `.a` 均无 `.debug_info` 段（体积差来自 P2P 裁剪与对象数，不只是"去 -g"）。
