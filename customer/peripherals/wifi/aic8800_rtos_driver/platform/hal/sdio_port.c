/*
 * Copyright (C) 2018-2024 AICSemi Ltd.
 *
 * All Rights Reserved
 */

//-------------------------------------------------------------------
// Driver Header Files
//-------------------------------------------------------------------
// ==========================================================================
// [AIC8800-PORT][SiFli] SDIO/MMC 子系统 → SiFli SDK 的 RT-Thread SDIO 框架
//   基线面向 TI mmcsd_core; SiFli SDK 用 RT-Thread SDIO (drivers/sdio.h)。
// ==========================================================================
#include <rtthread.h>
#include <drivers/sdio.h>        /* rt_sdio_function/rt_sdio_driver + sdio_io_*/sdio_register_driver/sdio_enable_func/sdio_attach_irq */
#include <drivers/mmcsd_core.h>  /* MMC/SD 核心 */
#include <drivers/mmcsd_card.h>  /* rt_mmcsd_card + SDIO_MAX_FUNCTIONS */
#include "sdio_api.h"            /* 驱动 host-API (lib/include/drv_api/inc, 已在 CPPPATH) */
#include "sys_api.h"             /* 驱动 host-API */

// ==========================================================================
// [AIC8800-PORT][SiFli] 类型兼容层: 基线 TI-mmcsd 命名 → RT-Thread SDIO 命名
//   结构体成员 (func: num/card/manufacturer/product/func_code/max_blk_size/
//   cur_blk_size; card: host/cis/cccr/sdio_function[]/sdio_function_num;
//   driver: name/probe/remove/id) 与函数名 (sdio_register_driver/sdio_enable_func/
//   sdio_io_readb/writeb/sdio_attach_irq/sdio_io_rw_extended_block) 完全一致,
//   仅顶层类型名不同, 故宏桥接避免逐处改名(47处)引入回归。
//   SDIOM_MAX_FUNCS 由 aic_sdio_types.h 自定义=3 (驱动内部数组上界), 无需映射。
// ==========================================================================
#define sdio_func       rt_sdio_function
#define mmcsd_card      rt_mmcsd_card
#define sdio_driver_t   rt_sdio_driver
#define sdio_device_id  rt_sdio_device_id
#define sdio_irq_h_t    rt_sdio_irq_handler_t   /* 替代 line~433 fallback typedef */

/* [AIC8800-PORT][SiFli] aic_sdio_* 透传包装器 (static inline) 定义在本文件后段
 * "Driver functions define" 区, 但 aicwf_sdio_drv_register / __aicwf_sdio_enable_function
 * 在前段调用; GCC14 对"隐式声明 + static 定义跟随非 static 声明"报错, 故前置声明。 */
static inline int aic_sdio_register_driver(struct sdio_driver_t *drv);
static inline int aic_sdio_enable_func(struct sdio_func *func);
// ==========================================================================
// [PLATFORM] 以下为 AIC 项目内部头文件，通常不需要修改
// ==========================================================================
#include "sdio_port.h"
#include "plat_port.h"
#include "rtos_port.h"
#include <errno.h>
#include "aic_log.h"
#include "co_math.h"
/* hal_co.h 依赖 host 内部头文件，仅声明所需函数 */
extern void aicwf_hal_func_release_set(int value);
extern uint8_t aicwf_hal_func_release_get(void);
extern void aicwf_hal_flag_tx_set(bool value);
#ifdef CONFIG_OOB
#endif /* CONFIG_OOB */

/* ==========================================================================
 * [PLATFORM] SDIO 总线抽象层 — 平台移植说明
 *
 * 本文件是 AIC8800 驱动与目标 RTOS/SDK 的 SDIO 总线桥接层。移植到新平台时，
 * 只需修改本文件顶部标记为 [PLATFORM] 的平台相关函数与数据结构，勿改动
 * 依赖这些封装之上的 host 驱动逻辑 (host/driver/layer_common/src/sdio_co.c)。
 *
 * ---- SDIO 标准扫卡 / 枚举流程 (由平台 MMC/SDIO 核心完成, 本文件消费结果) ----
 * 主机识别 SDIO 卡的标准序列如下 (由平台 MMC/SDIO 子系统的 probe 前完成):
 *   CMD0  GO_IDLE_STATE       复位卡到 idle
 *   CMD8  SEND_IF_COND        查询电压/接口条件, 区分 SD 与 SDIO
 *   CMD5  IO_SEND_OP_COND     SDIO 特有: 查询 IO 能力(电压/function 数量/ready),
 *                             等价于 SD 卡的 ACMD41
 *   CMD3  SEND_RELATIVE_ADDR  获取 RCA (相对卡地址)
 *   CMD7  SELECT_CARD         用 RCA 选中卡, 此后 function 0 的 CMD52/CMD53 可用
 *
 * ---- 扫卡后 function 映射 ----
 * SDIO 卡有 Function 0..7, 其中:
 *   Function 0 为必选公共控制功能, 承载 CCCR + FBR + CIS:
 *     CCCR (Card Common Control Registers): 公共控制区, 含 I/O 能力/中断使能(IEN/IENM)
 *     FBR  (Function Basic Registers): 每个 function 的接口代码/块大小/能力
 *     CIS  (Card Information Structure): 厂商/产品 ID (VID/PID)
 *   Function 1..7 为可选 I/O 功能 (AIC8800 通常 Function 1 为 WiFi, DC/DW 另有 Function 2)
 * 本文件在 __aicwf_aic_drv_init() 中按 func->num 把平台 function 列表映射到
 * sdio_function_local[]，后续 host 通过 FUNC_0/FUNC_1/FUNC_2 索引访问。
 *
 * ---- CMD52 / CMD53 ----
 *   CMD52 (IO_RW_DIRECT): 单字节寄存器读写, 用于读 CCCR/FBR、使能 I/O、设置中断。
 *     本文件封装为 aicwf_sdio_readb_cmd52* / aicwf_sdio_writeb_cmd52*。
 *   CMD53 (IO_RW_EXTENDED): 块/字节批量读写, 用于固件下载与数据收发。
 *     本文件封装为 aicwf_sdio_readsb / aicwf_sdio_writesb / aicwf_sdio_writesb_msg。
 *
 * ---- 中断配置 ----
 *   SDIO IRQ 由 CCCR 中的 IENM(Int Enable Master) + 各 function 的 IEN(Int Enable)
 *   共同控制。本文件在 probe 阶段始终声明宿主支持 SDIO IRQ (MMCSD_SUP_SDIO_IRQ)，
 *   运行时通过 aicwf_sdio_isr(en) 使能/关闭, 与 RX 是否轮询(aicwifi_sdio_rxdata_mode)
 *   解耦。
 *
 * ---- 平台相关函数 (移植时需适配) ----
 *   以下函数直接依赖平台 MMC/SDIO 子系统 API，移植时重点适配:
 *     __aicwf_aic_drv_init / __aicwf_aic_dev_remove (probe/remove 回调)
 *     aicwf_sdio_drv_register (驱动注册)
 *     aicwf_sdio_claim_host / aicwf_sdio_release_host (宿主互斥锁)
 *     __aicwf_sdio_enable_function (function 使能)
 *     aicwf_mmc_enable_isr (ISR 使能)
 *     aic_sdio_io_rw_extended_block / aic_sdio_io_readb / aic_sdio_io_writeb (CMD52/53 底层)
 *     aic_sdio_register_driver / aic_sdio_enable_func (驱动/function 注册)
 *     aicwf_sdio_claim_irq / aicwf_sdio_release_irq (IRQ 注册)
 *     aicwf_sdio_readsb / aicwf_sdio_writesb / aicwf_sdio_writesb_msg (CMD53 批量传输)
 *     __aicwf_sdio_get_func0 / __aicwf_sdio_f0_readb / aicwf_sdio_f0_writeb (Function 0 访问)
 *   其余 func_init / interrupt_init / set_block_size 等为基于上述封装的驱动逻辑, 一般不改。
 * ========================================================================== */

/* 运行时 chipmatch 全局变量 + getter — 在 CONFIG_SDIO_SUPPORT 之外,
 * 确保非 SDIO 模式 (如 USB) 也能链接到此符号 (返回 AICWF_CHIPID_UNMATCHED 表示未匹配).
 * wifi_if.c 的 aicwf_get_chipid() 通过此接口优先使用运行时匹配结果. */
uint16_t g_runtime_chipid = AICWF_CHIPID_UNMATCHED;
uint16_t g_runtime_vid = 0;
uint16_t g_runtime_pid = 0;

uint16_t aicwf_get_runtime_chipid(void)
{
    return g_runtime_chipid;
}

#ifdef CONFIG_SDIO_SUPPORT

uint32_t  SDIO_DEVICEID = 0xc18d;   // 8800DC/DW/DL  // wait for using.

//-------------------------------------------------------------------
// Driver Variables define
//-------------------------------------------------------------------
int msgcfm_poll_en = 1;
uint8_t sdio_unexcept_test = 0;
struct aic_sdio_dev sdio_dev = {0,};

/* Runtime-configurable SDIO behavior flags (see sdio_port.h) */
uint8_t config_rxtask_insdio = CONFIG_RXTASK_INSDIO_DEFAULT;
uint8_t config_irq_sus = CONFIG_IRQ_SUS_DEFAULT;
//sdio rxdata mode
unsigned char aicwifi_sdio_rxdata_mode = CONFIG_RX_POLLING_DEFAULT;

/* ==========================================================================
 * SDIO(DAT1) 中断风暴规避 —— 只保留"不放开 SDIO 中断"这一处开关
 *
 * 原因: SDHCI 的 SDHCI_INT_CARD_INT 是电平中断(DAT1 被卡拉低期间一直有效),
 *       而库内 aicwf_sdio_rx_task 每轮结束都无条件 sdio_host_enable_isr(TRUE),
 *       形成 "中断->唤醒RX任务->读 intstatus=0->再放开中断->立刻又中断" 死循环。
 *       该中断 NVIC 优先级高于串口, 串口 RX 永远得不到服务 => shell 收不到输入,
 *       表现成"整机卡死"(只打印一次 "msh />", 之后回车无回显)。
 *
 * 规避: g_sdio_irq_mask_forced = 1 时, 本文件所有 "使能 SDIO 中断" 的路径都改
 *       成保持屏蔽; 收包由库内 10ms 轮询兜底
 *       (需同时把 aicwifi_sdio_rxdata_mode 置 1, 见 example/aic8800_d80/src/main.c)。
 * ========================================================================== */
uint32_t g_sdio_irq_mask_forced = 0;            /* 1 = 不放开 SDIO 中断 */
// [PLATFORM] sdio_func / SDIOM_MAX_FUNCS 由平台 SDIO 头文件定义
// TODO: [AIC8800-PORT] 确认平台 struct sdio_func 成员命名一致
struct sdio_func *sdio_function_local[SDIOM_MAX_FUNCS];

/* Platform ADMA Variables Import */
static void (*host_sdio_irq_cb)(void);


//-------------------------------------------------------------------
// Driver Import functions
//-------------------------------------------------------------------
/* Platform functions */
#define DRV_NAME "AIC8800D"
#define DEVICE_FUNC_CODE    0x07

/* SDIO 设备 ID 表 — 按 AIC_CHIP_MODEL 选择对应芯片系列的 ID */
static struct sdio_device_id aic_sdio_ids[] =
{
#if defined(CONFIG_AIC8800D80) || defined(CONFIG_AIC8800D80N)
    /* D80 系列: D80 / D80N / D80LN / D80WN / D40N / D40LN / D40WN / D80X2
     * 首条 product 用 SDIO_ANY_PROD_ID: 平台的 sdio_match_card() 只比卡级(func 0)
     * 公共 CIS 中的 product, 且只取本表首条, 逐个型号列出只有第一条会生效;
     * 具体型号由 probe 内 aicwf_sdio_chipmatch() 按卡级 PID 判定
     * (D80=0x0082, D80N=0x9081, D80LN=0x9082, D80WN=0x9083, ...) */
    {DEVICE_FUNC_CODE, SDIO_VENDOR_ID_AIC8800D80,  SDIO_ANY_PROD_ID},
    {DEVICE_FUNC_CODE, SDIO_VENDOR_ID_AIC8800D80N, SDIO_DEVICE_ID_AIC8800D80N},
    {DEVICE_FUNC_CODE, SDIO_VENDOR_ID_AIC8800D80N, SDIO_DEVICE_ID_AIC8800D80LN},
    {DEVICE_FUNC_CODE, SDIO_VENDOR_ID_AIC8800D80N, SDIO_DEVICE_ID_AIC8800D80WN},
    {DEVICE_FUNC_CODE, SDIO_VENDOR_ID_AIC8800D80N, SDIO_DEVICE_ID_AIC8800D40N},
    {DEVICE_FUNC_CODE, SDIO_VENDOR_ID_AIC8800D80N, SDIO_DEVICE_ID_AIC8800D40LN},
    {DEVICE_FUNC_CODE, SDIO_VENDOR_ID_AIC8800D80N, SDIO_DEVICE_ID_AIC8800D40WN},
    //{DEVICE_FUNC_CODE, SDIO_VENDOR_ID_AIC8800D80X2, SDIO_DEVICE_ID_AIC8800D80X2},
#elif defined(CONFIG_AIC8800DC) || defined(CONFIG_AIC8800DW)
    {DEVICE_FUNC_CODE, SDIO_VENDOR_ID_AIC8800DC,  SDIO_DEVICE_ID_AIC8800DC},
#elif defined(CONFIG_AIC8800D)
    {DEVICE_FUNC_CODE, SDIO_VENDOR_ID_AIC8801,    SDIO_DEVICE_ID_AIC8801},
#endif
};

// [PLATFORM] sdio_driver_t 由平台 SDIO 子系统定义
// TODO: [AIC8800-PORT] 确认平台 struct sdio_driver_t 成员 (.probe/.remove/.name/.id) 命名一致
struct sdio_drv_priv
{
    struct sdio_driver_t wifi_drv;
    int drv_registered;
};
// [PLATFORM] mmcsd_card 由平台 MMC/SD 子系统定义
// TODO: [AIC8800-PORT] 替换为平台对应的 MMC/SD 卡抽象类型
static rt_int32_t __aicwf_aic_drv_init(struct mmcsd_card *card);
static rt_int32_t __aicwf_aic_dev_remove(struct mmcsd_card *card);

/* 运行时 chipmatch 前置声明 — 定义在文件末尾, 需在 __aicwf_aic_drv_init 之前可见.
 * 与源项目 (ESP-IDF) 通过 FBR 读取 VID/PID 不同, 当前平台 (TI mmcsd_core)
 * 已在 probe 时解析 CIS 并填入 card->cis.manufacturer/product, 直接使用即可. */
static int aicwf_sdio_chipmatch(uint16_t vid, uint16_t pid);

static struct sdio_drv_priv sdio_drvpriv =
{
    .wifi_drv.probe = __aicwf_aic_drv_init,
    .wifi_drv.remove = __aicwf_aic_dev_remove,
    .wifi_drv.name = (char *)DRV_NAME,
    .wifi_drv.id = aic_sdio_ids,
};

/**
 * @brief SDIO驱动probe回调：SDIO卡插入时初始化
 *
 * 记录各SDIO Function信息，并根据配置决定是否启用SDIO中断模式。
 *
 * @param card MMC/SDIO卡结构
 * @return 0=成功
 */
// [PLATFORM] mmcsd_card 及其成员 (cccr/cis/sdio_function/host/flags) 由平台 MMC 子系统定义
// TODO: [AIC8800-PORT] 替换为平台 MMC/SDIO 热插拔回调，适配平台 struct 成员命名
//   - card->cccr.sdio_version  → 平台 SDIO 版本号
//   - card->cis.manufacturer   → 平台 CIS 厂商 ID
//   - card->sdio_function[i]   → 平台 SDIO Function 数组
//   - card->host->flags |= MMCSD_SUP_SDIO_IRQ → 平台 SDIO 中断使能标志
static rt_int32_t __aicwf_aic_drv_init(struct mmcsd_card *card)
{
    uint8_t i, num, func_idx;

    DBG_SDIO_VRB("====>__aicwf_aic_drv_init():sdio_version:%d,manufacturer:0x%04x,product:0x%04x\r\n", card->cccr.sdio_version, card->cis.manufacturer, card->cis.product);

    // 初始化本地 function 列表为 NULL
    for (i = 0; i < SDIOM_MAX_FUNCS; i++)
        sdio_function_local[i] = NULL;

    /* 平台数组下标即功能号: [0]=FUNC0(公共控制区), [1..N]=各 I/O 功能,
     * 合法上界是 sdio_function_num(含), 共 sdio_function_num+1 项,
     * 故本地下标上界取 min(sdio_function_num, SDIOM_MAX_FUNCS-1) */
    num = co_min(card->sdio_function_num, (uint8_t)(SDIOM_MAX_FUNCS - 1));

    // 按 func->num 映射：索引必须与实际功能号对齐，不匹配的槽位保持 NULL
    for (i = 0; i <= num; i++)
    {
        struct sdio_func *f = card->sdio_function[i];
        func_idx = f->num;
        if (func_idx >= SDIOM_MAX_FUNCS)
        {
            DBG_SDIO_ERR("__aicwf_aic_drv_init(): func->num=%d out of range, skip\n", func_idx);
            continue;
        }
        sdio_function_local[func_idx] = f;
        DBG_SDIO_INF("[%d]:%x %x\n", f->num, f->manufacturer, f->product);
        DBG_SDIO_INF("__aicwf_aic_drv_init():func:%d,maxblksize:%d,curblksize:%d\r\n", f->num, f->max_blk_size, f->cur_blk_size);
    }

    /* SDIO IRQ 是标准能力, 始终向平台 MMC/SDIO 核心声明支持;
     * 运行时的使能/关闭由 aicwf_sdio_isr() 控制, 与"是否轮询"解耦。 */
    card->host->flags |= MMCSD_SUP_SDIO_IRQ;

    /* 运行时 chipmatch — 平台已在 probe 时解析 CIS, 直接取 card->cis.manufacturer/product.
     * 源项目 (ESP-IDF) 用 aicwf_sdio_read_fbr_vid_pid() 读原始 CIS; 当前平台 (TI mmcsd_core)
     * 已解析好, 无需重复读 FBR. 匹配结果存 g_runtime_chipid 供 wifi_if.c 优先使用. */
    {
        uint16_t vid = (uint16_t)card->cis.manufacturer;
        uint16_t pid = (uint16_t)card->cis.product;
        int chipid = aicwf_sdio_chipmatch(vid, pid);
        g_runtime_vid = vid;
        g_runtime_pid = pid;
        /* chipmatch 匹配失败返回 -1; AIC8801 合法匹配值为 0, 不可用 0 判未匹配 */
        g_runtime_chipid = (chipid < 0) ? AICWF_CHIPID_UNMATCHED : (uint16_t)chipid;
        DBG_SDIO_INF("[chipmatch] VID=0x%04X, PID=0x%04X -> chipid=%d\n", vid, pid, g_runtime_chipid);
    }

    return 0;
}

/**
 * @brief SDIO驱动remove回调：SDIO卡拔出时清理
 * @param card MMC/SDIO卡结构
 * @return 0=成功
 */
static rt_int32_t __aicwf_aic_dev_remove(struct mmcsd_card *card)
{
    DBG_SDIO_INF("====>__aicwf_aic_dev_remove()\n");
    return 0;
}

/**
 * @brief 注册AIC SDIO驱动到SDIO子系统
 * @return 注册结果, 0=成功
 */
int aicwf_sdio_drv_register(void)
{
    int err;
    err = aic_sdio_register_driver(&sdio_drvpriv.wifi_drv);

    DBG_SDIO_INF("====>aicwf_sdio_drv_register():ret:%d\n", err);

    return err;
}

void aicwf_sdio_claim_host(uint32_t func_num);
void aicwf_sdio_release_host(uint32_t func_num);

/**
 * @brief 使能指定SDIO Function（内部实现）
 * @param index SDIO Function索引
 * @return 0=成功, -1=失败
 */
static int __aicwf_sdio_enable_function(uint8_t index)
{
    int ret = 0;

    aicwf_sdio_claim_host(index);
    ret = aic_sdio_enable_func(sdio_function_local[index]);
    aicwf_sdio_release_host(index);
    if (ret != 0)
    {
        DBG_SDIO_ERR("aic_sdio_enable_func fail:%d!!\n", ret);
        return -1;
    }

    return 0;
}

/**
 * @brief 使能/禁用MMC SDIO中断
 * @param enable 1=使能, 0=禁用
 */
// [PLATFORM] func->card->host->ops->enable_sdio_irq 是平台 MMC 层深层访问
// TODO: [AIC8800-PORT] 替换为平台提供的 SDIO 中断使能/禁用 API
static void aicwf_mmc_enable_isr(int32_t enable)
{
    struct sdio_func *func = NULL;
    uint8_t i;

    // 找到第一个有效的 function 用于访问 card->host
    for (i = 0; i < SDIOM_MAX_FUNCS; i++)
    {
        if (sdio_function_local[i] != NULL)
        {
            func = sdio_function_local[i];
            break;
        }
    }

    if (!func)
        return;

    func->card->host->ops->enable_sdio_irq(func->card->host, enable);
}


//-------------------------------------------------------------------
// Driver Macros Functions
//-------------------------------------------------------------------
#define SDIO_PRE_OP() {\
    if(!(wifi_chip_status_get()))\
        return -1;\
}

#define SDIO_PRE_OP_B() {\
    if(!(wifi_chip_status_get()))\
        return false;\
}

#define SDIO_PRE_OPV(v) {\
    if(!(wifi_chip_status_get()))\
    {\
        *v=-1;\
        return;\
    }\
}

#define SDIO_PRE_OPVR(v) {\
    if(!(wifi_chip_status_get()))\
    {\
        *v=-1;\
        return 1;\
    }\
}

#define SDIO_WR_FAIL(ret){\
    if(ret) {\
        sdio_dev.wr_fail++;\
    }\
    else\
        sdio_dev.wr_fail = 0;\
    if(sdio_dev.wr_fail > 15)\
    {\
        aicwf_driver_unexcepted(0x04);\
    }\
}


#define SDIO_ANY_ID (~0)


//-------------------------------------------------------------------
// Driver functions define
//-------------------------------------------------------------------
/* ==========================================================================
 * Platform SDIO I/O Wrappers — 由平台方实现
 *
 * 以下 aic_ 前缀函数封装了平台相关的 SDIO 底层操作。
 * 移植到新的 RTOS/SDK 平台时，将函数体替换为平台对应的 SDIO API 调用。
 *
 * 当前实现直接透传至平台 SDK 的 sdio_* API，仅作为参考示例。
 * ========================================================================== */

static inline int aic_sdio_io_rw_extended_block(struct sdio_func *func,
        int write, unsigned int addr, int incr_addr, uint8_t *buf, int blk_cnt)
{
    // TODO: [AIC8800-PORT] 替换为平台 SDIO CMD53 块读写 API
    return sdio_io_rw_extended_block(func, write, addr, incr_addr, buf, blk_cnt);
}

static inline uint8_t aic_sdio_io_readb(struct sdio_func *func,
                                        unsigned int addr, int *err_ret)
{
    // TODO: [AIC8800-PORT] 替换为平台 SDIO CMD52 字节读 API
    /* [AIC8800-PORT][SiFli] SDK sdio_io_readb 的 err 形参为 rt_int32_t*(=long*),
     * err_ret 为 int*; ARM32 下同宽(4B), 显式 cast 消除 GCC14 不兼容指针 error */
    return sdio_io_readb(func, addr, (rt_int32_t *)err_ret);
}

static inline int aic_sdio_io_writeb(struct sdio_func *func,
                                     unsigned int addr, uint8_t val)
{
    // TODO: [AIC8800-PORT] 替换为平台 SDIO CMD52 字节写 API
    return sdio_io_writeb(func, addr, val);
}

static inline int aic_sdio_register_driver(struct sdio_driver_t *drv)
{
    // TODO: [AIC8800-PORT] 替换为平台 SDIO 驱动注册 API
    return sdio_register_driver(drv);
}

static inline int aic_sdio_enable_func(struct sdio_func *func)
{
    // TODO: [AIC8800-PORT] 替换为平台 SDIO Function 使能 API
    return sdio_enable_func(func);
}

static inline int aic_sdio_attach_irq(struct sdio_func *func,
                                      sdio_irq_h_t handler)
{
    // TODO: [AIC8800-PORT] 替换为平台 SDIO 中断注册 API
    return sdio_attach_irq(func, handler);
}

static inline int aic_sdio_detach_irq(struct sdio_func *func)
{
    // TODO: [AIC8800-PORT] 替换为平台 SDIO 中断释放 API
    return sdio_detach_irq(func);
}

/* AIC driver sdio base functions */
// [PLATFORM] sdio_irq_h_t 类型由平台 SDIO 子系统定义
// TODO: [AIC8800-PORT] 若平台头文件已提供该 typedef，可删除此行
// 当前作为 fallback 定义，依赖平台 struct sdio_function
/* [AIC8800-PORT][SiFli] 停用 fallback typedef: sdio_irq_h_t 已由顶部兼容宏
 * = rt_sdio_irq_handler_t (SDK mmcsd_card.h 提供), 再 typedef 会与 SDK 冲突 */
// typedef void (sdio_irq_h_t)(struct sdio_function *);
/**
 * @brief SDIO主机上锁（总线互斥）
 * @param func_num SDIO功能号（当前未使用）
 */
void aicwf_sdio_claim_host(uint32_t func_num)
{
    (void)func_num;
    //aic sdio wr use onwer thread lock .
    //mmcsd_host_lock(func->card->host);
}

/**
 * @brief SDIO主机解锁（总线互斥）
 * @param func_num SDIO功能号（当前未使用）
 */
void aicwf_sdio_release_host(uint32_t func_num)
{
    (void)func_num;
    //mmcsd_host_unlock(func->card->host);
}

/**
 * @brief 注册SDIO中断处理函数
 * @param func    SDIO Function
 * @param handler 中断处理函数
 * @return aic_sdio_attach_irq()的返回值
 */
static int32_t aicwf_sdio_claim_irq(struct sdio_func *func,
                                    sdio_irq_h_t handler)
{
    return aic_sdio_attach_irq(func, handler);
}

/**
 * @brief 释放SDIO中断
 * @param func SDIO Function
 * @return aic_sdio_detach_irq()的返回值
 */
static int32_t aicwf_sdio_release_irq(struct sdio_func *func)
{
    return aic_sdio_detach_irq(func);
}

/**
 * @brief SDIO批量读数据（CMD53扩展块读）
 * @param func_num SDIO功能号
 * @param dst      目标缓冲区
 * @param addr     寄存器地址
 * @param count    读取字节数
 * @return 0=成功, 非0=失败
 */
int aicwf_sdio_readsb(uint32_t func_num, void *dst, unsigned int addr, int count)
{
    int ret;
    struct sdio_func *func = sdio_function_local[func_num];

    SDIO_PRE_OP();

#ifdef CONFIG_SDIO_ADMA
    /* [PLATFORM] ADMA 读分支需平台实现: 单缓冲 DMA 读 + 读后 CacheInvalidate
     * (DMA 写入的 cache line 必须先失效再被 CPU 读取)。见 port/inc/sdio_port.h
     * 的 ADMA 契约与 aidoc/adma平台移植参考.md。
     * 本参考平台未实现 —— 显式拦截, 避免把 ADMA 语义下的 count 当作字节数
     * 走普通块读, 造成静默数据错误。 */
#error "CONFIG_SDIO_ADMA: implement aicwf_sdio_readsb() ADMA branch for this platform (see port/inc/sdio_port.h ADMA contract)"
#endif

    aicwf_rtos_mutex_lock(sdio_dev.bus_txrx, -1);
    ret = aic_sdio_io_rw_extended_block(func, 0, addr, 0, (uint8_t *)dst, count);
    aicwf_rtos_mutex_unlock(sdio_dev.bus_txrx);

    if (ret)
    {
        DBG_SDIO_INF("%s: %d\n", __func__, ret);
    }
    SDIO_WR_FAIL(ret);
    return ret;
}

/**
 * @brief SDIO批量写数据（CMD53扩展块写）
 * @param func_num SDIO功能号
 * @param addr     寄存器地址
 * @param src      源数据缓冲区
 * @param count    写入字节数
 * @return 0=成功, 非0=失败
 */
int aicwf_sdio_writesb(uint32_t func_num, unsigned int addr, void *src, int count)
{
    int ret;
    struct sdio_func *func = sdio_function_local[func_num];
    SDIO_PRE_OP();

#ifdef CONFIG_SDIO_ADMA
    /* [PLATFORM] ADMA 写分支需平台实现: src = const sdio_sg_list *,
     * count = 段数 → 交给平台 DMA/描述符机制 (如 ASR/064:
     * NU_SDIODrv_Write_Memory_sg(addr, sg_list, segcnt)), 每段 CacheClean +
     * 同步屏障, 并以 PROTECT_TX_ADMA 等价手段保证发送完成前缓冲不被复用。
     * 见 port/inc/sdio_port.h 的 ADMA 契约与 aidoc/adma平台移植参考.md。 */
#error "CONFIG_SDIO_ADMA: implement aicwf_sdio_writesb() ADMA branch for this platform (see port/inc/sdio_port.h ADMA contract)"
#endif

    aicwf_rtos_mutex_lock(sdio_dev.bus_txrx, -1);
    ret = aic_sdio_io_rw_extended_block(func, 1, addr, 0, (uint8_t *)src, count);
    aicwf_rtos_mutex_unlock(sdio_dev.bus_txrx);

    if (ret)
    {
        DBG_SDIO_INF("%s: %d\n", __func__, ret);
    }
    SDIO_WR_FAIL(ret);
    return ret;
}

/**
 * @brief SDIO批量写消息（CMD53扩展块写，消息模式）
 *
 * 与 aicwf_sdio_writesb() 功能相同，用于消息帧传输。
 *
 * @param func_num SDIO功能号
 * @param addr     寄存器地址
 * @param src      源数据缓冲区
 * @param count    写入字节数
 * @return 0=成功, 非0=失败
 */
int aicwf_sdio_writesb_msg(uint32_t func_num, unsigned int addr, void *src, int count)
{
    int ret;
    struct sdio_func *func = sdio_function_local[func_num];
    SDIO_PRE_OP();

    aicwf_rtos_mutex_lock(sdio_dev.bus_txrx, -1);
    ret = aic_sdio_io_rw_extended_block(func, 1, addr, 0, (uint8_t *)src, count);
    aicwf_rtos_mutex_unlock(sdio_dev.bus_txrx);

    if (ret)
    {
        DBG_SDIO_INF("%s: %d\n", __func__, ret);
    }
    SDIO_WR_FAIL(ret);
    return ret;
}

/**
 * @brief 获取可用于 FUNC0 操作的 SDIO func 句柄
 *
 * 大多数平台 SDIO function 列表不含 FUNC0，因此 sdio_function_local[FUNC_0]
 * 通常为 NULL。但 FUNC0 (CCCR) 的 CMD52 读写只需要访问 card->host 总线，
 * 任意一个有效 func 的 ->card->host 均可用于 FUNC0 地址空间的读写。
 *
 * @return 首个可用的 struct sdio_func *, NULL=无可用 func
 */
static struct sdio_func *__aicwf_sdio_get_func0(void)
{
    uint8_t i;

    // 优先 FUNC0
    if (sdio_function_local[FUNC_0] != NULL)
        return sdio_function_local[FUNC_0];

    // fallback: 用任意有效 func 访问 card->host
    for (i = 1; i < SDIOM_MAX_FUNCS; i++)
    {
        if (sdio_function_local[i] != NULL)
            return sdio_function_local[i];
    }

    return NULL;
}

/**
 * @brief SDIO Function 0 单字节读
 * @param addr    寄存器地址
 * @param err_ret 输出的错误码（可为NULL）
 * @return 读取的字节值
 */
static uint8_t __aicwf_sdio_f0_readb(unsigned int addr, int *err_ret)
{
    int ret;
    uint8_t b = 0;
    // FUNC0 地址空间的 CMD52 可由任意 func 的 card->host 完成
    struct sdio_func *func = __aicwf_sdio_get_func0();

    if (!func)
    {
        if (err_ret)
            *err_ret = -1;
        return 0;
    }

    SDIO_PRE_OPVR(err_ret);

    aicwf_rtos_mutex_lock(sdio_dev.bus_txrx, -1);
    b = aic_sdio_io_readb(func, addr, &ret);
    aicwf_rtos_mutex_unlock(sdio_dev.bus_txrx);

    if (err_ret)
        *err_ret = ret;

    if (ret)
    {
        DBG_SDIO_INF("%s: %d\n", __func__, ret);
    }
    SDIO_WR_FAIL(ret);
    return b;
}

/**
 * @brief SDIO Function 0 单字节写
 * @param b       写入的字节值
 * @param addr    寄存器地址
 * @param err_ret 输出的错误码（可为NULL）
 */
static void aicwf_sdio_f0_writeb(unsigned char b, unsigned int addr,
                                 int *err_ret)
{
    int ret;
    // FUNC0 地址空间的 CMD52 可由任意 func 的 card->host 完成
    struct sdio_func *func = __aicwf_sdio_get_func0();

    if (!func)
    {
        if (err_ret)
            *err_ret = -1;
        return;
    }

    SDIO_PRE_OPV(err_ret);

    aicwf_rtos_mutex_lock(sdio_dev.bus_txrx, -1);
    ret = aic_sdio_io_writeb(func, addr, b);
    aicwf_rtos_mutex_unlock(sdio_dev.bus_txrx);

    if (err_ret)
        *err_ret = ret;

    if (ret)
    {
        DBG_SDIO_INF("%s: %d\n", __func__, ret);
    }
    SDIO_WR_FAIL(ret);
}

/**
 * @brief 根据厂商ID和设备ID查找SDIO Function
 * @param class  SDIO功能类码
 * @param vendor 厂商ID（SDIO_ANY_ID表示匹配任意）
 * @param device 设备ID（SDIO_ANY_ID表示匹配任意）
 * @return 匹配的SDIO Function指针, NULL=未找到
 */
// [PLATFORM] func->func_code/manufacturer/product/num 由平台 struct sdio_func 定义
// TODO: [AIC8800-PORT] 确认平台 struct sdio_func 成员命名，必要时适配
static struct sdio_func *__aicwf_sdio_find_func(uint8_t class, uint16_t vendor, uint16_t device)
{
    struct sdio_func *func;
    uint8_t j;


    for (j = 0; j < SDIOM_MAX_FUNCS; j++)
    {
        func = sdio_function_local[j];
        if (func == NULL)
            continue;
        DBG_SDIO_VRB("sdio_func[%d]: class:0x%x, vendor:0x%x, device:0x%x\r\n",
                     j, func->func_code, func->manufacturer, func->product);
        //if (class != (uint8_t)SDIO_ANY_ID && class != func->class)
        //    continue;
        if ((vendor != (uint16_t)SDIO_ANY_ID) && (vendor != func->manufacturer))
            continue;
        if ((device != (uint16_t)SDIO_ANY_ID) && (device != func->product))
            continue;

        DBG_SDIO_INF("sdio find func num:%d\r\n", func->num);
        return func;
    }

    return NULL;
}

/**
 * @brief SDIO CMD52单字节读（通用，按功能号）
 *
 * 统一封装 Function 1/2 的 CMD52 读操作，func_num 不为 0。
 *
 * @param func_num SDIO功能号（FUNC_1 或 FUNC_2）
 * @param addr     寄存器地址
 * @param data     输出的数据字节
 * @return true=成功, false=失败
 */
bool aicwf_sdio_readb_cmd52_by_num(uint32_t func_num, uint32_t addr, uint8_t *data)
{
    int err;
    uint8_t b;
    struct sdio_func *func = sdio_function_local[func_num];
    SDIO_PRE_OP_B();

    aicwf_rtos_mutex_lock(sdio_dev.bus_txrx, -1);
    b = aic_sdio_io_readb(func, addr, &err);
    aicwf_rtos_mutex_unlock(sdio_dev.bus_txrx);

    SDIO_WR_FAIL(err);
    if (err)
    {
        DBG_SDIO_ERR("__aicwf_sdio_readb_cmd52 fail %d!\n", err);
        return false;
    }
    *data = b;
    return true;
}

/**
 * @brief SDIO CMD52单字节写（通用，按功能号）
 *
 * 统一封装 Function 1/2 的 CMD52 写操作，func_num 不为 0。
 *
 * @param func_num SDIO功能号（FUNC_1 或 FUNC_2）
 * @param addr     寄存器地址
 * @param data     写入的数据字节
 * @return true=成功, false=失败
 */
bool aicwf_sdio_writeb_cmd52_by_num(uint32_t func_num, uint32_t addr, uint8_t data)
{
    int err;
    struct sdio_func *func = sdio_function_local[func_num];
    SDIO_PRE_OP_B();

    aicwf_rtos_mutex_lock(sdio_dev.bus_txrx, -1);
    err = aic_sdio_io_writeb(func, addr, data);
    aicwf_rtos_mutex_unlock(sdio_dev.bus_txrx);

    SDIO_WR_FAIL(err);
    if (err)
    {
        DBG_SDIO_ERR("__aicwf_sdio_writeb_cmd52 fail %d!\n", err);
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * D80N(0x9082) flow-ctrl 读重定向的阶段开关   [AIC8800-PORT][SiFli]
 *
 * 背景(证据见 example/aic8800_d80/docs/aic8800d80n_flowctrl_q1q2_vendor_report.md):
 *   库把 sdio_reg.flow_ctrl_reg 硬编码为 Q1(0x03), 且数据通路与 msg(cfg) 通路
 *   共用这一个字段; 而本芯片 ROM/CINIT/PATCH/CALIB 阶段只有 Q2(0x09) 有 TX 信用
 *   (Q1 恒 0, Q2 恒 2; 且必须先哑读一次 Q1, Q2 才有效), 故在 func1 CMD52 读入口做
 *   0x03 -> 0x09 的读侧重定向。该规避让初始化 100% 通过。
 *
 * 遗留问题: 重定向此前无条件且永久生效, 于是补丁 FW 跑起来之后:
 *   · msg  通路判据 fc_reg != 0        -> Q2=2 通过   -> 扫描/认证/关联 OK
 *   · 数据 通路判据 fc_reg >  THRESH(2) -> Q2=2 不通过 -> data fc:-2,
 *     EAPOL(msg2/4) 永远发不出芯片 -> AP 每秒重发 msg1/4, 4 次后 deauth
 *     reason=15(4WAY_HANDSHAKE_TIMEOUT) —— 即握手失败、连接超时。
 *
 * 本开关把重定向限定在真正需要的阶段, 以便实测"FW 运行后 Q1 是多少":
 *   AIC_FC_Q1_AFTER_FWBOOT = 1: FW 就绪后停止重定向, 直读真实 Q1(0x03)
 *                               Q1 > 2  -> 数据通路立刻可用(握手可完成)
 *                               Q1 == 0 -> msg 通路退化为 msg fc:0(同修复前现象)
 *   AIC_FC_Q1_AFTER_FWBOOT = 0: 保持原行为(永远读 Q2), 仅作 A/B 对照
 *
 *   AIC_FC_RAW_DBG = 1: 每 AIC_FC_RAW_DBG_PERIOD 次 flow-ctrl 读, 附带打印一次
 *                       raw Q1/Q2(默认关闭: 报告 §3(4) 记录反复访问 Q1 会触发票
 *                       芯片侧 "sdio func1 error bit:800, func1 free cnt=0, ...")
 * ------------------------------------------------------------------------- */
#define AIC_FC_Q1_REDIRECT_EN    1
#define AIC_FC_Q1_AFTER_FWBOOT   1
#define AIC_FC_RAW_DBG           0
#define AIC_FC_RAW_DBG_PERIOD    2000

/* 1 = ROM/CINIT/PATCH/CALIB 阶段: 读 Q1 即重定向到 Q2;
 * 由 aicwf_sdio_enable_irq_after_fw_boot() 在补丁 FW 启动成功后清零 */
static volatile int g_fc_redirect_q2 = AIC_FC_Q1_REDIRECT_EN;

/**
 * @brief SDIO CMD52单字节读（Function 1）
 * @param addr 寄存器地址
 * @param data 输出的数据字节
 * @return true=成功, false=失败
 */
bool aicwf_sdio_readb_cmd52(uint32_t addr, uint8_t *data)
{
#if AIC_FC_Q1_REDIRECT_EN
    if (addr == SDIOWIFI_FLOW_CTRL_Q1_REG_V3)
    {
        /* "前 5 次先哑读 Q1/Q2"
         * 与两次 rt_kprintf 造成的延时都被观察到是承重的：
         *   · 去掉本段(直接读 Q2)                          -> 拿不到信用，启动失败
         *   · 每次都哑读 Q1 再读 Q2(无延时)                -> 芯片反复报
         *        "sdio func1 error bit:800, func1 free cnt=0, func1 free queue2 cnt=2"
         *        并需要重试很多次才成功（反复访问 Q1 会招惹芯片）
         * [AIC8800-PORT][SiFli] 本段是 ROM/CINIT 阶段 Q2 给出有效信用的必要条件,
         * 与阶段无关地原样保留, 不做任何时序改动 */
        static uint32_t fc_dbg_cnt = 0;
        uint8_t v = 0;
        bool ok;

        fc_dbg_cnt++;

        if (fc_dbg_cnt <= 5)
        {
            uint8_t v03 = 0, v09 = 0;
            bool ok03 = aicwf_sdio_readb_cmd52_by_num(FUNC_1, SDIOWIFI_FLOW_CTRL_Q1_REG_V3, &v03);
            bool ok09 = aicwf_sdio_readb_cmd52_by_num(FUNC_1, SDIOWIFI_FLOW_CTRL_Q2_REG_V3, &v09);
            rt_kprintf("[AICFC] #%u raw 0x03=%u(ok%d) 0x09=%u(ok%d) chipid=%u field=0x%02x redir=%d\n",
                       fc_dbg_cnt, v03, ok03, v09, ok09,
                       g_runtime_chipid, sdio_dev.sdio_reg.flow_ctrl_reg, g_fc_redirect_q2);
        }
#if AIC_FC_RAW_DBG
        else if ((AIC_FC_RAW_DBG_PERIOD != 0) && ((fc_dbg_cnt % AIC_FC_RAW_DBG_PERIOD) == 0))
        {
            /* 周期探针: 观察 FW 运行后 Q1/Q2 是否变化 (默认关闭, 原因见开关处说明) */
            uint8_t v03 = 0, v09 = 0;
            bool ok03 = aicwf_sdio_readb_cmd52_by_num(FUNC_1, SDIOWIFI_FLOW_CTRL_Q1_REG_V3, &v03);
            bool ok09 = aicwf_sdio_readb_cmd52_by_num(FUNC_1, SDIOWIFI_FLOW_CTRL_Q2_REG_V3, &v09);
            rt_kprintf("[AICFC] #%u raw 0x03=%u(ok%d) 0x09=%u(ok%d) redir=%d\n",
                       fc_dbg_cnt, v03, ok03, v09, ok09, g_fc_redirect_q2);
        }
#endif

        /* [AIC8800-PORT][SiFli] 仅在"需要重定向的阶段"且为 D80N 时才翻译到 Q2;
         * g_fc_redirect_q2 被 aicwf_sdio_enable_irq_after_fw_boot() 清零后,
         * 本函数返回的是真实 Q1(0x03) —— 数据通路的判据是 fc_reg > 2 */
        if (g_fc_redirect_q2 && (g_runtime_chipid == PRODUCT_ID_AIC8800D80N))
        {
            ok = aicwf_sdio_readb_cmd52_by_num(FUNC_1, SDIOWIFI_FLOW_CTRL_Q2_REG_V3, &v);
            if ((fc_dbg_cnt <= 5) || ((fc_dbg_cnt % 200) == 0))
                rt_kprintf("[AICFC] #%u -> redirect 0x09 val=%u ok=%d\n", fc_dbg_cnt, v, ok);
            *data = v;
            return ok;
        }

        if ((fc_dbg_cnt <= 5) && (g_runtime_chipid != PRODUCT_ID_AIC8800D80N))
            rt_kprintf("[AICFC] #%u NOT redirect: chipid=%u (expect %u)\n",
                       fc_dbg_cnt, g_runtime_chipid, PRODUCT_ID_AIC8800D80N);
    }
#endif /* AIC_FC_Q1_REDIRECT_EN */

    return aicwf_sdio_readb_cmd52_by_num(FUNC_1, addr, data);
}

/**
 * @brief SDIO CMD52单字节读（Function 2）
 * @param addr 寄存器地址
 * @param data 输出的数据字节
 * @return true=成功, false=失败
 */
bool aicwf_sdio_readb_cmd52_func2(uint32_t addr, uint8_t *data)
{
    return aicwf_sdio_readb_cmd52_by_num(FUNC_2, addr, data);
}

/**
 * @brief SDIO CMD52单字节写（Function 1）
 * @param addr 寄存器地址
 * @param data 写入的数据字节
 * @return true=成功, false=失败
 */
bool aicwf_sdio_writeb_cmd52(uint32_t addr, uint8_t data)
{
    return aicwf_sdio_writeb_cmd52_by_num(FUNC_1, addr, data);
}

/**
 * @brief SDIO CMD52单字节写（Function 2）
 * @param addr 寄存器地址
 * @param data 写入的数据字节
 * @return true=成功, false=失败
 */
bool aicwf_sdio_writeb_cmd52_func2(uint32_t addr, uint8_t data)
{
    return aicwf_sdio_writeb_cmd52_by_num(FUNC_2, addr, data);
}

#ifdef CONFIG_OOB
// This interface is from ASR1803-SDK.
void aicwf_sdio_set_irq_handler(void (*cb)(void));
static void aicwf__sdio_irq_enable(UINT32 base_addr, UINT16 mask)
{

}

static void aicwf_sdio_oobirq_task(void *argv)
{
    int ret = 0;
    uint32_t wait_time = 1000;
    while (1)
    {
        ret = aicwf_rtos_semaphore_wait(sdio_dev.sdio_oobirq_sema, wait_time);
        if (ret)
            DBG_SDIO_WRN("sdio_oobirq_sema timeout: %d\n", ret);
        aic_sdio_rx_task(0);
    }
}

static void aicwf_sdio_oob_irq_hdl(void)
{
    int count = 0;
    /* [AIC8800-PORT][BUGFIX] 硬中断上下文, 用 fromisr 版本读计数
     * (get_count 内部进临界区, ISR 中禁用) */
    count = aicwf_rtos_semaphore_get_count_fromisr(sdio_dev.sdio_oobirq_sema);
    if (count == 0)
        aicwf_rtos_semaphore_signal(sdio_dev.sdio_oobirq_sema, true);
    else
        DBG_SDIO_INF("sdio oob isr: %d\n", count);
}

void aicwf_sdio_oob_enable(void)
{
    int ret = 0;

    //configure oob gpio
    //to add

    aicwf_sdio_set_irq_handler(NULL);

    //disable sdio interrupt
    DBG_SDIO_INF("%s: SDIOWIFI_INTR_CONFIG_REG Disable\n", __func__);
    ret = aicwf_sdio_writeb_cmd52_func2(sdiodev->sdio_reg.intr_config_reg, 0x0);
    if (ret == false)
    {
        DBG_SDIO_ERR("ERR: reg:%d write failed!\n", sdiodev->sdio_reg.intr_config_reg);
    }

    ret = aicwf_rtos_task_create(aicwf_sdio_oobirq_task, "oobirq_task", SDIO_OOBIRQ_TASK,
                                 sdio_oobirq_stack_size, NULL, sdio_oobirq_priority, &sdiodev->sdio_oobirq_hdl);
    if (ret)
    {
        DBG_SDIO_ERR("ERR: oobirq_task create failed\n");
    }
    else
    {
        sdiodev->oob_enable = true;
    }
}
#endif /* CONFIG_OOB */

#ifndef CONFIG_OOB
void aicwf_sdio_oob_enable(void) { }
#endif

/**
 * @brief SDIO中断底层回调（来自MMC/SDIO层）
 *
 * 将中断转发到已注册的 host_sdio_irq_cb 处理函数。
 *
 * @param func 触发中断的SDIO Function
 */
// [PLATFORM] struct sdio_function 由平台 SDIO 子系统定义
// TODO: [AIC8800-PORT] 替换为平台 SDIO 中断回调签名
static void aicwf_host_sdio_interrupt(struct sdio_func *func)
{
    if (host_sdio_irq_cb)
        host_sdio_irq_cb();
}

/**
 * @brief 设置SDIO中断处理回调
 * @param cb 中断回调函数指针
 */
void aicwf_sdio_set_irq_handler(void (*cb)(void))
{
    host_sdio_irq_cb = cb;
}

/**
 * @brief 使能/禁用SDIO中断
 * @param en 1=使能, 0=禁用
 */
void aicwf_sdio_isr(uint8_t en)
{
    /* SDIO(DAT1) 中断风暴规避: 见本文件 g_sdio_irq_mask_forced 说明 */
    if (en && g_sdio_irq_mask_forced)
    {
        aicwf_mmc_enable_isr(0);
        return;
    }
    aicwf_mmc_enable_isr(en);
}

/**
 * @brief SDIO主机中断使能控制
 *
 * 使能时仅控制MMC中断开关；禁用时会释放所有已注册的SDIO中断。
 *
 * @param enable true=使能, false=禁用
 * @return true
 */
bool aicwf_sdio_host_enable_isr(bool enable)
{
    /* SDIO(DAT1) 中断风暴规避: 库 aicwf_sdio_rx_task 每轮结束都会无条件
     * enable=TRUE, 电平中断下会立刻再次进入中断形成风暴; 这里直接压住不放行,
     * 收包由 aicwifi_sdio_rxdata_mode=1 的 10ms 轮询兜底。 */
    if (enable && g_sdio_irq_mask_forced)
    {
        aicwf_mmc_enable_isr(0);
        return true;
    }

    aicwf_mmc_enable_isr(enable);

    if (!enable)
    {
        aicwf_rtos_mutex_lock(sdio_dev.bus_txrx, -1);
        aicwf_sdio_claim_host(FUNC_1);
        aicwf_sdio_release_irq(sdio_function_local[FUNC_1]);
        aicwf_sdio_release_host(FUNC_1);

#if (defined(CONFIG_AIC8800DC) || defined(CONFIG_AIC8800DW))
        aicwf_sdio_claim_host(FUNC_2);
        aicwf_sdio_release_irq(sdio_function_local[FUNC_2]);
        aicwf_sdio_release_host(FUNC_2);
#endif
        aicwf_rtos_mutex_unlock(sdio_dev.bus_txrx);
    }
    return true;
}

/**
 * @brief SDIO中断处理（内部实现）
 *
 * 在非OOB模式下通过信号量唤醒RX任务处理接收数据。
 * 在OOB模式下由OOB任务独立处理。
 */
static void aicwf_sdio_irq_hdl(void)
{
#ifdef CONFIG_OOB
    if (sdio_dev.oob_enable == false)
#endif
    {
        //sdio_host_enable_isr(false);
        if (!config_rxtask_insdio)
        {
            aicwf_plat_gpio_trigger(DRV_TRG_SDIO, 1);
            if (config_irq_sus)
                aicwf_sdio_isr(false);
            //aicwf_rtos_semaphore_signal(sdio_dev.sdio_rx_sema, false);
            aicwf_rtos_semaphore_signal(sdio_dev.sdio_rx_sema, true);
            aicwf_plat_gpio_trigger(DRV_TRG_SDIO, 0);
        }
        else
        {
            //aicwf_sdio_isr(false);
            aic_sdio_rx_task(0);
            //aicwf_sdio_isr(true);
        }
    }
}

/**
 * @brief 初始化SDIO中断
 *
 * 为Function 1（和DC/DW的Function 2）注册中断处理函数，
 * 并关闭消息确认轮询模式。
 *
 * @return 0=成功
 */
int aicwf_sdio_interrupt_init(void)
{
    aicwf_sdio_set_irq_handler(aicwf_sdio_irq_hdl);

    aicwf_rtos_mutex_lock(sdio_dev.bus_txrx, -1);
    aicwf_sdio_claim_host(FUNC_1);
    aicwf_sdio_claim_irq(sdio_function_local[FUNC_1], aicwf_host_sdio_interrupt);
    aicwf_sdio_release_host(FUNC_1);

#if (defined(CONFIG_AIC8800DC) || defined(CONFIG_AIC8800DW))
    aicwf_sdio_claim_host(FUNC_2);
    aicwf_sdio_claim_irq(sdio_function_local[FUNC_2], aicwf_host_sdio_interrupt);
    aicwf_sdio_release_host(FUNC_2);
#endif
    aicwf_rtos_mutex_unlock(sdio_dev.bus_txrx);

    msgcfm_poll_en = 0;
    return 0;
}

/**
 * @brief 设置SDIO Function块大小（内部实现）
 *
 * 通过目标 Function 的 FBR 寄存器设置块大小，并回读验证。
 *
 * @param index   目标 Function 在 sdio_function_local[] 中的索引（FUNC_1 或 FUNC_2）
 * @param addr    FBR 块大小寄存器地址
 * @param blksize 目标块大小（0或>512时默认为SDIOWIFI_FUNC_BLOCKSIZE）
 * @return true=成功, false=失败
 */
static bool __aicwf_sdio_set_block_size(uint8_t index, unsigned int addr, unsigned int blksize)
{
    unsigned char blk[2];
    int err = 0;

    struct sdio_func *func = sdio_function_local[index];
    if (!func)
    {
        DBG_SDIO_ERR("%s: sdio_function_local[%d] is NULL\n", __func__, index);
        return false;
    }

    DBG_SDIO_INF("%s: fn=%d, addr=%d, blksize=%d\n", __func__, func->num, addr, blksize);

    if ((blksize == 0) || (blksize > 512))
    {
        blksize = SDIOWIFI_FUNC_BLOCKSIZE;
    }

    /* 块大小寄存器位于 Function 0 (CCCR) 地址空间，通过 F0 读写接口操作 */
    blk[0] = blksize & 0x0ff;
    blk[1] = (blksize >> 8) & 0x0ff;

    aicwf_sdio_f0_writeb(blk[0], addr, &err);
    if (err)
    {
        DBG_SDIO_ERR("aic_sdio_set_block_size fail 0!\n");
        return false;
    }

    aicwf_sdio_f0_writeb(blk[1], addr + 1, &err);
    if (err)
    {
        DBG_SDIO_ERR("aic_sdio_set_block_size fail 1!\n");
        return false;
    }

    blk[0] = __aicwf_sdio_f0_readb(addr, &err);
    if (err)
    {
        DBG_SDIO_ERR("sdio_readb 0x%d fail 0!\n", addr);
        return false;
    }

    blk[1] = __aicwf_sdio_f0_readb(addr + 1, &err);
    if (err)
    {
        DBG_SDIO_ERR("sdio_readb 0x%x fail 1!\n", addr + 1);
        return false;
    }

    if (((unsigned int)(blk[1] << 8) | blk[0]) != blksize)
    {
        DBG_SDIO_ERR("%s: fail, target=%d, readback=%d!\n", __func__, blksize, ((unsigned int)(blk[1] << 8) | blk[0]));
        return false;
    }

    func->cur_blk_size = blksize;

    return true;
}

/**
 * @brief 释放SDIO Function 2（关闭中断并释放IRQ）
 */
void aicwf_sdio_release_func2(void)
{
    int ret = 0;

    /* DL/DL_H 通过 CONFIG_NO_SDIO_FUNC_RELEASE 禁用 release func2:
     * aicwf_sdio_func_init 末尾已调用 aicwf_hal_func_release_set(0),
     * 此处运行时判断使该宏真正生效 (DL 复用 DC 库, 运行时 chipid 同为 DC,
     * 仅靠 chipid 无法区分, 必须以运行时开关为准) */
    if (!aicwf_hal_func_release_get())
    {
        DBG_SDIO_INF("%s: release func2 disabled, skip.\n", __func__);
        return;
    }

    /* flag_tx_set(false) 与 release 绑定: func2 释放后 TX 数据改走 FUNC_1,
     * 与 release 放在一起, 由上面的运行时开关统一控制 */
    aicwf_hal_flag_tx_set(false);

    DBG_SDIO_INF("%s\n", __func__);
    aicwf_sdio_claim_host(FUNC_2);
    ret = aicwf_sdio_writeb_cmd52_func2(sdio_dev.sdio_reg.intr_config_reg, 0x0);
    if (ret < 0)
    {
        DBG_SDIO_ERR("reg:%d write failed!\n", sdio_dev.sdio_reg.intr_config_reg);
    }

    aicwf_sdio_release_irq(sdio_function_local[FUNC_2]);
    aicwf_sdio_release_host(FUNC_2);
}

/**
 * @brief SDIO Function初始化（标准模式）
 *
 * 初始化Function 1和Function 2：使能功能、设置块大小、
 * 禁用字节模式、使能SDIO中断。
 *
 * @param chipid  芯片产品ID
 * @param sdiodev SDIO设备结构
 * @return 0=成功, 负值=失败
 */
int aicwf_sdio_func_init(uint16_t chipid, struct aic_sdio_dev *sdiodev)
{
    int32_t ret = 0;
    uint8_t block_bit0 = 0x1;
    uint8_t byte_mode_disable = 0x1;//1: no byte mode
    struct sdio_func *func;

    /* SDIO Function 1 */
    //aicwf_sdio_claim_host(sdio_function[1]);
    if (chipid == PRODUCT_ID_AIC8801)
    {
        /* Enable Function 0 */
        func = __aicwf_sdio_find_func(0x7, SDIO_VENDOR_ID_AIC8801, SDIO_DEVICE_ID_AIC8801);
        if (!func)
        {
            ret = -ENODEV;
            DBG_SDIO_ERR("%s: __aicwf_sdio_find_func func0 failed!!(%d)\n", __func__, ret);
            return ret;
        }

    }
    else if (chipid == PRODUCT_ID_AIC8800DC || chipid == PRODUCT_ID_AIC8800DW)
    {
        /* Enable Function 0 */
        func = __aicwf_sdio_find_func(0x7, SDIO_VENDOR_ID_AIC8800DC, SDIO_DEVICE_ID_AIC8800DC);
        if (!func)
        {
            ret = -ENODEV;
            DBG_SDIO_ERR("%s: __aicwf_sdio_find_func func0 failed!!(%d)\n", __func__, ret);
            return ret;
        }
    }
    ret = __aicwf_sdio_enable_function(sdio_function_local[FUNC_1]->num);
    //aicwf_sdio_release_host(sdio_function_local[1]);

    ret = __aicwf_sdio_set_block_size(FUNC_1, sdiodev->sdio_reg.fun1_blk_size_reg, SDIOWIFI_FUNC_BLOCKSIZE);
    if (ret == false)
    {
        ret = -EREMOTEIO;
        DBG_SDIO_ERR("%s: blksize set failed\n", __func__);
        return ret;
    }

    ret = aicwf_sdio_writeb_cmd52(sdiodev->sdio_reg.register_block, block_bit0);
    if (ret == false)
    {
        ret = -EREMOTEIO;
        DBG_SDIO_ERR("reg:%d write failed!\n", sdiodev->sdio_reg.register_block);
        return ret;
    }

    //1: no byte mode
    ret = aicwf_sdio_writeb_cmd52(sdiodev->sdio_reg.bytemode_enable_reg, byte_mode_disable);
    if (ret == false)
    {
        ret = -EREMOTEIO;
        DBG_SDIO_ERR("reg:%d write failed!\n", sdiodev->sdio_reg.bytemode_enable_reg);
        return ret;
    }

    //enable sdio interrupt
    ret = aicwf_sdio_writeb_cmd52(sdiodev->sdio_reg.intr_config_reg, 0x7);
    if (ret == false)
    {
        ret = -EREMOTEIO;
        DBG_SDIO_ERR("reg:%d write failed!\n", sdiodev->sdio_reg.intr_config_reg);
        return ret;
    }
    DBG_SDIO_INF("aicwf_sdio_host_init:enable fun1 ok!\n");

    /* SDIO Function 2 */
    //aicwf_sdio_claim_host(sdio_function[2]);
    if (chipid == PRODUCT_ID_AIC8800DC || chipid == PRODUCT_ID_AIC8800DW)
    {
        /* Enable Function 2 */
        func = __aicwf_sdio_find_func(0x7, SDIO_VENDOR_ID_AIC8800DC, 0xc18d);
        if (!func)
        {
            ret = -ENODEV;
            DBG_SDIO_ERR("%s: __aicwf_sdio_find_func func2 failed!!(%d)\n", __func__, ret);
            return ret;
        }
    }
    ret = __aicwf_sdio_enable_function(sdio_function_local[FUNC_2]->num);
    //aicwf_sdio_release_host(sdio_function[2]);

    if (chipid == PRODUCT_ID_AIC8800DC || chipid == PRODUCT_ID_AIC8800DW)
    {
        ret = __aicwf_sdio_set_block_size(FUNC_2, sdiodev->sdio_reg.fun2_blk_size_reg, SDIOWIFI_FUNC_BLOCKSIZE);
        if (ret == false)
        {
            ret = -EREMOTEIO;
            DBG_SDIO_ERR("aicwf_sdio_host_init:func2: blksize set failed\n");
            return ret;
        }

        ret = aicwf_sdio_writeb_cmd52_func2(sdiodev->sdio_reg.register_block, block_bit0);
        if (ret < 0)
        {
            DBG_SDIO_ERR("reg:%d write failed!\n", sdiodev->sdio_reg.register_block);
            return ret;
        }

        //1: no byte mode
        ret = aicwf_sdio_writeb_cmd52_func2(sdiodev->sdio_reg.bytemode_enable_reg, byte_mode_disable);
        if (ret < 0)
        {
            DBG_SDIO_ERR("reg:%d write failed!\n", sdiodev->sdio_reg.bytemode_enable_reg);
            return ret;
        }

        //enable sdio interrupt
        ret = aicwf_sdio_writeb_cmd52_func2(sdiodev->sdio_reg.intr_config_reg, 0x7);
        if (ret < 0)
        {
            DBG_SDIO_ERR("reg:%d write failed!\n", sdiodev->sdio_reg.intr_config_reg);
            return ret;
        }
        DBG_SDIO_INF("%s:enable fun2 ok!\n", __func__);
    }

    /* DL 型号禁用 release sdio function2（运行时控制，host 库与 DC 共用） */
#ifdef CONFIG_NO_SDIO_FUNC_RELEASE
    aicwf_hal_func_release_set(0);
#endif

    return 0;
}

/**
 * @brief SDIO Function初始化（SDIOv3模式，D80系列）
 *
 * 针对D80系列芯片的SDIOv3初始化流程：
 * 使能Function 1、设置块大小、配置IO pad、禁用字节模式、使能中断。
 *
 * @param chipid  芯片产品ID
 * @param sdiodev SDIO设备结构
 * @return 0=成功, 负值=失败
 */
int aicwf_sdiov3_func_init(uint16_t chipid, struct aic_sdio_dev *sdiodev)
{
    int ret = 0;   /* [AIC8800-PORT][SiFli] 原 int32_t(=long); aicwf_sdio_f0_writeb 形参为 int*, 改 int 与函数返回类型一致 */
    uint8_t byte_mode_disable = 0x1;//1: no byte mode

    /* SDIO Function 1 */
    //aicwf_sdio_claim_host(sdio_function[1]);
    if (chipid == PRODUCT_ID_AIC8800D80 || chipid == PRODUCT_ID_AIC8800D80N)
    {
        struct sdio_func *func;
        /* 本平台不填 function 级 VID/PID: 本卡 func1 的 CIS 只有 FUNCID/FUNCE,
         * 没有 CISTPL_MANFID, 其 product 恒为 0, __aicwf_sdio_find_func() 按型号
         * 必然查不到; 型号已由 probe 中 chipmatch 按卡级 PID 校验,
         * 这里直接取 FUNC_1 并确认存在 */
        func = sdio_function_local[FUNC_1];
        if (!func)
        {
            ret = -ENODEV;
            DBG_SDIO_ERR("%s: FUNC_1 not found!!(%d)\n", __func__, ret);
            return ret;
        }
    }
    ret = __aicwf_sdio_enable_function(sdio_function_local[FUNC_1]->num);
    if (ret)
    {
        DBG_SDIO_ERR("__aicwf_sdio_enable_function failed\n");
    }
    //aicwf_sdio_release_host(sdio_function[1]);

    ret = __aicwf_sdio_set_block_size(FUNC_1, sdiodev->sdio_reg.fun1_blk_size_reg, SDIOWIFI_FUNC_BLOCKSIZE);
    if (ret == false)
    {
        ret = -EREMOTEIO;
        DBG_SDIO_ERR("%s: blksize set failed\n", __func__);
        return ret;
    }
    DBG_SDIO_INF("sdio_host_init:sdio_set_block_size %d\n", SDIOWIFI_FUNC_BLOCKSIZE);

    aicwf_sdio_f0_writeb(0x7F, 0xF2, &ret);
    if (ret)
    {
        DBG_SDIO_ERR("set fn0 0xF2 fail %d\n", ret);
        return ret;
    }

#if 0 //set phase and sample
    {
        val |= SDIOCLK_FREE_RUNNING_BIT;
        aicwf_sdio_f0_writeb(val, 0xF0, &ret);
        if (ret)
        {
            DBG_SDIO_ERR("set iopad ctrl fail %d\n", ret);
            return ret;
        }
        aicwf_sdio_f0_writeb(0x0, 0xF8, &ret);
        if (ret)
        {
            DBG_SDIO_ERR("set iopad delay2 fail %d\n", ret);
            return ret;
        }
        aicwf_sdio_f0_writeb(0x20, 0xF1, &ret);
        if (ret)
        {
            DBG_SDIO_ERR("set iopad delay1 fail %d\n", ret);
            return ret;
        }
        aicwf_rtos_udelay(100);
    }
#endif

    //1: no byte mode
    ret = aicwf_sdio_writeb_cmd52(sdiodev->sdio_reg.bytemode_enable_reg, byte_mode_disable);
    if (ret < 0)
    {
        DBG_SDIO_ERR("reg:%d write failed!\n", sdiodev->sdio_reg.bytemode_enable_reg);
        return ret;
    }

    ret = aicwf_sdio_writeb_cmd52(sdiodev->sdio_reg.intr_config_reg, 0x7);
    if (ret == false)
    {
        ret = -EREMOTEIO;
        DBG_SDIO_ERR("reg:%d write failed!\n", sdiodev->sdio_reg.intr_config_reg);
        return ret;
    }


#if 0
    ret = aicwf_sdio_writeb_cmd52(sdiodev->sdio_reg.wakeup_reg, 0x11);
    if (ret < 0)
    {
        DBG_SDIO_ERR("reg:%d write failed!\n", sdiodev->sdio_reg.wakeup_reg);
        return ret;
    }

#if 1
    aicwf_rtos_udelay(5);
    ret = aicwf_sdio_readb_cmd52(sdiodev->sdio_reg.sleep_reg, &val1);
    if (ret < 0)
    {
        DBG_SDIO_ERR("reg:%d read failed!\n", sdiodev->sdio_reg.sleep_reg);
        return ret;
    }

    if (!(val1 & 0x10))
    {
        DBG_SDIO_ERR("wakeup fail\n");
    }
    else
    {
        DBG_SDIO_INF("sdio ready\n");
    }
#endif
#endif

    return 0;
}

//-------------------------------------------------------------------
// 运行时 chipmatch + 固件加载后中断使能 (参考 Linux aicsdio.c / 源项目 sdio_port.c)
//-------------------------------------------------------------------

/**
 * @brief 根据硬件实际 VID/PID 匹配 chipid (参考 Linux aicsdio.c aicwf_sdio_chipmatch)
 *
 * 匹配顺序与 Linux 驱动一致:
 *   - AIC8801:     VID=0x5449, PID=0x0145
 *   - AIC8800DC:   VID=0xc8a1, PID=0xc08d
 *   - AIC8800D80:  VID=0xc8a1, PID=0x0082
 *   - AIC8800D80N/DN: VID=0xc8a1, PID=0x9081/0x9082/0x9084/0x9085
 *   - AIC8800D80WN:   VID=0xc8a1, PID=0x9083/0x9086
 *   - AIC8800D80X2:   VID=0xc8a1, PID=0x2082
 *
 * @param vid 硬件厂商 ID (来自 card->cis.manufacturer)
 * @param pid 硬件产品 ID (来自 card->cis.product)
 * @return PRODUCT_ID_* 枚举值 (AIC8801 为 0), 或 -1 表示未匹配 (fallback 到编译时宏)
 */
static int aicwf_sdio_chipmatch(uint16_t vid, uint16_t pid)
{
    if (vid == SDIO_VENDOR_ID_AIC8801 && pid == SDIO_DEVICE_ID_AIC8801)
    {
        DBG_SDIO_INF("%s: USE AIC8801\n", __func__);
        return PRODUCT_ID_AIC8801;
    }
    else if (vid == SDIO_VENDOR_ID_AIC8800DC && pid == SDIO_DEVICE_ID_AIC8800DC)
    {
        DBG_SDIO_INF("%s: USE AIC8800DC\n", __func__);
        return PRODUCT_ID_AIC8800DC;
    }
    else if (vid == SDIO_VENDOR_ID_AIC8800D80 && pid == SDIO_DEVICE_ID_AIC8800D80)
    {
        DBG_SDIO_INF("%s: USE AIC8800D80\n", __func__);
        return PRODUCT_ID_AIC8800D80;
    }
    else if (vid == SDIO_VENDOR_ID_AIC8800D80N &&
             (pid == SDIO_DEVICE_ID_AIC8800D80N ||
              pid == SDIO_DEVICE_ID_AIC8800D80LN ||
              pid == SDIO_DEVICE_ID_AIC8800D40N ||
              pid == SDIO_DEVICE_ID_AIC8800D40LN))
    {
        DBG_SDIO_INF("%s: USE AIC8800D80N (pid=0x%04X)\n", __func__, pid);
        return PRODUCT_ID_AIC8800D80N;
    }
    else if (vid == SDIO_VENDOR_ID_AIC8800D80N &&
             (pid == SDIO_DEVICE_ID_AIC8800D80WN ||
              pid == SDIO_DEVICE_ID_AIC8800D40WN))
    {
        DBG_SDIO_INF("%s: USE AIC8800D80WN (pid=0x%04X)\n", __func__, pid);
        return PRODUCT_ID_AIC8800D80N; /* 与 D80N 共用初始化路径 */
    }
    else if (vid == SDIO_VENDOR_ID_AIC8800D80X2 && pid == SDIO_DEVICE_ID_AIC8800D80X2)
    {
        DBG_SDIO_INF("%s: USE AIC8800D80X2\n", __func__);
        return PRODUCT_ID_AIC8800D80X2;
    }
    else
    {
        DBG_SDIO_ERR("%s: unknown VID=0x%04X, PID=0x%04X, fallback to compile-time\n",
                     __func__, vid, pid);
        return -1;
    }
}

/**
 * @brief 固件加载完成后补写 intr_config_reg=0x07 使能 WiFi 芯片侧 SDIO 中断
 *
 * bootrom 阶段该寄存器可能未 ready (写失败), start_from_bootrom 成功后调用
 * 此函数补写. 对齐源项目 aicwf_sdio_enable_irq_after_fw_boot().
 *
 * @return 0=成功, -1=失败
 */
int aicwf_sdio_enable_irq_after_fw_boot(void)
{
    int ret;
    struct aic_sdio_dev *sdiodev = &sdio_dev;

    DBG_SDIO_INF("aicwf_sdio_enable_irq_after_fw_boot: retry write intr_config_reg=0x07\n");

    ret = aicwf_sdio_writeb_cmd52(sdiodev->sdio_reg.intr_config_reg, 0x07);
    if (!ret)
    {
        DBG_SDIO_ERR("%s: post-fw-boot write intr_config_reg still failed!\n", __func__);
        return -1;
    }

#if AIC_FC_Q1_AFTER_FWBOOT
    /* [AIC8800-PORT][SiFli] 补丁 FW(PATCH/CALIB) 已启动, 数据队列(Q1)应已开始发放
     * TX 信用: 在此收回 ROM 阶段的 Q1->Q2 读重定向, 让数据通路读到真实 Q1。
     * 本函数是库在 "start_from_bootrom success." 之后调用的第一个明确锚点。
     * 置 AIC_FC_Q1_AFTER_FWBOOT=0 可保持原行为(永远读 Q2)做 A/B 对照。 */
    if (g_fc_redirect_q2)
    {
        g_fc_redirect_q2 = 0;
        rt_kprintf("[AICFC] patch FW up -> flow-ctrl read back to real Q1(0x03)\n");
    }
#endif

    DBG_SDIO_INF("aicwf_sdio_enable_irq_after_fw_boot: intr_config_reg=0x07 OK (WiFi IRQ enabled)\n");
    return 0;
}

#endif /* CONFIG_SDIO_SUPPORT */
