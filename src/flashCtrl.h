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

 * 3、 灯语模式（可选）
 * 需要“长亮 N 次 + 短亮 M 次”这类灯语时报错的项目置 1；只用周期闪烁的项目置 0，
 * 可省掉灯语状态占用的 RAM 与相关代码：
 #define FLASH_CTRL_LANG_EN          1

 * 4、 静态内存池的存放区（可选）
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

#ifndef FLASH_CTRL_LANG_EN
#define FLASH_CTRL_LANG_EN   1      /* 默认开启灯语；不需要的项目在 userFlashCtrlCfg.h 里置 0 */
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

/* 句柄的工作模式：一个执行器同一时刻只处于一种模式 */
typedef enum {
    FLASH_CTRL_MODE_SIMPLE = 0,  /* 简单模式：按 on/off 节拍无限循环闪烁 */
#if FLASH_CTRL_LANG_EN
    FLASH_CTRL_MODE_LANG   = 1,  /* 灯语模式：长亮 N 次 + 短亮 M 次 + 收尾熄灭，按轮循环 */
#endif
} flashCtrlMode_t;

/* 闪烁控制句柄：一个句柄对应一个执行器（LED、蜂鸣器……），
 * 句柄由本库在静态内存池中分配，调用方只需记录返回的指针。
 * 简单模式的阈值放在联合体外面，因此切到灯语模式再切回来时阈值不会丢；
 * 两种模式的进度/过程变量互斥，放在联合体里共用同一块内存。 */
typedef struct FlashCtrl {
    io_ctrl_func turn_on;   /* 进入动作态的注册函数 */
    io_ctrl_func turn_off;  /* 进入静默态的注册函数 */
    io_sts_func  get_sts;   /* 状态读取的注册函数 */
    uint16_t     on_thd;    /* 简单模式：动作态节拍数，0：不动作（保持静止） */
    uint16_t     off_thd;   /* 简单模式：静止态节拍数，0：不停止（保持动作） */
    uint8_t      mode;      /* flashCtrlMode_t，由库内维护 */
    uint8_t      inUse;     /* 内存池槽位占用标志，由库内维护，调用方不要改写 */
    struct FlashCtrl* next; /* 多实例工作时链表指针，由库内维护 */

    union {
        /* 简单模式的进度 */
        struct {
            uint16_t on_cnt;        /* 动作态已计节拍数，由库内维护 */
            uint16_t off_cnt;       /* 静止态已计节拍数，由库内维护 */
        } simple;
#if FLASH_CTRL_LANG_EN
        /* 灯语模式：长亮 N 次 + 短亮 M 次 + 收尾熄灭，跑完一轮自动进入下一轮 */
        struct {
            uint16_t phaseCnt;      /* 当前阶段已计节拍数，由库内维护 */
            uint16_t cycleCnt;      /* 已完成的整轮数，可查询/清零 */
            uint8_t  longTotal;     /* 本轮需要长亮的次数，由触发接口设置 */
            uint8_t  shortTotal;    /* 本轮需要短亮的次数，由触发接口设置 */
            uint8_t  longDone;      /* 已完成长亮次数，由库内维护 */
            uint8_t  shortDone;     /* 已完成短亮次数，由库内维护 */
            uint8_t  phase;         /* 当前阶段，由库内维护 */
            uint8_t  lit;           /* 执行器状态缓存（0：动作态，1：静止态），由库内维护 */
        } lang;
#endif
    } u;
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

/* 设置动作态/静止态的时长（简单模式），两者都会清零节拍计数，并让句柄回到简单模式：
 * setThdCnt 按节拍数（0 拍 = 不进入该相位），setThdMs 按毫秒数（库内用 FLASH_CTRL_CYCLE_MS 换算，向上取整）；
 * getThdCnt 读回当前的节拍数（阈值不随模式切换丢失，灯语模式下也可读回切走前设置的值）。 */
void flashCtrl_setThdCnt(FlashCtrl* handle, uint16_t on_thd, uint16_t off_thd);
void flashCtrl_setThdMs(FlashCtrl* handle, uint16_t on_ms, uint16_t off_ms);
void flashCtrl_getThdCnt(FlashCtrl* handle, uint16_t* on_thd, uint16_t* off_thd);

/* 清零当前阶段的进度计数，不改变阈值、模式与执行器状态 */
void flashCtrl_reset(FlashCtrl* handle);

/* 启停与调度：start 把句柄加入工作链表，ticks 由调用方按固定周期调用 */
int flashCtrl_start(FlashCtrl* handle);

/* 停止节拍控制：把句柄移出工作链表，并按 offState 指定停止时执行器的末态
 * （FLASH_CTRL_STS_KEEP 不改动执行器；ACTIVE/IDLE 会切换状态并清零节拍计数）。 */
void flashCtrl_stop(FlashCtrl* handle, flashCtrlOffState_t offState);

void flashCtrl_ticks(void);

#if FLASH_CTRL_LANG_EN
/* ---------------- 灯语模式 ----------------
 * 时序：起始熄灭(可选) → [长亮亮/长亮灭]*longTotal → 组间熄灭 → [短亮亮/短亮灭]*shortTotal → 收尾熄灭 → 下一轮
 * 一轮跑完自动开始下一轮，轮数由 flashCtrl_getLangCycle 查询，便于“同一个错误码报 N 轮再换下一个”。 */

/* 灯语时长全局配置（所有句柄共用一套，配置一次即可）。
 * 各段熄灭时长是共用的，因此只需四个参数：
 *   longOn:  长亮的点亮时长
 *   shortOn: 短亮的点亮时长
 *   gapOff:  每次闪烁之间的熄灭时长（长亮之间、短亮之间、长亮组与短亮组之间共用）
 *   leadOff: 首尾熄灭时长（触发时执行器正亮着就先灭这么久；一轮跑完的收尾熄灭也用它）
 * 传 0 表示该时长 0 拍（该阶段被跳过）。
 * 例：报“2 次长亮 + 2 次短亮”，长亮亮 300ms、短亮亮 100ms、闪烁间熄灭 200ms、首尾各熄 700ms：
 *     flashCtrl_setLangCfgMs(300, 100, 200, 700);  再 flashCtrl_langTrigger(handle, 2, 2); */
void flashCtrl_setLangCfgCnt(uint16_t longOn, uint16_t shortOn, uint16_t gapOff, uint16_t leadOff);
void flashCtrl_setLangCfgMs(uint16_t longOnMs, uint16_t shortOnMs, uint16_t gapOffMs, uint16_t leadOffMs);

/* 触发灯语：设置本轮的长亮/短亮次数，并把轮数、进度清零后从头开始跑。
 * 次数传 0 表示该组不报；两者都传 0 表示取消灯语（回到空闲，不动执行器状态）。 */
void flashCtrl_langTrigger(FlashCtrl* handle, uint8_t timesOfLong, uint8_t timesOfShort);

/* 退出灯语、回到简单模式：灯语进度清零，执行器恢复按 on_thd / off_thd 周期闪烁，
 * 阈值沿用切到灯语模式之前设置的值（不必重设）。 */
void flashCtrl_exitLangMode(FlashCtrl* handle);

/* 查询已完成的整轮数（65535 封顶，不清零则一直保持）。 */
uint16_t flashCtrl_getLangCycle(FlashCtrl* handle);

/* 只清零轮数，不影响正在跑的进度（同一错误码重新计次时用）。 */
void flashCtrl_resetLangCycle(FlashCtrl* handle);
#endif

#ifdef __cplusplus
}
#endif

#endif /* d25787ac_53ee_1f4b_eedd_3ee716ceec1a */
