# AIC8800D80N SDIO WiFi Bring-up —— 交接文档（当前问题汇总）

> **给接手的 agent/同事**：这份文档是自包含的，不需要之前的对话上下文。
> 详细的问题编年史（P1~P6）见同目录 `AIC8800D80N_问题汇总.md`；**本文第 4 节修正了那份文档里对"当前卡点"的描述**，以本文为准。

---

## 0. 交接摘要（30 秒版）

| 项 | 内容 |
| --- | --- |
| **现状** | SDIO 全链路（枚举 / 读 CIS / 驱动匹配 / func1 配置 / 中断）**全部成功**；但 **WiFi 初始化未完成**，`GetWiFiInitStat()` 恒为 0 |
| **唯一卡点** | 主机要给芯片发**第一条消息**时，芯片从不发放"消息流控额度"（function1 偏移 `0x03` = `FLOW_CTRL_Q1`，连续 **556 次读到 0**）→ 消息事务超时 → 驱动放弃并收尾 |
| **后果** | 芯片固件（CINIT/patch/fmac）**一个字节都没下载**（全日志 **0 次 CMD53**），芯片仍停在 bootrom |
| **责任方** | **芯片/模组侧**（主机侧动作已逐条核对无误，见第 5 节） |
| **下一步** | ① 做"芯片探活"实验（本文 6.1，尚未做）② 问原厂 4 个问题（6.2）③ 硬件核对 4 项（6.3） |
| **硬约束** | ⚠️ 只允许改 **WiFi 驱动**（`customer/peripherals/wifi/aic8800_rtos_driver/**`，另含一处已做的 RT-Thread SDIO 核心修复）⚠️ **不要动 ulog / cpuport** ⚠️ **不要自动烧录**（烧录需人工置 boot 模式） |

---

## 1. 环境与基线

| 项 | 值 |
| --- | --- |
| 芯片 | AIC8800**D80LN**，SDIO CIS 报 VID `0xC8A1` / PID `0x9082`；驱动运行期判定 **D80N 系**（`chipid=4`） |
| 主控 / 板 | SiFli SF32LB55X，`sf32lb55-lcd_n16r4n4` |
| 工程（工作区） | `D:\SiFli\SDK\SiFli_SDK_main\SiFli-SDK\example\rt_device\aic8800_d80` |
| 驱动 | `customer/peripherals/wifi/aic8800_rtos_driver`（platform 层源码 + 预编译 host 库 `lib/libaic8800_driver.a`，版本串 `NV1.1.6 STA AP P2P ggd4509b7 20260915`） |
| SDK 基线 | git HEAD `ecc29fc18`（2026-10-09 16:01 "[bug][wifi] Fix AIC8800 WiFi driver issues"） |
| 编译 | `& 'G:\Code\VScode\SF32_Code\setup_sifli.ps1'` 然后 `cd project; scons --board=sf32lb55-lcd_n16r4n4 -j8` |
| 产物 | `project/build_sf32lb55-lcd_n16r4n4_hcpu/main.bin`（1,568,488 B，距 ROM 上限仅 ~4.4 KB） |
| 日志目录 | `G:\Code\VScode\SF32_Code\SiFli_demos\test_log\` |
| 当前状态 | **可编译、可运行、不再死机**，稳定停在"芯片消息通道"这一步 |

**日志文件对照（按时间顺序）**

| 文件 | 内容 | 结论 |
| --- | --- | --- |
| `1.txt` | 修改前：`init_sdio ret=0` 后总线被断电，CMD52 全超时（`timeout 0x00000000`） | 已修（见 3.1） |
| `2.txt` / `3.txt` | 死机：HardFault + ulog 重入断言（`[ULOG-REENT] ... hold=2`） | 已修（见 3.2） |
| `4.txt` | 栈改 4096 后不再死机，稳定停在 msg fc 轮询 | 现症状首次出现 |
| `5.txt` | 补 `CONFIG_AIC8800D80N` 后现象不变（排除该疑点） | 见 3.4 |
| **`log.txt`**（= 原 `6.txt`） | **带轨迹打印的最新版；本文所有结论以此为准** | 见第 4 节 |

---

## 2. 五分钟上手：这条链长什么样

```
应用 (src/main.c) ─► AIC platform 层（本仓库源码，可改）
                      ├─ 电源脚 GPIO124、SDIO 寄存器读写封装、固件数组、任务/栈
                      ▼
                   预编译 host 库 libaic8800_driver.a（不可改，只有符号）
                      ├─ 与芯片的协议栈、消息通道、固件下载流程
                      ▼
                   RT-Thread SDIO/MMCSD 核心（SDK） ─► SiFli SDHCI 驱动 ─► 硬件
```

**必须分清的四件事**（本次排查就是靠这个划分避免走偏）：

| # | 名称 | 看 ID 吗 | 失败现象 | 责任层 |
| --- | --- | --- | --- | --- |
| ① | **寻卡/枚举**（CMD0/5/3/7 + CCCR/FBR/CIS） | ❌ 不看 | 没有 `card registered`；CMD 超时 | 主控（已通过） |
| ② | **驱动匹配**（拿 VID/PID 比 ID 表 → probe） | ✅ | 有 `card registered` 但没有 probe 打印，`sdio_register_driver` 返回 `-4` | 主控（已通过） |
| ③ | **消息握手**（主机↔芯片 bootrom 的消息通道） | — | **CMD52 有响应但数据恒为 0**，消息事务超时 | **芯片侧 ← 当前卡点** |
| ④ | **固件下载**（CINIT/patch/fmac，CMD53） | — | 0 次 CMD53 | 依赖 ③ |

**术语**：CCCR=卡级控制寄存器（`0x02` I/O Enable、`0x04` 中断使能）；FBR=功能寄存器（func1 从 `0x100` 起）；CIS=卡的身份证（`0x20`=厂商/产品 ID）；func0=公共控制，func1=WiFi 数据/消息通道；`msg fc`=消息流控额度（D80 系在 func1 偏移 `0x03`，**由芯片写入**）。

---

## 3. 已经修掉的问题（不要重复排查）

### 3.1 SDIO 枚举成功后总线被无条件断电（最关键的一处）
- **现象**：`init_sdio ret=0` 之后紧接 `clock 0Hz … powermode 0 … Vdd 0`；随后第一条 CMD52 就 `wait 52 completed timeout 0x00000000`（中断状态一位都没置 = 控制器发不出命令），enable function / 块大小全失败。
- **位置/修改**：`rtos/rtthread/components/drivers/sdio/mmcsd_core.c:670-674`
  ```c
  rt_int32_t ret = init_sdio(host, ocr);
  LOG_I("init_sdio ret=%d", ret);
  if (ret)                      /* 原来是无条件 mmcsd_power_off(host); */
      mmcsd_power_off(host);
  ```
- **注**：该文件属 RT-Thread SDIO 核心（不属 WiFi 驱动目录），但**任何在探测线程之外访问总线的驱动都会踩**，故保留修复。

### 3.2 WiFi 初始化线程栈溢出 → HardFault
- **证据**：线程表 `aicwf_wi … stack size 0x00000400 max used 92%`（`aicwf_wi` = `aicwf_wifi_init_task`，1024 B 栈，最深只剩 82 B）；故障点紧跟 `rwnx_timer_task` 创建之后。
- **修改**：`port/inc/plat_port.h:104` `RWNX_WIFI_STACK_SIZE (1024) → (4096)`。
- **验证**：`4.txt`／`5.txt`／`log.txt` 均运行 40~90 秒无任何死机。

### 3.3 第一次 HardFault 的现场被 ulog 重入断言吞掉（已加观测，未改核心）
- **现象**：只有 `[ULOG-REENT] vectactive=3 … hold=2 th=aicwf_wi owner=aicwf_wi` + `Assertion failed at function:output_lock, line number:297`，看不到故障寄存器。
- **原理**：故障处理函数自己也走 ulog，而被打断的线程正持锁 → 递归持锁 → 撞断言。
- **修改**：`platform/platform_init.c:85` 新增 `aicwf_fault_hook()`（读 `CFSR/HFSR/MMFAR/BFAR/ICSR` + 异常帧 `r0-r3/r12/lr/pc/psr`，**用 `rt_device_write()` 直写串口绕开 ulog**），`:145` 在 `aic_platform_init()` 里 `rt_hw_exception_install()` 注册。
- **依据**：`rtos/rtthread/libcpu/arm/cortex-m33/cpuport.c:475-481`（钩子在核心打印之前调用，返回 `RT_EOK` 可跳过打印）。

### 3.4 编译宏缺 `CONFIG_AIC8800D80N`（已修，但**不是**当前卡点原因）
- **问题**：运行期 `chipid=4`（D80N），但构建只定义 `CONFIG_AIC8800D80` → `aic_fw_port.c:94-100` 的 D80N 固件数组不编、`:1504-1520` 的 D80N 分支体被编译掉（取固件静默失败）。
- **修改**：`SConscript:64` 增加 `'CONFIG_AIC8800D80N'`（**必须与 `CONFIG_AIC8800D80` 并存**：D80N 主 FMAC 复用 D80，见 `aic_fw_port.c:1516`）。镜像新增 4 个数组 ≈71 KB（`nm main.elf` 可验证）。
- **副作用已处理**：ROM 溢出 19,716 B → 工程侧 `proj.conf` 关闭 **iperf** 与 **lwIP debug**（可随时开回）。
- **重要**：加上之后日志**完全没变**（仍 0 次 CMD53）→ 该问题真实但**不解释当前卡点**。

### 3.5 SDIO 匹配链加固（防将来再踩）
- `sdio_port.c:155-196`：ID 表"硬约定"注释（本平台 core 只读第一条、第一条必须 `SDIO_ANY_PROD_ID`）+ 型号宏全缺失时 `#error`。
- `sdio_port.c:255`（probe 卡级信息打印）、`:267`（`sdio_function[i]` 空指针防御）、`:331-352`（注册返回值检查与定位指引；**禁止循环重试**，因为 `sdio_register_driver()` 每次调用都会插一个新节点）。

---

## 4. 【核心】当前唯一卡点：现象 ≠ 原因（务必按此理解）

### 4.1 先排除一个常见误判

> ❌ "CMD52 发了很多次，导致流程终止"
> ✅ 事实：那些 CMD52 是主机**反复询问**芯片"有没有消息额度"，**每条都有响应、都 `Wait cmd 52 done`**（数据 `0x00`）。它是**症状**，不是原因。

### 4.2 真实因果链（`log.txt` 原文，被 `drv.sdhci` 调试刷屏埋掉了）

```
[AIC][SDIO]msg fc:0                                            ← 读 func1 0x03 = 0（重复 11 次）
[AIC][SDIO]tx msg fc retry fail                                ← 主机放弃发这条消息
[AIC][MACIF]cmd_mgr sema wait err or timer out:400,401         ← 命令 400/401 等信号量超时
[AIC][MACIF]cmd res:-3, 400,401                                ← 返回 -3 = RT_ETIMEOUT
[AIC][MACIF]40500000 rd fail: -3                               ← 读芯片寄存器 0x40500000 的消息事务失败
[AIC][MACIF]system config fail                                 ← 与 D40 参考源码 `rwnx_main.c:908` 同一句
[AIC][MACIF]err_platon                                         ← `goto err_platon`（D40 参考同名标签）
[AIC][FHOST]aic_wifi_open:fdrv fail
[AIC][FHOST]aic_wifi_close wait task suspend … remove task      ← 收尾
[AIC][SDIO]aicwf_sdio_host_deinit / SDIO: disabling IRQ for function 1
[AIC][FHOST]wifi_open fail, ret=-1
[AIC][APP]wifi_drive_op: 0
```

**精确定位**：
- **停在哪个函数**：host 库 `rwnx_fdrv_init()` 里的 **`system_config_8800d80n()`**（它第一步要"发消息读芯片寄存器 `0x40500000`"）；
- **停在哪个点**：**消息事务超时**（`cmd_mgr` 400/401 等不到确认）；
- **为什么**：消息发不出去 ← `msg fc`（func1 偏移 `0x03`）**连续 556 次读到 0**；
- **之后不是卡死**：驱动走完失败清理（关中断、删 6 个任务），`aic_wifi_open` 返回 `-1`；`log.txt` 末尾的 `GetWiFiInitStat->>0` 只是**应用在空转**（`wifi_port_plt.c:78-83` 的轮询；`wifi_init_status` 已不可能置 1）。

时间线：`17:31:50.272` 上电 → `~17:31:51.6` SDIO 配置完成 → `~17:31:51.7` 开始轮询 fc → `17:32:43` 放弃收尾（≈53 秒）。

### 4.3 为什么"绕过握手直接烧固件"不可行

用 D40 完整源码（同协议族参考实现）核对：**配置、下载、启动全都是消息事务**。

| 步骤 | 实现 | 机制 |
| --- | --- | --- |
| 配芯片寄存器 | `system_config_8800d80n()` → 读/写芯片寄存器 | **消息**（`rwnx_send_dbg_mem_read_req` 等）← 当前卡在这 |
| 下载固件 | `aic8800d40/fdrv/macif/rwnx_platform.c:177-213` `rwnx_plat_bin_fw_upload_2()`：每 1 KB 调 `rwnx_send_dbg_mem_block_write_req()` | **消息** |
| 启动固件 | `aic8800d40/fdrv/macif/rwnx_main.c:715-745` `start_from_bootrom()`：`rwnx_send_dbg_mem_read_req()` + `rwnx_send_dbg_start_app_req()` | **消息** |

我们的预编译库里同样含这些符号（`rwnx_send_dbg_mem_block_write_req` / `rwnx_send_dbg_start_app_req` / …），机制一致。
**结论**：`msg fc` 是"芯片 bootrom 活着且愿意对话"的唯一入口；即便在 port 侧直接 `extern` 调库的 `rwnx_platform_on(g_rwnx_hw)` 跳过 `system_config`，其内部第一步 `rwnx_plat_fmac_load()` → `rwnx_plat_bin_fw_upload_2()` 依然要走消息 → 依然卡在同一处。**没有裸寄存器/裸 FIFO 的固件下载旁路。**

### 4.4 一句话结论

**主机侧动作已全部正确，问题是芯片侧不发放消息额度——即"芯片不开口"，不是主机代码能绕过去的。**

---

## 5. 已经验证过的事实（新 agent 不必重做）

1. **主机侧寄存器访问全序列与参考实现逐条一致**（`log.txt` 中 `demo start` 之后全部 CMD52，共 12 类）：
   `R/W fn0 0x02`（I/O Enable）、`R/W fn0 0x04`（中断使能 0→3）、`W fn0 0xF2←0x7F`、`R/W fn0 0x110/0x111`（FBR1 块大小→512 B，含回读校验）、`W fn1 0x00←0x07`（`INTR_ENABLE`，V3 映射 `0x00`）、`W fn1 0x07←0x01`（`BYTEMODE_ENABLE`）、`R fn1 0x03 ×556`（流控，恒 0）。
   对照 `aic8800d40/fdrv/layer_common/src/sdio_co.c:51-78`（V3 寄存器映射）、`platform/hal/sdio_port.c:692-790`（V3 初始化）、`fdrv/layer_abstract/inc/sdio_def.h:35-45`（地址常量）：**无遗漏、无错写**。
2. **轨迹打印结果**（`sdio_port.c:575-645` CMD53 / `:818,:840` CMD52 / `aic_fw_port.c:1521,1560,1594` 固件载入器）：
   全日志仅 **2 条** `[trace]`（都是初始化末尾的寄存器写）、**0 次 CMD53**、**0 次 `load_firmware/load_patch_tbl/load_calib`** → 库从未取固件、从未搬运数据。
3. **匹配链成功**：`card registered: man=0xc8a1 prod=0x9082 f1prod=0x0000 funcs=1` → `init_sdio ret=0` → probe `man=0xC8A1 prod=0x9082 funcs=1 cccr_ver=4` → `aicwf_sdio_drv_register():ret:0`。
   （`f1prod=0` 正常：func1 的 CIS 只有 `CISTPL_FUNCE(0x21)`，没有 `CISTPL_MANFID(0x20)`，**不能用来判型号**。）
4. **"最后一条命令"是正常完成的**：`Send cmd 52 / ARG:0x80000800`（func0 CCCR `0x04` 写 0 = 关中断收尾）→ `Resp: 0x00001000` → `Wait cmd 52 done`。**不存在 SDHCI 超时**（那是 `1.txt` 的早期现象，已修）。
5. 曾怀疑但已排除：厂商 ID 不匹配（实际匹配成功）、CMD52 无响应、SDHCI 中断/命令等待、`GetWiFiInitStat` 能证明成功（它只是应用轮询）。

---

## 6. 下一步（建议按顺序）

### 6.1 芯片"探活"实验（尚未做，成本几分钟，能砍掉一半可能性）
在 port 侧加**一次性**（非周期）读取并打印：
`fn1 0x01`(INTR_PENDING)、`fn1 0x04`(MISC_INT_STATUS)、`fn1 0x09`(FLOW_CTRL_Q2)、`fn1 0x0A`(CLK_TEST_RESULT)、`fn0 0x00/0x08`。
- 全 0 且永不变 → 芯片数字核/bootrom **没有在跑** → 直奔硬件（复位脚、启动模式 strap、供电模式）；
- 有非 0 或会变化 → 芯片是活的 → 问题在消息通道配置/握手 → 把 4.2 的失败链 + 5.1 的序列表发原厂。

### 6.2 问原厂（AIC）——可直接复制
> 平台：SiFli SF32LB55X，SDIO 主控；芯片 AIC8800D80LN（SDIO CIS 报 PID `0x9082`），host 库 `NV1.1.6 … 20260915`。
> 现象：SDIO 枚举/匹配/func1 配置全部成功；主机在 `rwnx_fdrv_init → system_config_8800d80n()` 第一步（发消息读芯片寄存器 `0x40500000`）失败：
> `cmd_mgr sema wait err or timer out:400,401` → `cmd res:-3` → `40500000 rd fail: -3` → `system config fail` → `wifi_open fail, ret=-1`。
> 直接原因是消息发不出去：`FLOW_CTRL_Q1`（function1 偏移 `0x03`）连续 556 次读到 `0x00`，库打印 `tx msg fc retry fail`。
> 因此全程 **0 次 CMD53**，固件（CINIT/patch/fmac）从未下载。
> **请确认：**
> 1. D80LN 在 bootrom 阶段，`FLOW_CTRL_Q1`（func1 `0x03`）由谁、在什么条件下置为非 0？
> 2. 主机是否需要先写某个寄存器才开放消息通道（如 `INTR_TO_DEVICE` fn1 `0x02`、`MISC_CTRL` fn1 `0x08`、func0 `0xF0~0xF8`）？我们当前的完整寄存器序列可提供逐条对照。
> 3. bootrom 阶段是否需要先做唤醒/复位释放？
> 4. 本库版本是否正式支持 PID `0x9082`？若有 D80N bootrom 阶段的操作序列文档请提供。

### 6.3 硬件 / 模组核对
1. 除 GPIO124（`CONFIG_AIC8800_RTOS_WIRELESS_PWR_GPIO=124`）外，模组是否还有 **RESET_N / CHIP_EN / 启动模式 strap** 需特定电平？
2. 模组供电是 **DCDC 还是 LDO**？（厂商文档提到 `aicwifi_vrf_mode`）
3. 模组是否**自带 flash 且已烧固件**？（影响芯片启动路径）
4. 上电时序是否满足手册（当前：拉低 10 ms → 拉高 → 等 200 ms，之后才开始枚举）。

---

## 7. 改动清单与回退

**回退补丁**：`example/rt_device/aic8800_d80/dsh-driver-changes.patch`
`19,369 B`，7 个文件，`+188/-7`，SHA256 `0624AE81D7679DBE0506F1B4A3A0BD8C3124555CD633F10A5ECA107496E3D260`
（注：轨迹打印与 `proj.conf` 之后又各改过一次，若需最新全量补丁请重新 `git diff --output=` 生成。）

| 文件 | 位置 | 内容 | 回退影响 |
| --- | --- | --- | --- |
| `rtos/.../drivers/sdio/mmcsd_core.c` | `:670-674` | SDIO 成功后不再无条件断电 | ⚠️ 回退会重现 `1.txt` 的 CMD52 全超时 |
| `.../aic8800_rtos_driver/port/inc/plat_port.h` | `:104` | `RWNX_WIFI_STACK_SIZE 1024→4096` | ⚠️ 回退会重现死机 |
| `.../platform/platform_init.c` | `:85`, `:145` | 故障现场钩子 + 注册 | 只是少一份保险 |
| `.../platform/hal/sdio_port.c` | `:155-196`, `:255`, `:267`, `:331-352` | 匹配链加固 + 打印 | 无功能影响 |
| 同上 | `:575-645`, `:818`, `:840` | CMD53/CMD52 轨迹打印 | 无功能影响 |
| `.../SConscript` | `:64` | `+CONFIG_AIC8800D80N` | ⚠️ 回退则 D80N patch/cinit/calib 不再编入 |
| `.../platform/fmac/aic_fw_port.c` | `:1521`, `:1560`, `:1594` | 固件载入器轨迹 | 无功能影响 |
| `example/.../aic8800_d80/project/proj.conf` | — | 关 iperf + lwIP debug（腾 ROM ≈ 20 KB） | 可随时开回 |

```powershell
# 全部回退
git -C D:\SiFli\SDK\SiFli_SDK_main\SiFli-SDK checkout -- `
  rtos/rtthread/components/drivers/sdio/mmcsd_core.c `
  customer/peripherals/wifi/aic8800_rtos_driver/SConscript `
  customer/peripherals/wifi/aic8800_rtos_driver/port/inc/plat_port.h `
  customer/peripherals/wifi/aic8800_rtos_driver/platform/platform_init.c `
  customer/peripherals/wifi/aic8800_rtos_driver/platform/hal/sdio_port.c `
  customer/peripherals/wifi/aic8800_rtos_driver/platform/fmac/aic_fw_port.c `
  example/rt_device/aic8800_d80/project/proj.conf

# 恢复
git -C D:\SiFli\SDK\SiFli_SDK_main\SiFli-SDK apply `
  D:\SiFli\SDK\SiFli_SDK_main\SiFli-SDK\example\rt_device\aic8800_d80\dsh-driver-changes.patch
```
> ⚠️ **不要**用 `git checkout -- .` / `git reset --hard`（仓库里有大量构建产物 ` M` 与其它人的本地改动）。

---

## 8. 附录：常用命令 / 关键数字

```powershell
# 只看有效日志行（滤掉 drv.sdhci 调试与 GetWiFiInitStat 刷屏）
Select-String -Path <日志> -Pattern '\[AIC\]|\[trace\]|msg fc|AIC-FAULT|Assertion' |
  Where-Object { $_.Line -notmatch 'GetWiFiInitStat|drv\.sdhci' }
# 确认 D80N 固件数组已进镜像
& 'D:\SiFli\Toolain\tools\arm-none-eabi-gcc\14.2.1\bin\arm-none-eabi-nm.exe' --print-size --size-sort main.elf | Select-String 'fmacfw'
```

`log.txt` 关键计数：总行数 6397；`Send cmd` 665 次；`[trace]` 2 条；`CMD53` 0 次；`load_firmware` 0 次；`msg fc` 12 次；`tx msg fc retry fail` 1 次；`Resp: 0x00001000` 599 次（多为流控轮询）。
时间跨度：`17:31:50.272 → 17:32:43.470`（≈53 s，其中绝大部分是流控重试与收尾）。
