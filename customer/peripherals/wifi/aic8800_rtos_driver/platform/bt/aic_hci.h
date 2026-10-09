/*
 * Copyright (C) 2018-2024 AICSemi Ltd.
 *
 * Host-facing HCI: H4 send, RX queue, then upper cb.
 * Transport is compile-time: AIC_HCI_OVER_SDIO (default), AIC_HCI_OVER_UART, or
 * AIC_HCI_OVER_USB (CMake AIC_HCI_TRANSPORT).
 */

#ifndef AIC_HCI_H
#define AIC_HCI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIC_HCI_H4_CMD  0x01u
#define AIC_HCI_H4_ACL  0x02u
#define AIC_HCI_H4_SCO  0x03u
#define AIC_HCI_H4_EVT  0x04u

typedef void (*aic_hci_rx_cb_t)(uint8_t type, uint8_t *data, uint16_t len);

int aic_hci_init(void);
/* 与 aic_hci_init 对称: 停 HCI RX 任务、摘 RX 回调并释放队列对象 (幂等) */
int aic_hci_deinit(void);
int aic_hci_send(uint8_t type, const uint8_t *data, uint16_t len);
void aic_hci_set_rx_cb(aic_hci_rx_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* AIC_HCI_H */
