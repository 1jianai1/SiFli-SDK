/*
 * Copyright (C) 2018-2024 AICSemi Ltd.
 *
 * All Rights Reserved
 */

#include "wifi_api.h"
#include "sys_api.h"      /* 基准 d4509b7 同步: aic_pwrctl_enable_set 声明 (下方 [DEMO] pwrctl 块用) */
#include "wifi_if.h"      /* [AIC8800-PORT][SiFli] aic_wifi_init_mac 声明 */
#include "netif_api.h"
#include "fmac_port.h"
#include "aic_bus.h"
#include "wifi_port_priv.h"
#include "netif_port.h"
#include "aic_assert.h"
#include "aic_log.h"
#include "plat_port.h"
#include "aic_chip_desc.h"
#include <rtthread.h>          /* [AIC8800-PORT][SiFli] 原 FreeRTOS.h; RT_WAITING_FOREVER */
#ifdef CONFIG_SDIO_SUPPORT
    #include <drivers/mmcsd_core.h> /* [AIC8800-PORT][SiFli] 原 "mmcsd_core.h"; mmcsd_wait_cd_changed/MMCSD_HOST_PLUGED */
    #include "sdio_port.h"          /* aicwf_sdio_drv_register 声明 */

    /* [AIC8800-PORT][SiFli] mmcsd_get_stat 未在 mmcsd_core.h 中声明 (定义在 mmcsd_core.c:632),
    * 参照 swt6621s/wifi_device.c:72 同样方式局部声明。 */
    extern uint8_t mmcsd_get_stat(void);
#endif


/* [AIC8800-PORT][SiFli] platform 层内部函数, 无共享头声明 (基线靠隐式声明,
 * GCC14 升为 error); 定义处: aic_platform_init=platform_init.c:15,
 * wifi_driver_init=wifi_port.c:638, aic_wifi_cmd=wifi_port_cmd.c:120。按定义签名前置声明。 */
extern void aic_platform_init(void);
extern void wifi_driver_init(void);
extern void aic_wifi_cmd(char *atcmd, unsigned int athandle, unsigned char chan_num);

#define lwipx_ipv4_addr(addr) ((addr[0]) | (addr[1] << 8) | (addr[2] << 16) | (addr[3] << 24))

/**
 * @brief 等待总线设备就绪
 *
 * 根据编译期总线选择 (AIC_BUS_TYPE) 调用对应总线的就绪等待与驱动注册。
 * - SDIO: 阻塞等待 SDIO 设备插入并注册 SDIO 驱动
 * - USB:  调用 USB 驱动注册 (stub 阶段直接返回)
 */
static void aicwf_aic_wifi_bus_waitready(void)
{
#ifdef CONFIG_SDIO_SUPPORT
    /* [AIC8800-PORT][SiFli] 不能用 mmcsd_wait_cd_changed() 阻塞等 MMCSD_HOST_PLUGED:
     * mmcsd_core.c 只在 SD 卡/eMMC 探测成功和拔卡分支向 mmcsd_hotpluge_mb 投递消息,
     * **SDIO 探测分支不投递**, 纯 SDIO 场景下该等待永不返回, 主流程会卡死
     * (现场表现: 打印完 "AIC_SDIO_IRQ_MASK_FORCED=1" 后无任何 [AIC] 日志, 直接停在 shell)。
     * 改为轮询 mmcsd_get_stat() (探测流程已执行完), 与 swt6621s/wifi_device.c 的
     * rt_check_sdio_ready() 做法一致。 */
    int timeout = 50;   /* 最多等 5s */

    while (timeout-- > 0)
    {
        if (mmcsd_get_stat())
            break;
        aicwf_rtos_msleep(100);
    }

    if (mmcsd_get_stat())
        DBG_SDIO_INF("detect sdio device\r\n");
    else
        DBG_SDIO_ERR("sdio detect timeout\r\n");

    aicwf_sdio_drv_register();
#elif defined(CONFIG_USB_SUPPORT)
    DBG_USB_INF("usb bus waitready (stub)\r\n");
    aicwf_usb_drv_register();
#endif
}

/**
 * @brief 等待WiFi驱动初始化完成
 *
 * 轮询GetWiFiInitStat()直到驱动初始化完成。
 */
static void aicwf_aic_wifi_wait_driver_init(void)
{
    while (!GetWiFiInitStat())
    {
        aicwf_rtos_msleep(200);
    }
}
/**
 * @brief AIC WiFi驱动演示测试入口
 *
 * 完整的WiFi驱动启动流程：
 * 1. 平台初始化
 * 2. 等待SDIO设备就绪
 * 3. 初始化WiFi驱动
 * 4. 等待驱动初始化完成
 * 5. 初始化MAC和网络接口
 * 6. 配置默认IP/子网掩码
 * 7. 配置GPIO触发
 */
//start aic wifi driver
void aic_wifi_demo_test(void)
{
    uint8_t      addr_server[4] = {192, 168, 13, 1};
    uint8_t      addr_netmask[4] = {255, 255, 255, 0};
    uint32_t     server, mask;
    server = lwipx_ipv4_addr(addr_server);
    mask = lwipx_ipv4_addr(addr_netmask);

    aic_platform_init();

    /* [DEMO] SDIO 总线 pwrctl 运行时开关 — 时序: 必须在 wifi_driver_init() 之前。
     * pwrctl 任务/定时器在驱动初始化 (rwnx_main) 内按 g_aic_pwrctl_enable 初值
     * 决定是否创建, 后改不生效。
     *   aic_pwrctl_enable_set(0) = 调试: 总线恒 ACTIVE, 排除休眠时序干扰;
     *   aic_pwrctl_enable_set(1) = 量产默认 (不调用即为 1)。 */
#if defined(CONFIG_SDIO_BUS_PWRCTRL) || defined(CONFIG_USB_SUPPORT)
    aic_pwrctl_enable_set(0);
#endif

    aicwf_aic_wifi_bus_waitready();
    wifi_driver_init();
    aicwf_aic_wifi_wait_driver_init();
    aic_wifi_init_mac();
    // aicwf_net_if_plat_init();

    /* [DEMO] FW 日志级别配置 — 时序: 必须在驱动初始化完成后 (aic_wifi_init_mac
     * 之后) 调用, 依赖 g_rwnx_hw 与消息通道就绪。一站式打开: ① 0x13 回传通道
     * 总开关 ② lp_level 总闸门 ③ 模块过滤 ④ 级别阈值。此处配置全模块 + >=INF。
     * DC/DW family 不支持 FW 日志回传 (CFG_PRINT 0x13 通道), 跳过。 */
    if (aic_chip_desc_chipid_get() != AIC_CHIP_ID_8800DC &&
            aic_chip_desc_chipid_get() != AIC_CHIP_ID_8800DW)
    {
        aic_wifi_fw_log_enable(AIC_FW_MOD_DEFAULT, AIC_FW_SEV_INF);
    }
#if defined(AIC_FEATURE_BT) && defined(AIC_FEATURE_NIMBLE)
    /* BT patch 已由驱动初始化加载 (host 无条件 aicbt_init);
     * 此处拉起 NimBLE 协议栈任务 (STACK/nimble 提供)。需 AIC_FEATURE_NIMBLE
     * 编入 nimble 才有此符号, 否则链接失败; 故与 BT 门控取与。
     * nimble_stack_start 内部自检 aic_bt_fw_ready(), fw 未加载则拒绝启动。 */
    {
        extern int nimble_stack_start(void);
        (void)nimble_stack_start();
    }
#endif


    aicwf_net_if_set_tousedip(server);
    aicwf_net_if_set_netmask(mask);

    aicwf_plat_gpio_trigger(DRV_TRG_SDIO_RX, 0);
    aicwf_plat_gpio_trigger(DRV_TRG_FHOST_RX, 0);
    aicwf_plat_gpio_trigger(DRV_TRG_FHOST_TX, 0);
    aicwf_plat_gpio_trigger(DRV_TRG_NET_TX, 0);
    aicwf_plat_gpio_trigger(DRV_TRG_SDIO, 0);
    aicwf_plat_gpio_trigger(DRV_TRG_POWCTR, 0);
    aicwf_plat_gpio_trigger(DRV_TRG_USB, 0);
}
/**
 * @brief AT命令测试接口
 *
 * 将串口接收的AT命令转发给WiFi命令处理函数。
 *
 * @param temp_uart_buf 串口接收的AT命令字符串
 */
//test aic wifi command
void cmd_test(const char *temp_uart_buf)
{
    aic_wifi_cmd((char *)temp_uart_buf, 0, 0);
}
