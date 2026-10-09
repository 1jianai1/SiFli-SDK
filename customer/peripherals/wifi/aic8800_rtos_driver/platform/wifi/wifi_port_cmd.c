/*
 * Copyright (C) 2018-2024 AICSemi Ltd.
 *
 * All Rights Reserved
 *
 * WiFi AT/CLI command dispatcher.
 */

#include "wifi_port_priv.h"
#include "wifi_api.h"
#include "bt_api.h"
#ifdef AIC_HCI_OVER_UART
    #include "aic_bt_uart.h"
#endif
#include "cli_api.h"
#include "netif_api.h"
#include "fmac_port.h"
#include "sdio_port.h"
#include "netif_port.h"
#include "aic_log.h"
#ifdef CONFIG_IPERF
    #include "iperf.h"
#endif
#include <string.h>
#include <stdlib.h>

/*
 * 命令前置占位符个数 (cmd 起始位置声明)。
 *
 * 有些平台的 Shell/AT 框架会在真实命令前附加一个占位 token
 * (如 "wifi iperf ..."), 此时需要跳过该占位符, 应置 1;
 * 若平台命令本身即为一行的第一个 token (如 "iperf ..."), 则置 0。
 *
 * 默认置 1, 具体依据实际移植平台而定 (见 docs/aic8800-porting/SKILL.md)。
 */
#ifndef AICWF_CMD_SKIP_PREFIX
    #define AICWF_CMD_SKIP_PREFIX  1
#endif

/**
 * @brief CLI命令响应回调，将结果通过AT接口返回
 * @param rsp 命令响应字符串
 */
static void aicwf_app_wlan_cli_cmd_cb(char *rsp)
{
    aicwf_ATRESP(0, 1, 0, rsp);
}

/**
 * @brief 解析AT命令参数字符串
 *
 * 将空格/制表符分隔的命令字符串解析为argv数组，
 * 原地修改字符串（用'\0'分隔各参数）。
 *
 * @param cmd  输入的命令字符串（会被修改）
 * @param argv 输出的参数指针数组
 * @return 参数个数（argc）
 */
static int aicwf_app_wlan_cmd_parse_argv(char *cmd, char *argv[])
{
    int last, argc;
    char *start, *end;

    argc = 0;
    start = cmd;

    while (start != '\0')
    {
        while (*start == ' ' || *start == '\t')
            start++;
        if (*start == '\0')
            break;
        end = start;
        while (*end != ' ' && *end != '\t' && *end != '\0')
            end++;
        last = *end == '\0';
        *end = '\0';
        argv[argc++] = start;
        if (last)
            break;
        start = end + 1;
    }

    return argc;
}

/**
 * @brief WiFi AT命令分发处理
 *
 * 解析AT命令字符串并分发到各子命令处理逻辑。
 * 支持的命令包括：
 * - aicrftest: RF测试
 * - switch: 模式切换
 * - stainfo/stanum/stalist: STA信息查询
 * - uapst: 状态查询
 * - mac: MAC地址读写
 * - mem: 内存信息/资源管理
 * - info: 版本和芯片信息
 * - wps: WPS操作
 * - ps/pp: 省电/功耗控制
 * - temp: 温度读取
 * - dpd: DPD（数字预失真）
 * - shut: 关断/重启控制
 * - fw: 固件版本
 * - filter: MAC过滤
 * - sta: STA连接/扫描
 * - rssi: RSSI读取
 * - ap: AP启动/停止
 * - agg: 聚合配置
 * - cca: CCA阈值设置
 * - 5g: 5GHz配置
 * - fixrate: 固定速率
 * - btstart: 蓝牙启动
 * - p2pstart: P2P启动
 * - iperf: iperf性能测试
 *
 * @param atcmd     AT命令字符串
 * @param athandle  AT接口句柄
 * @param chan_num  通道号
 */
void aic_wifi_cmd(char *atcmd, unsigned int athandle, unsigned char chan_num)
{
    int argc, ret = 0, i;
    char *argv[20];
    char rev_c[64];
    char rsp[128] = {0};
    struct aic_wifi_dev *wifidev = &aicwf_wifi_dev;

    DBG_APP_INF("%s %s\n", __func__, atcmd);
    if (os_strlen(atcmd) > 64)
    {
        DBG_APP_INF("%s too long\n", __func__);
        goto end;
    }

    memset(rev_c, 0, 64);
    memcpy(rev_c, atcmd, os_strlen(atcmd));

    argc = aicwf_app_wlan_cmd_parse_argv(atcmd, &argv[0]);
    if (argc == 0)
    {
        DBG_APP_ERR("cmd error\n");
        goto end;
    }
    /* 跳过命令前置占位符, 使 argv[0] 指向真实命令名 */
    for (i = 0; i < argc - AICWF_CMD_SKIP_PREFIX; i++)
    {
        argv[i] = argv[i + AICWF_CMD_SKIP_PREFIX];
    }
    argc -= AICWF_CMD_SKIP_PREFIX;

    //rf test
    if (os_strcmp(argv[0], "aicrftest") == 0)
    {
        if (wifidev->wifi_mode == WIFI_MODE_RFTEST)
        {
            if (argc > 1)
            {
                ret = aic_cli_run_cmd(rev_c + os_strlen("aicrftest") + 1);
            }
        }
        else
        {
            aicwf_ATRESP(athandle, 1, 0, "rf bin is not exist or not in productmode!!!");
        }
    }
    else if (os_strcmp(argv[0], "switch") == 0)
    {
        AIC_WIFI_MODE mode = WIFI_MODE_UNKNOWN;
        if (os_strcmp(argv[1], "ap") == 0)
            mode =  WIFI_MODE_AP;
        else if (os_strcmp(argv[1], "test") == 0)
            mode =  WIFI_MODE_RFTEST;
        else if (os_strcmp(argv[1], "sta") == 0)
            mode =  WIFI_MODE_STA;

        if ((mode == WIFI_MODE_RFTEST) && !aicwf_rftest_fw_available())
        {
            aicwf_ATRESP(athandle, 1, 0, "rf bin is not exist!!!");
        }
        else
        {
            sprintf(rsp, "set OK wait 2~3s switch %d to %d ", wifidev->wifi_mode, mode);
            wifi_set_mode(mode);
            wifi_driver_action(0x03);
            aicwf_ATRESP(athandle, 1, 0, rsp);
        }
    }
    else if (os_strcmp(argv[0], "stainfo") == 0)
    {
        sta_list stal;
        uint8_t n;

        int ret = app_wlan_sta_list(&stal);

        if (ret == 0)
        {
            for (n = 0; n < stal.sta_count; n++)
            {
                sprintf(rsp, "addr:%x:%x:%x", stal.info[n].mac_address[0], stal.info[n].mac_address[1], stal.info[n].mac_address[2]);
                aicwf_ATRESP(athandle, 1, 0, rsp);
            }

            sprintf(rsp, "sta:n:%d", stal.sta_count);
            aicwf_ATRESP(athandle, 1, 0, rsp);
        }
        else
        {
            aicwf_ATRESP(athandle, 1, 0, "read list fail");
        }
    }
    else if (os_strcmp(argv[0], "uapst") == 0)
    {
        uint8_t st = GetWifiStatus();
        uint8_t ist = GetWiFiInitStat();

        sprintf(rsp, "wifi st:%d init:%d\n", st, ist);
        aicwf_ATRESP(athandle, 1, 0, rsp);
        return;
    }
    else if (os_strcmp(argv[0], "mac") == 0)
    {
        if (GetWiFiInitStat())
        {
            char mac[6];
            char rsp[32] = {0};
            int type = strtoul(argv[1], NULL, 10);
            if (type == 0)
            {
                GetMACAddr((uint8_t *)mac);
                if (argc == 8)
                {
                    mac[0] = (char)strtoul(argv[2], NULL, 16);
                    mac[1] = (char)strtoul(argv[3], NULL, 16);
                    mac[2] = (char)strtoul(argv[4], NULL, 16);
                    mac[3] = (char)strtoul(argv[5], NULL, 16);
                    mac[4] = (char)strtoul(argv[6], NULL, 16);
                    mac[5] = (char)strtoul(argv[7], NULL, 16);
                    aic_set_mac_address((uint8_t *)mac);
                    sprintf(rsp, "mac:%x:%x:%x:%x:%x:%x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                    aicwf_ATRESP(athandle, 1, 0, rsp);
                }
                else
                {
                    aicwf_ATRESP(athandle, 1, 0, "param err");
                }
            }
            else if (type == 1)
            {
                memcpy(mac, aic_get_mac_address(), 6);
                sprintf(rsp, "mac:%x:%x:%x:%x:%x:%x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                aicwf_ATRESP(athandle, 1, 0, rsp);
            }
            else
            {
                aicwf_ATRESP(athandle, 1, 0, "param err");
            }
        }
        else
            aicwf_ATRESP(athandle, 1, 0, "wifi is not init");
    }
    else if (os_strcmp(argv[0], "stanum") == 0)
    {
        int n;
        n = app_wlan_get_sta_num();
        sprintf(rsp, "n:%d\n", n);
        aicwf_ATRESP(athandle, 1, 0, rsp);
    }
    else if (os_strcmp(argv[0], "mem") == 0)
    {
#ifdef CONFIG_RTOS_RESLIST
        uint32_t mcnt, fcnt = 0, rcnt;
        static void *test_p = NULL;
        mcnt = strtoul(argv[1], NULL, 10);
        if (mcnt == 0)
        {
            aicwf_rtos_mem_cur_cnt(&mcnt, &fcnt, &rcnt);
            sprintf(rsp, "mem u:%d  m:%d f:%d\n", mcnt - fcnt, mcnt, fcnt);
            if (test_p)
                aicwf_rtos_free(test_p);
            aicwf_ATRESP(athandle, 1, 0, rsp);
        }
        else if (mcnt == 1)
        {
            aicwf_rtos_res_list_info_show();
            aicwf_ATRESP(athandle, 1, 0, "ok see log");
            return;
        }
        else if (mcnt == 2)
        {
            aicwf_rtos_remove_all();
            aicwf_ATRESP(athandle, 1, 0, "ok see log");
            return;
        }
        else
        {
            test_p = aicwf_rtos_malloc(mcnt);
        }

        if (mcnt > fcnt)
            sprintf(rsp, "mem u:%d  m:%d f:%d %p\n", mcnt - fcnt, mcnt, fcnt, test_p);
        else
            sprintf(rsp, "mem u:%d  m:%d f:%d %p\n", mcnt + (0xFFFFFFFF - fcnt), mcnt, fcnt, test_p);
        aicwf_ATRESP(athandle, 1, 0, rsp);
#else
        aicwf_ATRESP(athandle, 1, 0, rsp);
#endif
    }
    else if (os_strcmp(argv[0], "info") == 0)
    {
        uint8_t info[3] = {0};
        char ver[64] = {0};
        aic_wifi_sw_version(ver);
        aic_get_chip_info(info);
        sprintf(rsp, "ver:%s cid:0x%x(%s) sid:%d fmat:%d mode:%d\n", ver, info[0], (info[0] & 0xc0) ? "H" : "T", info[1], info[2], wifidev->wifi_mode);
        aicwf_ATRESP(athandle, 1, 0, rsp);
    }
#ifdef CONFIG_STATION
    else if (os_strcmp(argv[0], "apinfo") == 0)
    {
        struct aic_connected_ap_info_t *apinfo = aic_wifi_get_sta_connected_ap_info();
        if (apinfo)
        {
            sprintf(rsp, "ssid:%.32s bssid:%02x:%02x:%02x:%02x:%02x:%02x freq:%u band:%u fmt:%u rate:%u",
                    apinfo->ssid,
                    apinfo->bssid[0], apinfo->bssid[1], apinfo->bssid[2],
                    apinfo->bssid[3], apinfo->bssid[4], apinfo->bssid[5],
                    apinfo->freq, apinfo->band, apinfo->format, apinfo->max_rate);
            aicwf_ATRESP(athandle, 1, 0, rsp);
        }
        else
        {
            aicwf_ATRESP(athandle, 1, 0, "Conn AP info unavailable");
        }
    }
#endif
    else if (os_strcmp(argv[0], "wps") == 0)
    {
        if (os_strcmp(argv[1], "pbc") == 0)
        {
            app_wlan_wps_use_pbc();
        }
        else if (os_strcmp(argv[1], "pin") == 0)
        {
            app_wlan_wps_use_pin(argv[2]);
        }
        else if (os_strcmp(argv[1], "cancel") == 0)
        {
            app_wlan_wps_use_cancel();
        }
        else if (os_strcmp(argv[1], "disable") == 0)
        {
            app_wlan_wps_disable();
        }
        else if (os_strcmp(argv[1], "cms") == 0)
        {
            aic_ap_wps_config_methods(argv[2]);
        }
    }
    else if (os_strcmp(argv[0], "ps") == 0)
    {
        int ret = 0, lvl = 0;
        lvl = strtoul(argv[1], NULL, 10);
        ret = aic_wifi_set_ps_lvl(lvl);
        if (!ret)
        {
            sprintf(rsp, "ap_ps lvl:%d, ret:%d\n", lvl, ret);
            aicwf_ATRESP(athandle, 1, 0, rsp);
        }
        else
        {
            aicwf_ATRESP(athandle, 1, 0, "+ERR\n");
        }
    }
    else if (os_strcmp(argv[0], "pp") == 0)
    {
        int lv = strtoul(argv[1], NULL, 10);
        if (lv <= 8)
        {
            aicwf_hal_switchPP(lv);
            sprintf(rsp, "pp set: %d,%d\n", lv, ret);
        }
        else
        {
            sprintf(rsp, "pp err: %d,%d\n", lv, ret);
        }
        aicwf_ATRESP(athandle, 1, 0, rsp);
    }
    else if (os_strcmp(argv[0], "temp") == 0)
    {
        int32_t temp = 0;
        ret = aic_wifi_get_temp(&temp);
        if (!ret)
            sprintf(rsp, "get chip temp: %d\n", temp);
        else
            sprintf(rsp, "get chip temp err: %d\n", ret);
        aicwf_ATRESP(athandle, 1, 0, rsp);
    }
#if (defined(CONFIG_AIC8800DC) || defined(CONFIG_AIC8800DW))
    else if (os_strcmp(argv[0], "dpd") == 0)
    {
        uint32_t result = aic_get_dpd_result();
#ifdef CONFIG_DPD
        sprintf(rsp, "dpd result: %x\n", result);
        aicwf_ATRESP(athandle, 1, 0, rsp);
#else
        aicwf_ATRESP(athandle, 1, 0, "+ERR");
#endif
    }
#endif
    else if (os_strcmp(argv[0], "shut") == 0)
    {
        uint32_t op;
        op = strtoul(argv[1], NULL, 10);
        if (op == 222)
        {
            sdio_unexcept_test = 1;
        }
        else if (op == 666)
        {
            sdio_unexcept_test = 2;
        }
        else if (op == 999)
        {
            sdio_unexcept_test = 0;
        }
        else
        {
            wifi_driver_action(op);
        }

        sprintf(rsp, "+OK: %d %d\n", op, sdio_unexcept_test);
        aicwf_ATRESP(athandle, 1, 0, rsp);
    }
    else if (os_strcmp(argv[0], "fw") == 0)
    {
        sprintf(rsp, "fw ver: %s\n", aic_get_fw_ver());
        aicwf_ATRESP(athandle, 1, 0, rsp);
    }
    else if (os_strcmp(argv[0], "filter") == 0)
    {
        uap_mac_filter filter;
        int fmode = strtoul(argv[1], NULL, 10);

        filter.filter_mode = fmode;
        filter.mac_count = 1;
        filter.mac_list[0][0] = strtoul(argv[2], NULL, 16);
        filter.mac_list[0][1] = strtoul(argv[3], NULL, 16);
        filter.mac_list[0][2] = strtoul(argv[4], NULL, 16);
        filter.mac_list[0][3] = strtoul(argv[5], NULL, 16);
        filter.mac_list[0][4] = strtoul(argv[6], NULL, 16);
        filter.mac_list[0][5] = strtoul(argv[7], NULL, 16);

        app_wlan_params_set_filter_only(&filter, NULL);
    }
    else if (os_strcmp(argv[0], "sta") == 0)
    {
        uint32_t ip;
        uint8_t ipb[4];

        if (os_strcmp(argv[1], "conn") == 0)
        {
            uint8_t *ssid = (uint8_t *)argv[2];
            uint8_t *pw = (uint8_t *)argv[3];
            uint16_t to = strtoul(argv[4], NULL, 10);
            ret = app_wlan_sta_start(ssid, pw, to);

            if (ret >= 0)
            {
                ip = app_wlan_sta_get_ip();
                memcpy(ipb, &ip, 4);
                sprintf(rsp, "+OK ip: %u:%u:%u:%u\n", ipb[0], ipb[1], ipb[2], ipb[3]);
                aicwf_ATRESP(athandle, 1, 0, rsp);
            }
            else
            {
                aicwf_ATRESP(athandle, 1, 0, "+ERR");
            }
            return;
        }
        else if (os_strcmp(argv[1], "scan") == 0)
        {
            ret = app_wlan_scan_start();
            if (ret >= 0)
            {
                app_wlan_sta_show_scan_res();
                aicwf_ATRESP(athandle, 1, 0, "+OK");
            }
            else
            {
                aicwf_ATRESP(athandle, 1, 0, "+ERR");
            }
            return;

        }
        else if (os_strcmp(argv[1], "stop") == 0)
        {
            app_wlan_sta_stop();
        }
    }
    else if (os_strcmp(argv[0], "rssi") == 0)
    {
        int rssi = aic_wifi_ap_rssi_get();
        sprintf(rsp, "Conn AP rssi: %d\n", rssi);
        aicwf_ATRESP(athandle, 1, 0, rsp);
    }
    else if (os_strcmp(argv[0], "ap") == 0)
    {
        if (os_strcmp(argv[1], "start") == 0)
        {
            wifi_ap_cfg.band = strtoul(argv[2], NULL, 10);
            wifi_ap_cfg.type = strtoul(argv[3], NULL, 10);
            wifi_ap_cfg.enable_he = strtoul(argv[4], NULL, 10);
            wifi_ap_cfg.enable_acs = strtoul(argv[5], NULL, 10);
            wifi_ap_cfg.channel =  strtoul(argv[6], NULL, 10);
            wifi_ap_cfg.sercurity_type = strtoul(argv[7], NULL, 10);
            /* 成功时由 app_wlan_bss_config 内部置 wifi_mode=AP, 失败不污染模式状态 */
            app_wlan_bss_config(1);
        }
        else if (os_strcmp(argv[1], "stop") == 0)
        {
            app_wlan_bss_config(0);
        }
        else
        {
            aicwf_ATRESP(athandle, 1, 0, "+ERR");
        }
    }
    else if (os_strcmp(argv[0], "stalist") == 0)
    {
        aic_wifi_get_stalist2str(rsp);
        aicwf_ATRESP(athandle, 1, 0, rsp);
    }
    else if (os_strcmp(argv[0], "agg") == 0)
    {
        int en, id;
        en = strtoul(argv[1], NULL, 10);
        id = strtoul(argv[2], NULL, 10);
        aic_wifi_set_agg(en, id);
    }
    else if (os_strcmp(argv[0], "bare_bypass") == 0)
    {
        /* [backport EC] 纯 TCP ACK 重排旁路运行时开关 (rwnx_utils.c) */
        extern uint8_t aicwifi_bare_bypass;
        int value = strtoul(argv[1], NULL, 10);
        if (value <= 1)
        {
            aicwifi_bare_bypass = value;
            sprintf(rsp, "+OK bare_bypass:%d\n", aicwifi_bare_bypass);
        }
        else
        {
            sprintf(rsp, "+ERR bare_bypass:input err:%d\n", value);
        }
        aicwf_ATRESP(athandle, 1, 0, rsp);
    }
    else if (os_strcmp(argv[0], "cca") == 0)
    {
        extern int aic_wifi_set_cca_threshold_param(struct aic_mm_set_cca_threshold_req * req);
        struct aic_mm_set_cca_threshold_req reqt;
        int r, f;
        r = strtol(argv[1], NULL, 10);
        f = strtol(argv[2], NULL, 10);
        reqt.auto_cca_en = 0;
        reqt.hwconfig_id = 0;
        reqt.cca20p_rise_th = r;
        reqt.cca20s_fall_th = r;
        reqt.cca20p_fall_th = f;
        reqt.cca20s_fall_th = f;

        aic_wifi_set_cca_threshold_param(&reqt);

        sprintf(rsp, "+OK r:%d f:%d\n", r, f);
        aicwf_ATRESP(athandle, 1, 0, rsp);
    }
    else if (os_strcmp(argv[0], "5g") == 0)
    {
        uap_config_param *uap_cfg = (uap_config_param *)aicwf_rtos_malloc(sizeof(uap_config_param));
        uap_config_scan_chan *scan_param = (uap_config_scan_chan *)aicwf_rtos_malloc(sizeof(uap_config_scan_chan));

        memset(uap_cfg, 0, sizeof(uap_config_param));
        memset(scan_param, 0, sizeof(uap_config_scan_chan));

#define AP_SSID_5G  "BTDs-5G"
#define AP_PASS_5G  "12345678"

        memcpy(uap_cfg->ssid, AP_SSID_5G, strlen(AP_SSID_5G));
        uap_cfg->ssid_len = strlen(AP_SSID_5G);
        memcpy(uap_cfg->wpa_cfg.passphrase, AP_PASS_5G, strlen(AP_PASS_5G));
        uap_cfg->wpa_cfg.length = strlen(AP_PASS_5G);
        uap_cfg->bcast_ssid_ctrl = 1;
        uap_cfg->action = UAP_ACTION_SET;
        uap_cfg->mode = UAP_BAND_AX;
        uap_cfg->acs_mode = 0;
        uap_cfg->channel = 40;
        uap_cfg->rf_band = UAP_RF_50_G;
        uap_cfg->channel_bandwidth = UAP_CHAN_WIDTH_40MHZ;
        uap_cfg->sec_chan = UAP_SEC_CHAN_ABOVE;
        uap_cfg->max_sta_count = 12;
        uap_cfg->protocol = UAP_PROTOCOL_WPA;
        uap_cfg->wpa_cfg.group_cipher = 1;

        scan_param->action = ACTION_SET;
        scan_param->band = UAP_RF_50_G;
        scan_param->total_chan = 13;
        scan_param->chan_num[0] = 36;
        scan_param->chan_num[1] = 40;
        scan_param->chan_num[2] = 44;
        scan_param->chan_num[3] = 48;
        scan_param->chan_num[4] = 52;
        scan_param->chan_num[5] = 56;
        scan_param->chan_num[6] = 60;
        scan_param->chan_num[7] = 64;
        scan_param->chan_num[8] = 149;
        scan_param->chan_num[9] = 153;
        scan_param->chan_num[10] = 157;
        scan_param->chan_num[11] = 161;
        scan_param->chan_num[12] = 165;

        app_wlan_scan_channels_config(scan_param);
        aicwf_rtos_free(scan_param);
        aicwf_rtos_free(uap_cfg);
    }
    else if (os_strcmp(argv[0], "fixrate") == 0)
    {
        extern int aic_wifi_fixrate(int argc, int argv[]);
        int param[4];
        param[0] = strtoul(argv[1], NULL, 10);
        param[1] = strtoul(argv[2], NULL, 10);
        param[2] = strtoul(argv[3], NULL, 10);
        param[3] = strtoul(argv[4], NULL, 10);

        aic_wifi_fixrate(4, param);

        sprintf(rsp, "+OK fixrate:%d %d %d %d\n", param[0], param[1], param[2], param[3]);
        aicwf_ATRESP(athandle, 1, 0, rsp);
    }
#ifdef AIC_FEATURE_BT
    else if (os_strcmp(argv[0], "btstart") == 0)
    {
#ifdef AIC_HCI_OVER_UART
        /* UART HCI 后端入口 (aic_hci.c 编译期映射 hci_tr_*): 打开 UART + 拉起
         * H4 重组任务, 幂等; NimBLE 路径下 aic_hci_init 也会经 hci_tr_start 到此 */
        (void)aic_bt_uart_hci_start();
#endif
#if defined(AIC_FEATURE_NIMBLE)
        /* SDIO/UART 邮箱路径下 BLE 栈由 wifi_port_plt 随 WiFi bring-up 自动拉起;
         * 此处兼顾 btstop 之后的手动重启 (nimble_stack_start 幂等) */
        extern int nimble_stack_start(void);
        (void)nimble_stack_start();
#endif
        aicwf_ATRESP(athandle, 1, 0, "+OK");
    }
    else if (os_strcmp(argv[0], "btstop") == 0)
    {
#if defined(AIC_FEATURE_NIMBLE)
        /* 停 BLE: 停广播 -> 退宿主任务 -> 停 HCI RX, 并复位幂等标志使可重启。
         * 只动 BLE, 不影响 WiFi (共用 fhost_rx_task 由 wlan_if.c 侧保护)。 */
        extern int nimble_stack_stop(void);
        aicwf_ATRESP(athandle, 1, 0, (nimble_stack_stop() == 0) ? "+OK" : "+ERR");
#else
        aicwf_ATRESP(athandle, 1, 0, "+ERR");
#endif
    }
#endif
#ifdef CONFIG_P2P_SUPPORT
    else if (os_strcmp(argv[0], "p2pstart") == 0)
    {
        char *name = argv[1];
        char *passphrase = argv[2];
        char *ssid = argv[3];

        if (argc > 3)
        {
            int ret = app_wlan_p2p_start(name, ssid, passphrase);
            if (ret == 0)
            {
                aicwf_ATRESP(athandle, 1, 0, "+OK");
            }
            else
            {
                aicwf_ATRESP(athandle, 1, 0, "+ERR");
            }
        }
        else
        {
            app_wlan_p2p_stop();
        }
    }
#endif
#ifdef CONFIG_IPERF
    else if (os_strcmp(argv[0], "iperf") == 0)
    {
        if (os_strcmp(argv[1], "start") == 0)
        {
            int handle;
            handle = iperf_parse_argv(argc - 1, &argv[1]);
            net_if_t *netif = aicwf_net_if_get(0);
            if (handle < 0)
            {
                aicwf_ATRESP(athandle, 1, 0, "+ERR");
            }
            else
            {
                iperf_start((struct netif *)netif, handle);
            }
        }
        else if (os_strcmp(argv[1], "stop") == 0)
        {
            iperf_stop("a");
        }
    }
#endif

end:
    aicwf_ATRESP(athandle, 1, ret, "aic cmd finish");
}
