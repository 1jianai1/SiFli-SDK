/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AIC8800D40N SDIO WiFi 示例 — 最小骨架
 *
 * 集成路线1: 驱动自建 lwIP netif, 不使用 SDK rt_wlan 框架。
 * main 尽量薄 —— 全部上电/总线/netif 启动逻辑交给驱动的
 * aic_wifi_demo_test() (customer/peripherals/wifi/aic8800d40n/platform/wifi/wifi_port_plt.c):
 *
 *   aic_wifi_demo_test()
 *     -> aic_platform_init()
 *     -> aicwf_aic_wifi_bus_waitready()   (注册 SDIO 驱动, 等卡就绪)
 *     -> wifi_driver_init()
 *     -> aic_wifi_init_mac()
 *     -> aicwf_net_if_plat_init()         (建立 lwIP netif)
 *
 * 注意(运行期, 非编译期): aic_wifi_demo_test() 会阻塞等待 SDIO 卡就绪,
 *   需真实硬件。无硬件时本工程的验收目标是"编译通过 + 库链入 + 生成
 *   main.elf/main.bin"。
 */
#include <rtthread.h>

/* 驱动入口 (platform 适配层, 由 aic8800d40n 的 SConscript 编入) */
extern void aic_wifi_demo_test(void);
/* 驱动 CLI 包装: cmd_test(line) -> aic_wifi_cmd((char*)line, 0, 0) */
extern void cmd_test(const char *temp_uart_buf);

int main(void)
{
    rt_kprintf("[aic8800d40n] demo start\n");

    /* 路线1: 驱动内部完成 平台初始化 / SDIO 总线注册 / netif 建立 */
    aic_wifi_demo_test();

    rt_kprintf("[aic8800d40n] driver up. use 'wifi_test <cmd>' to control\n");
    return 0;
}

/*
 * MSH: wifi_test wifi_on / scan / connect <ssid> <pwd> ...
 * 把参数拼成整行(<=64B)转 cmd_test -> aic_wifi_cmd(buf,0,0)。
 * (wifi_port_plt.c 的 cmd_test 未 MSH 导出, 故在此包一层)
 */
static int wifi_test(int argc, char **argv)
{
    char line[64];
    int  i, len = 0;

    if (argc < 2)
    {
        rt_kprintf("usage: wifi_test <cmd> [args...]\n");
        rt_kprintf("  e.g. wifi_test wifi_on\n");
        rt_kprintf("       wifi_test scan\n");
        rt_kprintf("       wifi_test connect <ssid> <pwd>\n");
        return -1;
    }

    for (i = 1; i < argc; i++)
    {
        len += rt_snprintf(line + len, sizeof(line) - len, "%s%s",
                           (i > 1) ? " " : "", argv[i]);
        if (len >= (int)sizeof(line) - 1)
            break;
    }

    cmd_test(line);
    return 0;
}
MSH_CMD_EXPORT(wifi_test, aic8800d40n wifi test command);
