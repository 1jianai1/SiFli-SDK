# AIC8800D80N 驱动本地补丁清单（覆盖厂家驱动后必看）

> 背景：`customer/peripherals/wifi/aic8800_rtos_driver/` 被厂家 diff 包整体覆盖时，
> 会把平台侧**本地 bring-up 补丁**和**型号配置**一起冲掉（已发生两次）。
> 本文件记录"覆盖后必须补回什么"，覆盖后照此清单逐项恢复即可。
>
> 权威副本：覆盖前的工作版本已提交在 git 中，逐文件取回用
> `git show HEAD:<路径> > <目标>`；下面每项都给出 HEAD 中的参考位置。

---

## 一、型号与构建开关（不补 = 初始化直接失败）

本板芯片实测 **AIC8800D80N**：卡级 `PID=0x9082`、`chip_id=3`、`sub_id=1`、`rev=U03`。

| 位置 | 必须取值 | 作用 |
|---|---|---|
| `SConscript` → `CPPDEFINES` | `'CONFIG_AIC8800D80N'` | **决定 platform 层固件表**（见下） |
| `aic_cfg.mk` | `AIC_CHIP_MODEL ?= D80N` | Make 层型号（重编 host 库时用） |
| `platform_config.mk` | `AIC_CHIP_MODEL := D80N` | 同上 |
| `platform_config_cortex-m33.mk` | `AIC_CHIP_MODEL := D80N` | 同上 |

**为什么必须 D80N**：`platform/fmac/aic_fw_port.c` 里 D80N 的 CINIT/PATCH/CALIB 固件
**只在 `#if defined(CONFIG_AIC8800D80N)` 下编译**（含 `case FMACFW_CINIT_8800D80N_U02:`）。
若按 D80 编译，bootrom 阶段 `aic_fw_ptr_get(id=60)` 返回 "unknown id"、size=0，日志表现：

```
[AIC][MACIF]====> D80N stage1: CINIT (bootrom)
[AIC][FMAC]FW ptr: unknown id 60
[AIC][MACIF]  [CINIT] fw_id=60, dst=0x00133000, size=0
[AIC][MACIF]D80N CINIT upload fail: -1
[AIC][FHOST]aic_wifi_open:fdrv fail
```

（`FMACFW_CINIT_8800D80N_U02` 在 `port/inc/aic_fw_port.h` 枚举中正好 = 60。）

`SConscript` 还需保留平台段的 **上电引脚注入**（厂家包无此段）：

```python
pwr_gpio = GetConfigValue('AIC8800_RTOS_WIRELESS_PWR_GPIO')
if pwr_gpio != '':
    CPPDEFINES += ['AIC_WIRELESS_PWR_GPIO=' + str(pwr_gpio)]
```

对应 Kconfig 选项在 `customer/peripherals/wifi/Kconfig`：
`WIFI_USING_AIC8800_RTOS` / `AIC8800_RTOS_WIRELESS_PWR_GPIO` / `AIC8800_USE_RT_WLAN`。

---

## 二、平台源码补丁（厂家版删掉或退回 stub，必须补回）

| # | 文件 | 补回内容 | HEAD 参考 |
|---|---|---|---|
| 1 | `platform/hal/sdio_port.c` | `g_sdio_irq_mask_forced`（非 static，main.c 引用）+ `aicwf_sdio_isr()`/`aicwf_sdio_host_enable_isr()` 中断风暴守卫 | 约 139–152、1019–1047 |
| 2 | `platform/hal/sdio_port.c` | 卡识别表首条用 `SDIO_ANY_PROD_ID`（本卡 func1 CIS 无 MANFID，product 恒 0） | 约 172–177 |
| 3 | `platform/hal/sdio_port.c` | `aicwf_sdiov3_func_init()` 直取 `sdio_function_local[FUNC_1]` 回退 | 约 1361–1374 |
| 4 | `platform/hal/sdio_port.c` | 功能号上界 `co_min(..., SDIOM_MAX_FUNCS - 1)` + `i <= num` | 约 241–247 |
| 5 | `platform/hal/sdio_port.c` | D80N flow-ctrl 读侧重定向，开关 **`#define AIC_FC_Q1_REDIRECT_EN 1`** | 约 795–897、1519–1551 |
| 6 | `platform/hal/plat_port.c` | 上电 GPIO + `aicwf_wifi_pwr_locked` 锁 + `aicwf_sdio_pdn_wifi()`/`aicwf_sdio_reset_wifi()`；`aicwf_ATRESP()` 补回 `rt_kprintf` | 整段 |
| 7 | `platform/platform_init.c` | 整个文件：`INIT_BOARD_EXPORT(aicwf_wifi_power_on)`（拉低→10ms→拉高→200ms，`HAL_Delay` 忙等） | 全文 53 行 |
| 8 | `port/inc/plat_port.h` | `FHOST_WPA_STACK_SIZE (8192)`、`RWNX_WIFI_STACK_SIZE (4096)`（厂家为 3072/1024，会栈溢出） | 约 98、104 |
| 9 | `platform/wifi/wifi_port.c` | `#elif defined(CONFIG_AIC8800D80N)` → `aic_get_wps_ModelName()` 返回 `"AIC-8800-D80N"` | 约 206–216 |
| 10 | `platform/netif/netif_port.c` + `port/inc/netif_port.h` | RT-Thread netdev 桥接：`aicwf_net_if_netdev_add/_del/_sync`，DHCP 成功后 `aicwf_net_if_netdev_sync(NULL)` | 约 744–1990 |
| 11 | `platform/wifi/wifi_port_plt.c` | **轮询等卡**：把 `mmcsd_wait_cd_changed(RT_WAITING_FOREVER)` 换成 `mmcsd_get_stat()` 有界轮询（+ 局部 `extern uint8_t mmcsd_get_stat(void);`） | 约 19–25、47–67 |
| 12 | `platform/wifi/wifi_port_sta.c` | DHCP 成功后补 `aicwf_net_if_netdev_sync(NULL);`（与第 10 项配对） | 约 177–184 |

### 第 1、5、7、8、9、10 项的症状对照

| 缺哪项 | 表现 |
|---|---|
| 缺 1 | 链接失败 `undefined reference to 'g_sdio_irq_mask_forced'`；或运行时 DAT1 中断风暴把串口饿死（只剩一次 `msh />`） |
| 缺 5 | 能扫描/认证/关联，但连接时 `data fc:-2` → `deauth reason=15 (4WAY_HANDSHAKE_TIMEOUT)` |
| 缺 7 | 上电时序缺失，SDIO 卡枚举失败（上电必须早于 SDHCI host 初始化） |
| 缺 8 | `sta conn` 期间 `fhost_wpa_task` 栈溢出 → usage fault `STKOF(SCS_CFSR.UFSR:0x10)` |
| 缺 9 | D80N 构建链接失败 `undefined reference to 'aic_get_wps_ModelName'` |
| 缺 10 | DHCP ACK 时 `netdev_set_dns_server()` 拿到 NULL → `RT_ASSERT(netdev)` 断言；`ifconfig`/ping 恒 LINK_DOWN；跨网段 `sendto` 立即 `EHOSTUNREACH` |
| 缺 11 | **主流程静默卡死**：打印完 `[aic8800] AIC_SDIO_IRQ_MASK_FORCED=1` 后**没有任何 `[AIC]` 日志**，直接停在 `msh />`（`mmcsd_wait_cd_changed` 永不返回，因为 `mmcsd_detect()` 的 SDIO 分支不投递 hotplug 消息） |

> 第 6、7 项的 `rt_kprintf` 是**有意**与原厂日志并存：`DBG_APP_INF` 走 AIC 调试通道，
> 只抓 `[AIC][*]` 时看不到扫描/连接结果。若发现每行打印两次，说明两条通道都指向控制台，可去掉其一。

---

## 三、SDK 公共代码侧（**不随驱动覆盖丢失**，已随 SDK 提交）

| 文件 | 改动 |
|---|---|
| `rtos/rtthread/components/drivers/sdio/sdio.c` | `sdio_match_card()` 支持 `SDIO_ANY_PROD_ID`，function CIS 无 MANFID 时回退卡级 product |
| `rtos/rtthread/components/drivers/include/drivers/sdio.h` | `#define SDIO_ANY_PROD_ID 0xffff` |
| `rtos/rtthread/components/drivers/sdio/mmcsd_core.c` | `mmcsd_detect()` 的 SDIO 分支**不投递** `mmcsd_hotpluge_mb`（因此等卡必须轮询 `mmcsd_get_stat()`，不能 `mmcsd_wait_cd_changed(RT_WAITING_FOREVER)`） |

---

## 四、覆盖后的标准恢复流程

1. 按第一节改 4 处型号开关（默认 D80N）；
2. 按第二节 1–5 补 `sdio_port.c`（注意 5 的开关要置 1）；
3. 按第二节 6–7 补 `plat_port.c` / `platform_init.c`；
4. 按第二节 8–10 补 `plat_port.h` / `wifi_port.c` / `netif_port.c(+h)`；
5. 删掉覆盖带进来的源码内 `*.o`（厂家包会带 14 个）；
6. 重新配置编译：清 `build_<board>_hcpu` 后
   `scons --board=sf32lb55-lcd_n16r4n4 -j8`。

编译通过的判据（本文件撰写时实测）：`EXIT=0`，`main.elf ≈ 4.37 MB`、`main.bin ≈ 1.08 MB`，
且 `main.map` 中可见 `fmacfw_cinit_8800d80n_u02` / `fmacfw_patch_8800d80n_u02` /
`fmacfw_calib_8800d80n_u02` / `fmacfw_patch_tbl_8800d80n_u02`。
