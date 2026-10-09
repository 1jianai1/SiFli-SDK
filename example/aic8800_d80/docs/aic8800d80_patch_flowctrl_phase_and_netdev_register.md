# AIC8800D80(N) 两处移植补丁说明 —— SDIO flow-ctrl 阶段开关 + RT-Thread netdev 注册

> 适用工程：`example/aic8800_d80`（路线1：驱动自建 lwIP netif，不用 SDK `rt_wlan`/`eth_device`）
> 适用芯片：AIC8800D80N（卡级 `VID=0xC8A1 PID=0x9082`，运行时 `chipid=4`）
> host 库：`libaic8800_driver.a`，release `NV1.1.6 STA AP P2P ggd4509b7 20260915`
> 固件套件：`chip_sub_id=1`（D80N U02 的 CINIT / PATCH / PATCH_TBL / CALIB）

---

## 0. 摘要

| # | 文件 | 位置 | 解决什么 | 开关 |
|---|---|---|---|---|
| 1 | `customer/peripherals/wifi/aic8800_rtos_driver/platform/hal/sdio_port.c` | 新增宏块（约 776 行）、`aicwf_sdio_readb_cmd52()`（约 815 行）、`aicwf_sdio_enable_irq_after_fw_boot()`（约 1508 行） | 4 次握手（EAPOL）永远完不成、连接超时；根因是 D80N 的 Q1→Q2 读重定向**永久生效**，把数据通路信用永久钉在 Q2=2 | `AIC_FC_Q1_AFTER_FWBOOT`（默认 1）、`AIC_FC_RAW_DBG`（默认 0） |
| 2 | `customer/peripherals/wifi/aic8800_rtos_driver/platform/netif/netif_port.c` | 头部（约 27 行）、新增两个辅助函数（约 764 行）、`aicwf_net_if_add()`（约 899 行）、`aicwf_net_if_release_all()`（约 1004 行） | DHCP ACK 到达时 `RT_ASSERT(netdev)` 断言、tcpip 线程 fatal error；根因是自建 lwIP netif 没有配套的 RT-Thread netdev，而本 SDK 的 lwIP 把 DNS 完全托管给 netdev | 无（靠 `RT_USING_NETDEV` 编译开关） |

**两处改动都只落在 platform 移植层**：不动 AIC 预编译库（`libaic8800_driver.a`）、不动共享的 lwIP / RT-Thread 代码、不动 `main.c`。因此重新同步 AIC 基线或 RT-Thread 时，只需重新套用这两处。

---

## 1. 故障链（两份实测日志）

两个症状是**串行**出现的：补丁 1 修完，才会走到补丁 2 的位置。

| 阶段 | 现象 | 关键证据 |
|---|---|---|
| 打补丁 1 之前 | 扫描/认证/关联全成功，**4 次握手失败**；AP 每秒重发一次 msg1/4、共 4 次后 `deauth reason=15`(4WAY_HANDSHAKE_TIMEOUT) | 连接全过程约每 180ms 一条 `[AIC][SDIO]data fc:-2`；`[AICFC] #N -> redirect 0x09 val=2` |
| 打补丁 1 之后 | 握手完成、连接成功、DHCP 已发出，但 **DHCP ACK 到达瞬间 tcpip 线程崩溃** | `Assertion failed at function:netdev_set_dns_server, line number:727 ,(netdev)` → `fatal error on thread: tcpip` |

补丁 1 生效后的"成功"证据（便于他人对照）：`fhost_tx_req eapol, len 135` 之后出现 FW 侧 TX 确认
`[FW] txl :8e88, len=159` + `[FW] cfms: …, st=9`，随后 `add ptk` / `add gtk` / `WPA network 0: connected` /
`AIC-STA: conn` / `wlan_start_sta: connected freq=2437 band=0` / `tx_sched DHCP dn …`。

---

## 2. 补丁一：D80N flow-ctrl 读重定向只覆盖 ROM/CINIT 阶段

### 2.1 症状与定位

- 现象：`iwpriv sta conn` 扫描/认证/关联都 OK，4 次握手失败，AP 以 reason 15 断开。
- 关键日志：`[AIC][SDIO]data fc:-2` 全程持续。
- 定位（对照 `customer/peripherals/wifi/aic8800d40` 的 vendor 源码）：

| 判据 | 代码 | Q2=2 时 |
|---|---|---|
| 数据通路 | `aicwf_sdio_flow_ctrl()`：`if (fc_reg > DATA_FLOW_CTRL_THRESH)`，`THRESH = 2` | `2 > 2` 为假 → **永远无信用** |
| msg/cfg 通路 | `aicwf_sdio_flow_ctrl_msg()`：`if (fc_reg != 0)` | `2 != 0` 成立 → **通** |

所以只有"管理帧能走、数据帧走不了"这一种表现；而 EAPOL（msg2/4）是**数据帧**，发不出芯片，
AP 自然收不到，握手必然超时。`data fc:%d` 打印的是 `-fc_reg`（`sdio_co.c:680`），
故 `-2` 就等于"读到的信用是 2"。

### 2.2 原理

- 库把 `sdio_reg.flow_ctrl_reg` 硬编码为 **Q1(0x03)**，且数据通路与 msg 通路**共用这一个字段**。
- 本芯片在 **ROM/CINIT/PATCH/CALIB 阶段**只有 **Q2(0x09)** 有信用（实测 Q1=0、Q2=2，
  且必须先哑读一次 Q1、Q2 才有效），所以 platform 层在 func1 CMD52 读入口做了 `0x03 -> 0x09` 的重定向
  —— 这个规避让**初始化**100% 通过（详见 `aic8800d80n_flowctrl_q1q2_vendor_report.md`）。
- 但它**无条件且永久生效**：补丁 FW 跑起来之后，数据队列本应开始发放信用，
  而 host 仍在读 Q2=2，永远过不了 `> 2`。
- 实测结论：FW 就绪后 Q1 是有效的（切回 Q1 后握手立刻走通），因此正确语义是
  **ROM 阶段读 Q2、FW 运行后读回 Q1**。

### 2.3 改动清单（3 处）

#### (1) 新增开关 —— 放在 `aicwf_sdio_readb_cmd52()` 定义之前（原文约 776 行）

```c
/* ---------------------------------------------------------------------------
 * D80N(0x9082) flow-ctrl 读重定向的阶段开关   [AIC8800-PORT][SiFli]
 *
 * 背景(证据见 example/aic8800_d80/docs/aic8800d80n_flowctrl_q1q2_vendor_report.md):
 *   库把 sdio_reg.flow_ctrl_reg 硬编码为 Q1(0x03), 且数据通路与 msg(cfg) 通路
 *   共用这一个字段; 而本芯片 ROM/CINIT/PATCH/CALIB 阶段只有 Q2(0x09) 有 TX 信用
 *   (Q1 恒 0, Q2 恒 2; 且必须先哑读一次 Q1, Q2 才有效), 故在 func1 CMD52 读入口做
 *   0x03 -> 0x09 的读侧重定向。该规避让初始化 100% 通过。
 *
 * 遗留问题: 重定向此前无条件且永久生效, 于是补丁 FW 跑起来之后:
 *   · msg  通路判据 fc_reg != 0        -> Q2=2 通过   -> 扫描/认证/关联 OK
 *   · 数据 通路判据 fc_reg >  THRESH(2) -> Q2=2 不通过 -> data fc:-2,
 *     EAPOL(msg2/4) 永远发不出芯片 -> AP 每秒重发 msg1/4, 4 次后 deauth
 *     reason=15(4WAY_HANDSHAKE_TIMEOUT) —— 即握手失败、连接超时。
 *
 * 本开关把重定向限定在真正需要的阶段, 以便实测"FW 运行后 Q1 是多少":
 *   AIC_FC_Q1_AFTER_FWBOOT = 1: FW 就绪后停止重定向, 直读真实 Q1(0x03)
 *                               Q1 > 2  -> 数据通路立刻可用(握手可完成)
 *                               Q1 == 0 -> msg 通路退化为 msg fc:0(同修复前现象)
 *   AIC_FC_Q1_AFTER_FWBOOT = 0: 保持原行为(永远读 Q2), 仅作 A/B 对照
 *
 *   AIC_FC_RAW_DBG = 1: 每 AIC_FC_RAW_DBG_PERIOD 次 flow-ctrl 读, 附带打印一次
 *                       raw Q1/Q2(默认关闭: 报告 §3(4) 记录反复访问 Q1 会触发票
 *                       芯片侧 "sdio func1 error bit:800, func1 free cnt=0, ...")
 * ------------------------------------------------------------------------- */
#define AIC_FC_Q1_AFTER_FWBOOT   1
#define AIC_FC_RAW_DBG           0
#define AIC_FC_RAW_DBG_PERIOD    2000

/* 1 = ROM/CINIT/PATCH/CALIB 阶段: 读 Q1 即重定向到 Q2;
 * 由 aicwf_sdio_enable_irq_after_fw_boot() 在补丁 FW 启动成功后清零 */
static volatile int g_fc_redirect_q2 = 1;
```

#### (2) 改写 `aicwf_sdio_readb_cmd52()` 函数体

**只改两处语义**：重定向判据加 `g_fc_redirect_q2`；`NOT redirect` 那条打印只在真·非 D80N 时出现。
**ROM 阶段的"前 5 次哑读 Q1/Q2"原样保留**（它是 Q2 能给出有效信用的必要条件，改动它会让初始化失败）。

```c
    if (addr == SDIOWIFI_FLOW_CTRL_Q1_REG_V3)
    {
        /* ... 原注释保留, 补了一行说明 ... */
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
```

对应"改动前"的两行（便于 diff）：

```c
        if (g_runtime_chipid == PRODUCT_ID_AIC8800D80N)          /* 改前: 无条件重定向 */
        ...
        if (fc_dbg_cnt <= 5)                                     /* 改前: D80N 也会走到这条 */
            rt_kprintf("[AICFC] #%u NOT redirect: chipid=%u (expect %u)\n", ...);
```

#### (3) 在补丁 FW 启动成功处收回重定向 —— `aicwf_sdio_enable_irq_after_fw_boot()`

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

**为什么挂在这里**：库在 `[AIC][MACIF]start_from_bootrom success.` 之后立刻调用本函数
（日志顺序：`start_from_bootrom success.` → `aicwf_sdio_enable_irq_after_fw_boot: retry write …` → 本行），
是全流程中"FW 已就绪"的第一个明确锚点，且**由驱动自动触发，不依赖应用层调用顺序**。

**替代挂点**（若基线里这个函数被改名/移动）：`src/main.c` 中 `aic_wifi_demo_test()` 是阻塞的，
返回时 `GetWiFiInitStat()==1`、FW 必已就绪，可把开关做成 `void aic_fc_q2_redirect_set(int on);`
在 `aic_wifi_demo_test();` 之后调用。

### 2.4 开关与回滚

| 宏 | 值 | 行为 |
|---|---|---|
| `AIC_FC_Q1_AFTER_FWBOOT` | `1`（默认） | FW 就绪后读真实 Q1 → 数据通路可用 |
| | `0` | 保持旧行为（永远读 Q2），用于 A/B 复现 `data fc:-2` + 握手失败 |
| `AIC_FC_RAW_DBG` | `0`（默认） | 不额外读 Q1（避免触发芯片侧 `error bit:800 / free cnt=0` 日志） |
| | `1` | 每 `AIC_FC_RAW_DBG_PERIOD` 次 flow-ctrl 读附带打印一次 raw Q1/Q2 |

回滚 = 把 `AIC_FC_Q1_AFTER_FWBOOT` 置 0（行为与打补丁前完全一致，不会变砖）。

---

## 3. 补丁二：为自建 lwIP netif 补注册 RT-Thread netdev

### 3.1 症状与定位

补丁 1 生效、连接成功后，DHCP 已发出（`tx_sched DHCP dn …`、`[FW] tx dhcp`），
约 90ms 后：

```
Assertion failed at function:netdev_set_dns_server, line number:727 ,(netdev)
...
fatal error on thread: tcpip
```

`netdev.c:727` 是 `netdev_set_dns_server()` 的 `RT_ASSERT(netdev)` —— 传入的 `netdev` 是 NULL。
（能崩在这里也**反证 DHCP ACK 已收到**：该调用只出现在 lwIP 的 `dhcp_handle_ack()` 里。）

### 3.2 原理

本 SDK 的 lwIP 被 RT-Thread 改造过，**把 netdev 当作 DNS 的唯一载体**：

| 调用点 | 代码 | 无 netdev 时 |
|---|---|---|
| `lwip-2.1.2/src/core/ipv4/dhcp.c:742`（`dhcp_handle_ack`） | `netdev_set_dns_server(netdev_get_by_name(netif->name), n, &dns_addr)` | NULL → `RT_ASSERT` 崩 |
| `lwip-2.1.2/src/core/dns.c:387 / 399`（`dns_setserver`） | 遍历 `netif_list` 逐个 `netdev_set_dns_server(...)` | 同上 |
| `lwip-2.1.2/src/core/dns.c:422 / 855 / 1176 …`（解析时） | 取 `netdev_default->dns_servers[]` | 没有任何落点，域名解析不可用 |

而**路线1** 的驱动自己 `netifapi_netif_add()` + 自己的 init 回调建 netif，
SDK 的 `ethernetif.c::netdev_add()` 是 `static`、只服务走 `ethernetif_init` 的标准网卡，
**不会**为它建 netdev → 缺口。

> 注意 `netdev_get_by_name(netif->name)` 的查法：`netif->name` 是 `char[NETIF_NAMESIZE]`（该 SDK 为 6），
> 本驱动只写了 `name[0]='w'; name[1]='l'`，所以查表串就是 `"wl"`，按 2 字节前缀匹配。
> 因此 netdev 名取成 `netif->name`（`"wl"`）即可，与 `ethernetif.c::netdev_add()` 的做法一致。

### 3.3 改动清单（4 处，均在 `platform/netif/netif_port.c`）

#### (1) 头部加 netdev 头与 ops 声明（原文锚点：`#include "lwip/dhcp.h"`）

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

#### (2) 新增状态与两个辅助函数（原文锚点：`static err_t aicwf_net_if_init(struct netif *net_if);` 之后）

```c
static err_t aicwf_net_if_init(struct netif *net_if);

#ifdef RT_USING_NETDEV
/* 每个 netif 槽位对应一个 RT-Thread netdev (静态复用, 不依赖堆) */
static struct netdev s_netdev_pool[PLAT_NETIF_POOL_SIZE];
static uint8_t       s_netdev_used[PLAT_NETIF_POOL_SIZE];

static void aicwf_net_if_netdev_del(struct netif *nif);

/**
 * @brief 为已加入 lwIP 的 netif 补注册 RT-Thread netdev
 *
 * 路线1 的驱动自己用 netifapi_netif_add + 自己的 init 回调建 netif, 因此 SDK
 * ethernetif.c::netdev_add() 不会为它建 netdev。而本 SDK 的 lwIP 把 netdev 当作
 * DNS 的唯一载体:
 *   - lwip/.../ipv4/dhcp.c::dhcp_handle_ack():
 *         netdev_set_dns_server(netdev_get_by_name(netif->name), ...)
 *   - lwip/.../core/dns.c::dns_setserver():
 *         遍历 netif_list 逐个 netdev_set_dns_server()
 *   - lwip/.../core/dns.c 解析时: 取 netdev_default->dns_servers[]
 * 三者只要查不到 netdev, 就会在 netdev_set_dns_server() 的 RT_ASSERT(netdev) 处
 * 直接断言 (实测: DHCP ACK 到达瞬间 tcpip 线程 fatal error)。
 * 故按 ethernetif.c 相同语义注册 netdev: 名称取 netif->name(2 字节前缀, 与
 * netdev_get_by_name(netif->name) 的查法一致), user_data 指向本 netif。
 *
 * @return 0=成功, -1=失败
 */
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

    if (netdev_register(nd, name, nif) != 0) {
        memset(nd, 0, sizeof(struct netdev));
        DBG_NETAL_ERR("%s: netdev_register(%s) fail\r\n", __func__, name);
        return -1;
    }

    nd->ops        = &lwip_netdev_ops;
    nd->hwaddr_len = nif->hwaddr_len;
    memcpy(nd->hwaddr, nif->hwaddr, nif->hwaddr_len);
    nd->ip_addr    = nif->ip_addr;
    nd->gw         = nif->gw;
    nd->netmask    = nif->netmask;
    s_netdev_used[idx] = 1;

    DBG_NETAL_INF("%s: netdev '%s' registered (netif %p)\r\n", __func__, name, nif);
    return 0;
}

/**
 * @brief 摘除 netif 对应的 RT-Thread netdev
 *
 * netdev_unregister() 内部会 rt_memset 掉该结构, 故静态槽位可安全复用。
 */
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

#### (3) `aicwf_net_if_add()` 里，加入 lwIP 成功后注册（锚点：`DBG_NETAL_INF("%s status=%d\r\n", __func__, status);`）

```c
    DBG_NETAL_INF("%s status=%d\r\n", __func__, status);

    if (status != ERR_OK)
        return -1;

#ifdef RT_USING_NETDEV
    /* [AIC8800-PORT][SiFli] 补注册 RT-Thread netdev: 本 SDK 的 lwIP 把 DNS 存在
     * netdev 上, 且 dhcp.c/dns.c 会以 netif->name 反查 netdev, 缺失即断言 */
    aicwf_net_if_netdev_add(net_if);
#endif

    return 0;
}
```

#### (4) `aicwf_net_if_release_all()` 里，摘 netif 前先摘 netdev（锚点：`DBG_NETAL_INF("%s: remove netif %c%c%d\n", …)`）

```c
        DBG_NETAL_INF("%s: remove netif %c%c%d\n", __func__,
                      nif->name[0], nif->name[1], nif->num);
#ifdef RT_USING_NETDEV
        /* 先摘 netdev: 否则驱动重初始化时同一槽位会带旧登记二次入链 */
        aicwf_net_if_netdev_del(nif);
#endif
        netif_set_down(nif);
        netif_remove(nif);
```

> 第 (4) 处不能省：`netdev_register()` 会 `rt_slist_init()` 该节点再尾插，
> 若同一节点已在链上而没摘除，会造成**自环（尾节点 next 指向自己）**，
> 后续任何遍历 `netdev_list` 的代码都会死循环。驱动支持反初始化后重初始化
> （`aicwf_net_if_release_all()` 就是为此存在的），所以必须配对。

### 3.4 为什么不用其它改法

| 备选 | 结论 |
|---|---|
| 改 lwIP `dhcp.c`，查不到 netdev 就回退 `dns_setserver()` | **不可行**：本 SDK 的 `dns_setserver()`（`dns.c:374-406`）自己也是"遍历 netif → 查 netdev → `netdev_set_dns_server`"，会二次断言。仅加 NULL 判断跳过则 DHCP 下发的 DNS 无处存放，域名解析失效 |
| `proj.conf` 关掉 `CONFIG_RT_USING_NETDEV` | 可行但功能倒退：`dhcp.c/dns.c` 会退回 lwIP 原生 `dns_servers[]` 数组（能跑通），但同时失去 `ifconfig` / `netstat` / netdev 版 `ping`（`NETDEV_USING_IFCONFIG/PING/NETSTAT` 本来就开着）。且属于配置变更，需重新 kconfig + 全量编译，不易与 SDK 其他组件解耦 |
| 自己写一份 `netdev_ops` | 不必：SDK 已有 `lwip_netdev_ops`（`lwip/port/ethernetif.c:327`），各回调都以 `netdev->user_data` 当 `struct netif *`，正好与本驱动"`user_data = netif`"契合；其 `set_dns_server` 落到 `netdev_low_level_set_dns_server()`，不回调 `dns_setserver()`，**无递归** |
| 把驱动 netif 改成走 SDK 的 `ethernetif_init` | 改动面过大：本驱动的 netif 有自己的 `output`（`aicwf_net_if_init`）与 TX/RX 通路，换成 SDK 的以太网框架等于重写数据通路 |
| 动态 `rt_calloc` 一个 netdev | 可行，但静态槽位（`PLAT_NETIF_POOL_SIZE`，与 netif 池一一对应）更简单：`netdev_unregister()` 内部会 `rt_memset` 掉结构，槽位可安全复用，且不引入堆失败路径 |

### 3.5 验证方法

编译（用工程已有的 `compile_commands.json`，只做语法检查、不产出 .o）：

```powershell
$b = "example\<proj>\project\build_<board>"          # 例: build_sf32lb55-lcd_n16r4n4_hcpu
$j = Get-Content "$b\compile_commands.json" -Raw | ConvertFrom-Json
foreach ($f in 'sdio_port\.c$','netif_port\.c$') {
    $cmd = ($j | Where-Object { $_.file -match $f -and $_.file -notmatch 'aic8800d40' })[0].command
    cmd /c (($cmd -replace '\s-o\s+\S+', ' -fsyntax-only') + ' 2>&1')
    "--- $f exit=$LASTEXITCODE ---"
}
```
（注意：要在 `project` 目录下执行，`compile_commands.json` 里的 `-Ibuild_xxx`、`-I.` 是相对该目录的。）

运行时应看到（按顺序）：

```
[FW] fwlog redir enable=1
[AIC][MACIF]D80N: WiFi PATCH upload ...
[AIC][MACIF]start_from_bootrom success.
[AIC][SDIO]aicwf_sdio_enable_irq_after_fw_boot: retry write intr_config_reg=0x07
[AICFC] patch FW up -> flow-ctrl read back to real Q1(0x03)      ← 补丁1 生效标志
[AIC][SDIO]aicwf_sdio_enable_irq_after_fw_boot: intr_config_reg=0x07 OK (WiFi IRQ enabled)
```

连接时（`wifi_test iwpriv sta conn <ssid> <pwd> 30000`）：

| 期望 | 说明 |
|---|---|
| **全程没有** `data fc:` / `msg fc:` | 补丁1 的核心判据 |
| `fhost_tx_req eapol, len 135` → `[FW] txl :8e88, len=159` → `[FW] cfms: …, st=9` | EAPOL 真的出芯片 |
| `add ptk` / `add gtk` / `WPA network 0: connected` / `AIC-STA: conn` | 4 次握手完成 |
| `app_wlan_sta_start: dhcp finish` + IP 打印，**无** `Assertion failed at function:netdev_set_dns_server` | 补丁2 的核心判据 |
| `ifconfig` 能看到 `wl` 接口 | netdev 已注册 |

---

## 4. 移植到其它工程 / 版本时的注意事项

1. **补丁1 是芯片相关**：只在 `g_runtime_chipid == PRODUCT_ID_AIC8800D80N`（0x9082 / 0x9081 / 0x9084 / 0x9085 / 0x9083 / 0x9086）时才有意义。其它型号不会被这段代码影响（判据里带了 chipid）。
2. **补丁2 只在 `RT_USING_NETDEV=y` 时需要**（本工程 `proj.conf: CONFIG_RT_USING_NETDEV=y`）。若某工程关了 netdev，`dhcp.c/dns.c` 里的 netdev 分支不会编译，补丁2 可以不要。
3. **如果 AIC 后续提供修正过的 host 库**（按"型号 + 阶段"自动选择队列寄存器），补丁1 可以整段删除 —— 那才是厂商报告里要求的正解；补丁2 在"驱动自建 netif"的架构下仍然需要。
4. 行号会随基线变动，请按**锚点文本**套用：
   - 补丁1：`bool aicwf_sdio_readb_cmd52(uint32_t addr, uint8_t *data)`、`int aicwf_sdio_enable_irq_after_fw_boot(void)`、`aicwf_sdio_writeb_cmd52(sdiodev->sdio_reg.intr_config_reg, 0x07)`
   - 补丁2：`#include "lwip/dhcp.h"`、`static err_t aicwf_net_if_init(struct netif *net_if);`、`DBG_NETAL_INF("%s status=%d`、`DBG_NETAL_INF("%s: remove netif %c%c%d`
5. 两处都用 `#ifdef RT_USING_NETDEV` / `#if AIC_FC_*` 包裹，**删掉即回到原状**，便于与上游对比。

## 5. 已知遗留（不影响功能）

- `netdev_flags_sync()` 在 `ethernetif.c` 里是 `static`，本补丁无法复用 → netdev 的
  IP / netmask / gw / flags 镜像是**注册时刻**的快照（`0.0.0.0` / DOWN）。
  仅影响 `ifconfig` 的显示，不影响收发（lwIP 实际用的是 netif）。要美化可在 DHCP 完成回调里补
  `netdev_low_level_set_ipaddr()/_set_gw()/_set_netmask()/_set_link_status()`。
- 连接过程中会做两轮全信道扫描（含 5G），耗时约 6.6s，属流程开销，非故障。

## 6. 相关文档

| 文档 | 内容 |
|---|---|
| `aic8800d80n_flowctrl_q1q2_vendor_report.md` | Q1/Q2 现象的完整定位过程 + 给 AIC 的问题清单（补丁1 的前置） |
| `aic8800d80n_flowctrl_postfw_q1_switch.md` | 补丁1 的实验方案、判读表与实测结果（含厂商报告 §7 Q2 的答案） |
| `aic8800d80_sdio_bringup_handoff.md` | SDIO 上电/总线/初始化流程的移植说明 |
| 本文件 | 两处补丁的落地记录（改动 + 理由 + 验证 + 回滚） |
