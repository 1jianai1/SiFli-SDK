/*
 * Copyright (C) 2018-2025 AICSemi Ltd.
 *
 * All Rights Reserved
 *
 * Host-owned SDIO type definitions.
 * Provides SDIO constants without platform dependencies.
 */

#ifndef _AIC_SDIO_TYPES_H_
#define _AIC_SDIO_TYPES_H_

#include <stdint.h>

/* SDIO function numbers */
#define FUNC_0              0
#define FUNC_1              1
#define FUNC_2              2
#define SDIOM_MAX_FUNCS     3

/* TX slist max entries */
#define SDIO_TX_SLIST_MAX   136

/* SDIO scatter-gather list (ADMA mode)
 *
 * 字段名必须与 host 侧用法一致: layer_common/src/sdio_co.c 组帧时写的是
 *   sdiodev->sg_list[i].buf / .len
 * (旧版此处为 addr → 开 CONFIG_SDIO_ADMA 时 host 编不过)。
 * 同时 host 的 drv_api/inc/netif_def.h 里也是按 `buf` 语义落数据段。
 *
 * 平台若已有自己的 sg 类型 (HAL 提供同名同布局类型), 可在平台 port 头定义
 * AICWF_SDIO_SG_LIST_EXTERNAL 并先 include 平台 HAL 头, 从而跳过本定义,
 * 避免重复声明 (#256 invalid redeclaration)。示例 (ASR/064 平台):
 *   #define AICWF_SDIO_SG_LIST_EXTERNAL
 *   #include "sdio.h"   // typedef struct { unsigned char *buf; unsigned int len; } sdio_sg_list;
 * 约束: 平台类型必须与 host 语义一致 —— 首成员为段起始地址, 次成员为段长。
 */
#ifdef CONFIG_SDIO_ADMA
#ifndef AICWF_SDIO_SG_LIST_EXTERNAL
typedef struct
{
    void     *buf;   /* 段起始地址 (host: sdiodev->sg_list[i].buf) */
    uint32_t  len;   /* 段长 (字节) */
} sdio_sg_list;
#endif /* AICWF_SDIO_SG_LIST_EXTERNAL */
#endif /* CONFIG_SDIO_ADMA */

#endif /* _AIC_SDIO_TYPES_H_ */
