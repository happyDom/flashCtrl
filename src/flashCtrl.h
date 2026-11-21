/* SPDX-License-Identifier: MIT */
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

/* 使用注意（都是实际集成到项目时踩过的坑，照着做能省不少调试时间）
 * 1、 is_on 必须真读端口状态，不要返回软件标志位。语义与命名同向：
 *     返回 1 = 动作态（亮 / 鸣叫），返回 0 = 静止态（灭 / 静默）。
 *     因此“低电平点亮”的 LED 要写成 return (P42 ? 0 : 1);（引脚读到 0 才代表亮）。
 *     简单模式每一拍都靠它判断执行器当前状态，标志位一旦与硬件不一致（别的逻辑改了灯、
 *     上电初值不同、被外部强行拉低……）就会“跑飞”：该灭的时候不灭、该亮的时候不亮。
 *     灯语模式内部用句柄里的状态缓存，只在状态变化时写端口，不受这一条影响。
 * 2、 节拍最好来自定时器中断（在中断里调 flashCtrl_ticks），别挂在长任务之后的主循环里：
 *     主循环一旦被长任务拖慢，节拍跟着变长，flashCtrl_setThdMs 设的毫秒数就偏了。
 * 3、 调度链表只在 flashCtrl_ticks() 的上下文里被改动：start / stop / deinit 都是“提交请求”
 *     （只写句柄里的两个单字节，不碰任何链表指针），实际的挂链/摘链由 ticks 在下一拍执行。
 *     因此主循环可以随时调用它们，不必自己加临界区，也不会出现
 *     “ticks 正在遍历链表、主循环同时摘链”的竞争；代价是启停最多延后一拍生效。
 *     附带好处：8051 上 2 字节指针的写入不是原子的，把链表改动全部收进同一个上下文，
 *     也避免了 ISR 读到“半个地址”的可能。
 *     注意：停止/释放时的末态处理也在 ticks 里执行，因此 turn_on / turn_off 可能在中断上下文
 *     被调用，实现必须短小、可重入、不阻塞，且不要在里面再调用本库的 start/stop/deinit。
 * 4、 FLASH_CTRL_ACTUATOR_NUM 按实际用量收紧：内存池是静态占 RAM 的
 *     （NUM * sizeof(FlashCtrl)）。项目定形后用 flashCtrl_getUsedCount() 读一次实际数量再调小。
 * 5、 灯语的时长是全局一套（所有句柄共用），多个执行器要报不同错误码时，
 *     靠“同一套时长 + 不同次数”区分；确实需要两套节奏时，就分别实例化各一份本库。
 * 6、 句柄里的字段由库内逐项维护，调用方只把 flashCtrl_init 返回的指针存起来即可，
 *     不要自行改写句柄成员（尤其 inUse、next、mode，改了会破坏内存池与调度链表）。
 * 7、 灯语时长配置传 0 表示“跳过该阶段”：例如只想报长亮、不要短亮，就把 timesOfShort 传 0；
 *     给很短的毫秒数时注意：实际时长 = 拍数 × FLASH_CTRL_CYCLE_MS，10ms 一拍时给 5ms 实际就是 10ms。
 * 8、 对外的时长接口一律用毫秒（setThdMs / setLangCfgMs），库内不暴露“节拍数”接口：
 *     这里的“拍”是 flashCtrl_ticks() 的调用周期（由 FLASH_CTRL_CYCLE_MS 声明），属于集成参数，
 *     与应用层自己的节拍没有关系；两个单位混用最容易把时长算错，所以只保留毫秒这一个口径。
 */

#include "userFlashCtrlCfg.h"

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

/* 执行器控制函数类型：由调用方实现“动作、停止”的具体动作 */
typedef void (*io_ctrl_func)(void);

/* 执行器状态读取函数类型：由调用方实现“当前是否处于动作态”的读取。
 * 返回 1：执行器处于“动作态”（LED 点亮、蜂鸣器鸣叫……）
 * 返回 0：执行器处于“静止态”（LED 熄灭、蜂鸣器静默……）
 * 语义与函数名同向（is_on），因此可以直接把“isTurnOn()”这类用户函数注册进来；
 * 低电平点亮的 LED 写成 return (P42 ? 0 : 1);。 */
typedef uint8_t (*io_is_on_func)(void);

/* 执行器的目标状态，用于 flashCtrl_stop / flashCtrl_deinit 的末态参数与
 * flashCtrl_setThdMs 的“闪满后终态”参数（与 is_on 的返回值含义无关，避免两者混淆故单独定义）
 * 取值与动作接口呼应：ON ≡ turnOn（动作态）、OFF ≡ turnOff（静止态）、KEEP 不动执行器 */
typedef enum {
    FLASH_CTRL_STS_KEEP = 0,   /* 保持执行器当前状态，不做任何切换 */
    FLASH_CTRL_STS_ON   = 1,   /* 进入动作态（点亮 LED / 蜂鸣器鸣叫），等价 turnOn */
    FLASH_CTRL_STS_OFF  = 2,   /* 进入静止态（熄灭 LED / 蜂鸣器静默），等价 turnOff */
} flashCtrlState_t;

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
    io_is_on_func is_on;    /* 状态读取的注册函数：1 = 动作态（亮），0 = 静止态（灭） */
    uint16_t     on_thd;    /* 简单模式：动作态节拍数，0：不动作（保持静止） */
    uint16_t     off_thd;   /* 简单模式：静止态节拍数，0：不停止（保持动作） */
    uint16_t     cycles;    /* 简单模式：需要闪烁的次数，0 表示一直闪下去（由 setThdMs 设置） */
    uint8_t      endState;  /* 简单模式：闪满 cycles 次后的终态（flashCtrlState_t，由 setThdMs 设置） */
    uint8_t      mode;      /* flashCtrlMode_t，由库内维护 */
    uint8_t      inUse;     /* 内存池槽位占用标志，由库内维护，调用方不要改写 */
    struct FlashCtrl* next; /* 多实例工作时链表指针，由库内维护 */

    /* 启停/释放请求：调用方通过 start/stop/deinit 只写这两个单字节（最后一个写入即“提交”），
     * 由 ticks 读取并执行。单字节写在 8051 上也是原子的，不会读到“半个指针”。 */
    volatile uint8_t pendingReq;      /* 请求码：0=无，1=启动，2=停止，3=释放 */
    volatile uint8_t pendingOffState; /* 请求携带的末态参数（flashCtrlState_t） */

    union {
        /* 简单模式的进度（参数在联合体外面，切到灯语再切回来时不会丢） */
        struct {
            uint16_t on_cnt;        /* 动作态已计节拍数，由库内维护 */
            uint16_t off_cnt;       /* 静止态已计节拍数，由库内维护 */
            uint16_t cycleCnt;      /* 已完成的闪烁次数，由库内维护 */
            uint8_t  done;          /* 1 = 已闪满设定次数、停在终态（由库内维护） */
        } simple;
#if FLASH_CTRL_LANG_EN
        /* 灯语模式：长亮 N 次 + 短亮 M 次 + 收尾熄灭，跑完指定轮数后自停 */
        struct {
            uint16_t phaseCnt;      /* 当前阶段已计节拍数，由库内维护 */
            uint16_t cycleCnt;      /* 已完成的整轮数，由库内维护 */
            uint8_t  longTotal;     /* 本轮需要长亮的次数，由触发接口设置 */
            uint8_t  shortTotal;    /* 本轮需要短亮的次数，由触发接口设置 */
            uint8_t  cycles;        /* 触发时设定的总轮数，0 表示无限重复（由触发接口设置） */
            uint8_t  longDone;      /* 已完成长亮次数，由库内维护 */
            uint8_t  shortDone;     /* 已完成短亮次数，由库内维护 */
            uint8_t  phase;         /* 当前阶段，由库内维护 */
            uint8_t  isOn;          /* 执行器状态缓存（1：动作态/亮，0：静止态/灭），由库内维护 */
        } lang;
#endif
    } u;
} FlashCtrl;

/* 在库内静态内存池中分配一个句柄，并登记执行器的 动作 / 停止 / 状态读取 函数。
 * 返回 0（空指针）表示内存池已满（需要调大 FLASH_CTRL_ACTUATOR_NUM）或 is_on 传了空指针。 */
FlashCtrl* flashCtrl_init(io_ctrl_func turn_on, io_ctrl_func turn_off, io_is_on_func is_on);

/* 请求释放（异步）：只登记请求，实际摘链 + 末态处理 + 归还槽位由 flashCtrl_ticks() 在下一拍执行，
 * 口径与 flashCtrl_stop 完全一致。offState 传 FLASH_CTRL_STS_KEEP 表示只回收资源、不动执行器。
 * 执行完成后槽位可被后续 flashCtrl_init 复用。 */
void flashCtrl_deinit(FlashCtrl* handle, flashCtrlState_t offState);

/* 查询内存池中已分配的句柄数量 / 内存池容量 */
uint16_t flashCtrl_getUsedCount(void);
uint16_t flashCtrl_getCapacity(void);

/* 设置简单模式的参数：动作态/静止态时长（毫秒）+ 闪烁次数 + 闪满后的终态。
 *   on_ms   : 动作态时长（点亮/鸣叫），0 表示不进入动作态；
 *   off_ms  : 静止态时长（熄灭/静默），0 表示不进入静止态；
 *   cycles  : 需要闪烁几次，0 表示一直闪下去（不结束）；
 *             一次闪烁 = 静止段 → 动作段 → 回到静止段 的完整一圈（在动作段结束、切回静止态的那一刻计数 +1），
 *             所以 on_ms=0 或 off_ms=0 时永远凑不齐一整圈，cycles 实际不起作用（常亮/常灭请直接传 0）；
 *   endState: 闪满 cycles 次后的终态，取值同 flashCtrlState_t：
 *             FLASH_CTRL_STS_ON 停在动作态（亮/鸣）、
 *             FLASH_CTRL_STS_OFF 与 FLASH_CTRL_STS_KEEP 都停在静止态（灭/静）。
 *             注意 KEEP 在这里与 flashCtrl_stop 不同：闪烁按节拍走完最后一圈本来就要切回静止态，
 *             所以 KEEP 只是“不额外干预执行器”，结果与 OFF 相同，不会把执行器留在动作态。
 *             想让灯亮着收尾就传 ON。
 * 会清零节拍计数与次数进度，并让句柄回到简单模式（等于退出灯语）；
 * 闪满次数后句柄仍在节拍链表里，但不再推进（可用 flashCtrl_isFlashDone 查询），
 * 需要重新开始时再调一次本接口或 flashCtrl_reset。
 * 库内用 FLASH_CTRL_CYCLE_MS 换算成拍（向上取整，非 0 毫秒至少 1 拍；0 毫秒 = 不进入该相位）。
 * 单位口径：本库对外只用毫秒；内部“拍”= flashCtrl_ticks() 的调用周期，
 *   它是集成参数（FLASH_CTRL_CYCLE_MS），与应用层自己的节拍无关。
 * 精度受拍长限制：实际时长 = 拍数 × FLASH_CTRL_CYCLE_MS（10ms 一拍时给 5ms 实际为 10ms）。 */
void flashCtrl_setThdMs(FlashCtrl* handle, uint16_t on_ms, uint16_t off_ms,
                        uint16_t cycles, flashCtrlState_t endState);

/* 简单模式是否已闪满设定次数：1 已闪满（此时停在 endState 指定的终态、不再推进），0 未闪满；
 * cycles 传 0（一直闪下去）、句柄为空或当前不在简单模式时恒返回 0。
 * 灯语模式的完成查询用 flashCtrl_isLangDone（口径是“跑满轮数”，与本接口不通用）。 */
uint8_t flashCtrl_isFlashDone(FlashCtrl* handle);

/* 简单模式已完成的闪烁次数（cycles=0 时是累计值，达到 65535 后饱和）；
 * 句柄为空或当前不在简单模式时返回 0。 */
uint16_t flashCtrl_getFlashCnt(FlashCtrl* handle);

/* 清零当前阶段的进度计数（简单模式清节拍计数与次数进度、灯语模式清阶段计时），
 * 不改变阈值、模式与执行器状态。闪满次数后调用它可以让同一套参数再跑一遍。 */
void flashCtrl_reset(FlashCtrl* handle);

/* 提交“启动”请求（异步）：句柄由 flashCtrl_ticks() 在下一拍挂入工作链表；
 * ticks 由调用方按固定周期调用（通常放在定时器中断里）。
 * 返回 0：请求已受理；-1：句柄非法（空指针或已释放）。
 * 同一拍内对同一句柄多次提交时，以最后一次为准（同字段后写覆盖前写）。 */
int flashCtrl_start(FlashCtrl* handle);

/* 提交“停止”请求（异步）：由 flashCtrl_ticks() 在下一拍摘链并按 offState 处理末态
 * （FLASH_CTRL_STS_KEEP 不改动执行器；ON/OFF 会切换状态并清零节拍计数）。 */
void flashCtrl_stop(FlashCtrl* handle, flashCtrlState_t offState);

/* ---------------- 两个“稳态”快捷接口（语义糖） ----------------
 * 作用：让“常亮 / 常灭”这个意图在调用处一眼可见，不必自己去凑阈值。
 * 实现上就是阈值的一个特例，句柄照旧留在节拍链表里：
 *   flashCtrl_turnOn (h) ≡ flashCtrl_setThdMs(h, 1, 0, 0, FLASH_CTRL_STS_KEEP)   → 熄灭态一拍内被点亮，之后保持
 *   flashCtrl_turnOff(h) ≡ flashCtrl_setThdMs(h, 0, 1, 0, FLASH_CTRL_STS_KEEP)   → 点亮态一拍内被熄灭，之后保持
 * 为什么这样可行（简单模式的判据）：
 *   - off_thd = 0 表示“不熄灭”，于是只要处于动作态就永远停在那里（on_thd 不参与判断）；
 *   - on_thd  = 0 表示“不点亮”，于是只要处于静止态就永远停在那里（off_thd 不参与判断）。
 *   - 那个 1 只是“让对应相位成立”的占位值（1ms 折算 1 拍，值本身不会被用到）。
 * 要点：
 *   - 同步：立即改写阈值并清节拍计数，下一拍由 flashCtrl_ticks 把执行器带到目标态；句柄必须已经 start，
 *     否则只改了阈值、没人推进；
 *   - 每拍都会按 is_on 纠偏：期间若有别的代码直接动了端口，下一拍会被拉回目标态；
 *   - 会覆盖之前的 on_thd / off_thd / cycles / endState（并切回简单模式，退出灯语），
 *     所以把有限次闪烁改成常亮后次数自然失效；想恢复闪烁要重新 setThdMs；
 *   - 句柄为 0（空指针）时安全返回。 */
void flashCtrl_turnOn(FlashCtrl* handle);
void flashCtrl_turnOff(FlashCtrl* handle);

/* 节拍调度：先执行主循环提交的启动/停止/释放请求（链表只在这里被改动，
 * 调用方无需再加临界区），再推进所有已挂链句柄的节拍。 */
void flashCtrl_ticks(void);

#if FLASH_CTRL_LANG_EN
/* ---------------- 灯语模式 ----------------
 * 轮数在触发时一次给定：跑满设定轮数后本灯语自停（停在熄灭态），由调用方检查跑完后切换下一个错误码，
 * 因此不会出现“底层还在跑、应用层已换成新灯语”的半途切换。
 *
 * 各段时长由 flashCtrl_setLangCfgMs 一次配置（全局，所有句柄共用），四个参数各管一段：
 *   longOn  → 每次长亮的“亮”段
 *   shortOn → 每次短亮的“亮”段
 *   gapOff  → 闪烁之间的“灭”段（长亮之间、短亮之间、长亮组与短亮组之间共用）
 *   leadOff → 首尾的“灭”段（触发时的起始熄灭、每轮结束的收尾熄灭共用）
 * 任一段传 0 表示跳过该段（0 时长阶段会在同一拍内被跳过，不会形成死循环）。
 *
 * 实际时序（与 flashCtrl_toggleLang / langAdvance 的实现逐段对应）：
 *   [可选] 起始熄灭(leadOff)   ← 仅当触发瞬间执行器正亮着且 leadOff>0；本来就灭则整段跳过
 *     → ([长亮(longOn) → 灭(gapOff)] × longTotal)      ← longTotal = 0 时整组不报
 *     → [组间熄灭(gapOff)]      ← 仅当“长亮组和短亮组都要报”时才插入这一段
 *     → ([短亮(shortOn) → 灭(gapOff)] × shortTotal)     ← shortTotal = 0 时整组不报
 *     → 收尾熄灭(leadOff)       ← 每轮结束都执行，它同时也是两轮之间的间隔
 *     → 未跑满 cycles：直接回到长亮组开始下一轮（本轮收尾已经灭过，不再重复起始熄灭）
 *       跑满 cycles：停在熄灭态、不再推进（应用层查 flashCtrl_isLangDone 后切下一个）
 * longTotal 与 shortTotal 都传 0：取消灯语（相位回空闲，不改动执行器）；
 * cycles 传 0：无限重复（用于常驻故障指示，需要 stop / exitLangMode 才结束）。
 * 注意“次数”的计法：一次长亮 = 亮段 + 其后的灭段，长亮次数在灭段结束时才 +1（短亮同理）。 */

/* 灯语时长全局配置（所有句柄共用一套，配置一次即可）。
 * 各段熄灭时长是共用的，因此只需四个参数：
 *   longOn:  长亮的点亮时长
 *   shortOn: 短亮的点亮时长
 *   gapOff:  每次闪烁之间的熄灭时长（长亮之间、短亮之间、长亮组与短亮组之间共用）
 *   leadOff: 首尾熄灭时长（触发时执行器正亮着就先灭这么久；一轮跑完的收尾熄灭也用它）
 * 传 0 毫秒表示该阶段被跳过。
 * 例：报“2 次长亮 + 2 次短亮”，长亮亮 300ms、短亮亮 100ms、闪烁间熄灭 200ms、首尾各熄 700ms：
 *     flashCtrl_setLangCfgMs(300, 100, 200, 700);  再 flashCtrl_startLang(handle, 2, 2, 1); */
void flashCtrl_setLangCfgMs(uint16_t longOnMs, uint16_t shortOnMs, uint16_t gapOffMs, uint16_t leadOffMs);

/* 开始灯语：设置本轮的长亮/短亮次数与需要重复的整轮数，并把轮数、进度清零后从头开始跑。
 * 已在跑灯语的句柄可直接再调一次换下一个灯语（旧进度作废，立即从新灯语的第一段开始）。
 *   timesOfLong / timesOfShort: 长亮、短亮次数，传 0 表示该组不报；
 *   cycles: 需要跑几整轮，跑满后自动停在熄灭态（等调用方查 flashCtrl_isLangDone 再切换）；
 *           传 0 表示无限重复（用于常驻故障指示，需要主动停/切才结束）。
 * 两个次数都传 0 表示取消灯语（回到空闲，不动执行器状态）。 */
void flashCtrl_startLang(FlashCtrl* handle, uint8_t timesOfLong, uint8_t timesOfShort, uint8_t cycles);

/* 退出灯语、回到简单模式：灯语进度清零，执行器恢复按 on_thd / off_thd 周期闪烁，
 * 阈值沿用切到灯语模式之前设置的值（不必重设）。 */
void flashCtrl_exitLangMode(FlashCtrl* handle);

/* 灯语是否已跑完设定轮数：1 已跑完（此时执行器停在熄灭态、不再推进），0 还在跑；
 * cycles 传 0（无限重复）或当前不在灯语模式时，恒返回 0。 */
uint8_t flashCtrl_isLangDone(FlashCtrl* handle);

/* 查询已完成的整轮数（65535 封顶，跑满设定轮数后不再增长）。 */
uint16_t flashCtrl_getLangCycle(FlashCtrl* handle);

/* 只清零已完成的轮数（典型用途：同一错误码重新计次/重新触发前先清）。 */
void flashCtrl_resetLangCycle(FlashCtrl* handle);
#endif

#ifdef __cplusplus
}
#endif

#endif /* d25787ac_53ee_1f4b_eedd_3ee716ceec1a */
