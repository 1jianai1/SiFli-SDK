/*
 * Copyright (C) 2018-2024 AICSemi Ltd.
 *
 * All Rights Reserved
 *
 * WiFi P2P platform functions.
 */

#ifdef CONFIG_P2P_SUPPORT

#include "wifi_port_priv.h"
#include "wifi_api.h"
#include "aic_log.h"
#include <string.h>

#define AP_SSID_DFT  "BTD-AP"
#define AP_PASS_DFT  "12345678"

struct aic_p2p_cfg aic_wifi_p2p_cfg =
{
    .enable_acs = 1,
    .band = 0,
    .channel = 6,
    .type = WIFI_AP_MODE_2G4,
    .enable_acs = 1,
    .enable_he = 1,
    .aic_p2p_ssid = {
        .array = AP_SSID_DFT,
        .length = sizeof(AP_SSID_DFT) - 1,
    },
    .aic_ap_passwd = {
        .array = AP_PASS_DFT,
        .length = sizeof(AP_PASS_DFT) - 1,
    },
    .p2p_dev_name = AP_PASS_DFT,
};

/**
 * @brief 启动P2P（WiFi Direct）功能
 *
 * 配置P2P设备名称、SSID和密码后启动P2P GO（Group Owner）。
 * 传入参数为空时保持原有配置不变。
 *
 * @param dev_name P2P设备名称（可选，NULL保持原值）
 * @param ssid     P2P组SSID（可选，NULL保持原值）
 * @param passwd   P2P组密码（可选，NULL保持原值）
 * @return >=0 成功, <0 失败
 */
int app_wlan_p2p_start(const char *dev_name, const char *ssid, const char *passwd)
{
    int ret;

    if (dev_name)
    {
        memset(aic_wifi_p2p_cfg.p2p_dev_name, 0, sizeof(aic_wifi_p2p_cfg.p2p_dev_name));
        strncpy((char *)aic_wifi_p2p_cfg.p2p_dev_name, dev_name, sizeof(aic_wifi_p2p_cfg.p2p_dev_name) - 1);
    }
    if (ssid)
    {
        memset(aic_wifi_p2p_cfg.aic_p2p_ssid.array, 0, sizeof(aic_wifi_p2p_cfg.aic_p2p_ssid.array));
        strncpy((char *)aic_wifi_p2p_cfg.aic_p2p_ssid.array, ssid, sizeof(aic_wifi_p2p_cfg.aic_p2p_ssid.array) - 1);
        aic_wifi_p2p_cfg.aic_p2p_ssid.length = strlen((char *)aic_wifi_p2p_cfg.aic_p2p_ssid.array);
    }
    if (passwd)
    {
        memset(aic_wifi_p2p_cfg.aic_ap_passwd.array, 0, sizeof(aic_wifi_p2p_cfg.aic_ap_passwd.array));
        strncpy((char *)aic_wifi_p2p_cfg.aic_ap_passwd.array, passwd, sizeof(aic_wifi_p2p_cfg.aic_ap_passwd.array) - 1);
        aic_wifi_p2p_cfg.aic_ap_passwd.length = strlen((char *)aic_wifi_p2p_cfg.aic_ap_passwd.array);
    }

    if (wifi_switch_mode(WIFI_MODE_P2P) < 0)
    {
        DBG_APP_ERR("%s: need exit current mode first\n", __func__);
        return -1;
    }

    ret = aic_wifi_start_p2p(&aic_wifi_p2p_cfg);

    if (ret >= 0)
    {
        /* switch_mode 可能已置 APP2P, 勿覆盖组合值 */
        if (aicwf_wifi_dev.wifi_mode != WIFI_MODE_APP2P)
            aicwf_wifi_dev.wifi_mode = WIFI_MODE_P2P;
    }
    else
    {
        /* start 失败: 先清理半启动状态 (wlan_start_p2p 成功但 GROUP_ADD 失败时
         * p2p_started=1, wpa iface/netif/dhcp 需经 stop 回收), 再回退模式 */
        aic_wifi_stop_p2p();
        if (aicwf_wifi_dev.wifi_mode == WIFI_MODE_APP2P)
            aicwf_wifi_dev.wifi_mode = WIFI_MODE_AP;
        else
            aicwf_wifi_dev.wifi_mode = WIFI_MODE_UNKNOWN;
        aic_wifi_mode_set((int)aicwf_wifi_dev.wifi_mode);
    }

    return ret;
}

/**
 * @brief 停止P2P功能
 * @return aic_wifi_stop_p2p()的返回值
 */
int app_wlan_p2p_stop(void)
{
    int ret = 0;

    /* 幂等: 未处于 P2P 相关模式时无需停止 */
    if (aicwf_wifi_dev.wifi_mode != WIFI_MODE_P2P &&
            aicwf_wifi_dev.wifi_mode != WIFI_MODE_APP2P)
    {
        return 0;
    }

    ret = aic_wifi_stop_p2p();
    if (ret == 0)
    {
        /* stop 成功, 恢复剩余对端模式 (APP2P 中 P2P 已停 -> AP) */
        if (aicwf_wifi_dev.wifi_mode == WIFI_MODE_APP2P)
            aicwf_wifi_dev.wifi_mode = WIFI_MODE_AP;
        else
            aicwf_wifi_dev.wifi_mode = WIFI_MODE_UNKNOWN;
        aic_wifi_mode_set((int)aicwf_wifi_dev.wifi_mode);
    }

    return ret;
}

#endif /* CONFIG_P2P_SUPPORT */
