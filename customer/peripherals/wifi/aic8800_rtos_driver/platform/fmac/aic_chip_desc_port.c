/**
 * Copyright (C) 2018-2026 AICSemi Ltd.
 *
 * All Rights Reserved
 *
 * aic_chip_desc platform 数据实现。
 *
 * 单库统一架构下本文件是原 host 侧 CONFIG_AIC88xx 型号宏的唯一数据归宿:
 * 各型号的总线协议行为/电源管理/协议差异全部以静态数据表表达。
 * 裁剪: 产品只用某型号时, 删除表中其余条目即可 (host 查不到会打印提示)。
 *
 * 数据来源 (原宏位置, 迁移时逐一核对):
 * - tx_hdr_crc8:       sdio_co.c tx header[3] / hal_co.c aicwf_hal_msg_push
 * - sdio_fc_reg_mask:  sdio_co.c flowctrl 读取后 & SDIOWIFI_FLOWCTRL_MASK_REG
 * - func_flag_tx_init: sdio_co.c / usb_co.c func_flag_tx 静态初值
 * - sdio_wakeup_val:   sdio_co.c aicwf_sdio_bus_wakeup wakeup_reg_val
 * - sdio_sleep_ack_poll: sdio_co.c aicwf_sdio_bus_sleep_allow
 * - sdio_misc_int_path / sdio_intmaskf2_decode: sdio_co.c rx intstatus 读取
 * - msgcfm_poll_path:  sdio_co.c send_pkt 8801 cfm 轮询分支
 * - msg_fc_bypass:     sdio_co.c aicwf_sdio_tx_msg 免流控 (仅 D80N=1,
 *                      对齐 Linux aicsdio.c tx_msg 型号特判; 缺省 0=流控)
 * - sta_info_more_info: lmac_msg.h mm_get_sta_info_req.more_info (变长打包)
 * - supports_5g:      USE_5G 宏 (fhost_config.c 信道表/rwnx_main.c stack_start)
 */

#include "aic_chip_desc.h"

static const struct aic_chip_desc aic_chip_desc_table[] =
{
    {
        .chipid             = AIC_CHIP_ID_8801,
        .tx_hdr_crc8        = 0,
        .sdio_fc_reg_mask   = 1,
        .func_flag_tx_init  = 1,
        .sdio_wakeup_val    = 0x01,
        .sdio_sleep_ack_poll = 1,
        .sdio_misc_int_path = 0,
        .sdio_intmaskf2_decode = 0,
        .msgcfm_poll_path   = 1,
        .msg_fc_bypass      = 0,
        .sta_info_more_info = 0,
        .supports_5g        = 0,
    },
    {
        .chipid             = AIC_CHIP_ID_8800DC,
        .tx_hdr_crc8        = 0,
        .sdio_fc_reg_mask   = 1,
        .func_flag_tx_init  = 1,
        .sdio_wakeup_val    = 0x01,
        .sdio_sleep_ack_poll = 1,
        .sdio_misc_int_path = 0,
        .sdio_intmaskf2_decode = 0,
        .msgcfm_poll_path   = 0,
        .msg_fc_bypass      = 0,
        .sta_info_more_info = 0,
        .supports_5g        = 0,
    },
    {
        .chipid             = AIC_CHIP_ID_8800DW,
        .tx_hdr_crc8        = 0,
        .sdio_fc_reg_mask   = 1,
        .func_flag_tx_init  = 1,
        .sdio_wakeup_val    = 0x01,
        .sdio_sleep_ack_poll = 1,
        .sdio_misc_int_path = 0,
        .sdio_intmaskf2_decode = 0,
        .msgcfm_poll_path   = 0,
        .msg_fc_bypass      = 0,
        .sta_info_more_info = 0,
        .supports_5g        = 0,
    },
    {
        .chipid             = AIC_CHIP_ID_8800D80,
        .tx_hdr_crc8        = 1,
        .sdio_fc_reg_mask   = 0,
        .func_flag_tx_init  = 0,
        .sdio_wakeup_val    = 0x11,
        .sdio_sleep_ack_poll = 0,
        .sdio_misc_int_path = 1,
        .sdio_intmaskf2_decode = 1,
        .msgcfm_poll_path   = 0,
        .msg_fc_bypass      = 0,
        .sta_info_more_info = 1,
        .supports_5g        = 1,
    },
    {
        .chipid             = AIC_CHIP_ID_8800D80N,
        .tx_hdr_crc8        = 1,
        .sdio_fc_reg_mask   = 0,
        .func_flag_tx_init  = 0,
        .sdio_wakeup_val    = 0x11,
        .sdio_sleep_ack_poll = 0,
        .sdio_misc_int_path = 1,
        .sdio_intmaskf2_decode = 1,
        .msgcfm_poll_path   = 0,
        .msg_fc_bypass      = 1,
        .sta_info_more_info = 1,
        .supports_5g        = 1,
    },
    {
        .chipid             = AIC_CHIP_ID_8800D80X2,
        .tx_hdr_crc8        = 1,
        .sdio_fc_reg_mask   = 0,
        .func_flag_tx_init  = 0,
        .sdio_wakeup_val    = 0x11,
        .sdio_sleep_ack_poll = 0,
        .sdio_misc_int_path = 1,
        .sdio_intmaskf2_decode = 1,
        .msgcfm_poll_path   = 0,
        .msg_fc_bypass      = 0,
        .sta_info_more_info = 0,   /* X2 不带 more_info (原宏仅 D80/D80N) */
        .supports_5g        = 1,
    },
};

const struct aic_chip_desc *aic_chip_desc_lookup(uint16_t chipid)
{
    uint16_t i;

    for (i = 0; i < sizeof(aic_chip_desc_table) / sizeof(aic_chip_desc_table[0]); i++)
    {
        if (aic_chip_desc_table[i].chipid == chipid)
        {
            return &aic_chip_desc_table[i];
        }
    }
    return NULL;
}
