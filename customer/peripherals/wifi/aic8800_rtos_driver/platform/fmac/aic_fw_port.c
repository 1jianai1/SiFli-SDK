/*
 * Copyright (C) 2018-2024 AICSemi Ltd.
 *
 * All Rights Reserved
 *
 * Platform firmware data provider & loader - merged from aic_fw.c + fmac_port.c.
 * Provides firmware binary data to host driver through aic_fw_port interface,
 * and implements firmware loading strategies (filemode / flashmode / innermode).
 */

#include <stdint.h>
#include "rtos_port.h"
#include "aic_cfg.h"
#include "aic_log.h"
#include "sys_api.h"
#include "wifi_api.h"
#include "aic_fw_port.h"
#include "wifi_def.h"

#if CONFIG_LOAD_FW == 2   /* innermode: 固件内嵌为 C 数组, 才需要固件数组头文件 */
    /* WiFi firmware binary headers */
    #if defined(CONFIG_AIC8801)
        #include "fw/fmacfw.h"
        #if defined(CONFIG_WIFI_MODE_RFTEST)
            #include "fw/fmacfw_rf.h"
        #endif
    #endif

    #if defined(CONFIG_AIC8800DC) || defined(CONFIG_AIC8800DW)
        #if !defined(CONFIG_AIC8800DC_H) && !defined(CONFIG_AIC8800DW_H)
            /* DC/DW/DL base (sub_id 0/1) firmware */
            #include "fw/fmacfw_patch_8800dc_u02.h"
            #include "fw/fmacfw_patch_tbl_8800dc_u02.h"
            #include "fw/fmacfw_calib_8800dc_u02.h"
        #else
            #if defined(AIC_HCI_OVER_SDIO)
                /* DC_H mailbox HCI needs the hbt fmac (plain h_u02 has no TDLS_SDIO_BT). */
                #include "fw/fmacfw_patch_8800dc_hbt_u02.h"
                #include "fw/fmacfw_patch_tbl_8800dc_hbt_u02.h"
                #include "fw/fmacfw_calib_8800dc_hbt_u02.h"
                #define fmacfw_patch_8800dc_h_u02      fmacfw_patch_8800dc_hbt_u02
                #define fmacfw_patch_tbl_8800dc_h_u02  fmacfw_patch_tbl_8800dc_hbt_u02
                #define fmacfw_calib_8800dc_h_u02      fmacfw_calib_8800dc_hbt_u02
            #else
                /* DC_H/DW_H/DL_H (sub_id 2) firmware — 节省 Flash, 不含 non-H 固件 */
                #include "fw/fmacfw_patch_8800dc_h_u02.h"
                #include "fw/fmacfw_patch_tbl_8800dc_h_u02.h"
                #include "fw/fmacfw_calib_8800dc_h_u02.h"
            #endif
        #endif
        #if defined(CONFIG_WIFI_MODE_RFTEST)
            #include "fw/lmacfw_rf_8800dc.h"
        #endif
    #endif

    #if defined(CONFIG_AIC8800D80)
        #if !defined(CONFIG_AIC8800D80_H)
            /* D80 base (U01/U02/U03) firmware */
            #if defined(AIC_HCI_OVER_SDIO)
                /* SDIO 邮箱 HCI 需要带 BT 的 fmac: 普通 d80_u02 无 TDLS_SDIO_BT 消息,
                * 下发 HCI 会被 FW ke_task_handler_get 断言 (idx < TASK_DESC[type].idx_max)
                * (对齐 Linux SDIO 驱动 aic_bsp_main.c 的 CONFIG_SDIO_BT 分支) */
                #include "fw/fmacfwbt_8800d80_u02.h"
                #define fmacfw_8800d80_u02      fmacfwbt_8800d80_u02
            #else
                #include "fw/fmacfw_8800d80_u02.h"
            #endif
            #if defined(CONFIG_WIFI_MODE_RFTEST)
                #include "fw/lmacfw_rf_8800d80_u02.h"
            #endif
        #else
            /* D80_H (H version) firmware — 按 总线 + IPC 场景区分 (对齐 Linux) */
            #if defined(CONFIG_USB_SUPPORT)
                /* USB H 固件 (与 SDIO 二进制不同) */
                #if defined(CONFIG_FOR_IPCAM)
                    #include "fw/fmacfw_8800d80_h_u02_usb_ipc.h"
                #else
                    #include "fw/fmacfw_8800d80_h_u02_usb.h"
                #endif
            #else
                /* SDIO H 固件 */
                #if defined(CONFIG_FOR_IPCAM)
                    #include "fw/fmacfw_8800d80_h_u02_ipc.h"
                #else
                    #include "fw/fmacfw_8800d80_h_u02.h"
                #endif
            #endif
            #if defined(CONFIG_WIFI_MODE_RFTEST)
                #include "fw/lmacfw_rf_8800d80_u02.h"
            #endif
        #endif
    #endif

    #if defined(CONFIG_AIC8800D80N)
        /* D80N WiFi 固件: patch_load 多阶段加载 (参考 Linux aic8800d80n_compat.c) */
        /* U02 (chip_sub_id==1) — 当前芯片使用版本 */
        #include "fw/fmacfw_patch_8800d80n_u02.h"
        #include "fw/fmacfw_patch_tbl_8800d80n_u02.h"
        #include "fw/fmacfw_cinit_8800d80n_u02.h"
        #include "fw/fmacfw_calib_8800d80n_u02.h"
        #if defined(CONFIG_AIC8800D80N_U02C)
            /* U02C (chip_sub_id!=1) — 仅在 CONFIG_AIC8800D80N_U02C 定义时编译 */
            #include "fw/fmacfw_patch_8800d80n_u02c.h"
            #include "fw/fmacfw_patch_tbl_8800d80n_u02c.h"
            #include "fw/fmacfw_cinit_8800d80n_u02c.h"
            #include "fw/fmacfw_calib_8800d80n_u02c.h"
        #endif /* CONFIG_AIC8800D80N_U02C */
        #if defined(CONFIG_WIFI_MODE_RFTEST)
            #include "fw/lmacfw_rf_8800d80n.h"
            #include "fw/lmacfw_rf_8800d80n_i.h"
        #endif
    #endif

    #if defined(CONFIG_AIC8800D80X2)
        /* D80X2 (U05 基线) WiFi 固件 */
        #include "fw/fmacfw_8800d80x2.h"
        #if defined(CONFIG_WIFI_MODE_RFTEST)
            #include "fw/lmacfw_rf_8800d80x2.h"
        #endif
    #endif

    /* BT firmware binary headers */
    #ifdef CONFIG_BT_SUPPORT
        #if defined(CONFIG_AIC8801)
            #include "fw/fw_patch_u03.h"
            #include "fw/fw_patch_table_u03.h"
            #include "fw/fw_adid_u03.h"
        #endif
        #if defined(CONFIG_AIC8800D80)
            #include "fw/fw_adid_8800d80_u02.h"
            #include "fw/fw_patch_8800d80_u02.h"
            #include "fw/fw_patch_table_8800d80_u02.h"
            #include "fw/fw_patch_8800d80_u02_ext0.h"
            #include "fw/fw_patch_8800d80_u02_ext1.h"   /* BT patch 表 id=1 的 ext patch */
        #endif
        #if defined(CONFIG_AIC8800D80N)
            #include "fw/fw_adid_8800d80n_u02.h"
            #include "fw/fw_patch_8800d80n_u02.h"
            #include "fw/fw_patch_table_8800d80n_u02.h"
            #include "fw/fw_patch_8800d80n_u02_ext0.h"
            #if defined(CONFIG_AIC8800D80N_U02C)
                #include "fw/fw_adid_8800d80n_u02c.h"
                #include "fw/fw_patch_8800d80n_u02c.h"
                #include "fw/fw_patch_table_8800d80n_u02c.h"
                #include "fw/fw_patch_8800d80n_u02c_ext0.h"
            #endif /* CONFIG_AIC8800D80N_U02C */
            #if defined(CONFIG_WIFI_MODE_RFTEST)
                /* D80N RFTEST BT 固件 (Linux CPMODE_TEST 专用 _rf_ 变体) */
                #include "fw/fw_patch_8800d80n_rf_u02.h"
                #include "fw/fw_patch_8800d80n_rf_u02_ext0.h"
                #include "fw/fw_patch_table_8800d80n_rf_u02.h"
                #if defined(CONFIG_AIC8800D80N_U02C)
                    #include "fw/fw_patch_8800d80n_rf_u02c.h"
                    #include "fw/fw_patch_8800d80n_rf_u02c_ext0.h"
                    #include "fw/fw_patch_table_8800d80n_rf_u02c.h"
                #endif /* CONFIG_AIC8800D80N_U02C */
            #endif /* CONFIG_WIFI_MODE_RFTEST */
        #endif
        #if defined(CONFIG_AIC8800D80X2)
            #include "fw/fw_adid_8800d80x2_u05.h"
            #include "fw/fw_patch_8800d80x2_u05.h"
            #include "fw/fw_patch_table_8800d80x2_u05.h"
            #include "fw/fw_patch_8800d80x2_u05_ext0.h"
            #include "fw/fw_patch_8800d80x2_u05_ext1.h"
        #endif
        #if defined(CONFIG_AIC8800DC_H)
            #include "fw/fw_adid_8800dc_u02h.h"
            #include "fw/fw_patch_8800dc_u02h.h"
            #include "fw/fw_patch_table_8800dc_u02h.h"
        #endif
    #endif /* CONFIG_BT_SUPPORT */
#endif /* CONFIG_LOAD_FW == 2 */

/* ==========================================================================
 * Firmware data provider (from former aic_fw.c)
 * ========================================================================== */

/**
 * @brief 获取固件数据指针
 *
 * 根据固件ID返回编译时嵌入的固件数组指针。
 * 支持WiFi固件（各芯片型号）和BT固件。
 *
 * @param id 固件类型ID
 * @return 固件数据指针, NULL=未找到
 */
#if CONFIG_LOAD_FW == 2
void *aic_fw_ptr_get(enum aic_fw id)
{
    switch (id)
    {
#if defined(CONFIG_AIC8801)
    case FMACFW:
        return (void *)fmacfw;
#if defined(CONFIG_WIFI_MODE_RFTEST)
    case FMACFW_RF:
        return (void *)fmacfw_rf;
#endif
#endif

#if defined(CONFIG_AIC8800DC) || defined(CONFIG_AIC8800DW)
    case FMACFW_PATCH_8800DC:
        return NULL;
#if !defined(CONFIG_AIC8800DC_H) && !defined(CONFIG_AIC8800DW_H)
    case FMACFW_PATCH_8800DC_U02:
        return (void *)fmacfw_patch_8800dc_u02;
    case FMACFW_PATCH_TBL_8800DC_U02:
        return (void *)fmacfw_patch_tbl_8800dc_u02;
    case FMACFW_CALIB_8800DC_U02:
        return (void *)fmacfw_calib_8800dc_u02;
#else
    case FMACFW_PATCH_8800DC_H_U02:
        return (void *)fmacfw_patch_8800dc_h_u02;
    case FMACFW_PATCH_TBL_8800DC_H_U02:
        return (void *)fmacfw_patch_tbl_8800dc_h_u02;
    case FMACFW_CALIB_8800DC_H_U02:
        return (void *)fmacfw_calib_8800dc_h_u02;
#endif
#if defined(CONFIG_WIFI_MODE_RFTEST)
    case LMACFW_RF_8800DC:
        return (void *)lmacfw_rf_8800dc;
    case FMACFW_RF_PATCH_8800DC:
        return NULL;
#endif
#endif

#if defined(CONFIG_AIC8800D80)
#if !defined(CONFIG_AIC8800D80_H)
    case FMACFW_8800D80:
        return NULL;
    case FMACFW_8800D80_U02:
        return (void *)fmacfw_8800d80_u02;
#if defined(CONFIG_WIFI_MODE_RFTEST)
    case FMACFW_RF_8800D80_U02:
        return (void *)lmacfw_rf_8800d80_u02;
#endif
#if defined(CONFIG_WIFI_MODE_RFTEST)
    case FMACFW_RF_8800D80:
        return NULL;
#endif
#else
#if defined(CONFIG_USB_SUPPORT)
    case FMACFW_8800D80_H_U02_USB:
        return (void *)fmacfw_8800d80_h_u02_usb;
#if defined(CONFIG_FOR_IPCAM)
    case FMACFW_8800D80_H_U02_USB_IPC:
        return (void *)fmacfw_8800d80_h_u02_usb_ipc;
#endif
#else
    case FMACFW_8800D80_H_U02:
        return (void *)fmacfw_8800d80_h_u02;
#if defined(CONFIG_FOR_IPCAM)
    case FMACFW_8800D80_H_U02_IPC:
        return (void *)fmacfw_8800d80_h_u02_ipc;
#endif
#endif
#if defined(CONFIG_WIFI_MODE_RFTEST)
    case FMACFW_RF_8800D80_U02:
        return (void *)lmacfw_rf_8800d80_u02;
#endif
#endif
#endif

#if defined(CONFIG_AIC8800D80N)
    /* U02 (当前芯片版本) */
    case FMACFW_PATCH_8800D80N_U02:
        return (void *)fmacfw_patch_8800d80n_u02;
    case FMACFW_PATCH_TBL_8800D80N_U02:
        return (void *)fmacfw_patch_tbl_8800d80n_u02;
    case FMACFW_CINIT_8800D80N_U02:
        return (void *)fmacfw_cinit_8800d80n_u02;
    case FMACFW_CALIB_8800D80N_U02:
        return (void *)fmacfw_calib_8800d80n_u02;
        /* U02C — 仅在 CONFIG_AIC8800D80N_U02C 定义时编译 */
#if defined(CONFIG_AIC8800D80N_U02C)
    case FMACFW_PATCH_8800D80N_U02C:
        return (void *)fmacfw_patch_8800d80n_u02c;
    case FMACFW_PATCH_TBL_8800D80N_U02C:
        return (void *)fmacfw_patch_tbl_8800d80n_u02c;
    case FMACFW_CINIT_8800D80N_U02C:
        return (void *)fmacfw_cinit_8800d80n_u02c;
    case FMACFW_CALIB_8800D80N_U02C:
        return (void *)fmacfw_calib_8800d80n_u02c;
#endif /* CONFIG_AIC8800D80N_U02C */
#if defined(CONFIG_WIFI_MODE_RFTEST)
    case LMACFW_RF_8800D80N:
        return (void *)lmacfw_rf_8800d80n;
    /* 8819i (chip_id 高 2 位 = 0xC0) RF 测试固件 */
    case LMACFW_RF_8800D80N_I:
        return (void *)lmacfw_rf_8800d80n_i;
#endif
#endif

#if defined(CONFIG_AIC8800D80X2)
    case FMACFW_8800D80X2:
        return (void *)fmacfw_8800d80x2;
#if defined(CONFIG_WIFI_MODE_RFTEST)
    case LMACFW_RF_8800D80X2:
        return (void *)lmacfw_rf_8800d80x2;
#endif
#endif

#ifdef CONFIG_BT_SUPPORT
#if defined(CONFIG_AIC8801)
    case FW_ADID_U03:
        return (void *)fw_adid_u03;
    case FW_PATCH_U03:
        return (void *)fw_patch_u03;
    case FW_PATCH_TABLE_U03:
        return (void *)fw_patch_table_u03;
#endif
#if defined(CONFIG_AIC8800D80)
    case FW_ADID_8800D80_U02:
        return (void *)fw_adid_8800d80_u02;
    case FW_PATCH_8800D80_U02:
        return (void *)fw_patch_8800d80_u02;
    case FW_PATCH_TABLE_8800D80_U02:
        return (void *)fw_patch_table_8800d80_u02;
    case FW_PATCH_8800D80_U02_EXT:
        return (void *)fw_patch_8800d80_u02_ext0;
    case FW_PATCH_8800D80_U02_EXT1:
        return (void *)fw_patch_8800d80_u02_ext1;
#endif
#if defined(CONFIG_AIC8800D80N)
    case FW_ADID_8800D80N_U02:
        return (void *)fw_adid_8800d80n_u02;
#if defined(CONFIG_AIC8800D80N_U02C)
    case FW_ADID_8800D80N_U02C:
        return (void *)fw_adid_8800d80n_u02c;
#endif /* CONFIG_AIC8800D80N_U02C */
    case FW_PATCH_8800D80N_U02:
        return (void *)fw_patch_8800d80n_u02;
#if defined(CONFIG_AIC8800D80N_U02C)
    case FW_PATCH_8800D80N_U02C:
        return (void *)fw_patch_8800d80n_u02c;
#endif /* CONFIG_AIC8800D80N_U02C */
    case FW_PATCH_TABLE_8800D80N_U02:
        return (void *)fw_patch_table_8800d80n_u02;
#if defined(CONFIG_AIC8800D80N_U02C)
    case FW_PATCH_TABLE_8800D80N_U02C:
        return (void *)fw_patch_table_8800d80n_u02c;
#endif /* CONFIG_AIC8800D80N_U02C */
    case FW_PATCH_8800D80N_U02_EXT:
        return (void *)fw_patch_8800d80n_u02_ext0;
#if defined(CONFIG_AIC8800D80N_U02C)
    case FW_PATCH_8800D80N_U02C_EXT:
        return (void *)fw_patch_8800d80n_u02c_ext0;
#endif /* CONFIG_AIC8800D80N_U02C */
#if defined(CONFIG_WIFI_MODE_RFTEST)
    /* D80N RFTEST BT 固件 (Linux CPMODE_TEST 专用 _rf_ 变体) */
    case FW_PATCH_8800D80N_RF_U02:
        return (void *)fw_patch_8800d80n_rf_u02;
    case FW_PATCH_8800D80N_RF_U02_EXT:
        return (void *)fw_patch_8800d80n_rf_u02_ext0;
    case FW_PATCH_TABLE_8800D80N_RF_U02:
        return (void *)fw_patch_table_8800d80n_rf_u02;
#endif
#if defined(CONFIG_WIFI_MODE_RFTEST) && defined(CONFIG_AIC8800D80N_U02C)
    case FW_PATCH_8800D80N_RF_U02C:
        return (void *)fw_patch_8800d80n_rf_u02c;
    case FW_PATCH_8800D80N_RF_U02C_EXT:
        return (void *)fw_patch_8800d80n_rf_u02c_ext0;
    case FW_PATCH_TABLE_8800D80N_RF_U02C:
        return (void *)fw_patch_table_8800d80n_rf_u02c;
#endif
#endif
#if defined(CONFIG_AIC8800D80X2)
    case FW_ADID_8800D80X2_U05:
        return (void *)fw_adid_8800d80x2_u05;
    case FW_PATCH_8800D80X2_U05:
        return (void *)fw_patch_8800d80x2_u05;
    case FW_PATCH_TABLE_8800D80X2_U05:
        return (void *)fw_patch_table_8800d80x2_u05;
    case FW_PATCH_8800D80X2_U05_EXT0:
        return (void *)fw_patch_8800d80x2_u05_ext0;
    case FW_PATCH_8800D80X2_U05_EXT1:
        return (void *)fw_patch_8800d80x2_u05_ext1;
#endif
#if defined(CONFIG_AIC8800DC_H)
    case FW_ADID_8800DC_U02H:
        return (void *)fw_adid_8800dc_u02h;
    case FW_PATCH_8800DC_U02H:
        return (void *)fw_patch_8800dc_u02h;
    case FW_PATCH_TABLE_8800DC_U02H:
        return (void *)fw_patch_table_8800dc_u02h;
#endif
#endif /* CONFIG_BT_SUPPORT */

    default:
        DBG_FMAC_INF("FW ptr: unknown id %d\n", id);
        return NULL;
    }
}

/**
 * @brief 获取固件数据大小
 * @param id 固件类型ID
 * @return 固件大小（字节）, 0=未找到
 */
uint32_t aic_fw_size_get(enum aic_fw id)
{
    switch (id)
    {
#if defined(CONFIG_AIC8801)
    case FMACFW:
        return sizeof(fmacfw);
#if defined(CONFIG_WIFI_MODE_RFTEST)
    case FMACFW_RF:
        return sizeof(fmacfw_rf);
#endif
#endif

#if defined(CONFIG_AIC8800DC) || defined(CONFIG_AIC8800DW)
    case FMACFW_PATCH_8800DC:
        return 0;
#if !defined(CONFIG_AIC8800DC_H) && !defined(CONFIG_AIC8800DW_H)
    case FMACFW_PATCH_8800DC_U02:
        return sizeof(fmacfw_patch_8800dc_u02);
    case FMACFW_PATCH_TBL_8800DC_U02:
        return sizeof(fmacfw_patch_tbl_8800dc_u02);
    case FMACFW_CALIB_8800DC_U02:
        return sizeof(fmacfw_calib_8800dc_u02);
#else
    case FMACFW_PATCH_8800DC_H_U02:
        return sizeof(fmacfw_patch_8800dc_h_u02);
    case FMACFW_PATCH_TBL_8800DC_H_U02:
        return sizeof(fmacfw_patch_tbl_8800dc_h_u02);
    case FMACFW_CALIB_8800DC_H_U02:
        return sizeof(fmacfw_calib_8800dc_h_u02);
#endif
#if defined(CONFIG_WIFI_MODE_RFTEST)
    case LMACFW_RF_8800DC:
        return sizeof(lmacfw_rf_8800dc);
    case FMACFW_RF_PATCH_8800DC:
        return 0;
#endif
#endif

#if defined(CONFIG_AIC8800D80)
#if !defined(CONFIG_AIC8800D80_H)
    case FMACFW_8800D80:
        return 0;
    case FMACFW_8800D80_U02:
        return sizeof(fmacfw_8800d80_u02);
#if defined(CONFIG_WIFI_MODE_RFTEST)
    case FMACFW_RF_8800D80_U02:
        return sizeof(lmacfw_rf_8800d80_u02);
#endif
#if defined(CONFIG_WIFI_MODE_RFTEST)
    case FMACFW_RF_8800D80:
        return 0;
#endif
#else
#if defined(CONFIG_USB_SUPPORT)
    case FMACFW_8800D80_H_U02_USB:
        return sizeof(fmacfw_8800d80_h_u02_usb);
#if defined(CONFIG_FOR_IPCAM)
    case FMACFW_8800D80_H_U02_USB_IPC:
        return sizeof(fmacfw_8800d80_h_u02_usb_ipc);
#endif
#else
    case FMACFW_8800D80_H_U02:
        return sizeof(fmacfw_8800d80_h_u02);
#if defined(CONFIG_FOR_IPCAM)
    case FMACFW_8800D80_H_U02_IPC:
        return sizeof(fmacfw_8800d80_h_u02_ipc);
#endif
#endif
#if defined(CONFIG_WIFI_MODE_RFTEST)
    case FMACFW_RF_8800D80_U02:
        return sizeof(lmacfw_rf_8800d80_u02);
#endif
#endif
#endif

#if defined(CONFIG_AIC8800D80N)
    /* U02 (当前芯片版本) */
    case FMACFW_PATCH_8800D80N_U02:
        return sizeof(fmacfw_patch_8800d80n_u02);
    case FMACFW_PATCH_TBL_8800D80N_U02:
        return sizeof(fmacfw_patch_tbl_8800d80n_u02);
    case FMACFW_CINIT_8800D80N_U02:
        return sizeof(fmacfw_cinit_8800d80n_u02);
    case FMACFW_CALIB_8800D80N_U02:
        return sizeof(fmacfw_calib_8800d80n_u02);
        /* U02C — 仅在 CONFIG_AIC8800D80N_U02C 定义时编译 */
#if defined(CONFIG_AIC8800D80N_U02C)
    case FMACFW_PATCH_8800D80N_U02C:
        return sizeof(fmacfw_patch_8800d80n_u02c);
    case FMACFW_PATCH_TBL_8800D80N_U02C:
        return sizeof(fmacfw_patch_tbl_8800d80n_u02c);
    case FMACFW_CINIT_8800D80N_U02C:
        return sizeof(fmacfw_cinit_8800d80n_u02c);
    case FMACFW_CALIB_8800D80N_U02C:
        return sizeof(fmacfw_calib_8800d80n_u02c);
#endif /* CONFIG_AIC8800D80N_U02C */
#if defined(CONFIG_WIFI_MODE_RFTEST)
    case LMACFW_RF_8800D80N:
        return sizeof(lmacfw_rf_8800d80n);
    /* 8819i (chip_id 高 2 位 = 0xC0) RF 测试固件 */
    case LMACFW_RF_8800D80N_I:
        return sizeof(lmacfw_rf_8800d80n_i);
#endif
#endif

#if defined(CONFIG_AIC8800D80X2)
    case FMACFW_8800D80X2:
        return sizeof(fmacfw_8800d80x2);
#if defined(CONFIG_WIFI_MODE_RFTEST)
    case LMACFW_RF_8800D80X2:
        return sizeof(lmacfw_rf_8800d80x2);
#endif
#endif

#ifdef CONFIG_BT_SUPPORT
#if defined(CONFIG_AIC8801)
    case FW_ADID_U03:
        return sizeof(fw_adid_u03);
    case FW_PATCH_U03:
        return sizeof(fw_patch_u03);
    case FW_PATCH_TABLE_U03:
        return sizeof(fw_patch_table_u03);
#endif
#if defined(CONFIG_AIC8800D80)
    case FW_ADID_8800D80_U02:
        return sizeof(fw_adid_8800d80_u02);
    case FW_PATCH_8800D80_U02:
        return sizeof(fw_patch_8800d80_u02);
    case FW_PATCH_TABLE_8800D80_U02:
        return sizeof(fw_patch_table_8800d80_u02);
    case FW_PATCH_8800D80_U02_EXT:
        return sizeof(fw_patch_8800d80_u02_ext0);
    case FW_PATCH_8800D80_U02_EXT1:
        return sizeof(fw_patch_8800d80_u02_ext1);
#endif
#if defined(CONFIG_AIC8800D80N)
    case FW_ADID_8800D80N_U02:
        return sizeof(fw_adid_8800d80n_u02);
#if defined(CONFIG_AIC8800D80N_U02C)
    case FW_ADID_8800D80N_U02C:
        return sizeof(fw_adid_8800d80n_u02c);
#endif /* CONFIG_AIC8800D80N_U02C */
    case FW_PATCH_8800D80N_U02:
        return sizeof(fw_patch_8800d80n_u02);
#if defined(CONFIG_AIC8800D80N_U02C)
    case FW_PATCH_8800D80N_U02C:
        return sizeof(fw_patch_8800d80n_u02c);
#endif /* CONFIG_AIC8800D80N_U02C */
    case FW_PATCH_TABLE_8800D80N_U02:
        return sizeof(fw_patch_table_8800d80n_u02);
#if defined(CONFIG_AIC8800D80N_U02C)
    case FW_PATCH_TABLE_8800D80N_U02C:
        return sizeof(fw_patch_table_8800d80n_u02c);
#endif /* CONFIG_AIC8800D80N_U02C */
    case FW_PATCH_8800D80N_U02_EXT:
        return sizeof(fw_patch_8800d80n_u02_ext0);
#if defined(CONFIG_AIC8800D80N_U02C)
    case FW_PATCH_8800D80N_U02C_EXT:
        return sizeof(fw_patch_8800d80n_u02c_ext0);
#endif /* CONFIG_AIC8800D80N_U02C */
#if defined(CONFIG_WIFI_MODE_RFTEST)
    /* D80N RFTEST BT 固件 (Linux CPMODE_TEST 专用 _rf_ 变体) */
    case FW_PATCH_8800D80N_RF_U02:
        return sizeof(fw_patch_8800d80n_rf_u02);
    case FW_PATCH_8800D80N_RF_U02_EXT:
        return sizeof(fw_patch_8800d80n_rf_u02_ext0);
    case FW_PATCH_TABLE_8800D80N_RF_U02:
        return sizeof(fw_patch_table_8800d80n_rf_u02);
#endif
#if defined(CONFIG_WIFI_MODE_RFTEST) && defined(CONFIG_AIC8800D80N_U02C)
    case FW_PATCH_8800D80N_RF_U02C:
        return sizeof(fw_patch_8800d80n_rf_u02c);
    case FW_PATCH_8800D80N_RF_U02C_EXT:
        return sizeof(fw_patch_8800d80n_rf_u02c_ext0);
    case FW_PATCH_TABLE_8800D80N_RF_U02C:
        return sizeof(fw_patch_table_8800d80n_rf_u02c);
#endif
#endif
#if defined(CONFIG_AIC8800D80X2)
    case FW_ADID_8800D80X2_U05:
        return sizeof(fw_adid_8800d80x2_u05);
    case FW_PATCH_8800D80X2_U05:
        return sizeof(fw_patch_8800d80x2_u05);
    case FW_PATCH_TABLE_8800D80X2_U05:
        return sizeof(fw_patch_table_8800d80x2_u05);
    case FW_PATCH_8800D80X2_U05_EXT0:
        return sizeof(fw_patch_8800d80x2_u05_ext0);
    case FW_PATCH_8800D80X2_U05_EXT1:
        return sizeof(fw_patch_8800d80x2_u05_ext1);
#endif
#if defined(CONFIG_AIC8800DC_H)
    case FW_ADID_8800DC_U02H:
        return sizeof(fw_adid_8800dc_u02h);
    case FW_PATCH_8800DC_U02H:
        return sizeof(fw_patch_8800dc_u02h);
    case FW_PATCH_TABLE_8800DC_U02H:
        return sizeof(fw_patch_table_8800dc_u02h);
#endif
#endif /* CONFIG_BT_SUPPORT */

    default:
        return 0;
    }
}
#else /* CONFIG_LOAD_FW != 2: 固件数组未内嵌, 文件/Flash 模式不走此接口 */
void *aic_fw_ptr_get(enum aic_fw id)
{
    (void)id;
    return NULL;
}

uint32_t aic_fw_size_get(enum aic_fw id)
{
    (void)id;
    return 0;
}
#endif /* CONFIG_LOAD_FW == 2 */

/* ==========================================================================
 * Firmware loader (from former fmac_port.c)
 * ========================================================================== */

#if CONFIG_LOAD_FW == 0   /* filemode */
#include <unistd.h>
#include <fcntl.h>

/**
 * @brief 从文件系统读取固件二进制文件
 *
 * 在 AICBSP_FW_PATH 下查找 fw_name 指定的文件，
 * 读取全部内容到动态分配的缓冲区中。
 *
 * @param fw_name 固件文件名
 * @param fw_buf  输出的固件缓冲区指针
 * @return 固件大小（字节）, 0=失败
 */
int aicwf_rwnx_read_fw_bin(char *fw_name, uint32_t **fw_buf)
{
    char path[128];
    void *buffer;
    int fd, n;
    off_t length;

    *fw_buf = NULL;
    if (!fw_name)
        return 0;

    snprintf(path, 128, "%s/%s", AICBSP_FW_PATH, fw_name);

    fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        DBG_FMAC_INF("Can not open the file %s\n", path);
    }
    else
    {
        length = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        buffer = aicwf_rtos_malloc(length);
        if (buffer)
        {
            n = read(fd, buffer, length);
            if (n != length)
            {
                DBG_FMAC_INF("Can not read fw %s\n", path);
                aicwf_rtos_free(buffer);
                close(fd);
                return 0;
            }
            *fw_buf = buffer;
            return length;
        }
        close(fd);
    }

    return 0;
}

/**
 * @brief 加载WiFi固件
 *
 * 根据芯片ID和模式选择对应的固件文件名，然后从文件系统加载。
 *
 * @param chipid 芯片产品ID
 * @param mode   WiFi模式
 * @param fw_buf 输出的固件缓冲区指针
 * @return 固件大小（字节）, 0=失败
 */
int aicwf_rwnx_load_firmware(uint16_t chipid, int mode, uint32_t **fw_buf)
{
    int size = 0;
    char *fw_name = NULL;
    *fw_buf = NULL;

    if (chipid == PRODUCT_ID_AIC8801)
    {
        if (mode == WIFI_MODE_RFTEST)
        {
            fw_name = "fmacfw_rf.bin";
        }
        else
        {
            fw_name = "fmacfw.bin";
        }
    }
    else if (chipid == PRODUCT_ID_AIC8800DC || chipid == PRODUCT_ID_AIC8800DW)
    {
        if (aic_get_chip_sub_id() == 0)
        {
            if (mode == WIFI_MODE_RFTEST)
            {
                if (aic_get_aic8800dc_rf_flag() == 0)
                {
                    fw_name = "lmacfw_rf_8800dc.bin";
                }
                else if (aic_get_aic8800dc_rf_flag() == 1)
                {
                    fw_name = "fmacfw_rf_patch_8800dc.bin";
                }
            }
            else
            {
                fw_name = "fmacfw_patch_8800dc.bin";
            }
        }
        else if (aic_get_chip_sub_id() == 1)
        {
            if (mode == WIFI_MODE_RFTEST)
            {
                fw_name = "lmacfw_rf_8800dc.bin";
            }
            else
            {
                fw_name = "fmacfw_patch_8800dc_u02.bin";
            }
        }
    }
    else if (chipid == PRODUCT_ID_AIC8800D80)
    {
        if (aic_get_chip_id_h())
        {
            /* H 版本: 按总线 + IPC 区分固件 (参考 Linux aic_bsp_main.c) */
            if (mode == WIFI_MODE_RFTEST)
            {
                fw_name = "lmacfw_rf_8800d80_u02.bin";
            }
            else
            {
#if defined(CONFIG_USB_SUPPORT)
#if defined(CONFIG_FOR_IPCAM)
                fw_name = "fmacfw_8800d80_h_u02_usb_ipc.bin";
#else
                fw_name = "fmacfw_8800d80_h_u02_usb.bin";
#endif
#else
#if defined(CONFIG_FOR_IPCAM)
                fw_name = "fmacfw_8800d80_h_u02_ipc.bin";
#else
                fw_name = "fmacfw_8800d80_h_u02.bin";
#endif
#endif
            }
        }
        else if (aic_get_chip_id() == CHIP_REV_U01)
        {
            if (mode == WIFI_MODE_RFTEST)
            {
                fw_name = "lmacfw_rf_8800d80.bin";
            }
            else
            {
                fw_name = "fmacfw_8800d80.bin";
            }
        }
        else
        {
            /* U02/U03 及后续版本 */
            if (mode == WIFI_MODE_RFTEST)
            {
                fw_name = "lmacfw_rf_8800d80_u02.bin";
            }
            else
            {
                fw_name = "fmacfw_8800d80_u02.bin";
            }
        }
    }
    else if (chipid == PRODUCT_ID_AIC8800D80N)
    {
        /* D80N 主 FMAC 复用 D80 (Linux: D80N wl_fw = fmacfw_8800d80.bin) */
        if (mode == WIFI_MODE_RFTEST)
        {
            fw_name = "lmacfw_rf_8800d80n.bin";
        }
        else
        {
            fw_name = "fmacfw_8800d80_u02.bin";
        }
    }

    return aicwf_rwnx_read_fw_bin(fw_name, fw_buf);
}

/**
 * @brief 加载补丁表固件
 *
 * 仅DC/DW系列芯片需要加载补丁表。
 *
 * @param chipid 芯片产品ID
 * @param fw_buf 输出的固件缓冲区指针
 * @return 固件大小（字节）, 0=无需加载或失败
 */
int aicwf_rwnx_load_patch_tbl(uint16_t chipid, uint32_t **fw_buf)
{
    int size = 0;
    char *fw_name = NULL;

    if (chipid == PRODUCT_ID_AIC8800DC || chipid == PRODUCT_ID_AIC8800DW)
    {
        if (aic_get_chip_sub_id() == 1)
        {
            fw_name = "fmacfw_patch_tbl_8800dc_u02.bin";
        }
    }

    return aicwf_rwnx_read_fw_bin(fw_name, fw_buf);
}

/**
 * @brief 释放固件缓冲区
 * @param fw_buf 固件缓冲区指针的指针
 */
void aicwf_rwnx_restore_firmware(uint32_t **fw_buf)
{
    aicwf_rtos_free(*fw_buf);
    *fw_buf = NULL;
}

/* filemode: 无 flash 固件类型探测, 兜底返回 0 */
uint32_t aicwf_get_wifi_fw_type(void)
{
    return 0;
}

#elif CONFIG_LOAD_FW == 1  /* flashmode */
#include "ReliableData.h"
#include "MRD.h"
#include "FlashPartition.h"

extern int RDWIFI_ReadFmSize(int type, int *length, unsigned int *fmaddr, int *headerlen);
extern void Rdisk_FlashRead(unsigned int flashaddress, unsigned char *buffer, unsigned int size);

extern unsigned int LzmaUncompressLen(const unsigned char *src);
int LzmaUncompress(unsigned char *dest, size_t  *destLen, const unsigned char *src, size_t  *srcLen);

#if (defined(CONFIG_AIC8800DC) || defined(CONFIG_AIC8800DW))
    #define WIFI_LZMA_LEN               (128*1024)
#elif defined(CONFIG_AIC8800D80)
    #define WIFI_LZMA_LEN               (256*1024)
#endif
static void *wifibin = NULL;
static uint8_t rdwifi_fw_type = 0;

/**
 * @brief 检查固件数据是否为LZMA压缩格式
 * @param src 固件数据指针
 * @return 1=压缩, 0=未压缩
 */
static unsigned char aicwf_isLzmaCompressed(const unsigned char *src)
{
    unsigned int len = 0;
    int ii = 0;
    unsigned char *temp = NULL;
    unsigned char header_fixed[5] = {0x5d, 0x00, 0x00, 0x10, 0x00};

    temp = (unsigned char *)src;
    for (ii = 0; ii < sizeof(header_fixed); ii++)
    {
        if (*(temp + ii) != header_fixed[ii])
            break;
    }
    if (ii == sizeof(header_fixed))
        return 1;
    else
        return 0;
}

/**
 * @brief 获取WiFi固件类型（flash模式）
 * @return 固件类型标志位
 */
uint32_t aicwf_get_wifi_fw_type(void)
{
    return rdwifi_fw_type;
}

/**
 * @brief 从Flash分区探测WiFi固件类型
 *
 * 读取Flash中WiFi区域和MRD区域的固件类型标志，
 * 确定当前存储的是普通固件还是RF测试固件。
 *
 * @return 固件类型标志位（bit0=普通固件, bit1=RF测试固件, bit2=校准区固件）
 */
unsigned char RDWIFI_Fw_type(void)
{
    FlashLayoutConfInfo *pFlashLayout = GetFlashLayoutConfig();
    int   headersize = (sizeof(RD_BUFFER_HEADER) + sizeof(RD_ENTRY));
    UINT8  *WiFiArea = (UINT8 *)aicwf_rtos_malloc(headersize);
    UINT8 *pRAMbuf = NULL;
    RD_ENTRY entry = {0};
    RD_BUFFER_HEADER header = {0};

    Rdisk_FlashRead(pFlashLayout->WIFIFirmAddress, WiFiArea, headersize);

    pRAMbuf = WiFiArea + sizeof(RD_BUFFER_HEADER);

    memcpy(&entry, (RD_ENTRY *)pRAMbuf, sizeof(RD_ENTRY));

    if (entry.entryType == MRD_WIFI_NORMAL_FM)
    {
        rdwifi_fw_type |= 0x01;         //nomal fw in wifi area
    }
    else if (entry.entryType == MRD_WIFI_CALI_FM)
    {
        rdwifi_fw_type |= 0x02;         //rf test fw in wifi area
    }

    if (pFlashLayout->WIFICalFirmAddress != 0xFFFFFFFF)
    {
        rdwifi_fw_type |= 0x04;         //rf rest fw in cal area
    }

    Rdisk_FlashRead(pFlashLayout->MRDStartAddress, WiFiArea, headersize);

    pRAMbuf = WiFiArea + sizeof(RD_BUFFER_HEADER);

    memcpy(&entry, (RD_ENTRY *)pRAMbuf, sizeof(RD_ENTRY));

    if (entry.entryType == MRD_WIFI_NORMAL_FM)
    {
        rdwifi_fw_type |= 0x08;       //patch and cali bin for rf rest
    }


    aicwf_rtos_free(WiFiArea);

    DBG_FMAC_INF("%s: WIFIFirm=%x,MRDStart=%x,type=%x", __func__,
                 pFlashLayout->WIFIFirmAddress, pFlashLayout->MRDStartAddress, rdwifi_fw_type);

    return rdwifi_fw_type;
}


/**
 * @brief 从Flash分区读取WiFi固件（内部实现）
 *
 * 遍历Flash MRD分区查找指定文件名和类型的固件，
 * 如果固件是LZMA压缩的则自动解压。
 *
 * @param fw_name 固件文件名
 * @param fw_buf  输出的固件缓冲区指针
 * @param type    固件类型（1=普通固件, 2=RF校准固件）
 * @return 固件大小, 负值=失败
 */
static int __aicwf_RDWIFI_ReadFm(char *fw_name, uint32_t **fw_buf, unsigned char type)
{
    RD_ENTRY entry = {0};
    RD_BUFFER_HEADER header = {0};
    UINT8 *pRAMbuf = NULL;
    int  ret = -1;
    int len, hlen;
    unsigned int fwaddr = 0, offset = 0;
    void *buffer;

    int   headersize = (sizeof(RD_BUFFER_HEADER) + sizeof(RD_ENTRY));
    UINT8  *WiFiArea = (UINT8 *)aicwf_rtos_malloc(headersize);

    FlashLayoutConfInfo *pFlashLayout = GetFlashLayoutConfig();

    if (NULL == WiFiArea)
    {
        DBG_FMAC_ERR("malloc wifiarea error");
        return -1;
    }

    fwaddr = pFlashLayout->WIFIFirmAddress;

    if (pFlashLayout->WIFICalFirmAddress != 0xFFFFFFFF)
    {
        if ((type == 2) && (!(rdwifi_fw_type & 0x02)))
            fwaddr = pFlashLayout->WIFICalFirmAddress;
    }

    if ((rdwifi_fw_type & 0x02) && (rdwifi_fw_type & 0x08) && (type == 1))
    {
        fwaddr = pFlashLayout->MRDStartAddress;
    }

    DBG_FMAC_INF("Read WiFi Firmware--->:%s, fwaddr=%x, type:%d", fw_name, fwaddr, type);

    hlen = sizeof(RD_BUFFER_HEADER);
    do
    {

        Rdisk_FlashRead(fwaddr, WiFiArea, headersize);
        pRAMbuf = WiFiArea + hlen;

        {
            UINT32 entry_size = 0;
            UINT8  pad_size = 0;
            UINT32 file_size = 0;

            memcpy(&entry, (RD_ENTRY *)pRAMbuf, sizeof(RD_ENTRY));
            entry_size = (entry.fileAndPadSize & 0x00FFFFFF);
            pad_size = (UINT8)(((entry.fileAndPadSize & 0xFF000000) >> 24));
            DBG_FMAC_INF("name:%s:%d,%d", entry.file_name, entry_size, pad_size);

            //check type
            if (type == 1 && entry.entryType == MRD_WIFI_NORMAL_FM ||
                    type == 2 && entry.entryType == MRD_WIFI_CALI_FM)
            {
                file_size = entry_size - pad_size;
                if (file_size == 0)
                {
                    ret = -2;
                    goto end;
                }
            }
            else
            {
                ret = -2;
                goto end;
            }

            //find file name
            if (!strcmp(fw_name, entry.file_name))
            {

                buffer = aicwf_rtos_malloc(file_size);
                if (buffer != NULL)
                {
                    Rdisk_FlashRead(fwaddr + headersize - 4, buffer, file_size);
                    if (aicwf_isLzmaCompressed(buffer))
                    {
                        unsigned int wificombosize = LzmaUncompressLen(buffer);
                        void *wifibin = aicwf_rtos_malloc(wificombosize);
                        unsigned int combolzmalen = WIFI_LZMA_LEN;
                        ret = LzmaUncompress(wifibin, &wificombosize, buffer, &combolzmalen);
                        if (!ret)
                        {
                            *fw_buf = wifibin;
                            ret = wificombosize;
                            aicwf_rtos_free(buffer);

                            DBG_FMAC_INF("decomp WiFi Firmware--->:%d %d\n", wificombosize, **fw_buf);
                        }
                        else
                        {
                            ret = -3;
                            aicwf_rtos_free(buffer);
                            aicwf_rtos_free(wifibin);
                        }
                    }
                    else
                    {
                        *fw_buf = buffer;
                        ret = file_size;
                    }

                }
                else
                {
                    ret = -4;
                }
                break;
            }
            else
            {
                fwaddr += (entry_size + headersize - 4);
                hlen = 0;
                headersize = sizeof(RD_ENTRY);
            }

        }
    }
    while (1);
end:
    aicwf_rtos_free(WiFiArea);

    return ret;
}


static int aicwf_rwnx_read_fw_bin(char *fw_name, uint32_t **fw_buf, unsigned char mode)
{
    int ret;

    ret = __aicwf_RDWIFI_ReadFm(fw_name, fw_buf, mode);

    if (ret <= 0)
    {
        DBG_FMAC_INF("aicwf_rwnx_read_fw_bin: %d\n", ret);
        aic_set_chip_fw_match(ret);
        return 0;
    }
    else
    {
        aic_set_chip_fw_match(1);
        return ret;
    }
}

/**
 * @brief 加载WiFi固件
 *
 * 根据芯片ID和模式选择对应的固件文件名，然后从文件系统加载。
 *
 * @param chipid 芯片产品ID
 * @param mode   WiFi模式
 * @param fw_buf 输出的固件缓冲区指针
 * @return 固件大小（字节）, 0=失败
 */
int aicwf_rwnx_load_firmware(uint16_t chipid, int mode, uint32_t **fw_buf)
{
    unsigned char local_mode;
    char *fw_name = NULL;
    *fw_buf = NULL;

    if (chipid == PRODUCT_ID_AIC8801)
    {
        if (mode == WIFI_MODE_RFTEST)
        {
            fw_name = "fmacfw_rf.bin";
        }
        else
        {
            fw_name = "fmacfw.bin";
        }
    }
    else if (chipid == PRODUCT_ID_AIC8800DC || chipid == PRODUCT_ID_AIC8800DW)
    {
        if (aic_get_chip_sub_id() == 0)
        {
            if (mode == WIFI_MODE_RFTEST)
            {
                if (aic_get_aic8800dc_rf_flag() == 0)
                {
                    fw_name = "lmacfw_rf_8800dc.bin";
                }
                else if (aic_get_aic8800dc_rf_flag() == 1)
                {
                    fw_name = "fmacfw_rf_patch_8800dc.bin";
                }
            }
            else
            {
                fw_name = "fmacfw_patch_8800dc.bin";
            }
        }
        else if (aic_get_chip_sub_id() == 1)
        {
            if (mode == WIFI_MODE_RFTEST)
            {
                fw_name = "lmacfw_rf_8800dc.bin";
            }
            else
            {
                fw_name = "fmacfw_patch_8800dc_u02.bin";
            }
        }
        else if (aic_get_chip_sub_id() == 2)
        {
            if (mode == WIFI_MODE_RFTEST)
            {
                fw_name = "lmacfw_rf_8800dc.bin";
            }
            else
            {
#if defined(AIC_HCI_OVER_SDIO)
                fw_name = "fmacfw_patch_8800dc_hbt_u02.bin";
#else
                fw_name = "fmacfw_patch_8800dc_h_u02.bin";
#endif
            }
        }
    }
    else if (chipid == PRODUCT_ID_AIC8800D80)
    {
        if (aic_get_chip_id_h())
        {
            /* H 版本: 按总线 + IPC 区分固件 (参考 Linux aic_bsp_main.c) */
            if (mode == WIFI_MODE_RFTEST)
            {
                fw_name = "lmacfw_rf_8800d80_u02.bin";
            }
            else
            {
#if defined(CONFIG_USB_SUPPORT)
#if defined(CONFIG_FOR_IPCAM)
                fw_name = "fmacfw_8800d80_h_u02_usb_ipc.bin";
#else
                fw_name = "fmacfw_8800d80_h_u02_usb.bin";
#endif
#else
#if defined(CONFIG_FOR_IPCAM)
                fw_name = "fmacfw_8800d80_h_u02_ipc.bin";
#else
                fw_name = "fmacfw_8800d80_h_u02.bin";
#endif
#endif
            }
        }
        else
        {
            if (aic_get_chip_id() == CHIP_REV_U01)
            {
                if (mode == WIFI_MODE_RFTEST)
                {
                    fw_name = "lmacfw_rf_8800d80.bin";
                }
                else
                {
                    fw_name = "fmacfw_8800d80.bin";
                }
            }
            else
            {
                if (mode == WIFI_MODE_RFTEST)
                {
                    fw_name = "lmacfw_rf_8800d80_u02.bin";
                }
                else
                {
                    fw_name = "fmacfw_8800d80_u02.bin";
                }
            }
        }
    }
    else if (chipid == PRODUCT_ID_AIC8800D80N)
    {
        /* D80N 主 FMAC 复用 D80 (Linux: D80N wl_fw = fmacfw_8800d80.bin) */
        if (mode == WIFI_MODE_RFTEST)
        {
            fw_name = "lmacfw_rf_8800d80n.bin";
        }
        else
        {
            fw_name = "fmacfw_8800d80_u02.bin";
        }
    }

    if (mode == WIFI_MODE_RFTEST)
        local_mode = 2;
    else
        local_mode = 1;

    return aicwf_rwnx_read_fw_bin(fw_name, fw_buf, local_mode);
}

/**
 * @brief 加载补丁表固件
 *
 * 仅DC/DW系列芯片需要加载补丁表。
 *
 * @param chipid 芯片产品ID
 * @param fw_buf 输出的固件缓冲区指针
 * @return 固件大小（字节）, 0=无需加载或失败
 */
int aicwf_rwnx_load_patch_tbl(uint16_t chipid, uint32_t **fw_buf)
{
    if (chipid == PRODUCT_ID_AIC8800DC || chipid == PRODUCT_ID_AIC8800DW)
    {
        if (aic_get_chip_sub_id() == 1)
        {
            return aicwf_rwnx_read_fw_bin("fmacfw_patch_tbl_8800dc_u02.bin", fw_buf, 1);
        }
        else if (aic_get_chip_sub_id() == 2)
        {
#if defined(AIC_HCI_OVER_SDIO)
            return aicwf_rwnx_read_fw_bin("fmacfw_patch_tbl_8800dc_hbt_u02.bin", fw_buf, 1);
#else
            return aicwf_rwnx_read_fw_bin("fmacfw_patch_tbl_8800dc_h_u02.bin", fw_buf, 1);
#endif
        }
    }

    return 0;
}

/**
 * @brief 加载校准固件（flash模式）
 *
 * 仅DC/DW系列芯片需要加载校准数据。
 *
 * @param chipid 芯片产品ID
 * @param fw_buf 输出的固件缓冲区指针
 * @return 固件大小, 0=无需加载或失败
 */
int aicwf_rwnx_load_calib(uint16_t chipid, uint32_t **fw_buf)
{
    if (chipid == PRODUCT_ID_AIC8800DC || chipid == PRODUCT_ID_AIC8800DW)
    {
        if (aic_get_chip_sub_id() == 1)
        {
            return aicwf_rwnx_read_fw_bin("fmacfw_calib_8800dc_u02.bin", fw_buf, 1);
        }
        else if (aic_get_chip_sub_id() == 2)
        {
#if defined(AIC_HCI_OVER_SDIO)
            return aicwf_rwnx_read_fw_bin("fmacfw_calib_8800dc_hbt_u02.bin", fw_buf, 1);
#else
            return aicwf_rwnx_read_fw_bin("fmacfw_calib_8800dc_h_u02.bin", fw_buf, 1);
#endif
        }
    }

    return 0;
}

/**
 * @brief 释放固件缓冲区
 * @param fw_buf 固件缓冲区指针的指针
 */
void aicwf_rwnx_restore_firmware(uint32_t **fw_buf)
{
    aicwf_rtos_free(*fw_buf);
    *fw_buf = NULL;
}
#elif CONFIG_LOAD_FW == 2
/* innermode: read from array */

/**
 * @brief 加载WiFi固件
 *
 * 根据芯片ID和模式选择对应的固件文件名，然后从文件系统加载。
 *
 * @param chipid 芯片产品ID
 * @param mode   WiFi模式
 * @param fw_buf 输出的固件缓冲区指针
 * @return 固件大小（字节）, 0=失败
 */
int aicwf_rwnx_load_firmware(uint16_t chipid, int mode, uint32_t **fw_buf)
{
    int size = 0;
    *fw_buf = NULL;

    if (chipid == PRODUCT_ID_AIC8801)
    {
#if defined(CONFIG_AIC8801)
        if (mode == WIFI_MODE_RFTEST)
        {
            *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW_RF);
            size = aic_fw_size_get(FMACFW_RF);
        }
        else
        {
            *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW);
            size = aic_fw_size_get(FMACFW);
        }
#endif /* CONFIG_AIC8801 */
    }
    else if (chipid == PRODUCT_ID_AIC8800DC || chipid == PRODUCT_ID_AIC8800DW)
    {
#if defined(CONFIG_AIC8800DC) || defined(CONFIG_AIC8800DW)
        if (aic_get_chip_sub_id() == 0)
        {
            if (mode == WIFI_MODE_RFTEST)
            {
                if (aic_get_aic8800dc_rf_flag() == 0)
                {
                    *fw_buf = (uint32_t *)aic_fw_ptr_get(LMACFW_RF_8800DC);
                    size = aic_fw_size_get(LMACFW_RF_8800DC);
                }
                else if (aic_get_aic8800dc_rf_flag() == 1)
                {
                    *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW_RF_PATCH_8800DC);
                    size = aic_fw_size_get(FMACFW_RF_PATCH_8800DC);
                }
            }
            else
            {
                *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW_PATCH_8800DC);
                size = aic_fw_size_get(FMACFW_PATCH_8800DC);
            }
        }
        else if (aic_get_chip_sub_id() == 1)
        {
            if (aic_get_aic8800dc_calib_flag() == 0)
            {
                if (mode == WIFI_MODE_RFTEST)
                {
                    DBG_FMAC_INF("aic load lmacfw_rf_8800dc.bin\n\r");
                    *fw_buf = (uint32_t *)aic_fw_ptr_get(LMACFW_RF_8800DC);
                    size = aic_fw_size_get(LMACFW_RF_8800DC);
                }
                else
                {
                    DBG_FMAC_INF("aic load fmacfw_patch_8800dc_u02.bin\n\r");
                    *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW_PATCH_8800DC_U02);
                    size = aic_fw_size_get(FMACFW_PATCH_8800DC_U02);
                }
            }
            else if (aic_get_aic8800dc_calib_flag() == 1)
            {
                DBG_FMAC_INF("aic load fmacfw_calib_8800dc_u02.bin\n\r");
                *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW_CALIB_8800DC_U02);
                size = aic_fw_size_get(FMACFW_CALIB_8800DC_U02);
            }
        }
        else if (aic_get_chip_sub_id() == 2)
        {
            if (aic_get_aic8800dc_calib_flag() == 0)
            {
                if (mode == WIFI_MODE_RFTEST)
                {
                    DBG_FMAC_INF("aic load lmacfw_rf_8800dc.bin\n\r");
                    *fw_buf = (uint32_t *)aic_fw_ptr_get(LMACFW_RF_8800DC);
                    size = aic_fw_size_get(LMACFW_RF_8800DC);
                }
                else
                {
                    DBG_FMAC_INF("aic load fmacfw_patch_8800dc_h_u02.bin\n\r");
                    *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW_PATCH_8800DC_H_U02);
                    size = aic_fw_size_get(FMACFW_PATCH_8800DC_H_U02);
                }
            }
            else if (aic_get_aic8800dc_calib_flag() == 1)
            {
                DBG_FMAC_INF("aic load fmacfw_calib_8800dc_h_u02.bin\n\r");
                *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW_CALIB_8800DC_H_U02);
                size = aic_fw_size_get(FMACFW_CALIB_8800DC_H_U02);
            }
        }
#endif /* CONFIG_AIC8800DC or CONFIG_AIC8800DW */
    }
    else if (chipid == PRODUCT_ID_AIC8800D80)
    {
#if defined(CONFIG_AIC8800D80)
        if (aic_get_chip_id_h())
        {
            /* H 版本: 按总线 + IPC 区分固件 (参考 Linux aic_bsp_main.c) */
            if (mode == WIFI_MODE_RFTEST)
            {
#if defined(CONFIG_WIFI_MODE_RFTEST)
                *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW_RF_8800D80_U02);
                size = aic_fw_size_get(FMACFW_RF_8800D80_U02);
#endif
            }
            else
            {
#if defined(CONFIG_USB_SUPPORT)
#if defined(CONFIG_FOR_IPCAM)
                *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW_8800D80_H_U02_USB_IPC);
                size = aic_fw_size_get(FMACFW_8800D80_H_U02_USB_IPC);
#else
                *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW_8800D80_H_U02_USB);
                size = aic_fw_size_get(FMACFW_8800D80_H_U02_USB);
#endif
#else
#if defined(CONFIG_FOR_IPCAM)
                *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW_8800D80_H_U02_IPC);
                size = aic_fw_size_get(FMACFW_8800D80_H_U02_IPC);
#else
                *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW_8800D80_H_U02);
                size = aic_fw_size_get(FMACFW_8800D80_H_U02);
#endif
#endif
            }
        }
        else if (aic_get_chip_id() == CHIP_REV_U01)
        {
            if (mode == WIFI_MODE_RFTEST)
            {
#if defined(CONFIG_WIFI_MODE_RFTEST)
                *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW_RF_8800D80);
                size = aic_fw_size_get(FMACFW_RF_8800D80);
#endif
            }
            else
            {
                *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW_8800D80);
                size = aic_fw_size_get(FMACFW_8800D80);
            }
        }
        else
        {
            /* U02/U03 及后续版本 */
            if (mode == WIFI_MODE_RFTEST)
            {
#if defined(CONFIG_WIFI_MODE_RFTEST)
                *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW_RF_8800D80_U02);
                size = aic_fw_size_get(FMACFW_RF_8800D80_U02);
#endif
            }
            else
            {
                *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW_8800D80_U02);
                size = aic_fw_size_get(FMACFW_8800D80_U02);
            }
        }
#endif /* CONFIG_AIC8800D80 */
    }
    else if (chipid == PRODUCT_ID_AIC8800D80N)
    {
#if defined(CONFIG_AIC8800D80N)
        if (mode == WIFI_MODE_RFTEST)
        {
#if defined(CONFIG_WIFI_MODE_RFTEST)
            *fw_buf = (uint32_t *)aic_fw_ptr_get(LMACFW_RF_8800D80N);
            size = aic_fw_size_get(LMACFW_RF_8800D80N);
#endif
        }
        else
        {
            /* D80N 主 FMAC 复用 D80 (Linux: D80N wl_fw = fmacfw_8800d80.bin) */
            *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW_8800D80_U02);
            size = aic_fw_size_get(FMACFW_8800D80_U02);
        }
#endif /* CONFIG_AIC8800D80N */
    }
    else if (chipid == PRODUCT_ID_AIC8800D80X2)
    {
#if defined(CONFIG_AIC8800D80X2)
        if (mode == WIFI_MODE_RFTEST)
        {
#if defined(CONFIG_WIFI_MODE_RFTEST)
            *fw_buf = (uint32_t *)aic_fw_ptr_get(LMACFW_RF_8800D80X2);
            size = aic_fw_size_get(LMACFW_RF_8800D80X2);
#endif
        }
        else
        {
            /* D80X2 U05 基线主 FMAC: fmacfw_8800d80x2.bin */
            *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW_8800D80X2);
            size = aic_fw_size_get(FMACFW_8800D80X2);
        }
#endif /* CONFIG_AIC8800D80X2 */
    }
    return size;
}

/**
 * @brief 加载补丁表固件
 *
 * 仅DC/DW系列芯片需要加载补丁表。
 *
 * @param chipid 芯片产品ID
 * @param fw_buf 输出的固件缓冲区指针
 * @return 固件大小（字节）, 0=无需加载或失败
 */
int aicwf_rwnx_load_patch_tbl(uint16_t chipid, uint32_t **fw_buf)
{
    int size = 0;

    if (chipid == PRODUCT_ID_AIC8800DC || chipid == PRODUCT_ID_AIC8800DW)
    {
#if defined(CONFIG_AIC8800DC) || defined(CONFIG_AIC8800DW)
        if (aic_get_chip_sub_id() == 1)
        {
            DBG_FMAC_INF("aic load fmacfw_patch_tbl_8800dc_u02.bin\n\r");
            *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW_PATCH_TBL_8800DC_U02);
            size = aic_fw_size_get(FMACFW_PATCH_TBL_8800DC_U02);
        }
        else if (aic_get_chip_sub_id() == 2)
        {
            DBG_FMAC_INF("aic load fmacfw_patch_tbl_8800dc_h_u02.bin\n\r");
            *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW_PATCH_TBL_8800DC_H_U02);
            size = aic_fw_size_get(FMACFW_PATCH_TBL_8800DC_H_U02);
        }
#endif /* CONFIG_AIC8800DC or CONFIG_AIC8800DW */
    }
    return size;
}

/**
 * @brief 加载校准固件（flash模式）
 *
 * 仅DC/DW系列芯片需要加载校准数据。
 *
 * @param chipid 芯片产品ID
 * @param fw_buf 输出的固件缓冲区指针
 * @return 固件大小, 0=无需加载或失败
 */
int aicwf_rwnx_load_calib(uint16_t chipid, uint32_t **fw_buf)
{
    int size = 0;
#if defined(CONFIG_AIC8800DC) || defined(CONFIG_AIC8800DW)
    if (chipid == PRODUCT_ID_AIC8800DC || chipid == PRODUCT_ID_AIC8800DW)
    {
        if (aic_get_chip_sub_id() == 1)
        {
            DBG_FMAC_INF("aic load fmacfw_calib_8800dc_u02.bin\n\r");
            *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW_CALIB_8800DC_U02);
            size = aic_fw_size_get(FMACFW_CALIB_8800DC_U02);
        }
        else if (aic_get_chip_sub_id() == 2)
        {
            DBG_FMAC_INF("aic load fmacfw_calib_8800dc_h_u02.bin\n\r");
            *fw_buf = (uint32_t *)aic_fw_ptr_get(FMACFW_CALIB_8800DC_H_U02);
            size = aic_fw_size_get(FMACFW_CALIB_8800DC_H_U02);
        }
    }
#endif /* CONFIG_AIC8800DC or CONFIG_AIC8800DW */
    return size;
}

/**
 * @brief 释放固件缓冲区
 * @param fw_buf 固件缓冲区指针的指针
 */
void aicwf_rwnx_restore_firmware(uint32_t **fw_buf)
{
    *fw_buf = NULL;
}

/* innermode: 无 flash 固件类型探测, 兜底返回 0 */
uint32_t aicwf_get_wifi_fw_type(void)
{
    return 0;
}

#endif

/**
 * @brief 判断 RF 测试固件是否可用（platform 层是否已通过 RFTEST 开关加入固件）
 *
 * innermode: 按编译期芯片型号检查对应 RF 固件数组是否已编译进包。
 * filemode/flashmode: 复用 fw_type 位探测（flash 有效，filemode 无探测机制返回 0）。
 *
 * @return 1=可用, 0=不可用
 */
int aicwf_rftest_fw_available(void)
{
#if CONFIG_LOAD_FW == 2
#if defined(CONFIG_AIC8801)
    return (aic_fw_ptr_get(FMACFW_RF) != NULL && aic_fw_size_get(FMACFW_RF) > 0) ? 1 : 0;
#elif defined(CONFIG_AIC8800DC) || defined(CONFIG_AIC8800DW)
    return (aic_fw_ptr_get(LMACFW_RF_8800DC) != NULL && aic_fw_size_get(LMACFW_RF_8800DC) > 0) ? 1 : 0;
#elif defined(CONFIG_AIC8800D80)
    return (aic_fw_ptr_get(FMACFW_RF_8800D80_U02) != NULL && aic_fw_size_get(FMACFW_RF_8800D80_U02) > 0) ? 1 : 0;
#elif defined(CONFIG_AIC8800D80N)
    return (aic_fw_ptr_get(LMACFW_RF_8800D80N) != NULL && aic_fw_size_get(LMACFW_RF_8800D80N) > 0) ? 1 : 0;
#elif defined(CONFIG_AIC8800D80X2)
    return (aic_fw_ptr_get(LMACFW_RF_8800D80X2) != NULL && aic_fw_size_get(LMACFW_RF_8800D80X2) > 0) ? 1 : 0;
#else
    return 0;
#endif
#else
    return (aicwf_get_wifi_fw_type() & 0x02) ? 1 : 0;
#endif
}
