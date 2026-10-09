/*
 * Copyright (C) 2018-2024 AICSemi Ltd.
 *
 * HCI wrapper: H4 send + RX queue. Bus is aic_bt_sdio / uart / usb.
 */

#include <stddef.h>
#include <string.h>
#include "aic_hci.h"
#include "rtos_port.h"

#if defined(AIC_HCI_OVER_UART)
    #include "aic_bt_uart.h"
    #define hci_tr_start     aic_bt_uart_hci_start
    #define hci_tr_send      aic_bt_uart_send
    #define hci_tr_set_rx_cb aic_bt_uart_set_rx_cb
#elif defined(AIC_HCI_OVER_USB)
    #include "aic_bt_usb.h"
    #define hci_tr_start     aic_bt_usb_start
    #define hci_tr_send      aic_bt_usb_send
    #define hci_tr_set_rx_cb aic_bt_usb_set_rx_cb
#elif defined(AIC_HCI_OVER_SDIO)
    #include "aic_bt_sdio.h"
    #define hci_tr_start     aic_bt_sdio_start
    #define hci_tr_send      aic_bt_sdio_send
    #define hci_tr_set_rx_cb aic_bt_sdio_set_rx_cb
#else
    #error "unknown BT HCI transport: define AIC_HCI_OVER_SDIO / _UART / _USB"
#endif

#define HCI_H4_MAX      1024
#define HCI_RX_Q_DEPTH  16
#define HCI_RX_STACK    3072
#define HCI_RX_PRIO     80
#define HCI_RX_TASK_ID  21

struct hci_pkt
{
    uint16_t len;
    uint8_t data[HCI_H4_MAX];
};

static rtos_semaphore s_rx_sem;
static rtos_task_handle s_rx_task;
static rtos_mutex s_rx_mu;
static aic_hci_rx_cb_t s_rx_cb;
static struct hci_pkt s_rxq[HCI_RX_Q_DEPTH];
static struct hci_pkt s_rx_work;
static uint32_t s_rxq_head;
static uint32_t s_rxq_tail;
static int s_inited;

static int
h4_is_cmd_ack(const uint8_t *data, uint32_t len)
{
    return (len >= 2u && data[0] == AIC_HCI_H4_EVT &&
            (data[1] == 0x0Eu || data[1] == 0x0Fu));
}

static int
rxq_put(const uint8_t *data, uint32_t len)
{
    uint32_t next;
    int rc = -1;

    if (data == NULL || len == 0u || len > HCI_H4_MAX)
    {
        return -1;
    }

    (void)aicwf_rtos_mutex_lock(s_rx_mu, -1);
    next = (s_rxq_head + 1u) % HCI_RX_Q_DEPTH;
    if (next != s_rxq_tail)
    {
        s_rxq[s_rxq_head].len = (uint16_t)len;
        memcpy(s_rxq[s_rxq_head].data, data, len);
        s_rxq_head = next;
        rc = 0;
    }
    else if (h4_is_cmd_ack(data, len))
    {
        s_rxq_tail = (s_rxq_tail + 1u) % HCI_RX_Q_DEPTH;
        next = (s_rxq_head + 1u) % HCI_RX_Q_DEPTH;
        if (next != s_rxq_tail)
        {
            s_rxq[s_rxq_head].len = (uint16_t)len;
            memcpy(s_rxq[s_rxq_head].data, data, len);
            s_rxq_head = next;
            rc = 0;
        }
    }
    (void)aicwf_rtos_mutex_unlock(s_rx_mu);
    return rc;
}

static int
rxq_get(struct hci_pkt *out)
{
    uint16_t len;

    (void)aicwf_rtos_mutex_lock(s_rx_mu, -1);
    if (s_rxq_head == s_rxq_tail)
    {
        (void)aicwf_rtos_mutex_unlock(s_rx_mu);
        return -1;
    }
    len = s_rxq[s_rxq_tail].len;
    out->len = len;
    memcpy(out->data, s_rxq[s_rxq_tail].data, len);
    s_rxq_tail = (s_rxq_tail + 1u) % HCI_RX_Q_DEPTH;
    (void)aicwf_rtos_mutex_unlock(s_rx_mu);
    return 0;
}

static void
tr_rx(uint8_t *data, uint32_t len)
{
    if (rxq_put(data, len) == 0 && s_rx_sem != NULL)
    {
        (void)aicwf_rtos_semaphore_signal(s_rx_sem, false);
    }
}

static void
hci_rx_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        (void)aicwf_rtos_semaphore_wait(s_rx_sem, -1);
        while (rxq_get(&s_rx_work) == 0)
        {
            if (s_rx_work.len < 1u)
            {
                continue;
            }
            if (s_rx_cb != NULL)
            {
                s_rx_cb(s_rx_work.data[0], &s_rx_work.data[1],
                        (uint16_t)(s_rx_work.len - 1u));
            }
        }
    }
}

void
aic_hci_set_rx_cb(aic_hci_rx_cb_t cb)
{
    s_rx_cb = cb;
}

int
aic_hci_send(uint8_t type, const uint8_t *data, uint16_t len)
{
    uint8_t buf[HCI_H4_MAX];

    if ((uint32_t)len + 1u > HCI_H4_MAX)
    {
        return -1;
    }
    if (data == NULL && len != 0u)
    {
        return -1;
    }
    buf[0] = type;
    if (len != 0u)
    {
        memcpy(&buf[1], data, len);
    }
    return hci_tr_send(buf, (uint32_t)len + 1u);
}

int
aic_hci_init(void)
{
    if (s_inited)
    {
        return 0;
    }
    if (aicwf_rtos_mutex_create(&s_rx_mu, "hci_rxq") != 0)
    {
        return -1;
    }
    if (aicwf_rtos_semaphore_create(&s_rx_sem, "hci_rx",
                                    (int)(HCI_RX_Q_DEPTH + 8u), 0) != 0)
    {
        return -1;
    }
    if (aicwf_rtos_task_create(hci_rx_task, "hci_rx", HCI_RX_TASK_ID,
                               HCI_RX_STACK, NULL, HCI_RX_PRIO,
                               &s_rx_task) != 0)
    {
        return -1;
    }

    hci_tr_set_rx_cb(tr_rx);
    if (hci_tr_start() != 0)
    {
        return -1;
    }
    s_inited = 1;
    return 0;
}

int
aic_hci_deinit(void)
{
    int i;

    if (!s_inited)
    {
        return 0;
    }

    /* 先封入口: 之后到达的 FW 数据不再入队/不再回调上层 */
    s_inited = 0;
    s_rx_cb = NULL;

    /* 请 hci_rx 任务退出 (阻塞在信号量上, 补一次 signal 唤醒), 它随后自删并置空句柄 */
    s_rx_stop = 1;
    if (s_rx_sem != NULL)
    {
        (void)aicwf_rtos_semaphore_signal(s_rx_sem, false);
    }
    for (i = 0; i < HCI_RX_EXIT_TIMEOUT_MS && s_rx_task != NULL; i++)
    {
        aicwf_rtos_msleep(1);
    }
    if (s_rx_task != NULL)
    {
        /* 兜底: 任务未按预期退出则强制删除, 避免其继续挂在即将释放的信号量上 */
        aicwf_rtos_task_delete(s_rx_task);
        s_rx_task = NULL;
    }

    /* 等一拍, 让可能在 rxq_put 临界区内的 tr_rx (fhost_rx_task 上下文) 走完再释放对象 */
    aicwf_rtos_msleep(1);
    if (s_rx_sem != NULL)
    {
        aicwf_rtos_semaphore_delete(s_rx_sem);
        s_rx_sem = NULL;
    }
    if (s_rx_mu != NULL)
    {
        aicwf_rtos_mutex_delete(s_rx_mu);
        s_rx_mu = NULL;
    }
    s_rxq_head = 0;
    s_rxq_tail = 0;
    s_rx_stop = 0;

    return 0;
}
