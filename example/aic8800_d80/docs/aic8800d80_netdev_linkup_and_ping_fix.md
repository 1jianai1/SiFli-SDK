# AIC8800D80 移植补丁 3 —— netdev 链路标志同步（修 "ping: network interface device(wl) status error."）

> **适用工程**：`example/aic8800_d80`（路线1：驱动自建 lwIP netif，不用 SDK `rt_wlan`/`eth_device`）
> **适用芯片**：AIC8800D80N（卡级 `VID=0xC8A1 PID=0x9082`，运行时 `chipid=4`）
> **前置文档**：本补丁是 [aic8800d80_patch_flowctrl_phase_and_netdev_reregister.md](aic8800d80_patch_flowctrl_phase_and_netdev_register.md) 补丁二的直接后续 —— 补丁二让 netdev **存在**，本补丁让 netdev 的**状态**正确。
> **一句话**：Wi-Fi 连接、DHCP 全都正常，但 `ping` 一个包都发不出去，因为 `netdev->flags` 里 `NETDEV_FLAG_LINK_UP` 从来没被置过。

---

## 0. 摘要

| # | 文件 | 改什么 | 解决什么 |
|---|---|---|---|
| 1 | `customer/peripherals/wifi/aic8800_rtos_driver/platform/netif/netif_port.c` | 新增 `aicwf_net_if_netdev_sync()`（把 lwIP netif 的链路标志/地址镜像到 netdev），并在 `aicwf_net_if_add()` 的两条路径上调用 | `netdev` 恒为 `LINK_DOWN` → netdev 版 `ping` 被前置判据拒绝 |
| 2 | 同上（`aicwf_net_if_add()` 早退分支） | 接口已存在时也补一次镜像 | 断开重连后 `ping` 不回退 |
| 3 | 同上 + `platform/wifi/wifi_port_sta.c` | DHCP 完成后调用 `aicwf_net_if_netdev_sync(NULL)`；同时镜像 MTU 与 DHCP 标志 | `ifconfig` 恒显示 `ip address: 0.0.0.0` / `MTU: 0` / `DHCP_DISABLE`（原文档 §5 的遗留） |

**不涉及**：AIC 预编译库（`libaic8800_driver.a`）、共享 lwIP/RT-Thread 代码、数据通路（TX/RX）、`main.c`。

---

## 1. 症状（实测日志）

连接与 DHCP 完全成功：

```
[AIC][FHOST][FW] tx dhcp
[AIC][APP]app_wlan_sta_get_ip:34:107:237:10[AIC][APP]app_wlan_sta_start: dhcp finish
[AIC][APP]app_wlan_sta_get_ip:34:107:237:10+OK ip: 10:237:107:34
msh />
```

紧接着 `ping` **在发包之前**就被拒了：

```
msh /> ping 8.8.8.8
ping: not found specified netif, using default netdev wl.
ping: network interface device(wl) status error.
msh />
```

判读要点：

| 输出 | 含义 |
|---|---|
| `not found specified netif, using default netdev wl.` | **不是错误**。`ping` 的 msh 包装不传接口名，走 `netdev_default`；它说明 netdev 确实注册了、名字是 `wl`、而且是 default。 |
| `network interface device(wl) status error.` | **真正的故障点**。落在 `netdev.c` 的 `NETDEV_PING_IS_COMMONICABLE` 不成立分支。 |
| 完全没有 `bytes from ...` / `timeout` | 一个 ICMP 包都没发出去 —— 所以这**不是**网络/防火墙/路由问题。 |

另外 `ifconfig` 曾返回：

```
ifconfig: network interface device list error.
```

这是**时机问题，不是故障**：netdev 是在 `sta conn` 流程里（`aic_wifi_start_sta()` → `aicwf_net_if_add()` → `netdev_register()`）才注册的，连接**之前** `netdev_list` 就是 `RT_NULL`，`netdev_list_if()` 因此在 `netdev.c:1083` 直接报错返回。要在 **`sta conn` 成功之后**再敲 `ifconfig`。

---

## 2. 根因链

### 2.1 `ping` 的前置判据

`netdev_cmd_ping()`（`rtos/rtthread/components/net/netdev/src/netdev.c:1280`）：

```c
#define NETDEV_PING_IS_COMMONICABLE(netdev) \
    ((netdev) && (netdev)->ops && (netdev)->ops->ping && \
        netdev_is_up(netdev) && netdev_is_link_up(netdev)) \
```

两个判据来自（`netdev.h:175-176`）：

```c
#define netdev_is_up(netdev)      (((netdev)->flags & NETDEV_FLAG_UP) ? 1 : 0)
#define netdev_is_link_up(netdev) (((netdev)->flags & NETDEV_FLAG_LINK_UP) ? 1 : 0)
```

报 `not support ping feature` 还是 `status error` 可以区分是 `ops->ping` 缺失还是标志位缺失：
日志报的是 **`status error`**（`netdev.c:1329`），说明 `ops->ping` 在（`RT_LWIP_USING_PING=y` 已开），**只是标志位不对**。

### 2.2 这两个标志位由谁设置

`netdev_register()` 只做链入 + 赋值，**从不设置 `NETDEV_FLAG_UP`/`NETDEV_FLAG_LINK_UP`**（`netdev.c:102-141`）。这两个位只能由：

```c
void netdev_low_level_set_status(struct netdev *netdev, rt_bool_t is_up);        /* netdev.c:942 */
void netdev_low_level_set_link_status(struct netdev *netdev, rt_bool_t is_up);   /* netdev.c:975 */
```

置位。而在本 SDK 里，这两个函数**只被 lwIP 自己回调**（`lwip-2.1.2/src/core/netif.c`）：

```c
/* netif_set_up() */
netdev_low_level_set_status(netdev_get_by_name(netif->name), RT_TRUE);          /* :951  */
/* netif_set_down() */
netdev_low_level_set_status(netdev_get_by_name(netif->name), RT_FALSE);         /* :1042 */
/* netif_set_link_up() */
netdev_low_level_set_link_status(netdev_get_by_name(netif->name), RT_TRUE);     /* :1120 */
/* netif_set_link_down() */
netdev_low_level_set_link_status(netdev_get_by_name(netif->name), RT_FALSE);    /* :1150 */
```

### 2.3 而这个驱动绕过了 `netif_set_link_up()`

`aicwf_net_if_init()`（`platform/netif/netif_port.c`）里是**直接把标志写进 `netif->flags`**：

```c
net_if->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_IGMP;   /* 出现两次, 后者覆盖前者 */
```

全树 grep 确认：`customer/peripherals/wifi/aic8800_rtos_driver` 下**没有任何一处调用 `netif_set_link_up()`**。

于是形成割裂：

| 视图 | 载体 | 链路状态 |
|---|---|---|
| lwIP 协议栈 | `netif->flags` | **LINK_UP**（所以 ARP / DHCP / 收发包都正常） |
| RT-Thread netdev | `netdev->flags` | **LINK_DOWN**（永远没人置位） |

**失败的只有 netdev 这一侧**，例如 netdev 版 `ping`、`ifconfig` 的 `FLAGS` 行。

### 2.4 为什么"能连上 + 能拿 IP"仍然 ping 不通

- Wi-Fi 关联 → 走固件，与 netdev 无关；
- DHCP → 走 lwIP 的 `netif`（`netif->output = etharp_output`），与 netdev 无关；
  DHCP ACK 能收到，反证 RX 通路、TX 通路、flow-ctrl 数据信用全部正常；
- `ping` → 走 **netdev 命令层**，在真正调用 `netdev->ops->ping()`（`lwip_netdev_ping`）之前就被 `netdev_is_link_up()` 挡住了。

所以两者并不矛盾，只是 `ping` 比 DHCP 多经过了一层检查。

---

## 3. 改动清单

### 3.1 新增镜像函数（`platform/netif/netif_port.c`，紧跟 `aicwf_net_if_netdev_del()` 之后）

```c
/**
 * @brief 把 lwIP netif 的链路标志 / 地址镜像到 RT-Thread netdev
 *
 *  1) 链路标志: aicwf_net_if_init() 是直接写 netif->flags 的, 绕过了
 *     netif_set_link_up(); 而本 SDK 的 lwIP 只在 netif_set_link_up()/
 *     netif_set_link_down() 里调用 netdev_low_level_set_link_status()
 *     -> netdev.flags 恒为 LINK_DOWN -> netdev 版 ping 直接报
 *        "ping: network interface device(wl) status error."
 *  2) 地址: netdev 的 ip/gw/netmask 是 netdev_register() 时刻的快照,
 *     DHCP 拿到地址后不会自动回灌 -> ifconfig 恒显示 0.0.0.0
 *
 * 顺序: 先置标志再写地址 —— set_link_status(TRUE) 在 "is_up && ip 非 0" 时
 * 会调 sal_check_netdev_internet_up(); 先置标志时 ip 尚为 0, 避开该路径。
 * 幂等, 可重复调用。
 *
 * @param net_if 网络接口句柄, NULL = 默认 STA 接口 (同 aicwf_net_if_get_ip)
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
    /* 与 ethernetif.c::netdev_flags_sync() 一致: 供 ifconfig 显示 DHCP_ENABLE */
    netdev_low_level_set_dhcp_status(nd, RT_TRUE);
#endif
#else
    (void)net_if;
#endif /* RT_USING_NETDEV */
}
```

> 用了 `#ifdef` 包住函数体（而不是包住整个函数），这样 `RT_USING_NETDEV=n` 的工程里
> 调用方无需额外条件编译，符号也一定存在。

### 3.2 声明（`port/inc/netif_port.h`，Interface management 段）

```c
/* [AIC8800-PORT][SiFli] 把 lwIP netif 的链路标志/地址镜像到 RT-Thread netdev。
 * net_if=NULL 表示默认 STA 接口; 幂等, 可重复调用; RT_USING_NETDEV=n 时为空实现。 */
void  aicwf_net_if_netdev_sync(net_if_t net_if);
```

### 3.3 调用点一（**本补丁的核心**）：`aicwf_net_if_add()` 注册 netdev 之后

```c
#ifdef RT_USING_NETDEV
    aicwf_net_if_netdev_add(net_if);

    /* netdev 已登记 -> 立刻按 netif 的真实状态补齐镜像, 关键是 NETDEV_FLAG_LINK_UP */
    aicwf_net_if_netdev_sync(net_if);
#endif

    return 0;
```

顺序很关键：**必须先 `aicwf_net_if_netdev_add()` 再 `sync()`**，否则 `netdev_get_by_name(nif->name)` 查不到 netdev。
`netifapi_netif_add()` 是阻塞的（走 tcpip 线程 + 信号量），返回时 `aicwf_net_if_init()` 已跑完，
所以此刻 `nif->name` 已是 `"wl"`、`nif->flags` 已含 `NETIF_FLAG_LINK_UP`。

### 3.4 调用点二：`aicwf_net_if_add()` 的"接口已存在"早退分支

```c
    if (net_if->output != NULL) {
        DBG_NETAL_INF("%s already added to LwIP\r\n", __func__);
#ifdef RT_USING_NETDEV
        /* 重连场景走这条早退分支: netdev 可能已被 aicwf_net_if_release_all()
         * 摘除后重建, 这里补一次镜像 (幂等) */
        aicwf_net_if_netdev_sync(net_if);
#endif
        return 0;
    }
```

### 3.5 调用点三（顺带修 `ifconfig` 的 `0.0.0.0`）：`app_wlan_sta_start()`

`platform/wifi/wifi_port_sta.c`，DHCP 轮询成功分支：

```c
            if(aicwf_net_if_dhcp_done(0))
            {
                ret = 0;
                app_wlan_sta_get_ip();
                /* DHCP 已拿到地址: 把地址/链路标志回灌 netdev */
                aicwf_net_if_netdev_sync(NULL);
                break;
            }
```

---

## 4. 验证方法

### 4.1 编译

```powershell
& "D:\SiFli\SDK\SiFli-SDK-WiFi\setup_sifli.ps1"
& "D:\SiFli\SDK\SiFli-SDK-WiFi\SiFli-SDK\export.ps1"
cd D:\SiFli\SDK\SiFli-SDK-WiFi\SiFli-SDK\example\aic8800_d80\project
scons --board=sf32lb55-lcd_n16r4n4 -j8
```

> A/B 对比前建议 `scons --board=sf32lb55-lcd_n16r4n4 -c` 清一次：本工程多次出现
> "改了但烧的是旧镜像"的误判（见 `aic8800d80n_final_changes_and_history.md` §3 第 8 条）。

### 4.2 运行判据

`sta conn` 成功、`dhcp finish` 之后：

```
msh /> ifconfig
network interface device: wl (Default)
MTU: 1500
MAC: cc c4 b2 dc 17 ed
FLAGS: UP LINK_UP DHCP_ENABLE                           <-- 关键: 要有 LINK_UP
ip address: 10.237.107.34
gw address: 10.237.107.49
net mask  : 255.255.255.0
dns server #0: 10.237.107.49
```

修复前应为 `FLAGS: UP LINK_DOWN DHCP_DISABLE`、`MTU: 0`、`ip address: 0.0.0.0`。

> `FLAGS` 里**不会**出现 `ETHARP / BROADCAST / IGMP`。这三个位由 `ethernetif.c::netdev_flags_sync()`
> 从 `netif->flags` 映射，而该函数是 `static`、且只服务走 `ethernetif_init` 的标准网卡，本驱动补注册的
> netdev 用不到它。已确认它们在 `netdev.c` 中**仅被 `ifconfig` 的打印语句引用**（`netdev.c:1137-1139`），
> 属纯显示项，不影响任何功能。要补齐可在 `aicwf_net_if_netdev_sync()` 里照抄那三行。

再 `ping`：不应再出现 `status error`，而应开始**真正发包**（成功打印 `bytes from ...`，或超时打印 `timeout`）。

### 4.3 实测结果（2026-10-12，board `sf32lb55-lcd_n16r4n4`）

`ifconfig` 三项全部正确（`LINK_UP` / 真实 IP / 真实网关），`ping` 进入真正收发阶段：

```
msh /> ping 10.237.107.49                                <-- 网关
ping: not found specified netif, using default netdev wl.
ping: from 10.237.107.49 icmp_seq=1 timeout
60 bytes from 10.237.107.49 icmp_seq=2 ttl=64 time=126 ms
60 bytes from 10.237.107.49 icmp_seq=3 ttl=64 time=29 ms
60 bytes from 10.237.107.49 icmp_seq=4 ttl=64 time=29 ms
--- 10.237.107.49 ping statistics ---
4 packets transmitted, 3 received, 25% packet loss
minimum = 29ms, maximum = 126ms, average = 61ms
```

**结论：Wi-Fi 数据通路端到端打通**（网关回 `ttl=64`）。`status error` 已彻底消失。

三个容易被误读的点：

| 现象 | 判读 |
|---|---|
| `ping 10.237.107.1` 100% 丢 | **`.1` 不是本网网关**，本网网关是 DHCP 下发的 `10.237.107.49`。`.1` 无人应答属正常，不是故障。 |
| `ping 8.8.8.8` 100% 丢 | 网关能通说明本机网络栈正常；**企业网/运营商封 8.8.8.8 的 ICMP** 是很常见的。判断"外网通不通"请改用 `ping 114.114.114.114` 或 `ping www.baidu.com`（后者同时验证 DNS）。 |
| 网关 25% 丢包、且 **seq=1 必丢** | 空闲后首个包丢失是本工程已知现象：`g_sdio_irq_mask_forced=1` 时收包靠 10ms 轮询兜底，且空闲后需要重建 BA 会话（日志中可见 `[FW] mm_ba_add_req_handler 0, 64, 32`），因此第 1 个包通常超时、第 2 个包耗时偏高（126ms），其后稳定在 ~29ms。属流程开销，非故障。 |

### 4.4 A/B 对照（可选）

把 `aicwf_net_if_add()` 里新增的 `aicwf_net_if_netdev_sync(net_if);` 注释掉、重编，
即可复现 `FLAGS: ... LINK_DOWN` + `ping: network interface device(wl) status error.`。

---

## 5. 打完补丁后如果**仍然** ping 不通

此时 `ping` 至少已经把包发出去了，"不通"的性质变了，按下面顺序排查（**先网关，别直接打 8.8.8.8**）：

| 步骤 | 命令 | 判读 |
|---|---|---|
| 1 | `ping 10.237.107.1` | 网关。通 → 二层 / ARP / 驱动 / 路由全 OK |
| 2 | `ping <同网段某台机>` | 通 → 本机网络栈没问题 |
| 3 | `ping 8.8.8.8` | **很多企业网/运营商直接封 8.8.8.8 的 ICMP**，不通不代表网络坏。上面两步通就说明网络是好的 |
| 4 | `ping www.baidu.com` | 验证 DNS（DNS 由 DHCP 写进 netdev，补丁二已保证有落点） |

本工程特有的两个额外干扰项：

- `g_sdio_irq_mask_forced = 1` 时收包靠 10ms 轮询兜底（`aic8800d80n_final_changes_and_history.md` §1.B），
  表现为"**偶尔通、丢包多**"，而不是 100% 不通。
- flow-ctrl 数据通路：DHCP 已经收发成功，说明数据信用正常，可基本排除。

### 5.1 实测：网关通、外网不通（2026-10-12）

> ⚠️ **本节的分析已被推翻，保留仅作教训记录。**
> 当时据此得出"设备侧已无问题、问题在网络侧"的结论是**错误**的，
> 真实根因是 lwIP `netif_default` 缺失（[补丁4](aic8800d80_lwip_netif_default_missing.md)）。
> 错在哪里：这里假设"ping 网关的帧和 ping 外网的帧在设备侧走同一条路"，
> 但忽略了设备侧还有一步**路由查表**——同网段命中 `netif`、跨网段要靠 `netif_default`，
> 而后者是 NULL。少算了这一步，就把设备侧问题误判成了网络侧问题。

`ping 10.237.107.49`（网关）**4/4、0% 丢包**，但 `ping 114.114.114.114` 与 `ping www.baidu.com` 全超时。

**（被推翻的）推断**：这个组合似乎说明设备侧已无问题，理由是转发到外网与 ping 网关在设备侧走的是同一条路：

| 环节 | ping 网关 | ping 外网 | 差异 |
|---|---|---|---|
| 路由查表 | 命中直连 `10.237.107.0/24` | 未命中 → 用 `gw = 10.237.107.49` | 同一台网关 |
| ARP | 解析网关 MAC | **复用同一份 ARP 缓存** | 无 |
| 发的以太网帧 | dst=网关 MAC, src=本机 MAC | **完全相同** | 只有 IP 目的地址不同 |

也就是说，只要网关能回包，发往外网的帧一定也正确送到了网关。剩下的可能全在网关之外。

**判据（按成本从低到高）**

| # | 做法 | 结论 |
|---|---|---|
| 1 | 用**同一个 SSID** 的 PC / 手机执行 `ping 114.114.114.114` | **决定性**。PC 也不通 → 网络策略封的，与本设备无关，收工 |
| 2 | `ping www.baidu.com` 的**完整输出** | 若打印 `ping: unknown host www.baidu.com` → DNS 也坏了；若直接开始 `icmp_seq=1 timeout` → **域名解析成功**，说明 UDP/53 已通到上游，再排除 ICMP 被单独封禁 |
| 3 | `ping <同网段另一台 PC 的 IP>` | 通 → 同网段互通正常 |
| 4 | `ping <公司另一个网段的 IP>` | 通 → 网关**会**转发这台设备的跨网段流量，问题缩到公网出口；不通 → 网关不转发本设备流量（准入 / ACL / 802.1X 后置认证），找网管 |
| 5 | `iperf -c <有 iperf 服务端的机器>` | 本固件唯一可用的 TCP 测试（`CONFIG_PKG_NETUTILS_IPERF=y`；netutils 的 ping/tftp/ntp/telnet 都没编进来）。TCP 通而 ICMP 不通 → 确认只是 ICMP 被策略丢弃 |

**真正需要再回头查驱动的情形只有一种**：网关 ping 出现**持续**丢包（不是只有首包），
或同网段其他主机也 ping 不到。那时优先怀疑 `g_sdio_irq_mask_forced` 的 10ms 轮询丢包，
而不是 netdev 镜像。

### 5.2 实测：DNS 通、ICMP 出公网不通（2026-10-12 复测）

```
msh /> ping www.baidu.com
ping: not found specified netif, using default netdev wl.
ping: from 180.101.51.73 icmp_seq=1 timeout      <-- 注意这里: 域名已解析成真实公网 IP
...
4 packets transmitted, 0 received, 100% packet loss
```

`ping: from 180.101.51.73` 这一行是**关键证据**：`www.baidu.com` 被成功解析成了
`180.101.51.73`（百度江苏电信 CDN 的公网地址，不是私网地址，说明 **DNS 没有被门户劫持**）。
也就是说：设备发出了一次跨网段的 UDP/53 查询并**收到了应答**。

至此，设备侧每一层都已被实测验证过：

| 能力 | 证据 | 状态 |
|---|---|---|
| DHCP 获取地址 | `ip address: 10.237.107.34` | ✅ |
| ARP / 二层到网关 | `ping 10.237.107.49` 4/4、0% 丢包 | ✅ |
| ICMP 到网关 | 同上 | ✅ |
| 路由选路（跨网段走网关） | 发往外网的帧与发往网关的帧只有 IP 目的地址不同 | ✅ |
| DNS 查询 + 应答解析 | `www.baidu.com` → `180.101.51.73` | ✅ |
| 网关**转发本设备流量到公网** | 无任何回包 | ❌ **唯一没验证通过的一环** |

**当时的初步结论**是"驱动与协议栈已无问题，剩下的只能由网络侧解释"。**这个结论后来被推翻了**：
继续用 `nettest` 测 TCP 时发现失败耗时是 **`0 ms`**、`errno=118`（`EHOSTUNREACH`），
即 `connect()` 在**本地路由查表**就失败了，根本没有发包 —— 那是**第二个独立的驱动缺口**
（lwIP 的 `netif_default` 从未被设置，导致所有跨网段目的地都无路由）。

详见 [aic8800d80_lwip_netif_default_missing.md](aic8800d80_lwip_netif_default_missing.md)（补丁4）。

**教训**：光看"超时"分不清"本地发不出去"和"网络丢包"，必须同时看 **errno 和耗时**。
`nettest` 就是为了把这两个数字打出来才加的。

---

## 6. 与既有文档的关系 / 勘误

- [aic8800d80_patch_flowctrl_phase_and_netdev_reregister.md](aic8800d80_patch_flowctrl_phase_and_netdev_register.md) §5「已知遗留」原话是：

  > netdev 的 IP / netmask / gw / flags 镜像是注册时刻的快照（`0.0.0.0` / DOWN）。
  > **仅影响 `ifconfig` 的显示，不影响收发**（lwIP 实际用的是 netif）。

  **后半句不准确**：`DOWN` 会直接封掉 netdev 命令层的 `ping`
  （`NETDEV_PING_IS_COMMONICABLE` 的前置判据），不是"只影响显示"。
  该段由本补丁修正。

- "IP 是快照"这一半是对的，本补丁的调用点三（§3.5）修掉它。

- 本补丁不改变"补丁二必须在 `RT_USING_NETDEV=y` 时才有意义"的前提。

---

## 7. 回滚

| 想回到… | 做法 |
|---|---|
| 完全原状（复现 `status error`） | 删除 `aicwf_net_if_add()` 两处 `aicwf_net_if_netdev_sync(...)` 调用；`app_wlan_sta_start()` 里那处删掉；`netif_port.c`/`netif_port.h` 的 `aicwf_net_if_netdev_sync()` 定义与声明可留可删（留着不调用无副作用） |
| 只回滚 `ifconfig` 地址修复 | 只删 `app_wlan_sta_start()` 里的 `aicwf_net_if_netdev_sync(NULL);` |

三处改动全部带 `[AIC8800-PORT][SiFli]` 注释，`grep aicwf_net_if_netdev_sync` 即可定位全部位置。

---

## 8. 相关文档

| 文档 | 内容 |
|---|---|
| [aic8800d80_patch_flowctrl_phase_and_netdev_reregister.md](aic8800d80_patch_flowctrl_phase_and_netdev_register.md) | 补丁1（flow-ctrl 阶段开关）+ 补丁2（netdev 注册） |
| [aic8800d80n_final_changes_and_history.md](aic8800d80n_final_changes_and_history.md) | 终版改动清单 + 修改史 + 回退方法 |
| [aic8800d80_network_connectivity_test_guide.md](aic8800d80_network_connectivity_test_guide.md) | `nettest`（TCP 连通性测试）命令用法 + 手机热点场景排查流程 |
| [aic8800d80_lwip_netif_default_missing.md](aic8800d80_lwip_netif_default_missing.md) | 补丁4：lwIP `netif_default` 从未设置（跨网段流量 0ms 全失败） |
| [aic8800d80_netif_netdev_final_changes.md](aic8800d80_netif_netdev_final_changes.md) | **终版改动清单**（本文与补丁4 的最终形态提炼，照抄即可） |
| 本文件 | 补丁3：netdev 链路标志同步（修 `ping` / `ifconfig`）——**过程记录** |
