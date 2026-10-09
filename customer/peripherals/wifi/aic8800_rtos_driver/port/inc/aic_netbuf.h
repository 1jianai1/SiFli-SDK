/*
 * Copyright (C) 2018-2024 AICSemi Ltd.
 *
 * All Rights Reserved
 *
 * Unified net_buf_tx_t definition.
 *
 * This is the SINGLE canonical definition of net_buf_tx_t.
 *
 * It lives in a standalone, minimal-dependency header so that:
 *   - netif_port.h   (port/inc/)  can reference net_buf_tx_t / net_if_t
 *   - netif_def.h    (drv_api/inc/) can alias it to TCPIP_PACKET_INFO_T
 *
 * None of these three files need circular-include guards anymore —
 * they simply #include "aic_netbuf.h" unconditionally.
 *
 * The struct layout is pointer-compatible with TCPIP_PACKET_INFO_T
 * (defined in netif_def.h for internal use).
 */

#ifndef _AIC_NETBUF_H_
#define _AIC_NETBUF_H_

#include "aic_porting_types.h"

typedef void *net_if_t;  /* opaque network interface handle */

typedef struct net_buf_tx
{
    uint8_t *data_ptr;
    uint32_t data_len;
    uint32_t pkt_type;
    uint32_t net_id;
    void    *frag_next;  /* [PLATFORM] nocopy TX (NET_IF_UAP_NCPYBUF=1): pbuf 链指针，
                            透传到 SDIO 聚合 scatter copy；copy 模式恒为 NULL */
#ifdef CONFIG_SDIO_ADMA
    /* ADMA 模式下 TX 缓冲由平台"整块分配 + 对齐"提供: data_ptr 指向对齐后的
     * payload, 整块起始地址(释放用)记在这里, 平台 netif_port.c 的
     * aicwf_net_buf_tx_free() 用它 aicwf_rtos_free()。
     * 与 host drv_api/inc/netif_def.h 的 TCPIP_PACKET_INFO_T.data_oprt 同语义;
     * 缺此字段时 platform/netif/netif_port.c 的 ADMA 分支编不过 (#136 no field)。 */
    uint8_t *data_oprt;
#endif
} net_buf_tx_t;

#endif /* _AIC_NETBUF_H_ */
