/*
 * Copyright (C) 2018-2024 AICSemi Ltd.
 *
 * All Rights Reserved
 *
 * WiFi core lifecycle, event handling, and platform configuration.
 */

#include "wifi_port_priv.h"
#include "wifi_api.h"
#include "cli_api.h"
#include "netif_api.h"
#include "sdio_api.h"
#include "sys_api.h"
#include "fmac_port.h"
#include "sdio_port.h"
#include "netif_port.h"
#include "aic_log.h"
#include <string.h>

/* ============================================================
 * Global state
 * ============================================================ */

int g_athandle = 0;

struct aic_wifi_dev aicwf_wifi_dev =
{
    .wifi_mode = WIFI_MODE_UNKNOWN,
};

int g_vendor_specific_apple = 0;
static wifi_event_handle m_scan_result_handle = NULL;
static wifi_event_handle m_scan_done_handle = NULL;
static wifi_event_handle m_join_success_handle = NULL;
static wifi_event_handle m_join_fail_handle = NULL;
static wifi_event_handle m_leave_handle = NULL;
wifi_priv_event_cb priv_aic_wifi_event_cb = NULL;

/* [AIC8800-PORT] 通用事件扩展回调 (供 rt_wlan 桥接层等消费全量事件)。
 * 与 priv_aic_wifi_event_cb (签名 (MAC_addr,mode), MAC/mode 通知用途) 区分:
 * 本回调收 (AIC_WIFI_EVENT, aic_wifi_event_data)。原各 m_xxx_handle 为 static
 * 且无对外注册入口, 恒 NULL; 此钩子为 platform 侧新增, NULL 守卫,
 * 未注册时零开销, 路线1 (OFF) 无影响。 */
static aic_wifi_event_cb m_ext_event_cb = NULL;
static uint8_t wifi_auto_start = 0;

void aic_wifi_cmd(char *atcmd, unsigned int athandle, unsigned char chan_num);
void wifi_driver_action(unsigned int act);

/**
 * @brief 设置WiFi配置标志
 * @param value 配置状态值，1表示已配置
 */
void SetWifiConfigFlag(uint8_t value)
{
    DBG_APP_INF("%s %d\n", __func__, value);
    aicwf_wifi_dev.wifi_config_st = value;
}

/**
 * @brief 获取WiFi配置标志
 * @return 当前配置状态值
 */
unsigned char GetWifiConfigFlag(void)
{
    return aicwf_wifi_dev.wifi_config_st;
}

/**
 * @brief 获取当前WiFi工作模式
 * @return WiFi模式，参见 AIC_WIFI_MODE 枚举
 */
int aic_wifi_get_mode(void)
{
    return aicwf_wifi_dev.wifi_mode;
}

/**
 * @brief 获取WiFi BSS状态
 *
 * 根据配置标志和AP的实际运行状态返回WiFi状态：
 * - 已配置且AP已启动返回1
 * - 已配置但AP未启动返回0
 * - 未配置返回0xff
 *
 * @return 0=AP已停止, 1=AP已启动, 0xff=未配置
 */
uint8_t GetWifiStatus(void)
{
    if (aicwf_wifi_dev.wifi_config_st == 1)
    {
        if (app_wlan_status() == UAP_BSS_START)
            return 1;
        else
            return 0;
    }
    else
    {
        return 0xff;
    }
}

/**
 * @brief 获取WiFi MAC地址（内部实现）
 * @param wifi_mac 输出的6字节MAC地址缓冲区
 * @return 始终返回0
 */
static int aicwf_get_wifi_mac(uint8_t *wifi_mac)
{
    uint8_t *mac;
    mac = aic_get_mac_address();
    memcpy(wifi_mac, mac, 6);
    DBG_APP_INF("%s :%x:%x:%x:%x:%x:%x\n", __func__,
                wifi_mac[0], wifi_mac[1], wifi_mac[2], wifi_mac[3], wifi_mac[4], wifi_mac[5]);
    return 0;
}

/**
 * @brief 获取WiFi MAC地址
 * @param macbuf 输出的6字节MAC地址缓冲区
 * @return 始终返回0
 */
int GetMACAddr(uint8_t *macbuf)
{
    DBG_APP_INF("%s", __func__);
    aicwf_get_wifi_mac(macbuf);
    return 0;
}

/**
 * @brief 不区分大小写的字符串比较
 * @param s1 第一个字符串
 * @param s2 第二个字符串
 * @param n  最多比较的字符数
 * @return 0=相等, 正数=s1大于s2, 负数=s1小于s2
 */
int strncasecmp(const char *s1, const char *s2, size_t n)
{
    int c1, c2;
    while (n > 0)
    {
        c1 = *((unsigned char *)(s1++));
        if (c1 >= 'A' && c1 <= 'Z')
            c1 = c1 + ('a' - 'A');
        c2 = *((unsigned char *)(s2++));
        if (c2 >= 'A' && c2 <= 'Z')
            c2 = c2 + ('a' - 'A');
        if (c1 != c2)
        {
            return (c1 - c2);
        }
        if (c1 == '\0')
        {
            return 0;
        }
        --n;
    }
    return 0;
}

/**
 * @brief 获取WiFi初始化状态
 * @return WiFi初始化状态值（由驱动初始化任务设置）
 */
uint8_t GetWiFiInitStat(void)
{
    DBG_APP_INF("%s->>%d\n", __func__, aicwf_wifi_dev.wifi_init_status);
    return aicwf_wifi_dev.wifi_init_status;
}

/**
 * @brief 获取WPS配置方法字符串
 * @return 配置方法字符串（"push_button keypad"）
 */
const char *aic_get_wps_configmethod(void)
{
    const char *cm = "push_button keypad";
    return cm;
}

/**
 * @brief 获取WPS制造商名称
 * @return 制造商名称（"AIC"）
 */
const char *aic_get_wps_Manufac(void)
{
    const char *cm = "AIC";
    return cm;
}

#if defined(CONFIG_AIC8800DW)
/**
 * @brief 获取WPS型号名称（DW系列）
 * @return 型号名称（"AIC-8800-DW"）
 */
const char *aic_get_wps_ModelName(void)
{
    const char *cm = "AIC-8800-DW";
    return cm;
}
#elif defined(CONFIG_AIC8800D80)
/**
 * @brief 获取WPS型号名称（D80系列）
 * @return 型号名称（"AIC-8800-D80"）
 */
const char *aic_get_wps_ModelName(void)
{
    const char *cm = "AIC-8800-D80";
    return cm;
}
#elif defined(CONFIG_AIC8800D80N)
/**
 * @brief 获取WPS型号名称（D80N系列）
 * @return 型号名称（"AIC-8800-D80N"）
 *
 * [AIC8800-PORT][SiFli] 平台保留分支: 厂家 b4036bf 合并时删掉了本 D80N 分支,
 * 而 D80N 构建下 host 库 (wpa_supplicant.c:wpa_supplicant_add_iface) 会引用
 * aic_get_wps_ModelName —— 缺此分支即链接报 undefined reference。
 */
const char *aic_get_wps_ModelName(void)
{
    const char *cm = "AIC-8800-D80N";
    return cm;
}
#endif

/**
 * @brief 获取WPS序列号
 * @return 序列号字符串（"12345"）
 */
const char *aic_get_wps_SerialNum(void)
{
    const char *cm = "12345";
    return cm;
}

/**
 * @brief 获取WPS型号编号
 * @return 型号编号字符串（"123"）
 */
const char *aic_get_wps_ModelNum(void)
{
    const char *cm = "123";
    return cm;
}

/**
 * @brief 获取WPS设备名称
 * @return 设备名称字符串（"E5576-821"）
 */
const char *aic_get_wps_DeviceName(void)
{
    const char *cm = "E5576-821";
    return cm;
}

/**
 * @brief 获取WPA配置扩展标志
 * @return 配置扩展标志（0x0）
 */
unsigned char aic_get_wpas_config_extra(void)
{
    return 0x0;
}

/**
 * @brief 获取ACS（自动信道选择）噪声参数
 * @param fix  输出的固定标志
 * @param n24g 输出的2.4GHz噪声值（dBm）
 * @param n5g  输出的5GHz噪声值（dBm）
 */
void aic_get_acs_noise(unsigned char *fix, signed char *n24g, signed char *n5g)
{
    *fix = 1;
    *n24g = -89;
    *n5g = -92;
}

/**
 * @brief 获取ACS信道权重因子
 * @return 信道权重因子字符串，格式如 "1:0.98 6:0.98 11:0.98"
 */
const char *aic_get_acs_chfactor(void)
{
    const char *fc =  "1:0.98 6:0.98 11:0.98";
    return fc;
}

/**
 * @brief 获取ACS扫描次数
 * @return 扫描次数（5）
 */
unsigned char aic_get_acs_scan_num(void)
{
    return 5;
}

/**
 * @brief 获取省电等级
 * @return 省电等级（5）
 */
unsigned char aic_get_ps_level(void)
{
    return 5;
}

/**
 * @brief 获取MAMT保存配置值
 * @return 配置值（20）
 */
unsigned int aic_config_mamt_saved(void)
{
    return 20;
}

/**
 * @brief 设置WiFi工作模式
 *
 * 根据固件类型和传入模式参数决定WiFi的工作模式。
 * RF测试模式可通过固件类型标志位或生产模式自动进入。
 *
 * @param mode 目标WiFi模式
 */
void wifi_set_default_mode(void)
{
    unsigned char fwtype = aicwf_get_wifi_fw_type();
    struct aic_wifi_dev *wifidev = &aicwf_wifi_dev;

    if (fwtype & 0x02)
        wifidev->wifi_mode = WIFI_MODE_RFTEST;
    else if (aicwf_hal_IsInProduction_Mode() && (fwtype & 0x04))
        wifidev->wifi_mode = WIFI_MODE_RFTEST;

    if ((fwtype & 0x05) == 0x05)
    {
        wifidev->wifi_mode = WIFI_MODE_UNKNOWN;
    }

    /* No AT/factory fwtype: keep the combo fmac path (the mailbox HCI needs
       the WiFi firmware to be started). */
    if (wifidev->wifi_mode == WIFI_MODE_UNKNOWN)
        wifidev->wifi_mode = WIFI_MODE_STA;
}

/**
 * @brief 强制设置 WiFi 工作模式（不检查当前状态）
 *
 * 同步设置 platform(wifi_mode) 与 driver(rwnx_hw->mode) 两级模式。
 * 通常由 AT 命令等外部触发场景使用，配合 wifi_driver_action(0x03) 重启驱动生效。
 *
 * @param mode 目标 WiFi 模式
 */
void wifi_set_mode(AIC_WIFI_MODE mode)
{
    struct aic_wifi_dev *wifidev = &aicwf_wifi_dev;

    /* 同步 platform/driver 两级模式, 供复位路径按目标模式执行 deinit/init */
    wifidev->wifi_mode = mode;
    aic_wifi_mode_set((int)mode);
}

/**
 * @brief 切换 WiFi 工作模式
 *
 * 如果目标模式与当前模式不同，且当前模式不是 WIFI_MODE_UNKNOWN，
 * 则返回 -1 表示需要先退出当前模式才能切换。
 * 模式允许时，通过 aic_wifi_mode_set 切换到目标模式。
 *
 * @param mode 目标 WiFi 模式
 * @return 0=成功, -1=需先退出当前模式
 */
int wifi_switch_mode(AIC_WIFI_MODE mode)
{
    struct aic_wifi_dev *wifidev = &aicwf_wifi_dev;
    AIC_WIFI_MODE cur = wifidev->wifi_mode;

    if (cur == mode || cur == WIFI_MODE_UNKNOWN)
    {
        /* Keep platform role in sync — netif TX/RX gates on aicwf_wifi_dev.wifi_mode */
        wifidev->wifi_mode = mode;
        aic_wifi_mode_set((int)mode);
        return 0;
    }

    /* STA + SoftAP concurrent: allow STA<->AP without tearing the other down */
    if ((cur == WIFI_MODE_STA && mode == WIFI_MODE_AP) ||
            (cur == WIFI_MODE_AP && mode == WIFI_MODE_STA) ||
            (cur == WIFI_MODE_APSTA &&
             (mode == WIFI_MODE_STA || mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)))
    {
        DBG_APP_INF("%s: concurrent cur=%d + %d -> APSTA\n", __func__, cur, mode);
        wifidev->wifi_mode = WIFI_MODE_APSTA;
        aic_wifi_mode_set((int)WIFI_MODE_APSTA);
        return 0;
    }

#ifdef CONFIG_P2P_SUPPORT
    /* APP2P 并发固定启动顺序: 先 P2P 后 AP (对齐 STA/AP 的先 STA 后 AP)。
     * AP 先起时 P2P GO 只能跟随 AP 信道(含 ACS 实选), 5G 非发现信道
     * (如 ch44)上手机扫不到 GO; 故 AP 已运行时拒绝再起 P2P,
     * 须先 ap stop 再 p2p start */
    if ((cur == WIFI_MODE_P2P && mode == WIFI_MODE_AP) ||
            (cur == WIFI_MODE_APP2P &&
             (mode == WIFI_MODE_AP || mode == WIFI_MODE_P2P || mode == WIFI_MODE_APP2P)))
    {
        DBG_APP_INF("%s: concurrent cur=%d + %d -> APP2P\n", __func__, cur, mode);
        wifidev->wifi_mode = WIFI_MODE_APP2P;
        aic_wifi_mode_set((int)WIFI_MODE_APP2P);
        return 0;
    }
#endif

#ifdef CONFIG_P2P_SUPPORT
    if (cur == WIFI_MODE_AP && mode == WIFI_MODE_P2P)
    {
        DBG_APP_ERR("%s: APP2P order fixed (P2P first, AP second): "
                    "stop AP before starting P2P\n", __func__);
        return -1;
    }
#endif

    DBG_APP_INF("%s: cur=%d target=%d, need exit first\n",
                __func__, cur, mode);
    return -1;
}

/**
 * @brief 注册WiFi私有事件回调
 * @param cb 事件回调函数指针
 */
void aic_wifi_priv_event_register(wifi_priv_event_cb cb)
{
    priv_aic_wifi_event_cb = cb;
}

/* [AIC8800-PORT] 注册通用事件扩展回调 (见 m_ext_event_cb 说明)。
 * rt_wlan 桥接层 (aic8800_wlan_dev.c) 在 wlan_init 时注册, 用于
 * 捕获 SCAN_DONE / LEAVE / STA_DISCONNECT 等异步事件并转成 rt_wlan 事件。 */
void aic_wifi_ext_event_register(aic_wifi_event_cb cb)
{
    m_ext_event_cb = cb;
}

/**
 * @brief WiFi事件回调分发函数
 *
 * 根据事件类型将事件分发到已注册的各类事件处理器。
 * 支持的事件包括：扫描结果、扫描完成、连接成功/失败、
 * 离开、探测请求、EAPOL完成、关联/去关联指示等。
 *
 * @param enEvent 事件类型
 * @param enData  事件数据
 */
void aic_wifi_event_callback(AIC_WIFI_EVENT enEvent, aic_wifi_event_data *enData)
{
    switch (enEvent)
    {
    case SCAN_RESULT_EVENT:
    {
        if (m_scan_result_handle)
        {
            m_scan_result_handle(enData);
        }
        break;
    }
    case SCAN_DONE_EVENT:
    {
        if (m_scan_done_handle)
        {
            m_scan_done_handle(enData);
        }
        break;
    }
    case JOIN_SUCCESS_EVENT:
    {
        if (m_join_success_handle)
        {
            m_join_success_handle(enData);
        }
        break;
    }
    case JOIN_FAIL_EVENT:
    {
        if (m_join_fail_handle)
        {
            m_join_fail_handle(enData);
        }
        break;
    }
    case LEAVE_RESULT_EVENT:
    {
        if (m_leave_handle)
        {
            m_leave_handle(enData);
        }
        break;
    }
    case PRO_DISC_REQ_EVENT:
    {
        uint32_t *mac_addr = (uint32_t *)enData->data.auth_deauth_data.reserved;
        DBG_APP_VRB("PRO_DISC_REQ_EVENT mac_addr = %02x:%02x:%02x:%02x:%02x:%02x\r\n"
                    , mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    }
    case EAPOL_STA_FIN_EVENT:
    {
        uint32_t *mac_addr = (uint32_t *)enData->data.auth_deauth_data.reserved;
        DBG_APP_VRB("EAPOL_STA_FIN_EVENT mac_addr = %02x:%02x:%02x:%02x:%02x:%02x\r\n"
                    , mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    }
    case EAPOL_P2P_FIN_EVENT:
    {
        uint32_t *mac_addr = (uint32_t *)enData->data.auth_deauth_data.reserved;
        DBG_APP_VRB("EAPOL_P2P_FIN_EVENT mac_addr = %02x:%02x:%02x:%02x:%02x:%02x\r\n"
                    , mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    }
    case ASSOC_IND_EVENT:
    {
        uint32_t *mac_addr = (uint32_t *)enData->data.auth_deauth_data.reserved;
        DBG_APP_VRB("ASSOC_IND_EVENT mac_addr = %02x:%02x:%02x:%02x:%02x:%02x\r\n"
                    , mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    }
    case STA_DISCONNECT_EVENT:
    {
        DBG_APP_INF("STA_DISCONNECT_EVENT\r\n");
        break;
    }
    case DISASSOC_STA_IND_EVENT:
    {
        uint32_t *mac_addr = (uint32_t *)enData->data.auth_deauth_data.reserved;
        DBG_APP_VRB("DISASSOC_STA_IND_EVENT mac_addr = %02x:%02x:%02x:%02x:%02x:%02x\r\n"
                    , mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    }
    case DISASSOC_P2P_IND_EVENT:
    {
        uint32_t *mac_addr = (uint32_t *)enData->data.auth_deauth_data.reserved;
        DBG_APP_VRB("DISASSOC_P2P_IND_EVENT mac_addr = %02x:%02x:%02x:%02x:%02x:%02x\r\n"
                    , mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    }
    default:
    {
        break;
    }
    }

    /* [AIC8800-PORT] 分发到扩展事件回调 (rt_wlan 桥接层); NULL 守卫, 未注册时零开销 */
    if (m_ext_event_cb)
    {
        m_ext_event_cb(enEvent, enData);
    }
}

/**
 * @brief CLI命令响应回调，将结果通过AT接口返回
 * @param rsp 命令响应字符串
 */
static void aicwf_CliCmdCb(char *rsp)
{
    aicwf_ATRESP(0, 1, 0, rsp);
}

/**
 * @brief WiFi驱动初始化任务入口
 *
 * 该任务通过信号量驱动WiFi驱动的生命周期管理，支持以下操作：
 * - AIC_DRIVER_REBOOT_PU/OPEN: 上电复位并初始化WiFi
 * - AIC_DIRVER_REBOOT_PD/CLOSE: 断电并去初始化WiFi
 * - AIC_DRIVER_REBOOT_ADP/RESET: 适配测试模式下的完整复位流程
 *
 * @param argv 任务参数（未使用）
 */
static void aicwf_wifi_init_task(void *argv)
{
    unsigned char fwtype = aicwf_get_wifi_fw_type();
    unsigned char need_start = 0;
    struct aic_wifi_dev *wifidev = &aicwf_wifi_dev;

    wifi_set_default_mode();

    DBG_APP_INF("wifi_mode: %d\n", wifidev->wifi_mode);

    aic_cli_set_cb(aicwf_CliCmdCb);
    while (1)
    {
        aicwf_rtos_semaphore_wait(wifidev->wifi_task_sem, AIC_RTOS_WAIT_FOREVEVR);
        DBG_APP_INF("wifi_drive_op: %d\n", wifidev->wifi_drive_op);

        switch (wifidev->wifi_drive_op)
        {
        case AIC_DRIVER_REBOOT_PU:
        {
            aicwf_sdio_reset_wifi();
            if (aicwf_sdio_reset())
                break;
            need_start = wifi_auto_start;
        }
        case AIC_DRIVER_REBOOT_OPEN:
            aic_wifi_init(wifidev->wifi_mode, NULL);
            if ((need_start) && (wifidev->g_wifi_init))
            {
                wifidev->wifi_init_status = wifidev->g_wifi_init;
                wifi_bss_st = 0;
                if (wifidev->wifi_mode == WIFI_MODE_AP)
                {
                    app_wlan_bss_config(1);
                }
                need_start = 0;
            }
            else if (!wifidev->g_wifi_init)
            {
                aicwf_driver_unexcepted(0x08);
            }
            break;

        case AIC_DIRVER_REBOOT_PD:
            aic_sdio_host_predeinit();
            aicwf_sdio_pdn_wifi();
        case AIC_DRIVER_REBOOT_CLOSE:
            aic_wifi_deinit(wifidev->wifi_mode);
            /* deinit 即全停, 模式复位为 UNKNOWN */
            wifidev->wifi_mode = WIFI_MODE_UNKNOWN;
            need_start = 0;
            break;

        case AIC_DRIVER_REBOOT_ADP:
            aic_set_adap_test(1);
        case AIC_DRIVER_REBOOT_RESET:
        {
            /* 记录复位前模式: deinit 用它清理, init 用它选择固件(RFTEST 区分) */
            AIC_WIFI_MODE restore_mode = wifidev->wifi_mode;
            aic_sdio_host_predeinit();
            aicwf_sdio_pdn_wifi();
            aic_wifi_deinit(restore_mode);
            aicwf_sdio_reset_wifi();
            if (!aicwf_sdio_reset())
            {
                if (aic_wifi_init(restore_mode, NULL) >= 0)
                {
                    wifidev->wifi_init_status = wifidev->g_wifi_init;
                    wifi_bss_st = 0;
                    /* 仅自动恢复 AP(同模式经 wifi_switch_mode 放行);
                       STA 恢复只重配扫描信道, 由上层重新发起连接 */
                    if (restore_mode == WIFI_MODE_AP)
                    {
                        app_wlan_bss_config(1);
                    }
                    else if (restore_mode == WIFI_MODE_STA)
                    {
                        app_wlan_scan_channels_config(NULL);
                    }
                }
                else if (!wifidev->g_wifi_init)
                {
                    aicwf_driver_unexcepted(0x08);
                }
            }
        }
        break;

        default:
            break;
        }

        wifidev->wifi_drive_op = 0;
        aic_set_adap_test(0);
        wifidev->wifi_init_status = wifidev->g_wifi_init;

        if (!wifidev->g_wifi_init)
            wifi_bss_st = 0xff;
    }
}

/**
 * @brief 初始化WiFi驱动
 *
 * 创建WiFi初始化信号量和驱动管理任务。
 * 任务创建后通过 wifi_driver_action() 触发启动操作。
 */
void wifi_driver_init(void)
{
    int ret = 0;
    struct aic_wifi_dev *wifidev = &aicwf_wifi_dev;

    DBG_APP_INF("%s", __func__);


    ret = aicwf_rtos_semaphore_create_only(&wifidev->wifi_task_sem, "wifi_init_sem", 1, 0);
    if (ret)
    {
        DBG_APP_ERR("wifi task sem create fail\n");
    }

    ret = aicwf_rtos_task_create_only(aicwf_wifi_init_task, "aicwf_wifi_init_task", 0,
                                      rwnx_wifi_stack_size, NULL, rwnx_wifi_priority, &wifidev->wifi_task_init_hdl);

    if (ret || (wifidev->wifi_task_init_hdl == NULL))
    {
        DBG_APP_ERR("wifi task init create fail\n");
    }

    wifi_driver_action(0x01);
    return;
}

/**
 * @brief 触发WiFi驱动操作
 *
 * 向WiFi初始化任务发送信号量，触发指定的驱动操作。
 * 操作码定义：
 * - 0x01: 上电复位并打开
 * - 0x02: 断电并关闭
 * - 0x03: 完整复位
 * - 其他：自定义操作
 *
 * @param act 驱动操作码
 */
void wifi_driver_action(unsigned int act)
{
    struct aic_wifi_dev *wifidev = &aicwf_wifi_dev;
    if (aicwf_rtos_semaphore_get_count(wifidev->wifi_task_sem) != 0)
    {
        DBG_APP_INF("r u creazy?");
        return;
    }

    wifidev->wifi_drive_op = act;
    aicwf_rtos_semaphore_signal(wifidev->wifi_task_sem, 0);
}
