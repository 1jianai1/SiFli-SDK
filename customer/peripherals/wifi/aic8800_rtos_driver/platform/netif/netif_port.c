/*
 * Copyright (C) 2018-2024 AICSemi Ltd.
 *
 * All Rights Reserved
 */

//-------------------------------------------------------------------
// Driver Header Files
//-------------------------------------------------------------------
#include "co_math.h"
#include "netif_api.h"
#include "wifi_api.h"
#include "rtos_port.h"
#include "netif_port.h"
#include "aic_log.h"
#include "aic_assert.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "lwip/init.h"
/* [AIC8800-PORT][SiFli] 路线1 直接依赖的 lwIP 头: 基线经其他头传递包含,
 * SiFli lwIP-2.1.2 下不再传递; 对应选项 lwipopts.h 已开 (NETIF_API/ARP/DHCP=1) */
#include "lwip/netif.h"      /* struct netif, netif_linkoutput_fn */
#include "lwip/netifapi.h"   /* netifapi_netif_add, netifapi_dhcp_start/stop/release */
#include "lwip/etharp.h"     /* etharp_output, ETHARP_HWADDR_LEN */
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

#ifdef AIC8800_USE_RT_WLAN
    #include "wlan_dev.h"  /* rt_wlan_dev_report_data, struct rt_wlan_device */
    extern struct rt_wlan_device *aic8800_wlan_get_device(void);
#endif

/* host layer byte-order / inet utilities (aic_sock.c) */
extern int aic_inet_aton(const char *cp, void *addr);

#define NETIF_PKT_DHCP  0
#define NETIF_PKT_PING  1

/* net_if buffer */
#define NET_HEAD_OFFSET         (14)
#define ERR_OK                  (0)


#if NET_IF_UAP_NCPYBUF
    #define NET_TXBUF_CNT           (32*2)
#else
    #define NET_TXBUF_CNT           (28)
#endif
#define NET_TXBUF_TH            (NET_TXBUF_CNT/2-2)
#define NET_RX_LWIP_CNT         (40)

/**
 * @brief DHCP 报文跟踪
 *
 * 按协议逐层解析连续以太网帧 (ETH+IPv4+UDP+DHCP), 校验 magic cookie 后
 * 扫描 option 53 取 msg type, 打印 xid/yiaddr/chaddr。
 * 替代旧偏移硬编码实现 (帧长变化即失效)。
 *
 * @param data 以太网帧起始地址（须连续, 链式 pbuf 由调用方先行线性化/跳过）
 * @param len  帧长
 * @param tag  日志标签（如 "send"/"recv"）
 */
static void aicwf_dhcp_trace(uint8_t *f, uint32_t len, const char *tag)
{
    static const char *const type_str[] =
    {
        "", "DISCOVER", "OFFER", "REQUEST", "DECLINE",
        "ACK", "NAK", "RELEASE", "INFORM"
    };
    uint32_t ip, udp, dhcp, opt, xid, yiaddr;
    uint16_t dport;
    uint8_t ihl, msg_type = 0;

    if (f == NULL || len < 46)
        return;
    if (f[12] != 0x08 || f[13] != 0x00)         /* ETH_P_IP */
        return;
    ihl = (uint8_t)(f[14] & 0x0f) * 4;
    if (ihl < 20 || f[14 + 9] != 17)            /* proto != UDP */
        return;
    ip = 14;
    udp = ip + ihl;
    if (len < udp + 8 + 240)                    /* BOOTP 固定头 236 + cookie 4 */
        return;
    dport = (uint16_t)((f[udp + 2] << 8) | f[udp + 3]);
    if (dport != 67 && dport != 68)             /* DHCP server/client */
        return;
    dhcp = udp + 8;
    if (f[dhcp + 236] != 0x63 || f[dhcp + 237] != 0x82 ||
            f[dhcp + 238] != 0x53 || f[dhcp + 239] != 0x63)  /* magic cookie */
        return;
    xid = ((uint32_t)f[dhcp + 4] << 24) | ((uint32_t)f[dhcp + 5] << 16) |
          ((uint32_t)f[dhcp + 6] << 8) | f[dhcp + 7];
    yiaddr = ((uint32_t)f[dhcp + 16] << 24) | ((uint32_t)f[dhcp + 17] << 16) |
             ((uint32_t)f[dhcp + 18] << 8) | f[dhcp + 19];
    /* option 53 (msg type): 从 cookie 后扫描, 跳过 Pad/其他选项 */
    opt = dhcp + 240;
    while (opt + 1 < len)
    {
        if (f[opt] == 0)                        /* Pad */
        {
            opt++;
            continue;
        }
        if (f[opt] == 255)                      /* End */
            break;
        if (f[opt] == 53 && opt + 2 < len)
        {
            msg_type = f[opt + 2];
            break;
        }
        opt += 2 + f[opt + 1];
    }
    if (msg_type >= 1 && msg_type <= 8)
    {
        DBG_NETAL_INF("%s DHCP %s xid=0x%08x yiaddr=0x%08x chaddr=%02x:%02x:%02x:%02x:%02x:%02x\r\n",
                      tag, type_str[msg_type], xid, yiaddr,
                      f[dhcp + 28], f[dhcp + 29], f[dhcp + 30],
                      f[dhcp + 31], f[dhcp + 32], f[dhcp + 33]);
    }
}

//-------------------------------------------------------------------
// Driver Variables define
//-------------------------------------------------------------------
static bool volatile net_out_ing = false;
struct aic_netif_dev netif_dev = {0};

/*
 * Platform-internal netif pool.
 *
 * net_if_t (void*) 直接存储 struct netif*（指向 s_netif_pool 中的槽位）。
 * 不需要额外的 handle→slot 映射数组。
 * Index 0 = STA vif (default).
 *
 * 参数分配与类型转换规则:
 *  - netif_alloc_for_handle(&h): 检查 *p_h 是否已在 pool 范围内，
 *    不在则从 pool 分配空闲槽位，将槽位地址赋给 *p_h，返回该槽位。
 *  - netif_from_handle(h): 判断 h 是否落在 s_netif_pool 范围内，
 *    是则直接 (struct netif*)h 类型转换返回。
 *  - netif_handle_in_pool(h): 判断 h 是否落在 pool 范围内。
 */
#define PLAT_NETIF_POOL_SIZE  4
static struct netif s_netif_pool[PLAT_NETIF_POOL_SIZE];

/**
 * @brief 检查句柄是否指向 s_netif_pool 中的某个槽位
 *
 * 通过指针地址范围判断，无需遍历映射表。
 *
 * @param h 网络接口句柄 (实际是 struct netif*)
 * @return true=在 pool 范围内, false=不在
 */
static bool netif_handle_in_pool(net_if_t h)
{
    struct netif *nif = (struct netif *)h;
    return (nif >= &s_netif_pool[0] &&
            nif <  &s_netif_pool[PLAT_NETIF_POOL_SIZE]);
}

/**
 * @brief 类型转换: net_if_t → struct netif*
 *
 * 句柄本身就是 pool 中槽位指针，判断在范围内后直接转换。
 * 不在范围内返回 NULL。
 */
static struct netif *netif_from_handle(net_if_t h)
{
    if (netif_handle_in_pool(h))
        return (struct netif *)h;
    DBG_NETAL_ERR("%s: handle %p not in pool\r\n", __func__, h);
    return NULL;
}

/**
 * @brief 从 pool 中分配空闲槽位，将地址写入 *p_h
 *
 * 逻辑:
 *  1. 若 *p_h 已在 pool 范围内 → 已分配过，直接返回该槽位。
 *  2. 若不在 pool 内 → 查找空闲槽位 (内容为 0)，归零后将其地址赋给 *p_h。
 *
 * 分配在驱动初始化阶段单线程执行，无需锁保护。
 *
 * @param p_h  指向句柄变量的指针 (输入/输出: 成功时被赋值为槽位地址)
 * @return 分配的 netif 槽位指针，pool 满时返回 NULL
 */
static struct netif *netif_alloc_for_handle(net_if_t *p_h)
{
    int i;

    if (!p_h)
        return NULL;

    /* 1. *p_h 已在 pool 中 → 直接返回 */
    if (netif_handle_in_pool(*p_h))
        return (struct netif *)*p_h;

    /* 2. 分配空闲槽位 */
    for (i = 0; i < PLAT_NETIF_POOL_SIZE; i++)
    {
        /* 空闲槽位判断: 未被初始化 (num==0 且 output==NULL) */
        if (s_netif_pool[i].output == NULL)
        {
            memset(&s_netif_pool[i], 0, sizeof(struct netif));
            *p_h = (net_if_t)&s_netif_pool[i];
            return &s_netif_pool[i];
        }
    }

    DBG_NETAL_ERR("%s: pool full\n", __func__);
    return NULL;
}

//-------------------------------------------------------------------
// Driver Import functions
//-------------------------------------------------------------------
/* Platform functions */


//-------------------------------------------------------------------
// Driver platform functions
//-------------------------------------------------------------------
int aicwf_tx_eth_data_process(uint8_t *data, uint32_t len, void *msg);
int aicwf_tx_eth_data_process_if(net_if_t net_if, uint8_t *data, uint32_t len, void *msg);
/**
 * @brief 分配 RX 网络缓冲区（pbuf）
 *
 * 从 LwIP pbuf pool 中分配指定大小的原始数据包。
 *
 * @param length 所需缓冲区大小（字节）
 * @return pbuf 指针，失败返回 NULL
 */
void *aicwf_platform_net_buf_rx_alloc(uint32_t length)
{
    struct pbuf *p_buf = pbuf_alloc(PBUF_RAW, length, PBUF_POOL);

    if (p_buf)
    {
        return (void *)p_buf;
    }
    return NULL;
}

/**
 * @brief RX 零拷贝回退: 分配 pbuf 并拷贝以太网帧到其 payload
 *
 * host 层不持有 lwip 头, 由平台层完成 alloc + copy。返回 pbuf 指针
 * (与 aicwf_platform_net_buf_rx_alloc 同语义), 失败返回 NULL。
 *
 * @param eth_data 以太网帧起点
 * @param len      帧长
 * @return pbuf 指针, 失败返回 NULL
 */
void *aicwf_platform_net_buf_rx_alloc_copy(const uint8_t *eth_data, uint32_t len)
{
    struct pbuf *p_buf = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);

    if (p_buf == NULL)
    {
        DBG_NETAL_ERR("%s alloc fail len:%u\n", __func__, len);
        return NULL;
    }
    memcpy(p_buf->payload, eth_data, len);
    return (void *)p_buf;
}

/**
 * @brief 获取缓冲区前置描述符指针
 *
 * @param buffer 数据缓冲区地址
 * @return 描述符指针（buffer - sizeof(void*)）
 */
void *aicwf_platform_net_buf_rx_desc(uint8_t *buffer)
{
    return (void *)(buffer - sizeof(void *));
}

/**
 * @brief 释放 RX 网络缓冲区（pbuf）
 *
 * 注意: 只有在上游路径调用过 pbuf_ref() 增加引用计数后，
 *       此处才能调用 pbuf_free()。若上游未调用 pbuf_ref()，
 *       则 pbuf 所有权归属于 LwIP
 *
 * @param ref pbuf 指针
 */

void aicwf_platform_net_buf_tx_free(void *ref)
{
#if NET_IF_UAP_NCPYBUF
    /* nocopy: 发送时已 pbuf_ref()，此处释放驱动侧引用。
     * 仅对已入队 (pmsg) 的帧调用；丢弃/回退 copy 路径不得调用 */
    if (ref)
    {
        pbuf_free((struct pbuf *)ref);
    }
#else
    /* 无对应 pbuf_ref()，不可 free，避免双重释放 */
    // pbuf_free((struct pbuf *)ref);
    (void)ref;
#endif
}

#if NET_IF_UAP_NCPYBUF
/**
 * @brief nocopy TX 聚合阶段拷贝 hook（由 host 库 sdio_co.c 调用）
 *
 * 单段 pbuf: 直接 memcpy；链式 pbuf: 按段 scatter copy。
 * src 指向 [hostdesc(28B) + 去除以太网头的载荷] 的连续逻辑视图起点，
 * 其中 [src+14, src+28) 与首段 pbuf 的前 14B（以太网头）重叠，
 * fhost_tx 已在调用前将 hostdesc 写入该区域，故首段拷 len+14 即可。
 *
 * @param pmsg pbuf 链头指针
 * @param src  逻辑连续视图起点 (= 首段 payload - 14)
 * @param len  总长 (= tot_len + 14)
 * @param dst  聚合 buf 写入位置
 */
void aicwf_platform_nocopy_aggr_copy(void *pmsg, const uint8_t *src,
                                     uint32_t len, uint8_t *dst)
{
    struct pbuf *p = (struct pbuf *)pmsg;

    if (p->next == NULL)
    {
        /* 单段: 内存已连续 */
        memcpy(dst, src, len);
        return;
    }

    /* 链式: 首段 [src, src + p->len + 14) 覆盖 hostdesc + 首段全部数据 */
    uint32_t first_len = p->len + NET_HEAD_OFFSET;
    memcpy(dst, src, first_len);
    dst += first_len;

    struct pbuf *q;
    for (q = p->next; q != NULL; q = q->next)
    {
        memcpy(dst, q->payload, q->len);
        dst += q->len;
    }
}
#endif /* NET_IF_UAP_NCPYBUF */

#ifdef CONFIG_RX_NOCOPY
#if LWIP_SUPPORT_CUSTOM_PBUF
/* host 层 layer_common 符号(hal_co.c 实现, 平台层不直接 include hal_co.h) */
extern void aicwf_hal_buf_ref(void *node);
extern void aicwf_hal_buf_unref(void *node);

/* [PLATFORM] RX 零拷贝: custom pbuf 头小池。
 * 不写入 SDIO 共享缓冲区本体(控制区不可写), 独立静态池分配;
 * pbuf_custom 无 arg 字段, 用包装结构携带 node 指针。 */
#define AICWF_RX_ZCOPY_HDR_CNT  96   /* >= SDIO_RX_BUF_COUNT_DEFAULT, 避免早于 node 池耗尽 */

struct aicwf_rx_zcopy_hdr
{
    struct pbuf_custom pc;
    void *node;               /* 持有引用的 SDIO RX node */
    struct aicwf_rx_zcopy_hdr *next;  /* 空闲链 */
};

static struct aicwf_rx_zcopy_hdr rx_zcopy_hdr_pool[AICWF_RX_ZCOPY_HDR_CNT];
static struct aicwf_rx_zcopy_hdr *rx_zcopy_free_list;
static rtos_mutex rx_zcopy_mutex;
static uint8_t rx_zcopy_inited;
static uint32_t rx_zcopy_fallback_cnt;  /* 头池耗尽回退 copy 计数 */

static void aicwf_rx_zcopy_init(void)
{
    int i;

    if (rx_zcopy_inited)
    {
        return;
    }
    rx_zcopy_free_list = NULL;
    for (i = AICWF_RX_ZCOPY_HDR_CNT - 1; i >= 0; i--)
    {
        rx_zcopy_hdr_pool[i].next = rx_zcopy_free_list;
        rx_zcopy_free_list = &rx_zcopy_hdr_pool[i];
    }
    aicwf_rtos_mutex_create(&rx_zcopy_mutex, "rx_zcopy.mutex");
    rx_zcopy_inited = 1;
}

/* lwip 消费完 pbuf(ref 归零)时回调, 回收头 + 解除 node 引用。
 * 可能在 tcpip 线程/定时器/rx 任务任意上下文触发, unref 内部已加锁安全。 */
static void aicwf_rx_zcopy_pbuf_free(struct pbuf *p)
{
    struct aicwf_rx_zcopy_hdr *hdr = (struct aicwf_rx_zcopy_hdr *)p;

    aicwf_hal_buf_unref(hdr->node);

    aicwf_rtos_mutex_lock(rx_zcopy_mutex, -1);
    hdr->node = NULL;
    hdr->pc.custom_free_function = NULL;
    hdr->next = rx_zcopy_free_list;
    rx_zcopy_free_list = hdr;
    aicwf_rtos_mutex_unlock(rx_zcopy_mutex);
}

/**
 * @brief RX 零拷贝 hook: 建 custom pbuf 直指 SDIO 缓冲内以太网帧
 *
 * @param eth_data 帧起点(以太网头), 位于 SDIO RX node 缓冲内
 * @param len      帧长
 * @param node     帧所属 SDIO RX node(void*, USB 配置下可为 NULL → 回退)
 * @return pbuf 指针; 失败返回 NULL, 调用方回退时走 copy 路径
 */
void *aicwf_platform_rx_zero_copy(uint8_t *eth_data, uint32_t len, void *node)
{
    struct aicwf_rx_zcopy_hdr *hdr;
    struct pbuf *p;

    if (!rx_zcopy_inited)
    {
        aicwf_rx_zcopy_init();
    }
    if (!eth_data || !len || !node)
    {
        return NULL;
    }

    aicwf_rtos_mutex_lock(rx_zcopy_mutex, -1);
    hdr = rx_zcopy_free_list;
    if (hdr)
    {
        rx_zcopy_free_list = hdr->next;
    }
    aicwf_rtos_mutex_unlock(rx_zcopy_mutex);

    if (!hdr)
    {
        rx_zcopy_fallback_cnt++;
        if ((rx_zcopy_fallback_cnt & 0xFF) == 1)
        {
            DBG_NETAL_WRN("rx_zcopy hdr pool dry, fallback cnt=%u\n",
                          rx_zcopy_fallback_cnt);
        }
        return NULL;
    }

    hdr->node = node;
    hdr->pc.custom_free_function = aicwf_rx_zcopy_pbuf_free;
    /* PBUF_RAW: 无协议头偏移, payload 直指以太网头; len 不得超 u16 */
    p = pbuf_alloced_custom(PBUF_RAW, (u16_t)len, PBUF_RAM, &hdr->pc,
                            eth_data, (u16_t)len);
    if (p == NULL)
    {
        /* 参数异常(length>payload_mem_len 才会发生), 归还头并回退 */
        aicwf_rtos_mutex_lock(rx_zcopy_mutex, -1);
        hdr->node = NULL;
        hdr->pc.custom_free_function = NULL;
        hdr->next = rx_zcopy_free_list;
        rx_zcopy_free_list = hdr;
        aicwf_rtos_mutex_unlock(rx_zcopy_mutex);
        return NULL;
    }

    aicwf_hal_buf_ref(node);
    return (void *)p;
}

/**
 * @brief RX 零拷贝反初始化: 清理 custom pbuf 头池 + 互斥锁 + 状态复位
 *
 * 必须在 SDIO/USB node 池 free (aicwf_sdio_buf_deinit) 之前调用;
 * 正常时序下此时所有 custom pbuf 已由 lwip 消费完 (custom_free_fn 归还头),
 * 头池应完全回到 free_list。若仍有头被占用, 说明存在未消费 pbuf,
 * 此时 free node 池会导致 custom_free_fn 后续 unref 悬空 node → use-after-free。
 */
void aicwf_rx_zcopy_deinit(void)
{
    int free_cnt = 0;
    struct aicwf_rx_zcopy_hdr *h;

    if (!rx_zcopy_inited)
    {
        return;
    }

    /* 校验头池是否完全空闲: free_list 应含全部 AICWF_RX_ZCOPY_HDR_CNT 个头 */
    h = rx_zcopy_free_list;
    while (h)
    {
        free_cnt++;
        h = h->next;
    }
    if (free_cnt != AICWF_RX_ZCOPY_HDR_CNT)
    {
        DBG_NETAL_WRN("rx_zcopy deinit: hdr not all free (%d/%d), node free may UAF!\n",
                      free_cnt, AICWF_RX_ZCOPY_HDR_CNT);
    }

    aicwf_rtos_mutex_delete(rx_zcopy_mutex);
    rx_zcopy_free_list = NULL;
    rx_zcopy_inited = 0;
    rx_zcopy_fallback_cnt = 0;
}
#else /* !LWIP_SUPPORT_CUSTOM_PBUF: 平台 lwip 未开 custom pbuf, 恒回退 copy */
/* [PLATFORM] RX 零拷贝 hook 回退: 平台 lwip 未开启 LWIP_SUPPORT_CUSTOM_PBUF,
 * 无法建 custom pbuf, 恒返回 NULL 使 host 走 alloc+memcpy copy 路径。 */
void *aicwf_platform_rx_zero_copy(uint8_t *eth_data, uint32_t len, void *node)
{
    (void)eth_data;
    (void)len;
    (void)node;
    return NULL;
}

void aicwf_rx_zcopy_deinit(void)
{
}
#endif /* LWIP_SUPPORT_CUSTOM_PBUF */
#endif /* CONFIG_RX_NOCOPY */

void aicwf_platform_net_buf_rx_free(void *ref)
{
    pbuf_free((struct pbuf *)ref);
}

/**
 * @brief 将数据包传递给 LwIP 协议栈输入
 *
 * 通过 netif->input 回调将 pbuf 注入 LwIP。
 *
 * @param data  数据指针（未直接使用）
 * @param len   数据长度（未直接使用）
 * @param desc  pbuf 描述符
 * @param netif 网络接口句柄
 * @return ERR_OK(0)=成功, 其他=LwIP 错误码
 */
static int __aicwf_aicwifi_pkt_send_to_platform(uint8_t *data, uint32_t len, uint8_t *desc, net_if_t netif)
{
    struct pbuf *p_buf = (struct pbuf *)desc;
    struct netif *nif = netif_from_handle(netif);
    int errcode;

    //p_buf->payload = data;
    //p_buf->len = len;

#if LWIP_TCPIP_CORE_LOCKING
    /* [PLATFORM] RX 直连入栈路径在 fhost 任务上下文与 tcpip 线程并发,
     * LWIP_TCPIP_CORE_LOCKING 开启时必须持 core lock 进入协议栈
     * (详见 lwipopts.md) */
    LOCK_TCPIP_CORE();
#endif /* LWIP_TCPIP_CORE_LOCKING */
    errcode = nif->input(p_buf, nif);
#if LWIP_TCPIP_CORE_LOCKING
    UNLOCK_TCPIP_CORE();
#endif /* LWIP_TCPIP_CORE_LOCKING */
    if (errcode != ERR_OK)
    {
        DBG_NETAL_ERR("netif input failed, errcode = %d/n", errcode);
    }
    return errcode;
}

/**
 * @brief 零拷贝方式发送数据到 WiFi UAP
 *
 * 直接转发给 aicwf_tx_eth_data_process 处理。
 *
 * @param data 数据指针
 * @param len  数据长度
 * @param msg  关联的 pbuf 消息
 * @return 处理结果
 */
static unsigned long aicwf_netif_send_to_wifi_uap_nocpy(uint8_t *data, uint32_t len, void *msg)
{
    return aicwf_tx_eth_data_process(data, len, msg);
}

/**
 * @brief LwIP netif linkoutput 回调
 *
 * 被 LwIP ARP 层调用，将数据包发送到 WiFi 驱动。
 *
 * @param net_if 网络接口句柄
 * @param p_buf  要发送的 pbuf
 * @return 处理结果
 */
static err_t __aicwf_net_if_output(net_if_t net_if, struct pbuf *p_buf)
{
    /* 链式 pbuf（如 [头 pbuf]→[REF 载荷]）必须传 tot_len：
     * 首段 len 仅为头部长度，按首段定长会导致芯片按短 MSDU 发帧（帧截断） */
    /* net_if 透传: 并发模式下 SoftAP/P2P (vif2) 下行走各自接口 */
    return aicwf_tx_eth_data_process_if(net_if, p_buf->payload, p_buf->tot_len, p_buf);
}


//-------------------------------------------------------------------
// Driver netif base API define
//-------------------------------------------------------------------
/**
 * @brief 获取网络接口名称
 *
 * 类型转换: net_if_t → struct netif* (通过 netif_from_handle)。
 * 格式为 "wl" + 数字序号。
 *
 * @param net_if 网络接口句柄
 * @param buf    输出缓冲区
 * @param len    缓冲区大小
 * @return 名称长度（固定为3）
 */
int aicwf_net_if_get_name(net_if_t net_if, char *buf, int len)
{
    struct netif *nif;
    nif = netif_from_handle(net_if);
    if (!nif)
        return 0;
    if (len > 0)
        buf[0] = nif->name[0];
    if (len > 1)
        buf[1] = nif->name[1];
    if (len > 2)
        buf[2] = nif->num + '0';
    if (len > 3)
        buf[3] = '\0';

    return 3;
}

/**
 * @brief 检查网络接口是否已启动
 *
 * 类型转换: net_if_t → struct netif* (通过 netif_from_handle)。
 *
 * @param net_if 网络接口句柄
 * @return true=已启动, false=未启动或参数无效
 */
uint8_t aicwf_net_if_is_up(net_if_t net_if)
{
    struct netif *nif;
    if (!netif_handle_in_pool(net_if))
    {
        DBG_NETAL_ERR("%s: handle %p not in pool\r\n", __func__, net_if);
        return false;
    }
    nif = netif_from_handle(net_if);
    return netif_is_up(nif);
}

/**
 * @brief 启动网络接口
 *
 * 类型转换: net_if_t → struct netif* (通过 netif_from_handle)。
 *
 * @param net_if 网络接口句柄
 */
void aicwf_net_if_up(net_if_t net_if)
{
    struct netif *nif;
    if (!netif_handle_in_pool(net_if))
    {
        DBG_NETAL_ERR("%s: handle %p not in pool, skip\r\n", __func__, net_if);
        return;
    }
    nif = netif_from_handle(net_if);
    netif_set_up(nif);
}

/**
 * @brief 关闭网络接口
 *
 * 类型转换: net_if_t → struct netif* (通过 netif_from_handle)。
 *
 * @param net_if 网络接口句柄
 */
void aicwf_net_if_down(net_if_t net_if)
{
    struct netif *nif;
    int i;
    /* [BUGFIX] (共性: netif 生命周期) NULL 语义改为"下线 pool 全部活跃 netif";
     * 旧实现 NULL 走 not-in-pool 跳过, 等于没执行 */
    if (net_if == NULL)
    {
        for (i = 0; i < PLAT_NETIF_POOL_SIZE; i++)
        {
            if (s_netif_pool[i].output != NULL)
                netif_set_down(&s_netif_pool[i]);
        }
        return;
    }

    if (!netif_handle_in_pool(net_if))
    {
        DBG_NETAL_ERR("%s: handle %p not in pool, skip\r\n", __func__, net_if);
        return;
    }
    nif = netif_from_handle(net_if);
    netif_set_down(nif);
}

/**
 * @brief 设置默认网络接口
 *
 * 类型转换: net_if_t → struct netif* (通过 netif_from_handle)。
 *
 * @param net_if 网络接口句柄
 */
void aicwf_net_if_set_default(net_if_t net_if)
{
    if (!netif_handle_in_pool(net_if))
    {
        DBG_NETAL_ERR("%s: handle %p not in pool\r\n", __func__, net_if);
        return;
    }
    netif_set_default(netif_from_handle(net_if));
}

/**
 * @brief 设置网络接口 IP 地址、子网掩码和网关
 *
 * 类型转换: net_if_t → struct netif* (通过 netif_from_handle)。
 *
 * @param net_if 网络接口句柄
 * @param ip     IPv4 地址
 * @param mask   子网掩码
 * @param gw     网关地址
 */
void aicwf_net_if_set_ip(net_if_t net_if, uint32_t ip, uint32_t mask, uint32_t gw)
{
    struct netif *nif;

    if (!netif_handle_in_pool(net_if))
    {
        DBG_NETAL_ERR("%s: handle %p not in pool, skip\r\n", __func__, net_if);
        return;
    }

    nif = netif_from_handle(net_if);
    if (!nif)
        return;

    ip4_addr_t netif_ip;
    ip4_addr_t netif_mask;
    ip4_addr_t netif_gw;
    netif_ip.addr = ip;
    netif_mask.addr = mask;
    netif_gw.addr = gw;
    netif_set_addr(nif, &netif_ip, &netif_mask, &netif_gw);
}

/**
 * @brief 获取网络接口当前 IP 地址、子网掩码和网关
 *
 * 类型转换: net_if_t → struct netif* (通过 netif_from_handle)。
 *
 * @param net_if_h 网络接口句柄（NULL=默认接口）
 * @param ip       输出 IPv4 地址（可为 NULL）
 * @param mask     输出子网掩码（可为 NULL）
 * @param gw       输出网关地址（可为 NULL）
 * @return 0=成功, -1=失败
 */
int aicwf_net_if_get_ip(net_if_t net_if_h, uint32_t *ip, uint32_t *mask, uint32_t *gw)
{
    struct netif *net_if;

    /* [BUGFIX] (共性: netif 反查) NULL 表示默认接口: 按真实 STA vif(idx 0) 反查, 而非假定 pool[0]
     * (驱动重初始化/APSTA 下 AP 先启动时 pool[0] 不是 STA) */
    if (net_if_h == NULL)
    {
        net_if_h = net_if_find_from_wifi_idx(0);
        if (!net_if_h)
            return -1;
    }

    if (!netif_handle_in_pool(net_if_h))
    {
        DBG_NETAL_ERR("%s: handle %p not in pool\r\n", __func__, net_if_h);
        return -1;
    }

    net_if = netif_from_handle(net_if_h);
    if (!net_if)
    {
        DBG_NETAL_ERR("%s: netif_from_handle fail\r\n", __func__);
        return -1;
    }

    if (ip)
        *ip = net_if->ip_addr.addr;
    if (mask)
        *mask = net_if->netmask.addr;
    if (gw)
        *gw = net_if->gw.addr;

    return 0;
}

static err_t aicwf_net_if_init(struct netif *net_if);

/* [AIC8800-PORT][SiFli] RT-Thread netdev 桥接 (补注册)。
 * 只在"路线1: 驱动自建 netif"(AIC8800_USE_RT_WLAN 未开) 下需要:
 * AIC8800_USE_RT_WLAN 路线下 netif/netdev 均由 rt_wlan 框架创建并登记
 * (wlan_lwip.c + ethernetif.c::netdev_add), 驱动再注册会重名, 故整块排除。 */
#if defined(RT_USING_NETDEV) && !defined(AIC8800_USE_RT_WLAN)
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
     *   - 与本机同网段的目的地: ipv4/ip4.c:195 之前的 ip4_addr_netcmp 命中本
     *     netif -> 正常发出。所以 DHCP、ping 网关、到网关的 DNS 全部正常;
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

    if (netdev_register(nd, name, nif) != 0)
    {
        memset(nd, 0, sizeof(struct netdev));
        DBG_NETAL_ERR("%s: netdev_register(%s) fail\r\n", __func__, name);
        return -1;
    }

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
#endif /* RT_USING_NETDEV && !AIC8800_USE_RT_WLAN */

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

    if (net_if == NULL)
    {
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

/**
 * @brief 添加网络接口到 LwIP
 *
 * 通过 netif_alloc_for_handle 从 pool 分配槽位，将槽位地址写入 *net_if_h，
 * 再注册到 LwIP。
 *
 * @param net_if_h 指向网络接口句柄的指针 (输入/输出: 成功时被赋值为 pool 槽位地址)
 * @param ipaddr   IPv4 地址
 * @param netmask  子网掩码
 * @param gw       网关地址
 * @param vif      虚拟接口指针（fhost_vif）
 * @return 0=成功, -1=失败
 */
int aicwf_net_if_add(net_if_t *net_if_h, const uint32_t *ipaddr, const uint32_t *netmask, const uint32_t *gw, void *vif)
{
    err_t status;
    struct netif *net_if;

    if (!net_if_h)
    {
        DBG_NETAL_INF("%s net_if_h is NULL\r\n", __func__);
        return -1;
    }

    /* 1. 从 pool 分配槽位，*net_if_h 被赋值为槽位地址 */
    net_if = netif_alloc_for_handle(net_if_h);
    DBG_NETAL_INF("%s net_if=%p\r\n", __func__, net_if);
    if (!net_if)
    {
        DBG_NETAL_ERR("%s pool full\r\n", __func__);
        return -1;
    }

    /* 2. 检查是否已注册到 LwIP (output 在 init 回调中被赋值, 与 pool 空闲判断
     * 一致; 不能用 num 判断 — num 已完全交由 lwIP 分配, 不再作注册标志) */
    if (net_if->output != NULL)
    {
        DBG_NETAL_INF("%s already added to LwIP\r\n", __func__);
#ifdef RT_USING_NETDEV
        /* [AIC8800-PORT][SiFli] 重连场景走这条早退分支: netdev 可能已被
         * aicwf_net_if_release_all() 摘除后重建, 这里补一次镜像 (幂等) */
        aicwf_net_if_netdev_sync(net_if);
#endif
        return 0;
    }

    /* 3. 注册接口到 LwIP 协议栈 */
#ifndef AIC8800_USE_RT_WLAN
    status = netifapi_netif_add(net_if,
                                (const ip4_addr_t *)ipaddr,
                                (const ip4_addr_t *)netmask,
                                (const ip4_addr_t *)gw,
                                vif,
                                aicwf_net_if_init,
                                tcpip_input);
#else
    /* M2: AIC8800_USE_RT_WLAN 模式下, netif 由 rt_wlan 框架创建 (wlan_lwip.c),
     * 驱动跳过自建 netif; pool 槽位仍分配 (满足 host lib 内部假设) */
    status = ERR_OK;
#endif

    DBG_NETAL_INF("%s status=%d\r\n", __func__, status);

#ifndef AIC8800_USE_RT_WLAN
    /* [AIC8800-PORT][SiFli] 以下 netdev 补注册只适用于"路线1: 驱动自建 netif"。
     * AIC8800_USE_RT_WLAN 路线下 netif/netdev 均由 rt_wlan 框架创建并登记
     * (wlan_lwip.c + ethernetif.c::netdev_add), 驱动不得重复注册。 */
    if (status == ERR_OK)
    {
#ifdef RT_USING_NETDEV
        /* [AIC8800-PORT][SiFli] 补注册 RT-Thread netdev: 本 SDK 的 lwIP 把 DNS 存在
         * netdev 上, 且 dhcp.c/dns.c 会以 netif->name 反查 netdev, 缺失即断言 */
        aicwf_net_if_netdev_add(net_if);

        /* [AIC8800-PORT][SiFli] netdev 已登记 -> 立刻按 netif 的真实状态补齐镜像。
         * 关键的是 NETDEV_FLAG_LINK_UP: init 里直写 netif->flags 的写法不会经过
         * netif_set_link_up(), netdev 侧永远是 LINK_DOWN, netdev 版 ping 会被
         * "network interface device(wl) status error" 直接拒绝 (详见本函数实现处注释)。 */
        aicwf_net_if_netdev_sync(net_if);
#endif /* RT_USING_NETDEV */

        return 0;
    }

    return -1;
#else
    return (status == ERR_OK ? 0 : -1);
#endif /* AIC8800_USE_RT_WLAN */
}

/**
 * @brief LwIP netif 初始化回调
 *
 * 设置接口名称、MAC 地址、输出回调和标志位。
 *
 * @param net_if LwIP netif 结构
 * @return 0=成功
 */
static err_t aicwf_net_if_init(struct netif *net_if)
{
    //err_t status = ERR_OK;
    void *fvif;
    const uint8_t *vmac;

    /* netif_add() 已将 state 置为传入的 fhost_vif; 仅当为空时回退 vif[0] */
    fvif = net_if->state;
    if (!fvif)
        fvif = aic_get_fhost_default_vif();
    net_if->state = fvif;

#if LWIP_NETIF_HOSTNAME
    /* Initialize interface hostname */
    net_if->hostname = "AicWlan";
#endif

    net_if->name[ 0 ] = 'w';
    net_if->name[ 1 ] = 'l';

    /* lwIP 版本开机可见 (双 VIF 只打一次): nocopy TX 类型检查分支随版本切换,
     * >=2.1 走 type_internal 位检查, <=2.0/1.x 走 type 枚举检查 */
    {
        static bool lwip_ver_logged = false;
        if (!lwip_ver_logged)
        {
            lwip_ver_logged = true;
            DBG_NETAL_INF("lwip version: %s\n", LWIP_VERSION_STRING);
        }
    }

    net_if->output = etharp_output;
    net_if->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_IGMP;
    net_if->hwaddr_len = ETHARP_HWADDR_LEN;
    net_if->mtu = 1500;
    /* [AIC8800-PORT][SiFli] __aicwf_net_if_output 首参为 net_if_t(void*), lwIP
     * linkoutput 首参为 struct netif*; 运行时同值(句柄透传 netif_from_handle),
     * GCC14 将不兼容指针赋值提升为 error, 故显式 cast */
    net_if->linkoutput = (netif_linkoutput_fn)__aicwf_net_if_output;

    /* [BUGFIX] (共性: MAC 单一来源) MAC 单一来源为 fhost_vif->mac_addr (由启动路径
     * set_mac 写入: STA=base, AP/P2P=base+vif_idx), 此处仅镜像同步到
     * netif->hwaddr, 不再重复计算; 异常兜底: set_mac 未先行调用时回退 base,
     * 保证 hwaddr 非零 */
    vmac = aic_fhost_vif_get_mac(fvif);
    if ((vmac[0] | vmac[1] | vmac[2] | vmac[3] | vmac[4] | vmac[5]) == 0)
    {
        aic_fhost_vif_set_mac(fvif, aic_get_mac_address());
        vmac = aic_fhost_vif_get_mac(fvif);
    }
    memcpy(net_if->hwaddr, vmac, ETHARP_HWADDR_LEN);


    /* don't set NETIF_FLAG_ETHARP if this device is not an ethernet one */
    net_if->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_IGMP;
    //net_if->flags = NETIF_FLAG_IGMP | NETIF_FLAG_ETHERNET | NETIF_FLAG_ETHARP;
    /* [BUGFIX] (共性: 接口名唯一) 不覆盖 net_if->num: lwIP netif_add 分配全局唯一
     * num (名字 wl<num>), 接口名唯一是 fhost_vif_idx_from_name() 按名反查 vif
     * 的前提; 曾按 vif_idx 覆盖导致 STA/AP 并发时两个 netif 同名 "wl2",
     * AP 的 WPA iface 被绑到 STA 的 vif0 (concurrent add_iface failed) */

    return 0;
}

/**
 * @brief 刷新 netif 硬件地址镜像（MAC 单一来源）
 *
 * set_mac 之后 netif 已存在时调用本函数刷新镜像 (首次 add 由 init 同步);
 * 保证 lwip ARP sender MAC / 以太网帧源 MAC 与固件 vif 收发 MAC 一致。
 */
void aicwf_net_if_update_mac(net_if_t net_if)
{
    struct netif *nif = netif_from_handle(net_if);
    if (!nif || !nif->state)
        return;
    memcpy(nif->hwaddr, aic_fhost_vif_get_mac(nif->state), ETHARP_HWADDR_LEN);
}

/**
 * @brief 回收全部 pool netif (驱动反初始化时调用)
 *
 * [BUGFIX] (共性: netif 生命周期) 修复 netif 只增不减导致的槽位泄漏: 旧实现在驱动重初始化
 * (fhost_init 将 net_if_handle 清 NULL) 后无任何 netif_remove, 旧 netif
 * 永久占据 netif_list 且 pool 槽 output!=NULL 不可复用, 名字漂移
 * (wl0→wl1→...) 且 4 次重初始化后 pool 耗尽。
 * fhost_deinit 中调用: 逐槽下线/移出 lwIP, 槽位清零回池 (output=NULL)。
 */
void aicwf_net_if_release_all(void)
{
    int i;

    for (i = 0; i < PLAT_NETIF_POOL_SIZE; i++)
    {
        struct netif *nif = &s_netif_pool[i];
        if (nif->output == NULL)
            continue;
        DBG_NETAL_INF("%s: remove netif %c%c%d\n", __func__,
                      nif->name[0], nif->name[1], nif->num);
#if defined(RT_USING_NETDEV) && !defined(AIC8800_USE_RT_WLAN)
        /* [AIC8800-PORT][SiFli] 先摘 netdev: 否则驱动重初始化时同一槽位会带旧登记二次入链 */
        aicwf_net_if_netdev_del(nif);
#endif
        netif_set_down(nif);
        netif_remove(nif);
        /* 槽位回池: netif_remove 不清 output, 需整体清零以恢复可分配 */
        memset(nif, 0, sizeof(struct netif));
    }
}

/**
 * @brief 初始化网络子系统
 *
 * 创建 L2 信号量/互斥量、TX 缓冲区池和 RX 信号量。
 * 支持多次调用，已初始化时直接返回。
 *
 * @return 0=成功
 */
int aicwf_net_init(void)
{
    uint8_t i;
    struct aic_netif_dev *netifdev = &netif_dev;

    if (netifdev->net_inited)
    {
        return 0;
    }

    if (aicwf_rtos_semaphore_create(&netifdev->l2_semaphore, "l2_semaphore", 1, 0))
    {
        AIC_ASSERT_ERR(0);
    }

    if (aicwf_rtos_mutex_create(&netifdev->l2_mutex, "l2_mutex"))
    {
        AIC_ASSERT_ERR(0);
    }

#ifdef CONFIG_RX_WAIT_LWIP
    if (aicwf_rtos_semaphore_create(&netifdev->fhost_rx_lwip_sema, "fhost_rx_lwip_sema", NET_RX_LWIP_CNT, 0))
    {
        AIC_ASSERT_ERR(0);
    }
#endif /* CONFIG_RX_WAIT_LWIP */

    // Initial free tx buf
    co_list_init(&netifdev->net_tx_buf_free_list);
    if (aicwf_rtos_semaphore_create(&netifdev->net_tx_buf_sema, "net_tx_buf_sema", NET_TXBUF_CNT, 0))
    {
        AIC_ASSERT_ERR(0);
    }

    netifdev->net_tx_buf_cnt = 0;
    for (i = 0; i < NET_TXBUF_CNT; i++)
    {
        struct net_tx_buf_tag *net_tx_buffer = aicwf_rtos_malloc(sizeof(struct net_tx_buf_tag));
        if (net_tx_buffer)
        {
            //AIC_LOG_PRINTF("%s net_buf %d %x\n", __func__, i, (net_tx_buffer));
            co_list_push_back(&netifdev->net_tx_buf_free_list, (struct co_list_hdr *)(net_tx_buffer));
            netifdev->net_tx_buf_cnt++;
            aicwf_rtos_semaphore_signal(netifdev->net_tx_buf_sema, 0);
        }
    }

    DBG_NETAL_INF("net_tx_buf_sema initial count:%d\n", aicwf_rtos_semaphore_get_count(netifdev->net_tx_buf_sema));
    if (aicwf_rtos_mutex_create(&netifdev->net_tx_buf_mutex, "net_tx_buf_mutex"))
    {
        AIC_ASSERT_ERR(0);
    }

    netifdev->net_inited = true;

    return 0;
}

/**
 * @brief 反初始化网络子系统
 *
 * 释放 TX 缓冲区池，删除所有信号量和互斥量，关闭网络接口。
 */
void aicwf_net_deinit(void)
{
    //uint8_t i;
    //struct net_tx_buf_tag *tx_buf;
    struct aic_netif_dev *netifdev = &netif_dev;

#ifndef CONFIG_DRIVER_ORM
    for (i = 0; i < NET_TXBUF_CNT; i++)
    {
        aicwf_rtos_mutex_lock(netifdev->net_tx_buf_mutex, -1);
        tx_buf = (struct net_tx_buf_tag *)co_list_pop_front(&netifdev->net_tx_buf_free_list);
        netifdev->net_tx_buf_cnt--;
        aicwf_rtos_mutex_unlock(netifdev->net_tx_buf_mutex);
        aicwf_rtos_free(tx_buf);
    }
#endif

    aicwf_rtos_semaphore_delete(netifdev->net_tx_buf_sema);
    aicwf_rtos_mutex_delete(netifdev->net_tx_buf_mutex);
    aicwf_rtos_semaphore_delete(netifdev->l2_semaphore);
    aicwf_rtos_mutex_delete(netifdev->l2_mutex);
#ifdef CONFIG_RX_WAIT_LWIP
    aicwf_rtos_semaphore_delete(netifdev->fhost_rx_lwip_sema);
#endif /* CONFIG_RX_WAIT_LWIP */

    RTOS_RES_NULL(netifdev->net_tx_buf_sema);
    RTOS_RES_NULL(netifdev->net_tx_buf_mutex);
    RTOS_RES_NULL(netifdev->l2_semaphore);
    RTOS_RES_NULL(netifdev->l2_mutex);
#ifdef CONFIG_RX_WAIT_LWIP
    RTOS_RES_NULL(netifdev->fhost_rx_lwip_sema);
#endif /* CONFIG_RX_WAIT_LWIP */

    memset(netifdev->l2_filter, 0, sizeof(netifdev->l2_filter));
    aicwf_net_if_down(NULL);
    netifdev->net_inited = false;
}

/**
 * @brief 网络子系统预反初始化
 *
 * 等待发送完成后关闭网络接口，为正式反初始化做准备。
 */
void aicwf_net_predeinit(void)
{
    netif_dev.net_inited = false;
    DBG_NETAL_INF("[netif] down and preinit enter\n");
    while (net_out_ing)
    {
        aicwf_rtos_msleep(1);
    }

    //down wifi netif
    aicwf_net_if_down(NULL);
    DBG_NETAL_INF("[netif] down and preinit exit\n");
}

/**
 * @brief 分配 TX 网络缓冲区
 *
 * 分配内存并复制 payload 数据，支持 SDIO ADMA 对齐。
 *
 * @param payload 数据指针
 * @param length  数据长度
 * @return net_buf_tx_t 指针，失败返回 NULL
 */
net_buf_tx_t *aicwf_net_buf_tx_alloc(const uint8_t *payload, uint32_t length)
{
    unsigned int reserved_len = aic_offsetof();
    uint8_t ext_len = 0, offset = 0;
#ifdef CONFIG_SDIO_ADMA
    ext_len = 8 + 64;
    offset = 4 + 32;
#endif
    void *obuf = aicwf_rtos_malloc(CO_ALIGN4_HI(sizeof(net_buf_tx_t)) + CO_ALIGN4_HI(length + reserved_len) + ext_len);
    if (!obuf)
    {
        DBG_NETAL_ERR("%s tx buffer null\n", __func__);
        return NULL;
    }
#ifdef  CONFIG_SDIO_ADMA
    void *nbuf = (void *)CO_ALIGN4_HI((uint32_t)obuf);
#else
    void *nbuf = obuf;
#endif
    net_buf_tx_t *buf = (net_buf_tx_t *)nbuf;
    memset(buf, 0, (sizeof(net_buf_tx_t)));
    uint8_t *payload_buf = (uint8_t *)buf + CO_ALIGN4_HI(sizeof(net_buf_tx_t)) + offset;
    memcpy((payload_buf + reserved_len), payload, length);
    buf->data_ptr = (payload_buf + reserved_len);
    buf->data_len = length;
    buf->pkt_type = TCPIP_PKTTYPE_MAX;
#ifdef  CONFIG_SDIO_ADMA
    buf->data_oprt = obuf;
#endif
    //AIC_LOG_PRINTF("%s tcpip %p _buf %p %p\n", __func__, buf, payload_buf, buf->data_ptr);

    return buf;
}

/**
 * @brief 释放 TX 网络缓冲区
 *
 * 根据包类型决定释放方式：TCPIP 包直接 free，ETHER 包回收到空闲列表。
 *
 * @param buf TX 缓冲区指针
 */
void aicwf_net_buf_tx_free(net_buf_tx_t *buf)
{
    struct aic_netif_dev *netifdev = &netif_dev;

    if (!buf)
    {
        DBG_NETAL_ERR("Err tx buf is null\n");
        return ;
    }

    if (TCPIP_PKTTYPE_MAX == buf->pkt_type)
    {
        //AIC_LOG_PRINTF("nbtf:%p/%p\n", buf, buf->data_ptr);
#ifdef CONFIG_SDIO_ADMA
        aicwf_rtos_free(buf->data_oprt);
#else
        aicwf_rtos_free(buf);
#endif
        buf = NULL;
    }
    else if (TCPIP_PKTTYPE_ETHER == buf->pkt_type)
    {
        //buf->data_ptr -= (SDIO_HOSTDESC_SIZE);
        //buf->data_ptr -= sizeof(struct co_list_hdr);
        uint8_t *list_hdr = (uint8_t *)buf;
        list_hdr -= sizeof(struct co_list_hdr);
        //AIC_LOG_PRINTF("%s buf %p, net_buf %p\n", __func__, list_hdr, buf->data_ptr);
#if NET_IF_UAP_NCPYBUF
        /* [PLATFORM] nocopy TX 释放: pmsg==buf → 回退线性化堆缓冲 (rtos_free);
         * pmsg!=buf → pmsg 为已 ref 的 pbuf (pbuf_free) */
        {
            struct net_tx_buf_tag *tag = (struct net_tx_buf_tag *)list_hdr;
            if (tag->pmsg != NULL)
            {
                if (tag->pmsg == tag->buf)
                {
                    aicwf_rtos_free(tag->pmsg);
                }
                else
                {
                    aicwf_platform_net_buf_tx_free(tag->pmsg);
                }
                tag->pmsg = NULL;
            }
        }
#endif
        aicwf_rtos_mutex_lock(netifdev->net_tx_buf_mutex, -1);
        co_list_push_back(&netifdev->net_tx_buf_free_list, (struct co_list_hdr *)(list_hdr));
        netifdev->net_tx_buf_cnt++;
        aicwf_rtos_mutex_unlock(netifdev->net_tx_buf_mutex);
        aicwf_rtos_semaphore_signal(netifdev->net_tx_buf_sema, 0);

    }
    else
    {
        DBG_NETAL_INF("%s free what? net_buf %p %d\n", __func__, buf->data_ptr, buf->pkt_type);
    }
}

/**
 * @brief 获取 TX 空闲缓冲区数量
 *
 * @return 空闲缓冲区个数
 */
static uint32_t __aicwf_net_buf_tx_free_list_cnt(void)
{
    struct aic_netif_dev *netifdev = &netif_dev;
#if 1
    aicwf_rtos_mutex_lock(netifdev->net_tx_buf_mutex, -1);
    uint32_t cnt = co_list_cnt(&netifdev->net_tx_buf_free_list);
    aicwf_rtos_mutex_unlock(netifdev->net_tx_buf_mutex);
    return cnt;
#else
    return netifdev->net_tx_buf_cnt;
#endif
}

/**
 * @brief 检查 TX 空闲缓冲区是否不足
 *
 * 当空闲数 <= 阈值时返回 true。
 *
 * @return true=不足, false=充足
 */
bool aicwf_net_buf_tx_free_list_is_full(void)
{
    uint32_t cnt = __aicwf_net_buf_tx_free_list_cnt();

    return (cnt <= NET_TXBUF_TH);
}

/**
 * @brief 分配 RX 数据缓冲区
 *
 * 分配 pbuf 并将其记录到资源追踪列表。
 *
 * @param length 所需缓冲区大小
 * @return pbuf 指针，失败返回 NULL
 */
uint8_t *aicwf_net_buf_rx_alloc(uint32_t length)
{
    void *pbuf = aicwf_platform_net_buf_rx_alloc(length);
    void *pmsg;

    if (pbuf == NULL)
    {
        DBG_NETAL_ERR("%s alloc tx_buf failed\n", __func__);

        return NULL;
    }

    pmsg = pbuf;//aicwf_platform_net_buf_rx_desc(pbuf);

    return pbuf;
}

/**
 * @brief 释放 RX 数据缓冲区
 *
 * 释放 pbuf 并从资源追踪列表中移除。
 *
 * @param msg pbuf 消息指针
 */
void aicwf_net_buf_rx_free(void *msg)
{
    aicwf_platform_net_buf_rx_free(msg);
}


/* Internal copy core: platform copies data into allocated pbuf */
/**
 * @brief 处理接收到的以太网数据（拷贝版本）
 *
 * 分配新的 pbuf 并复制数据后传递给 LwIP。
 *
 * @param pdata 接收数据指针
 * @param len   数据长度
 * @param netif 网络接口句柄
 * @return 0=成功, 负数=失败
 */
static int aicwf_rx_eth_data_process_copy(unsigned char *pdata,
        unsigned short len, net_if_t netif)
{
    int ret;
    void *msg;
    unsigned char *pmem = NULL;

    /* 并发/单模式统一 per-vif 门控: 该 netif 绑定的 vif 就绪即放行,
     * 与全局模式无关 (APSTA 中一侧断开不影响另一侧) */
    if (!aic_net_if_vif_ready(netif))
    {
        DBG_NETAL_INF("rx:vif not ready\n");
        goto exit;
    }

    if (len == 0)
    {
        DBG_NETAL_INF("lwip_wifi_in_buf len 0\n");
        goto exit;
    }

#if NETIF_PKT_DHCP
    aicwf_dhcp_trace(pdata, len, "netif-in");

    uint8_t *rdata = (uint8_t *)pdata;
    if (rdata[35] == 0x44 && rdata[37] == 0x43)
    {
        DBG_NETAL_INF("DHCP:");
        aicwf_dump_buffer("dhcp_payload", (uint8_t *)rdata, len);
    }
#endif

#if NETIF_PKT_PING
#if !NETIF_PKT_DHCP
    uint8_t *rdata = (uint8_t *)pdata;  /* DHCP 块已声明则复用, 避免同作用域重复声明编译失败 */
#endif
    if (rdata[12] == 0x08 && rdata[13] == 0x00 && rdata[34] == 0x08 && rdata[35] == 0x00)
    {
        DBG_NETAL_INF("NET-IN  ICMP: %d[%d]\n", (rdata[40] << 8) | rdata[41], aicwf_rtos_now(0));
        //aicwf_dump_buffer("rx_payload", (uint8_t *)rdata, len);
    }
#endif

    {
        //no rorder net buf don't push res list
        //pmsg 16 ||wifi extend 108||mac eth head || IP data ||....
        msg = aicwf_platform_net_buf_rx_alloc(len + AICWF_WIFI_HDR);
        if (msg == NULL)
        {
            DBG_NETAL_ERR("lwip_wifi_in_buf_alloc fail\n");

            return -2;
        }

        pmem = (uint8_t *)(((struct pbuf *)msg)->payload);

        if (pmem == NULL)
        {
            DBG_NETAL_INF("lwip_wifi_in_buf pmsg is null\n");
            return -3;
        }

        memcpy(pmem + AICWF_WIFI_HDR, pdata, len);
        ret = __aicwf_aicwifi_pkt_send_to_platform(pmem + AICWF_WIFI_HDR, len, msg, netif);

        //ret !0 free buf wifi module  else lwip
        if (ret != 0)
        {
            DBG_NETAL_ERR("lwip_wifi input fail2 ret:%d len:%d\n!!!!", ret, len);
            aicwf_platform_net_buf_rx_free(msg);
        }

    }
    return ret;

exit:
    return -1;
}

/**
 * @brief RX 数据公共入口
 *
 * 5 参数入口。CONFIG_RX_NOCOPY 下: reorder 直通路径(is_reord)与零拷贝路径
 * (lwip_msg 为 custom pbuf, payload 直指 pdata) 直接入栈; 否则走 alloc+copy。
 *
 * @param pdata    数据指针
 * @param len      数据长度
 * @param netif    网络接口句柄
 * @param lwip_msg pbuf; 零拷贝/reorder 直通路径非空
 * @param is_reord reorder 帧标志
 * @return 处理结果
 */
int aicwf_rx_eth_data_process(uint8_t *pdata, uint16_t len, net_if_t netif,
                              void *lwip_msg, uint8_t is_reord)
{
#ifdef AIC8800_USE_RT_WLAN
    /* M2: RX 桥接到 rt_wlan 框架 (框架持有 netif, 驱动只投递裸帧) */
    (void)netif;
    (void)lwip_msg;
    (void)is_reord;
    rt_wlan_dev_report_data(aic8800_wlan_get_device(), pdata, (int)len);
    return 0;
#else
#ifdef CONFIG_RX_NOCOPY
    /* reorder 直通 / 零拷贝: lwip_msg 已建好(payload 直指 pdata),
     * 入栈失败时 pbuf_free 触发 custom_free_fn 归还头 + 解除 node 引用 */
    if (is_reord || lwip_msg)
    {
        int ret = __aicwf_aicwifi_pkt_send_to_platform(pdata, len, lwip_msg, netif);
        if (ret != 0)
        {
            DBG_NETAL_ERR("lwip_wifi zcopy input fail ret:%d len:%d\n", ret, len);
            aicwf_platform_net_buf_rx_free(lwip_msg);
        }
        return ret;
    }
#endif /* CONFIG_RX_NOCOPY */
    return aicwf_rx_eth_data_process_copy((unsigned char *)pdata, len, netif);
#endif /* AIC8800_USE_RT_WLAN */
}

#ifdef CONFIG_FILTER_TCP_ACK
#define MAX_TCP_ACK             80
/**
 * @brief 发送以太网数据（带 TCP ACK 过滤）
 *
 * 从 TX 缓冲区池分配缓冲区，复制数据并通过 WiFi 驱动发送。
 * 启用 CONFIG_FILTER_TCP_ACK 时过滤小包 TCP ACK。
 *
 * @param data 数据指针
 * @param len  数据长度
 * @param msg  关联的 pbuf 消息
 * @return 0=成功
 */
int aicwf_tx_eth_data_no_tcpack_process(UINT8 *data, UINT32 len, void *msg)
{
    net_buf_tx_t *sent_pkt;
    void *fhost_vif;
    net_if_t net_if = aic_get_vif_net_if();
    struct aic_netif_dev *netifdev = &netif_dev;

    //dwmp data for test
    //rwnx_data_dump("__aicwf_net_if_output", data, len);

    /* 并发/单模式统一 per-vif 门控 (同 RX 路径) */
    if (!aic_net_if_vif_ready(net_if))
    {
        DBG_NETAL_INF("tx:vif not ready\n");
        goto exit;
    }

    net_out_ing = 1;
    do
    {
        unsigned int reserved_len = aic_offsetof();//offsetof(struct hostdesc, cfm_cb);

#ifndef CONFIG_RX_WAIT_LWIP
#ifndef PROTECT_TX_ADMA
        int ret = aicwf_rtos_semaphore_wait(netifdev->net_tx_buf_sema, 100);
#else
        int ret = aicwf_rtos_semaphore_wait(netifdev->net_tx_buf_sema, 200);
#endif /* CONFIG_ADMA_PROTECT */
#else
        int ret = aicwf_rtos_semaphore_wait(netifdev->net_tx_buf_sema, 200);
#endif /* CONFIG_RX_WAIT_LWIP */

        struct net_tx_buf_tag *tx_buf = NULL;

        if (ret == 0)
        {
            aicwf_rtos_mutex_lock(netifdev->net_tx_buf_mutex, -1);
            tx_buf = (struct net_tx_buf_tag *)co_list_pop_front(&netifdev->net_tx_buf_free_list);
            netifdev->net_tx_buf_cnt--;
            aicwf_rtos_mutex_unlock(netifdev->net_tx_buf_mutex);
        }

        if (!tx_buf)
        {
            DBG_NETAL_INF("lwip_wifi_out_buf:tx full\n");
            /* nocopy: 此时尚未 pbuf_ref，所有权归 LwIP，不可 free */
#if !NET_IF_UAP_NCPYBUF
            aicwf_platform_net_buf_tx_free(msg);
#endif
            goto end;
        }

#if NET_IF_UAP_NCPYBUF
        /* [PLATFORM] nocopy TX: headroom 足够且链首段含以太网头时直接引用 pbuf；
         * 否则回退 copy。pmsg/frag_next 携带 pbuf 链指针到 SDIO 聚合 scatter copy。
         * 判定基于实际写 hostdesc 的位置 data（须与 pbuf 内存重叠）；
         * ADMA/USB 总线无 scatter hook（pkt_data 须连续），链式 pbuf 强制回退线性化。
         * 类型检查: PBUF_REF/ROM 的 payload 指向外部内存, [payload-14, payload)
         * 并非 pbuf 自有 headroom, 地址比较通过时 hostdesc 写入会越界破坏外部
         * buffer 之前的内存, 故链头仅 RAM/POOL 允许 nocopy;
         * 地址比较为单边检查（只验证不早于结构体末尾，不验证上界）；
         * 链式另需首段 len >= NET_HEAD_OFFSET: hostdesc 下半 [payload, payload+14)
         * 覆盖写首段以太网头区, 不得越过首段 payload 末尾 */
        struct pbuf *p_ref = (struct pbuf *)msg;
#if defined(PBUF_TYPE_FLAG_STRUCT_DATA_CONTIGUOUS)
        bool pbuf_own_buf = (p_ref->type_internal & PBUF_TYPE_FLAG_STRUCT_DATA_CONTIGUOUS) != 0;
#else
        bool pbuf_own_buf = true; /* lwip 1.x 无类型标志, 维持旧行为 */
#endif
#if defined(CONFIG_SDIO_ADMA) || defined(CONFIG_USB_SUPPORT)
        bool nocopy_ok = pbuf_own_buf
                         && (((uint8_t *)data - reserved_len)
                             >= ((uint8_t *)p_ref + sizeof(struct pbuf)))
                         && (p_ref->next == NULL);
#else
        bool nocopy_ok = pbuf_own_buf
                         && (((uint8_t *)data - reserved_len)
                             >= ((uint8_t *)p_ref + sizeof(struct pbuf)))
                         && ((p_ref->next == NULL) || (p_ref->len >= NET_HEAD_OFFSET));
#endif
        if (nocopy_ok)
        {
            pbuf_ref(p_ref);
            tx_buf->buf = data - reserved_len;
            tx_buf->pmsg = msg;
            /* 链式帧长度自洽：统一按全链长度，不依赖调用方传值 */
            len = p_ref->tot_len;
        }
        else
        {
            /* 回退线性化: nocopy 模式下 net_tx_buf_tag 无内联 buf，按需 malloc；
             * 标记 tag->buf=NULL + pmsg=堆指针，释放路径据此 free。
             * pbuf 所有权归 LwIP，不调 tx_free */
            uint32_t tot = p_ref->tot_len;
            tx_buf->buf = aicwf_rtos_malloc(reserved_len + tot);
            if (!tx_buf->buf)
            {
                DBG_NETAL_INF("lwip_wifi_out_buf:fb alloc fail\n");
                aicwf_rtos_mutex_lock(netifdev->net_tx_buf_mutex, -1);
                co_list_push_back(&netifdev->net_tx_buf_free_list, (struct co_list_hdr *)tx_buf);
                netifdev->net_tx_buf_cnt++;
                aicwf_rtos_mutex_unlock(netifdev->net_tx_buf_mutex);
                aicwf_rtos_semaphore_signal(netifdev->net_tx_buf_sema, 0);
                goto end;
            }
            tx_buf->pmsg = tx_buf->buf;
            pbuf_copy_partial(p_ref, tx_buf->buf + reserved_len, tot, 0);
            len = tot;
        }
#else
        memcpy((tx_buf->buf + reserved_len), data, len);
        aicwf_platform_net_buf_tx_free(msg);
#endif

        sent_pkt = &(tx_buf->pkt_hdr);
#if NET_IF_UAP_NCPYBUF
        sent_pkt->data_ptr = tx_buf->buf + reserved_len;
        sent_pkt->frag_next = nocopy_ok ? msg : NULL;
#else
        sent_pkt->data_ptr = tx_buf->buf + reserved_len;
        sent_pkt->frag_next = NULL;
#endif
        sent_pkt->data_len = len;
        sent_pkt->net_id   = 0;
        sent_pkt->pkt_type = TCPIP_PKTTYPE_ETHER;

#if NETIF_PKT_DHCP
        aicwf_dhcp_trace(data, len, "netif-out");
#endif

#if NETIF_PKT_PING
        uint8_t *rdata = (uint8_t *)sent_pkt->data_ptr;
        if (rdata[12] == 0x08 && rdata[13] == 0x00 && rdata[34] == 0x00 && rdata[35] == 0x00)
        {
            DBG_NETAL_INF("NET-OUT ICMP: %d", (rdata[40] << 8) | rdata[41]);
            aicwf_dump_buffer("tx_payload", rdata, len);
        }
#endif

        aic_tx_start(net_if, sent_pkt, NULL, NULL);

    }
    while (0);

end:
    net_out_ing = 0;
    return 0;

exit:
    /* nocopy: 丢弃路径未 pbuf_ref，所有权归 LwIP，不可 free */
#if !NET_IF_UAP_NCPYBUF
    aicwf_platform_net_buf_tx_free(msg);
#endif
    return 0;
}
#endif

/**
 * @brief 发送以太网数据到 WiFi 驱动（默认 vif0 接口）
 *
 * 兼容入口: 无 netif 上下文的调用方 (如 tcp_ack 重注入) 走 vif0。
 *
 * @param data 数据指针
 * @param len  数据长度
 * @param msg  关联的 pbuf 消息
 * @return 0=成功
 */
int aicwf_tx_eth_data_process(uint8_t *data, uint32_t len, void *msg)
{
    return aicwf_tx_eth_data_process_if(aic_get_vif_net_if(), data, len, msg);
}

/**
 * @brief 发送以太网数据到 WiFi 驱动（指定网络接口）
 *
 * 从 TX 缓冲区池分配缓冲区，复制数据并通过 aic_tx_start 发送。
 * net_if 透传自 lwIP linkoutput 回调 (per-vif), 并发模式下
 * SoftAP/P2P (vif2) 下行数据走各自接口而非恒 vif0。
 * 支持 STA/AP/P2P/APSTA/APP2P 模式检查和 TCP ACK 过滤。
 *
 * @param net_if 网络接口句柄 (NULL 回退 vif0)
 * @param data 数据指针
 * @param len  数据长度
 * @param msg  关联的 pbuf 消息
 * @return 0=成功
 */
int aicwf_tx_eth_data_process_if(net_if_t net_if, uint8_t *data, uint32_t len, void *msg)
{
    net_buf_tx_t *sent_pkt;
    //void *fhost_vif;
    struct aic_netif_dev *netifdev = &netif_dev;

    if (net_if == NULL)
        net_if = aic_get_vif_net_if();

    //dwmp data for test
    //rwnx_data_dump("__aicwf_net_if_output", data, len);

    /* 并发/单模式统一 per-vif 门控: netif 透传自 lwIP linkoutput,
     * SoftAP/P2P (vif2) 下行按各自接口 vif 状态精确放行 */
    if (!aic_net_if_vif_ready(net_if))
    {
        DBG_NETAL_INF("tx:vif not ready\n");
        goto exit;
    }

    aicwf_plat_gpio_trigger(DRV_TRG_NET_TX, 1);

#ifdef CONFIG_FILTER_TCP_ACK
#if TCP_ACK_LOG_ENABLE
    tcp_check_addr(data, len, "tx_uap");
#endif
    {
        if (len <= MAX_TCP_ACK)
        {
            if (aic_filter_send_tcp_ack(data, len, msg))
            {
#if TCP_ACK_LOG_ENABLE
                DBG_NETAL_INF("%s: filter\n", __func__);
#endif
                return 0;
            }
            else
            {
#if TCP_ACK_LOG_ENABLE
                DBG_NETAL_INF("%s: NOT\n", __func__);
#endif
            }
        }
    }
#endif

    net_out_ing = 1;
    do
    {
        unsigned int reserved_len = aic_offsetof();//offsetof(struct hostdesc, cfm_cb);

#ifndef CONFIG_RX_WAIT_LWIP
#ifndef PROTECT_TX_ADMA
        int ret = aicwf_rtos_semaphore_wait(netifdev->net_tx_buf_sema, 100);
#else
        int ret = aicwf_rtos_semaphore_wait(netifdev->net_tx_buf_sema, 200);
#endif /* CONFIG_ADMA_PROTECT */
#else
        int ret = aicwf_rtos_semaphore_wait(netifdev->net_tx_buf_sema, 200);
#endif /* CONFIG_RX_WAIT_LWIP */

        struct net_tx_buf_tag *tx_buf = NULL;

        if (ret == 0)
        {
            aicwf_rtos_mutex_lock(netifdev->net_tx_buf_mutex, -1);
            tx_buf = (struct net_tx_buf_tag *)co_list_pop_front(&netifdev->net_tx_buf_free_list);
            netifdev->net_tx_buf_cnt--;
            aicwf_rtos_mutex_unlock(netifdev->net_tx_buf_mutex);
        }

        if (!tx_buf)
        {
            DBG_NETAL_INF("lwip_wifi_out_buf:tx full\n");
            /* nocopy: 此时尚未 pbuf_ref，所有权归 LwIP，不可 free */
#if !NET_IF_UAP_NCPYBUF
            aicwf_platform_net_buf_tx_free(msg);
#endif
            goto end;
        }

#if NET_IF_UAP_NCPYBUF
        /* [PLATFORM] nocopy TX: 同 aicwf_tx_eth_data_no_tcpack_process，
         * 单段/链式统一引用 pbuf，聚合阶段 scatter copy；headroom 不足回退线性化。
         * 判定基于实际写 hostdesc 的位置 data（须与 pbuf 内存重叠）；
         * ADMA/USB 总线无 scatter hook（pkt_data 须连续），链式 pbuf 强制回退线性化。
         * 类型检查: PBUF_REF/ROM 的 payload 指向外部内存, [payload-14, payload)
         * 并非 pbuf 自有 headroom, 地址比较通过时 hostdesc 写入会越界破坏外部
         * buffer 之前的内存, 故链头仅 RAM/POOL 允许 nocopy;
         * 地址比较为单边检查（只验证不早于结构体末尾，不验证上界）；
         * 链式另需首段 len >= NET_HEAD_OFFSET: hostdesc 下半 [payload, payload+14)
         * 覆盖写首段以太网头区, 不得越过首段 payload 末尾 */
        struct pbuf *p_ref = (struct pbuf *)msg;
#if defined(PBUF_TYPE_FLAG_STRUCT_DATA_CONTIGUOUS)
        bool pbuf_own_buf = (p_ref->type_internal & PBUF_TYPE_FLAG_STRUCT_DATA_CONTIGUOUS) != 0;
#else
        bool pbuf_own_buf = true; /* lwip 1.x 无类型标志, 维持旧行为 */
#endif
        bool nocopy_ok = pbuf_own_buf
                         && (((uint8_t *)data - reserved_len)
                             >= ((uint8_t *)p_ref + sizeof(struct pbuf)))
                         && (p_ref->next == NULL);
        if (nocopy_ok)
        {
            pbuf_ref(p_ref);
            tx_buf->buf = data - reserved_len;
            tx_buf->pmsg = msg;
            /* 链式帧长度自洽：统一按全链长度，不依赖调用方传值 */
            len = p_ref->tot_len;
        }
        else
        {
            uint32_t tot = p_ref->tot_len;
            tx_buf->buf = aicwf_rtos_malloc(reserved_len + tot);
            if (!tx_buf->buf)
            {
                DBG_NETAL_INF("lwip_wifi_out_buf:fb alloc fail\n");
                aicwf_rtos_mutex_lock(netifdev->net_tx_buf_mutex, -1);
                co_list_push_back(&netifdev->net_tx_buf_free_list, (struct co_list_hdr *)tx_buf);
                netifdev->net_tx_buf_cnt++;
                aicwf_rtos_mutex_unlock(netifdev->net_tx_buf_mutex);
                aicwf_rtos_semaphore_signal(netifdev->net_tx_buf_sema, 0);
                goto end;
            }
            tx_buf->pmsg = tx_buf->buf;
            pbuf_copy_partial(p_ref, tx_buf->buf + reserved_len, tot, 0);
            len = tot;
        }
#else
        struct pbuf *q;
        struct pbuf *p_buf = (struct pbuf *)msg;
        uint32_t offset = 0;
        uint8_t *buf = tx_buf->buf + reserved_len;

        for (q = p_buf; q != NULL; q = q->next)
        {
            memcpy((void *)(buf + offset), (void *)q->payload, q->len);
            offset += q->len;
            //AIC_LOG_PRINTF("NET-OUT: %d",  q->len);
        }
        len = offset;
        aicwf_platform_net_buf_tx_free(msg);
#endif

        sent_pkt = &(tx_buf->pkt_hdr);
        sent_pkt->data_ptr = tx_buf->buf + reserved_len;
#if NET_IF_UAP_NCPYBUF
        sent_pkt->frag_next = nocopy_ok ? msg : NULL;
#else
        sent_pkt->frag_next = NULL;
#endif
        sent_pkt->data_len = len;
        sent_pkt->net_id   = 0;
        sent_pkt->pkt_type = TCPIP_PKTTYPE_ETHER;

#if NETIF_PKT_DHCP
        aicwf_dhcp_trace(data, len, "netif-out");
#endif

#if NETIF_PKT_PING
        uint8_t *rdata = (uint8_t *)sent_pkt->data_ptr;
        if (rdata[12] == 0x08 && rdata[13] == 0x00 && rdata[34] == 0x00 && rdata[35] == 0x00)
        {
            DBG_NETAL_INF("NET-OUT ICMP: %d[%d]\n", (rdata[40] << 8) | rdata[41], aicwf_rtos_now(0));
            //aicwf_dump_buffer("tx_payload", rdata, len);
        }
#endif

        aic_tx_start(net_if, sent_pkt, NULL, NULL);

    }
    while (0);

end:
    net_out_ing = 0;
    aicwf_plat_gpio_trigger(DRV_TRG_NET_TX, 0);
    return 0;

exit:
    /* nocopy: 丢弃路径未 pbuf_ref，所有权归 LwIP，不可 free */
#if !NET_IF_UAP_NCPYBUF
    aicwf_platform_net_buf_tx_free(msg);
#endif

    return 0;
}

//-------------------------------------------------------------------
// Driver other API define
//-------------------------------------------------------------------
static int net_dhcp_started = 0;
void aicwf_net_dhcp_stop(net_if_t net_if);
int aicwf_net_dhcp_address_obtained(net_if_t net_if);
/**
 * @brief 启动 DHCP 客户端
 *
 * @param net_if 网络接口句柄
 * @return 0=成功, -1=失败
 */
int aicwf_net_dhcp_start(net_if_t net_if)
{
#if LWIP_IPV4 && LWIP_DHCP
    if (netifapi_dhcp_start(netif_from_handle(net_if)) ==  ERR_OK)
    {
        net_dhcp_started = 1;
        return 0;
    }
#endif //LWIP_IPV4 && LWIP_DHCP

    return -1;
}


/**
 * @brief 检查 DHCP 是否已获取地址
 *
 * @param act 未使用
 * @return 1=已获取, 0=未获取
 */
uint8_t aicwf_net_if_dhcp_done(uint8_t act)
{
    /* [BUGFIX] (共性: netif 反查) 按真实 STA vif(idx 0) 反查 netif; pool[0] 假设
     * STA 恒为首槽, 驱动重初始化/APSTA 下 AP 先启动时不成立, 会查错接口 */
    net_if_t net_if = net_if_find_from_wifi_idx(0);
    if (!net_if)
        return 0;
    int ret = aicwf_net_dhcp_address_obtained(net_if);
    if (ret == 0)
    {
#ifdef RT_USING_NETDEV
        /* [AIC8800-PORT][SiFli] DHCP 成功 -> 立刻把新地址/链路标志回灌 RT-Thread
         * netdev: netdev 的 ip/gw/netmask 只是 netdev_register() 时刻的快照,
         * 不回灌则 ifconfig 恒显示 0.0.0.0、netdev 版 ping 报 "status error"。
         * sync(NULL) 语义同本函数: 按 vif idx 0 反查默认 STA netif; 幂等。
         * (旧版该调用点在 wifi_port_sta.c 的 app_wlan_sta_start() 里紧接
         *  aicwf_net_if_dhcp_done(0) 之后, 本轮只允许改本文件, 故内联到此) */
        aicwf_net_if_netdev_sync(NULL);
#endif
        return 1;
    }
    else
        return 0;
}

//switch netif mode host or slave
/**
 * @brief 切换网络接口 DHCP 模式\
 *
 * mode=1 启动 DHCP 客户端，否则停止。
 *
 * @param mode 1=启动 DHCP, 其他=停止 DHCP
 */
void aicwf_netif_mode_switch(uint8_t mode)
{
    /* [BUGFIX] (共性: netif 反查) 同 aicwf_net_if_dhcp_done: 按 vif idx 0 反查真实
     * STA netif, 而非假定 pool[0] */
    net_if_t net_if = net_if_find_from_wifi_idx(0);
    if (!net_if)
        return;
    if (mode == 1)
    {
        aicwf_net_dhcp_start(net_if);
        DBG_NETAL_INF("__aicwf_net_dhcp_start\n");
    }
    else
    {
        aicwf_net_dhcp_stop(net_if);
        DBG_NETAL_INF("__aicwf_net_dhcp_stop\n");
    }
}

/**
 * @brief 停止 DHCP 客户端
 *
 * @param net_if 网络接口句柄
 */
void aicwf_net_dhcp_stop(net_if_t net_if)
{
    if (net_dhcp_started == 0)
        return;
#if LWIP_IPV4 && LWIP_DHCP
    netifapi_dhcp_stop(netif_from_handle(net_if));
    net_dhcp_started = 0;
#endif //LWIP_IPV4 && LWIP_DHCP
}

/**
 * @brief 获取 DHCP 启动状态
 *
 * @return 1=已启动, 0=未启动
 */
int aicwf_net_dhcp_start_status(void)
{
    return net_dhcp_started;
}

/**
 * @brief 释放 DHCP 地址
 *
 * @param net_if 网络接口句柄
 * @return 0=成功, -1=失败
 */
int aicwf_net_dhcp_release(net_if_t net_if)
{
#if LWIP_IPV4 && LWIP_DHCP
    if (netifapi_dhcp_release(netif_from_handle(net_if)) ==  ERR_OK)
        return 0;
#endif //LWIP_IPV4 && LWIP_DHCP
    return -1;
}

/**
 * @brief 检查 DHCP 是否已分配地址
 *
 * @param net_if 网络接口句柄
 * @return 0=已分配, -1=未分配
 */
int aicwf_net_dhcp_address_obtained(net_if_t net_if)
{
#if LWIP_IPV4 && LWIP_DHCP
    if (dhcp_supplied_address(netif_from_handle(net_if)))
        return 0;
#endif //LWIP_IPV4 && LWIP_DHCP
    return -1;
}

/**
 * @brief 设置 DNS 服务器地址
 *
 * @param dns_server IPv4 DNS 服务器地址
 * @return 0=成功, -1=失败（未启用 LWIP_DNS）
 */
int aicwf_net_set_dns(uint32_t dns_server)
{
#if LWIP_DNS
    ip_addr_t ip;
    ip_addr_set_ip4_u32(&ip, dns_server);
    dns_setserver(0, &ip);
    return 0;
#else
    return -1;
#endif
}

/**
 * @brief 获取 DNS 服务器地址
 *
 * @param dns_server 输出 DNS 服务器 IPv4 地址
 * @return 0=成功, -1=失败
 */
int aicwf_net_get_dns(uint32_t *dns_server)
{
#if LWIP_DNS
    const ip_addr_t *ip;

    if (dns_server == NULL)
        return -1;

    ip = dns_getserver(0);
    *dns_server = ip_addr_get_ip4_u32(ip);

    return 0;
#else
    return -1;
#endif
}
/**
 * @brief 启动 DHCP 服务器
 *
 * @param net_if 网络接口句柄
 */
void aicwf_net_if_dhcp_server_start(net_if_t net_if)
{
    //uint8_t    addr_start[4] = {192, 168, 13, 20};
    //uint8_t    addr_end[4] = {192, 168, 13, 30};
    //dhcp_addr_start.addr  = lwip_ipv4_addr(addr_start);
    //dhcp_addr_end.addr        = lwip_ipv4_addr(addr_end);
    //dhcp_server_start(&gnetif[0], &dhcp_addr_start, &dhcp_addr_end);
#if LWIP_DHCPS
    struct netif *nif = netif_from_handle(net_if);
    char name[3];
    memset(name, 0, 3);
    memcpy(name, nif->name, sizeof(nif->name));
    dhcpd_start(name);
#endif
}

/**
 * @brief 停止 DHCP 服务器
 *
 * @param net_if 网络接口句柄
 */
void aicwf_net_if_dhcp_server_stop(net_if_t net_if)
{
#if LWIP_DHCPS
    struct netif *nif = netif_from_handle(net_if);
    char name[3];
    memset(name, 0, 3);
    memcpy(name, nif->name, sizeof(nif->name));
    dhcpd_stop(nif->name);
#endif
}



/**
 * @brief TCP/IP 协议栈初始化完成回调
 *
 * @param arg 未使用
 */
static void aicwf_tcpip_init_done(void *arg)
{
    (void)arg;
    DBG_NETAL_INF("netif_plat_init done\r\n");
}


/**
 * @brief 初始化平台网络层
 *
 * 调用 LwIP tcpip_init 启动 TCP/IP 协议栈。
 */
void aicwf_net_if_plat_init(void)
{
    tcpip_init(aicwf_tcpip_init_done, NULL);
}

/**
 * @brief 获取已使用的 IP 地址
 * @return IPv4 地址
 */
uint32_t aicwf_net_if_get_tousedip(void)
{
    return netif_dev.used_ip;
}

/**
 * @brief 设置已使用的 IP 地址
 * @param ip IPv4 地址
 */
void aicwf_net_if_set_tousedip(uint32_t ip)
{
    netif_dev.used_ip = ip;
}

/**
 * @brief 获取子网掩码
 * @return 子网掩码
 */
uint32_t aicwf_net_if_get_netmask(void)
{
    return netif_dev.netmask;
}
/**
 * @brief 设置子网掩码
 * @param netmask 子网掩码
 */
void aicwf_net_if_set_netmask(uint32_t netmask)
{
    netif_dev.netmask = netmask;
}

/**
 * @brief 获取指定索引的网络接口句柄
 *
 * @param index netif pool 索引
 * @return 网络接口句柄 (struct netif*)，索引越界返回 slot 0
 */
net_if_t aicwf_net_if_get(uint8_t index)
{
    if (index < PLAT_NETIF_POOL_SIZE)
        return (net_if_t)&s_netif_pool[index];
    return (net_if_t)&s_netif_pool[0];
}

/**
 * @brief 获取网络接口的 WiFi 索引号
 *
 * @param net_if 网络接口句柄
 * @return WiFi 接口索引
 */
int aicwf_net_if_get_wifi_idx(net_if_t net_if)
{
    return netif_get_index(netif_from_handle(net_if));
}

//-------------------------------------------------------------------
// 系统函数包装（host 闭源库 / wpa_supplicant 引用）
//-------------------------------------------------------------------
/**
 * @brief htons() 包装 — wpa_supplicant 预编译代码引用
 *
 * 字节翻转实现 (Cortex-M33 小端: host=LE, network=BE)。
 * [AIC8800-PORT][SiFli] 本文件 include 了 lwip/def.h, 其中 `#define htons lwip_htons`
 * 会把下面的函数名 htons 宏展开成 lwip_htons, 导致 host 预编译库引用的裸符号
 * htons 仍未定义 (链接报 undefined reference to htons)。故先 #undef 恢复真实函数名。
 * weak: 若平台其它处已提供 htons 则让其优先 (SKILL 移植指引: htons multiple definition → weak)。
 */
#undef htons
uint16_t __attribute__((weak)) htons(uint16_t hostshort)
{
    return (uint16_t)(((hostshort & 0xff00) >> 8) | ((hostshort & 0x00ff) << 8));
}

/**
 * @brief inet_aton() 包装 — wpa_supplicant 预编译代码引用
 *
 * 转发到 host 层 aic_inet_aton() 实现。
 */
int inet_aton(const char *cp, void *addr)
{
    return aic_inet_aton(cp, addr);
}


//-------------------------------------------------------------------
// DHCP Server 桩（LWIP_DHCPS 宏已保护调用方）
//-------------------------------------------------------------------
#if LWIP_DHCPS
/**
 * @brief DHCP Server 启动桩
 *
 * 当 SDK 不提供 dhcpd_start 时作为占位实现。
 * 移植时需替换为实际 DHCP Server 启动逻辑。
 */
void dhcpd_start(const char *name)
{
    (void)name;
    DBG_NETAL_INF("dhcpd_start: stub — %s\n", name);
}

/**
 * @brief DHCP Server 停止桩
 *
 * 当 SDK 不提供 dhcpd_stop 时作为占位实现。
 */
void dhcpd_stop(const char *name)
{
    (void)name;
    DBG_NETAL_INF("dhcpd_stop: stub — %s\n", name);
}
#endif
