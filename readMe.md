# readMe.md

## 介绍

这是一个用于控制指定执行器节律性动作的封装库，本库可以控制

- 简单控制
  - 控制LED灯按指定的周期闪烁（分别指定亮/熄节拍数）
  - 控制蜂鸣器按指定的周期鸣叫（分别指定鸣/熄节拍数）

- 灯语控制
  - 控制LED灯按指定的长亮/短亮数目执行闪烁
  - 控制蜂鸣器按指定的长鸣/短鸣数量执行鸣叫动作

本库不含任何硬件相关代码，执行器的动作与状态读取由调用方通过函数指针注册，因此同一套逻辑可复用于 LED、蜂鸣器或其它执行器。

## 库资源说明

```
src/
  flashCtrl.h              库对外接口
  flashCtrl.c              库实现（含静态内存池）
cfg/
  userFlashCtrlCfg.h.sample  配置文件参考样例（只读，带 .sample 后缀，不会被当作配置使用）
example/
  flashCtrl_example_led.c  调用方示例（LED / 蜂鸣器），不属于库本体
```

- 无动态内存：句柄由库内静态内存池分配，池大小 = `FLASH_CTRL_ACTUATOR_NUM * sizeof(FlashCtrl)`；
- 无库函数依赖：只引用 `stdint.h`，不使用 `string.h` / `memset`，初始化逐项赋值；
- 用户配置隔离：执行器数量与调度周期写在项目自己的 `userFlashCtrlCfg.h` 中，换项目只增删项目侧这个文件，不触碰本库任何文件；
- 双单位设置：可按键拍数（`flashCtrl_setThdCnt`）或毫秒数（`flashCtrl_setThdMs`）指定明暗时长；
- C51 友好：可通过 `FLASH_CTRL_MEM_ATTR` 把内存池指定到 `xdata` 等存储区。

> 本库的文件（含 `cfg/` 下的参考样例）请保持原样，仅作引用。项目相关的配置与改动一律留在项目侧，
> 避免向本库提交更新时把项目脏数据带进库仓库。

## 配置说明

请在你的项目中新建 `userFlashCtrlCfg.h`（文件名去掉 `.sample`，内容照抄本库 `cfg/userFlashCtrlCfg.h.sample`），不要在本库目录内创建或修改配置文件：

```c
#define FLASH_CTRL_ACTUATOR_NUM     2          /* 需要控制的执行器数量，1~255 */
#define FLASH_CTRL_CYCLE_MS         10         /* 调度周期（一拍多少毫秒），必须与实际调用 ticks 的周期一致 */
#define FLASH_CTRL_LANG_EN          1          /* 是否需要灯语模式：1 需要，0 裁掉灯语省 RAM/代码（缺省为 1） */
/* #define FLASH_CTRL_MEM_ATTR      xdata */  /* 可选，C51 上指定内存池存放区 */
```

`FLASH_CTRL_CYCLE_MS` 只影响 `flashCtrl_setThdMs`（毫秒）的换算，用 `flashCtrl_setThdCnt`（节拍数）时不会用到；
两个宏缺一个都会在编译期报错并提示。
建议开发初期给一个够用的值，项目定形后用 `flashCtrl_getUsedCount()` 读出实际用量再调小。

注意：`flashCtrl.h` 以 `#include "userFlashCtrlCfg.h"` 引用该文件时，会优先搜索包含它的目录，
因此请把配置文件放在**项目自己的目录**下（加入工程的头文件搜索路径），不要放进本库的 `src/` 或 `cfg/` 目录，
否则项目配置会混入库文件。库内样例带 `.sample` 后缀，无论如何都不会被 `#include` 命中，
即使工程把 `cfg/` 加进了搜索路径，也不会误改到库里的文件。

## 接口方法说明

### 类型

| 类型 | 说明 |
| --- | --- |
| `io_ctrl_func` | `void (*)(void)`，执行器控制动作（进入动作态 / 进入静止态） |
| `io_sts_func` | `uint8_t (*)(void)`，读取执行器状态；返回 0 表示动作态（点亮 / 鸣叫），返回 1 表示静止态（熄灭 / 静默） |
| `flashCtrlOffState_t` | `flashCtrl_stop` / `flashCtrl_deinit` 的末态参数：`FLASH_CTRL_STS_KEEP` 保持原状、`FLASH_CTRL_STS_ACTIVE` 停在动作态、`FLASH_CTRL_STS_IDLE` 停在静止态（与 `get_sts` 返回值含义无关，避免混淆） |
| `FlashCtrl` | 控制句柄，一个句柄对应一个执行器；由库内内存池分配，阈值与计数由库内维护 |

### 方法

| 方法 | 说明 |
| --- | --- |
| `flashCtrl_init(turn_on, turn_off, get_sts)` | 在静态内存池中分配句柄并登记动作函数（`turn_on` 对应原 `clear_P42`，`turn_off` 对应原 `set_P42`）与状态读取函数（对应原 `getP42Sts`）；返回句柄，池满返回 `NULL` |
| `flashCtrl_deinit(handle, offState)` | 先按 `offState` 停止节拍控制（口径与 `flashCtrl_stop` 相同），再清空槽位、归还内存池，槽位可被后续 `init` 复用；`KEEP` 表示只回收资源、不动执行器 |
| `flashCtrl_getUsedCount()` | 查询已分配的句柄数量 |
| `flashCtrl_getCapacity()` | 查询内存池容量（即 `FLASH_CTRL_ACTUATOR_NUM`） |
| `flashCtrl_setThdCnt(handle, on_thd, off_thd)` | 按节拍数设置动作态 / 静止态时长，同时清零节拍计数 |
| `flashCtrl_setThdMs(handle, on_ms, off_ms)` | 按毫秒数设置动作态 / 静止态时长，库内用 `FLASH_CTRL_CYCLE_MS` 换算成节拍数（向上取整，0 毫秒仍为 0 拍） |
| `flashCtrl_getThdCnt(handle, &on_thd, &off_thd)` | 读取当前节拍数（参数可传 `NULL`）；阈值不随模式切换丢失，灯语模式下也能读回切走前的值 |
| `flashCtrl_reset(handle)` | 清零当前阶段进度：简单模式清节拍计数，灯语模式清阶段计时；不改变阈值、模式与执行器状态 |
| `flashCtrl_start(handle)` | 把句柄加入工作链表，重复加入返回 -1 |
| `flashCtrl_stop(handle, offState)` | 把句柄移出工作链表，并按 `offState` 指定停止时的执行器末态：`KEEP` 不动硬件、`ACTIVE` 停在动作态、`IDLE` 停在静止态（后两者会切换状态并清零节拍计数） |
| `flashCtrl_ticks()` | 节拍调度，按固定周期调用即可推进所有已启动的句柄 |

> 单个句柄的节拍处理（即通用化后的 `led0_toggle()`）是库内私有函数，由 `flashCtrl_ticks` 内部调用，不对外暴露。

### 灯语模式（`FLASH_CTRL_LANG_EN = 1` 时可用）

一个句柄同一时刻只处于一种模式：
- 切到灯语模式：`flashCtrl_langTrigger(handle, ...)`；
- 退出灯语模式：`flashCtrl_exitLangMode(handle)`（阈值沿用切走前的值，无需重设），
  或者直接调 `flashCtrl_setThdCnt` / `flashCtrl_setThdMs`（等于“切回简单模式并重设时长”）。

两种模式的进度共用同一块句柄内存（联合体），而简单模式的阈值放在联合体外面，
所以来回切换时阈值不会丢，句柄也不会因为多了一种模式而明显变大。

灯的时序由**全局**配置一次，所有句柄共用；触发时给“长/短次数 + 要跑几轮”。
各段“熄灭”时长是共用的，因此配置只需四个时长：

| 参数 | 含义 |
| --- | --- |
| `longOn` | 长亮的点亮时长 |
| `shortOn` | 短亮的点亮时长 |
| `gapOff` | 每次闪烁之间的熄灭时长（长亮之间、短亮之间、长亮组与短亮组之间共用） |
| `leadOff` | 首尾熄灭时长（触发时的起始熄灭、一轮结束的收尾熄灭共用） |

| 方法 | 说明 |
| --- | --- |
| `flashCtrl_setLangCfgCnt(longOn, shortOn, gapOff, leadOff)` | 按节拍数配置灯语各段时长 |
| `flashCtrl_setLangCfgMs(longOnMs, shortOnMs, gapOffMs, leadOffMs)` | 同上，但按毫秒数给，库内用 `FLASH_CTRL_CYCLE_MS` 换算成节拍数 |
| `flashCtrl_langTrigger(handle, timesOfLong, timesOfShort, cycles)` | 触发灯语：指定长亮 / 短亮次数和要跑的整轮数；跑满 `cycles` 轮后**自停**（停在熄灭态）。两个次数都传 0 表示取消（回空闲、不动执行器）；`cycles` 传 0 表示无限重复 |
| `flashCtrl_isLangDone(handle)` | 是否已跑完设定轮数：1 已跑完、0 还在跑（`cycles = 0` 或不在灯语模式时恒为 0） |
| `flashCtrl_exitLangMode(handle)` | 退出灯语、回到简单模式，恢复按 `on_thd` / `off_thd` 周期闪烁（阈值沿用切走前的值） |
| `flashCtrl_getLangCycle(handle)` | 查询已完成的整轮数（65535 封顶，跑满设定轮数后不再增长） |
| `flashCtrl_resetLangCycle(handle)` | 只清零已完成的轮数 |

一轮的时序：

```
起始熄灭(可选，leadOff) → [长亮 on]*N → 熄灭(gapOff) → … → 组间熄灭(gapOff) → [短亮 on]*M → … → 收尾熄灭(leadOff) → 下一轮
```

- **轮数在触发时一次给定，跑满就自停**：不会出现“底层还在跑、应用层已经换成新灯语”的半途切换。
  代价是收尾熄灭之后的空闲会一直持续到应用层来切换（应用层慢一点，灯就多灭一会儿），
  但这比“报一半被打断”可接受；
- 跑满后 `flashCtrl_isLangDone()` 返回 1，此时执行器停在熄灭态、`ticks` 对本句柄不再写端口；
  应用层据此切换下一个错误码（在轮询周期里检查即可）；
- `cycles` 传 0 表示无限重复（常驻故障指示），此时 `isLangDone` 恒为 0，需要主动取消或切换；
- **起始熄灭是按需的**：触发时执行器正亮着才先灭 `leadOff` 拍（与简单模式 toggle 的口径一致）；
  触发时本就熄灭，则直接进入长亮组，不再额外延长一个熄灭周期；
- 任一段时长配置为 0 拍即跳过该阶段；`timesOfLong = 0` 时不报长亮组（组间熄灭也一并跳过）；
- 长亮次数用完后才进入短亮组，两组之间同样用 `gapOff` 分隔；
- 一轮结束时用 `leadOff` 收尾，这个收尾熄灭同时也是两轮之间的间隔；
- 各段的“灭”共用 `gapOff`（`leadOff` 只管首尾），所以灯语的节奏是均匀的：亮时长决定长短，灭时长统一；
- 停止/取消：取消用 `flashCtrl_langTrigger(handle, 0, 0, 0)`（回空闲、不动硬件）；
  连调度一起停并指定末态，用 `flashCtrl_stop(handle, offState)`，口径与简单模式完全相同。

示例（2 次长亮 + 2 次短亮、报 3 轮；长亮亮 300ms、短亮亮 100ms、闪烁间熄灭 200ms、首尾各熄 700ms）：

```c
flashCtrl_setLangCfgMs(300, 100, 200, 700);           /* 全局配置一次 */
flashCtrl_langTrigger(led0, 2, 2, 3);                  /* 报 3 轮，跑满自停 */

/* 在主循环里轮询（应用层慢一点没关系，灯只是多灭一会儿） */
if (flashCtrl_isLangDone(led0)) {
    flashCtrl_langTrigger(led0, 3, 1, 1);              /* 直接换下一个错误码，不必先取消 */
}

/* 常驻故障（一直报，直到故障消失） */
flashCtrl_langTrigger(led0, 1, 0, 0);                  /* cycles = 0：无限重复 */
flashCtrl_langTrigger(led0, 0, 0, 0);                  /* 故障消失：取消 */

flashCtrl_exitLangMode(led0);                          /* 不再用灯语：切回周期闪烁，原阈值仍在 */
```

> 切换下一个错误码时不需要先 `langTrigger(h, 0, 0, 0)`：跑满后本句柄已停在熄灭态，
> 直接触发新的一次即可（新灯语从熄灭态开始，不会再插一段起始熄灭）。

#### 常见错误码灯语对照表（可直接照抄，按需增删）

灯语时长是**全局一套**，所以下面所有错误码共用一组时长，只靠“长亮 / 短亮次数”区分。
参考配置（调度周期 10ms 时）：

```c
flashCtrl_setLangCfgMs(500, 150, 300, 1200);   /* 长亮亮 500ms、短亮亮 150ms、闪烁间熄灭 300ms、首尾各熄 1200ms */
```

| 灯语 | 长亮次数 | 短亮次数 | 报几轮（建议） | 典型含义 / 用途 |
| --- | --- | --- | --- | --- |
| 不报（灯语取消） | 0 | 0 | — | 待机、正常，灯由业务自己控制 |
| 1 长 | 1 | 0 | 2~3 | 电源异常（欠压 / 过压 / 掉电重启） |
| 2 长 | 2 | 0 | 2~3 | 传感器异常（开路、短路、无响应） |
| 3 长 | 3 | 0 | 2~3 | 通信超时（总线 / 上位机无应答） |
| 1 长 1 短 | 1 | 1 | 3 | 低电量 / 需要提醒的告警 |
| 2 长 1 短 | 2 | 1 | 3 | 过温保护（降额运行中） |
| 3 长 1 短 | 3 | 1 | 3 | 过流保护（输出已关断） |
| 1 长 2 短 | 1 | 2 | 3 | 参数 / 存储器错误（校验失败、写入失败） |
| 2 长 2 短 | 2 | 2 | 0（常驻） | 自检中、标定中（进度提示） |

几点约定（按这几点做，肉眼最容易分辨）：

- **长亮次数少、短亮次数多**更容易辨：长亮代表“大类”，短亮代表“具体项”，看长亮次数一眼就知道是哪一类；
- 同一时刻只报一个错误码：`langTrigger(h, 长, 短, 轮数)` 一次给定轮数，跑满自停后再切下一个；
- 切换的判据是 `flashCtrl_isLangDone(h)`（不是“已经跑到第几轮”），这样应用层轮询慢也不会有半途切换；
- 致命故障（需要断电复位）建议不用灯语，直接用简单模式高频闪烁：`flashCtrl_setThdMs(h, 50, 50)`；
- “一直报到故障消失”这类常驻指示，用 `cycles = 0`（无限重复），故障消失时 `langTrigger(h, 0, 0, 0)` 取消；
- 若觉得“只亮一下就过去了”，通常是 `gapOff` / `leadOff` 配得太大，先缩这两个值，再调亮时长。

### 节拍语义

`on_thd` 为动作态持续节拍数，`off_thd` 为静止态持续节拍数：

- `on_thd = 0`：不进入动作态（当前熄灭则保持熄灭）；
- `off_thd = 0`：不进入静止态（当前点亮则保持点亮）；
- 两者均为 0：仅累计动作 / 静止节拍数，不改变执行器状态；
- 两者均非 0：静止 `off_thd` 拍后动作、动作 `on_thd` 拍后停止，循环闪烁。

用 `flashCtrl_setThdMs` 时，上面各处的“节拍数”按 `FLASH_CTRL_CYCLE_MS` 换算：
`节拍数 = (毫秒数 + 周期 - 1) / 周期`（向上取整）。例如周期 10ms，设 50ms/150ms → 5 拍/15 拍。

### 典型用法

```c
static FlashCtrl* led0 = NULL;

static void led0_turn_on(void)    { P42 = 0; }              /* 拉低点亮 */
static void led0_turn_off(void)   { P42 = 1; }              /* 拉高熄灭 */
static uint8_t led0_get_sts(void) { return P42 ? 1 : 0; }   /* 0：点亮，1：熄灭 */

led0 = flashCtrl_init(led0_turn_on, led0_turn_off, led0_get_sts);
if (NULL == led0) { /* 池满：调大 FLASH_CTRL_ACTUATOR_NUM */ }

flashCtrl_setThdCnt(led0, 5, 15);       /* 按键拍数：亮 5 拍、灭 15 拍 */
/* 或按键毫秒数：flashCtrl_setThdMs(led0, 50, 150); */

flashCtrl_start(led0);               /* 加入调度链表 */

/* 固定节拍（如定时器 10ms 中断或主循环延时）中调用 */
flashCtrl_ticks();

/* 需要停止时，可选择停止后的末态 */
flashCtrl_stop(led0, FLASH_CTRL_STS_IDLE);   /* 停在熄灭；KEEP 保持原状，ACTIVE 停在点亮 */

/* 需要彻底释放时，同样指定末态（KEEP 表示只回收资源、不动执行器） */
flashCtrl_deinit(led0, FLASH_CTRL_STS_KEEP);
```

## 参与贡献

1、Fork 本仓库

2、新建 Feat_xxx 分支

3、提交代码

4、新建 Pull Request
