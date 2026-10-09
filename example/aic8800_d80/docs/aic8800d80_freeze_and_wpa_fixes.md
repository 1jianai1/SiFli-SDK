# AIC8800D80 SDIO 集成 —— 卡死 / 连接问题修复记录（终版）

> 面向接手的人：本文只记录 **最终生效的改动 + 理由 + 验证方法**。
> 调试过程中试过的临时手段（监控线程、ISR 计数、寄存器探针、holdoff 等）**已全部删除**，
> 照本文改就是终版。所有改动都在 `board=sf32lb55-lcd_n16r4n4` 下真机验证过。
>
> 日期：2026-10-12　工作目录：`SiFli-SDK/example/aic8800_d80`（下称 `$EX`），SDK 根 `SiFli-SDK/`（下称 `$SDK`）

---

## 0. 结论速览

| # | 文件 | 终版改动 | 理由 |
|---|---|---|---|
| 1 | `$EX/src/main.c` | 驱动初始化**之前**置 `aicwifi_sdio_rxdata_mode = 1`、`g_sdio_irq_mask_forced = 1`；用宏 `AIC_SDIO_IRQ_MASK_FORCED` 做 A/B 开关 | 规避 SDIO(DAT1) 电平中断风暴导致的"整机卡死" |
| 2 | `$SDK/customer/peripherals/wifi/aic8800_rtos_driver/platform/hal/sdio_port.c` | 新增全局 `g_sdio_irq_mask_forced`，并在 `aicwf_sdio_isr()` / `aicwf_sdio_host_enable_isr()` 里拦住"放开 SDIO 中断"的动作 | 闭源库每轮结束都无条件重新开中断，只能在 platform 层拦 |
| 3 | `$SDK/customer/peripherals/wifi/aic8800_rtos_driver/port/inc/plat_port.h` | `FHOST_WPA_STACK_SIZE` 由 `3072` 改为 `8192` | `sta conn` 时 `fhost_wpa_task` 栈溢出（usage fault STKOF） |

**未改动 / 已回退**：

- `platform/hal/plat_port.c`：调试期加过"空中断计数 + 触发 50 ms 屏蔽窗口"的钩子，
  **最终已全部回退**，本文件与本次修复无关。（当前树里那行
  `strstr(log_buf, "Interrupt but no data")` 过滤处于**被注释掉**的状态；它只影响日志是否刷屏，
  不影响功能，修复生效后这条日志本身也不会再出现。）
- 诊断期的 `aicmon` 监控线程、ISR 计数、func1 寄存器探针、50 ms holdoff 屏蔽窗口：**已全部删除**。
- `$EX/src/main.c` 里原先那段 `#if AIC_FC_FORCE_Q2 ...`（默认 `0` 的定时改写线程）在重写 main.c 时
  一并去掉了：它本来就是死代码，且按 `sdio_dev[8]` 硬取偏移的做法很脆弱；Q2(0x09) 的重定向现在
  完全在 `sdio_port.c` 的 `aicwf_sdio_readb_cmd52()` 里。**如需保留请先说明**。

---

## 1. 问题一：驱动起来后"整机卡死"

### 1.1 现象

初始化一路正常，日志能走到

```
[AIC][FHOST]set macaddr:cc:c4:b2:dc:17:ed
[AIC][FHOST][FW] ... (FW 日志正常回传)
[aic8800] driver up. use 'wifi_test <cmd>' to control
msh />
```

`msh />` **只打印一次**，之后反复敲回车没有任何新提示符、没有任何回显 → 表现为整机卡死。

若把 `plat_port.c` 里对 `"Interrupt but no data"` 的过滤去掉，串口会被

```
[AIC][FHOST][FW] Interrupt but no data, intstatus=0, retry_cnt=0
```

**持续刷屏**。

### 1.2 根因（链路已闭环）

1. FW 起来后会一直把 **DAT1 拉低**（SDIO 卡中断），这是**电平**中断；
2. SDHCI 控制器通过 `SDHCI_SIGNAL_ENABLE.SDHCI_INT_CARD_INT` 决定是否向 NVIC 上报该中断。
   Linux 的做法是 **ISR 里先 mask、驱动处理完再 unmask**；而本 SDK 的
   `$SDK/rtos/rtthread/bsp/sifli/drivers/drv_sdhci.c` 处理 `SDHCI_INT_CARD_INT` 时
   **既不清 `SDHCI_INT_STATUS` 也不 mask**，只做了唤醒；
3. 闭源库 `libaic8800_driver.a` 的 `aicwf_sdio_rx_task` 每轮结束都**无条件**
   `sdio_host_enable_isr(TRUE)`（同源代码见
   `$SDK/customer/peripherals/wifi/aic8800d40/fdrv/layer_common/src/sdio_co.c:1699`），
   platform 侧 `config_irq_sus = 0`（`port/inc/sdio_port.h`）也不会屏蔽；
4. 于是形成死循环：

   ```
   中断 -> 唤醒RX任务 -> 读 misc_int_status = 0 -> 再放开中断 -> 立刻又中断 ...
   ```

   （那条 `Interrupt but no data` 打印在 `sdio_co.c:1691`，分支条件是 `!polling`，
   即"信号量被唤醒"而非超时——所以"刷这条"就等于"中断在连续触发"，这就是风暴的实锤证据。）
5. 该中断 NVIC 优先级高于串口。**只要它刚退出就再次挂起，串口 RX 中断永远得不到服务** →
   tshell 收不到任何字符 → 看起来"整机卡死"。

> 注意：**不是**线程被饿死。驱动任务的优先级实际是 `SDIO_DATRX=30 / TCPIP=29`（见
> `port/inc/plat_port.h` + `CONFIG_PRIO_HIGH_IS_LARGE` 默认值），按 RT-Thread "数值小=优先级高"
> 的规则它们比 `tshell(20)` **更低**，抢不到 CPU 的说法不成立；卡死发生在**中断级**。

### 1.3 终版改动

#### (1) `$SDK/.../platform/hal/sdio_port.c`

在 `aicwifi_sdio_rxdata_mode` 定义之后新增一个全局开关（约 139-152 行）：

```c
/* ==========================================================================
 * SDIO(DAT1) 中断风暴规避 —— 只保留"不放开 SDIO 中断"这一处开关
 *
 * 原因: SDHCI 的 SDHCI_INT_CARD_INT 是电平中断(DAT1 被卡拉低期间一直有效),
 *       而库内 aicwf_sdio_rx_task 每轮结束都无条件 sdio_host_enable_isr(TRUE),
 *       形成 "中断->唤醒RX任务->读 intstatus=0->再放开中断->立刻又中断" 死循环。
 *       该中断 NVIC 优先级高于串口, 串口 RX 永远得不到服务 => shell 收不到输入,
 *       表现成"整机卡死"(只打印一次 "msh />", 之后回车无回显)。
 *
 * 规避: g_sdio_irq_mask_forced = 1 时, 本文件所有 "使能 SDIO 中断" 的路径都改
 *       成保持屏蔽; 收包由库内 10ms 轮询兜底
 *       (需同时把 aicwifi_sdio_rxdata_mode 置 1, 见 example/aic8800_d80/src/main.c)。
 * ========================================================================== */
uint32_t g_sdio_irq_mask_forced = 0;            /* 1 = 不放开 SDIO 中断 */
```

`aicwf_sdio_isr()`（约 997 行）：

```c
void aicwf_sdio_isr(uint8_t en)
{
    /* SDIO(DAT1) 中断风暴规避: 见本文件 g_sdio_irq_mask_forced 说明 */
    if (en && g_sdio_irq_mask_forced)
    {
        aicwf_mmc_enable_isr(0);
        return;
    }
    aicwf_mmc_enable_isr(en);
}
```

`aicwf_sdio_host_enable_isr()`（约 1016 行，**只加开头这一段，其余保持原样**）：

```c
bool aicwf_sdio_host_enable_isr(bool enable)
{
    /* SDIO(DAT1) 中断风暴规避: 库 aicwf_sdio_rx_task 每轮结束都会无条件
     * enable=TRUE, 电平中断下会立刻再次进入中断形成风暴; 这里直接压住不放行,
     * 收包由 aicwifi_sdio_rxdata_mode=1 的 10ms 轮询兜底。 */
    if (enable && g_sdio_irq_mask_forced)
    {
        aicwf_mmc_enable_isr(0);
        return true;
    }

    aicwf_mmc_enable_isr(enable);

    if(!enable)
    {
        ... 原样不动 ...
    }
    return true;
}
```

这两处是**平台上所有"使能 SDIO 中断"的唯一入口**：
`aicwf_mmc_enable_isr()` → `host->ops->enable_sdio_irq()` → `drv_sdhci.c:sdhci_enable_sdio_irq()`
→ `hal_sdhci_unmask_irqs(SDHCI_INT_CARD_INT)`。拦在这里，`SDHCI_SIGNAL_ENABLE` 里的 CARD_INT
始终为 0，控制器**根本不会向 NVIC 提这个中断**——不是"优先级变低再触发"，是压根不上报。

#### (2) `$EX/src/main.c`

`main()` 里、`aic_wifi_demo_test()` **之前**（约 22-58 行）：

```c
/* ---------------------------------------------------------------------------
 * SDIO(DAT1) 中断风暴规避开关
 *
 *   1 = 规避: 不放开 SDIO 中断, 收包改走库内 10ms 轮询        (默认, 实测正常)
 *   0 = 原样: 中断驱动收包 -> 复现上述卡死, 用于 A/B 对比
 *
 * 注: 置 0 时不会去改 aicwifi_sdio_rxdata_mode / g_sdio_irq_mask_forced,
 *     两者保持驱动自身默认值(CONFIG_RX_POLLING_DEFAULT=0 / 0), 即完全原始行为。
 * ------------------------------------------------------------------------- */
#define AIC_SDIO_IRQ_MASK_FORCED   1

extern unsigned char aicwifi_sdio_rxdata_mode;  /* sdio_port.c: 0=中断驱动 1=轮询 */
extern uint32_t      g_sdio_irq_mask_forced;    /* sdio_port.c: 1=不放开 SDIO 中断 */

static void aic_sdio_irq_policy_apply(void)
{
#if AIC_SDIO_IRQ_MASK_FORCED
    aicwifi_sdio_rxdata_mode = 1;
    g_sdio_irq_mask_forced   = 1;
#endif
    /* 唯一一条开关横幅, 便于 A/B 对比时确认烧的是哪个配置; 不需要可直接删 */
    rt_kprintf("[aic8800] AIC_SDIO_IRQ_MASK_FORCED=%d\n", AIC_SDIO_IRQ_MASK_FORCED);
}

int main(void)
{
    rt_kprintf("[aic8800] demo start\n");

    /* 必须在驱动初始化之前设置: 决定 SDIO RX 用中断还是轮询 */
    aic_sdio_irq_policy_apply();

    aic_wifi_demo_test();
    rt_kprintf("[aic8800] driver up. try: wifi_test iwpriv sta scan\n");
    return 0;
}
```

**为什么置 1 就能收包**：库内 `aic_sdio_rx_task` 用
`rtos_semaphore_wait(sdiodev->sdio_rx_sema, tout)`，`tout` 由 `aicwifi_sdio_rxdata_mode`
决定——为 0 时是 `FOREVER`（纯中断驱动），非 0 时是 **10 ms 超时轮询**。所以屏蔽中断后靠轮询
照样每 10 ms 读一次 `misc_int_status` 取数据：

- 实测扫描/连接时 FW 的日志、扫描结果（`[FW] 1 2412 ...`）、cfg 消息的 cfm **都能正常收到**；
- 库里"读到 `misc_int_status & SDIO_OTHER_INTERRUPT` 就写 `sleep_reg` 清卡侧软中断"的逻辑
  在轮询路径**同样会执行**，所以该清的卡侧中断还是清的。

代价：唤醒从事件驱动变成 10 ms 轮询（收包延迟最多 +10 ms），仅此而已。

### 1.4 A/B 对比与验证

```
scons --board=sf32lb55-lcd_n16r4n4 -j8          # 在 $EX/project 下，先 setup_sifli.ps1
```
- `AIC_SDIO_IRQ_MASK_FORCED = 1`：正常，`msh />` 之后回车有响应；`wifi_test iwpriv sta scan` 可用；
- `AIC_SDIO_IRQ_MASK_FORCED = 0`：**100% 复现**卡死 + `Interrupt but no data` 刷屏。

### 1.5 这是规避，不是根治

真正的病根在 SDK 侧，建议后续按 Linux `sdhci` 的语义修：

1. ISR 入口先 `mask CARD_INT`（防重入）；
2. 唤醒处理任务去读数据、清卡侧中断源；
3. 处理完确认没有待处理数据后再 `unmask`。

对应要动的是 `$SDK/rtos/rtthread/bsp/sifli/drivers/drv_sdhci.c` 的
`sdhci_irq()`（约 1267-1271 行）与 `sdhci_enable_sdio_irq()`（约 938 行）。
`g_sdio_irq_mask_forced` 属于"把中断整个换掉"，能跑但不省 CPU、也不是正解。

---

## 2. 问题二：`sta conn` 立刻 hard fault（栈溢出）

### 2.1 现象与证据

`wifi_test iwpriv sta conn jia-ni 240200408 30000` 刚进入 WPA 阶段就崩：

```
[AIC][FHOST]fhost_wpa_create_network
[AIC][FHOST]fhost_wpa_start
... Wps_context 348 ...
hard fault on thread: fhost_wp
fhost_wp  29  ready  ...  stack size 0x00000c00   max used 95%
usage fault:
SCB_CFSR_UFSR:0x10 STKOF
```

- `UFSR` bit4 = `STKOF`：线程栈越过 `PSPLIM`，**栈溢出**；
- 同一份 dump 里 `fhost_wpa` 的 `stack size 0x00000c00`(3072) / `max used 95%` 完全吻合；
- 崩在 `fhost_wpa_create_network` / `fhost_wpa_start` 内部——wpa_supplicant 的
  WPS/EAPOL/TLS 调用链局部变量很大，3 KB 不够（与 SDIO、与问题一无关）。

### 2.2 终版改动

`$SDK/customer/peripherals/wifi/aic8800_rtos_driver/port/inc/plat_port.h`：

```c
/* wpa_supplicant 任务栈: 原 3072 太小 —— 实测 sta conn 时 fhost_wpa_task
 * max used 95% 并触发 usage fault STKOF(SCB_CFSR_UFSR:0x10), 于
 * fhost_wpa_create_network/fhost_wpa_start 内部崩溃。wpa 的 WPS/EAPOL/TLS
 * 调用链局部变量很大, 给 8KB。 */
#define FHOST_WPA_STACK_SIZE        (8192)
```

校验：编译后 `plat_port.o` 的 `.data.fhost_wpa_stack_size` = `0x00002000`；
启动日志里该任务会打印 `stack_size= 2000`。堆足够（`total 910096 / used 256196`）。

> `fhost_wpa_stack_size` 是导出变量，也可以在应用层（`$EX/src/main.c`）在
> `aic_wifi_demo_test()` 前直接赋值覆盖；改宏更规范。

### 2.3 各线程栈余量（从同一次 fault dump / 实测读取，供后续参考）

| 线程 | 栈大小 | 峰值占用 | 备注 |
|---|---|---|---|
| `fhost_wpa` | 3072 → **8192** | **95%（已修）** | wpa_supplicant |
| `tshell` | 6144 | 75% | 命令行解析，后续加长命令要留意 |
| `cli_cmd` | 1024 | 60% | 偏紧 |
| `fhost_cntrl` | 2048 | 56% | |
| `fhost_rx` | 2048 | 37% | |
| `sdio_rx` / `wifi` / 其余 | — | ≤35% | |

再出现 `hard fault` + `STKOF`，就按同样方式加大对应宏。

---

## 3. 复现 / 验证步骤

### 3.1 编译

```powershell
& D:\SiFli\SDK\SiFli-SDK-WiFi\setup_sifli.ps1        # 只做环境变量导出, 不要跑 install.ps1
cd $SDK\example\aic8800_d80\project
scons --board=sf32lb55-lcd_n16r4n4 -j8
```

### 3.2 命令语法（**容易踩的坑**）

`aic_wifi_cmd()` 会丢弃整行命令的**第一个 token**（`wifi_port_cmd.c` 里
`AICWF_CMD_SKIP_PREFIX = 1`），所以第一个 token 是占位符（文档习惯写 `iwpriv`）：

```
wifi_test iwpriv uapst                    # 查 wifi st/init 状态
wifi_test iwpriv info
wifi_test iwpriv mac 0
wifi_test iwpriv switch sta               # 切模式, 打印后等 2~3s
wifi_test iwpriv sta scan                 # 扫描(阻塞, 数秒)
wifi_test iwpriv sta conn <ssid> <pwd> <timeout_ms>
wifi_test iwpriv sta stop
```

判读：

| 输出 | 含义 |
|---|---|
| `[n]:rssi=.. CH=.. BSSID=.. SSID=..` + `+OK` | 扫描成功 |
| `+NONE` | 命令跑通但一个 AP 都没扫到 |
| `+ERR` | 命令失败（连接/DHCP 失败，或 `g_wifi_init == 0`） |
| `+OK ip: a.b.c.d` | `sta conn` 成功且 DHCP 拿到地址 |
| **只有 `aic cmd finish`** | **命令没被识别**（`aic cmd finish` 是分发函数末尾无条件打印的，不能当成功标志） |

`sta conn` 是**同步阻塞**的：连接最长 `timeout_ms`（示例 30 s）+ DHCP 最长 10 s
（`app_wlan_sta_start()` 里 `STA_DHCP_TIMEOUT_MS = 10000/10` 次 × 10 ms）。
这期间 tshell 不会回显、不会出新提示符，**耐心等结果行，不要反复敲回车**。

---

## 4. 遗留项（本次未动，建议后续处理）

1. **SDHCI 中断语义**（问题一的正解）：`drv_sdhci.c` 的 `SDHCI_INT_CARD_INT` 分支应改为
   "ISR 内 mask → 处理 → 完成后 unmask"，并清 `SDHCI_INT_STATUS`；
   之后即可把 `AIC_SDIO_IRQ_MASK_FORCED` 置 0 恢复中断驱动。
2. **驱动任务优先级方向**：`port/inc/plat_port.h` 的 `CONFIG_PRIO_HIGH_IS_LARGE` 默认为 `1`
   （FreeRTOS 语义：数值大=优先级高），而 RT-Thread 相反。结果是驱动任务全部落在
   `19~31`（`aicwf_sd=30`、`aicwf_wi=31`，idle=31）。建议置 `0`，否则收包/定时器任务
   长期被其它线程压着，吞吐与时序会受影响。
3. **扫描结果 5G 信道打印错误**：`wifi_port_sta.c` 的 `app_wlan_sta_show_scan_res()` 用
   `aic_wifi_phy_channel_to_freq(0, channel)` 填 `CH=` 字段，band 固定传 0，5G 打印成 `CH=0`。
   真实频率在 `[SCAN/SSID] freq=...` 行里是对的，纯显示问题。
4. **Q1/Q2 阶段切换**：本文涉及的 `sdio_port.c` 里，`aicwf_sdio_readb_cmd52()` 对
   `chipid==D80N && addr==0x03` 仍是"无条件重定向到 Q2(0x09)"，未区分 ROM 阶段 / FW 运行阶段。
   厂商问题清单见 `aic8800d80n_flowctrl_q1q2_vendor_report.md`；该问题的后续处理已写在同目录
   的 `aic8800d80n_flowctrl_postfw_q1_switch.md` 与 `aic8800d80_patch_flowctrl_phase_and_netdev_register.md`，
   本文不重复。

---

## 5. 同目录相关文档

| 文档 | 内容 |
|---|---|
| `aic8800d80_sdio_bringup_handoff.md` | 早期 SDIO bring-up 问题交接（`msg fc:0` / D1 / 寄存器探针那一段历史） |
| `aic8800d80n_flowctrl_q1q2_vendor_report.md` | Q1/Q2 流向信用寄存器问题的厂商报告 |
| `aic8800d80n_flowctrl_postfw_q1_switch.md` | FW 运行后切回 Q1 的后续处理 |
| `aic8800d80_patch_flowctrl_phase_and_netdev_register.md` | flowctrl 阶段适配 + netdev 注册 |
| **本文** | **卡死（SDIO 中断风暴）+ `sta conn` 栈溢出 的终版改动与理由** |
