# AIC8800D80 —— 芯片被复位（外部供电不足 / brownout）

> **结论（用户实测确认）**：模组由 **USB 扩展坞**供电，同一路 USB 口还带了键盘，**供电能力不足**。
> 芯片在 `sta conn` 的 **DPD 射频校准**（峰值电流）时电压跌落 → 掉电复位。
>
> **伪装性**：芯片复位在主机侧表现为"SDIO 命令超时"，并直接触发驱动的断言死循环，
> **极其容易被误判成总线故障、固件 bug 或驱动移植问题**。本文记录完整判据链，供下次快速识别。
>
> 日期：2026-10-13　板型：`sf32lb55-lcd_n16r4n4`　模组：AIC8800D80N

---

## 1. 症状（两路日志）

### 1.1 HCPU 侧

```
09:44:06:588  [AIC][APP]SetWifiConfigFlag 1
09:44:06:590  [AIC][FHOST]Connect Wi-Fi: jia-ni, 240200408
09:44:06:593  [AIC][NETAL]aicwf_net_if_netdev_add: netdev 'wl' registered
              ... 扫描正常、拿到扫描结果 ...
09:44:10:088  [FW] join:jia-ni,2412,bw=0
09:44:10:094  [FW] misc dpd calib single      ← ★ 射频校准开始
09:44:10:094  [FW] dpd start
09:44:10:243  [FW] t1
09:44:10:495  [FW] t2
09:44:10:598  [AIC][SDIO]__aicwf_sdio_readb_cmd52 fail -116!   ← 开始，共 15 条
              [AIC][SDIO]aicwf_sdio_rx_task, ret=0, intstatus=0   （与上一条交替）
              ... 15 条 ...
09:44:10:786  [AIC][CMN]Assertion failed: 0, file ...\platfor...
09:44:16 起   TX: 后面没有任何回显（按回车无反应 = 系统已死）
09:44:20:079  [AIC][FHOST]WPA network 0: connection failed (disconnected)
09:44:20:093  [AIC][APP]app_wlan_sta_start: start sta fail -7
```

### 1.2 芯片侧（模组自己的串口）

```
09:51:00:499  capacitance cal
09:51:00:503  Done
09:51:00:644  wifi ready
09:51:00:646  PatchVer: la Aug 28 2026 10:34:20 - g9f0ce24 - 00000000
09:51:00:729  driver start
09:51:00:729  is 5g=1, sw_en=1, vendor_info=0x0
              ↓ 11 秒（这段时间的 FW 运行日志走 SDIO 回传，只在 HCPU 侧以 [FW] 前缀出现）
09:51:11:824  Bootrom [Sep 11 2025, g38f9137]     ← ★ 全新的 ROM banner = 芯片被完整复位
09:51:11:826  RstCause:0000,c0,Boot:0067,6
09:51:11:833  fw boot start
09:51:11:837  usb init done                        ← 之后停住（在等主机枚举，而主机已经死了）
```

**芯片侧那行全新的 `Bootrom` banner 是本案的决定性证据**：模组从 ROM 重新走了一遍上电启动。

---

## 2. 完整因果链

```
外部供电不足（USB 扩展坞 + 键盘共用一路 USB 口）
   ↓  DPD 射频校准 = 峰值电流，电压跌落
模组 3.3V 掉到复位阈值以下 → 芯片掉电复位
   ↓  复位/重启期间不响应 SDIO 命令
SDHCI 报 CMD52 命令超时（SDHCI_INT_TIMEOUT）
   ↓  drv_sdhci.c:1048  host->cmd->err = -ETIMEDOUT  (= -116)
   ↓  sdio_port.c:741   打印 __aicwf_sdio_readb_cmd52 fail -116!
   ↓  sdio_port.c:384   SDIO_WR_FAIL 连续失败计数 wr_fail++（成功则清零）
   ↓  连续 16 次 → wr_fail > 15
   ↓  plat_port.c:392   aicwf_driver_unexcepted(0x04)
   ↓  plat_port.c:405   AIC_ASSERT_ERR(0)
   ↓  aic_assert.h:11   打印 + while(1)   ← 整个系统永久死亡
```

### 关键代码位置

| 环节 | 位置 |
|---|---|
| CMD52 命令超时置错 | `rtos/rtthread/bsp/sifli/drivers/drv_sdhci.c:1048`（`SDHCI_INT_TIMEOUT` → `-ETIMEDOUT`） |
| 打印失败 | `customer/peripherals/wifi/aic8800_rtos_driver/platform/hal/sdio_port.c:741` |
| **连续失败看门狗** | 同文件 `:384` 的 `SDIO_WR_FAIL` 宏 |
| 异常分发 | `platform/hal/plat_port.c:394` `aicwf_driver_unexcepted()` |
| 断言死循环 | `port/inc/aic_assert.h:11`（`AIC_ASSERT_ERR` → `while(1);`） |
| 芯片复位动作（本次**未**触发） | `platform/wifi/wifi_port.c:600` `AIC_DRIVER_REBOOT_RESET` |

### 为什么日志里正好是 15 条

`SDIO_WR_FAIL` 宏：

```c
#define	SDIO_WR_FAIL(ret){
    if(ret) { sdio_dev.wr_fail++; }
    else      sdio_dev.wr_fail = 0;
    if(sdio_dev.wr_fail > 15) { aicwf_driver_unexcepted(0x04); }
}
```

第 16 次失败时 `wr_fail` 变成 16 → `> 15` 成立 → **在打印之前就断言了**。
所以日志里**恰好 15 条** `fail -116!`，与 `> 15` 完全吻合。看到 15 条就知道下一行必然是断言。

---

## 3. 为什么容易误判（本案的教训）

芯片复位在主机侧只表现为"**命令超时**"，看不出是复位：

| 看起来像 | 实际是 |
|---|---|
| SDIO 总线/时序故障 | 芯片掉电了 |
| 驱动移植问题（我们改的代码） | 与 netdev / lwIP 完全无关（失败在 `join`，还没到 IP 层） |
| 固件 crash | 外部供电不足 |
| "运营商封 ICMP / 热点不让共享"（本案更早的一次误判） | 也是别的原因 |

**决定性证据只有两条，都在芯片侧**：
1. 模组串口出现**全新的 `Bootrom [...]` banner**；
2. `RstCause` 的取值。

---

## 4. 下次怎么快速识别（判据表）

| 观察 | 结论 |
|---|---|
| 模组串口出现新的 `Bootrom` banner | **芯片被复位了** —— 不要再往总线/协议/驱动方向查 |
| `RstCause` **每次都是同一个值**（本案 `0000,c0`，与冷启动相同） | 该寄存器没给出信息，需向 AIC 索要位定义 |
| 主机侧是"连续 N 条 `fail -116!` → 断言" | 芯片在复位期间不应答，属**下游症状** |
| 复位总发生在 `dpd start` / 射频校准 / 关联 / 发射功率爬升附近 | **峰值电流** → 优先怀疑供电余量不足 |
| 用手按住/移动扩展坞、拔掉同口外设后不复现 | 供电/接触问题 |

**一次测量定案**：示波器看 **WL_REG_ON（GPIO124）** 与 **模组 3.3V**：

| 波形 | 结论 |
|---|---|
| WL_REG_ON 全程高，芯片却打新 `Bootrom` | **芯片自己复位**（掉电/看门狗/内部异常）→ 查供电 |
| WL_REG_ON 出现下拉脉冲 | 主机或电源轨塌陷把它复位了 → 查主机恢复逻辑 / 供电 |

本案属于第一种。

---

## 5. 规避与加固

### 5.1 立即措施（本案已确认有效）

- **模组单独供电**，不要和键盘、U 盘等外设共用一路 USB 口；
- 不用供电能力不明的 USB 扩展坞；必要时用**带独立供电（外部适配器）的 USB Hub**；
- 换线径更粗、压降更小的 USB 线。

### 5.2 硬件加固

- 模组 3.3V 就近并 **100µF 电解 + 100nF 陶瓷**（DPD/发射瞬间靠它顶住）；
- 确认供电路径（LDO 能力、走线阻抗）能满足射频峰值电流；
- 有条件时实测 DPD/发射瞬间的 3.3V 纹波与跌落幅度。

### 5.3 软件韧性（可选，建议产品化时做）

当前设计有个明显脆弱点：**连续 16 次 SDIO 失败就 `AIC_ASSERT_ERR(0)` → `while(1)`，整个系统永久死亡**。
只要芯片有一次复位/卡顿，主机就陪葬，且**无法自恢复**（本案芯片重启后停在 `usb init done` 等主机，而主机已死）。

可考虑把 `plat_port.c:405` 的：

```c
case 0x04:
    AIC_ASSERT_ERR(0);
break;
```

改为与其它事件码一致的恢复动作：

```c
case 0x04:
    aicwf_aic_driver_reboot(0x03);   /* 断电重启芯片, 让主机自恢复 */
break;
```

> 属厂商代码，改动前建议先与 AIC 确认语义。**副作用**：会把真实的固件/总线故障也一并掩盖成"重启一下就好了"，
> 所以更适合作为产品化的容错，而不是调试期的默认行为。

---

## 6. 相关文档

| 文档 | 内容 |
|---|---|
| [aic8800d80_full_modification_summary.md](aic8800d80_full_modification_summary.md) | 全工程完整修改流程 + 口头汇报版 |
| [aic8800d80_freeze_and_wpa_fixes.md](aic8800d80_freeze_and_wpa_fixes.md) | 卡死（中断风暴）+ wpa 栈溢出 |
| [aic8800d80_lwip_netif_default_missing.md](aic8800d80_lwip_netif_default_missing.md) | 公网不通的真实根因（与本案无关，但曾被误判混淆） |
| 本文件 | **芯片被复位 = 外部供电不足（brownout）** |
