# AIC8800D80N SDIO 适配 —— 终版改动清单 + 修改史

> **用途**：任何人按 §1 照抄即可把代码改到"当前可用终版"；§3 记录了本工程演进过程中**被推翻的中间方案**及其原因，避免重复踩坑。
> **适用范围**：SiFli SDK 2.5.0（Wi-Fi 树 `SiFli-SDK-WiFi\SiFli-SDK`）/ board `sf32lb55-lcd_n16r4n4` / 模组 AIC8800D80N（卡级 `VID=0xC8A1 PID=0x9082` → `chipid=4`）/ SDIO **4-bit**。
> **相关文档**：故障根因与给厂商的问题清单见 [aic8800d80n_flowctrl_q1q2_vendor_report.md](aic8800d80n_flowctrl_q1q2_vendor_report.md)；早期排查过程见 [aic8800d80_sdio_bringup_handoff.md](aic8800d80_sdio_bringup_handoff.md)。
> **后续（网络层）**：本文只覆盖 SDIO/flow-ctrl/中断风暴。之后网络接口层又修了三处缺口
> （netdev 注册、netdev 状态镜像、lwIP `netif_default`），终版清单见
> [aic8800d80_netif_netdev_final_changes.md](aic8800d80_netif_netdev_final_changes.md)。
> **两份合起来才是整个工程的终版。**

---

## 0. 一句话总结

四类改动（2 处在驱动 platform 层、1 处工程配置、1 处板级），核心是**"flow-ctrl 读侧重定向 + 阶段切换"**：

| # | 位置 | 改什么 | 为什么 |
|---|---|---|---|
| A | `aic8800_rtos_driver/platform/hal/sdio_port.c` | func1 CMD52 读入口把 `0x03(Q1)` 重定向到 `0x09(Q2)`；前 5 次先哑读 Q1/Q2；补丁 FW 启动后**切回真实 Q1** | 库把 flow-ctrl 硬编码成 Q1，而这颗 ROM 只在 Q2 给信用；若永久读 Q2 会让**数据通路**判据失败（4-way 握手超时） |
| B | 同上 | `g_sdio_irq_mask_forced`：不放行 SDIO(DAT1) 中断，收包走库内 10ms 轮询 | SDHCI 卡中断是**电平**中断，库每轮无条件 re-enable → 中断风暴 → 串口被饿死（shell "卡死"） |
| C | `example/aic8800_d80/project/proj.conf` | `CONFIG_SDMMC2_BUS_WIDTH_1_ONLY=n`（4 线） | 1-bit 只是早期为隔离 D1 的临时手段；4 线已验证可用 |
| D | `example/aic8800_d80/src/main.c` | 打开中断规避策略（`AIC_SDIO_IRQ_MASK_FORCED=1` + `aicwifi_sdio_rxdata_mode=1`）、保留 `wifi_test` 命令 | 配合 B；`wifi_test` 是唯一操作入口 |

板级（不属于本工程，但必须一致）：`customer/boards/sf32lb55-lcd_base/bsp_pinmux.c` 的 SD2 六根线 = `CLK:NOPULL`、`CMD/DIO0~3:PULLUP`。

---

## 1. 终版改动清单（照抄即可）

### 1.A 【核心】flow-ctrl 读侧重定向 + 阶段切换

**文件**：`customer/peripherals/wifi/aic8800_rtos_driver/platform/hal/sdio_port.c`

**(1) 阶段开关与状态变量（放在 `aicwf_sdio_readb_cmd52()` 之前，约 776–807 行）**

```c
/* 阶段开关注释块（略，见文件）：说明 msg 通路判据 fc_reg != 0、数据通路判据 fc_reg > 2 */
#define AIC_FC_Q1_AFTER_FWBOOT   1      /* 1 = 补丁 FW 启动后停止重定向, 直读真实 Q1 */
#define AIC_FC_RAW_DBG           0      /* 1 = 周期性附带打印 raw Q1/Q2 (默认关, 见 §3 第 9 条) */
#define AIC_FC_RAW_DBG_PERIOD    2000

/* 1 = ROM/CINIT/PATCH/CALIB 阶段: 读 Q1 即重定向到 Q2;
 * 由 aicwf_sdio_enable_irq_after_fw_boot() 在补丁 FW 启动成功后清零 */
static volatile int g_fc_redirect_q2 = 1;
```

**(2) 读入口本体（`bool aicwf_sdio_readb_cmd52(uint32_t addr, uint8_t *data)`，约 815–872 行）**

```c
bool aicwf_sdio_readb_cmd52(uint32_t addr, uint8_t *data)
{
    if (addr == SDIOWIFI_FLOW_CTRL_Q1_REG_V3)
    {
        static uint32_t fc_dbg_cnt = 0;
        uint8_t v = 0;
        bool ok;

        fc_dbg_cnt++;

        if (fc_dbg_cnt <= 5)
        {
            /* ★ 承重逻辑: 前 5 次必须先哑读 Q1 + Q2, 且两次 rt_kprintf 的延时也是必要的
             *   (去掉 -> 拿不到信用; 改成"每次都哑读 Q1" -> 芯片持续报 error bit:800 且要重试很多次) */
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
            uint8_t v03 = 0, v09 = 0;
            bool ok03 = aicwf_sdio_readb_cmd52_by_num(FUNC_1, SDIOWIFI_FLOW_CTRL_Q1_REG_V3, &v03);
            bool ok09 = aicwf_sdio_readb_cmd52_by_num(FUNC_1, SDIOWIFI_FLOW_CTRL_Q2_REG_V3, &v09);
            rt_kprintf("[AICFC] #%u raw 0x03=%u(ok%d) 0x09=%u(ok%d) redir=%d\n",
                       fc_dbg_cnt, v03, ok03, v09, ok09, g_fc_redirect_q2);
        }
#endif

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

**(3) 阶段切换点（`int aicwf_sdio_enable_irq_after_fw_boot(void)` 末尾，约 1508–1517 行）**

```c
#if AIC_FC_Q1_AFTER_FWBOOT
    /* 补丁 FW(PATCH/CALIB) 已启动, 数据队列(Q1)应已开始发放 TX 信用:
     * 在此收回 ROM 阶段的 Q1->Q2 读重定向, 让数据通路读到真实 Q1。
     * 本函数是库在 "start_from_bootrom success." 之后调用的第一个明确锚点。 */
    if (g_fc_redirect_q2) {
        g_fc_redirect_q2 = 0;
        rt_kprintf("[AICFC] patch FW up -> flow-ctrl read back to real Q1(0x03)\n");
    }
#endif
```

**理由（三条，缺一不可）**

1. **为什么在"读"侧而不是"写"侧**：库的 `aicwf_sdio_reg_init()` 对 chipid 3/4/5 把
   `sdio_dev.sdio_reg.flow_ctrl_reg` 硬编码成 `SDIOWIFI_FLOW_CTRL_Q1_REG_V3`（反汇编：
   `movs r3,#3; strb r3,[sdiodev+8]`），而平台无法在库之后可靠地改这个字段（实测 `field=0x03` 一直不变）。
   库每次读 flow-ctrl 都经过 `aicwf_sdio_readb_cmd52()`（反汇编 `aicwf_sdio_flow_ctrl_msg`：
   取 `[sdio_dev+8]` 当地址 → `bl aicwf_sdio_readb_cmd52`），所以在读侧翻译与调用顺序/时机无关。
2. **为什么需要"前 5 次哑读 Q1/Q2 + 延时"**：实测只有这个序列能让 ROM/CINIT 阶段的 Q2(0x09) 给出信用（`=2`）；
   直接读 Q2 拿不到；每次都读 Q1 会让芯片持续报 `sdio func1 error bit:800, func1 free cnt=0, func1 free queue2 cnt=2`
   并需要大量重试。详见 §3 第 7/8/9 条。
3. **为什么 FW 起来后必须切回 Q1**：msg(cfg) 通路判据是 `fc_reg != 0`（Q2=2 能满足），
   但**数据通路**判据是 `fc_reg > DATA_FLOW_CTRL_THRESH(2)`。永久读 Q2 会让数据通路永远拿不到信用 →
   `data fc:-2` → 4-way 握手 EAPOL msg2/4 发不出 → AP 4 次重发后 deauth，`reason=15(4WAY_HANDSHAKE_TIMEOUT)`。
   切换点选 `aicwf_sdio_enable_irq_after_fw_boot()`：它是库在 `start_from_bootrom success.` 之后调用的第一个明确锚点。

---

### 1.B 【必需】SDIO(DAT1) 中断风暴规避

**文件**：同一个 `sdio_port.c`

**(1) 全局开关（约 139–152 行）**

```c
/* SDIO(DAT1) 中断风暴规避: SDHCI_INT_CARD_INT 是电平中断(DAT1 被卡拉低期间一直有效),
 * 库内 aicwf_sdio_rx_task 每轮结束都无条件 sdio_host_enable_isr(TRUE) →
 * "中断→唤醒RX任务→读 intstatus=0→再放开中断→立刻又中断" 死循环;
 * 该中断 NVIC 优先级高于串口 → 串口 RX 永远得不到服务 → 表现为"整机卡死"
 * (只打印一次 "msh />", 之后回车无回显)。
 * 规避: 置 1 时所有"使能 SDIO 中断"的路径都保持屏蔽, 收包由 10ms 轮询兜底。 */
uint32_t g_sdio_irq_mask_forced = 0;            /* 1 = 不放开 SDIO 中断 */
```

**(2) 两处"使能中断"路径（约 997–1027 行）**

```c
void aicwf_sdio_isr(uint8_t en)
{
    if (en && g_sdio_irq_mask_forced) { aicwf_mmc_enable_isr(0); return; }
    aicwf_mmc_enable_isr(en);
}

bool aicwf_sdio_host_enable_isr(bool enable)
{
    if (enable && g_sdio_irq_mask_forced) { aicwf_mmc_enable_isr(0); return true; }

    aicwf_mmc_enable_isr(enable);
    if (!enable) { /* 原逻辑: 释放 FUNC_1/FUNC_2 的 IRQ */ }
    return true;
}
```

**理由**：见上面的注释。注意这是**电平**中断特有现象，属"没有上拉/对端持续拉低"叠加"库无条件 re-enable"的合力；
真正消除要靠厂商侧修 rx_task 的中断语义（或硬件 DAT1 上拉）。

---

### 1.C 工程配置 `project/proj.conf`

```ini
CONFIG_BSP_USING_SDIO=y
CONFIG_BSP_USING_SDMMC2=y
CONFIG_SDIO2_CARD_MODE=2
CONFIG_RT_SDIO_THREAD_PRIORITY=9
CONFIG_SDMMC2_BUS_WIDTH_1_ONLY=n      # 4 线; 置 y 为 1-bit(仅用于隔离 D1/D2/D3 的排查)
CONFIG_RT_SDIO_DEBUG=y
CONFIG_WIFI_USING_AIC8800_RTOS=y
CONFIG_AIC8800_RTOS_WIRELESS_PWR_GPIO=124
```

**理由**：`SDMMC2_BUS_WIDTH_1_ONLY=y` 会让 `drv_sdhci.c:1598` 不再声明 4-bit、`sdio.c:sdio_set_bus_wide()` 跳过写 CCCR 0x07；
早期用它隔离 D1 的规律性下拉，**4 线已验证可用**后改回 `=n`（4 线吞吐更好，也是量产姿态）。

---

### 1.D 工程 `src/main.c`

关键部分（其余为骨架 + `wifi_test` 命令）：

```c
/* 1 = 规避 SDIO 中断风暴(见 sdio_port.c 的 g_sdio_irq_mask_forced), 0 = 原样(会卡死, 用于 A/B) */
#define AIC_SDIO_IRQ_MASK_FORCED   1

extern unsigned char aicwifi_sdio_rxdata_mode;  /* sdio_port.c: 0=中断驱动 1=轮询 */
extern uint32_t      g_sdio_irq_mask_forced;    /* sdio_port.c: 1=不放开 SDIO 中断 */

static void aic_sdio_irq_policy_apply(void)
{
#if AIC_SDIO_IRQ_MASK_FORCED
    aicwifi_sdio_rxdata_mode = 1;
    g_sdio_irq_mask_forced   = 1;
#endif
    rt_kprintf("[aic8800] AIC_SDIO_IRQ_MASK_FORCED=%d\n", AIC_SDIO_IRQ_MASK_FORCED);
}

int main(void)
{
    rt_kprintf("[aic8800] demo start\n");
    aic_sdio_irq_policy_apply();        /* 必须在驱动初始化之前 */
    aic_wifi_demo_test();
    rt_kprintf("[aic8800] driver up. try: wifi_test iwpriv sta scan\n");
    return 0;
}
```

**注意**：`AIC_FC_FORCE_Q2`/`aic_fc_fix_thread`（早期在 main.c 里定时改 `sdio_dev[8]` 的临时线程）**已删除**，
该职责已由 §1.A 的读侧重定向取代。详见 §3 第 5/6 条。

`wifi_test` 命令：`aic_wifi_cmd()` 会丢弃整行第一个 token，故第一个参数是**占位符**：

```
wifi_test iwpriv uapst                  # 查 wifi 状态
wifi_test iwpriv sta scan               # 扫描(阻塞), 末尾 +OK / +NONE
wifi_test iwpriv sta conn <ssid> <pwd> <timeout_ms>
```

---

### 1.E 板级 pinmux（非本工程，但必须一致）

`customer/boards/sf32lb55-lcd_base/bsp_pinmux.c`：

```c
HAL_PIN_Set(PAD_PA44, SD2_CLK,  PIN_NOPULL, 1);
HAL_PIN_Set(PAD_PA45, SD2_CMD,  PIN_PULLUP, 1);
HAL_PIN_Set(PAD_PA47, SD2_DIO0, PIN_PULLUP, 1);
HAL_PIN_Set(PAD_PA49, SD2_DIO1, PIN_PULLUP, 1);
HAL_PIN_Set(PAD_PA51, SD2_DIO2, PIN_PULLUP, 1);
HAL_PIN_Set(PAD_PA55, SD2_DIO3, PIN_PULLUP, 1);
```

**理由**：4 线下 CMD/DAT0~3 全部参与传输，必须有上拉（内部约几十 kΩ，24MHz 下余量偏紧；
若出现偶发 CRC/掉线，建议外接 10kΩ 到 3.3V）。CLK 不上拉。

---

## 2. 编译与验证

**编译环境（两条 export 都要跑，否则 Kconfig 会因 SDK 树不对而报 `undefined symbol WIFI_USING_AIC8800_RTOS`）**

```powershell
& G:\Code\VScode\SF32_Code\setup_sifli.ps1                       # 提供 uv/git/python 与工具链路径
& D:\SiFli\SDK\SiFli-SDK-WiFi\SiFli-SDK\export.ps1               # ★ 指向 Wi-Fi 树(关键)
cd D:\SiFli\SDK\SiFli-SDK-WiFi\SiFli-SDK\example\aic8800_d80\project
scons --board=sf32lb55-lcd_n16r4n4 -j8
```

> A/B 对比前建议 `scons --board=sf32lb55-lcd_n16r4n4 -c` 清一次：
> 本工程多次出现"改了但烧的是旧镜像"的误判（见 §3 第 8 条备注）。

**判据（主机日志）**

```
chip_id=3, chip_sub_id=1                     ← 第一条 cfg 成功(修复前必超时)
====> D80N stage1: CINIT (bootrom)
Upload fw id=60 @ 0x00133000, size=17028     ← CINIT
D80N CINIT done (chipinfo=0x3c21, ...)
Upload fw id=58 @ 0x00188000, size=23680     ← PATCH
[PATCH_TBL] wrote 65 entries (130 words)
Upload fw id=61 @ 0x00138000, size=29544     ← CALIB
[AICFC] patch FW up -> flow-ctrl read back to real Q1(0x03)     ← 阶段切换
```

**判据（芯片侧日志）**：出现 `wifi ready / PatchVer: ... / driver start / is 5g=1`；
不应出现 `sdio func1 error bit:800, func1 free cnt=0, func1 free queue2 cnt=2`。

**判据（系统）**：shell 可用（回车有回显，不卡死）；`wifi_test iwpriv uapst` 有回包；
`sta conn` 不再 `reason=15`。

---

## 3. 修改史（含被推翻的中间方案）

> 说明：本工程是"边测边收敛"的，以下是按时间顺序的完整轨迹。**标 ❌ 的都是被推翻的方案**，
> 保留它们是为了让后来者不重复同样的路。

| # | 改动 | 目的 | 结论 |
|---|---|---|---|
| 1 | 例程加 CMD52 寄存器探针（func0 CCCR/FBR、func1 `0x00~0x1F` 全量 dump、`03/01/04` 各 8 次） | 证明 CMD52 通路与 func1 寄存器空间是否正常 | ✅ 结论已固化到报告（`0x03=0`、`0x09=2`）；探针代码**已删除** |
| 2 | 切 1-bit：`CONFIG_SDMMC2_BUS_WIDTH_1_ONLY=y` | 隔离 D1 的规律性下拉 | ⚠️ 有效但非必需；4 线同样可用，终版改回 `=n` |
| 3 | 板级 SD2 六根线改 `PIN_PULLUP`（CLK 除外） | 4 线数据完整性 | ✅ 保留 |
| 4 | 例程自动诊断序列：dump func1 + 写 `0x02=0x11`（唤醒握手）+ 偷读 RD FIFO | 找"为什么 0x03 恒 0" | ❌ 已删除。但**正是它 dump 出 `09=02`**，是本案的转折点；同时证明"唤醒握手"不是关键（写 0x02 前后无变化） |
| 5 | 例程定时线程：`sdio_dev[8] = 0x09`（每 500ms，1.5~15.5s） | 验证"库读错寄存器"的假设 | ❌ 已删除（临时手段）。当时能跑通，但时间余量只有**几毫秒**（第一次写 ≈1.5s，第一条 flow-ctrl 读 ≈1.45s 后），非常脆 |
| 6 | 平台 `aicwf_sdiov3_func_init()` 末尾写 `sdiodev->sdio_reg.flow_ctrl_reg = Q2` | 用"正规位置"取代定时线程 | ❌ **无效**：库始终把该字段保持为 Q1（实测 `field=0x03`），写侧是在和库抢同一个字段 |
| 7 | 平台 `aicwf_sdio_readb_cmd52()` **纯**读侧重定向（`addr = 0x09`） | 与库解耦、无时序依赖 | ❌ **拿不到信用**（启动仍失败）——说明"哑读 Q1"是必要的 |
| 8 | 在 §7 基础上加"前 5 次哑读 Q1/Q2 + 限流打印" | 定位 §7 失败原因 | ✅ **一次成功**；打印/哑读被证明承重，成为终版基础。（备注：本工程多次出现"改了却烧旧镜像"的误判，A/B 前务必 clean 或核对 bin 时间戳） |
| 9 | 精简版："**每次**都哑读 Q1 + 重定向"，去掉打印 | 想要干净最小版 | ❌ 芯片**持续**报 `sdio func1 error bit:800, func1 free cnt=0, func1 free queue2 cnt=2`，需大量重试才成功 → 结论：**反复访问 Q1 会招惹芯片**，只允许最初有限次 |
| 10 | 平台 `plat_port.c`：`strstr` 过滤 `"Interrupt but no data"` | 压制刷屏日志 | ❌ 已回退（该日志是真实信号，且属厂商文件）。后来改为 §1.B 的根因规避（中断风暴），过滤由使用方按需自理 |
| 11 | 中断风暴规避：`g_sdio_irq_mask_forced` + `aicwf_sdio_isr()`/`aicwf_sdio_host_enable_isr()` 压住不放行 + 例程开关 | 解决"shell 卡死"（中断优先级饿死串口） | ✅ 保留为终版 |
| 12 | flow-ctrl 重定向加**阶段切换** `AIC_FC_Q1_AFTER_FWBOOT`（FW 起来后切回真实 Q1） | 修 4-way 握手失败 | ✅ 保留为终版 |

**中间版本的"开关遗迹"（保留在代码里，便于 A/B，不产生副作用）**

| 开关 | 默认 | 作用 |
|---|---|---|
| `AIC_FC_Q1_AFTER_FWBOOT` | `1` | 0 = 永久重定向到 Q2（仅 A/B 对照） |
| `AIC_FC_RAW_DBG` / `AIC_FC_RAW_DBG_PERIOD` | `0` / `2000` | 周期探针打印 raw Q1/Q2（默认关，避免触发 §3 第 9 条现象） |
| `AIC_SDIO_IRQ_MASK_FORCED`（main.c） | `1` | 0 = 原样中断驱动收包（会卡死，用于 A/B） |
| `CONFIG_SDMMC2_BUS_WIDTH_1_ONLY`（proj.conf） | `n` | y = 1-bit（隔离 D1/D2/D3 用） |

---

## 4. 根因小结（为什么必须这么改）

1. **库侧硬编码**：`aicwf_sdio_reg_init()` 对 chipid 3/4/5 把 `sdio_reg.flow_ctrl_reg` 设为
   `SDIOWIFI_FLOW_CTRL_Q1_REG_V3(0x03)`；库内**从未引用** `SDIOWIFI_FLOW_CTRL_Q2_REG_V3(0x09)`。
2. **芯片侧实测**：这颗 0x9082 的 ROM/CINIT/PATCH/CALIB 阶段 **Q1 恒 0、Q2 恒 2**；
   且**必须先哑读一次 Q1，Q2 才有效**；**反复读 Q1 会让芯片报 `error bit:800`**。
3. **失败链**：库读 Q1 → 0 → 判定无 TX 缓冲 → 第一条 cfg（读 `0x40500000`）发不出 →
   `cmd_mgr` 超时 → `system config fail` → `fdrv fail` → 初始化失败（芯片侧日志停在 `fn1_ioen`）。
4. **修复思路**：在库与硬件之间（func1 CMD52 读入口）按**阶段**翻译 flow-ctrl 寄存器；
   ROM 阶段用 Q2、FW 起来后回 Q1，从而 msg 通路与数据通路都拿到信用。
5. 与总线宽度、D1 电平、板级上拉**无关**（1-bit/4-bit 现象一致；CMD52 读通路经独立探针验证正常）。

---

## 5. 已知遗留 & 厂商待确认

- 该规避位于**厂商驱动文件**，同步基线会被覆盖；需向厂商提**官方修法**。
- 待确认清单见 [aic8800d80n_flowctrl_q1q2_vendor_report.md](aic8800d80n_flowctrl_q1q2_vendor_report.md) §7
  （Q1/Q2 语义与访问顺序/频次、FW 起来后正确的队列、`error bit:800` 含义、是否有新库）。
- 中断风暴目前的规避是"闭掉中断走轮询"（10ms），**吞吐/时延不如中断驱动**；
  根因在库 `aicwf_sdio_rx_task` 的中断语义（电平中断 + 无条件 re-enable），建议一并向厂商提出。
- 4 线 + 内部上拉在 24MHz 下可用；若量产出问题，优先补外部 10kΩ 上拉。

---

## 6. 回退方法（逐条）

| 想回到… | 做法 |
|---|---|
| 完全原始（复现 `msg fc:0` 失败） | `sdio_port.c`：把 `aicwf_sdio_readb_cmd52()` 恢复成单行 `return aicwf_sdio_readb_cmd52_by_num(FUNC_1, addr, data);` |
| 只保留 ROM 阶段修复（不切回 Q1） | `AIC_FC_Q1_AFTER_FWBOOT` 置 `0` |
| 回到"临时线程"方案 | `main.c` 重新加 `aic_fc_fix_thread`（见 §3 第 5 条），并把 §1.A 的读侧重定向注释掉 |
| 回到 1-bit | `proj.conf`：`CONFIG_SDMMC2_BUS_WIDTH_1_ONLY=y` |
| 复现"shell 卡死" | `main.c`：`AIC_SDIO_IRQ_MASK_FORCED` 置 `0` |
