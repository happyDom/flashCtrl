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
| `flashCtrl_getThdCnt(handle, &on_thd, &off_thd)` | 读取当前节拍数（参数可传 `NULL`） |
| `flashCtrl_reset(handle)` | 清零节拍计数，不改变阈值与执行器状态 |
| `flashCtrl_start(handle)` | 把句柄加入工作链表，重复加入返回 -1 |
| `flashCtrl_stop(handle, offState)` | 把句柄移出工作链表，并按 `offState` 指定停止时的执行器末态：`KEEP` 不动硬件、`ACTIVE` 停在动作态、`IDLE` 停在静止态（后两者会切换状态并清零节拍计数） |
| `flashCtrl_ticks()` | 节拍调度，按固定周期调用即可推进所有已启动的句柄 |

> 单个句柄的节拍处理（即通用化后的 `led0_toggle()`）是库内私有函数，由 `flashCtrl_ticks` 内部调用，不对外暴露。

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

tbd
