/*
 * AIC8800D80 SDIO WiFi 集成工程 —— 最小骨架 (路线1: 驱动自建 lwIP netif)
 *
 * main 尽量薄: 全部上电/总线/netif 启动逻辑交给驱动 aic_wifi_demo_test()
 * (platform/wifi/wifi_port_plt.c:74 -> aic_platform_init() + aicwf_aic_wifi_bus_waitready())。
 *
 * 注意(运行期, 非编译期): aic_wifi_demo_test() 会阻塞等待 SDIO 卡就绪, 需真实硬件;
 *   无硬件时本工程的验收目标是"编译通过 + 三库链入 + 生成 main.elf/bin"。
 *
 * D80N(0x9082) 的 flow-ctrl 适配说明(历史, 勿照旧注释改代码):
 *   实际生效的是 sdio_port.c 里 aicwf_sdio_readb_cmd52() 的**读侧重定向**
 *   (Q1 0x03 -> Q2 0x09, 含 FW boot 后收回重定向), 不是写 sdio_reg.flow_ctrl_reg ——
 *   写侧方案已试验并否决(库始终把该字段保持 0x03)。
 *   厂家 b4036bf 起另提供 aic_chip_desc.msg_fc_bypass(D80N 条目置 1) 免 msg 通路流控。
 *   注: 本次合并的 lib/ 预编译库为 **D80** 版(WiFi-only), 芯片型号宏已切 D80。
 */
#include <rtthread.h>

/* nettest 命令所需的 socket 头 (与 packages/iot/netutils/iperf 相同的取法) */
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>

/* 驱动入口 (platform 适配层, 由 scons 编入) */
extern void aic_wifi_demo_test(void);
/* 驱动 CLI 包装: cmd_test(line) -> aic_wifi_cmd((char*)line, 0, 0) */
extern void cmd_test(const char *temp_uart_buf);

/* ---------------------------------------------------------------------------
 * SDIO(DAT1) 中断风暴规避开关   (机理详见 sdio_port.c 里 g_sdio_irq_mask_forced)
 *
 *   FW 起来后会持续拉 DAT1(SDIO 中断)。SDHCI_INT_CARD_INT 是电平中断, 而 host 库
 *   aicwf_sdio_rx_task 每轮结束都无条件 sdio_host_enable_isr(TRUE), 于是:
 *       中断 -> 唤醒RX任务 -> 读 intstatus=0 -> 再放开中断 -> 立刻又中断 ...
 *   该中断 NVIC 优先级高于串口, 把 shell 的输入中断饿死 => "整机卡死"
 *   (现象: 只打印一次 "msh />", 之后回车无响应; 不加过滤时串口被
 *    "[FW] Interrupt but no data" 刷屏)。
 *
 *   1 = 规避: 不放开 SDIO 中断, 收包改走库内 10ms 轮询        (默认, 实测正常)
 *   0 = 原样: 中断驱动收包 -> 复现上述卡死, 用于 A/B 对比
 *
 * 注: 置 0 时不会去改 aicwifi_sdio_rxdata_mode / g_sdio_irq_mask_forced,
 *     两者保持驱动自身默认值(CONFIG_RX_POLLING_DEFAULT=0 / 0), 即完全原始行为。
 * ------------------------------------------------------------------------- */
#define AIC_SDIO_IRQ_MASK_FORCED   1

extern unsigned char aicwifi_sdio_rxdata_mode;  /* sdio_port.c: 0=中断驱动 1=轮询 */
extern uint32_t      g_sdio_irq_mask_forced;    /* sdio_port.c: 1=不放开 SDIO 中断 */

static void aic_sdio_irq_policy_apply(void)
{
#if AIC_SDIO_IRQ_MASK_FORCED
    aicwifi_sdio_rxdata_mode = 1;
    g_sdio_irq_mask_forced   = 1;
#endif
    /* 唯一一条开关横幅, 便于 A/B 对比时确认烧的是哪个配置; 不需要可直接删 */
    rt_kprintf("[aic8800] AIC_SDIO_IRQ_MASK_FORCED=%d\n", AIC_SDIO_IRQ_MASK_FORCED);
}

int main(void)
{
    rt_kprintf("[aic8800] demo start\n");

    /* 必须在驱动初始化之前设置: 决定 SDIO RX 用中断还是轮询 */
    aic_sdio_irq_policy_apply();

    /* 路线1: 驱动内部完成 aic_platform_init() / SDIO 总线注册 / netif 建立 */
    aic_wifi_demo_test();

    rt_kprintf("[aic8800] driver up. try: wifi_test iwpriv sta scan\n");
    return 0;
}

/*
 * MSH: wifi_test <占位token> <命令> [args...]
 *
 * 注意 aic_wifi_cmd() 会丢弃整行的第一个 token (AICWF_CMD_SKIP_PREFIX=1),
 * 所以第一个 token 是占位符(文档习惯写 iwpriv), 真正的命令从第二个开始:
 *   wifi_test iwpriv uapst                 # 查 wifi st/init 状态
 *   wifi_test iwpriv info
 *   wifi_test iwpriv mac 0
 *   wifi_test iwpriv switch sta            # 切模式, 之后等 2~3s
 *   wifi_test iwpriv sta scan              # 扫描(阻塞), 逐条打印, 末尾 +OK / +NONE
 *   wifi_test iwpriv sta conn <ssid> <pwd> <timeout_ms>
 *   wifi_test iwpriv sta stop
 * 识别/成功看那几个回包行(+OK/+ERR/+NONE); 末尾的 "aic cmd finish" 是分发函数
 * 末尾无条件打印的, 不能当作成功标志。
 *
 * 实现: 把参数拼成整行(<=64B)转 cmd_test -> aic_wifi_cmd(buf,0,0)。
 * (wifi_port_plt.c 的 cmd_test 未 MSH 导出, 故在此包一层)
 */
static int wifi_test(int argc, char **argv)
{
    char line[64];
    int  i, len = 0;

    if (argc < 2)
    {
        rt_kprintf("usage: wifi_test <prefix> <cmd> [args]\n"
                   "  e.g. wifi_test iwpriv sta scan\n");
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
MSH_CMD_EXPORT(wifi_test, aic8800 wifi test command);
