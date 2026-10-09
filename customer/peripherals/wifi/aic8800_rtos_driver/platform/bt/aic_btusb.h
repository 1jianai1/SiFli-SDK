#ifndef AIC_BTUSB_H
#define AIC_BTUSB_H

#include <stdint.h>

struct usb_interface;

int aic_btusb_drv_register(void);
void aic_btusb_drv_unregister(void);

/* WiFi USB probe 遇到 BT if 时调用（本平台 USB core 只试第一个匹配驱动） */
int aic_btusb_try_bind(struct usb_interface *intf);

int aic_btusb_ready(void);

int aic_bt_hci_cmd(const uint8_t *cmd, uint16_t cmd_len,
                   uint8_t *rsp, uint16_t rsp_max, uint16_t *rsp_len,
                   uint32_t timeout_ms);

int aic_ble_adv_start(const char *name);
int aic_ble_adv_stop(void);

/* Local RW test characteristic 0xFFF1 (handle 0x0006) */
int aic_ble_test_get(char *buf, int buflen);
int aic_ble_test_set(const char *data, int len);

#endif
