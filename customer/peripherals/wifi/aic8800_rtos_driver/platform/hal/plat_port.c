/*
 * Copyright (C) 2018-2024 AICSemi Ltd.
 *
 * All Rights Reserved
 */

#include "aic_log.h"
#include "plat_port.h"
#include "compiler.h"
#include "rtos_port.h"
#include <stdarg.h>
// [PLATFORM] RT-Thread 平台头文件
#ifdef RT_THREAD
    #include <rtthread.h>
    #include <drivers/pin.h>
#endif
#include "aic_assert.h"
#include <stdio.h>
#include "userconfig.h"

#ifndef LINE
    #define LINE "\r\n"
#endif

/* 上电引脚: 由 Kconfig AIC8800_RTOS_WIRELESS_PWR_GPIO 经 SConscript 注入;
 * 值为负表示未接/未配置, 此时不做任何 GPIO 操作, 由硬件自行保证上电。 */
#ifndef AIC_WIRELESS_PWR_GPIO
    #define AIC_WIRELESS_PWR_GPIO (-1)
#endif

/* 上电时序参数 (参考 aic8800d40 驱动 aic8800_ele):
 * 拉低 -> 延时 -> 拉高 -> 延时等待芯片 ROM 启动 */
#define AIC_WIFI_PWR_OFF_MS     (10)
#define AIC_WIFI_PWR_ON_MS      (200)

/* Task Priority */
uint32_t sdio_datrx_priority       = SDIO_DATRX_PRIORITY;
uint32_t sdio_oobirq_priority      = SDIO_OOBIRQ_PRIORITY;
uint32_t sdio_pwrclt_priority      = SDIO_PWRCLT_PRIORITY;
uint32_t fhost_cntrl_priority      = FHOST_CNTRL_PRIORITY;
uint32_t fhost_wpa_priority        = FHOST_WPA_PRIORITY;
uint32_t fhost_tx_priority         = FHOST_TX_PRIORITY;
uint32_t fhost_rx_priority         = FHOST_RX_PRIORITY;
uint32_t fhost_tcpack_priority     = FHOST_TCPACK_PRIORITY;
uint32_t cli_cmd_priority          = CLI_CMD_PRIORITY;
uint32_t rwnx_timer_priority       = RWNX_TIMER_PRIORITY;
uint32_t rwnx_wifi_priority        = RWNX_WIFI_PRIORITY;
uint32_t rwnx_apm_priority = RWNX_APM_STALOSS_PRIORITY;
uint32_t tcpip_priority            = TCPIP_PRIORITY;
uint32_t task_end_prio             = TASK_END_PRIO;

/* Task Stack Size */
uint32_t sdio_datrx_stack_size       = SDIO_DATRX_STACK_SIZE;
uint32_t sdio_oobirq_stack_size      = SDIO_OOOBIRQ_STACK_SIZE;
uint32_t sdio_pwrctl_stack_size      = SDIO_PWRCTL_STACK_SIZE;
uint32_t fhost_cntrl_stack_size      = FHOST_CNTRL_STACK_SIZE;
uint32_t fhost_wpa_stack_size        = FHOST_WPA_STACK_SIZE;
uint32_t fhost_tx_stack_size         = FHOST_TX_STACK_SIZE;
uint32_t fhost_rx_stack_size         = FHOST_RX_STACK_SIZE;
uint32_t fhost_tcpack_stack_size     = FHOST_TCPACK_STACK_SIZE;
uint32_t cli_cmd_stack_size          = CLI_CMD_STACK_SIZE;
uint32_t rwnx_timer_stack_size       = RWNX_TIMER_STACK_SIZE;
uint32_t rwnx_wifi_stack_size        = RWNX_WIFI_STACK_SIZE;
uint32_t rwnx_apm_stack_size         = RWNX_APM_STACK_SIZE;
/* driver param setting */
/* define WPA log level */
enum
{
    WPA_MSG_EXCESSIVE,
    WPA_MSG_MSGDUMP,
    WPA_MSG_DEBUG,
    WPA_MSG_INFO,
    WPA_MSG_WARNING,
    WPA_MSG_ERROR
};
//dcdc mode
int aicwifi_vrf_mode = VRF_DCDC_MODE;
//wpa debug level
int aicwifi_wpa_log_level = WPA_MSG_INFO; /* DEBUG 会刷屏 RX_MGMT / 邻 BSS beacon */

/* 上电引脚编号, 负值表示无效 */
static int aicwf_wifi_pwr_pin = -1;

/* 首次上电时序完成(由板级 INIT_BOARD 触发)后置位, 用于屏蔽之后多余的电源循环。
 * 预编译库 aic_wifi_open() 内部仍会执行一次 pin_init/disable/10ms/enable, 但该时刻
 * SDIO 已完成枚举并工作在 4bit/高速模式, 芯片一掉电复位就会丢失 RCA 与总线宽度配置,
 * 而主机不会再枚举, 导致后续 CMD52 全部超时(实机日志 0x18001)。
 * 参考 aic8800d40 驱动: 同样的电源循环在 aic8800_init() 中是被 #if 0 屏蔽的。 */
static rt_bool_t aicwf_wifi_pwr_locked = RT_FALSE;

static void aicwf_wifi_pwr_pin_set(int on)
{
    if (aicwf_wifi_pwr_pin >= 0)
    {
        rt_pin_write(aicwf_wifi_pwr_pin, on ? PIN_HIGH : PIN_LOW);
    }
}

/**
 * @brief 初始化WiFi电源引脚（平台相关）
 * @note 引脚号来自 Kconfig AIC8800_RTOS_WIRELESS_PWR_GPIO (经 SConscript 注入
 *       为 AIC_WIRELESS_PWR_GPIO); 为负则视为未使用, 后续操作全部跳过。
 */
void aicwf_platform_pwr_wifi_pin_init(void)
{
    if (aicwf_wifi_pwr_pin >= 0)
    {
        return; /* 已初始化过, 不重复配置 */
    }

    aicwf_wifi_pwr_pin = AIC_WIRELESS_PWR_GPIO;
    if (aicwf_wifi_pwr_pin >= 0)
    {
        rt_pin_mode(aicwf_wifi_pwr_pin, PIN_MODE_OUTPUT);
        DBG_CMN_INF("wifi pwr pin = %d\n", aicwf_wifi_pwr_pin);
    }
}

/**
 * @brief 使能WiFi电源引脚（平台相关）
 * @note 首次拉高(板级上电时序)后置位 aicwf_wifi_pwr_locked, 之后不再重复操作。
 */
void aicwf_platform_pwr_wifi_pin_enable(void)
{
    if (aicwf_wifi_pwr_locked)
    {
        return;
    }

    aicwf_wifi_pwr_pin_set(1);
    aicwf_wifi_pwr_locked = RT_TRUE;
}

/**
 * @brief 禁用WiFi电源引脚（平台相关）
 * @note 上电时序完成后调用会被忽略: 此时 SDIO 已枚举, 掉电会让芯片复位并使
 *       后续 CMD52 超时; 需要真正断电/复位请走 aicwf_sdio_pdn_wifi()/
 *       aicwf_sdio_reset_wifi() (它们会先解除该保护)。
 */
void aicwf_platform_pwr_wifi_pin_disable(void)
{
    if (aicwf_wifi_pwr_locked)
    {
        return;
    }

    aicwf_wifi_pwr_pin_set(0);
}

/**
 * @brief 格式化字符串到缓冲区（调试用sprintf）
 * @param buffer 输出缓冲区
 * @param fmt    格式化字符串
 * @param ...    可变参数
 * @return 写入的字符数
 */
uint32_t aicwf_dbg_sprintf(char *buffer, const char *fmt, ...)
{
    uint32_t ret;
    const char *fmt_usr = (const char *) fmt;

    // print
    va_list args;
    va_start(args, fmt);

    ret = dbg_vsprintf(buffer, fmt_usr, args);

    va_end(args);

    return ret;
}

/**
 * @brief 格式化字符串到缓冲区（调试用snprintf，带长度限制）
 * @param buffer 输出缓冲区
 * @param size   缓冲区大小
 * @param fmt    格式化字符串
 * @param ...    可变参数
 * @return 写入的字符数
 */
uint32_t aicwf_dbg_snprintf(char *buffer, uint32_t size, const char *fmt, ...)
{
    uint32_t ret;
    const char *fmt_usr = (const char *) fmt;

    // print
    va_list args;
    va_start(args, fmt);

    ret = dbg_vsnprintf(buffer, size, fmt_usr, args);

    va_end(args);

    return ret;
}

/**
 * @brief 判断是否为生产模式
 * @return 0=非生产模式, 非0=生产模式
 */
uint32_t aicwf_hal_IsInProduction_Mode(void)
{
    return 0;
}

/**
 * @brief 切换PP（功耗性能）等级
 * @param lvl PP等级
 */
void aicwf_hal_switchPP(uint8_t lvl)
{

}

/**
 * @brief SDIO复位WiFi芯片
 * @return 0=成功, 非0=失败
 * @note 本板无独立复位脚, 复位由 aicwf_sdio_reset_wifi() 的电源循环完成;
 *       SDIO 总线本身由 MMC 核心重新枚举, 此处直接返回成功。
 */
int aicwf_sdio_reset(void)
{
    return 0;
}

/**
 * @brief 通过SDIO下电WiFi（平台相关）
 * @note 拉低电源脚; 未配置上电脚时为空操作。属于显式断电操作, 先解除上电保护。
 */
void aicwf_sdio_pdn_wifi(void)
{
    aicwf_wifi_pwr_locked = RT_FALSE;
    aicwf_platform_pwr_wifi_pin_disable();
}

/**
 * @brief 硬件复位WiFi芯片（平台相关）
 * @note 无独立复位脚, 用电源循环等效复位 (时序同 aicwf_wifi_power_on)。
 *       属于显式复位操作, 先解除上电保护; 完成后 SDIO 需由 MMC 核心重新枚举。
 */
void aicwf_sdio_reset_wifi(void)
{
    if (aicwf_wifi_pwr_pin < 0)
    {
        return;
    }

    aicwf_wifi_pwr_locked = RT_FALSE;
    aicwf_platform_pwr_wifi_pin_disable();
    aicwf_rtos_msleep(AIC_WIFI_PWR_OFF_MS);
    aicwf_platform_pwr_wifi_pin_enable();
    aicwf_rtos_msleep(AIC_WIFI_PWR_ON_MS);
}

/**
 * @brief AT命令响应输出
 * @param handle 接口句柄
 * @param type   响应类型
 * @param level  日志级别
 * @param buf    响应内容字符串
 */
void aicwf_ATRESP(int handle, int type, int level, char *buf)
{
    (void)handle;
    (void)type;
    (void)level;
    /* [BUGFIX] (共性回流) 经 AIC 调试日志输出: 串口抓包只采集 [AIC][*] DBG 输出,
     * 原实现 RT_THREAD 下仅 rt_kprintf / 非 RT_THREAD 仅 DBG_CMN_INF(受模块
     * 开关门控), 扫描/连接结果 (+OK / [n]:rssi=...) 在 AIC 抓包日志中不可见。
     * 平台另有控制台输出需求时在此追加 (如 RT-Thread rt_kprintf) */
    if (buf)
        DBG_APP_INF("%s\n", buf);
#ifdef RT_THREAD
    /* [LOCAL] 保留 AIC 抓包日志的同时, 恢复 RT-Thread 控制台输出:
     * 扫描/连接结果需要出现在控制台上, 不能只依赖 DBG_APP_INF 模块开关。 */
    if (buf)
        rt_kprintf("%s\n", buf);
#endif
}
/**
 * @brief 十六进制dump数据缓冲区（调试用）
 *
 * 以每行16字节的格式打印数据内容，不足16字节的行自动处理。
 *
 * @param tag  标签字符串
 * @param data 数据缓冲区
 * @param len  数据长度
 */
void aicwf_dump_buffer(char *tag, uint8_t *data, uint32_t len)
{
    int i, diff = 0;
    DBG_CMN_VRB("dump:%s, len(%d)", tag, len);
    for (i = 0; i < len; i += 16)
    {
        if ((i + 16) <= len)
        {
            DBG_CMN_VRB("[%x] %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x" LINE, &data[i],
                        data[i],   data[i + 1], data[i + 2],  data[i + 3],  data[i + 4],  data[i + 5],  data[i + 6],  data[i + 7],
                        data[i + 8], data[i + 9], data[i + 10], data[i + 11], data[i + 12], data[i + 13], data[i + 14], data[i + 15]);
        }
        else
        {
            diff = len - i;
            if (diff == 15)
                DBG_CMN_VRB("[%x] %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x" LINE, &data[i],
                            data[i],   data[i + 1], data[i + 2],  data[i + 3],  data[i + 4],  data[i + 5],  data[i + 6],  data[i + 7],
                            data[i + 8], data[i + 9], data[i + 10], data[i + 11], data[i + 12], data[i + 13], data[i + 14]);
            if (diff == 14)
                DBG_CMN_VRB("[%x] %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x" LINE, &data[i],
                            data[i],   data[i + 1], data[i + 2],  data[i + 3],  data[i + 4],  data[i + 5],  data[i + 6],  data[i + 7],
                            data[i + 8], data[i + 9], data[i + 10], data[i + 11], data[i + 12], data[i + 13]);
            if (diff == 13)
                DBG_CMN_VRB("[%x] %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x" LINE, &data[i],
                            data[i],   data[i + 1], data[i + 2],  data[i + 3],  data[i + 4],  data[i + 5],  data[i + 6],  data[i + 7],
                            data[i + 8], data[i + 9], data[i + 10], data[i + 11], data[i + 12]);
            if (diff == 12)
                DBG_CMN_VRB("[%x] %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x" LINE, &data[i],
                            data[i],   data[i + 1], data[i + 2],  data[i + 3],  data[i + 4],  data[i + 5],  data[i + 6],  data[i + 7],
                            data[i + 8], data[i + 9], data[i + 10], data[i + 11]);
            if (diff == 11)
                DBG_CMN_VRB("[%x] %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x" LINE, &data[i],
                            data[i],   data[i + 1], data[i + 2],  data[i + 3],  data[i + 4],  data[i + 5],  data[i + 6],  data[i + 7],
                            data[i + 8], data[i + 9], data[i + 10]);
            if (diff == 10)
                DBG_CMN_VRB("[%x] %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x" LINE, &data[i],
                            data[i],   data[i + 1], data[i + 2],  data[i + 3],  data[i + 4],  data[i + 5],  data[i + 6],  data[i + 7],
                            data[i + 8], data[i + 9]);
            if (diff == 9)
                DBG_CMN_VRB("[%x] %02x %02x %02x %02x %02x %02x %02x %02x  %02x" LINE, &data[i],
                            data[i],   data[i + 1], data[i + 2],  data[i + 3],  data[i + 4],  data[i + 5],  data[i + 6],  data[i + 7],
                            data[i + 8]);
            if (diff == 8)
                DBG_CMN_VRB("[%x] %02x %02x %02x %02x %02x %02x %02x %02x" LINE, &data[i],
                            data[i],   data[i + 1], data[i + 2],  data[i + 3],  data[i + 4],  data[i + 5],  data[i + 6],  data[i + 7]);
            if (diff == 7)
                DBG_CMN_VRB("[%x] %02x %02x %02x %02x %02x %02x %02x" LINE, &data[i],
                            data[i],   data[i + 1], data[i + 2],  data[i + 3],
                            data[i + 4], data[i + 5], data[i + 6]);
            if (diff == 6)
                DBG_CMN_VRB("[%x] %02x %02x %02x %02x %02x %02x" LINE, &data[i],
                            data[i],   data[i + 1], data[i + 2],  data[i + 3],
                            data[i + 4], data[i + 5]);
            if (diff == 5)
                DBG_CMN_VRB("[%x] %02x %02x %02x %02x %02x" LINE, &data[i],
                            data[i],   data[i + 1], data[i + 2],  data[i + 3],
                            data[i + 4]);
            if (diff == 4)
                DBG_CMN_VRB("[%x] %02x %02x %02x %02x" LINE, &data[i],
                            data[i],   data[i + 1], data[i + 2],  data[i + 3]);
            if (diff == 3)
                DBG_CMN_VRB("[%x] %02x %02x %02x" LINE, &data[i],
                            data[i],   data[i + 1], data[i + 2]);
            if (diff == 2)
                DBG_CMN_VRB("[%x] %02x %02x" LINE, &data[i],
                            data[i],   data[i + 1]);
            if (diff == 1)
                DBG_CMN_VRB("[%x] %02x" LINE, &data[i],
                            data[i]);
        }
    }
}

/**
 * @brief GPIO触发控制（RT-Thread平台实现）
 * @param id    GPIO ID
 * @param value GPIO输出值
 * @note 根据具体硬件平台的GPIO引脚配置调整
 */
/* GPIO 触发引脚映射表: 触发 id (DRV_TRG_*, 逻辑概念, 定义于 plat_port.h)
 * → 板级 GPIO 引脚 (物理概念)。id 与 pin 相互独立, 默认平台两者取值恰好一致,
 * 故默认映射表直接引用 DRV_TRG_*。其他平台适配时仅需修改本表引脚值与
 * 下方实际输出调用。 */
#define AIC_TRG_GPIO_SDIO       DRV_TRG_SDIO
#define AIC_TRG_GPIO_SDIO_RX    DRV_TRG_SDIO_RX
#define AIC_TRG_GPIO_FHOST_RX   DRV_TRG_FHOST_RX
#define AIC_TRG_GPIO_FHOST_TX   DRV_TRG_FHOST_TX
#define AIC_TRG_GPIO_NET_TX     DRV_TRG_NET_TX
#define AIC_TRG_GPIO_POWCTR     DRV_TRG_POWCTR
#define AIC_TRG_GPIO_USB        DRV_TRG_USB

void aicwf_plat_gpio_trigger(uint8_t id, uint8_t value)
{
    uint8_t pin;

    /* id → pin 映射: 各触发点对应的板级引脚由平台映射表决定 */
    switch (id)
    {
    case DRV_TRG_SDIO:
        pin = AIC_TRG_GPIO_SDIO;
        break;
    case DRV_TRG_SDIO_RX:
        pin = AIC_TRG_GPIO_SDIO_RX;
        break;
    case DRV_TRG_FHOST_RX:
        pin = AIC_TRG_GPIO_FHOST_RX;
        break;
    case DRV_TRG_FHOST_TX:
        pin = AIC_TRG_GPIO_FHOST_TX;
        break;
    case DRV_TRG_NET_TX:
        pin = AIC_TRG_GPIO_NET_TX;
        break;
    case DRV_TRG_POWCTR:
        pin = AIC_TRG_GPIO_POWCTR;
        break;
    case DRV_TRG_USB:
        pin = AIC_TRG_GPIO_USB;
        break;
    default:
        return; /* 未知触发 id, 忽略 */
    }

    /* [PLATFORM] 实际 GPIO 输出调用, 各平台按自身 SDK API 实现, 例:
     * RT-Thread: rt_pin_mode(pin, PIN_MODE_OUTPUT);
     *            rt_pin_write(pin, value ? PIN_HIGH : PIN_LOW);
     * 其他:      gpio_direction_output(pin, value); */
    (void)pin;
    (void)value;
}

// ============================================================
// HAL + LOG extern wrappers (called directly by host layer)
// ============================================================
#include <stdarg.h>

/* Functions moved from wifi_port.c */
extern void wifi_driver_action(unsigned int act);

/**
 * @brief 触发驱动重启
 * @param mode 重启模式码，传递给 wifi_driver_action()
 */
static void aicwf_aic_driver_reboot(unsigned int mode)
{
    wifi_driver_action(mode);
}

/**
 * @brief 驱动异常处理
 *
 * 根据异常事件类型执行相应处理：
 * - 0x01/0x02/0x40/0x80/0x100: 触发驱动重启
 * - 0x04/0x20: 触发断言错误
 * - 其他: 无操作
 *
 * @param evt 异常事件码
 */
void aicwf_driver_unexcepted(unsigned int evt)
{
    switch (evt)
    {
    case 0x01:
        aicwf_aic_driver_reboot(0x03);
        break;
    case 0x02:
        aicwf_aic_driver_reboot(0x03);
        break;
    case 0x04:
        AIC_ASSERT_ERR(0);
        break;
    case 0x08:
        break;
    case 0x10:
        break;
    case 0x20:
        AIC_ASSERT_ERR(0);
        break;
    case 0x40:
        aicwf_aic_driver_reboot(0x03);
        break;
    case 0x80:
        aicwf_aic_driver_reboot(0x03);
        break;
    case 0x100:
        aicwf_aic_driver_reboot(0x03);
        break;
    case 0x200:
        break;

    default:
        break;
    }
}

/* LOG output — use rt_kprintf for RT-Thread platform */
/**
 * @brief 日志输出函数（RT-Thread平台实现）
 * @param fmt 格式化字符串
 * @param ... 可变参数
 * @return vsprintf的返回值（写入字符数）
 */
int aicwf_log_printf(const char *fmt, ...)
{
    va_list ap;
    char log_buf[256];
    int r;

    va_start(ap, fmt);
#ifdef RT_THREAD
    r = rt_vsnprintf(log_buf, sizeof(log_buf), fmt, ap);
    va_end(ap);

    if (r > 0)
    {
        rt_kprintf("%s", log_buf);
    }
#else
    r = vsnprintf(log_buf, sizeof(log_buf), fmt, ap);
    va_end(ap);
    // [PLATFORM] 非 RT-Thread 平台需根据实际环境实现输出
    // 默认通过标准输出打印
    if (r > 0)
    {
        fwrite(log_buf, 1, r, stdout);
    }
#endif
    return r;
}

/// Debug module environment definition. (moved here for host)
static struct debug_env_tag dbg_env =
{
    .filter_module = DBG_MOD_ALL,
    .filter_severity = DBG_SEV_IDX_INF,//DBG_SEV_IDX_VRB,//
};

/**
 * @brief 测试模块日志级别过滤
 *
 * 判断指定模块的指定严重级别是否应该输出日志。
 *
 * @param mod_idx 模块索引
 * @param sev_idx 严重级别索引
 * @return 1=应输出日志, 0=不输出
 */
int platform_test_module_severity(unsigned int mod_idx, unsigned int sev_idx)
{
    int ret = 0;
    if ((mod_idx < DBG_MOD_IDX_MAX) && (dbg_env.filter_module & (1 << mod_idx)) &&
            (dbg_env.filter_severity >= sev_idx))
    {
        ret = 1;
    }
    return ret;
}


/* ---- HAL implementations (extern linkage) ---- */
