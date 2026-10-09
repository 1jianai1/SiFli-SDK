# AIC8800D80 网络接口移植 —— 终版改动清单（netdev 注册 · 状态镜像 · 默认路由）

> **用途**：任何人按 §3 的「锚点 + 代码块」照抄，即可把代码改到**网络可用的终版**（能连 Wi-Fi、能拿 IP、能 ping 通公网、能建 TCP）。
> **适用范围**：SiFli SDK 2.5.0（Wi-Fi 树 `SiFli-SDK-WiFi\SiFli-SDK`）/ board `sf32lb55-lcd_n16r4n4` / 模组 AIC8800D80N。
> **记录原则**：本文**只记录终版形态**。本轮排查过程中"改错了又改回"的中间步骤一律不写，避免后来者照抄到废稿；那些过程记录在各自的专题文档里（见 §7）。
> **改动文件**：4 个（3 个驱动移植层 + 1 个例程）。

---

## 0. 一句话总结

**路线1（驱动自建 lwIP netif、不用 SDK `rt_wlan`/`eth_device`）会绕开 SDK 的三条自动同步链**，
所以必须手工补回来。终版一共补了三条：

| # | 补的是什么 | 不补的后果 | 症状 |
|---|---|---|---|
| A | **RT-Thread netdev 本身** | lwIP 的 `dhcp.c`/`dns.c` 按 `netif->name` 反查 netdev 拿到 NULL → `RT_ASSERT` | DHCP ACK 到达瞬间 tcpip 线程 fatal error |
| B | **netdev 的链路/地址状态** | netdev 恒为 `LINK_DOWN` | `ping` 一个包都不发就报 `status error` |
| C | **lwIP 全局 `netif_default`** | `ip4_route()` 对**跨网段**目的地判定"无路由" | ping/连接任何公网地址 **0 ms** 失败，`errno=118` |

A 是上一轮就修好的（[补丁2](aic8800d80_patch_flowctrl_phase_and_netdev_register.md)）；**B、C 是本轮新增**，其中 C 是最隐蔽的一个。

---

## 1. 症状对照表（先看这张，决定你要不要改）

| 现象 | 缺哪一条 | 去 §几 |
|---|---|---|
| `Assertion failed at function:netdev_set_dns_server` + `fatal error on thread: tcpip` | A | §3.1（外链） |
| `ping: network interface device(wl) status error.` | B | §3.2 |
| `ifconfig` 显示 `MTU: 0` / `DHCP_DISABLE` / `ip address: 0.0.0.0` | B | §3.2 |
| `ping` 网关通，但 `ping`/连接任何**公网**地址都失败，且 `nettest` 显示 **0 ms** + `errno=118` | C | §3.3 |
| `ping www.baidu.com` 域名能解析（打印出公网 IP）但 4/4 超时 | C（最常见就是它） | §3.3 |
| `ping 8.8.8.8` / `114.114.114.114` 全超时 | **很可能也是 C**，不要急着怪运营商 | §3.3 |

---

## 2. 改动点速查

| # | 文件 | 位置 | 改动 |
|---|---|---|---|
| 1 | `platform/netif/netif_port.c` | `aicwf_net_if_netdev_add()` | 把 `nd->ops` / 地址 / MAC 的赋值**前移到 `netdev_register()` 之前** |
| 2 | `platform/netif/netif_port.c` | 文件内新增函数 | 新增 `aicwf_net_if_netdev_sync()` |
| 3 | `platform/netif/netif_port.c` | `aicwf_net_if_add()` | 新增 **2 处** `aicwf_net_if_netdev_sync()` 调用 |
| 4 | `port/inc/netif_port.h` | Interface management 段 | 声明 `aicwf_net_if_netdev_sync()` |
| 5 | `platform/wifi/wifi_port_sta.c` | `app_wlan_sta_start()` | DHCP 成功后调用 `aicwf_net_if_netdev_sync(NULL)` |
| 6 | `example/aic8800_d80/src/main.c` | 文件末尾 | 新增 `nettest` 命令（TCP 连通性测试） |

所有改动都带 `[AIC8800-PORT][SiFli]` 注释，`grep -rn "aicwf_net_if_netdev_sync\|netif_default" ` 可定位全部位置。

---

## 3. 终版改动清单（照抄即可）

### 3.1 补丁 A：netdev 注册（**非本轮改动**，但缺它后面两条无从谈起）

见 [aic8800d80_patch_flowctrl_phase_and_netdev_reregister.md](aic8800d80_patch_flowctrl_phase_and_netdev_register.md) §3：
`platform/netif/netif_port.c` 新增 `aicwf_net_if_netdev_add()` / `aicwf_net_if_netdev_del()`，
并在 `aicwf_net_if_add()` 中调用、`aicwf_net_if_release_all()` 中配对摘除。

**本文下面的改动都以 A 已存在为前提。**

---

### 3.2 补丁 B：把 netif 状态镜像给 netdev

#### (1) 新增函数 `aicwf_net_if_netdev_sync()`

- **文件**：`customer/peripherals/wifi/aic8800_rtos_driver/platform/netif/netif_port.c`
- **锚点**：紧跟 `aicwf_net_if_netdev_del()` 的结尾 `#endif /* RT_USING_NETDEV */` **之后**
  （`grep` 定位：`aicwf_net_if_netdev_del` 函数体后的那个 `#endif`）

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

    /* [AIC8800-PORT][SiFli] 兜底: lwIP 的全局 netif_default 是**跨网段路由的唯一
     * 出口** —— ipv4/ip4.c:229 在 netif_default==NULL 时对任何非本网段目的地直接
     * 判定无路由, 表现为 connect()/sendto() 0ms 失败、errno=118(EHOSTUNREACH)。
     * 正常情况下这一步已由 netdev_register() -> netdev_set_default() ->
     * ops->set_default() 完成 (见 aicwf_net_if_netdev_add 的说明); 这里再兜一次,
     * 覆盖"netdev 不是第一个注册的"等边界情况。幂等。 */
    if (netif_default == RT_NULL)
        netif_set_default(nif);
#else
    (void)net_if;
#endif /* RT_USING_NETDEV */
}
```

> 注意 `#ifdef` 包的是**函数体**而不是整个函数：这样 `RT_USING_NETDEV=n` 的工程里
> 调用方无需额外条件编译，符号也一定存在（不调用就不会有引用）。

#### (2) 声明

- **文件**：`customer/peripherals/wifi/aic8800_rtos_driver/port/inc/netif_port.h`
- **锚点**：`net_if_t aicwf_net_if_get(uint8_t index);` 之后、`/* Buffer management */` 之前

```c
/* [AIC8800-PORT][SiFli] 把 lwIP netif 的链路标志/地址镜像到 RT-Thread netdev。
 * 路线1 的 netdev 是补注册的, 两侧状态需显式同步 (原因见 netif_port.c 实现处)。
 * net_if=NULL 表示默认 STA 接口; 幂等, 可重复调用; RT_USING_NETDEV=n 时为空实现。 */
void  aicwf_net_if_netdev_sync(net_if_t net_if);
```

#### (3) 调用点一：`aicwf_net_if_add()` 的"接口已存在"早退分支

- **锚点**：`DBG_NETAL_INF("%s already added to LwIP\r\n", __func__);` 紧跟的 `return 0;`

```c
    if (net_if->output != NULL) {
        DBG_NETAL_INF("%s already added to LwIP\r\n", __func__);
#ifdef RT_USING_NETDEV
        /* [AIC8800-PORT][SiFli] 重连场景走这条早退分支: netdev 可能已被
         * aicwf_net_if_release_all() 摘除后重建, 这里补一次镜像 (幂等) */
        aicwf_net_if_netdev_sync(net_if);
#endif
        return 0;
    }
```

#### (4) 调用点二：`aicwf_net_if_add()` 注册 netdev 之后（**本条最关键**）

- **锚点**：`aicwf_net_if_netdev_add(net_if);` 之后、`return 0;` 之前

```c
#ifdef RT_USING_NETDEV
    /* [AIC8800-PORT][SiFli] 补注册 RT-Thread netdev: 本 SDK 的 lwIP 把 DNS 存在
     * netdev 上, 且 dhcp.c/dns.c 会以 netif->name 反查 netdev, 缺失即断言 */
    aicwf_net_if_netdev_add(net_if);

    /* [AIC8800-PORT][SiFli] netdev 已登记 -> 立刻按 netif 的真实状态补齐镜像。
     * 关键的是 NETDEV_FLAG_LINK_UP: init 里直写 netif->flags 的写法不会经过
     * netif_set_link_up(), netdev 侧永远是 LINK_DOWN, netdev 版 ping 会被
     * "network interface device(wl) status error" 直接拒绝 (详见本函数实现处注释)。 */
    aicwf_net_if_netdev_sync(net_if);
#endif

    return 0;
}
```

> **顺序不能颠倒**：必须先 `aicwf_net_if_netdev_add()`（登记 netdev）再 `sync()`，
> 因为 `sync()` 内部用 `netdev_get_by_name(nif->name)` 反查。

#### (5) 调用点三：DHCP 成功后回灌地址

- **文件**：`customer/peripherals/wifi/aic8800_rtos_driver/platform/wifi/wifi_port_sta.c`
- **锚点**：`app_wlan_sta_start()` 里 `app_wlan_sta_get_ip();` 之后

```c
            if(aicwf_net_if_dhcp_done(0))
            {
                ret = 0;
                app_wlan_sta_get_ip();
                /* [AIC8800-PORT][SiFli] DHCP 已拿到地址: 把 netif 的地址/链路标志
                 * 回灌 RT-Thread netdev, 否则 ifconfig 恒显示 0.0.0.0, 且 netdev
                 * 版 ping 会以 "status error" 拒绝发包 (幂等, 详见 aicwf_net_if_netdev_sync) */
                aicwf_net_if_netdev_sync(NULL);
                break;
            }
```

---

### 3.3 补丁 C：让 lwIP 拿到 `netif_default`（**本轮最难发现的一条**）

- **文件**：`customer/peripherals/wifi/aic8800_rtos_driver/platform/netif/netif_port.c`
- **锚点**：`aicwf_net_if_netdev_add()` 里 `memcpy(name, nif->name, ...)` 之后，原本是 `netdev_register()` 调用

把原本写在 `netdev_register()` **之后**的字段赋值，**整体前移**到它**之前**：

```c
    /* 与 ethernetif.c::netdev_add() 一致: netdev 名 = lwIP netif->name */
    memcpy(name, nif->name, sizeof(nif->name));
    name[NETIF_NAMESIZE - 1] = '\0';

    /* [AIC8800-PORT][SiFli] 这些字段必须在 netdev_register() **之前** 装好, 否则
     * lwIP 的全局 netif_default 永远不会被设置, 症状极其隐蔽:
     *
     *   netdev_register() 在 netdev_default==NULL 时会内部调用
     *   netdev_set_default(netdev_list) (netdev.c:126-129), 而
     *   netdev_set_default() 会执行 netdev->ops->set_default(netdev)
     *   (netdev.c:490-493); lwIP port 的这个回调正是 netif_set_default()
     *   (rtos/rtthread/components/net/lwip/port/ethernetif.c:321)。
     *   若此刻 nd->ops 还是 NULL, 这次同步被静默跳过 -> netif_default 恒为 NULL。
     *
     * 后果是"只有同网段能用":
     *   - 与本机同网段的目的地: ip4_addr_netcmp 命中本 netif -> 正常发出。
     *     所以 DHCP、ping 网关、到网关的 DNS 全部正常;
     *   - 任何跨网段目的地: 走到 ipv4/ip4.c:229
     *     "netif_default == NULL -> No route" 分支, ip4_route() 直接返回 NULL,
     *     sendto()/connect() 在本地就失败(0ms), errno=118(工具链 sys/errno.h
     *     里 EHOSTUNREACH)。表现为"能 ping 通网关, 但任何公网地址都 0ms 失败"。
     *
     * 注意: aicwf_net_if_set_default() 虽有定义, 但全工程无人调用, 兜不住这里。 */
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
```

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

> **为什么前移是安全的**：`netdev_register()` 内部**不做 `memset`**，只写
> `name` / `user_data` / `list` / `ifindex`，预先填好的字段不会被冲掉；
> 且 `user_data`（`netdev.c:104`）在 `netdev_set_default()`（`netdev.c:128`）**之前**已赋值，
> 所以钩子里 `netif_set_default(netif->user_data)` 拿到的正是本 netif。

---

### 3.4 `nettest` 命令（诊断工具，建议保留）

- **文件**：`example/aic8800_d80/src/main.c`（本工程内，非驱动）
- **位置**：文件末尾，`MSH_CMD_EXPORT(wifi_test, ...)` 之后
- **同时在文件头部增加 include**：

```c
/* nettest 命令所需的 socket 头 (与 packages/iot/netutils/iperf 相同的取法) */
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
```

完整实现见 [src/main.c](../src/main.c) 中的 `nettest` 段（约 110 行起），核心设计三点：

1. **为什么单开线程**：lwIP 的 `connect()` 阻塞且 SYN 重传超时不可控（可能几十秒），
   故丢进工作线程，msh 侧用**信号量等固定超时**。
2. **信号量不 detach**：超时返回时工作线程可能还阻塞在 `connect()` 里，
   若此时 `rt_sem_detach()`，线程稍后 `rt_sem_release()` 会操作已销毁对象。
   故信号量只初始化一次、常驻，每次进入前抽干残留 token。
3. **支持域名**：先 `inet_addr()` 试点分十进制，失败再 `gethostbyname()`，顺带验证 DNS。

用法：

```sh
nettest 180.101.51.73 80        # 直接给 IP
nettest www.baidu.com 80        # 给域名, 内部解析
nettest 114.114.114.114 53
```

---

## 4. 为什么必须这么改（理由汇总）

### 4.1 根子在"路线1 绕开了 SDK 的自动同步链"

SDK 的标准网卡走 `ethernetif.c::ethernetif_init` → `netdev_add()`，那里会一并完成
"建 netdev / 设默认接口 / 同步 flags"。路线1 的驱动自己
`netifapi_netif_add()` + 自己的 init 回调建 netif，**这三件事一件都不会自动发生**，
全部要手工补。补丁 A/B/C 正好一一对应这三件事。

### 4.2 缺 B 的机理

| 调用点 | 谁负责把 lwIP 的链路状态告诉 netdev |
|---|---|
| 标准网卡 | `netif_set_link_up()` 内部回调 `netdev_low_level_set_link_status()`（`lwip-2.1.2/src/core/netif.c:1120`） |
| 本驱动 | `aicwf_net_if_init()` **直接写** `net_if->flags |= NETIF_FLAG_LINK_UP`，绕过了函数调用 → netdev 收不到 |

而 `netdev_cmd_ping()` 的前置判据（`netdev.c:1288`）要求
`netdev_is_up() && netdev_is_link_up()`，两者取自 `netdev->flags`。
于是 lwIP 侧链路正常（ARP/DHCP 全好），netdev 侧恒 `LINK_DOWN`，`ping` 直接拒发。

### 4.3 缺 C 的机理（为什么要"前移赋值"而不是"补一次调用"）

`netif_default` 是 lwIP 的全局变量，**无初值**（`netif.c:119`），
只有 `netif_set_default()`（`netif.c:911`）会写它，而 `netif_add()` **不会**自动设置。

本 SDK 的设计是经由 netdev 钩子同步：

```
netdev_register()                      netdev.c:126-129
  └─ netdev_set_default()              netdev.c:483-502
       └─ if (netdev->ops && netdev->ops->set_default)   ← ★ 断点
            └─ lwip_netdev_set_default()  ethernetif.c:321
                 └─ netif_set_default()   ← 真正写入
```

补丁2 的写法是"先 `netdev_register()`、后装 `ops`"，于是第 4 步的判据为假，
**整条同步链被静默跳过**。所以正解是**把字段赋值前移**，让这次内部的
`netdev_set_default()` 能真正生效；`sync()` 里那句 `netif_default == NULL` 兜底
只是覆盖"netdev 不是第一个注册"的边界情况。

**为什么症状是"只有同网段能用"**：

| 目的地 | 路由方式 | 需要 `netif_default`？ |
|---|---|---|
| DHCP 广播 / 网关 / 到网关的 DNS | 同网段，`ip4_addr_netcmp` 命中本 netif | ❌ → 一直正常，掩盖了问题 |
| 任何公网地址 | 都不命中 → 落到默认接口（`ip4.c:229`） | ✅ → 返回 NULL，"无路由" |

### 4.4 为什么一定要留下 `nettest`

`ping` 用的是 netdev 层，**不打印 errno**。所以"本地路由失败"和"对方没回包"
在它眼里都是 `timeout`，长得一模一样——本轮排查就因此先误判成
「运营商封 ICMP」/「手机热点禁止共享」，白绕了两轮。

`nettest` 把 **errno + 耗时**打出来，用「多久失败」一刀切开：

| 耗时 | 含义 |
|---|---|
| **0 ms** | 本地就失败了（驱动/路由表问题），和网络无关 |
| 几 ms ~ 几百 ms | 收到了对方回应（RST/ICMP 不可达），**网络是通的** |
| 接近超时上限 | 无人应答，**网络不通** |

> 附：本工程 `lwipopts.h:24` 写了 `LWIP_ERRNO_INCLUDE "sys/errno.h"`，
> 所以 errno 用**工具链 newlib 的值**，不是 lwIP 自带表：
> `ETIMEDOUT=116`（不是110）、`EHOSTUNREACH=118`（不是113）、`ENETUNREACH=114`（不是101）。
> 查 errno 时别查错表。

---

## 5. 编译与验证

### 5.1 编译

```powershell
& "D:\SiFli\SDK\SiFli-SDK-WiFi\setup_sifli.ps1"
cd D:\SiFli\SDK\SiFli-SDK-WiFi\SiFli-SDK\example\aic8800_d80\project
scons --board=sf32lb55-lcd_n16r4n4 -j8
```

> 若 `export.ps1` 报 pypi 网络错误，可先 `$env:UV_OFFLINE = "1"`（用已缓存的 uv 包），
> 或 `$env:SIFLI_SDK_MIRROR_CHINA = "1"` 走国内镜像。
> **A/B 对比前务必 `scons --board=... -c` 清一次**——本工程多次出现"改了却烧旧镜像"的误判。

### 5.2 终版验收判据（2026-10-12 实测通过）

```
msh /> ifconfig
network interface device: wl (Default)
MTU: 1500                                                <- 补丁B
MAC: cc c4 b2 dc 17 ed
FLAGS: UP LINK_UP DHCP_ENABLE                            <- 补丁B(核心是 LINK_UP)
ip address: 10.237.107.34
gw address: 10.237.107.49
net mask  : 255.255.255.0
dns server #0: 10.237.107.49

msh /> ping 10.237.107.49                                <- 网关
4 packets transmitted, 4 received, 0% packet loss

msh /> nettest 180.101.51.73 80                          <- 补丁C
nettest: CONNECT OK (102 ms)  <== 目标 TCP 端口可达, 公网是通的

msh /> ping www.baidu.com                                <- 补丁C
60 bytes from 180.101.49.44 icmp_seq=1 ttl=52 time=100 ms
...
4 packets transmitted, 4 received, 0% packet loss
```

| 检查项 | 修复前 | 终版 |
|---|---|---|
| `ifconfig` FLAGS | `UP LINK_DOWN DHCP_DISABLE` | `UP LINK_UP DHCP_ENABLE` |
| `ifconfig` MTU / IP | `0` / `0.0.0.0` | `1500` / 真实 IP |
| `ping` 网关 | 通 | 通 |
| `nettest` 公网 | `CONNECT FAIL (errno=118, 0 ms)` | `CONNECT OK (102 ms)` |
| `ping` 公网域名 | 4/4 超时 | 4/4 收到 |

> 几点正常现象，别当故障：
> - `ping: not found specified netif, using default netdev wl.` —— msh 的 `ping` 不传接口名，
>   **每次都会打印**，它后面跟的是真正的结果。
> - 空闲后首次发包会看到 `[FW] mm_ba_add_req_handler 0, 64, 32`，耗时偏高（~100ms+），
>   之后稳定。这是重建 BA 会话 + `g_sdio_irq_mask_forced=1` 的 10ms 轮询兜底，属流程开销。
> - `ifconfig` 的 `FLAGS` 里**不会**有 `ETHARP/BROADCAST/IGMP`：它们由
>   `ethernetif.c::netdev_flags_sync()`（`static`，只服务标准网卡）映射，
>   且在本 SDK 里**仅被 `ifconfig` 打印语句引用**（`netdev.c:1137-1139`），纯显示项。

---

## 6. 回滚

按 `[AIC8800-PORT][SiFli]` 注释逐条反做即可：

| 想回到… | 做法 |
|---|---|
| 复现"ping status error" | 删掉 §3.2 的 (3)(4)(5) 三处 `aicwf_net_if_netdev_sync()` 调用 |
| 复现"跨网段 0ms 失败" | 把 §3.3 的字段赋值移回 `netdev_register()` 之后，并去掉 `sync()` 末尾的 `netif_default` 兜底 |
| 完全原始（回到补丁A 之前） | 见 [补丁2 文档](aic8800d80_patch_flowctrl_phase_and_netdev_register.md) §3.5 |
| 去掉 `nettest` | 删 `src/main.c` 里 `nettest` 段与新增的 socket include（不影响其它功能） |

`aicwf_net_if_netdev_sync()` 的函数体与声明可以留着不调用——不引用就不会产生副作用。

---

## 7. 相关文档

| 文档 | 内容 | 本文与它的关系 |
|---|---|---|
| [aic8800d80_patch_flowctrl_phase_and_netdev_reregister.md](aic8800d80_patch_flowctrl_phase_and_netdev_register.md) | 补丁1（flow-ctrl 阶段开关）+ 补丁2（netdev 注册 = 本文补丁 A） | **前提**，本文不重复 |
| [aic8800d80_netdev_linkup_and_ping_fix.md](aic8800d80_netdev_linkup_and_ping_fix.md) | 补丁3：netdev 链路标志同步（= 本文补丁 B）的**过程记录** | 本文 §3.2 是它的**终版提炼**；其 §5.1 有一段被推翻的推断，保留作教训 |
| [aic8800d80_lwip_netif_default_missing.md](aic8800d80_lwip_netif_default_missing.md) | 补丁4：lwIP `netif_default` 缺失（= 本文补丁 C）的**过程记录** | 本文 §3.3 是它的**终版提炼** |
| [aic8800d80_network_connectivity_test_guide.md](aic8800d80_network_connectivity_test_guide.md) | `nettest` 用法 + 网络排查流程 + 现象速查表 | 验证阶段配合使用 |
| [aic8800d80n_final_changes_and_history.md](aic8800d80n_final_changes_and_history.md) | **上一轮**（SDIO/flow-ctrl/中断风暴）的终版清单 + 修改史 | 与本文互补，两者合起来才是整个工程的终版 |
| 本文件 | **本轮**（netdev / 路由）终版改动清单 | —— |

---

## 8. 一句话教训（给后来者）

> 这套"驱动自建 netif + 补注册 netdev"的路线，SDK 不会替你做的同步一共有三件：
> **建 netdev、同步 netdev 状态、设 lwIP 默认接口**。
> 三者缺任何一个，症状都会被包装成"网络问题"（连接成功 / 接口状态错误 / 公网不通）。
>
> 而 **`ping` 的 `timeout` 不能证明"包已经发出去了"**——
> 排查时请用 `nettest` 看 **errno + 耗时**，`0 ms` 一定是本地问题。
