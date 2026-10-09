/*
 * Copyright (C) 2018-2024 AICSemi Ltd.
 *
 * All Rights Reserved
 */

#include <rtthread.h>
#include "bf0_hal.h"
#include "plat_port.h"
#include "rtos_port.h"

/* 上电时序参数, 与 platform/hal/plat_port.c 保持一致 (参考 aic8800d40 aic8800_ele) */
#define AIC_WIFI_PWR_OFF_MS     (10)
#define AIC_WIFI_PWR_ON_MS      (200)

/**
 * @brief WiFi 芯片上电 (板级, 时序参考 aic8800d40 驱动的 aic8800_ele)
 *
 * 拉低电源脚 -> 延时 -> 拉高 -> 延时等待芯片 ROM 启动。
 *
 * @note 必须注册在 INIT_BOARD_EXPORT: SDMMC/SDHCI host 驱动在
 *       INIT_DEVICE_EXPORT 阶段枚举 SDIO 卡, 因此上电要早于 host 初始化,
 *       否则卡在上电前已完成(失败的)枚举。引脚号未配置(负值)时本函数不做
 *       任何操作。
 *
 * @note 此处调度器尚未启动, 不能用 rt_thread_mdelay 等阻塞式延时,
 *       必须用 HAL_Delay 忙等 (与 aic8800d40 的 aic8800_ele 一致)。
 */
static int aicwf_wifi_power_on(void)
{
    aicwf_platform_pwr_wifi_pin_init();
    aicwf_platform_pwr_wifi_pin_disable();
    HAL_Delay(AIC_WIFI_PWR_OFF_MS);
    aicwf_platform_pwr_wifi_pin_enable();
    HAL_Delay(AIC_WIFI_PWR_ON_MS);

    return 0;
}
INIT_BOARD_EXPORT(aicwf_wifi_power_on);

/**
 * @brief 平台初始化函数（extern方式下为空操作）
 *
 * 在extern链接方式下，所有平台函数由链接器直接解析绑定，
 * 无需显式注册，本函数为空实现。
 *
 * @note 该函数在WiFi驱动演示测试入口 aic_wifi_demo_test() 中最先被调用。
 *       芯片上电已在 INIT_BOARD_EXPORT(aicwf_wifi_power_on) 完成, 此处不再
 *       重复上电, 避免枚举完成后复位掉 SDIO 卡。
 */
void aic_platform_init(void)
{
}
