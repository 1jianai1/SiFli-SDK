# AIC8800D80N(0x9082) SDIO 初始化失败 —— 定位结论 + 临时规避 + 待厂商确认项

> 提交目的：这是我们在 **SDK 侧**做的一处**临时规避**，现象已可 100% 复现、原因已定位到 **host 库内的一处硬编码**。
> 我们只提供思路与证据，希望贵司给出**官方修改**（库或明确的适配指引）。

---

## 1. 环境

| 项 | 值 |
|---|---|
| 主控 | SiFli SF32LB55X（board `sf32lb55-lcd_n16r4n4`，SDHCI2） |
| SDIO | 4-bit，时钟 24 MHz（1-bit 下现象完全相同） |
| 模组 | AIC8800D80N；卡级 `VID=0xC8A1, PID=0x9082` → 运行时 `chipid=4`（`PRODUCT_ID_AIC8800D80N`） |
| host 库 | `libaic8800_driver.a`，release **NV1.1.6 STA AP P2P ggd4509b7 20260915** |
| platform 层 | `customer/peripherals/wifi/aic8800_rtos_driver/platform/*`（SDK 编译） |
| 固件套件 | `chip_sub_id=1` → D80N(U02) 的 CINIT / PATCH / PATCH_TBL / CALIB |

## 2. 现象（修改前，100% 复现）

```
[AIC][SDIO]msg fc:0                                  ← 反复出现，每次约 2.1s，共 11 次（约 23s）
[AIC][SDIO]tx msg fc retry fail
[AIC][MACIF]cmd_mgr sema wait err or timer out:400,401
[AIC][MACIF]40500000 rd fail: -3                     ← 第一条 cfg（读芯片 ID）发不出去
[AIC][MACIF]system config fail
[AIC][FHOST]aic_wifi_open:fdrv fail
[AIC][FHOST]wifi_open fail, ret=-1                    ← WiFi 初始化彻底失败
```
芯片侧串口日志停在 `fn1_ioen`，之后再无输出。

## 3. 定位过程与证据

**(1) 库里的赋值被硬编码为 Q1。** 反汇编 `libaic8800_driver.a` 的 `aicwf_sdio_reg_init()`（`r4 = sdiodev`）：

```
chipid = [r5, #764];  cmp #2;  bhi → V3 分支
V3 分支 (chipid 3/4/5):
  str   0x02010005 → [r4,#4]        ; bytemode_len=05 intr_config=00 sleep=01 wakeup=02
  movs  r3, #3
  strb  r3 → [r4,#8]                ; ★ sdio_reg.flow_ctrl_reg = 0x03 (V3 FLOW_CTRL_Q1) —— 硬编码
  movs  r3, #7  ; strb → [r4,#11]   ; bytemode_enable = 0x07
  movs  r3, #4  ; strb → [r4,#13]   ; misc_int_status = 0x04
  movw  #0x100f → [r4,#14]          ; rd/wr fifo = 0x0F/0x10
```
`SDIOWIFI_FLOW_CTRL_Q2_REG_V3` 在整个库里**没有任何引用**（只在头文件里有 `#define`）。
我们不是走了别的芯片分支：分支条件是**运行时 chipid > 2**，本芯片 chipid=4 已进 V3 分支（其它寄存器值也印证：`0x00/0x07/0x13…` 全是 V3 布局）。

**(2) 芯片侧实测：只有 Q2 有信用。** 用独立 CMD52 探针实时读取 func1：

| 地址 | 含义 | 实测 |
|---|---|---|
| `0x00` | INTR_ENABLE | `0x07` |
| `0x01` | INTR_PENDING | `0x10`（bit4 = ready；bit0 = 0） |
| `0x03` | **FLOW_CTRL_Q1** | **恒 `0x00`** |
| `0x04` | MISC_INT_STATUS | `0x00` |
| `0x07` | BYTEMODE_ENABLE | `0x01` |
| `0x09` | **FLOW_CTRL_Q2** | **`0x02`** ← 唯一有 TX 信用的地方 |

**(3) 库读 flow-ctrl 的路径。** 反汇编 `aicwf_sdio_flow_ctrl_msg()`：

```
ldrb r0, [r6, #8]                    ; r6 = &sdio_dev → r0 = sdio_reg.flow_ctrl_reg (= 0x03)
bl   aicwf_sdio_readb_cmd52          ; ← platform 提供的 func1 CMD52 读函数
```

**(4) 附加发现：Q1/Q2 的访问方式会显著影响芯片行为。**
我们用四种写法做 A/B（同一固件、同一板子，每次 flow-ctrl 读的**实际总线序列**与结果如下）：

| 写法 | 每次 flow-ctrl 读的总线序列 | 结果 |
|---|---|---|
| A. 不做任何处理（库原样） | `读 0x03` | ❌ 恒 0 → `msg fc:0` → 初始化失败（原始故障） |
| B. 直接重定向到 Q2 | `读 0x09` | ❌ 仍拿不到信用，失败 |
| C. **每次**都先哑读 Q1 再读 Q2 | `读 0x03` → `读 0x09` | ⚠️ 最终能用，但**芯片侧持续打印** `sdio func1 error bit:800, func1 free cnt=0, func1 free queue2 cnt=2`，并需要**重试很多次**才成功 |
| D. **仅最初 5 次**哑读 Q1(+Q2)，之后只读 Q2（当前采用） | 首次 `03→09→09`，其后 `读 0x09` | ✅ **一次成功、芯片无报错** |

由此可得出两点，也请贵司一并确认：
- **Q2(0x09) 是 ROM 阶段有效的信用寄存器**，直接读 Q1 永远为 0；
- **反复访问 Q1(0x03) 会激怒芯片**（触发 `error bit:800` / `free cnt=0` 日志并拖慢握手），Q1 只在最开始访问有限次数即可；此后只读 Q2 即可稳定工作。

（另：写法 C/D 中都保留了少量 `rt_kprintf`，我们尚未分离"访问序列"与"打印带来的 ~400ms 延时"各自的作用；如实测需要延时，也说明 ROM 的信用上报存在时序依赖。）

日志证据：第一次 flow-ctrl 读时，`0x09` 的值在**任何延时之前**就已经是 2（`raw 0x03=0(ok1) 0x09=2(ok1)`），可见并非时序/延时所致，而是**该次 Q1 读使 Q2 变为有效**。这一点对我们理解 Q1/Q2 的语义很关键，也请贵司一并确认。

**(5) 结论。** 库永远从 Q1(0x03) 读 → 得到 0 → 判定"无可用 TX 缓冲" → **第一条 cfg 消息就发不出去** → `system config` 超时 → 初始化失败。与总线宽度、D1 电平、板级上拉都无关（1-bit / 4-bit 现象一致，CMD52 读通路经独立探针验证完全正常）。

## 4. 我们的临时修改（SDK 侧，仅一处判断）

**文件**：`customer/peripherals/wifi/aic8800_rtos_driver/platform/hal/sdio_port.c`
**函数**：`aicwf_sdio_readb_cmd52()`（func1 CMD52 单字节读的唯一入口）

```c
bool aicwf_sdio_readb_cmd52(uint32_t addr, uint8_t *data)
{
    /* 本芯片(0x9082) ROM 阶段 TX 信用在 V3 Q2(0x09)，而库固定读 Q1(0x03) → 读侧重定向。
     * 且实测必须先读一次 Q1，Q2 才会给出有效信用。 */
    if ((g_runtime_chipid == PRODUCT_ID_AIC8800D80N) &&
        (addr == SDIOWIFI_FLOW_CTRL_Q1_REG_V3))
    {
        uint8_t dummy = 0;
        aicwf_sdio_readb_cmd52_by_num(FUNC_1, SDIOWIFI_FLOW_CTRL_Q1_REG_V3, &dummy);
        addr = SDIOWIFI_FLOW_CTRL_Q2_REG_V3;      /* 0x03 -> 0x09 */
    }
    return aicwf_sdio_readb_cmd52_by_num(FUNC_1, addr, data);
}
```

说明两点，供贵司判断：

1. 我们最初尝试的是**写侧**——在 `aicwf_sdiov3_func_init()` 里把 `sdiodev->sdio_reg.flow_ctrl_reg` 改成 0x09。它不可靠：实测库自己始终把该字段保持为 `0x03`（日志 `field=0x03`），写侧等于和库抢同一个字段，能否生效取决于调用顺序。
2. 改成**读侧拦截**后 100% 稳定，因为无论库何时把字段设回 Q1，库的每一次 flow-ctrl 读都会被翻译到 Q2；其中那次"哑读 Q1"也是必需的（见 §3 第 (4) 条）。

## 5. 修改后的结果（已跑通）

```
[AICFC] raw 0x03=0(ok1) 0x09=2(ok1) chipid=4 field=0x03
[AICFC] -> redirect 0x09 val=2
[AIC][MACIF]chip_id=3, chip_sub_id=1
[AIC][MACIF]====> D80N stage1: CINIT (bootrom)
[AIC][MACIF]  [CINIT] fw_id=60, dst=0x00133000, size=17028
[AIC][MACIF]D80N CINIT exec: Start app @ 0x00133009, FNCALL
[AIC][MACIF]D80N CINIT done (chipinfo=0x3c21, waited=100 ms)
[AIC][MACIF]Upload fw id=58 @ 0x00188000, size=23680          ; WiFi PATCH
[AIC][MACIF]  [PATCH_TBL] wrote 65 entries (130 words)
[AIC][MACIF]Upload fw id=61 @ 0x00138000, size=29544          ; CALIB
```
芯片侧：`wifi ready / PatchVer: la Aug 28 2026 … / driver start / is 5g=1` —— **初始化成功**。

把上面的重定向注释掉即可 100% 复现失败，据此可确认因果关系。

## 6. 为什么我们只把它当作临时方案

- 该改动只是"把库问错的问题翻译一下"，**掩盖了库对 ROM 阶段队列语义的判断差异**；
- 它位于 SDK 的 platform 层，**每次同步贵司基线都会被覆盖**；
- 正确的做法应由 host 库按"芯片型号 + 阶段"选择队列寄存器，而不是由用户侧拦截读操作。

## 7. 请厂商确认 / 提供的问题清单

| # | 问题 |
|---|---|
| Q1 | D80N(0x9082) 在 **ROM 阶段**，cfg/message 通道（host→device 的 TX 信用）是否应当读 **Q2(0x09)**？此时 **Q1(0x03)** 的语义是什么（未初始化 / 无缓冲 / 另有用途）？ |
| Q2 | **FW 运行后**（PATCH/CALIB 完成、`start_from_bootrom` 之后）host 是否应当**切回 Q1**？如果有阶段切换，正确的切换时机是什么？ |
| Q3 | 芯片日志 `sdio func1 error bit:800, func1 free cnt=0, func1 free queue2 cnt=2` 的含义是什么？我们当前的用法（走 Q2）是否会触发它、是否有风险？ |
| Q4 | D80N ROM 是否有**官方推荐的初始化时序**？特别是 `sdio_port.c` 中被 `#if 0` 掉的唤醒握手（写 `0x02 = 0x11`，再读 `0x01` 的 bit4 判 ready）—— 我们没做该握手也能跑通，是否**应该补上**？ |
| Q5 | 是否有**已修正该问题的 host 库版本**？若有请提供库 + 对应的寄存器/队列选择规则。 |
| Q6 | 我们的读侧重定向是否会影响 **USB/SDIO 双模**或其它 D80 系列芯片（我们已用 `chipid == PRODUCT_ID_AIC8800D80N && addr == 0x03` 限定范围）？ |
| Q7 | §3(4) 表 C/D 的差异是否说明：**Q1(0x03) 只能被有限次数访问**，反复访问会触发芯片 `error bit:800 / func1 free cnt=0` 日志？host 侧对 Q1/Q2 的正确**访问顺序与频次**是什么？`error bit:800` 各位的定义？ |

## 8. 可提供的复现材料

- 主机串口完整日志、芯片 Bootrom/FW 串口完整日志
- func1 `0x00~0x1F` 寄存器全量 dump（修改前/后各一份）
- `main.asm` 中 `aicwf_sdio_reg_init()` / `aicwf_sdio_flow_ctrl_msg()` 的反汇编片段
- 复现步骤：SDK 2.5.0（Wi-Fi 树）+ board `sf32lb55-lcd_n16r4n4` + 4-bit SDIO，注释掉 §4 的重定向即可
