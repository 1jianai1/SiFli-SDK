/*
 * Copyright (C) 2018-2024 AICSemi Ltd.
 *
 * All Rights Reserved
 *
 * SDIO port header: ops-dispatch macros.
 */

#ifndef _SDIO_PORT_H_
#define _SDIO_PORT_H_

#include "rtos_port.h"
#include "co_list.h"
#include "aic_sdio_types.h"

/*
 * Runtime-configurable SDIO behavior flags.
 *
 * These were originally compile-time #define macros. However, since host
 * layer code (sdio_co.c) references them via #if directives, changing the
 * macros after the host library (.a) is built had no effect. Converting to
 * runtime variables allows platform code to set them at init time without
 * recompiling the host library.
 *
 * Platform defines and initializes these variables (typically in sdio_port.c).
 * Host code reads them at runtime via the extern declarations below.
 *
 * Default values are provided as _DEFAULT macros for platform convenience.
 */
/*
 * 默认值策略: RX 默认在【独立 task + 信号量】中处理 (=0, 参考实现, 见 2a5bf32
 * "SDIO RX 默认独立任务模式"); 仅当平台 SDIO 驱动在 ISR 内直接处理 RX 时才改为 1。
 * 运行期取值由下方 config_* 变量决定, 这里的编译期默认值只供平台初始化参考。
 * 详见 docs/aic8800-porting、aic8800-release-adaptation、aic8800-sync SKILL.md。
 */
/* 三个默认值均以 #ifndef 包裹: 平台可用 -DCONFIG_*_DEFAULT=<v> 编译期覆盖
 * (此前为裸 #define, 与 -D 冲突导致覆盖失效 —— 回流自 goke 平台存疑上报);
 * 运行期覆盖走下方 config_rxtask_insdio / config_irq_sus / aicwifi_sdio_rxdata_mode。 */
#ifndef CONFIG_RXTASK_INSDIO_DEFAULT
    #define CONFIG_RXTASK_INSDIO_DEFAULT    0   /**< RX in SDIO ISR context (1=in-ISR, 0=separate task, see strategy above) */
#endif
#ifndef CONFIG_RX_POLLING_DEFAULT
    #define CONFIG_RX_POLLING_DEFAULT       0   /**< RX data mode (0=interrupt-driven, 1=polling) */
#endif
#ifndef CONFIG_IRQ_SUS_DEFAULT
    #define CONFIG_IRQ_SUS_DEFAULT          0   /**< Re-enable SDIO IRQ after RX (0=no, 1=yes) */
#endif

extern uint8_t config_rxtask_insdio;    /**< runtime flag: CONFIG_RXTASK_INSDIO_DEFAULT */
extern uint8_t config_irq_sus;          /**< runtime flag: CONFIG_IRQ_SUS_DEFAULT */
extern unsigned char aicwifi_sdio_rxdata_mode; /**< runtime flag: CONFIG_RX_POLLING_DEFAULT (0=interrupt, 1=polling) */

/* Export variables (provided by platform or stubs) */
extern int msgcfm_poll_en;
extern uint8_t sdio_unexcept_test;
extern struct aic_sdio_dev sdio_dev;

#ifdef CONFIG_SDIO_ADMA
    extern unsigned char sdio_tx_buf_fill[512];
    extern unsigned char sdio_tx_buf_dummy[SDIO_TX_SLIST_MAX][8];
#endif

/* ---- SDIO abstraction — platform must implement ALL functions ---- */

/* Host control */
void aicwf_sdio_claim_host(uint32_t func_num);
void aicwf_sdio_release_host(uint32_t func_num);

/* ------------------------------------------------------------------
 * Bulk data transfer (CMD53) —— host <-> platform 的 ADMA 套接契约
 *
 * 非 ADMA (CONFIG_SDIO_ADMA 未定义):
 *   src/dst = 连续缓冲, count = 字节数; 平台需在写前 CacheClean、读后 CacheInvalidate
 *   (或由平台 HAL 内部完成)。
 *
 * ADMA (CONFIG_SDIO_ADMA 定义):
 *   aicwf_sdio_writesb(func, addr, sg_list, segcnt):
 *     src   = const sdio_sg_list *  (host 已填好每段的 buf/len, 含 8B SDIO 头段与 tail fill 段)
 *     count = 段数 (不是字节数!)
 *     平台职责: 把 sg 交给平台 DMA/描述符机制, 并对每段做 CacheClean + 同步屏障;
 *   aicwf_sdio_readsb(func, addr, dst, count):
 *     ADMA 下为单缓冲读 (平台可实现为 sg 长度 1), 平台需 CacheInvalidate;
 *   aicwf_sdio_writesb_msg(): 控制帧, 始终连续缓冲 + CacheClean (不走 ADMA 聚合)。
 *
 * 平台还需提供 (CONFIG_SDIO_ADMA 下, 声明见下):
 *   sdio_tx_buf_fill[512] / sdio_tx_buf_dummy[SDIO_TX_SLIST_MAX][8] —— cache 对齐, host 只填内容;
 *   PROTECT_TX_ADMA —— 保证发送完成前缓冲不被回收/改写
 *   (参考实现: 发送后轮询 DMA 完成标志再返回; 无保护时 host 复用聚合缓冲会踩到在飞数据)。
 *
 * sdio_sg_list 类型来源: port/inc/aic_sdio_types.h (平台可用 AICWF_SDIO_SG_LIST_EXTERNAL
 * 复用平台 HAL 类型)。平台侧 ADMA 接入清单与参考实现见 aidoc/adma平台移植参考.md。
 * ------------------------------------------------------------------ */
int  aicwf_sdio_readsb(uint32_t func_num, void *dst, unsigned int addr, int count);
int  aicwf_sdio_writesb(uint32_t func_num, unsigned int addr, void *src, int count);
int  aicwf_sdio_writesb_msg(uint32_t func_num, unsigned int addr, void *src, int count);

/* Register access (CMD52) */
bool aicwf_sdio_readb_cmd52(uint32_t addr, uint8_t *data);
bool aicwf_sdio_writeb_cmd52(uint32_t addr, uint8_t data);
bool aicwf_sdio_readb_cmd52_func2(uint32_t addr, uint8_t *data);
bool aicwf_sdio_writeb_cmd52_func2(uint32_t addr, uint8_t data);

/* 固件加载完成后补写 intr_config_reg 使能 WiFi 芯片侧 SDIO 中断 (sdio_port.c) */
int aicwf_sdio_enable_irq_after_fw_boot(void);

/* Initialization */
int  aicwf_sdio_func_init(uint16_t chipid, struct aic_sdio_dev *sdiodev);
int  aicwf_sdiov3_func_init(uint16_t chipid, struct aic_sdio_dev *sdiodev);
void aicwf_sdio_release_func2(void);

/* Interrupt */
bool aicwf_sdio_host_enable_isr(bool enable);
void aicwf_sdio_set_irq_handler(void (*cb)(void));
int  aicwf_sdio_interrupt_init(void);
void aicwf_sdio_isr(uint8_t en);

/* Driver registration */
int  aicwf_sdio_drv_register(void);

#ifdef CONFIG_OOB
    void aicwf_sdio_oob_enable(void);
#endif

#endif /* _SDIO_PORT_H_ */
