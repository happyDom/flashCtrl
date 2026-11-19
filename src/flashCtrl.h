#ifndef d25787ac_53ee_1f4b_eedd_3ee716ceec1a
#define d25787ac_53ee_1f4b_eedd_3ee716ceec1a

#include <stdint.h>

/* 请认真阅读以下关于 userFlashCtrlCfg.h 文件的使用说明
 * 1、 执行器数量
 * 本库不在运行时动态申请内存，而是使用库内的一块静态内存池，
 * 池的大小由需要控制的执行器数量决定。该数量由项目侧决定，因此：
 * 请在项目中创建一个 userFlashCtrlCfg.h（可照抄本库 cfg/ 目录下的 userFlashCtrlCfg.h.sample），
 *    并在其中定义下面的宏：
 #define FLASH_CTRL_ACTUATOR_NUM    2      //需要控制的执行器数量（LED、蜂鸣器……各算一个）

 * 2、 调度周期
 * 本库不自己计时，由调用方按固定周期调用 flashCtrl_ticks()，该周期即“一拍”。
 * 请在同一个文件里定义调度周期，用于把毫秒数换算成节拍数（flashCtrl_setThdMs）：
 #define FLASH_CTRL_CYCLE_MS         10     //调度周期，单位毫秒（例如主循环 10ms 调一次 ticks）

 * 3、 静态内存池的存放区（可选）
 * 在 51 类单片机上，可根据项目情况把内存池指定到 data/idata/xdata/pdata 区：
 #define FLASH_CTRL_MEM_ATTR        xdata

 * 注意：本库的所有文件（含 src/ 与 cfg/）都请保持原样，只做引用，不要写入与本项目相关的改动，
 *       避免后续向本库提交更新时夹带项目数据。
 * 本库只依赖 stdint.h，不使用 string.h 等库函数。
 */
#include "userFlashCtrlCfg.h"

#ifndef NULL
#if defined(__cplusplus)
#define NULL 0
#else
#define NULL ((void*)0)
#endif
#endif

#ifndef FLASH_CTRL_MEM_ATTR
#define FLASH_CTRL_MEM_ATTR
#endif

#ifndef FLASH_CTRL_ACTUATOR_NUM
#error "please #define FLASH_CTRL_ACTUATOR_NUM //in userFlashCtrlCfg.h"
#elif FLASH_CTRL_ACTUATOR_NUM <= 0
#error "FLASH_CTRL_ACTUATOR_NUM must be greater than 0!"
#endif

#ifndef FLASH_CTRL_CYCLE_MS
#error "please #define FLASH_CTRL_CYCLE_MS //in userFlashCtrlCfg.h"
#elif FLASH_CTRL_CYCLE_MS <= 0
#error "FLASH_CTRL_CYCLE_MS must be greater than 0!"
#endif

/* 毫秒 → 节拍数换算：向上取整，保证非 0 的毫秒数至少折算为 1 拍（0 毫秒仍为 0 拍，即“不动作”） */
#ifndef FLASH_CTRL_CNT_OF_MS
#define FLASH_CTRL_CNT_OF_MS(ms)  ((uint16_t)(((uint32_t)(ms) + (FLASH_CTRL_CYCLE_MS - 1u)) / FLASH_CTRL_CYCLE_MS))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 执行器控制函数类型：由调用方实现“动作、停止”的具体动作
 * （如：拉低端口点亮 LED、拉高端口熄灭 LED、使能蜂鸣器等） */
typedef void (*io_ctrl_func)(void);

/* 执行器状态读取函数类型：由调用方实现当前状态读取
 * 返回 0：执行器处于“动作态”（LED 点亮、蜂鸣器鸣叫……）
 * 返回 1：执行器处于“静止态”（LED 熄灭、蜂鸣器静默……） */
typedef uint8_t (*io_sts_func)(void);

/* 停止节拍控制时，执行器的末态要求（用于 flashCtrl_stop / flashCtrl_deinit 的参数，
 * 与 get_sts 的返回值含义无关，避免两者混淆故单独定义） */
typedef enum {
    FLASH_CTRL_STS_KEEP   = 0,   /* 保持执行器当前状态，不做任何切换 */
    FLASH_CTRL_STS_ACTIVE = 1,   /* 停止后进入动作态（点亮 LED / 蜂鸣器鸣叫） */
    FLASH_CTRL_STS_IDLE   = 2,   /* 停止后进入静止态（熄灭 LED / 蜂鸣器静默） */
} flashCtrlOffState_t;

/* 闪烁控制句柄：一个句柄对应一个执行器（LED、蜂鸣器……），
 * 句柄由本库在静态内存池中分配，调用方只需记录返回的指针，
 * 所有过程参数与计数都由本库维护。 */
typedef struct FlashCtrl {
    uint16_t on_thd;        /* 动作态节拍数：动作态需持续的节拍数，0：不进入动作态（保持静止） */
    uint16_t off_thd;       /* 静止态节拍数：静止态需持续的节拍数，0：不进入静止态（保持动作） */
    uint16_t on_cnt;        /* 动作态已计节拍数，由库内维护 */
    uint16_t off_cnt;       /* 静止态已计节拍数，由库内维护 */
    io_ctrl_func turn_on;   /* 进入动作态的注册函数（对应原 clear_P42） */
    io_ctrl_func turn_off;  /* 进入静止态的注册函数（对应原 set_P42） */
    io_sts_func get_sts;    /* 状态读取的注册函数（对应原 getP42Sts） */
    uint8_t inUse;          /* 内存池槽位占用标志，由库内维护，调用方不要改写 */
    struct FlashCtrl* next; /* 多实例工作时链表指针，由库内维护 */
} FlashCtrl;

/* 在库内静态内存池中分配一个句柄，并登记执行器的动作/停止/状态读取函数。
 * 返回 NULL 表示内存池已满（需要调大 FLASH_CTRL_ACTUATOR_NUM）或 get_sts 非法。 */
FlashCtrl* flashCtrl_init(io_ctrl_func turn_on, io_ctrl_func turn_off, io_sts_func get_sts);

/* 释放句柄：先按 offState 停止节拍控制（口径与 flashCtrl_stop 完全一致），再清空槽位、
 * 槽位可被后续 flashCtrl_init 复用。offState 传 FLASH_CTRL_STS_KEEP 表示只回收资源、不动执行器。 */
void flashCtrl_deinit(FlashCtrl* handle, flashCtrlOffState_t offState);

/* 查询内存池中已分配的句柄数量 / 内存池容量 */
uint16_t flashCtrl_getUsedCount(void);
uint16_t flashCtrl_getCapacity(void);

/* 设置动作态/静止态的时长，两者都会清零节拍计数：
 * setThdCnt 按节拍数（0 拍 = 不进入该相位），setThdMs 按毫秒数（库内用 FLASH_CTRL_CYCLE_MS 换算，向上取整）；
 * getThdCnt 读回当前的节拍数。 */
void flashCtrl_setThdCnt(FlashCtrl* handle, uint16_t on_thd, uint16_t off_thd);
void flashCtrl_setThdMs(FlashCtrl* handle, uint16_t on_ms, uint16_t off_ms);
void flashCtrl_getThdCnt(FlashCtrl* handle, uint16_t* on_thd, uint16_t* off_thd);

/* 清零节拍计数，不改变阈值与执行器状态 */
void flashCtrl_reset(FlashCtrl* handle);

/* 启停与调度：start 把句柄加入工作链表，ticks 由调用方按固定周期调用 */
int flashCtrl_start(FlashCtrl* handle);

/* 停止节拍控制：把句柄移出工作链表，并按 offState 指定停止时执行器的末态
 * （FLASH_CTRL_STS_KEEP 不改动执行器；ACTIVE/IDLE 会切换状态并清零节拍计数）。 */
void flashCtrl_stop(FlashCtrl* handle, flashCtrlOffState_t offState);

void flashCtrl_ticks(void);

#ifdef __cplusplus
}
#endif

#endif /* d25787ac_53ee_1f4b_eedd_3ee716ceec1a */
