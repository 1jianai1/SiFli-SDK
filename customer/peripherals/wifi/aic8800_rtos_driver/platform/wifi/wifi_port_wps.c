/*
 * Copyright (C) 2018-2024 AICSemi Ltd.
 *
 * All Rights Reserved
 *
 * WiFi WPS platform functions.
 */

#include "wifi_port_priv.h"
#include "wifi_api.h"
#include "aic_log.h"

extern void aic_set_wps_status_cb(aic_wps_status_notify cb);

#define WPSNOTIFYLISTNUM    5

static WSPNOTIFYFUNC WpsNotifyList[5];
static int WpsNotifyCnt = 0;

struct plat_wps_dev wps_dev = {0,};

/**
 * @brief 设置WPS配网方法
 * @param method WPS方法（0=PIN, 1=PBC按钮）
 */
static void aicwf_app_wlan_set_wps_method(int method)
{
    wps_dev.wpsmethod = method;
}

/**
 * @brief 获取当前WPS配网方法
 * @return WPS方法（0=PIN, 1=PBC）
 */
static int aicwf_app_wlan_get_wps_method(void)
{
    return wps_dev.wpsmethod;
}

/**
 * @brief 注册WPS状态通知回调
 * @param NotifyFunc 通知回调函数指针
 * @return 0=成功, -1=通知列表已满
 */
int app_wlan_wps_notify_reg(WSPNOTIFYFUNC NotifyFunc)
{
    if (WpsNotifyCnt < WPSNOTIFYLISTNUM)
    {
        WpsNotifyList[WpsNotifyCnt++] = NotifyFunc;
        return 0;
    }
    DBG_APP_INF("Wps notify list is full");
    return -1;
}

/**
 * @brief WPS内部状态通知分发
 *
 * 遍历已注册的通知回调列表，依次调用各回调通知WPS状态变化。
 *
 * @param state WPS通知状态码
 * @return 0
 */
static int aicwf_app_wlan_wps_notify(unsigned char state)
{
    int i = 0;
    DBG_APP_INF("Wps notify %d", state);
    for (i = 0; i < WpsNotifyCnt; i++)
    {
        (WpsNotifyList[i])(state);
    }
    return 0;
}

/**
 * @brief 获取WPS注册状态
 * @return WPS注册结果状态码
 */
int app_wlan_wps_get_reg_status(void)
{
    DBG_APP_INF("[WPS] app_wlan_wps_get_reg_status %d\r\n", wps_dev.wps_ret);
    return wps_dev.wps_ret;
}

/**
 * @brief WPS状态回调处理
 *
 * 接收底层WPS事件并转换为平台WPS状态。
 * 根据事件类型触发对应的通知回调。
 *
 * @param event WPS事件类型
 */
static void aicwf_app_wlan_wps_status_callback(int event)
{
    struct plat_wps_dev *wpsdev = &wps_dev;
    DBG_APP_INF("%s:ev:%d\n", __func__, event);
    switch (event)
    {
    case WPS_EV_PBC_OVERLAP:
        wpsdev->wps_ret = WPS_REG_OVERLAP;
        break;
    case WPS_EV_PBC_TIMEOUT:
        wpsdev->wps_ret = WPS_REG_FAIL;
        aicwf_app_wlan_wps_notify(3);
        break;
    case WPS_EV_PBC_ACTIVE:
        wpsdev->wps_ret = WPS_REG_ONGOING;
        aicwf_app_wlan_wps_notify(1);
        break;
    case WPS_EV_PBC_DISABLE:
        break;
    case WPS_EV_PWD_AUTH_FAIL:
    case WPS_EV_AP_PIN_TIMEOUT:
        wpsdev->wps_ret = WPS_REG_FAIL;
        aicwf_app_wlan_wps_notify(3);
        break;
    case WPS_EV_AP_PIN_SUCCESS:
        wpsdev->wps_ret = WPS_REG_SUCC;
        aicwf_app_wlan_wps_notify(2);
        break;
    case WPS_EV_SUCCESS:
        wpsdev->wps_ret = WPS_REG_SUCC;
        aicwf_app_wlan_wps_notify(2);
        break;
    case WPS_EV_SELECTED_TIMEOUT:
        wpsdev->wps_ret = WPS_REG_FAIL;
        aicwf_app_wlan_wps_notify(3);
        break;
    default:
        break;
    }
}

/**
 * @brief 通过PBC按钮方式启动WPS配网
 */
void app_wlan_wps_use_pbc(void)
{
    struct plat_wps_dev *wpsdev = &wps_dev;
    /* [BUGFIX] (共性: 并发 APSTA) SoftAP 在 vif2 (非 0), 用实际 AP vif idx */
    int ap_vif = aic_wifi_get_ap_vif_idx();
    if (ap_vif < 0)
        ap_vif = 0;

    wpsdev->wps_ret = WPS_REG_INVALIDE;

    if (app_wlan_status() != 0)
        return;

    wpsdev->wps_ret = WPS_REG_ONGOING;
    DBG_APP_INF("[AIC] WPS Use PBC...\r\n");
    aicwf_app_wlan_set_wps_method(1);

    aic_set_wps_status_cb(aicwf_app_wlan_wps_status_callback);
    aic_ap_wps_pbc(ap_vif);
    return;
}

/**
 * @brief 启动WPS配网流程
 */
void app_wlan_wps_launch(void)
{
    struct plat_wps_dev *wpsdev = &wps_dev;

    wpsdev->wps_ret = WPS_REG_INVALIDE;

    if (app_wlan_status() != 0)
        return;

    DBG_APP_INF("[AIC] WPS Launch...\r\n");
    wpsdev->wps_ret = WPS_REG_SUCC;
    return;
}

/**
 * @brief 禁用WPS功能
 */
void app_wlan_wps_disable(void)
{
    int ret = -1;
    struct plat_wps_dev *wpsdev = &wps_dev;
    /* [BUGFIX] (共性: 并发 APSTA) SoftAP 在 vif2 (非 0), 用实际 AP vif idx */
    int ap_vif = aic_wifi_get_ap_vif_idx();
    if (ap_vif < 0)
        ap_vif = 0;

    wpsdev->wps_ret = WPS_REG_INVALIDE;

    if (app_wlan_status() != 0)
        return;
    DBG_APP_INF("[AIC] WPS Disable...\r\n");

    ret = aic_ap_wps_disable(ap_vif);

    if (ret == 0)
        wpsdev->wps_ret = WPS_REG_SUCC;
    else
        wpsdev->wps_ret = WPS_REG_FAIL;

    ret = aic_ap_wps_cacel(ap_vif);
    if (ret == 0)
        wpsdev->wps_ret = WPS_REG_SUCC;
    else
    {
        wpsdev->wps_ret = WPS_REG_FAIL;
    }
    return;
}

/**
 * @brief 启用WPS功能
 */
void app_wlan_wps_enable(void)
{
    struct plat_wps_dev *wpsdev = &wps_dev;

    DBG_APP_INF("[AIC] WPS Enable...\r\n");
    wpsdev->wps_ret = WPS_REG_INVALIDE;

    if (app_wlan_status() != 0)
        return;

    wpsdev->wps_ret = WPS_REG_SUCC;
    return;
}

/**
 * @brief 取消当前WPS操作
 */
void app_wlan_wps_use_cancel(void)
{
    int ret = -1;
    struct plat_wps_dev *wpsdev = &wps_dev;
    /* [BUGFIX] (共性: 并发 APSTA) SoftAP 在 vif2 (非 0), 用实际 AP vif idx */
    int ap_vif = aic_wifi_get_ap_vif_idx();
    if (ap_vif < 0)
        ap_vif = 0;

    wpsdev->wps_ret = WPS_REG_ONGOING;

    DBG_APP_INF("[AIC] WPS Cancel...\r\n");

    wpsdev->wps_ret = WPS_REG_INVALIDE;

    if (app_wlan_status() != 0)
        return;

    ret = aic_ap_wps_cacel(ap_vif);

    if (ret == 0)
        wpsdev->wps_ret = WPS_REG_SUCC;
    else
        wpsdev->wps_ret = WPS_REG_FAIL;
    return;
}

/**
 * @brief 通过PIN码方式启动WPS配网
 * @param pin PIN码字符串（8位数字）
 */
void app_wlan_wps_use_pin(char *pin)
{
    int ret = -1;
    struct plat_wps_dev *wpsdev = &wps_dev;
    /* [BUGFIX] (共性: 并发 APSTA) SoftAP 在 vif2 (非 0), 用实际 AP vif idx */
    int ap_vif = aic_wifi_get_ap_vif_idx();
    if (ap_vif < 0)
        ap_vif = 0;

    DBG_APP_INF("[AIC] WPS Use PIN %s ...\r\n", pin);
    wpsdev->wps_ret = WPS_REG_INVALIDE;

    if (app_wlan_status() != 0)
        return;

    if (pin == NULL)
        return;

    aicwf_app_wlan_set_wps_method(0);
    aic_set_wps_status_cb(aicwf_app_wlan_wps_status_callback);
    ret = aic_ap_wps_pin(ap_vif, pin);

    if (ret != 0)
    {
        wpsdev->wps_ret = WPS_REG_FAIL;
        return;
    }

    wpsdev->wps_ret = WPS_REG_ONGOING;
    return;
}

/**
 * @brief 验证WPS PIN码校验和
 *
 * 使用标准的WPS PIN校验和算法验证PIN码的有效性。
 *
 * @param PIN 8位PIN码（unsigned long格式）
 * @return 1=校验通过, 0=校验失败
 */
int app_wlan_validate_checksum(unsigned long int PIN)
{
    unsigned int accum = 0;
    accum += 3 * ((PIN / 10000000) % 10);
    accum += 1 * ((PIN / 1000000) % 10);
    accum += 3 * ((PIN / 100000) % 10);
    accum += 1 * ((PIN / 10000) % 10);
    accum += 3 * ((PIN / 1000) % 10);
    accum += 1 * ((PIN / 100) % 10);
    accum += 3 * ((PIN / 10) % 10);
    accum += 1 * ((PIN / 1) % 10);
    return (0 == (accum % 10));
}
