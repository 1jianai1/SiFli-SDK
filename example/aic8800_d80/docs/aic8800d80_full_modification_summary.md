# AIC8800D80 WiFi 移植 —— 完整修改流程总结

> **本文用途**：一页读懂整个工程改了什么、为什么改、结果如何。
> **阅读方式**：每条统一按 **遇到了什么问题 → 发现是什么原因 → 怎么修改的 → 结果如何** 排列。
> **代码给全，说明从简。** 照抄即可到终版。
>
> **工程**：`example/aic8800_d80`　**板型**：`sf32lb55-lcd_n16r4n4`　**模组**：AIC8800D80N（`VID=0xC8A1 PID=0x9082` → `chipid=4`）
> **架构**：路线1 —— 驱动自建 lwIP netif，不用 SDK 的 `rt_wlan` / `eth_device`
> **日期**：2026-10-12

---

## 改动总表

| # | 阶段 | 文件 | 一句话 | 开关 |
|---|---|---|---|---|
| 1 | SDIO/固件 | `platform/hal/sdio_port.c` | flow-ctrl 读侧重定向 Q1→Q2（ROM 阶段） | 无（按 chipid 生效） |
| 2 | SDIO/固件 | 同上 | 补丁 FW 起来后**切回真实 Q1** | `AIC_FC_Q1_AFTER_FWBOOT` |
| 3 | SDIO/固件 | 同上 + `src/main.c` | SDIO(DAT1) 中断风暴规避 | `g_sdio_irq_mask_forced` / `AIC_SDIO_IRQ_MASK_FORCED` |
| 4 | SDIO/固件 | `port/inc/plat_port.h` | wpa 任务栈 3072 → 8192 | 无 |
| 5 | 板级/配置 | `proj.conf` + `bsp_pinmux.c` | 4-bit 总线 + CMD/DAT 上拉 | `CONFIG_SDMMC2_BUS_WIDTH_1_ONLY` |
| 6 | 网络接口 | `platform/netif/netif_port.c` | 为自建 netif **补注册 RT-Thread netdev** | `RT_USING_NETDEV` |
| 7 | 网络接口 | 同上 + `wifi_port_sta.c` + `netif_port.h` | **把 netif 状态镜像给 netdev** | `RT_USING_NETDEV` |
| 8 | 网络接口 | `platform/netif/netif_port.c` | **让 lwIP 拿到 `netif_default`** | `RT_USING_NETDEV` |
| 9 | **环境/硬件** | *（非代码）* | 模组改用**独立供电**，不与键盘等外设共用一路 USB | —— |
| A | 工具 | `src/main.c` | 新增 `nettest` TCP 连通性测试 | 可直接删 |

**一句话概括**：问题 1~2 让芯片**能起来、能连接**；问题 3~5 让系统**不会卡死、不会崩**；
问题 6~8 让网络**能用**（DHCP 不崩、`ping` 能发、公网能通）；
问题 9 是**外部供电不足导致芯片复位**，在主机侧伪装成 SDIO 总线故障，最后才查出来。

---

# 阶段一：SDIO 与固件加载（问题 1~5）

## 问题 1：初始化卡死在 `msg fc:0`

### 遇到了什么问题

```
[AIC][SDIO]msg fc:0                      <- 主机日志停在这里
```
芯片侧日志停在 `fn1_ioen`，之后永无输出。整条启动链（CINIT → PATCH → PATCH_TBL → CALIB）一步都没开始。

### 发现是什么原因

`msg fc:0` 不是总线/CRC 错误，而是"**芯片侧 cfg 通道报 0 个可用 TX 缓冲**"。

- 库把 `sdio_reg.flow_ctrl_reg` 对 chipid 3/4/5 硬编码为 **Q1(`0x03`)**，且数据通路与 msg 通路共用这一个字段；
- 这颗 D80N 在 ROM/CINIT/PATCH/CALIB 阶段 **Q1 恒 0、Q2(`0x09`) 恒 2**；
- 库读 Q1 得 0 → 判定无 TX 缓冲 → 第一条 cfg 消息发不出 → `cmd_mgr` 超时 → `system config fail` → 初始化失败。

判定"重试窗口里到底有没有 CMD52"用了独立探针，确认**有 CMD52 且返回 0**，排除"根本没读"。

### 怎么修改的

**文件**：`customer/peripherals/wifi/aic8800_rtos_driver/platform/hal/sdio_port.c`
**锚点**：`bool aicwf_sdio_readb_cmd52(uint32_t addr, uint8_t *data)` 定义**之前**

```c
/* ---------------------------------------------------------------------------
 * D80N(0x9082) flow-ctrl 读重定向的阶段开关   [AIC8800-PORT][SiFli]
 *
 * 背景: 库把 sdio_reg.flow_ctrl_reg 硬编码为 Q1(0x03), 且数据通路与 msg(cfg)
 *   通路共用这一个字段; 而本芯片 ROM/CINIT/PATCH/CALIB 阶段只有 Q2(0x09) 有 TX
 *   信用(Q1 恒 0, Q2 恒 2; 且必须先哑读一次 Q1, Q2 才有效), 故在 func1 CMD52
 *   读入口做 0x03 -> 0x09 的读侧重定向。该规避让初始化 100% 通过。
 *
 * 遗留问题: 重定向此前无条件且永久生效, 于是补丁 FW 跑起来之后:
 *   · msg  通路判据 fc_reg != 0        -> Q2=2 通过   -> 扫描/认证/关联 OK
 *   · 数据 通路判据 fc_reg >  THRESH(2) -> Q2=2 不通过 -> data fc:-2,
 *     EAPOL(msg2/4) 永远发不出芯片 -> AP 每秒重发 msg1/4, 4 次后 deauth
 *     reason=15(4WAY_HANDSHAKE_TIMEOUT) —— 即握手失败、连接超时。
 *
 * 本开关把重定向限定在真正需要的阶段, 以便实测"FW 运行后 Q1 是多少":
 *   AIC_FC_Q1_AFTER_FWBOOT = 1: FW 就绪后停止重定向, 直读真实 Q1(0x03)
 *   AIC_FC_Q1_AFTER_FWBOOT = 0: 保持原行为(永远读 Q2), 仅作 A/B 对照
 *
 *   AIC_FC_RAW_DBG = 1: 每 AIC_FC_RAW_DBG_PERIOD 次 flow-ctrl 读, 附带打印一次
 *                       raw Q1/Q2(默认关闭: 反复访问 Q1 会触发票芯片侧
 *                       "sdio func1 error bit:800, func1 free cnt=0, ...")
 * ------------------------------------------------------------------------- */
#define AIC_FC_Q1_AFTER_FWBOOT   1
#define AIC_FC_RAW_DBG           0
#define AIC_FC_RAW_DBG_PERIOD    2000

/* 1 = ROM/CINIT/PATCH/CALIB 阶段: 读 Q1 即重定向到 Q2;
 * 由 aicwf_sdio_enable_irq_after_fw_boot() 在补丁 FW 启动成功后清零 */
static volatile int g_fc_redirect_q2 = 1;
```

改写 `aicwf_sdio_readb_cmd52()` 函数体（**完整**）：

```c
bool aicwf_sdio_readb_cmd52(uint32_t addr, uint8_t *data)
{
    if (addr == SDIOWIFI_FLOW_CTRL_Q1_REG_V3)
    {
        /* "前 5 次先哑读 Q1/Q2"
         * 与两次 rt_kprintf 造成的延时都被观察到是承重的：
         *   · 去掉本段(直接读 Q2)                          -> 拿不到信用，启动失败
         *   · 每次都哑读 Q1 再读 Q2(无延时)                -> 芯片反复报
         *        "sdio func1 error bit:800, func1 free cnt=0, func1 free queue2 cnt=2"
         *        并需要重试很多次才成功（反复访问 Q1 会招惹芯片）
         * [AIC8800-PORT][SiFli] 本段是 ROM/CINIT 阶段 Q2 给出有效信用的必要条件,
         * 与阶段无关地原样保留, 不做任何时序改动 */
        static uint32_t fc_dbg_cnt = 0;
        uint8_t v = 0;
        bool ok;

        fc_dbg_cnt++;

        if (fc_dbg_cnt <= 5)
        {
            uint8_t v03 = 0, v09 = 0;
            bool ok03 = aicwf_sdio_readb_cmd52_by_num(FUNC_1, SDIOWIFI_FLOW_CTRL_Q1_REG_V3, &v03);
            bool ok09 = aicwf_sdio_readb_cmd52_by_num(FUNC_1, SDIOWIFI_FLOW_CTRL_Q2_REG_V3, &v09);
            rt_kprintf("[AICFC] #%u raw 0x03=%u(ok%d) 0x09=%u(ok%d) chipid=%u field=0x%02x redir=%d\n",
                       fc_dbg_cnt, v03, ok03, v09, ok09,
                       g_runtime_chipid, sdio_dev.sdio_reg.flow_ctrl_reg, g_fc_redirect_q2);
        }
#if AIC_FC_RAW_DBG
        else if ((AIC_FC_RAW_DBG_PERIOD != 0) && ((fc_dbg_cnt % AIC_FC_RAW_DBG_PERIOD) == 0))
        {
            /* 周期探针: 观察 FW 运行后 Q1/Q2 是否变化 (默认关闭, 原因见开关处说明) */
            uint8_t v03 = 0, v09 = 0;
            bool ok03 = aicwf_sdio_readb_cmd52_by_num(FUNC_1, SDIOWIFI_FLOW_CTRL_Q1_REG_V3, &v03);
            bool ok09 = aicwf_sdio_readb_cmd52_by_num(FUNC_1, SDIOWIFI_FLOW_CTRL_Q2_REG_V3, &v09);
            rt_kprintf("[AICFC] #%u raw 0x03=%u(ok%d) 0x09=%u(ok%d) redir=%d\n",
                       fc_dbg_cnt, v03, ok03, v09, ok09, g_fc_redirect_q2);
        }
#endif

        /* [AIC8800-PORT][SiFli] 仅在"需要重定向的阶段"且为 D80N 时才翻译到 Q2;
         * g_fc_redirect_q2 被 aicwf_sdio_enable_irq_after_fw_boot() 清零后,
         * 本函数返回的是真实 Q1(0x03) —— 数据通路的判据是 fc_reg > 2 */
        if (g_fc_redirect_q2 && (g_runtime_chipid == PRODUCT_ID_AIC8800D80N))
        {
            ok = aicwf_sdio_readb_cmd52_by_num(FUNC_1, SDIOWIFI_FLOW_CTRL_Q2_REG_V3, &v);
            if ((fc_dbg_cnt <= 5) || ((fc_dbg_cnt % 200) == 0))
                rt_kprintf("[AICFC] #%u -> redirect 0x09 val=%u ok=%d\n", fc_dbg_cnt, v, ok);
            *data = v;
            return ok;
        }

        if ((fc_dbg_cnt <= 5) && (g_runtime_chipid != PRODUCT_ID_AIC8800D80N))
            rt_kprintf("[AICFC] #%u NOT redirect: chipid=%u (expect %u)\n",
                       fc_dbg_cnt, g_runtime_chipid, PRODUCT_ID_AIC8800D80N);
    }

    return aicwf_sdio_readb_cmd52_by_num(FUNC_1, addr, data);
}
```

**为什么改在"读"侧**：库每次读 flow-ctrl 都经由本函数（反汇编确认），所以在读入口翻译与调用顺序/时机无关；而"写"侧是在和库抢同一个字段，实测无效。

### 结果如何

初始化 100% 通过：

```
====> D80N stage1: CINIT (bootrom)
Upload fw id=60 @ 0x00133000, size=17028
Upload fw id=58 @ 0x00188000, size=23680
[PATCH_TBL] wrote 65 entries (130 words)
Upload fw id=61 @ 0x00138000, size=29544
```

---

## 问题 2：扫描/认证/关联全成功，但 4 次握手失败（`reason=15`）

### 遇到了什么问题

`sta conn` 能扫到 AP、认证、关联都 OK，但 4 次握手失败，连接全过程持续打印：

```
[AIC][SDIO]data fc:-2
...
deauth reason=15 (4WAY_HANDSHAKE_TIMEOUT)
```

### 发现是什么原因

补丁 1 的重定向**无条件且永久生效**，补丁 FW 起来后 host 仍在读 Q2=2，而两条通路的判据不同：

| 通路 | 判据 | Q2=2 时 |
|---|---|---|
| msg(cfg) | `fc_reg != 0` | ✅ 通过 → 扫描/认证/关联 OK |
| 数据 | `fc_reg > DATA_FLOW_CTRL_THRESH(2)` | ❌ 永远不通过 → `data fc:-2` |

EAPOL（msg2/4）走**数据帧**，发不出芯片 → AP 每秒重发 msg1/4、4 次后 deauth。
`data fc:%d` 打印的是 `-fc_reg`，所以 `-2` 就是"读到的信用是 2"。

实测结论：**FW 就绪后 Q1 是有效的**，正确语义是"ROM 阶段读 Q2、FW 运行后读回 Q1"。

### 怎么修改的

**文件**：同一个 `sdio_port.c`
**锚点**：`int aicwf_sdio_enable_irq_after_fw_boot(void)` 末尾、`return 0;` 之前

```c
    ret = aicwf_sdio_writeb_cmd52(sdiodev->sdio_reg.intr_config_reg, 0x07);
    if (!ret) {
        DBG_SDIO_ERR("%s: post-fw-boot write intr_config_reg still failed!\n", __func__);
        return -1;
    }

#if AIC_FC_Q1_AFTER_FWBOOT
    /* [AIC8800-PORT][SiFli] 补丁 FW(PATCH/CALIB) 已启动, 数据队列(Q1)应已开始发放
     * TX 信用: 在此收回 ROM 阶段的 Q1->Q2 读重定向, 让数据通路读到真实 Q1。
     * 本函数是库在 "start_from_bootrom success." 之后调用的第一个明确锚点。
     * 置 AIC_FC_Q1_AFTER_FWBOOT=0 可保持原行为(永远读 Q2)做 A/B 对照。 */
    if (g_fc_redirect_q2) {
        g_fc_redirect_q2 = 0;
        rt_kprintf("[AICFC] patch FW up -> flow-ctrl read back to real Q1(0x03)\n");
    }
#endif

    DBG_SDIO_INF("aicwf_sdio_enable_irq_after_fw_boot: intr_config_reg=0x07 OK (WiFi IRQ enabled)\n");
    return 0;
}
```

**为什么挂这里**：库在 `start_from_bootrom success.` 之后立刻调用本函数，是"FW 已就绪"的第一个明确锚点，且由驱动自动触发，不依赖应用层顺序。

### 结果如何

```
[FW] fwlog redir enable=1
[AIC][MACIF]start_from_bootrom success.
[AICFC] patch FW up -> flow-ctrl read back to real Q1(0x03)      <- 阶段切换生效
```

- 全程**不再出现** `data fc:` / `msg fc:`；
- `fhost_tx_req eapol, len 135` → `[FW] txl :8e88, len=159` → EAPOL 真的出芯片；
- `add ptk` / `add gtk` / `WPA network 0: connected` / `AIC-STA: conn` → 握手完成。

---

## 问题 3：驱动起来后"整机卡死"

### 遇到了什么问题

```
[aic8800] driver up. use 'wifi_test <cmd>' to control
msh />
```

`msh />` **只打印一次**，之后反复敲回车没有任何提示符、没有回显。
去掉日志过滤后，串口被下面这条**持续刷屏**：

```
[AIC][FHOST][FW] Interrupt but no data, intstatus=0, retry_cnt=0
```

### 发现是什么原因

SDIO(DAT1) **电平中断风暴**：

1. FW 起来后一直把 **DAT1 拉低**（SDIO 卡中断），这是**电平**中断；
2. `drv_sdhci.c` 处理 `SDHCI_INT_CARD_INT` 时**既不清 `SDHCI_INT_STATUS` 也不 mask**，只做唤醒；
3. 闭源库 `aicwf_sdio_rx_task` 每轮结束都**无条件** `sdio_host_enable_isr(TRUE)`；
4. 于是死循环：`中断 → 唤醒RX任务 → 读 intstatus=0 → 再放开中断 → 立刻又中断 …`；
5. 该中断 NVIC 优先级高于串口 → **串口 RX 永远得不到服务** → 表现为"整机卡死"。

> 不是线程被饿死：驱动任务优先级（30/29）实际**低于** `tshell(20)`，卡死发生在**中断级**。

### 怎么修改的

**改动 3-(1)**：`platform/hal/sdio_port.c`，紧跟在 `unsigned char aicwifi_sdio_rxdata_mode = CONFIG_RX_POLLING_DEFAULT;` 之后

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

**改动 3-(2)**：同文件，`aicwf_sdio_isr()`（**完整**）

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

**改动 3-(3)**：同文件，`aicwf_sdio_host_enable_isr()`（**完整**）

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
        aicwf_rtos_mutex_lock(sdio_dev.bus_txrx, -1);
        aicwf_sdio_claim_host(FUNC_1);
        aicwf_sdio_release_irq(sdio_function_local[FUNC_1]);
        aicwf_sdio_release_host(FUNC_1);

        #if (defined(CONFIG_AIC8800DC) || defined(CONFIG_AIC8800DW))
        aicwf_sdio_claim_host(FUNC_2);
        aicwf_sdio_release_irq(sdio_function_local[FUNC_2]);
        aicwf_sdio_release_host(FUNC_2);    
        #endif
        aicwf_rtos_mutex_unlock(sdio_dev.bus_txrx);
    }
    return true;
}
```

这两处是平台上所有"使能 SDIO 中断"的**唯一入口**，拦在这里，`SDHCI_SIGNAL_ENABLE` 里的 CARD_INT 始终为 0，控制器**根本不会向 NVIC 提这个中断**。

**改动 3-(4)**：`example/aic8800_d80/src/main.c`，增加开关并在 `main()` 里、驱动初始化**之前**调用

```c
/* ---------------------------------------------------------------------------
 * SDIO(DAT1) 中断风暴规避开关   (机理详见 sdio_port.c 里 g_sdio_irq_mask_forced)
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

    /* 路线1: 驱动内部完成 aic_platform_init() / SDIO 总线注册 / netif 建立 */
    aic_wifi_demo_test();

    rt_kprintf("[aic8800] driver up. try: wifi_test iwpriv sta scan\n");
    return 0;
}
```

**为什么屏蔽中断还能收包**：库内 `aic_sdio_rx_task` 用 `rtos_semaphore_wait(sdio_rx_sema, tout)`，`tout` 由 `aicwifi_sdio_rxdata_mode` 决定——非 0 时是 **10ms 超时轮询**，照样周期性读 `misc_int_status` 取数据。

### 结果如何

- `AIC_SDIO_IRQ_MASK_FORCED = 1`：shell 正常，回车有回显，扫描/连接全部可用；
- `AIC_SDIO_IRQ_MASK_FORCED = 0`：**100% 复现**卡死 + `Interrupt but no data` 刷屏；
- 代价：收包从事件驱动变 10ms 轮询，延迟最多 +10ms。

> **这是规避不是根治**。正解是改 `drv_sdhci.c` 的 `SDHCI_INT_CARD_INT` 分支为
> "ISR 内先 mask → 处理 → 清 `SDHCI_INT_STATUS` → 完成后 unmask"（Linux `sdhci` 语义）。

---

## 问题 4：`sta conn` 立刻 hard fault（栈溢出）

### 遇到了什么问题

```
[AIC][FHOST]fhost_wpa_create_network
[AIC][FHOST]fhost_wpa_start
hard fault on thread: fhost_wp
fhost_wp  29  ready  ...  stack size 0x00000c00   max used 95%
usage fault:
SCB_CFSR_UFSR:0x10 STKOF
```

### 发现是什么原因

`UFSR` bit4 = `STKOF`：线程栈越过 `PSPLIM`，**栈溢出**。
同一次 dump 里 `stack size 0x00000c00`(3072) / `max used 95%` 完全吻合。
wpa_supplicant 的 WPS/EAPOL/TLS 调用链局部变量很大，3KB 不够。与 SDIO 无关。

### 怎么修改的

**文件**：`customer/peripherals/wifi/aic8800_rtos_driver/port/inc/plat_port.h`

```c
/* wpa_supplicant 任务栈: 原 3072 太小 —— 实测 sta conn 时 fhost_wpa_task
 * max used 95% 并触发 usage fault STKOF(SCB_CFSR_UFSR:0x10), 于
 * fhost_wpa_create_network/fhost_wpa_start 内部崩溃。wpa 的 WPS/EAPOL/TLS
 * 调用链局部变量很大, 给 8KB。 */
#define FHOST_WPA_STACK_SIZE        (8192)
```

### 结果如何

`plat_port.o` 的 `fhost_wpa_stack_size` = `0x00002000`，启动日志里该任务打印 `stack_size= 2000`，`sta conn` 不再崩。

> 各线程栈余量（供后续加代码时参考）：`tshell` 6144/75%、`cli_cmd` 1024/60%（偏紧）、
> `fhost_cntrl` 2048/56%、`fhost_rx` 2048/37%，其余 ≤35%。

---

## 问题 5：板级 4-bit 总线与上拉

### 遇到了什么问题

早期为隔离 D1 的规律性下拉，临时切到 1-bit 单线模式。

### 发现是什么原因

D1 的下拉在本案中与 `0x03` 恒 0 **没有因果关系**（CMD52 的读数据在 R5 响应里、走 CMD 线，DAT1 不参与）。
1-bit 只是隔离手段，**4-bit 已验证可用**，且吞吐更好。

### 怎么修改的

**文件 1**：`example/aic8800_d80/project/proj.conf`

```ini
CONFIG_BSP_USING_SDIO=y
CONFIG_BSP_USING_SDMMC2=y
CONFIG_SDIO2_CARD_MODE=2
CONFIG_RT_SDIO_THREAD_PRIORITY=9
CONFIG_SDMMC2_BUS_WIDTH_1_ONLY=n        # 4 线; 置 y 为 1-bit(仅用于隔离排查)
CONFIG_RT_SDIO_DEBUG=y
CONFIG_WIFI_USING_AIC8800_RTOS=y
CONFIG_AIC8800_RTOS_WIRELESS_PWR_GPIO=124
```

**文件 2**（板级，非本工程但必须一致）：`customer/boards/sf32lb55-lcd_base/bsp_pinmux.c`

```c
HAL_PIN_Set(PAD_PA44, SD2_CLK,  PIN_NOPULL, 1);            // SDIO2
HAL_PIN_Set(PAD_PA45, SD2_CMD,  PIN_PULLUP, 1);
HAL_PIN_Set(PAD_PA47, SD2_DIO0, PIN_PULLUP, 1);
HAL_PIN_Set(PAD_PA49, SD2_DIO1, PIN_PULLUP, 1);
HAL_PIN_Set(PAD_PA51, SD2_DIO2, PIN_PULLUP, 1);
HAL_PIN_Set(PAD_PA55, SD2_DIO3, PIN_PULLUP, 1);
```

`SDMMC2_BUS_WIDTH_1_ONLY=y` 会让 `drv_sdhci.c` 不置 `MMCSD_BUSWIDTH_4`、`sdio_set_bus_wide()` 跳过写 CCCR 0x07；改成 `=n` 才是量产姿态。CLK 不上拉，CMD/DAT0~3 必须上拉。

### 结果如何

4-bit + 24MHz + 内部上拉实测可用。若量产出偶发 CRC/掉线，优先补外部 10kΩ 上拉到 3.3V。

---

# 阶段二：网络接口（问题 6~8）

> **共同前提**：本工程走**路线1**（驱动自己 `netifapi_netif_add()` + 自己的 init 回调建 netif）。
> 这**绕开了 SDK `ethernetif.c::netdev_add()` 里的三条自动同步**，必须手工补回来：
> **① 建 netdev　② 同步 netdev 状态　③ 设 lwIP 默认接口**。问题 6/7/8 正好一一对应。

## 问题 6：DHCP ACK 到达瞬间 tcpip 线程崩溃

### 遇到了什么问题

```
Assertion failed at function:netdev_set_dns_server, line number:727 ,(netdev)
...
fatal error on thread: tcpip
```

### 发现是什么原因

本 SDK 的 lwIP 被 RT-Thread 改造过，**把 netdev 当作 DNS 的唯一载体**：

| 调用点 | 代码 |
|---|---|
| `lwip-2.1.2/src/core/ipv4/dhcp.c:742`（`dhcp_handle_ack`） | `netdev_set_dns_server(netdev_get_by_name(netif->name), n, &dns_addr)` |
| `lwip-2.1.2/src/core/dns.c:387/399`（`dns_setserver`） | 遍历 `netif_list` 逐个 `netdev_set_dns_server(...)` |

而路线1 的驱动**不会**为自建 netif 建 netdev（`ethernetif.c::netdev_add()` 是 `static`，只服务标准网卡）→
`netdev_get_by_name("wl")` 返回 NULL → `netdev_set_dns_server()` 里的 `RT_ASSERT(netdev)` 直接断言。
能崩在这里也**反证 DHCP ACK 已收到**。

### 怎么修改的

**文件**：`customer/peripherals/wifi/aic8800_rtos_driver/platform/netif/netif_port.c`

**(1) 头部加 netdev 头与 ops 声明**（锚点：`#include "lwip/dhcp.h"`）

```c
#include "lwip/dhcp.h"       /* dhcp_supplied_address */

#ifdef RT_USING_NETDEV
/* [AIC8800-PORT][SiFli] 路线1 的驱动自建 lwIP netif, 需自行补注册 RT-Thread netdev,
 * 否则 lwIP 侧的 netdev 钩子会拿到 NULL 直接断言 (详见 aicwf_net_if_netdev_add)。 */
#include "netdev.h"
/* lwIP port 的标准 ops 表 (rtos/rtthread/components/net/lwip/port/ethernetif.c):
 * 各回调均以 netdev->user_data 作为 struct netif *, 其 set_dns_server 落到
 * netdev_low_level_set_dns_server(), 不会再回调 dns_setserver(), 无递归风险 */
extern const struct netdev_ops lwip_netdev_ops;
#endif /* RT_USING_NETDEV */
```

**(2) 新增状态与两个辅助函数**（锚点：`static err_t aicwf_net_if_init(struct netif *net_if);` 之后）

```c
static err_t aicwf_net_if_init(struct netif *net_if);

#ifdef RT_USING_NETDEV
/* 每个 netif 槽位对应一个 RT-Thread netdev (静态复用, 不依赖堆) */
static struct netdev s_netdev_pool[PLAT_NETIF_POOL_SIZE];
static uint8_t       s_netdev_used[PLAT_NETIF_POOL_SIZE];

static void aicwf_net_if_netdev_del(struct netif *nif);

static int aicwf_net_if_netdev_add(struct netif *nif)
{
    struct netdev *nd;
    char name[NETIF_NAMESIZE];
    int idx;

    if (!nif)
        return -1;

    idx = (int)(nif - &s_netif_pool[0]);
    if (idx < 0 || idx >= PLAT_NETIF_POOL_SIZE)
        return -1;

    /* 幂等: 先摘掉旧登记, 避免 netdev_register() 把同一节点二次入链 */
    aicwf_net_if_netdev_del(nif);

    nd = &s_netdev_pool[idx];

    /* 与 ethernetif.c::netdev_add() 一致: netdev 名 = lwIP netif->name */
    memcpy(name, nif->name, sizeof(nif->name));
    name[NETIF_NAMESIZE - 1] = '\0';

    /* ★ 字段必须在 netdev_register() 之前装好 —— 理由见问题 8 */
    nd->ops        = &lwip_netdev_ops;
    nd->hwaddr_len = nif->hwaddr_len;
    memcpy(nd->hwaddr, nif->hwaddr, nif->hwaddr_len);
    nd->ip_addr    = nif->ip_addr;
    nd->gw         = nif->gw;
    nd->netmask    = nif->netmask;

    if (netdev_register(nd, name, nif) != 0) {
        memset(nd, 0, sizeof(struct netdev));
        DBG_NETAL_ERR("%s: netdev_register(%s) fail\r\n", __func__, name);
        return -1;
    }

    s_netdev_used[idx] = 1;

    DBG_NETAL_INF("%s: netdev '%s' registered (netif %p)\r\n", __func__, name, nif);
    return 0;
}

static void aicwf_net_if_netdev_del(struct netif *nif)
{
    int idx;

    if (!nif)
        return;

    idx = (int)(nif - &s_netif_pool[0]);
    if (idx < 0 || idx >= PLAT_NETIF_POOL_SIZE || !s_netdev_used[idx])
        return;

    netdev_unregister(&s_netdev_pool[idx]);
    s_netdev_used[idx] = 0;
}
#endif /* RT_USING_NETDEV */
```

**(3) `aicwf_net_if_add()` 里注册**（锚点：`DBG_NETAL_INF("%s status=%d\r\n", __func__, status);` 之后）

```c
    DBG_NETAL_INF("%s status=%d\r\n", __func__, status);

    if (status != ERR_OK)
        return -1;

#ifdef RT_USING_NETDEV
    aicwf_net_if_netdev_add(net_if);
#endif

    return 0;
}
```

**(4) `aicwf_net_if_release_all()` 里配对摘除**（锚点：`DBG_NETAL_INF("%s: remove netif %c%c%d\n", ...)`）

```c
void aicwf_net_if_release_all(void)
{
    int i;

    for (i = 0; i < PLAT_NETIF_POOL_SIZE; i++) {
        struct netif *nif = &s_netif_pool[i];
        if (nif->output == NULL)
            continue;
        DBG_NETAL_INF("%s: remove netif %c%c%d\n", __func__,
                      nif->name[0], nif->name[1], nif->num);
#ifdef RT_USING_NETDEV
        /* 先摘 netdev: 否则驱动重初始化时同一槽位会带旧登记二次入链 */
        aicwf_net_if_netdev_del(nif);
#endif
        netif_set_down(nif);
        netif_remove(nif);
        /* 槽位回池: netif_remove 不清 output, 需整体清零以恢复可分配 */
        memset(nif, 0, sizeof(struct netif));
    }
}
```

> 第 (4) 处**不能省**：`netdev_register()` 会 `rt_slist_init()` 该节点再尾插，
> 若同一节点已在链上而没摘除，会造成**自环**，后续任何遍历 `netdev_list` 的代码都会死循环。

### 结果如何

DHCP 正常完成，`Assertion failed at function:netdev_set_dns_server` 消失：

```
[AIC][FHOST][FW] tx dhcp
[AIC][APP]app_wlan_sta_start: dhcp finish
[AIC][APP]app_wlan_sta_get_ip:34:107:237:10+OK ip: 10:237:107:34
```

---

## 问题 7：`ping: network interface device(wl) status error.`

### 遇到了什么问题

Wi-Fi 连上、DHCP 拿到 IP，但 `ping` **一个包都不发**：

```
msh /> ping 8.8.8.8
ping: not found specified netif, using default netdev wl.
ping: network interface device(wl) status error.
```

`ifconfig` 也显示 `FLAGS: UP LINK_DOWN DHCP_DISABLE`、`MTU: 0`、`ip address: 0.0.0.0`。

### 发现是什么原因

`netdev_cmd_ping()` 的前置判据（`netdev.c:1288`）要求 `netdev_is_up() && netdev_is_link_up()`，
两者取 `netdev->flags`。而本 SDK 里这两个位**只由 lwIP 的 `netif_set_up()` / `netif_set_link_up()` 回调设置**：

```c
/* lwip-2.1.2/src/core/netif.c:1120 */
netdev_low_level_set_link_status(netdev_get_by_name(netif->name), RT_TRUE);
```

但驱动的 `aicwf_net_if_init()` 是**直接把标志写进 `netif->flags`**：

```c
net_if->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_IGMP;
```

**绕过了函数调用** → lwIP 侧链路正常（ARP/DHCP 全好），netdev 侧恒 `LINK_DOWN`。
driver 目录下 grep 确认**没有任何一处调用 `netif_set_link_up()`**。
地址同样是 `netdev_register()` 时刻的快照，DHCP 后不会回灌。

### 怎么修改的

**文件 1**：`platform/netif/netif_port.c`，新增函数（锚点：`aicwf_net_if_netdev_del()` 结尾的 `#endif /* RT_USING_NETDEV */` 之后）

```c
/**
 * @brief 把 lwIP netif 的链路标志 / 地址镜像到 RT-Thread netdev
 *
 * [AIC8800-PORT][SiFli] 路线1 的 netdev 是补注册的, 两侧状态必须显式同步,
 * 否则会出现"netif 正常工作但 netdev 侧看不到"的割裂:
 *
 *  1) 链路标志 (本函数的首要目的)
 *     aicwf_net_if_init() 是把 NETIF_FLAG_LINK_UP 直接写进 netif->flags 的,
 *     绕过了 netif_set_link_up(); 而本 SDK 的 lwIP 只在
 *     netif_set_link_up() / netif_set_link_down() 内部调用
 *     netdev_low_level_set_link_status()
 *     (rtos/rtthread/components/net/lwip/lwip-2.1.2/src/core/netif.c:1120 / 1150)。
 *     于是 netdev.flags 恒为 LINK_DOWN。后果:
 *       - netdev 版 ping 的前置判据 NETDEV_PING_IS_COMMONICABLE
 *         (netdev.c:1288, 要求 netdev_is_up() && netdev_is_link_up())
 *         不过 -> 一个 ICMP 包都不发就直接报
 *             ping: network interface device(wl) status error.
 *       - ifconfig 的 FLAGS 行显示 LINK_DOWN。
 *     注意: lwIP 侧的链路是好的(ARP/DHCP 都正常), 被卡住的只有 netdev 这一侧。
 *
 *  2) 地址
 *     netdev 的 ip/gw/netmask 是 netdev_register() 时刻的快照, DHCP 拿到地址后
 *     不会自动回灌, 所以 ifconfig 会一直显示 0.0.0.0。
 *
 * 顺序说明: 先置标志再写地址。netdev_low_level_set_link_status(TRUE) 在
 * "netdev_is_up() && ip 非 0" 时会调 sal_check_netdev_internet_up(); 先置标志时
 * ip 尚为 0, 可避免该路径被意外触发。
 *
 * 幂等: 重复调用只做几次字段赋值, 可安全地在任意时刻调用。
 *
 * @param net_if 网络接口句柄, NULL = 默认 STA 接口 (语义同 aicwf_net_if_get_ip)
 */
void aicwf_net_if_netdev_sync(net_if_t net_if)
{
#ifdef RT_USING_NETDEV
    struct netif *nif;
    struct netdev *nd;

    if (net_if == NULL) {
        net_if = net_if_find_from_wifi_idx(0);
        if (net_if == NULL)
            return;
    }

    nif = netif_from_handle(net_if);
    /* name[0]==0 表示 init 回调尚未跑过, 此时 netdev 也还没登记 */
    if (!nif || (nif->name[0] == '\0'))
        return;

    nd = netdev_get_by_name(nif->name);
    if (!nd)
        return;

    if (netif_is_up(nif))
        netdev_low_level_set_status(nd, RT_TRUE);
    if (netif_is_link_up(nif))
        netdev_low_level_set_link_status(nd, RT_TRUE);

    nd->mtu     = nif->mtu;
    nd->ip_addr = nif->ip_addr;
    nd->gw      = nif->gw;
    nd->netmask = nif->netmask;

#if LWIP_DHCP
    /* 与 ethernetif.c::netdev_flags_sync() 的做法一致: 供 ifconfig 显示 DHCP_ENABLE。
     * 不补这一位的话 ifconfig 会显示 DHCP_DISABLE, 与实际情况相反。 */
    netdev_low_level_set_dhcp_status(nd, RT_TRUE);
#endif

    /* 兜底: lwIP 的全局 netif_default 是**跨网段路由的唯一出口**
     * (详见问题 8)。正常情况下这一步已由 netdev_register() ->
     * netdev_set_default() -> ops->set_default() 完成; 这里再兜一次,
     * 覆盖"netdev 不是第一个注册的"等边界情况。幂等。 */
    if (netif_default == RT_NULL)
        netif_set_default(nif);
#else
    (void)net_if;
#endif /* RT_USING_NETDEV */
}
```

**文件 2**：`port/inc/netif_port.h`（锚点：`net_if_t aicwf_net_if_get(uint8_t index);` 之后）

```c
/* [AIC8800-PORT][SiFli] 把 lwIP netif 的链路标志/地址镜像到 RT-Thread netdev。
 * 路线1 的 netdev 是补注册的, 两侧状态需显式同步 (原因见 netif_port.c 实现处)。
 * net_if=NULL 表示默认 STA 接口; 幂等, 可重复调用; RT_USING_NETDEV=n 时为空实现。 */
void  aicwf_net_if_netdev_sync(net_if_t net_if);
```

**文件 3**：`platform/netif/netif_port.c` 的 `aicwf_net_if_add()`，**两处调用**

```c
    /* 2. 检查是否已注册到 LwIP */
    if (net_if->output != NULL) {
        DBG_NETAL_INF("%s already added to LwIP\r\n", __func__);
#ifdef RT_USING_NETDEV
        /* [AIC8800-PORT][SiFli] 重连场景走这条早退分支: netdev 可能已被
         * aicwf_net_if_release_all() 摘除后重建, 这里补一次镜像 (幂等) */
        aicwf_net_if_netdev_sync(net_if);
#endif
        return 0;
    }

    /* 3. 注册接口到 LwIP 协议栈 */
    status = netifapi_netif_add(net_if,
                               (const ip4_addr_t *)ipaddr,
                               (const ip4_addr_t *)netmask,
                               (const ip4_addr_t *)gw,
                               vif,
                               aicwf_net_if_init,
                               tcpip_input);

    DBG_NETAL_INF("%s status=%d\r\n", __func__, status);

    if (status != ERR_OK)
        return -1;

#ifdef RT_USING_NETDEV
    /* [AIC8800-PORT][SiFli] 补注册 RT-Thread netdev */
    aicwf_net_if_netdev_add(net_if);

    /* [AIC8800-PORT][SiFli] netdev 已登记 -> 立刻按 netif 的真实状态补齐镜像。
     * 关键的是 NETDEV_FLAG_LINK_UP: init 里直写 netif->flags 的写法不会经过
     * netif_set_link_up(), netdev 侧永远是 LINK_DOWN, netdev 版 ping 会被
     * "network interface device(wl) status error" 直接拒绝。 */
    aicwf_net_if_netdev_sync(net_if);
#endif

    return 0;
}
```

> **顺序不能颠倒**：必须先 `aicwf_net_if_netdev_add()` 再 `sync()`，
> 因为 `sync()` 内部用 `netdev_get_by_name(nif->name)` 反查。

**文件 4**：`platform/wifi/wifi_port_sta.c` 的 `app_wlan_sta_start()`，DHCP 成功后回灌地址

```c
        while(dhcp_timeout--)
        {
            if(aicwf_net_if_dhcp_done(0))
            {
                ret = 0;
                app_wlan_sta_get_ip();
                /* [AIC8800-PORT][SiFli] DHCP 已拿到地址: 把 netif 的地址/链路标志
                 * 回灌 RT-Thread netdev, 否则 ifconfig 恒显示 0.0.0.0, 且 netdev
                 * 版 ping 会以 "status error" 拒绝发包 (幂等) */
                aicwf_net_if_netdev_sync(NULL);
                break;
            }
            aicwf_rtos_msleep(STA_DHCP_POLL_INTERVAL_MS);
        }
```

### 结果如何

```
msh /> ifconfig
network interface device: wl (Default)
MTU: 1500
MAC: cc c4 b2 dc 17 ed
FLAGS: UP LINK_UP DHCP_ENABLE                          <- 关键: LINK_UP
ip address: 10.237.107.34
gw address: 10.237.107.49
net mask  : 255.255.255.0
dns server #0: 10.237.107.49
```

`ping` 不再报 `status error`，开始**真正发包**：

```
msh /> ping 10.237.107.49
4 packets transmitted, 4 received, 0% packet loss
```

> `FLAGS` 里**不会**出现 `ETHARP/BROADCAST/IGMP`：它们由 `ethernetif.c::netdev_flags_sync()`
> （`static`，只服务标准网卡）映射，且在本 SDK 里**仅被 `ifconfig` 的打印语句引用**
> （`netdev.c:1137-1139`），属纯显示项，不影响功能。

---

## 问题 8：能连上、能拿 IP，但任何公网地址都不通

### 遇到了什么问题

网关通、域名能解析，但公网全不通：

```
msh /> ping 10.237.107.49                    # 网关(同网段)
4 packets transmitted, 4 received, 0% packet loss        <- 通

msh /> ping www.baidu.com                    # 公网
ping: from 180.101.51.73 icmp_seq=1 timeout
4 packets transmitted, 0 received, 100% packet loss      <- 不通
```

一度被误判为「运营商封 ICMP」/「手机热点禁止共享」。加 `nettest` 后真相出现：

```
msh /> nettest 180.101.51.73 80
nettest: CONNECT FAIL (errno=118, 0 ms)
```

### 发现是什么原因

**`0 ms`** = 一个 SYN 都没发出去，`connect()` 在**本地**就失败了。网络被封不可能是 0 毫秒。

**`errno=118`** 要查**工具链的 `sys/errno.h`**（`lwipopts.h:24` 写了
`LWIP_ERRNO_INCLUDE "sys/errno.h"`，用的是 newlib 表，**不是 lwIP 自带表**）：

| 名字 | lwIP 自带表 | 本工程实际用的 newlib |
|---|---|---|
| `ETIMEDOUT` | 110 | **116** |
| `EHOSTUNREACH` | 113 | **118** ← 本案 |
| `ENETUNREACH` | 101 | **114** |

所以 `118` = `EHOSTUNREACH`，来自 lwIP 的路由查表失败。

`lwip-2.1.2/src/core/ipv4/ip4.c:229`：跨网段目的地只能靠全局 `netif_default` 兜底，
为 NULL 就直接返回"无路由"。而 `netif_default` **从来没被赋值**（`netif.c:119` 无初值，
只有 `netif_set_default()` 会写它，`netif_add()` 不会）。

本该写它的链条断在第一步：

```
netdev_register()                        netdev.c:126-129
  └─ netdev_set_default()                netdev.c:483-502
       └─ if (netdev->ops && netdev->ops->set_default)   ← ★ 断点
            └─ lwip_netdev_set_default()  ethernetif.c:321
                 └─ netif_set_default()   ← 真正写入
```

问题 6 的写法是"**先 `netdev_register()`、后装 `ops`**"，于是那一刻 `nd->ops == NULL`，
整条同步链被**静默跳过**。

**为什么只有同网段能用**：

| 目的地 | 路由方式 | 需要 `netif_default`？ |
|---|---|---|
| DHCP 广播 / 网关 / 到网关的 DNS | 同网段，`ip4_addr_netcmp` 命中本 netif | ❌ → **一直正常，掩盖了问题** |
| 任何公网地址 | 都不命中 → 落到默认接口 | ✅ → NULL，"无路由" |

### 怎么修改的

**文件**：`platform/netif/netif_port.c` 的 `aicwf_net_if_netdev_add()` —— 把字段赋值**整体前移**到 `netdev_register()` 之前（代码见问题 6 的 (2)，★ 标注处即本改动）。

对应的**改动前**形态（便于 diff）：

```c
    if (netdev_register(nd, name, nif) != 0) { ... }   /* 先注册 */

    nd->ops        = &lwip_netdev_ops;                 /* 后装 ops —— 太晚了 */
    nd->hwaddr_len = nif->hwaddr_len;
    memcpy(nd->hwaddr, nif->hwaddr, nif->hwaddr_len);
    nd->ip_addr    = nif->ip_addr;
    nd->gw         = nif->gw;
    nd->netmask    = nif->netmask;
    s_netdev_used[idx] = 1;
```

另在 `aicwf_net_if_netdev_sync()` 末尾加兜底（代码见问题 7 的 `if (netif_default == RT_NULL) netif_set_default(nif);`），
覆盖"netdev 不是第一个注册"的边界情况。

> **为什么前移是安全的**：`netdev_register()` 内部**不做 `memset`**，只写
> `name` / `user_data` / `list` / `ifindex`，预先填好的字段不会被冲掉；
> 且 `user_data`（`netdev.c:104`）在 `netdev_set_default()`（`netdev.c:128`）**之前**已赋值，
> 所以钩子里 `netif_set_default(netif->user_data)` 拿到的正是本 netif。

### 结果如何

```
msh /> nettest 180.101.51.73 80
nettest: CONNECT OK (102 ms)  <== 目标 TCP 端口可达, 公网是通的

msh /> ping www.baidu.com
60 bytes from 180.101.49.44 icmp_seq=1 ttl=52 time=100 ms
60 bytes from 180.101.49.44 icmp_seq=2 ttl=52 time=119 ms
60 bytes from 180.101.49.44 icmp_seq=3 ttl=52 time=109 ms
60 bytes from 180.101.49.44 icmp_seq=4 ttl=52 time=129 ms
4 packets transmitted, 4 received, 0% packet loss
```

**公网 TCP 与 ICMP 同时打通。** 之前的"运营商封 ICMP / 热点禁止共享"两个推测**都被推翻**。

---

# 阶段三：环境问题（问题 9）

## 问题 9：`sta conn` 途中芯片被复位（**外部供电不足**）

### 遇到了什么问题

公网已经跑通之后，换了一次连接又失败，HCPU 侧：

```
09:44:10:088  [FW] join:jia-ni,2412,bw=0
09:44:10:094  [FW] misc dpd calib single      ← 射频校准开始
09:44:10:094  [FW] dpd start
09:44:10:243  [FW] t1        09:44:10:495  t2
09:44:10:598  [AIC][SDIO]__aicwf_sdio_readb_cmd52 fail -116!   ← 共 15 条
              [AIC][SDIO]aicwf_sdio_rx_task, ret=0, intstatus=0   （交替出现）
09:44:10:786  [AIC][CMN]Assertion failed: 0, file ...\platfor...
09:44:16 起   TX: 后面没有任何回显（按回车无反应 = 系统已死）
09:44:20:093  [AIC][APP]app_wlan_sta_start: start sta fail -7
```

而模组自己的串口上出现了**全新的 ROM banner**：

```
09:51:00:729  driver start / is 5g=1, sw_en=1
09:51:11:824  Bootrom [Sep 11 2025, g38f9137]   ← ★ 芯片被完整复位
09:51:11:837  usb init done                      ← 之后停住（在等主机枚举，而主机已死）
```

### 发现是什么原因

**模组通过 USB 扩展坞供电，同一路 USB 口还带了键盘 —— 供电能力不足。**

DPD 是射频校准，是芯片电流最大的时刻之一；电压跌落把模组拉到复位阈值以下 → **芯片掉电复位**。
芯片一复位就不再响应 SDIO，于是：

```
芯片复位
  ↓ 复位期间不响应 SDIO 命令
SDHCI 报 CMD52 命令超时 → drv_sdhci.c:1048 host->cmd->err = -ETIMEDOUT (= -116)
  ↓ sdio_port.c:741 打印 __aicwf_sdio_readb_cmd52 fail -116!
  ↓ sdio_port.c:384 SDIO_WR_FAIL 连续失败计数 wr_fail++（成功则清零）
  ↓ 连续 16 次 → wr_fail > 15
  ↓ plat_port.c:392 aicwf_driver_unexcepted(0x04)
  ↓ plat_port.c:405 AIC_ASSERT_ERR(0) → aic_assert.h:11 while(1)
主机永久死亡（芯片重启后停在 usb init done 等主机，而主机已死）
```

**为什么日志里恰好 15 条 `fail -116!`**：`SDIO_WR_FAIL` 是 `if (wr_fail > 15)`，
第 16 次时计数变 16 → 成立 → **在打印之前就断言了**。看到 15 条就知道下一行必然是断言。

### 怎么修改的

**不是代码改动，是供电环境**：

| 措施 | 说明 |
|---|---|
| 模组**单独供电** | 不与键盘、U 盘等外设共用一路 USB 口 |
| 不用供电能力不明的 USB 扩展坞 | 必要时用带独立电源适配器的 Hub |
| 硬件加固 | 模组 3.3V 就近并 **100µF 电解 + 100nF 陶瓷**，靠它顶住射频峰值电流 |
| （可选）软件韧性 | 见下 |

**为什么这件事极具伪装性**：芯片复位在主机侧**只表现为"命令超时"**，看不出是复位。
它先后被误判成"SDIO 总线/时序故障"、"驱动移植问题"、"固件 crash"。
**唯一的决定性证据在芯片侧：模组串口出现全新的 `Bootrom [...]` banner。**

### 结果如何

改用独立供电后不再复位。

一次测量即可定案（示波器看 **WL_REG_ON / GPIO124** 与 **模组 3.3V**）：

| 波形 | 结论 |
|---|---|
| WL_REG_ON 全程高，芯片却打新 `Bootrom` | **芯片自己复位**（掉电/看门狗/内部异常）→ 查供电 ← **本案** |
| WL_REG_ON 出现下拉脉冲 | 主机或电源轨塌陷把它复位了 → 查主机恢复逻辑 / 供电 |

**可选加固（产品化时再动）**：现在连续 16 次 SDIO 失败就 `AIC_ASSERT_ERR(0)` → `while(1)`，
主机**无法自恢复**。可把 `plat_port.c:405` 的 `case 0x04:` 改成
`aicwf_aic_driver_reboot(0x03)`（与 `0x01/0x02` 一致，断电重启芯片），让主机能自愈。
属厂商代码，且会把真实故障一并掩盖成"重启一下就好"，调试期建议保持现状。

---

# 附录 A：`nettest` 命令（诊断工具）

## 为什么需要它

`ping` 用 netdev 层，**不打印 errno**——"本地路由失败"和"对方没回包"在它眼里都是 `timeout`，
长得一模一样。问题 8 就因为这一点被误判成网络问题，白绕了两轮。
`nettest` 把 **errno + 耗时**打出来，用"多久失败"一刀切开：

| 耗时 | 含义 |
|---|---|
| **0 ms** | 本地就失败（驱动/路由表），与网络无关 |
| 几 ms ~ 几百 ms | 收到了回应（RST / ICMP 不可达），**网络是通的** |
| 接近超时上限 | 无人应答，**网络不通** |

## 代码

**文件**：`example/aic8800_d80/src/main.c`
**位置 1**：文件头部 include

```c
/* nettest 命令所需的 socket 头 (与 packages/iot/netutils/iperf 相同的取法) */
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
```

**位置 2**：文件末尾

```c
/* ===========================================================================
 * MSH: nettest <ip或域名> <端口> [超时秒数, 默认8]     —— TCP 连通性测试
 *
 * 用法示例:
 *   nettest 180.101.51.73 80        # 直接给 IP
 *   nettest www.baidu.com 80        # 也可以给域名, 内部做 DNS 解析
 *   nettest 114.114.114.114 53
 *
 * 打印与判读:
 *   CONNECT OK    三次握手成功 -> 目标端口可达
 *   TIMEOUT       对方/路径上无任何回应 -> 被丢弃或不可达
 *   CONNECT FAIL  有回应但被拒绝(errno 非 0) -> 网络本身是通的
 *                 ★ 但要结合耗时看: 0ms 的 FAIL 其实是本地失败
 * ========================================================================= */
static struct rt_semaphore s_nt_sem;
static rt_bool_t         s_nt_sem_inited = RT_FALSE;
static volatile int      s_nt_ret;      /* 0=OK, -1=socket失败, -2=connect失败 */
static volatile int      s_nt_errno;
static struct sockaddr_in s_nt_addr;

static void nettest_worker(void *param)
{
    int sock, ret;
    (void)param;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        s_nt_ret   = -1;
        s_nt_errno = errno;
        rt_sem_release(&s_nt_sem);
        return;
    }

    ret = connect(sock, (struct sockaddr *)&s_nt_addr, sizeof(s_nt_addr));
    if (ret == 0)
    {
        s_nt_ret = 0;
    }
    else
    {
        s_nt_ret   = -2;
        s_nt_errno = errno;
    }
    closesocket(sock);

    rt_sem_release(&s_nt_sem);
}

static int nettest(int argc, char **argv)
{
    rt_thread_t tid;
    rt_tick_t   t0;
    rt_int32_t  timeout_ms;
    struct hostent *he;
    char ipstr[16];
    int  port, dt;

    if (argc < 3)
    {
        rt_kprintf("usage: nettest <ip|host> <port> [timeout_s, default 8]\n"
                   "  e.g. nettest 180.101.51.73 80\n"
                   "       nettest www.baidu.com 80\n");
        return -1;
    }

    port = atoi(argv[2]);
    if (port <= 0 || port > 65535)
    {
        rt_kprintf("nettest: bad port '%s'\n", argv[2]);
        return -1;
    }
    timeout_ms = (rt_int32_t)((argc >= 4) ? atoi(argv[3]) : 8) * 1000;
    if (timeout_ms <= 0)
        timeout_ms = 8000;

    rt_memset(&s_nt_addr, 0, sizeof(s_nt_addr));
    s_nt_addr.sin_family = AF_INET;
    s_nt_addr.sin_port   = htons((rt_uint16_t)port);

    s_nt_addr.sin_addr.s_addr = inet_addr(argv[1]);
    if (s_nt_addr.sin_addr.s_addr == INADDR_NONE)
    {
        /* 不是点分十进制 -> 当域名处理 (顺带验证 DNS) */
        he = gethostbyname(argv[1]);
        if (!he || !he->h_addr_list || !he->h_addr_list[0])
        {
            rt_kprintf("nettest: cannot resolve '%s'\n", argv[1]);
            return -1;
        }
        rt_memcpy(&s_nt_addr.sin_addr, he->h_addr_list[0], sizeof(s_nt_addr.sin_addr));
    }

    rt_strncpy(ipstr, inet_ntoa(s_nt_addr.sin_addr), sizeof(ipstr) - 1);
    ipstr[sizeof(ipstr) - 1] = '\0';
    rt_kprintf("nettest: TCP %s:%d (timeout %d ms) ...\n", ipstr, port, (int)timeout_ms);

    if (!s_nt_sem_inited)
    {
        rt_sem_init(&s_nt_sem, "ntest", 0, RT_IPC_FLAG_FIFO);
        s_nt_sem_inited = RT_TRUE;
    }
    else
    {
        /* 抽干上一次超时后工作线程才送回的残留 token */
        while (rt_sem_take(&s_nt_sem, 0) == RT_EOK)
            ;
    }

    s_nt_ret   = 0x7FFFFFFF;
    s_nt_errno = 0;

    tid = rt_thread_create("ntest", nettest_worker, RT_NULL, 2048, 20, 10);
    if (!tid)
    {
        rt_kprintf("nettest: no memory for worker thread\n");
        return -1;
    }
    rt_thread_startup(tid);

    t0 = rt_tick_get();
    if (rt_sem_take(&s_nt_sem, rt_tick_from_millisecond(timeout_ms)) != RT_EOK)
    {
        rt_kprintf("nettest: TIMEOUT after %d ms  <== 目标/路径无任何回应(被丢弃或不可达)\n",
                   (int)timeout_ms);
        return -1;
    }

    dt = (int)((rt_tick_get() - t0) * 1000 / RT_TICK_PER_SECOND);
    if (s_nt_ret == 0)
    {
        rt_kprintf("nettest: CONNECT OK (%d ms)  <== 目标 TCP 端口可达, 公网是通的\n", dt);
        return 0;
    }
    if (s_nt_ret == -1)
    {
        rt_kprintf("nettest: socket() FAIL (errno=%d)\n", s_nt_errno);
        return -1;
    }

    rt_kprintf("nettest: CONNECT FAIL (errno=%d, %d ms)  <== 有回应但被拒, 网络本身是通的\n",
               s_nt_errno, dt);
    return -1;
}
MSH_CMD_EXPORT(nettest, TCP connectivity test: nettest <ip|host> <port> [timeout_s]);
```

**三个设计要点**：
1. **为什么单开线程**：lwIP 的 `connect()` 阻塞且 SYN 重传超时不可控（可能几十秒），故丢进工作线程，
   msh 侧用信号量等固定超时。
2. **信号量不 detach**：超时返回时工作线程可能还阻塞在 `connect()` 里，
   若此时 `rt_sem_detach()`，线程稍后 `rt_sem_release()` 会操作已销毁对象。
   故信号量只初始化一次、常驻，每次进入前抽干残留 token。
3. **支持域名**：先 `inet_addr()` 试点分十进制，失败再 `gethostbyname()`，顺带验证 DNS。

---

# 附录 B：编译 / 烧录 / 验收

## 编译

```powershell
& "D:\SiFli\SDK\SiFli-SDK-WiFi\setup_sifli.ps1"
cd D:\SiFli\SDK\SiFli-SDK-WiFi\SiFli-SDK\example\aic8800_d80\project
scons --board=sf32lb55-lcd_n16r4n4 -j8
```

- 若 `export.ps1` 报 pypi 网络错误：先 `$env:UV_OFFLINE = "1"`（用已缓存的 uv 包），
  或 `$env:SIFLI_SDK_MIRROR_CHINA = "1"` 走国内镜像。
- **A/B 对比前务必 `scons --board=... -c` 清一次**——本工程多次出现"改了却烧旧镜像"的误判。
- 产物：`project/build_sf32lb55-lcd_n16r4n4_hcpu/main.bin`（或同目录 `download.bat`）。

## 串口命令（注意坑）

`aic_wifi_cmd()` 会丢弃整行命令的**第一个 token**（`AICWF_CMD_SKIP_PREFIX=1`），
所以第一个 token 是**占位符**（文档习惯写 `iwpriv`）：

```
wifi_test iwpriv uapst                    # 查 wifi 状态
wifi_test iwpriv sta scan                 # 扫描(阻塞, 数秒)
wifi_test iwpriv sta conn <ssid> <pwd> 30000
wifi_test iwpriv sta stop
```

`sta conn` 是**同步阻塞**的（连接最长 30s + DHCP 最长 10s），期间 tshell 不回显，**耐心等结果**。

## 终版验收判据

```
msh /> ifconfig
FLAGS: UP LINK_UP DHCP_ENABLE
ip address: 10.237.107.34
gw address: 10.237.107.49

msh /> ping 10.237.107.49          -> 4/4, 0% loss
msh /> nettest 180.101.51.73 80    -> CONNECT OK
msh /> ping www.baidu.com          -> 4/4, ttl=52
```

| 检查项 | 修复前 | 终版 |
|---|---|---|
| 芯片启动 | 卡在 `msg fc:0` | CINIT → PATCH → CALIB 全过 |
| 连接 | `reason=15` 握手超时 | `WPA network 0: connected` |
| shell | `msh />` 只打印一次后卡死 | 正常回显 |
| `sta conn` | `hard fault` STKOF | 正常 |
| `ifconfig` | `LINK_DOWN` / `MTU:0` / `0.0.0.0` | `LINK_UP` / `1500` / 真实 IP |
| `ping` | `status error`（0 个包） | 正常收发 |
| `nettest` 公网 | `CONNECT FAIL (errno=118, 0 ms)` | `CONNECT OK (102 ms)` |

## 正常现象（别当故障）

| 现象 | 解释 |
|---|---|
| `ping: not found specified netif, using default netdev wl.` | msh 的 `ping` 不传接口名，**每次都会打印**，后面跟的才是结果 |
| 空闲后首包丢失、第二包耗时 100ms+ | 需重建 BA 会话（`[FW] mm_ba_add_req_handler`），叠加 10ms 轮询兜底 |
| `[FW] trial rate/power=...` | 固件在做速率自适应 |
| `FLAGS` 里没有 `ETHARP/BROADCAST/IGMP` | 纯显示项（`netdev.c:1137-1139`），不影响功能 |

---

# 附录 C：口头汇报版

> 下面这段可以直接照着念，约 3 分钟。

---

这个 AIC8800D80 的 WiFi 移植，从"完全跑不起来"做到"能连、能上网"，一共解决了 **8 个问题**，
分两个阶段。

**第一阶段是 SDIO 和固件加载，5 个问题。**

第一个，初始化卡在 `msg fc:0`。查下来不是总线问题，是芯片的 cfg 通道报"没有可用缓冲"——
驱动库把流控寄存器硬编码成 Q1，而 D80N 在 ROM 阶段只有 Q2 有信用。我在 CMD52 的**读入口**
加了一层 `0x03 → 0x09` 的重定向，初始化就 100% 过了。

第二个，能扫到 AP、能关联，但 4 次握手总是超时。原因还是那个重定向——
它是**永久**生效的，补丁固件跑起来后数据通路判据变成 `fc_reg > 2`，读 Q2 恒等于 2 永远过不了，
EAPOL 发不出去。所以我在"补丁固件启动成功"那个锚点上把重定向收回，改回读真实 Q1，握手立刻通了。

第三个，驱动起来后整机卡死，回车没反应。根因是 **SDIO DAT1 电平中断风暴**——
固件一直拉低 DAT1，闭源库每轮结束又无条件重开中断，而 SDHCI 既不 mask 也不清状态位，
形成死循环，把串口中断饿死了。我在 platform 层把"放开 SDIO 中断"的动作全拦住，
收包改走库自带的 10ms 轮询。**这是规避不是根治**，正解要改 SDHCI 驱动的中断语义。

第四个，`sta conn` 直接 hard fault。是 `STKOF` 栈溢出，wpa 任务栈只有 3KB、
实测已用 95%。加到 8KB 就好了。

第五个是板级配置：回到 4 线 SDIO，并确认 CMD/DAT0~3 全部上拉。

**第二阶段是网络接口，3 个问题。这一阶段有个统一的根子。**

我们走的是"驱动自建 lwIP netif"的路线，它绕开了 SDK 标准网卡里的三件自动同步，
所以这三件都得手工补——**三个问题正好一一对应**。

第六个，DHCP 拿到地址的瞬间 tcpip 线程断言崩溃。因为这套 SDK 把 DNS 存在 netdev 上，
而我们的自建 netif 没有配套 netdev，一查就是空指针。补上 netdev 注册就好了。

第七个，`ping` 一个包都不发，报 `status error`。因为驱动的链路标志是**直接写 `netif->flags`** 的，
绕过了 `netif_set_link_up()`，而 SDK 只在那个函数里才会把状态同步给 netdev，
所以 netdev 那边一直是 `LINK_DOWN`。我加了一个镜像函数把状态和地址同步过去。

第八个最难找——网关联得通、域名也能解析，但**任何公网地址都不通**。
一开始我判断是运营商封了 ICMP，还怀疑过手机热点不让共享，**两个方向都是错的**。
后来我加了一条 `nettest` 命令专门做 TCP 连接测试，把 errno 和耗时打出来，
发现失败只要 **0 毫秒**——包根本没发出去，是本地路由查表就失败了。
根因是 lwIP 的全局 `netif_default` 从来没被设置过。因为第六个问题的写法是"先注册 netdev、
后装 ops 表"，而注册内部那次"设置默认接口"的钩子正好需要 ops，那一刻它是空的，
整条链被静默跳过了。我把字段赋值前移到注册之前，公网 TCP 和 ICMP 就同时通了。

**最后的结果**：`ifconfig` 显示 `UP LINK_UP`、真实 IP；网关 4/4 零丢包；
`nettest` 公网 `CONNECT OK`；`ping www.baidu.com` 4/4、ttl=52。

**最后还有一个坑，跟代码完全没关系。**

公网跑通之后，我换了一次连接，结果又失败了，这次是主机直接卡死。
查下来是 SDIO 命令超时触发了驱动的"连续 16 次就断言"看门狗。
一开始我怀疑是 SDIO 总线故障、4-bit 时序、甚至固件崩溃。后来拿到模组自己串口的日志，
发现它打出了一条**全新的 Bootrom 启动横幅**——**芯片被复位了**。

真正的原因是：模组走 USB 扩展坞供电，同一个 USB 口还带了键盘，**供电能力不够**；
芯片在射频校准那一瞬间电流最大，电压跌落，掉电重启。改成独立供电就好了。

这件事的教训是：**芯片复位在主机侧看起来只是"命令超时"**，完全看不出是复位。
一定要去看芯片自己的串口——只要出现新的 `Bootrom` 横幅，就别再往总线、协议、驱动方向查了。

**如果要留给后面的人一句话**：这套"自建 netif"的路线，SDK 不会替你做的同步一共有三件
——建 netdev、同步 netdev 状态、设 lwIP 默认接口，缺任何一个，症状都会被包装成"网络问题"。
还有，**`ping` 的 timeout 不能证明包已经发出去了**，排查时一定要看 errno 和耗时，
`0 毫秒` 一定是本地问题。这一点我们绕了两轮，已经写进文档了。

---

## 相关文档

| 文档 | 内容 |
|---|---|
| [aic8800d80_sdio_bringup_handoff.md](aic8800d80_sdio_bringup_handoff.md) | 早期 SDIO bring-up 排查交接（历史） |
| [aic8800d80n_flowctrl_q1q2_vendor_report.md](aic8800d80n_flowctrl_q1q2_vendor_report.md) | Q1/Q2 问题的厂商报告 |
| [aic8800d80n_flowctrl_postfw_q1_switch.md](aic8800d80n_flowctrl_postfw_q1_switch.md) | FW 运行后切回 Q1 的实验与判读 |
| [aic8800d80_patch_flowctrl_phase_and_netdev_reregister.md](aic8800d80_patch_flowctrl_phase_and_netdev_register.md) | 补丁1（flow-ctrl）+ 补丁2（netdev 注册）细则 |
| [aic8800d80_freeze_and_wpa_fixes.md](aic8800d80_freeze_and_wpa_fixes.md) | 卡死 + wpa 栈溢出的细则 |
| [aic8800d80n_final_changes_and_history.md](aic8800d80n_final_changes_and_history.md) | 第一轮终版清单 + 修改史 |
| [aic8800d80_netdev_linkup_and_ping_fix.md](aic8800d80_netdev_linkup_and_ping_fix.md) | 补丁3 过程记录 |
| [aic8800d80_lwip_netif_default_missing.md](aic8800d80_lwip_netif_default_missing.md) | 补丁4 过程记录 |
| [aic8800d80_network_connectivity_test_guide.md](aic8800d80_network_connectivity_test_guide.md) | `nettest` 用法 + 网络排查手册 |
| [aic8800d80_netif_netdev_final_changes.md](aic8800d80_netif_netdev_final_changes.md) | 网络接口层终版改动清单（照抄版） |
| [aic8800d80_power_supply_brownout_reset.md](aic8800d80_power_supply_brownout_reset.md) | 芯片被复位（外部供电不足）的完整判据链与规避措施 |
| **本文** | **全工程完整修改流程 + 口头汇报版** |
