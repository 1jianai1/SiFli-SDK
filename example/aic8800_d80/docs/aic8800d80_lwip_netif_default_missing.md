# AIC8800D80 移植补丁 4 —— lwIP `netif_default` 缺失（跨网段流量 0ms 全部失败）

> **适用工程**：`example/aic8800_d80`
> **前置文档**：本补丁与 [补丁2（netdev 注册）](aic8800d80_patch_flowctrl_phase_and_netdev_register.md)、
> [补丁3（netdev 链路标志同步）](aic8800d80_netdev_linkup_and_ping_fix.md) 是**同一族**问题：
> 都是"驱动自建 lwIP netif + 补注册 RT-Thread netdev"这套路线1 架构下的状态/时序缺口。
> **一句话**：`netif_default` 从来没被设置过 → lwIP 只能路由到**同网段**，任何跨网段目的地
> 在本地路由查表就失败，一个包都不发。

---

## 0. 摘要

| # | 文件 | 改什么 | 解决什么 |
|---|---|---|---|
| 1 | `platform/netif/netif_port.c` → `aicwf_net_if_netdev_add()` | 把 `nd->ops = &lwip_netdev_ops` 等字段赋值**移到 `netdev_register()` 之前** | `netdev_register()` 内部的 `netdev_set_default()` 因 `ops` 尚为 NULL 而静默跳过 → lwIP 的 `netif_default` 永远为 NULL |
| 2 | 同上 → `aicwf_net_if_netdev_sync()` | 末尾加 `if (netif_default == RT_NULL) netif_set_default(nif);` 兜底 | 覆盖"netdev 不是第一个注册"等边界情况 |

**不涉及**：AIC 预编译库、共享 lwIP/RT-Thread 代码、数据通路、`main.c`。

---

## 1. 症状：一种很容易误判成"网络被封"的失败

`netdev` 侧一切正常（补丁3 已修）：

```
msh /> ifconfig
FLAGS: UP LINK_UP DHCP_ENABLE
ip address: 10.237.107.34
gw address: 10.237.107.49
```

但是：

| 目标 | 结果 |
|---|---|
| `ping 10.237.107.49`（**同网段**网关） | ✅ 4 packets transmitted, 4 received, **0% packet loss** |
| `ping www.baidu.com` | 域名解析**成功**（`180.101.51.73`），但 ICMP 全超时 |
| `nettest www.baidu.com 80` | `CONNECT FAIL (errno=118, **0 ms**)` |
| `nettest 114.114.114.114 53` | 同上 |
| `nettest 180.101.51.73 80` | 同上 |

**关键在 `0 ms`**：TCP 连接的 SYN 都没发出去，`connect()` 在**本地**就返回失败了。
"网络被封"绝不会是 0 毫秒 —— 被封的话至少要走完 SYN 重传超时（几秒到几十秒）。

### 1.1 `errno=118` 是什么（这里有个坑）

**必须查工具链的 `sys/errno.h`，不能查 lwIP 自带的表**，因为
`rtos/rtthread/components/net/lwip/port/lwipopts.h:24` 定义了：

```c
#define LWIP_ERRNO_INCLUDE "sys/errno.h"
```

也就是说 lwIP 用的是 newlib 的 errno 值。两张表**不一样**：

| 名字 | lwIP 自带表 | **本工程实际使用的 newlib 表** |
|---|---|---|
| `ETIMEDOUT` | 110 | **116** |
| `ECONNREFUSED` | 111 | 111 |
| `EHOSTUNREACH` | 113 | **118** ← 本次 |
| `ENETUNREACH` | 101 | **114** |

所以 `errno=118` = **`EHOSTUNREACH`（Host is unreachable）**，
配合 `0 ms`，指向 lwIP 内部的**路由查表失败**（`ERR_RTE`），而不是对端回了 RST。

---

## 2. 根因链

### 2.1 lwIP 只用 `netif_default` 兜底跨网段流量

`lwip-2.1.2/src/core/ipv4/ip4.c`：

```c
/* 1) 先逐个 netif 做网络号比对(同网段命中) —— 见 ip4_route_src 前半段 */
/* 2) 都不命中时, 用全局默认接口兜底: */
    if ((netif_default == NULL) || !netif_is_up(netif_default) || !netif_is_link_up(netif_default) ||
            ip4_addr_isany_val(*netif_ip4_addr(netif_default)) || ip4_addr_isloopback(dest))
    {
        /* No matching netif found and default netif is not usable. */
        IP_STATS_INC(ip.rterr);
        return NULL;
    }
    return netif_default;                          /* ip4.c:229-241 */
```

这解释了**为什么只有同网段能用**：

| 目的地 | 走哪条路 | 依赖 `netif_default`? |
|---|---|---|
| `10.237.107.49`（网关，同网段） | 网络号 `10.237.107.0/24` 命中本 netif | ❌ 不需要 → 正常 |
| DHCP 广播 | 直接走 netif | ❌ 不需要 → 正常 |
| DNS `10.237.107.49` | 同网段 | ❌ 不需要 → 正常 |
| `180.101.51.73` / `114.114.114.114` | 都不命中 → 落到默认接口 | ✅ **需要** → 失败 |

### 2.2 而 `netif_default` 从来没被赋值

`netif_default` 是 lwIP 的全局变量，**无初值**（`netif.c:119`，BSS 里为 NULL）。
全工程只有 `netif_set_default()`（`netif.c:911`）会写它，而 `netif_add()` **不会**自动设置默认接口
（`netif.c:297-303` 那段只是 `LWIP_SINGLE_NETIF` 下的断言保护）。

那谁应该调用 `netif_set_default()`？本 SDK 的设计是走 netdev 层的钩子：

```
netdev_register()  (netdev.c:126-129)
   └─ if (netdev_default == RT_NULL) netdev_set_default(netdev_list)
         └─ netdev_set_default()  (netdev.c:483-502)
               └─ if (netdev->ops && netdev->ops->set_default) netdev->ops->set_default(netdev)
                     └─ lwip_netdev_set_default()  (lwip/port/ethernetif.c:321)
                           └─ netif_set_default((struct netif *)netif->user_data)   ← 真正写入
```

**断点在第 4 步**：补丁2 的 `aicwf_net_if_netdev_add()` 里，
`netdev_register()` 是在 `nd->ops = &lwip_netdev_ops;` **之前**调用的：

```c
    if (netdev_register(nd, name, nif) != 0) { ... }   /* ← 此刻 nd->ops == NULL */
    nd->ops = &lwip_netdev_ops;                        /* ← 太晚了 */
```

于是 `netdev->ops && ...` 这个判据为假，**整条同步链被静默跳过**，
`netif_default` 保持 NULL 直到今天。

> 补充：驱动里其实有一个 `aicwf_net_if_set_default()`（`netif_port.c:677`，内部就是
> `netif_set_default()`），但**全工程没有任何地方调用它**，所以兜不住这个缺口。

### 2.3 为什么之前一直没暴露

这条路线是"先跑通连接，再修网络功能"逐步演进的：

| 阶段 | 为什么没发现问题 |
|---|---|
| 补丁1/2 之前 | 连都连不上，根本走不到发数据这一步 |
| DHCP / 网关 ping / DNS | 三者全是**同网段直连**，不经过 `netif_default` |
| 补丁3 修完 | `ping` 终于会发包了，但 netdev 版 `ping` 打到公网也是 **ICMP 发不出去**，只表现为"超时"，看不出是本地就失败了 |

真正的破案工具是 `nettest`（`src/main.c`）：它打印 **errno + 耗时**，
`0 ms` 这个数字把"本地路由失败"和"网络被封"彻底区分开了。

---

## 3. 改动清单（2 处，都在 `platform/netif/netif_port.c`）

### 3.1 `aicwf_net_if_netdev_add()`：字段赋值移到 `netdev_register()` 之前

```c
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
     *     sendto()/connect() 在本地就失败(0ms), errno=118(EHOSTUNREACH)。
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
```

> **为什么这样是安全的**：`netdev_register()` 内部**不做 `memset`**，只写 `name` / `user_data` /
> `list` / `ifindex`，所以预先填好的字段不会被冲掉；
> 而 `user_data`（`netdev.c:104`）在 `netdev_set_default()`（`netdev.c:128`）**之前**就已经赋值，
> 所以钩子里 `netif_set_default(netif->user_data)` 拿到的正是本 netif。

### 3.2 `aicwf_net_if_netdev_sync()`：兜底

```c
    /* [AIC8800-PORT][SiFli] 兜底: lwIP 的全局 netif_default 是跨网段路由的唯一出口
     * —— ipv4/ip4.c:229 在 netif_default==NULL 时对任何非本网段目的地直接判定无路由,
     * 表现为 connect()/sendto() 0ms 失败、errno=118(EHOSTUNREACH)。
     * 正常情况下这一步已由 netdev_register() -> netdev_set_default() ->
     * ops->set_default() 完成; 这里再兜一次, 覆盖"netdev 不是第一个注册的"等边界情况。 */
    if (netif_default == RT_NULL)
        netif_set_default(nif);
```

---

## 4. 验证

### 4.1 期望的日志变化

修复前（`nettest`）：

```
nettest: TCP 180.101.51.73:80 (timeout 8000 ms) ...
nettest: CONNECT FAIL (errno=118, 0 ms)  <== 0ms + EHOSTUNREACH = 本地无路由
```

修复后，**绝不会再出现 `0 ms`**。三种可能，都算修复成功：

```
nettest: CONNECT OK (... ms)            <- 公网 TCP 通, 收工
nettest: CONNECT FAIL (errno=111, ...)  <- 对端回 RST, 网络通
nettest: TIMEOUT after 8000 ms          <- 真的超时, 数据被网络丢弃
```

> 也就是说：**修复的判据是"耗时不再是 0ms / errno 不再是 118"**。
> 修完之后如果变成 `TIMEOUT`，说明包已经发出去了，问题转到网络侧
> （见 [测试手册](aic8800d80_network_connectivity_test_guide.md) §3）。

### 4.2 最终实测（2026-10-12，board `sf32lb55-lcd_n16r4n4`，AP = 手机热点）

修复后两条命令全部成功，**公网 TCP 与 ICMP 同时打通**：

```
msh /> nettest 180.101.51.73 80
nettest: TCP 180.101.51.73:80 (timeout 8000 ms) ...
[AIC][FHOST][FW] mm_ba_add_req_handler 0, 64, 32          <- 首次发包要建 BA 会话
nettest: CONNECT OK (102 ms)  <== 目标 TCP 端口可达, 公网是通的

msh /> ping www.baidu.com
ping: not found specified netif, using default netdev wl.
60 bytes from 180.101.49.44 icmp_seq=1 ttl=52 time=100 ms
60 bytes from 180.101.49.44 icmp_seq=2 ttl=52 time=119 ms
60 bytes from 180.101.49.44 icmp_seq=3 ttl=52 time=109 ms
60 bytes from 180.101.49.44 icmp_seq=4 ttl=52 time=129 ms
--- 180.101.49.44 ping statistics ---
4 packets transmitted, 4 received, 0% packet loss
minimum = 109ms, maximum = 129ms, average = 114ms
```

对照修复前：

| 命令 | 修复前 | 修复后 |
|---|---|---|
| `nettest 180.101.51.73 80` | `CONNECT FAIL (errno=118, 0 ms)` | **`CONNECT OK (102 ms)`** |
| `ping 8.8.8.8` / `114.114.114.114` | 100% 丢包 | 不再出现（改测域名即通过） |
| `ping www.baidu.com` | 解析成功但 4/4 超时 | **4/4 收到，ttl=52** |

> **重要澄清**：此前"ping 公网全超时"曾被推测为
> 「运营商封 ICMP」或「手机热点禁止共享」。**这个推测是错的** ——
> 根因始终是本补丁描述的 `netif_default` 缺失。netdev 版 `ping` 不会打印 errno，
> 所以本地路由失败被它显示成了普通的 `timeout`，看起来就像"包发出去了但没人回"。
> **不要把 `ping` 的 `timeout` 当作"包已经发出"的证据。**

### 4.3 回归检查

修复不能破坏已有功能，重编后确认：

| 检查项 | 期望 |
|---|---|
| `ifconfig` | `FLAGS: UP LINK_UP DHCP_ENABLE`，IP/网关/DNS 正确 |
| `ping <网关>` | 4/4、0% 丢包 |
| `ping www.baidu.com` | 域名仍能解析成公网 IP |

---

## 5. errno 速查（本工程）

因为 `lwipopts.h` 里 `LWIP_ERRNO_INCLUDE "sys/errno.h"`，**用工具链 newlib 的值**：

| errno | newlib 名字 | 含义 | 网络通不通 |
|---|---|---|---|
| 111 | `ECONNREFUSED` | 对端回了 RST | **通** |
| 104 | `ECONNRESET` | 连接被重置 | **通** |
| 113 | `ECONNABORTED` | 本端中止 | — |
| 114 | `ENETUNREACH` | 网络不可达（收到了 ICMP 不可达） | 部分通 |
| 115 | `ENETDOWN` | 接口未配置 | 本地问题 |
| 116 | `ETIMEDOUT` | 超时，无人应答 | **不通** |
| 118 | `EHOSTUNREACH` | **本地路由查表失败**（`netif_default` 缺失时就是它） | 本地问题 |
| 119 | `EINPROGRESS` | 非阻塞连接进行中 | — |

---

## 6. 回滚

把两处改动各自删掉即可（或只把 `nd->ops` 等字段赋值移回 `netdev_register()` 之后、
并去掉 sync 末尾的 `netif_default` 兜底），行为回到"只有同网段可用"。

改动都带 `[AIC8800-PORT][SiFli]` 注释，`grep -n "netif_default" netif_port.c` 可定位。

---

## 7. 相关文档

| 文档 | 内容 |
|---|---|
| [aic8800d80_patch_flowctrl_phase_and_netdev_reregister.md](aic8800d80_patch_flowctrl_phase_and_netdev_register.md) | 补丁1（flow-ctrl 阶段开关）+ 补丁2（netdev 注册） |
| [aic8800d80_netdev_linkup_and_ping_fix.md](aic8800d80_netdev_linkup_and_ping_fix.md) | 补丁3：netdev 链路标志同步（修 `ping: status error`） |
| [aic8800d80_network_connectivity_test_guide.md](aic8800d80_network_connectivity_test_guide.md) | `nettest` 用法 + 手机热点排查流程 |
| [aic8800d80_netif_netdev_final_changes.md](aic8800d80_netif_netdev_final_changes.md) | **终版改动清单**（本文改动的最终形态提炼，照抄即可） |
| 本文件 | 补丁4：lwIP `netif_default` 缺失（跨网段 0ms 全失败）——**过程记录** |
