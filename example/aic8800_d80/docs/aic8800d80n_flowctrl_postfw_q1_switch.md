# D80N flow-ctrl 阶段切换实验：FW 运行后读回真实 Q1(0x03)

> 目的：补上厂商报告 `aic8800d80n_flowctrl_q1q2_vendor_report.md` §7 **Q2** 的实测答案
> ——"FW 运行后（PATCH/CALIB 完成、`start_from_bootrom` 之后）host 是否应当切回 Q1？"
> 同时验证 16.txt 里 4-way handshake 失败的根因是否就是"数据通道永远读不到 >2 的信用"。

---

## 1. 已知 / 未知

| | 内容 | 证据 |
|---|---|---|
| 已知 | 库把 `sdio_reg.flow_ctrl_reg` 硬编码为 `0x03`(Q1)，数据通路与 msg 通路**共用这一个字段** | 厂商报告 §3(1)(3)；15.txt `field=0x03` |
| 已知 | **ROM/CINIT/PATCH/CALIB 阶段**：Q1(0x03)=0，Q2(0x09)=2，且必须先哑读一次 Q1，Q2 才有效 | 15.txt:83 `raw 0x03=0(ok1) 0x09=2(ok1)`；报告 §3(4) |
| 已知 | 当前实现（报告 §4 变体 D：前 5 次哑读 Q1+Q2，之后只读 Q2）让**初始化 100% 通过** | 15.txt:90-189 引导链全绿 |
| 已知 | 重定向**无条件且永久**生效，没有任何阶段收回逻辑 | `sdio_port.c:784-820` |
| **未知** | **补丁 FW 跑起来之后 Q1(0x03) 是多少** —— 现有代码在那之后再也不读 Q1，全量日志里只有 `redirect 0x09 val=2` | 16.txt 中 `#200…#3200` 全部 `val=2` |

### 为什么这个未知数决定成败

数据通路的判据是 `fc_reg > DATA_FLOW_CTRL_THRESH(=2)`，msg 通路是 `fc_reg != 0`（`aic8800d40/fdrv/layer_common/src/sdio_co.c:644-741`）。
当前所有 flow-ctrl 读都被翻译成 Q2=2：

* msg 通路：`2 != 0` → **通过** → 扫描/认证/关联全部成功；
* 数据通路：`2 > 2` 为假 → **永远失败** → `[AIC][SDIO]data fc:-2` → EAPOL(msg2/4) 发不出芯片
  → AP 每秒重发一次 msg1/4、共 4 次后 `deauth reason=15`(4WAY_HANDSHAKE_TIMEOUT)（16.txt:118-298）。

即：**无论密码对错，当前配置下 4 次握手都不可能完成**。数据通路要么需要 Q1 在 FW 运行后变成 >2，
要么需要另一个真正表示"数据队列可用缓冲数"的寄存器。

---

## 2. 补丁（两处，均在 `customer/peripherals/wifi/aic8800_rtos_driver/platform/hal/sdio_port.c`）

### 2.1 新增开关（放在 `aicwf_sdio_readb_cmd52()` 定义之前，约第 775 行）

```c
/* ---------------------------------------------------------------------------
 * D80N flow-ctrl 读重定向的阶段开关
 *
 *   ROM/CINIT/PATCH/CALIB 阶段: 库要读 Q1(0x03)=0, 必须先哑读一次 Q1 再从 Q2(0x09)
 *   取信用(实测 Q2=2), 否则第一条 cfg 消息就发不出去 —— 见厂商报告 §3/§4。
 *
 *   补丁 FW 启动后, 数据队列(Q1)按理应当开始发放 TX 缓冲信用, 但那之后当前实现
 *   再也没读过 Q1, 因此"FW 运行后 Q1 是多少"是个未验证的前提。
 *
 *   AIC_FC_Q1_AFTER_FWBOOT = 1: FW 起来后停止重定向, 直接读真实 Q1(0x03)
 *                              → 若 Q1 > 2, 数据通路立刻可用(EAPOL 能发出)
 *                              → 若 Q1 == 0, msg 通路会退化成 msg fc:0(现象同修复前)
 *   AIC_FC_Q1_AFTER_FWBOOT = 0: 保持现状(永远读 Q2), 仅用于 A/B 对照
 * ------------------------------------------------------------------------- */
#define AIC_FC_Q1_AFTER_FWBOOT   1

/* 每 AIC_FC_RAW_DBG_PERIOD 次 flow-ctrl 读, 附带打印一次 raw Q1/Q2。
 * 注意: 报告 §3(4) 记录"反复访问 Q1 会激怒芯片"(error bit:800 / free cnt=0),
 * 默认关闭; 需要看 Q2 是否也变化时再开, 并把周期放大(如 2000)。 */
#define AIC_FC_RAW_DBG          0
#define AIC_FC_RAW_DBG_PERIOD   2000

/* 1 = ROM/CINIT 阶段的读重定向生效(默认); FW 启动后被 aicwf_sdio_enable_irq_after_fw_boot() 清零 */
static volatile int g_fc_redirect_q2 = 1;
```

### 2.2 改写 `aicwf_sdio_readb_cmd52()`（约 782-823 行）

替换整个函数体：

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
         *        并需要重试很多次才成功（反复访问 Q1 会招惹芯片） */
        static uint32_t fc_dbg_cnt = 0;
        uint8_t v = 0;
        bool ok;

        fc_dbg_cnt++;

#if AIC_FC_RAW_DBG
        if ((fc_dbg_cnt <= 5) || ((fc_dbg_cnt % AIC_FC_RAW_DBG_PERIOD) == 0))
        {
            uint8_t v03 = 0, v09 = 0;
            bool ok03 = aicwf_sdio_readb_cmd52_by_num(FUNC_1, SDIOWIFI_FLOW_CTRL_Q1_REG_V3, &v03);
            bool ok09 = aicwf_sdio_readb_cmd52_by_num(FUNC_1, SDIOWIFI_FLOW_CTRL_Q2_REG_V3, &v09);
            rt_kprintf("[AICFC] #%u raw 0x03=%u(ok%d) 0x09=%u(ok%d) chipid=%u field=0x%02x redir=%d\n",
                       fc_dbg_cnt, v03, ok03, v09, ok09,
                       g_runtime_chipid, sdio_dev.sdio_reg.flow_ctrl_reg, g_fc_redirect_q2);
        }
#else
        if (fc_dbg_cnt <= 5)
        {
            uint8_t v03 = 0, v09 = 0;
            bool ok03 = aicwf_sdio_readb_cmd52_by_num(FUNC_1, SDIOWIFI_FLOW_CTRL_Q1_REG_V3, &v03);
            bool ok09 = aicwf_sdio_readb_cmd52_by_num(FUNC_1, SDIOWIFI_FLOW_CTRL_Q2_REG_V3, &v09);
            rt_kprintf("[AICFC] #%u raw 0x03=%u(ok%d) 0x09=%u(ok%d) chipid=%u field=0x%02x\n",
                       fc_dbg_cnt, v03, ok03, v09, ok09,
                       g_runtime_chipid, sdio_dev.sdio_reg.flow_ctrl_reg);
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

        if (fc_dbg_cnt <= 5)
            rt_kprintf("[AICFC] #%u NOT redirect: chipid=%u redir=%d\n",
                       fc_dbg_cnt, g_runtime_chipid, g_fc_redirect_q2);
    }

    return aicwf_sdio_readb_cmd52_by_num(FUNC_1, addr, data);
}
```

> 关键点：ROM 阶段的"前 5 次哑读 Q1"逻辑**原样保留**，初始化路径不受影响；
> 唯一的行为变化是——`g_fc_redirect_q2` 清零后，flow-ctrl 读直接落到真实 Q1。

### 2.3 在 FW 启动成功处收回重定向（`aicwf_sdio_enable_irq_after_fw_boot()`，约 1446-1461 行）

```c
int aicwf_sdio_enable_irq_after_fw_boot(void)
{
    int ret;
    struct aic_sdio_dev *sdiodev = &sdio_dev;

    DBG_SDIO_INF("aicwf_sdio_enable_irq_after_fw_boot: retry write intr_config_reg=0x07\n");

    ret = aicwf_sdio_writeb_cmd52(sdiodev->sdio_reg.intr_config_reg, 0x07);
    if (!ret) {
        DBG_SDIO_ERR("%s: post-fw-boot write intr_config_reg still failed!\n", __func__);
        return -1;
    }

#if AIC_FC_Q1_AFTER_FWBOOT
    /* 补丁 FW(PATCH/CALIB) 已启动, 数据队列应已开始发放信用: 停止 Q1->Q2 重定向 */
    if (g_fc_redirect_q2) {
        g_fc_redirect_q2 = 0;
        rt_kprintf("[AICFC] patch FW up -> flow-ctrl read back to real Q1(0x03)\n");
    }
#endif

    DBG_SDIO_INF("aicwf_sdio_enable_irq_after_fw_boot: intr_config_reg=0x07 OK (WiFi IRQ enabled)\n");
    return 0;
}
```

依据：15.txt:186-189 显示该函数正是在 `start_from_bootrom success.` 之后被库调用一次，
是全流程中"FW 已就绪"的第一个明确锚点。

**替代挂点**（若不想依赖库的调用时机）：`src/main.c` 里 `aic_wifi_demo_test()` 是阻塞的，
返回时 `GetWiFiInitStat()==1`（15.txt:272），此时 FW 必已就绪。可把开关做成一个小接口：

```c
void aic_fc_q2_redirect_set(int on);   /* sdio_port.c 导出 */
...
aic_wifi_demo_test();
aic_fc_q2_redirect_set(0);
```

### 2.4 编译前确认

```c
#include "sdio_def.h"   /* 若 SDIOWIFI_FLOW_CTRL_Q1_REG_V3 / PRODUCT_ID_AIC8800D80N 未在此文件可见 */
```
（该文件已经用到这两个宏，通常无需新增 include。）

---

## 3. 结果判读

看完 `wifi_test iwpriv sta conn jia-ni <pwd> 30000` 一次即可定性：

| 现象 | 结论 | 下一步 |
|---|---|---|
| `data fc:` 消失，出现 `rx eapol` → `tx_req` → **msg3/4** → `connect:1,0` → DHCP/DONE | **Q1 在 FW 运行后有效，重定向必须按阶段收回** —— 16.txt 的握手失败根因确认 | 把 `AIC_FC_Q1_AFTER_FWBOOT` 固化，并更新厂商报告 §7 Q2/Q5；同时确认 msg 通路读 Q1 也无副作用 |
| `data fc:0` / `data fc:-1` / `data fc:-2` 仍出现 | Q1 在 FW 运行后仍 ≤2，不是时序问题 | 把 `data fc:` 的具体值记下来（`-fc_reg`，即 0/1/2），连同 §3(4) 的表一起追加到厂商报告；需要厂商给出数据队列信用的真实寄存器 |
| 变成 `msg fc:0` / `tx msg fc retry fail` / `cmd_mgr ... time out`（即报告 §2 的原始故障签名） | FW 运行后 Q1 仍为 0，**msg 通路也必须靠 Q2** | 说明"数据用 Q1、cfg 用 Q2"是真的两套队列，但库只暴露一个字段 → 只能请厂商改库（报告 §6/§7 的立场成立）。此时回滚：`AIC_FC_Q1_AFTER_FWBOOT` 置 0 |
| `msg fc:0` 且芯片侧开始刷 `sdio func1 error bit:800, func1 free cnt=0` | 反复访问 Q1 触发了报告的 §3(4) 现象 | 同上回滚；并且**不要**开 `AIC_FC_RAW_DBG` |

判读要点：`data fc:N` 里的 N 就是 `-Q1`（`sdio_co.c:680` 的 `ret = -fc_reg`），
所以即使失败也能量化"FW 运行后 Q1 到底是多少"，这正是报告 §7 Q2 缺的那个数。

---

## 4. 风险与回滚

* 补丁只改**读侧重定向的生效时机**，不写芯片、不改库字段；初始化路径的前 5 次哑读完全保留。
* 最坏结果 = FW 起来后 msg 通路退化（现象与修复前一致），**不会砖**；把宏置 0 重烧即恢复。
* `AIC_FC_RAW_DBG` 默认 0：避免额外 Q1 读触发 `error bit:800`。
* 实验期间保持 `AIC_SDIO_IRQ_MASK_FORCED=1`、总线 4-bit、SDIO 24MHz **不变**，一次只动一个变量。
* 建议同时对 `sifli-employee`(-76dBm，同场景) 做同样一次连接，排除 jia-ni(-81dBm) 的信号因素。

---

## 5. 与厂商报告的对应

| 报告条目 | 本实验的贡献 |
|---|---|
| §7 Q2（FW 运行后是否切回 Q1） | `AIC_FC_Q1_AFTER_FWBOOT=1` 的实测结果直接回答 |
| §7 Q1（ROM 阶段 Q1 的语义） | 不变；但可补充"FW 运行后 Q1=?"，界定时序依赖 |
| §7 Q5（是否有已修正的库） | 若 Q1 有效，则说明库只需按"型号+阶段"选队列寄存器，改法明确 |
| §2 原始故障 vs 16.txt 新故障 | 说明临时规避（变体 D）解决了**初始化**，但把**数据通路**永久钉在 Q2=2 上 |

---

## 6. 实测结果（17.txt，2026-10-12 15:47~15:48）

**判定表第 1 行成立：`AIC_FC_Q1_AFTER_FWBOOT=1` 生效 → 握手完成 → 连接成功 → DHCP 已发出。**

| 关键日志 | 说明 |
|---|---|
| `[AICFC] patch FW up -> flow-ctrl read back to real Q1(0x03)`（15:45:39.223） | 阶段钩子命中，重定向已收回 |
| **全篇无 `data fc:` / `msg fc:`** | 对比 16.txt 里每约 180ms 一次的 `data fc:-2` |
| `fhost_tx_req eapol, len 135` → `[FW] txl :8e88, len=159` → `cfms: 127834, sn=0, st=9` | EAPOL msg2/4 **真的发出去了**（`txl`/`cfms` 是 FW 侧 TX 确认，16.txt 中从未出现） |
| `rx eapol:8e88,189` → `add ptk:0,2` / `add gtk:1,2` / `suite-add:2` | msg3/4 到达，PTK + GTK 安装完成 |
| `WPA network 0: connected` / `AIC-STA: conn` / `wlan_start_sta: connected freq=2437 band=0` | 连接成功 |
| `tx_sched DHCP dn ...` + `[FW] tx dhcp` | DHCP 已发出（IP 层通了） |
| `handle addba_req:tid=0` | 数据通路聚合协商成功 |

结论：
- **FW 运行后 Q1 > DATA_FLOW_CTRL_THRESH(=2)** —— 重定向必须按阶段收回，这就是 16.txt 握手失败的根因；
- **`240200408` 本来就是正确密码**（16.txt 的 4 次握手失败与密码无关）；
- 厂商报告 §7 **Q2 的答案：是，必须在 `start_from_bootrom` 之后切回 Q1(0x03)**。

### 6.1 随之暴露的第二个独立问题（已修）

DHCP ACK 到达时崩在 `netdev_set_dns_server()` 的 `RT_ASSERT(netdev)`（netdev 为 NULL），
tcpip 线程 fatal error。根因：本 SDK 的 lwIP 把 **DNS 完全托管给 RT-Thread netdev**
（`lwip/.../ipv4/dhcp.c:742`、`core/dns.c:387/399`、解析时取 `netdev_default->dns_servers[]`），
而路线1 的驱动自建 lwIP netif、从未注册 netdev（SDK 的 `ethernetif.c::netdev_add()` 是 static，
只服务走 `ethernetif_init` 的标准网卡）。

修法（`platform/netif/netif_port.c`）：新增 `aicwf_net_if_netdev_add()/_del()`，
在 `aicwf_net_if_add()` 成功后注册、在 `aicwf_net_if_release_all()` 中摘除，
复用 SDK 现成的 `lwip_netdev_ops`（`lwip/port/ethernetif.c:327`，其 `set_dns_server` 落到
`netdev_low_level_set_dns_server()`，不会回调 `dns_setserver()`，无递归）。
注意：**不要**改 lwIP 的 `dhcp.c/dns.c` 走 `dns_setserver()` 回退 —— 本 SDK 的
`dns_setserver()` 自己也是遍历 netif 查 netdev，同样会断言。
