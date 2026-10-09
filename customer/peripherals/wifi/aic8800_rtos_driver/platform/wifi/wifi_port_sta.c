/*
 * Copyright (C) 2018-2024 AICSemi Ltd.
 *
 * All Rights Reserved
 *
 * WiFi STA mode platform functions.
 */

#include "wifi_port_priv.h"
#include "wifi_api.h"
#include "netif_api.h"
#include "aic_log.h"
#include <string.h>
#include "netif_port.h"
#define STA_DHCP_POLL_INTERVAL_MS 10
#define STA_DHCP_TIMEOUT_MS (10000/STA_DHCP_POLL_INTERVAL_MS)

/**
 * @brief 获取STA模式下DHCP获取的IP地址
 * @return 32位IP地址（网络字节序）
 */
uint32_t app_wlan_sta_get_ip(void)
{
    uint32_t ip = 0;
    aicwf_net_if_get_ip(NULL, &ip, NULL, NULL);
    DBG_APP_INF("%s:%d:%d:%d:%d", __func__,
                ((uint8_t *)&ip)[3], ((uint8_t *)&ip)[2], ((uint8_t *)&ip)[1], ((uint8_t *)&ip)[0]);

    return ip;
}

/**
 * @brief 认证方式枚举 → AT 输出字符串
 */
static const char *app_wlan_auth_mode_str(aic_wifi_auth_mode_e mode)
{
    switch (mode)
    {
    case AIC_WIFI_AUTH_NONE:
        return "NONE";
    case AIC_WIFI_AUTH_WEP:
        return "WEP";
    case AIC_WIFI_AUTH_WPA_PSK:
        return "WPA-PSK";
    case AIC_WIFI_AUTH_WPA2_PSK:
        return "WPA2-PSK";
    case AIC_WIFI_AUTH_WPA3_SAE:
        return "WPA3-SAE";
    case AIC_WIFI_AUTH_WPA2_WPA3:
        return "WPA2/WPA3";
    default:
        return "OTHER";
    }
}

/**
 * @brief 显示WiFi扫描结果
 *
 * 获取扫描结果，按RSSI降序排列并通过AT接口输出。
 *
 * @return 扫描到的AP数量
 */
uint8_t app_wlan_sta_show_scan_res(void)
{
    wifi_ap_list_t aplist;
    uint16_t apcnt;
    char rsp[128] = {0};
    uint8_t index[MAX_AP_COUNT];
    int i, j;
    uint8_t rs1, rs2, tmp;

    aic_wifi_getscan(&aplist);

    if (aplist.ap_count == 0)
    {
        aicwf_ATRESP(g_athandle, 1, 0, "+NONE");
        return 0;
    }

    for (i = 0; i < aplist.ap_count; i++)
    {
        index[i] = i;
    }

    for (i = 0; i < aplist.ap_count - 1; i++)
    {
        for (j = 0; j < (aplist.ap_count - 1 - i); j++)
        {
            rs1 = aplist.ap_info[index[j]].rssi;
            rs2 = aplist.ap_info[index[j + 1]].rssi;

            if (rs1 < rs2)
            {
                tmp = index[j];
                index[j] = index[j + 1];
                index[j + 1] = tmp;
            }
        }
    }

    for (apcnt = 0; apcnt < aplist.ap_count; apcnt++)
    {
        memset(rsp, 0, 128);
        /* SSID 放最后: SSID 可含空格, LEN/AUTH 置于其前不破坏解析;
         * SSID_LEN=0 表示隐藏 AP (SSID 字段为占位符) */
        sprintf(rsp, "[%d]:rssi=%3d dBm CH=%3d BSSID=%02x:%02x:%02x:%02x:%02x:%02x AUTH=%s SSID_LEN=%d SSID=%s",
                apcnt + 1, (int8_t)(aplist.ap_info[index[apcnt]].rssi), aic_wifi_phy_channel_to_freq(0, aplist.ap_info[index[apcnt]].channel),
                aplist.ap_info[index[apcnt]].bssid[0], aplist.ap_info[index[apcnt]].bssid[1],
                aplist.ap_info[index[apcnt]].bssid[2], aplist.ap_info[index[apcnt]].bssid[3],
                aplist.ap_info[index[apcnt]].bssid[4], aplist.ap_info[index[apcnt]].bssid[5],
                app_wlan_auth_mode_str((aic_wifi_auth_mode_e)aplist.ap_info[index[apcnt]].auth_mode),
                aplist.ap_info[index[apcnt]].ssid_len,
                (char *)aplist.ap_info[index[apcnt]].ssid);
        aicwf_ATRESP(g_athandle, 1, 0, rsp);
    }

    return aplist.ap_count;
}

/**
 * @brief STA连接状态回调函数
 *
 * 根据连接状态打印相应日志信息。
 *
 * @param st 当前STA状态（已连接/已断开/其他）
 */
static void aicwf_app_wlan_sta_status_cb(aic_wifi_mac_status_e st)
{
    if (WIFI_STA_STATUS_DISCONNECTED == st)
    {
        /* FW 主动断开时同步 mode 跟踪值, 对齐 sta stop 成功路径的复位语义
         * (APSTA->AP, STA->UNKNOWN)。否则 mode 残留僵尸 STA: ap stop 时
         * "恢复剩余对端" 会把 APSTA 降级成 STA(链路已断), 后续 p2p start
         * 因 STA->P2P 无允许组合被拒。与主动 sta stop 幂等 */
        AIC_WIFI_MODE *mode = &aicwf_wifi_dev.wifi_mode;
        AIC_WIFI_MODE new_mode = *mode;

        if (*mode == WIFI_MODE_APSTA)
            new_mode = WIFI_MODE_AP;
        else if (*mode == WIFI_MODE_STA)
            new_mode = WIFI_MODE_UNKNOWN;

        if (new_mode != *mode)
        {
            DBG_APP_INF("AIC-STA: disconn, mode sync %d -> %d\n",
                        *mode, new_mode);
            *mode = new_mode;
            aic_wifi_mode_set((int)new_mode);
        }
        else
        {
            DBG_APP_INF("AIC-STA: disconn\n");
        }
    }
    else if (WIFI_STA_STATUS_CONNECTED == st)
    {
        DBG_APP_INF("AIC-STA: conn\n");
    }
    else
    {
        DBG_APP_INF("AIC-STA: other\n");
    }
}

/**
 * @brief STA模式下启动WiFi连接
 *
 * 发起STA连接，连接成功后启动DHCP获取IP地址。
 * 通过轮询方式等待DHCP完成，超时时间约10秒。
 * 如果DHCP在超时前完成则返回成功，否则返回失败。
 *
 * @param ssid       AP的SSID
 * @param pw         AP的密码
 * @param timeout_ms 连接超时时间（毫秒）
 * @return >=0 成功（DHCP完成）, <0 失败
 */
int app_wlan_sta_start(uint8_t *ssid, uint8_t *pw, int timeout_ms)
{
    int ret  = 0;
    int dhcp_timeout = STA_DHCP_TIMEOUT_MS;

    SetWifiConfigFlag(1);
    aic_wifi_sta_status_cb_set(aicwf_app_wlan_sta_status_cb);

    /* 模式互斥: 当前处于其他模式时被拒绝, 必须先 stop 回到 UNKNOWN */
    if (wifi_switch_mode(WIFI_MODE_STA) < 0)
    {
        DBG_APP_ERR("%s: need exit current mode first\n", __func__);
        return -1;
    }

    ret = aic_wifi_start_sta(ssid, pw, timeout_ms);
    if (ret >= 0)
    {
        /* switch_mode 可能已置 APSTA, 勿覆盖组合值 */
        if (aicwf_wifi_dev.wifi_mode != WIFI_MODE_APSTA)
            aicwf_wifi_dev.wifi_mode = WIFI_MODE_STA;
        aicwf_netif_mode_switch(1);

        DBG_APP_INF("%s: start dhcp\n", __func__);
        ret = -10;
        while (dhcp_timeout--)
        {
            if (aicwf_net_if_dhcp_done(0))
            {
                ret = 0;
                app_wlan_sta_get_ip();
                /* [AIC8800-PORT][SiFli] DHCP 已拿到地址: 把 netif 的地址/链路标志
                 * 回灌 RT-Thread netdev, 否则 ifconfig 恒显示 0.0.0.0, 且 netdev
                 * 版 ping 会以 "status error" 拒绝发包
                 * (幂等, 详见 aicwf_net_if_netdev_sync; aicwf_net_if_dhcp_done 内亦有等价调用) */
                aicwf_net_if_netdev_sync(NULL);
                break;
            }
            aicwf_rtos_msleep(STA_DHCP_POLL_INTERVAL_MS);
        }

        if (dhcp_timeout)
        {
            DBG_APP_INF("%s: dhcp finish\n", __func__);
        }
        else
        {
            DBG_APP_WRN("%s: dhcp timeout\n", __func__);
        }
    }
    else
    {
        DBG_APP_ERR("%s: start sta fail %d\n", __func__, ret);
    }

    if (ret < 0)
    {
        aic_wifi_stop_sta();
        /* start 失败回退: 并发场景恢复对端角色 (AP), 单模式回 UNKNOWN,
         * 避免 SoftAP 存活但模式残留 APSTA 导致状态不一致 */
        if (aicwf_wifi_dev.wifi_mode == WIFI_MODE_APSTA)
            aicwf_wifi_dev.wifi_mode = WIFI_MODE_AP;
        else
            aicwf_wifi_dev.wifi_mode = WIFI_MODE_UNKNOWN;
        aic_wifi_mode_set((int)aicwf_wifi_dev.wifi_mode);
    }

    return ret;
}

/**
 * @brief STA模式下停止WiFi连接
 * @return 0=成功, 非0=失败
 */
int app_wlan_sta_stop(void)
{
    int ret = 0;

    /* 幂等: 未处于 STA 相关模式时无需停止 */
    if (aicwf_wifi_dev.wifi_mode != WIFI_MODE_STA &&
            aicwf_wifi_dev.wifi_mode != WIFI_MODE_APSTA)
    {
        return 0;
    }

    ret = aic_wifi_stop_sta();
    if (ret == 0)
    {
        /* stop 成功, 恢复剩余对端模式 (APSTA 中 STA 已停 -> AP) */
        if (aicwf_wifi_dev.wifi_mode == WIFI_MODE_APSTA)
            aicwf_wifi_dev.wifi_mode = WIFI_MODE_AP;
        else
            aicwf_wifi_dev.wifi_mode = WIFI_MODE_UNKNOWN;
        aic_wifi_mode_set((int)aicwf_wifi_dev.wifi_mode);
    }
    else
    {
        DBG_APP_ERR("%s: fail, %d\n", __func__, ret);
    }

    return ret;
}

/**
 * @brief 启动WiFi扫描
 * @return >=0 成功, -1=WiFi未初始化
 */
int app_wlan_scan_start(void)
{
    int ret = -1;
    struct aic_wifi_dev *wifidev = &aicwf_wifi_dev;
    if (wifidev->g_wifi_init)
    {
        ret = aic_wifi_scan();
    }
    return ret;
}
