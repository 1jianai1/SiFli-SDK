/*
 * SPDX-FileCopyrightText: 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Copyright (C) 2018-2024 AICSemi Ltd.
 *
 * All Rights Reserved
 */

#ifndef _PLAT_PORT_H_
#define _PLAT_PORT_H_

#include "rtconfig.h"

#define PLATFORM_CACHE_LINE_SIZE        (32)

void platform_pwr_wifi_pin_init(void);
void platform_pwr_wifi_pin_enable(void);
void platform_pwr_wifi_pin_disable(void);
void platform_rst_wifi_pin_init(void);
void platform_rst_wifi_pin_enable(void);
void platform_rst_wifi_pin_disable(void);
void platform_rst_bt_pin_init(void);
void platform_rst_bt_pin_enable(void);
void platform_rst_bt_pin_disable(void);

#endif /* _PLAT_PORT_H_ */