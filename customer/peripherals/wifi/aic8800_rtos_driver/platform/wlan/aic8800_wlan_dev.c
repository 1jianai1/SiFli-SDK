/**
 * @file    aic8800_wlan_dev.c
 * @brief   AIC8800 → RT-Thread Wi-Fi 管理框架对接层 (rt_wlan_dev_ops)
 *
 * 编译条件: CONFIG_AIC8800_USE_RT_WLAN=y (Kconfig: customer/peripherals/wifi/Kconfig)
 *           该选项 select RT_USING_WIFI, 一键拉起 rt_wlan 框架。
 *
 * 架构 (方案B 双模):
 *   开启本层 → netif 归属权让给 rt_wlan 框架 (eth_device/netdev), 驱动只填 ops;
 *              netif_port.c 的 RX 经 aic8800_wlan_get_device()→rt_wlan_dev_report_data 注入,
 *              TX 经 ops->wlan_send→aicwf_tx_eth_data_process 直通硬件。
 *   关闭(默认) → 维持路线1 (驱动自建 netif, STA+AP+P2P 并发完整), 本文件不参编。
 *
 * 事件模型:
 *   AIC8800 host lib 异步事件 → wifi_port.c:aic_wifi_event_callback → m_ext_event_cb
 *   (本文件在 init 时经 aic_wifi_ext_event_register 注册) → 翻译成 rt_wlan 事件。
 *   op 触发的确定性结果 (join/disconnect/softap/ap_stop) 同步上报;
 *   scan 完成 (SCAN_DONE) 与被动断连 (LEAVE/STA_DISCONNECT) 走异步桥接。
 *
 * 框架 API (rtos/rtthread/components/drivers/wlan/wlan_dev.h):
 *   注册:   rt_wlan_dev_register(wlan, "wlan0", ops, flag, user_data)
 *   RX投递: rt_wlan_dev_report_data(device, buff, len)   [netif_port.c 调用]
 *   事件:   rt_wlan_dev_indicate_event_handle(device, RT_WLAN_DEV_EVT_xxx, buff)
 *   TX出口: 框架 linkoutput → ops->wlan_send(wlan, buff, len)
 */

#include <rtthread.h>
#include <string.h>

/* RT-Thread Wi-Fi 设备框架 (CPPPATH 由 SConscript 注入 components/drivers/wlan) */
#include "wlan_dev.h"

/* AIC8800 host lib 控制面 API + 类型 */
#include "wifi_api.h"     /* aic_wifi_scan / start_sta / start_ap / stop_sta / rssi / mode_set / set_ps_lvl */
#include "wifi_def.h"     /* AIC_WIFI_MODE / AIC_WIFI_EVENT / aic_ap_cfg / aic_wifi_event_cb */
#include "netif_api.h"    /* aic_get_mac_address / aic_set_mac_address */

#define DBG_TAG   "aic.wlan"
#define DBG_LVL   DBG_INFO
#include <rtdbg.h>

/* ============================================================================
 * 外部符号 (platform 侧实现, 无公开头声明者在此 extern)
 * ============================================================================ */
/* wifi_port.c: [AIC8800-PORT] 通用事件扩展回调注册 (见该文件 m_ext_event_cb) */
extern void aic_wifi_ext_event_register(aic_wifi_event_cb cb);
/* netif_port.c: 以太网帧 TX 直通硬件入口 */
extern int aicwf_tx_eth_data_process(uint8_t *data, uint32_t len, void *msg);

/* ============================================================================
 * 模块状态
 * ============================================================================ */
static struct rt_wlan_device aic8800_wlan_dev;   /* 设备实例 (供 ops/桥接/getter 引用) */
static int g_ps_level  = 0;   /* 省电级别缓存 (AIC 无 ps_lvl getter) */
static int g_channel   = 0;   /* set_channel 缓存, softap 时生效 (STA 信道随 AP) */

/* ============================================================================
 * 辅助: 扫描结果翻译 + 上报
 * ============================================================================ */

/**
 * @brief 取 AIC 扫描结果, 翻译成 rt_wlan_info 逐条上报 SCAN_REPORT, 末尾 SCAN_DONE
 *
 * 在 SCAN_DONE_EVENT 异步桥接里调用 (AIC wifi 任务上下文)。
 * 注: wifi_ap_info_t 不含 security 字段 → 统一填 SECURITY_UNKNOWN (M4 降级, M5 可完善)。
 */
static void aic8800_report_scan_results(struct rt_wlan_device *wlan)
{
    static wifi_ap_list_t ap_list;   /* static: ~3KB, 避免事件上下文(AIC wifi 任务)栈压力 */
    struct rt_wlan_info info;
    struct rt_wlan_buff buff;
    int i, cnt;

    rt_memset(&ap_list, 0, sizeof(ap_list));
    aic_wifi_getscan(&ap_list);

    buff.data = &info;
    buff.len  = sizeof(info);

    cnt = (int)ap_list.ap_count;
    if (cnt > MAX_AP_COUNT) cnt = MAX_AP_COUNT;

    for (i = 0; i < cnt; i++)
    {
        wifi_ap_info_t *ap = &ap_list.ap_info[i];
        int slen = 0;
        while (slen < (int)sizeof(ap->ssid) && ap->ssid[slen] != 0) slen++;

        rt_memset(&info, 0, sizeof(info));
        info.security = SECURITY_UNKNOWN;                              /* AIC 扫描结果无 security */
        info.band     = (ap->channel > 14) ? RT_802_11_BAND_5GHZ       /* 5GHZ=0 */
                        : RT_802_11_BAND_2_4GHZ;    /* 2.4GHZ=1 */
        info.datarate = 0;
        info.channel  = (rt_int16_t)ap->channel;
        info.rssi     = (rt_int16_t)(int)ap->rssi;                     /* unsigned 存的是补码, (int)还原负值 */
        info.hidden   = 0;
        info.ssid.len = (rt_uint8_t)slen;
        if (slen > 0)
            rt_memcpy(info.ssid.val, ap->ssid, slen);
        rt_memcpy(info.bssid, ap->bssid, sizeof(info.bssid));

        rt_wlan_dev_indicate_event_handle(wlan, RT_WLAN_DEV_EVT_SCAN_REPORT, &buff);
    }

    rt_wlan_dev_indicate_event_handle(wlan, RT_WLAN_DEV_EVT_SCAN_DONE, RT_NULL);
    LOG_I("scan done: %d AP reported", cnt);
}

/**
 * @brief AIC 事件 → rt_wlan 事件桥接 (注册到 wifi_port.c 的 m_ext_event_cb)
 *
 * 仅处理异步/被动事件; op 触发的确定性结果由各 op 同步上报, 避免重复。
 */
static void aic8800_wlan_event_bridge(AIC_WIFI_EVENT event, aic_wifi_event_data *data)
{
    struct rt_wlan_device *wlan = &aic8800_wlan_dev;
    (void)data;

    switch (event)
    {
    case SCAN_DONE_EVENT:
        aic8800_report_scan_results(wlan);
        break;
    case LEAVE_RESULT_EVENT:
    case STA_DISCONNECT_EVENT:
        LOG_I("event: disconnect (async)");
        rt_wlan_dev_indicate_event_handle(wlan, RT_WLAN_DEV_EVT_DISCONNECT, RT_NULL);
        break;
    default:
        break;
    }
}

/* ============================================================================
 * rt_wlan_dev_ops 实现
 * ============================================================================ */

/** wlan_init — 框架首次使用前调用; 注册事件桥接 (幂等), 硬件初始化由路线1 init 路径完成 */
static rt_err_t aic8800_wlan_init(struct rt_wlan_device *wlan)
{
    (void)wlan;
    aic_wifi_ext_event_register(aic8800_wlan_event_bridge);
    LOG_I("wlan_init: event bridge registered");
    return RT_EOK;
}

/** wlan_mode — STA/AP 模式设置; STA_AP 并发不进标准框架 (返回 ENOSYS, 请走路线1) */
static rt_err_t aic8800_wlan_mode(struct rt_wlan_device *wlan, rt_wlan_mode_t mode)
{
    (void)wlan;
    switch (mode)
    {
    case RT_WLAN_STATION:
        aic_wifi_mode_set(WIFI_MODE_STA);
        return RT_EOK;  /* 0 */
    case RT_WLAN_AP:
        aic_wifi_mode_set(WIFI_MODE_AP);
        return RT_EOK;  /* 1 */
    default:
        LOG_W("mode %d unsupported (STA_AP 并发请用路线1)", (int)mode);
        return -RT_ENOSYS;
    }
}

/** wlan_scan — 触发扫描 (异步); 结果经 SCAN_DONE_EVENT 桥接上报 SCAN_REPORT/DONE */
static rt_err_t aic8800_wlan_scan(struct rt_wlan_device *wlan, struct rt_scan_info *scan_info)
{
    (void)wlan;
    (void)scan_info;
    int ret = aic_wifi_scan();
    LOG_I("wlan_scan trigger ret=%d", ret);
    return (ret == 0) ? RT_EOK : -RT_ERROR;
}

/** wlan_join — 连接 STA (aic_wifi_start_sta 阻塞至连上/超时) + 同步上报 CONNECT/CONNECT_FAIL */
static rt_err_t aic8800_wlan_join(struct rt_wlan_device *wlan, struct rt_sta_info *sta_info)
{
    char ssid[RT_WLAN_SSID_MAX_LENGTH + 1] = {0};
    char pw[RT_WLAN_PASSWORD_MAX_LENGTH + 1] = {0};
    int slen, klen, ret;

    slen = sta_info->ssid.len;
    if (slen > RT_WLAN_SSID_MAX_LENGTH) slen = RT_WLAN_SSID_MAX_LENGTH;
    rt_memcpy(ssid, sta_info->ssid.val, slen);
    klen = sta_info->key.len;
    if (klen > RT_WLAN_PASSWORD_MAX_LENGTH) klen = RT_WLAN_PASSWORD_MAX_LENGTH;
    if (klen > 0) rt_memcpy(pw, sta_info->key.val, klen);

    LOG_I("wlan_join ssid=%s", ssid);
    ret = aic_wifi_start_sta((uint8_t *)ssid, (uint8_t *)pw, 30000);   /* 30s 超时 */
    if (ret == 0)
    {
        rt_wlan_dev_indicate_event_handle(wlan, RT_WLAN_DEV_EVT_CONNECT, RT_NULL);
        LOG_I("wlan_join success");
        return RT_EOK;
    }
    rt_wlan_dev_indicate_event_handle(wlan, RT_WLAN_DEV_EVT_CONNECT_FAIL, RT_NULL);
    LOG_E("wlan_join failed: %d", ret);
    return -RT_ERROR;
}

/** wlan_softap — 构造 aic_ap_cfg 启动 SoftAP + 上报 AP_START */
static rt_err_t aic8800_wlan_softap(struct rt_wlan_device *wlan, struct rt_ap_info *ap_info)
{
    struct aic_ap_cfg cfg;
    int slen, klen, ret;

    rt_memset(&cfg, 0, sizeof(cfg));

    slen = ap_info->ssid.len;
    if (slen > AIC_MAX_SSID_LEN) slen = AIC_MAX_SSID_LEN;
    cfg.aic_ap_ssid.length = (unsigned char)slen;
    rt_memcpy(cfg.aic_ap_ssid.array, ap_info->ssid.val, slen);

    klen = ap_info->key.len;
    if (klen > AIC_MAX_PASSWD_LEN) klen = AIC_MAX_PASSWD_LEN;
    /* OPEN 网络: 密码长度置 0 */
    if (ap_info->security != SECURITY_OPEN && klen > 0)
    {
        cfg.aic_ap_passwd.length = (unsigned char)klen;
        rt_memcpy(cfg.aic_ap_passwd.array, ap_info->key.val, klen);
        cfg.sercurity_type = KEY_WPA2;                 /* 加密统一 WPA2 (覆盖最常见场景, M5 可细分) */
    }
    else
    {
        cfg.aic_ap_passwd.length = 0;
        cfg.sercurity_type = KEY_NONE;
    }

    cfg.channel     = (unsigned char)ap_info->channel;
    cfg.band        = (ap_info->channel > 14) ? 1 : 0; /* AIC: 0=2.4G, 1=5G */
    cfg.type        = 0;                               /* 20M */
    cfg.hidden_ssid = ap_info->hidden ? 1 : 0;
    cfg.bcn_interval = 100;                            /* 100ms 默认 beacon */
    cfg.sta_num      = 4;                              /* 默认最大 4 STA */
    cfg.dtim         = 1;

    LOG_I("wlan_softap ssid=%.*s ch=%d", slen, (char *)cfg.aic_ap_ssid.array, cfg.channel);
    ret = aic_wifi_start_ap(&cfg);
    if (ret == 0)
    {
        g_channel = cfg.channel;
        rt_wlan_dev_indicate_event_handle(wlan, RT_WLAN_DEV_EVT_AP_START, RT_NULL);
        return RT_EOK;
    }
    LOG_E("wlan_softap failed: %d", ret);
    return -RT_ERROR;
}

/** wlan_disconnect — 断开 STA + 同步上报 DISCONNECT */
static rt_err_t aic8800_wlan_disconnect(struct rt_wlan_device *wlan)
{
    LOG_I("wlan_disconnect");
    aic_wifi_stop_sta();
    rt_wlan_dev_indicate_event_handle(wlan, RT_WLAN_DEV_EVT_DISCONNECT, RT_NULL);
    return RT_EOK;
}

/** wlan_ap_stop — 停止 SoftAP + 上报 AP_STOP */
static rt_err_t aic8800_wlan_ap_stop(struct rt_wlan_device *wlan)
{
    LOG_I("wlan_ap_stop");
    aic_wifi_stop_ap();
    rt_wlan_dev_indicate_event_handle(wlan, RT_WLAN_DEV_EVT_AP_STOP, RT_NULL);
    return RT_EOK;
}

/** wlan_ap_deauth — 踢除 STA; AIC 无干净单播 deauth API → 降级 */
static rt_err_t aic8800_wlan_ap_deauth(struct rt_wlan_device *wlan, rt_uint8_t mac[])
{
    (void)wlan;
    (void)mac;
    LOG_W("wlan_ap_deauth unsupported");
    return -RT_ENOSYS;
}

/** wlan_scan_stop — AIC 无独立停扫 API; 扫描自然结束, 返回 EOK */
static rt_err_t aic8800_wlan_scan_stop(struct rt_wlan_device *wlan)
{
    (void)wlan;
    return RT_EOK;
}

/** wlan_get_rssi — 当前连接 RSSI (dBm) */
static int aic8800_wlan_get_rssi(struct rt_wlan_device *wlan)
{
    (void)wlan;
    return (int)aic_wifi_ap_rssi_get();
}

/** wlan_set_powersave — 映射 aic_wifi_set_ps_lvl; 缓存级别供 get 查询 */
static rt_err_t aic8800_wlan_set_powersave(struct rt_wlan_device *wlan, int level)
{
    (void)wlan;
    g_ps_level = level;
    aic_wifi_set_ps_lvl(level);
    LOG_D("wlan_set_powersave level=%d", level);
    return RT_EOK;
}

/** wlan_get_powersave — 返回缓存级别 (AIC 无 ps_lvl getter) */
static int aic8800_wlan_get_powersave(struct rt_wlan_device *wlan)
{
    (void)wlan;
    return g_ps_level;
}

/** wlan_cfg_promisc — 混杂模式; AIC8800 无对应 API → 降级 */
static rt_err_t aic8800_wlan_cfg_promisc(struct rt_wlan_device *wlan, rt_bool_t start)
{
    (void)wlan;
    (void)start;
    LOG_W("wlan_cfg_promisc unsupported");
    return -RT_ENOSYS;
}

/** wlan_cfg_filter — 帧过滤; AIC8800 无对应 API → 降级 */
static rt_err_t aic8800_wlan_cfg_filter(struct rt_wlan_device *wlan, struct rt_wlan_filter *filter)
{
    (void)wlan;
    (void)filter;
    return -RT_ENOSYS;
}

/** wlan_set_channel — STA 信道随 AP, 此处缓存供 softap 生效 */
static rt_err_t aic8800_wlan_set_channel(struct rt_wlan_device *wlan, int channel)
{
    (void)wlan;
    g_channel = channel;
    LOG_D("wlan_set_channel=%d (softap 生效)", channel);
    return RT_EOK;
}

/** wlan_get_channel — 已连接则从 connected_ap_info 的 freq 换算, 否则返回缓存 */
static int aic8800_wlan_get_channel(struct rt_wlan_device *wlan)
{
    struct aic_connected_ap_info_t *info;
    (void)wlan;
    info = aic_wifi_get_sta_connected_ap_info();
    if (info && info->freq)
        return aic_wifi_phy_freq_to_channel(info->band, info->freq);
    return g_channel;
}

/** wlan_set_country — 国家码; AIC8800 无对应 API → 降级 */
static rt_err_t aic8800_wlan_set_country(struct rt_wlan_device *wlan, rt_country_code_t country_code)
{
    (void)wlan;
    (void)country_code;
    return -RT_ENOSYS;
}

/** wlan_get_country — 降级返回枚举首值 */
static rt_country_code_t aic8800_wlan_get_country(struct rt_wlan_device *wlan)
{
    (void)wlan;
    return (rt_country_code_t)0;
}

/** wlan_set_mac — 写本地 MAC (aic_set_mac_address) */
static rt_err_t aic8800_wlan_set_mac(struct rt_wlan_device *wlan, rt_uint8_t mac[])
{
    (void)wlan;
    aic_set_mac_address((uint8_t *)mac);
    return RT_EOK;
}

/** wlan_get_mac — 读本地 MAC (aic_get_mac_address 返回 base MAC) */
static rt_err_t aic8800_wlan_get_mac(struct rt_wlan_device *wlan, rt_uint8_t mac[])
{
    uint8_t *m;
    (void)wlan;
    m = aic_get_mac_address();
    if (m) rt_memcpy(mac, m, 6);
    else   rt_memset(mac, 0, 6);
    return RT_EOK;
}

/** wlan_recv — 拉取模式 (本驱动不用); RX 走 push: netif_port.c → rt_wlan_dev_report_data */
static int aic8800_wlan_recv(struct rt_wlan_device *wlan, void *buff, int len)
{
    (void)wlan;
    (void)buff;
    (void)len;
    return 0;
}

/** wlan_send — 框架 TX 出口 (lwIP linkoutput → 此函数); 以太网帧直通 AIC 硬件 */
static int aic8800_wlan_send(struct rt_wlan_device *wlan, void *buff, int len)
{
    (void)wlan;
    return aicwf_tx_eth_data_process((uint8_t *)buff, (uint32_t)len, RT_NULL);
}

/* ============================================================================
 * ops 结构体
 * ============================================================================ */
static const struct rt_wlan_dev_ops aic8800_wlan_ops =
{
    .wlan_init          = aic8800_wlan_init,
    .wlan_mode          = aic8800_wlan_mode,
    .wlan_scan          = aic8800_wlan_scan,
    .wlan_join          = aic8800_wlan_join,
    .wlan_softap        = aic8800_wlan_softap,
    .wlan_disconnect    = aic8800_wlan_disconnect,
    .wlan_ap_stop       = aic8800_wlan_ap_stop,
    .wlan_ap_deauth     = aic8800_wlan_ap_deauth,
    .wlan_scan_stop     = aic8800_wlan_scan_stop,
    .wlan_get_rssi      = aic8800_wlan_get_rssi,
    .wlan_set_powersave = aic8800_wlan_set_powersave,
    .wlan_get_powersave = aic8800_wlan_get_powersave,
    .wlan_cfg_promisc   = aic8800_wlan_cfg_promisc,
    .wlan_cfg_filter    = aic8800_wlan_cfg_filter,
    .wlan_set_channel   = aic8800_wlan_set_channel,
    .wlan_get_channel   = aic8800_wlan_get_channel,
    .wlan_set_country   = aic8800_wlan_set_country,
    .wlan_get_country   = aic8800_wlan_get_country,
    .wlan_set_mac       = aic8800_wlan_set_mac,
    .wlan_get_mac       = aic8800_wlan_get_mac,
    .wlan_recv          = aic8800_wlan_recv,
    .wlan_send          = aic8800_wlan_send,
};

/* ============================================================================
 * 公开接口: 供 netif_port.c RX 桥接调用
 * ============================================================================ */
struct rt_wlan_device *aic8800_wlan_get_device(void)
{
    return &aic8800_wlan_dev;
}

/* ============================================================================
 * 初始化: 注册 rt_wlan_device + 事件桥接
 *
 * 时机: INIT_DEVICE_EXPORT — RT-Thread 设备初始化阶段。
 * 硬件初始化仍由路线1 的 aicwf_wifi_init_task 完成; 此处仅注册框架设备与事件回调,
 * 不触碰硬件, 故 boot 期安全。App 首次 rt_wlan_connect/scan 时框架回调 wlan_init。
 * ============================================================================ */
static int aic8800_wlan_dev_init(void)
{
    rt_err_t err;

    err = rt_wlan_dev_register(&aic8800_wlan_dev,
                               "wlan0",            /* App 用 rt_wlan_find_device("wlan0") */
                               &aic8800_wlan_ops,
                               0,                  /* flags */
                               RT_NULL);           /* user_data */
    if (err != RT_EOK)
    {
        LOG_E("rt_wlan_dev_register failed: %d", (int)err);
        return -1;
    }

    /* 注册事件桥接 (SCAN_DONE / LEAVE / STA_DISCONNECT → rt_wlan 事件) */
    aic_wifi_ext_event_register(aic8800_wlan_event_bridge);

    LOG_I("AIC8800 wlan device registered as 'wlan0' (M4: ops + async events)");
    return 0;
}
INIT_DEVICE_EXPORT(aic8800_wlan_dev_init);
