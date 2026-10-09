# AIC8800D80N SDIO bring-up 问题交接说明

> 面向接手排查的 agent/同事。目标：解释"卡在哪、已排除什么、还剩哪些假设、怎么验证"，
> 并给出当前代码里的诊断开关与判读表。
> 工作目录：`SiFli-SDK/example/aic8800_d80`；SDK 根：`SiFli-SDK/`（下称 `$SDK`）；
> 驱动：`$SDK/customer/peripherals/wifi/aic8800_rtos_driver`（host 预编译库 + SiFli 适配的 platform 层）。

---

## 1. 硬件 / 软件基线

| 项 | 值 |
|---|---|
| 主控 | SiFli SF32LB55X (HCPU, Cortex-M33)，板型 `sf32lb55-lcd_n16r4n4` |
| WiFi 模块 | AIC8800D80N 系列，卡级 VID/PID = `0xC8A1 / 0x9082`（代码里按 D80N 家族处理） |
| 芯片 ROM | `Bootrom [Sep 11 2025, g38f9137]`（比驱动库内 D80 固件集更新） |
| 总线 | SDIO2 = SDHCI2/SDMMC2，4-bit，初始化后 24MHz（枚举期 400kHz） |
| SDIO 引脚 | PA44=CLK, PA45=CMD, PA47=D0, PA49=**D1**, PA51=D2, PA55=D3 |
| 上电脚 | WL_REG_ON = GPIO124（`CONFIG_AIC8800_RTOS_WIRELESS_PWR_GPIO=124`） |
| host 驱动 | `libaic8800_driver.a`（预编译，2026/9/16，release `NV1.1.6 STA AP P2P ggd4509b7`）+ `libaic_mbedtls.a`；**单库、与型号无关** |
| platform 层 | scons 编的 14 个文件（`$SDK/customer/peripherals/wifi/aic8800_rtos_driver/platform/**`） |
| 总线/固件模式 | `CONFIG_LOAD_FW=2`（innermode，固件以 C 数组内嵌）；BT/NimBLE 未移植 |

---

## 2. 现象（两条串口的日志尾部）

**主机（HCPU）日志**，关键顺序（时间戳 = 分:秒.毫秒）：

```
02.199  Serial:C2 Chip:1 Package:b Rev:80 ...
02.201  [AIC][CMN]wifi pwr pin = 124
02.480  [S/D] detect SDIO begin ... 02.518  card registered: man=0xc8a1 prod=0x9082 f1prod=0x0000 funcs=1
02.519  I/SDIO: init_sdio ret=0
02.523  [aic8800] demo start
02.526  [AIC][SDIO]aicwf_sdio_chipmatch: USE AIC8800D80N (pid=0x9082)
02.526  [AIC][SDIO][chipmatch] VID=0xC8A1, PID=0x9082 -> chipid=4
02.527  [AIC][APP]wifi_driver_init / GetWiFiInitStat->>0
02.534  [AIC][APP]wifi_drive_op: 1          <- 上电复位 + init
02.535  [AIC][FHOST]aic_wifi_init, mode=0
02.535  [AIC][FHOST]aic_wifi_open:0,0
02.567  [AIC][FHOST]aicwf chipid: runtime match chipid=4
02.567  [AIC][SDIO]aicwf_sdio_reg_init:chip=4
02.699  [AIC][SDIO]__aicwf_sdio_set_block_size: fn=1, addr=272, blksize=512
02.826  [AIC][SDIO]sdio_host_init:sdio_set_block_size 512
02.922  [AIC][SDIO]sdio func init success
02.925  [AIC][SDIO]sdio_rx_buf: count=64 size=2048 ... / sdio_rx_node_sema initial count:64
02.925  创建 aicwf_sdio_rx_task / cli_cmd_task / fhost_cntrl_task / fhost_rx_task / rwnx_timer_task
02.998  [AIC][MACIF]rwnx_fdrv_init enter
02.999  创建 apm_task
03.115 ~ 05.114  [AIC][APP]GetWiFiInitStat->>0   (主线程 200ms 轮询, 共 11 次)
05.302  [AIC][SDIO]msg fc:0                      <<< 卡在这里
```

**WiFi 芯片（ROM）日志**：

```
02.220  Bootrom [Sep 11 2025, g38f9137]
02.222  RstCause:0000,c0,Boot:0067,6
02.222  Device mode
02.223  Auto boot to 0x000101a9
02.225  fw boot start / boot_arg=0x0
02.228  pu usb / usb init / usb init done
02.470  cmd5
02.631  hosttype=1
02.838  fn1_ioen                                  <<< 之后永无输出
```

要点：**芯片日志停在 `fn1_ioen`**（= 它的 SDIO function 1 IO 刚被使能），之后再没收到主机的任何消息；**主机日志停在 `msg fc:0`**。

---

## 3. 已定位的卡点（机制已明确）

调用链（主机侧）：

```
main() -> aic_wifi_demo_test()                     (src/main.c)
  -> aic_platform_init() / aicwf_sdio_drv_register()
  -> wifi_driver_init() -> aicwf_wifi_init_task     (platform/wifi/wifi_port.c:638/536)
       op=AIC_DRIVER_REBOOT_PU -> aic_wifi_init() -> aic_wifi_open()   [库内]
          wifi_chip_status_set()
          aicwf_sdio_reg_init(chip=4)              -> sdio_reg.flow_ctrl_reg=0x03 (V3 映射)
          aicwf_sdio_host_init() -> platform aicwf_sdiov3_func_init()  (platform/hal/sdio_port.c:1215)
          rwnx_fdrv_init()                          [库内]
             rwnx_apm_staloss_init()
             **第一条 cfg 消息** rwnx_send_dbg_mem_read_req(0x40500000, ...)
                -> aicwf_sdio_tx_msg -> aicwf_sdio_flow_ctrl_msg
                     -> 读 func1 reg 0x03  => 恒 0x00
```

库内 `aicwf_sdio_flow_ctrl_msg`（`libaic8800_driver.a(sdio_co.c.o)`，本次构建中符号地址 `0x100539f4`）逻辑：

- 读 `sdio_reg.flow_ctrl_reg`（D80 家族 = **0x03**）；
- 为 0 时重试 **50 次**（`FLOW_CTRL_RETRY_COUNT`，30×200µs + 10×2ms + 10×10ms ≈ **126ms/轮**）；
- 50 次后打印 **`msg fc:%d`**（值即 `fc_reg`=0），**返回 0**；
- 上层 `aicwf_sdio_tx_msg`（`0x10054e04`）再重试最多 10 轮，最后 `tx msg fc retry fail` → `rwnx_fdrv_init` 失败 → `aic_wifi_close()`（清 `chip_status`）→ `GetWiFiInitStat()` 永远为 0 → 主线程无限轮询。

**`msg fc:0` 的语义**：不是总线/CRC/命令错误，而是"芯片侧配置通道（cfg/msg TX 缓冲）报 0 个可用 buffer"。因为 cfg 通道起不来，后面整条启动链（D80N 的 PATCH → PATCH_TBL → CINIT → CALIB）一步都没开始。

> 注意：`platform/hal/sdio_port.c:713` 的 `aicwf_sdio_readb_cmd52_by_num()` 在
> `wifi_chip_status_get()==0` 时会**直接返回 false 且不写 *data**，而库把 0 当"成功" →
> 也能产生同样的 `msg fc:0`，且**总线上一条 CMD52 都没有**。所以"重试窗口里到底有没有 CMD52"
> 是区分"真读数=0"与"根本没读"的关键（本次波形已确认有 CMD52 且返回 0）。

---

## 4. 已经确定 / 已排除的事实

1. **SDIO 枚举是好的**：CMD5/CMD3/CMD7/CCCR/CIS 全部成功，读到正确的 VID/PID 与 func1 属性
   （`maxblksize=2048`）。`sdio_match_card()` 的 product 匹配曾被修过（`sdio.c` 里取了卡级 CIS 的 product）。
2. **CMD52 读写通路是好的**：`__aicwf_sdio_set_block_size()`（`platform/hal/sdio_port.c:995-1049`）会向
   func0 的 `0x110/0x111` 写 `512` 并**读回比较**，日志里没有 `fail, target=512, readback=...`
   → 写进去、读回来一致。
3. **CMD52 的读数据在 R5 响应里、走 CMD 线**（`sdio_io_readb()` → `sdio_io_rw_direct()`），
   DAT1 不参与 → **D1 的活动与本次 `0x03` 读值为 0 没有因果关系**。
4. **型号宏与固件数据**：host 库单库、型号无关；`AIC_CHIP_MODEL`/`CONFIG_AIC8800D80N` 只影响 platform 侧
   编进镜像的固件数据。当前 D80N 编译下镜像里有
   `.rodata.fmacfw_patch_8800d80n_u02 / _patch_tbl_ / _cinit_ / _calib_`（4 个都在，见 `main.map`）。
   运行时 chipid=4 的分支（`rwnx_platform_on`, `0x100642d8`）**不走** `rwnx_plat_fmac_load()`，
   所以**不需要** D80 主 FMAC 数组（`aic_fw_port.c:1359-1371` 那段 D80N 分支在 innermode 下不可达）。
5. 板级 SDIO 六根线在 `bsp_pinmux.c:134-139` 当前是 **`PIN_NOPULL`**（CLK/CMD/D0~D3 全是）；
   历史上曾出现过 `PIN_PULLUP` 的版本，说明被人改过 —— 需确认哪个是有意为之。
6. 芯片 ROM 日志只出现**一次**（没有在主机上电/复位动作之后再打一次 banner）——
   `GPIO124` 的 `WL_REG_ON` 时序是否真的让芯片复位过，存疑。
7. **D1 的活动特性（实测波形）**：D1 的"规律性下拉"集中在**主机发送 CMD52 的时段**。
   两种解释（都要考虑，已被决策为"直接把 D1 从数据通路隔离掉"）：
   - **(a) 串扰/耦合**：CMD52 不使用任何 DAT 线（双方都不驱动 DAT）。若 D1 悬空
     （板级 `PIN_NOPULL` + 无外接上拉），相邻 CMD 线的跳变就会在 D1 上耦合出周期性下陷，
     并把直流电平拖到中间值（量到 2.6V）。特征：与 CMD 边沿一一对应、窄凹陷、通常到不了轨。
   - **(b) 芯片在拉 SDIO 中断**：按 SDIO 规范，**DAT1 在 1-bit 与 4-bit 下都是"卡→主机中断"线**
     （DAT1 被拉低 = 中断请求）。若芯片有挂起中断且主机没清，D1 会被**持续**拉低。
     特征：与时序无关的成段低电平；且 `CCCR 0x05(INT_PENDING)` / `func1 0x01(INTR_PENDING)` 同时非 0。
   → 判定手段：① 探针已打印 `ioex`/`ipend`（CCCR 0x04/0x05）与 func1 `0x01` 各 8 次，看是否与 D1 低电平同步；
     ② 把 `AIC_SDIO_IRQ_MASK_TEST` 置 1 做 A/B（每轮清 CCCR IOEx=IENM/IEN1 关掉卡的信号能力），
        若 D1 从此不再下拉 ⇒ 就是中断(b)；若照旧 ⇒ 是耦合(a)/其它。
   → 无论 (a)/(b)，**D1 与 `0x03` 恒 0 都没有因果关系**（CMD52 的读数据在 R5、走 CMD 线）。
   （硬件上仍建议给 CMD/DAT0~3 加上拉；若属 (b)，那说明芯片在"举手"，本身是重要线索。）

---

## 6. 当前仓库里的诊断改动（都在 `example/aic8800_d80`，可随时回退）

| 文件 | 内容 | 状态 / 如何关闭 |
|---|---|---|
| `src/main.c` | `AIC_SDIO_REG_PROBE`（默认 1）：一条独立于驱动的 CMD52 探针，直接引用 `platform/hal/sdio_port.c` 的全局 `sdio_function_local[]`；从 0.5s 起每 500ms 采 20 次，打印 func0 CCCR `0x00/0x04(IOEx)/0x05(INT_PENDING)` + FBR `0x110/0x111`、func1 的 `0x07` 写-读回、`0x03/0x01/0x04` 各 8 次连读，以及 `0x0A`（V2 映射指纹）；导出 msh 命令 `aic_sdio_reg_probe` | **保留**；`#define AIC_SDIO_REG_PROBE 0` 可关闭 |
| `src/main.c` | `AIC_SDIO_IRQ_MASK_TEST`（默认 0）：置 1 时每轮把 CCCR `0x04`(IOEx: IENM/IEN1) 清 0，禁止卡发中断信号 → 判定 D1 的下拉是否就是"芯片拉中断" | 改宏值 |
| `project/proj.conf` | `CONFIG_SDMMC2_BUS_WIDTH_1_ONLY=y` — **单线(1-bit)模式，已打开**：`drv_sdhci.c:1598` 不置 `MMCSD_BUSWIDTH_4`，`sdio_set_bus_wide()` 跳过（不写 CCCR 0x07），卡保持 1-bit，CMD53 数据只走 DAT0，**D1/D2/D3 退出数据通路** | 注释该行即回 4-bit |
| `src/main.c` | ~~`AIC_SDIO_D1_ISOLATE` / `AIC_SDIO_D1_BIAS`（PA49→GPIO 隔离 + D1 电平采样）~~ | **已按要求删除**（改为用 1-bit 模式隔离，见上一行） |

编译/烧录环境：`export.ps1` 初始化后，在 `example/aic8800_d80/project` 下
`scons --board=sf32lb55-lcd_n16r4n4_hcpu -j8`（工具链 `arm-none-eabi-gcc 14.2.1`）。
注意 GCC 14 把若干警告升为 error（已踩过：`sdio_io_readb` 第 3 形参是 `rt_int32_t*`）。

---

## 7. 复现与数据采集清单（交接后先做这一步）

1. 编译烧录当前版本（探针 + **1-bit 单线模式已打开**；PA49 GPIO 隔离代码已删除），抓完整两路串口日志。
2. 期望在主机日志里看到周期性：
   ```
   [AICPROBE] f0 rev=.. blk=.... ioex=.. ipend=..(CCCR中断状态) | mask_test=0
              f1 wr07=01 rd07=.. 00=.. 0A=..
              03: xx xx xx xx xx xx xx xx
              01: ...      04: ...
   ```
   （前 1~2 次会打 `[AICPROBE] func not ready`，属正常。）
3. 判读表：

| 观察 | 结论 |
|---|---|
| `f0 rev`≠0 且 `rd07=01` | CMD52 / func1 寄存器空间都正常 |
| `03:` 8 个值全 0 且一致 | 芯片确实不给 buf（ROM 状态/协议），与 D1、电平余量无关 |
| `03:` 值在 `00/01/02` 间跳变 | 采样/电平余量不足（支持厂商的 2.6V 判断） |
| `0A:` 非 0 而 `03:` 恒 0 | ROM 用的是 **V2 寄存器映射** |
| `rd07` 读不回 `01` | func1 寄存器空间写不进/读不回 → func1 使能或时序问题 |
| `ioex` 的 bit0(=`IENM`) 为 1 | 卡被允许发中断 → DAT1(D1) 可能就是在拉中断 |
| `ipend`(`CCCR 0x05`) 或 func1 `01:` 非 0，且与 D1 低电平同步 | **D1 是芯片在拉 SDIO 中断**（见 §4.7-b）；把 `AIC_SDIO_IRQ_MASK_TEST=1` 复核 |
| `mask_test=1` 后 D1 不再下拉 | 确认是中断；此时应顺着"芯片为何一直中断"排查（挂起位是哪个 function） |
| `mask_test=1` 后 D1 照旧下拉 | 不是中断 → 回到耦合/悬空(加外接上拉复测) |
4. 1-bit 与 4-bit 对比（只改 `proj.conf` 一行）：若 `03:` 在 1-bit 下变化 → 与总线宽度/D1 相关；若完全不变 → 与总线宽度无关。
5. 另需确认：`GPIO124(WL_REG_ON)` 在 `wifi_drive_op:1` 时是否真的产生复位（是否出现第二次 bootrom banner）。

---

## 8. 待问厂商的问题

1. AIC8800D80N/`0x9082` 的 **boot ROM 寄存器映射**是否与 D80 一致（flow-ctrl 到底在 func1 `0x03` 还是 `0x0A`）？
2. ROM 阶段是否要求先做 **唤醒/就绪握手**（写 `0x02`=`0x11`、读 `0x01` bit4），还是要求 **1-bit** 才放开 cfg 通道？
3. `D80N 主 FMAC 复用 D80`（`aic_fw_port.c:1367`）在 innermode 下是否真的不需要主 FMAC？
   `aicwf_rwnx_load_firmware()` 的 D80N 分支引用 `FMACFW_8800D80_U02`（只在 `CONFIG_AIC8800D80` 下存在）
   是否有意为之？
4. D80N 的 SDIO IO 电平/上拉要求（规格书推荐值），是否认可 2.6V + 内部上拉。

---

## 9. 关键文件 / 符号索引

**平台与例程**
- `src/main.c` — `AIC_SDIO_REG_PROBE`（CMD52 寄存器探针，**保留**）+ `AIC_SDIO_IRQ_MASK_TEST`（中断屏蔽 A/B，默认 0）；PA49 GPIO 隔离代码已删除
- `project/proj.conf` — `CONFIG_SDMMC2_BUS_WIDTH_1_ONLY=y`（**已打开 = 单线 1-bit 模式**，用于把 D1/D2/D3 移出数据通路）
- `$SDK/customer/peripherals/wifi/aic8800_rtos_driver/platform/hal/sdio_port.c`
  - `995-1049` `__aicwf_sdio_set_block_size()`（func0 写-读回，CMD52 通路证据）
  - `1215-1316` `aicwf_sdiov3_func_init()`（V3 寄存器初始化；`1292-1313` 的唤醒握手是 `#if 0`）
  - `1337-1368` `aicwf_sdio_chipmatch()`（`0x9082` → `PRODUCT_ID_AIC8800D80N` = 4）
  - `713-731` `aicwf_sdio_readb_cmd52_by_num()`（chip_status=0 时静默返回 false，是个坑）
- `.../platform/fmac/aic_fw_port.c` — 各型号固件数组选择（D80N 分支：`94/263/323/474/534/1360/1481`）
- `.../platform/fmac/aic_chip_desc_port.c` — 运行时型号差异表（**全型号无条件**，含 `sdio_fc_reg_mask`、`sdio_wakeup_val`）
- `.../lib/include/drv_api/inc/sdio_def.h` — `SDIOWIFI_FLOW_CTRL_REG=0x0A`(V2) / `FLOW_CTRL_Q1=0x03`(V3)、`FLOW_CTRL_RETRY_COUNT=50`

**SDK 侧**
- `$SDK/rtos/rtthread/bsp/sifli/drivers/drv_sdhci.c:1590-1600` — 4-bit 开关（`SDMMC2_BUS_WIDTH_1_ONLY`）
- `$SDK/rtos/rtthread/components/drivers/sdio/sdio.c:755-778` — `sdio_set_bus_wide()`
- `$SDK/customer/boards/sf32lb55-lcd_base/bsp_pinmux.c:134-139` — SD2 六线 pinmux
- `$SDK/customer/boards/sf32lb55-lcd_base/bsp_init.c:583-591` — `BSP_QSPI_PowerUp()` 会重设 PA49 复用

**逆向用产物（构建目录内，符号名稳定、地址随构建变）**
- `project/build_sf32lb55-lcd_n16r4n4_hcpu/main.asm` / `main.map`（预编译库的反汇编/符号表）
  - `aicwf_sdio_flow_ctrl_msg`（打印 `msg fc:%d`）、`aicwf_sdio_tx_msg`
  - `rwnx_fdrv_init`、`rwnx_platform_on`（chipid==4 的固件加载分支）
  - `aicwf_plat_cinit_exec_8800d80n`（按 `chip_sub_id` 取 id 60 / 64，取不到退回 60）
  - `aicwf_rwnx_load_firmware`（platform，D80N 分支取 `FMACFW_8800D80_U02`）

**AIC 固件 enum 对照**（`port/inc/aic_fw_port.h`）
`58=FMACFW_PATCH_8800D80N_U02, 59=..._PATCH_TBL_U02, 60=..._CINIT_U02, 61=..._CALIB_U02,
 62/63/64/65=对应 _U02C`；`50/51=LMACFW_RF_8800D80N[_I]`（RFTEST）。

---

## 10. 一句话结论

SDIO 链路与 CMD52 通路都正常，卡点是 **boot ROM 拒绝提供 cfg 通道缓冲（func1 `0x03` 恒 0）**，
导致第一条 dbg 消息发不出去；芯片侧停在 `fn1_ioen`。

- **D1 处置：已切到单线(1-bit)模式**（`proj.conf`），D1/D2/D3 退出数据通路（CMD53 只走 DAT0）。
  但要记住：**DAT1 在 1-bit/4-bit 下都是 SDIO 中断线**，所以 D1 若仍被规律性拉低，
  要么是芯片在拉中断（看 `CCCR 0x05 ipend` / func1 `0x01`，或用 `AIC_SDIO_IRQ_MASK_TEST=1` 验证），
  要么是悬空耦合（`PIN_NOPULL` + 无外接上拉）。这两者都与 `0x03` 恒 0 无因果关系。
- **剩余怀疑**集中在 ROM 侧：① 寄存器映射（V2 `0x0A` vs V3 `0x03`）；
  ② ROM 阶段是否需要额外唤醒/就绪握手（`sdio_port.c:1292-1313` 被 `#if 0`）；
  ③ 芯片是否真的被 `WL_REG_ON` 复位过（ROM banner 只出现一次）；
  ④ 芯片是否有挂起中断一直没被主机处理（若是，说明 ROM 在等主机做某事）。
- 现场留着 **`AIC_SDIO_REG_PROBE`**（func0 CCCR/FBR + func1 读回 + `0x03/0x01/0x04` 8 次连读 + `0x0A` 指纹
  + CCCR `ioex/ipend`），跑一轮即可区分"真读数 0"、“映射/使能问题”与“芯片在拉中断”。
