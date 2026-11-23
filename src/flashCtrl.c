/* SPDX-License-Identifier: MIT */
#include "flashCtrl.h"

/*
 * flashCtrl —— 通用节拍闪烁控制库
 *
 * 设计目标：把“闪烁”的控制逻辑封装成一个通用函数，方便在不同项目中复用，
 * 不关心控制的对象是 LED 灯、蜂鸣器还是其它执行器。
 *
 *   - 执行器的控制动作（点亮/熄灭、鸣叫/静默）由调用方通过函数指针注册，
 *     具体动作由调用方维护（见 flashCtrl_init 的 turn_on / turn_off）；
 *   - 执行器的当前状态由调用方通过函数指针注册（见 flashCtrl_init 的 is_on）；
 *   - 控制过程用到的参数由库内维护，通过 flashCtrl_setThdMs（按毫秒，
 *     内部用 FLASH_CTRL_CYCLE_MS 换算成拍）设置，
 *     模式参考 multi_button 的 button_init / button_attach / button_start / button_ticks 用法；
 *   - 句柄由库内静态内存池分配并返回，调用方只记录指针，避免同一句柄
 *     在多处被混用、重复初始化；
 *   - 内存池大小由用户项目的 userFlashCtrlCfg.h 中 FLASH_CTRL_ACTUATOR_NUM 决定，
 *     不需要动态内存，也不依赖 string.h 等库函数（不使用 memset）；
 *   - 对外只暴露必要接口：分配句柄、设置时长与次数、启停调度；
 *     单个句柄的节拍处理 flashCtrl_toggle 是库内私有函数，不对外暴露。
 *     句柄数量由编译期配置决定、分配后不再归还，所以不提供动态释放与用量查询接口。
 *
 * 两种模式：
 *   1) 简单模式：亮 on_thd 拍、灭 off_thd 拍交替循环；起步时若当前亮着则先补满一个
 *      灭段再开始，若当前灭着则立刻点亮；闪满次数后按 endState 收尾（要停在 ON 的
 *      同样先补满一个灭段），保证任意两个亮段之间都有完整熄灭；
 *   2) 灯语模式：起始熄灭(可选) → [长亮]*N → 组间熄灭 → [短亮]*M → 收尾熄灭，
 *      轮数在触发时一次给定，跑满设定轮数后自停（停在熄灭态），调用方查 flashCtrl_isLangDone
 *      后再切换下一个错误码；cycles 传 0 表示无限重复（常驻故障指示）。
 *
 * 模式切换：flashCtrl_startLang 切到灯语模式，flashCtrl_exitLangMode（或 setThdMs）
 * 切回简单模式；简单模式的阈值存放在联合体外面，所以来回切换时阈值不会丢。
 */

#define FLASH_CNT_MAX                0xFFFFu
#define FLASH_CTRL_LANG_STS_UNKNOWN  0xFFu   /* 灯语状态缓存"未知"：强制下一次写端口 */

/* 节拍计数（饱和处理，防止长时间运行后回绕） */
#define FLASH_CNT_INC(cnt)    do { if((cnt) < FLASH_CNT_MAX) (cnt)++; } while(0)

#if FLASH_CTRL_LANG_EN
/* 灯语各阶段（仅库内使用） */
#define LANG_PH_OFF         0
#define LANG_PH_START_OFF    1   /* 触发后的起始熄灭 */
#define LANG_PH_LONG_ON      2
#define LANG_PH_LONG_OFF     3
#define LANG_PH_BETWEEN_OFF  4   /* 长亮组与短亮组之间 */
#define LANG_PH_SHORT_ON     5
#define LANG_PH_SHORT_OFF    6
#define LANG_PH_GAP_OFF      7   /* 一轮结束后的收尾熄灭 */

/* 灯语时长全局配置：所有句柄共用一套，开关灯语时也只需要配置一次 */
/* 灯语时长全局配置：所有句柄共用一套，开关灯语时也只需要配置一次。
 * 各段“熄灭”时长是共用的，因此只保留四个时长。 */
static uint16_t gLangLongOn;
static uint16_t gLangShortOn;
static uint16_t gLangGapOff;    /* 闪烁之间的熄灭（长亮之间、短亮之间、两组之间共用） */
static uint16_t gLangLeadOff;   /* 首尾熄灭（触发时的起始熄灭、一轮结束的收尾熄灭共用） */

/* 全局时长同样走"请求 + 影子"：主循环只写影子，由 ticks 落库 */
static volatile uint16_t gLangPend[4];   /* 待应用：longOn / shortOn / gapOff / leadOff（毫秒） */
static volatile uint8_t gLangCfgReq; /* 0：无；bit7：影子写入中；SET_LANG_MS：待应用 */
#endif

/* 静态内存池：直接声明成 FlashCtrl 数组。
 * 好处：类型正确（不再把 uint8_t 数组强转成结构体访问，绕开严格别名问题）、
 *       对齐由编译器保证、sizeof 与 stride 天然一致，
 *       而且调试器里可以直接看每个句柄的成员，不必自己算偏移。 */
static FlashCtrl FLASH_CTRL_MEM_ATTR flashPool[FLASH_CTRL_ACTUATOR_NUM];
static FlashCtrl* head_handle;                          /* 调度链表头 */

/* 池是否已做过"首次清零"。
 * 不依赖启动代码把 RAM 清 0：池若被指定到不清零的区域（noinit / pdata / 备份区），
 * inUse 与 pendingReq 的初值是垃圾，会被误判成"槽位已占用"（init 永远返回 0），
 * 或者让 serviceRequests 去处理垃圾请求。 */
static uint8_t poolInited;

/* ---- 空对象：句柄内永远存放“可调用”的函数 ----------------------------------------------
 * 函数指针与通用指针宽度不保证一致（C251 上 code 指针 2 字节、通用空指针 3 字节），
 * 直接把通用空指针赋给函数指针会截断并触发 C151 告警。改用“空实现”兜底后：
 *   1) 句柄里不存在空函数指针，ticks 内部不必逐拍判空；
 *   2) 调用方依旧可以传 0（空指针，见 flashCtrl_init），由 init 统一替换为空实现。 */
static void    flashCtrl_noOp(void)    { }                /* 空动作：什么也不做 */

/* ---- 请求码：调用方只写单个单字节，链表操作一律在 ticks 上下文里完成 ---- */
#define FLASH_REQ_NONE      0u
#define FLASH_REQ_START     1u
#define FLASH_REQ_STOP      2u

/* ---- 参数请求码（与启停分开一个字节，保证"先配参数再 start"两次提交都生效）----
 * 调用方（主循环）只写影子参数 + 这唯一一个单字节；参数落库、以及所有对执行器
 * 端口的写，全部由 ticks 在自己的上下文里完成。因此本库不需要临界区，也不依赖
 * 任何硬件（不用关中断）。
 * 位 7 是"影子正在写"标志：调用方先把它置起来、写完影子再写真正的命令码；
 * ticks 见到位 7、或拷贝影子前后两次读到的命令码不一致，就跳过这一拍、下一拍重来。
 * 单字节写在 8051 上也是原子的，所以这套 seqlock 式握手不需要互斥。 */
#define FLASH_CFG_NONE             0x00u
#define FLASH_CFG_BUSY             0x80u    /* 影子写入中 */
#define FLASH_CFG_SET_SIMPLE       0x01u    /* 简单模式参数 */
#define FLASH_CFG_START_LANG       0x02u    /* 触发灯语 */
#define FLASH_CFG_EXIT_LANG        0x03u    /* 退出灯语 */
#define FLASH_CFG_RESET            0x04u    /* 清进度 */
#define FLASH_CFG_RESET_LANG_CYCLE 0x05u    /* 清灯语轮数 */
#define FLASH_CFG_SET_LANG_MS      0x06u    /* 全局灯语时长（走 gLangCfgReq） */

static void clearProgress(FlashCtrl* handle);
static void doStart(FlashCtrl* handle);
static void doStop(FlashCtrl* handle, flashCtrlState_t offState);
static void serviceRequests(void);

/* 池首次使用前把"占用标志 + 挂起请求"清干净，不依赖 RAM 初值 */
static void ensurePoolInited(void) {
    uint16_t i;

    if(poolInited) return;

    for(i = 0; i < (uint16_t)FLASH_CTRL_ACTUATOR_NUM; i++){
        flashPool[i].inUse      = 0;
        flashPool[i].pendingReq = FLASH_REQ_NONE;
        flashPool[i].cfgReq     = FLASH_CFG_NONE;
        flashPool[i].phaseFresh = 0;
    }

    poolInited = 1;
}

/**
  * @brief  在静态内存池中分配一个闪烁控制句柄，并登记执行器函数。
  * @param  turn_on:  进入动作态的函数（如拉低端口点亮 LED），可传 0（空指针）
  * @param  turn_off: 进入静止态的函数（如拉高端口熄灭 LED），可传 0（空指针）
  * @param  is_on:    读取“当前是否处于动作态”的函数，返回 1：动作态（亮），0：静止态（灭），不可传 0
  * @retval 分配到的句柄；0：内存池已满（需调大 FLASH_CTRL_ACTUATOR_NUM）或参数非法
  */
FlashCtrl* flashCtrl_init(io_ctrl_func turn_on, io_ctrl_func turn_off, io_is_on_func is_on) {
    uint16_t i;
    FlashCtrl* handle;

    if(!is_on) return 0;    // 状态读取函数是必须的

    ensurePoolInited();

    for(i = 0; i < (uint16_t)FLASH_CTRL_ACTUATOR_NUM; i++){
        handle = &flashPool[i];
        if(0 == handle->inUse){
            // 显式逐项初始化，不依赖 memset
            /* 空函数指针一律用“空实现”兜底：调用方可以传空指针，句柄内不会留下空指针。
               注意：这里不要写成 (void*)0 —— C251 上通用指针 3 字节、code 指针 2 字节，
               跨类型赋值会截断并触发 C151 告警；三目判断 / 裸 0 都不会告警。 */
            handle->turn_on  = turn_on  ? turn_on  : flashCtrl_noOp;
            handle->turn_off = turn_off ? turn_off : flashCtrl_noOp;
            handle->is_on    = is_on;        /* 入口已判空，必为有效函数 */
            handle->mode     = FLASH_CTRL_MODE_SIMPLE;
            handle->next     = 0;

            handle->pendingReq      = FLASH_REQ_NONE;        /* 新槽位无挂起请求 */
            handle->pendingOffState = FLASH_CTRL_STS_KEEP;

            handle->inUse    = 1;    /* 最后置占用标志：此前 ticks 不会把它当成已分配槽位 */

            handle->on_thd  = 0;
            handle->off_thd = 0;
            handle->cycles  = 0;
            handle->endState = FLASH_CTRL_STS_KEEP;
            handle->u.simple.on_cnt   = 0;
            handle->u.simple.off_cnt  = 0;
            handle->u.simple.cycleCnt = 0;
            handle->u.simple.flashed  = 0;
            handle->u.simple.gap      = 0;
            handle->u.simple.finishOn = 0;
            handle->u.simple.done     = 0;
#if FLASH_CTRL_LANG_EN
            handle->u.lang.phaseCnt   = 0;
            handle->u.lang.cycleCnt   = 0;
            handle->u.lang.longTotal  = 0;
            handle->u.lang.shortTotal = 0;
            handle->u.lang.cycles     = 0;
            handle->u.lang.longDone   = 0;
            handle->u.lang.shortDone  = 0;
            handle->u.lang.phase      = LANG_PH_OFF;
            handle->u.lang.isOn       = FLASH_CTRL_LANG_STS_UNKNOWN;   /* 未知：第一次驱动一定写端口 */
#endif
            return handle;
        }
    }

    return 0;   // 池满
}

/**
  * @brief  设置简单模式参数：动作态/静止态时长（毫秒）、闪烁次数、闪满后的终态。
  *         换算向上取整，保证非 0 的毫秒数至少 1 拍；0 毫秒等价于 0 拍。
  * @param  handle:   闪烁控制句柄
  * @param  on_ms:    动作态时长，单位毫秒
  * @param  off_ms:   静止态时长，单位毫秒
  * @param  cycles:   需要闪烁几次，0 表示一直闪下去
  * @param  endState: 闪满次数后的终态（FLASH_CTRL_STS_KEEP / ON / OFF）
  * @retval None
  */
void flashCtrl_setThdMs(FlashCtrl* handle, uint16_t on_ms, uint16_t off_ms,
                        uint16_t cycles, flashCtrlState_t endState) {
    if(0 == handle || 0 == handle->inUse) return;

    /* 只写影子参数 + 一个单字节提交，不碰 ticks 正在读的任何字段 */
    handle->cfgReq = FLASH_CFG_BUSY;

    handle->cfg.simple.on_ms    = on_ms;
    handle->cfg.simple.off_ms   = off_ms;
    handle->cfg.simple.cycles   = cycles;
    handle->cfg.simple.endState = (uint8_t)endState;

    handle->cfgReq = FLASH_CFG_SET_SIMPLE;   /* 单字节写即"提交" */
}

/* 参数落库：只在 ticks 里执行 */
static void applySimple(FlashCtrl* handle, uint16_t on_ms, uint16_t off_ms,
                        uint16_t cycles, uint8_t endState) {
    handle->on_thd  = FLASH_CTRL_CNT_OF_MS(on_ms);
    handle->off_thd = FLASH_CTRL_CNT_OF_MS(off_ms);
    handle->cycles  = cycles;
    handle->endState = endState;

    handle->u.simple.on_cnt   = 0;
    handle->u.simple.off_cnt  = 0;
    handle->u.simple.cycleCnt = 0;
    handle->u.simple.flashed  = 0;
    handle->u.simple.gap      = 0;
    handle->u.simple.finishOn = 0;
    handle->u.simple.done     = 0;

    /* 模式最后写：先让进度数据落到"简单模式"的样子，再按简单模式解释 */
    handle->mode = FLASH_CTRL_MODE_SIMPLE;
}

/**
  * @brief  简单模式是否已闪满设定次数（此后停在 endState 指定的终态、不再推进）。
  * @param  handle: 闪烁控制句柄
  * @retval 1 已闪满 / 0 未闪满（含 cycles=0 一直闪、或当前不在简单模式）
  */
uint8_t flashCtrl_isFlashDone(FlashCtrl* handle) {
    if(0 == handle || 0 == handle->inUse) return 0;
    if(FLASH_CTRL_MODE_SIMPLE != handle->mode) return 0;

    return handle->u.simple.done;
}

/**
  * @brief  简单模式已完成的闪烁次数（cycles=0 时是累计值，达到 65535 后饱和）。
  * @param  handle: 闪烁控制句柄
  * @retval 已完成的次数
  */
uint16_t flashCtrl_getFlashCnt(FlashCtrl* handle) {
    if(0 == handle || 0 == handle->inUse) return 0;
    if(FLASH_CTRL_MODE_SIMPLE != handle->mode) return 0;

    return handle->u.simple.cycleCnt;
}

/**
  * @brief  清零当前模式的阶段进度（库内私有）：简单模式清节拍计数、已闪次数与“已闪满”标志，
  *         灯语模式清阶段计时（不改次数，灯语看 cycleCnt 判断跑完没有）。
  * @param  handle: 闪烁控制句柄
  * @retval None
  */
static void clearProgress(FlashCtrl* handle) {
    if(FLASH_CTRL_MODE_SIMPLE == handle->mode){
        handle->u.simple.on_cnt   = 0;
        handle->u.simple.off_cnt  = 0;
        handle->u.simple.cycleCnt = 0;
        handle->u.simple.flashed  = 0;
        handle->u.simple.gap      = 0;
        handle->u.simple.finishOn = 0;
        handle->u.simple.done     = 0;   /* 清掉“已闪满”标志，同一套参数可以再跑一遍 */
    }
#if FLASH_CTRL_LANG_EN
    else {
        handle->u.lang.phaseCnt = 0;
    }
#endif
}

/**
  * @brief  清零当前阶段的进度：简单模式清节拍计数、已闪次数与“已闪满”标志（可让同一套参数再跑一遍），
  *         灯语模式清阶段计时。不改变阈值、模式与执行器状态。
  * @param  handle: 闪烁控制句柄
  * @retval None
  */
void flashCtrl_reset(FlashCtrl* handle) {
    if(0 == handle || 0 == handle->inUse) return;

    handle->cfgReq = FLASH_CFG_RESET;   /* 单字节提交，由 ticks 下一拍清进度 */
}

/**
  * @brief  简单模式的节拍处理（库内私有）：等价于通用化后的 led0_toggle。
  *         起步与收尾都要考虑“前后状态的灭段连起来”，避免两个亮段首尾相接：
  *           - 起步时当前是灭的 → 立即点亮开始闪；
  *           - 起步时当前是亮的 → 先灭，补一个完整灭段（off_ms）再开始闪；
  *           - 闪满后要停在 ON  → 先补一个完整灭段，再点亮并停住；
  *           - 闪满后要停在 OFF → 亮段一结束就灭，直接停住。
  * @param  handle: 闪烁控制句柄
  * @retval None
  */
static void flashCtrl_toggle(FlashCtrl* handle) {
    uint8_t  isOn;                     // 执行器当前状态（1：动作态/亮，0：静止态/灭）
    uint16_t on_thd;
    uint16_t off_thd;

    /* 已闪满并停在终态：先返回，不调 is_on。
     * 闪满的句柄会一直留在链表里，若每拍还调一次 is_on，遇到 is_on 带副作用的场合
     * （清标志、软件消抖、ADC 采样）就是白白浪费，甚至误动作。 */
    if(handle->u.simple.done) return;

    isOn    = handle->is_on();
    on_thd  = handle->on_thd;
    off_thd = handle->off_thd;

    /* ---- 补灭段：起步时正亮着、或闪满后要停在 ON，都先补一个完整 off_ms 的灭段 ---- */
    if(handle->u.simple.gap){
        if(isOn){                     /* 中途被外部点亮：重新灭掉，灭段从头算 */
            handle->turn_off();
            handle->u.simple.off_cnt = 0;
        }

        /* 先计本拍，再判断是否走满：这样“补一个 off_ms”就是整 off_thd 拍，不多不少 */
        FLASH_CNT_INC(handle->u.simple.off_cnt);
        if(handle->u.simple.off_cnt < off_thd) return;

        handle->u.simple.off_cnt = 0;
        handle->u.simple.gap     = 0;
        handle->turn_on();
        handle->u.simple.flashed = 1;

        if(handle->u.simple.finishOn){
            handle->u.simple.done = 1;   /* 收尾：点亮后停住 */
        }
        return;
    }

    /* ---- 起步：按当前硬件状态决定先灭还是先亮 ---- */
    if(0 == handle->u.simple.flashed){
        handle->u.simple.off_cnt = 0;

        if(0 == on_thd){                    /* 不进入动作态（常灭型）：不占这一拍，
                                             * 直接落到下面的亮/灭判据里（turnOff 当拍就能灭） */
            handle->u.simple.flashed = 1;
        } else if(isOn && 0 != off_thd){    /* 当前亮着：先灭，补一个完整灭段再开始闪 */
            handle->turn_off();
            handle->u.simple.gap = 1;
            return;
        } else {                            /* 当前灭着：立即点亮开始闪 */
            handle->turn_on();
            handle->u.simple.flashed = 1;
            handle->u.simple.on_cnt  = 0;
            return;
        }
    }

    if(0 == isOn){   /* 执行器当前处于静止态：计本拍，走满 off_thd 拍就点亮 */
        FLASH_CNT_INC(handle->u.simple.off_cnt);

        if(0 != on_thd && handle->u.simple.off_cnt >= off_thd){
            handle->u.simple.off_cnt = 0;
            handle->turn_on();   // 灭段走完，进入动作态
        }
    } else {   /* 执行器当前处于动作态：计本拍，走满 on_thd 拍就熄灭 */
        FLASH_CNT_INC(handle->u.simple.on_cnt);

        if(0 == on_thd){   /* 没有动作段（常灭型）：不计次，把执行器灭掉即可 */
            handle->u.simple.on_cnt = 0;
            if(0 != off_thd){
                handle->turn_off();
            }
            return;
        }

        if(0 != off_thd && handle->u.simple.on_cnt >= on_thd){
            /* 亮段走完：立刻切回静止态，并计一次闪烁 */
            handle->u.simple.on_cnt  = 0;
            handle->u.simple.off_cnt = 0;
            handle->turn_off();
            FLASH_CNT_INC(handle->u.simple.cycleCnt);

            if(0 != handle->cycles && handle->u.simple.cycleCnt >= handle->cycles){
                if(FLASH_CTRL_STS_ON == handle->endState){
                    /* 收尾要点亮：先补一个完整灭段，否则最后一次亮会与收尾的亮连成一片 */
                    handle->u.simple.gap      = 1;
                    handle->u.simple.finishOn = 1;
                } else {
                    /* OFF / KEEP：亮段刚结束就已经灭好，直接停住 */
                    handle->u.simple.done = 1;
                }
            }
        }
    }
}

#if FLASH_CTRL_LANG_EN
/**
  * @brief  灯语：把执行器驱动到指定状态（1：动作态/亮，0：静止态/灭），状态未变则不重复写端口。
  * @param  handle: 闪烁控制句柄
  * @param  isOn:   目标状态（1 = 亮，0 = 灭）
  * @retval None
  */
static void langDrive(FlashCtrl* handle, uint8_t isOn) {
    /* 缓存为"未知"(0xFF) 时一定写一次端口：上电初值、外部直接改过端口、
     * stop 指定末态后又重新 start 的情况，都靠这一步重新同步硬件 */
    if(handle->u.lang.isOn == isOn) return;   // 避免每拍重复写端口

    if(1 == isOn){
        handle->turn_on();
    } else {
        handle->turn_off();
    }

    handle->u.lang.isOn = isOn;
}

/**
  * @brief  灯语：取某阶段的时长（节拍数）。
  * @param  phase: 阶段
  * @retval 该阶段的节拍数
  */
static uint16_t langDur(uint8_t phase) {
    switch(phase){
    case LANG_PH_START_OFF:
    case LANG_PH_GAP_OFF:     return gLangLeadOff;   /* 首尾熄灭共用 */
    case LANG_PH_LONG_OFF:
    case LANG_PH_BETWEEN_OFF:
    case LANG_PH_SHORT_OFF:   return gLangGapOff;    /* 闪烁之间的熄灭共用 */
    case LANG_PH_LONG_ON:     return gLangLongOn;
    case LANG_PH_SHORT_ON:    return gLangShortOn;
    default:                  return 0;
    }
}

/**
  * @brief  灯语：进入某阶段——执行该阶段的端口动作，并把阶段计时清零。
  * @param  handle: 闪烁控制句柄
  * @param  phase:  目标阶段
  * @retval None
  */
static void langEnter(FlashCtrl* handle, uint8_t phase) {
    switch(phase){
    case LANG_PH_LONG_ON:
    case LANG_PH_SHORT_ON:
        langDrive(handle, 1);   // 点亮 / 鸣叫（动作态）
        break;

    case LANG_PH_START_OFF:
    case LANG_PH_LONG_OFF:
    case LANG_PH_BETWEEN_OFF:
    case LANG_PH_SHORT_OFF:
    case LANG_PH_GAP_OFF:
        langDrive(handle, 0);   // 熄灭 / 静默（静止态）
        break;

    default:
        break;
    }

    handle->u.lang.phase    = phase;
    handle->u.lang.phaseCnt = 0;
}

/**
  * @brief  灯语：轮次开始时该先进哪个“亮”阶段（跳过次数为 0 的组）。
  * @param  handle: 闪烁控制句柄
  * @retval 阶段
  */
static uint8_t langFirstOnPhase(FlashCtrl* handle) {
    if(handle->u.lang.longTotal > handle->u.lang.longDone)   return LANG_PH_LONG_ON;
    if(handle->u.lang.shortTotal > handle->u.lang.shortDone) return LANG_PH_SHORT_ON;
    return LANG_PH_GAP_OFF;
}

/**
  * @brief  灯语：当前阶段到点后，进入下一阶段。
  * @param  handle: 闪烁控制句柄
  * @retval None
  */
static void langAdvance(FlashCtrl* handle) {
    switch(handle->u.lang.phase){
    case LANG_PH_START_OFF:
        langEnter(handle, langFirstOnPhase(handle));
        break;

    case LANG_PH_LONG_ON:
        langEnter(handle, LANG_PH_LONG_OFF);
        break;

    case LANG_PH_LONG_OFF:
        handle->u.lang.longDone++;
        if(handle->u.lang.longDone < handle->u.lang.longTotal){
            langEnter(handle, LANG_PH_LONG_ON);
        } else if(handle->u.lang.shortTotal > 0){
            langEnter(handle, LANG_PH_BETWEEN_OFF);   // 两组之间留一个间隔
        } else {
            langEnter(handle, LANG_PH_GAP_OFF);
        }
        break;

    case LANG_PH_BETWEEN_OFF:
        langEnter(handle, LANG_PH_SHORT_ON);
        break;

    case LANG_PH_SHORT_ON:
        langEnter(handle, LANG_PH_SHORT_OFF);
        break;

    case LANG_PH_SHORT_OFF:
        handle->u.lang.shortDone++;
        if(handle->u.lang.shortDone < handle->u.lang.shortTotal){
            langEnter(handle, LANG_PH_SHORT_ON);
        } else {
            langEnter(handle, LANG_PH_GAP_OFF);
        }
        break;

    case LANG_PH_GAP_OFF:
        // 一轮结束：计轮数
        FLASH_CNT_INC(handle->u.lang.cycleCnt);
        handle->u.lang.longDone  = 0;
        handle->u.lang.shortDone = 0;

        if(0 != handle->u.lang.cycles && handle->u.lang.cycleCnt >= handle->u.lang.cycles){
            // 已跑满设定轮数：停在熄灭态，不再推进（由调用方查 isLangDone 后切换下一个错误码）
            langEnter(handle, LANG_PH_OFF);
        } else {
            // 还没跑满（或 cycles 为 0 表示无限重复）：执行器此时仍是熄灭态，直接开始下一轮，
            // 因此不再插入“起始熄灭”
            langEnter(handle, langFirstOnPhase(handle));
        }
        break;

    default:
        langEnter(handle, LANG_PH_OFF);
        break;
    }
}

/**
  * @brief  灯语模式的节拍处理（库内私有）。
  * @param  handle: 闪烁控制句柄
  * @retval None
  */
static void flashCtrl_toggleLang(FlashCtrl* handle) {
    uint8_t guard = 0;
    uint8_t ticked = 0;   /* 本拍的“时间片”是否已经用于计时 */

    if(LANG_PH_OFF == handle->u.lang.phase){
        handle->phaseFresh = 0;
        return;
    }

    /* 本次相位是由参数请求在"本拍开头"落库并驱动端口的（见 applyLangStart / doStart），
     * 点亮与计时要占同一拍的话，每段都会比设定短一拍。这里把本拍的时间片当作
     * 已经用在"进入相位"上了：0 时长阶段照样在同一拍内跳过去，正常阶段从下一拍开始计时。 */
    if(handle->phaseFresh){
        handle->phaseFresh = 0;
        ticked = 1;
    }

    while(guard++ < 8){   // 防止 0 时长阶段形成死循环
        uint16_t dur = langDur(handle->u.lang.phase);

        if(0 == dur){
            // 0 时长阶段：同拍跳过，不占时间
            langAdvance(handle);
            if(LANG_PH_OFF == handle->u.lang.phase) return;
            continue;
        }

        if(ticked) return;   // 本拍已用完，新阶段从下一拍开始计时

        /* 先计本拍再判断：该阶段正好持续 dur 拍，切换发生在走满的那一拍 */
        FLASH_CNT_INC(handle->u.lang.phaseCnt);
        if(handle->u.lang.phaseCnt < dur) return;

        langAdvance(handle);   // 本阶段到点，本拍切换（phaseCnt 由 langEnter 清零）
        if(LANG_PH_OFF == handle->u.lang.phase) return;
        ticked = 1;
    }
}

/**
  * @brief  配置灯语的各段时长（全局，所有句柄共用），单位：毫秒。
  * @param  longOnMs:  长亮的点亮时长
  * @param  shortOnMs: 短亮的点亮时长
  * @param  gapOffMs:  闪烁之间的熄灭时长
  * @param  leadOffMs: 首尾熄灭时长
  * @retval None
  */
void flashCtrl_setLangCfgMs(uint16_t longOnMs, uint16_t shortOnMs, uint16_t gapOffMs, uint16_t leadOffMs) {
    /* 与句柄参数同一套办法：只写全局影子，由 ticks 落库 */
    gLangCfgReq = FLASH_CFG_BUSY;

    gLangPend[0] = longOnMs;
    gLangPend[1] = shortOnMs;
    gLangPend[2] = gapOffMs;
    gLangPend[3] = leadOffMs;

    gLangCfgReq = FLASH_CFG_SET_LANG_MS;
}

/* 全局灯语时长落库：只在 ticks 里执行 */
static void langCfgApply(void) {
    uint16_t a, b, c, d;

    if(FLASH_CFG_SET_LANG_MS != gLangCfgReq) return;

    a = gLangPend[0];
    b = gLangPend[1];
    c = gLangPend[2];
    d = gLangPend[3];

    if(FLASH_CFG_SET_LANG_MS != gLangCfgReq) return;   /* 拷贝期间被改写：下一拍重来 */

    gLangCfgReq = FLASH_CFG_NONE;

    gLangLongOn  = FLASH_CTRL_CNT_OF_MS(a);
    gLangShortOn = FLASH_CTRL_CNT_OF_MS(b);
    gLangGapOff  = FLASH_CTRL_CNT_OF_MS(c);
    gLangLeadOff = FLASH_CTRL_CNT_OF_MS(d);
}

/**
  * @brief  开始一轮灯语：设置本轮的长亮/短亮次数与需要重复的整轮数，轮数与进度清零后从头开始跑。
  * @param  handle:       闪烁控制句柄
  * @param  timesOfLong:  长亮次数（0：不报长亮组）
  * @param  timesOfShort: 短亮次数（0：不报短亮组；两者都为 0 表示取消灯语）
  * @param  cycles:       需要跑几整轮，跑满后自动停在熄灭态；0 表示无限重复
  * @retval None
  */
void flashCtrl_startLang(FlashCtrl* handle, uint8_t timesOfLong, uint8_t timesOfShort, uint8_t cycles) {
    if(0 == handle || 0 == handle->inUse) return;

    /* 只写影子 + 一个单字节提交；灯语的一切判断与端口动作都在 ticks 里做 */
    handle->cfgReq = FLASH_CFG_BUSY;

    handle->cfg.lang.longTotal  = timesOfLong;
    handle->cfg.lang.shortTotal = timesOfShort;
    handle->cfg.lang.cycles     = cycles;

    handle->cfgReq = FLASH_CFG_START_LANG;
}

/* 灯语触发落库：只在 ticks 里执行 */
static void applyLangStart(FlashCtrl* handle, uint8_t timesOfLong, uint8_t timesOfShort, uint8_t cycles) {
    /* 取消灯语：两个次数都为 0。此时不碰联合体、也不改模式，
     * 免得把正在跑的简单模式进度清掉。 */
    if(0 == timesOfLong && 0 == timesOfShort){
        if(FLASH_CTRL_MODE_LANG == handle->mode){
            handle->u.lang.phase      = LANG_PH_OFF;
            handle->u.lang.phaseCnt   = 0;
            handle->u.lang.longTotal  = 0;
            handle->u.lang.shortTotal = 0;
            handle->u.lang.cycles     = 0;
            handle->u.lang.longDone   = 0;
            handle->u.lang.shortDone  = 0;
        }
        return;
    }

    /* 防呆：四段时长一次都没配过（全 0）时，所有阶段都是 0 拍，ticks 会在当拍
     * 把它们全部跳过并立刻"跑满轮数"→ isLangDone 返回 1，而灯一下都不亮。
     * 应用层据此以为这个错误码已经报完，故障指示等于丢了。这里按"取消灯语"处理。 */
    if(0 == gLangLongOn && 0 == gLangShortOn && 0 == gLangGapOff && 0 == gLangLeadOff){
        if(FLASH_CTRL_MODE_LANG == handle->mode){
            handle->u.lang.phase      = LANG_PH_OFF;
            handle->u.lang.phaseCnt   = 0;
            handle->u.lang.longTotal  = 0;
            handle->u.lang.shortTotal = 0;
            handle->u.lang.cycles     = 0;
            handle->u.lang.longDone   = 0;
            handle->u.lang.shortDone  = 0;
            handle->u.lang.cycleCnt   = 0;
        }
        return;
    }

    handle->u.lang.longTotal  = timesOfLong;
    handle->u.lang.shortTotal = timesOfShort;
    handle->u.lang.cycles     = cycles;
    handle->u.lang.longDone   = 0;
    handle->u.lang.shortDone  = 0;
    handle->u.lang.cycleCnt   = 0;
    handle->u.lang.phaseCnt   = 0;
    handle->u.lang.isOn       = handle->is_on();     /* 状态读取函数由 init 保证有效（空实现返回 0） */

    if(1 == handle->u.lang.isOn && gLangLeadOff > 0){
        langEnter(handle, LANG_PH_START_OFF);   // 正亮着：先灭一下再开始
    } else {
        langEnter(handle, langFirstOnPhase(handle));   // 本就熄灭：不额外延长熄灭周期
    }

    /* 模式最后写：先让相位数据准备好，再按灯语解释（doStart 会据此重驱动端口） */
    handle->mode = FLASH_CTRL_MODE_LANG;

    handle->phaseFresh = 1;   /* 第一段端口是本拍驱动的，本拍不再重复计时 */
}

/**
  * @brief  退出灯语，回到简单模式：灯语进度清零，恢复按 on_thd / off_thd 周期闪烁，
  *         阈值沿用切到灯语模式之前设置的值。
  * @param  handle: 闪烁控制句柄
  * @retval None
  */
void flashCtrl_exitLangMode(FlashCtrl* handle) {
    if(0 == handle || 0 == handle->inUse) return;

    handle->cfgReq = FLASH_CFG_EXIT_LANG;   /* 单字节提交，下一拍生效 */
}

/* 退出灯语落库：只在 ticks 里执行 */
static void applyLangExit(FlashCtrl* handle) {
    handle->u.lang.phase    = LANG_PH_OFF;
    handle->u.lang.phaseCnt = 0;
    handle->u.lang.cycleCnt = 0;
    handle->u.lang.longTotal  = 0;
    handle->u.lang.shortTotal = 0;
    handle->u.lang.cycles     = 0;
    handle->u.lang.longDone   = 0;
    handle->u.lang.shortDone  = 0;
    handle->u.lang.isOn       = FLASH_CTRL_LANG_STS_UNKNOWN;

    /* 灯语与简单模式共用同一块内存：简单模式的进度逐项显式清零，
     * 不能指望"上面写的灯语字段刚好覆盖成 0"——布局一变就会漏掉某个标志。 */
    handle->u.simple.on_cnt   = 0;
    handle->u.simple.off_cnt  = 0;
    handle->u.simple.cycleCnt = 0;
    handle->u.simple.flashed  = 0;
    handle->u.simple.gap      = 0;
    handle->u.simple.finishOn = 0;
    handle->u.simple.done     = 0;

    /* 模式最后写：先按简单模式的口径把进度准备好再切 */
    handle->mode = FLASH_CTRL_MODE_SIMPLE;
}

/**
  * @brief  灯语是否已跑完设定轮数。
  * @param  handle: 闪烁控制句柄
  * @retval 1：已跑完（执行器停在熄灭态、不再推进）；
  *         0：还在跑，或 cycles 传了 0（无限重复），或当前不在灯语模式
  */
uint8_t flashCtrl_isLangDone(FlashCtrl* handle) {
    if(0 == handle || 0 == handle->inUse) return 0;
    if(FLASH_CTRL_MODE_LANG != handle->mode) return 0;
    if(0 == handle->u.lang.cycles) return 0;   // 无限重复，不存在“跑完”

    return (handle->u.lang.cycleCnt >= handle->u.lang.cycles) ? 1 : 0;
}

/**
  * @brief  查询灯语已完成的整轮数。
  * @param  handle: 闪烁控制句柄
  * @retval 已完成的整轮数（65535 封顶）；非灯语模式返回 0
  */
uint16_t flashCtrl_getLangCycle(FlashCtrl* handle) {
    if(0 == handle || 0 == handle->inUse) return 0;
    if(FLASH_CTRL_MODE_LANG != handle->mode) return 0;

    return handle->u.lang.cycleCnt;
}

/**
  * @brief  只清零灯语轮数，不影响正在跑的进度。
  * @param  handle: 闪烁控制句柄
  * @retval None
  */
void flashCtrl_resetLangCycle(FlashCtrl* handle) {
    if(0 == handle || 0 == handle->inUse) return;

    handle->cfgReq = FLASH_CFG_RESET_LANG_CYCLE;   /* 单字节提交，下一拍生效 */
}

/* 清灯语轮数落库：只在 ticks 里执行 */
static void applyResetLangCycle(FlashCtrl* handle) {
    if(FLASH_CTRL_MODE_LANG != handle->mode) return;

    /* 灯语已经跑满、停在空闲相位（LANG_PH_OFF）时不能再清轮数：
     * 清成 0 后 isLangDone 永久返回 0，而 phase 是 OFF、ticks 不再推进，
     * 轮数也永远涨不回来 —— 轮询 isLangDone 的应用层会一直卡在这里。
     * 想重新计次请直接再调一次 flashCtrl_startLang，它自己会把轮数清零并从头跑。 */
    if(LANG_PH_OFF == handle->u.lang.phase) return;

    handle->u.lang.cycleCnt = 0;
}
#endif /* FLASH_CTRL_LANG_EN */

/**
  * @brief  启动闪烁控制，把句柄加入工作链表。
  * @param  handle: 闪烁控制句柄
  * @retval 0：请求已受理（重复提交无害，已在链表中的会被忽略）；-1：句柄非法（空指针或未分配）
  */
int flashCtrl_start(FlashCtrl* handle) {
    if(0 == handle || 0 == handle->inUse) return -1;   // 入口判空：调用方可能传空指针

    handle->pendingReq = FLASH_REQ_START;   // 单字节写即“提交”，由 ticks 下一拍挂链
    return 0;
}

/* 实际的挂链动作：只在 flashCtrl_ticks() -> serviceRequests() 里执行 */
static void doStart(FlashCtrl* handle) {
    FlashCtrl* target;
    uint16_t   n;

    /* 遍历带槽位上界：即使某个句柄的 next 被写坏成自环，
     * 也不会在 ticks（可能是中断）里转成死循环 */
    for(n = 0, target = head_handle; target && n < (uint16_t)FLASH_CTRL_ACTUATOR_NUM;
        n++, target = target->next){
        if(target == handle) return;   // 已在链表中：重复请求忽略
    }

#if FLASH_CTRL_LANG_EN
    /* 灯语不读 is_on、只靠相位推进，所以重新挂链时必须自己把端口驱动到相位该有的状态；
     * 并且从头报一遍 —— 停过一次之后接着报"半截错误码"没有意义，
     * 否则恢复后可能剩半段不亮（该亮的段因为"相位没变、缓存也没变"而没人写端口）。 */
    if(FLASH_CTRL_MODE_LANG == handle->mode && LANG_PH_OFF != handle->u.lang.phase){
        handle->u.lang.phaseCnt   = 0;
        handle->u.lang.longDone   = 0;
        handle->u.lang.shortDone  = 0;
        handle->u.lang.cycleCnt   = 0;
        langEnter(handle, langFirstOnPhase(handle));
        handle->phaseFresh = 1;   /* 同上：端口是本拍驱动的，本拍不再重复计时 */
    }
#endif

    handle->next = head_handle;
    head_handle  = handle;
}

/**
  * @brief  停止节拍控制：把句柄从工作链表中移除，并按 offState 指定执行器末态。
  * @param  handle:   闪烁控制句柄
  * @param  offState: FLASH_CTRL_STS_KEEP：保持执行器当前状态，不改动硬件；
  *                   FLASH_CTRL_STS_ON：停止后停在动作态（点亮/鸣叫）；
  *                   FLASH_CTRL_STS_OFF：停止后停在静止态（熄灭/静默）。
  *                   指定 ON/OFF 时会切换执行器状态并清零节拍计数。
  * @retval None
  */
void flashCtrl_stop(FlashCtrl* handle, flashCtrlState_t offState) {
    if(0 == handle || 0 == handle->inUse) return;   // 入口判空：调用方可能传空指针

    handle->pendingOffState = (uint8_t)offState;
    handle->pendingReq      = FLASH_REQ_STOP;
}

/**
  * @brief  把执行器固定为“常亮”（动作态）：语义糖，等价于
  *         flashCtrl_setThdMs(handle, 1, 0, 0, FLASH_CTRL_STS_KEEP)。
  *         off_thd=0 表示“不熄灭”，故一旦进入动作态就保持；on_thd 的 1 只是占位。
  * @param  handle: 闪烁控制句柄（需已 start，否则只改阈值、无人推进）
  * @retval None
  */
void flashCtrl_turnOn(FlashCtrl* handle) {
    flashCtrl_setThdMs(handle, 1, 0, 0, FLASH_CTRL_STS_KEEP);
}

/**
  * @brief  把执行器固定为“常灭”（静止态）：语义糖，等价于
  *         flashCtrl_setThdMs(handle, 0, 1, 0, FLASH_CTRL_STS_KEEP)。
  *         on_thd=0 表示“不点亮”，故一旦进入静止态就保持；off_thd 的 1 只是占位。
  * @param  handle: 闪烁控制句柄（需已 start，否则只改阈值、无人推进）
  * @retval None
  */
void flashCtrl_turnOff(FlashCtrl* handle) {
    flashCtrl_setThdMs(handle, 0, 1, 0, FLASH_CTRL_STS_KEEP);
}

/* 实际的摘链 + 末态处理：只在 flashCtrl_ticks() -> serviceRequests() 里执行 */
static void doStop(FlashCtrl* handle, flashCtrlState_t offState) {
    FlashCtrl** curr;
    uint16_t    n;

    for(n = 0, curr = &head_handle; *curr && n < (uint16_t)FLASH_CTRL_ACTUATOR_NUM; n++){
        if(*curr == handle){
            *curr        = (*curr)->next;
            handle->next = 0;
            break;
        } else {
            curr = &(*curr)->next;
        }
    }

    switch(offState){
    case FLASH_CTRL_STS_ON:
        handle->turn_on();
#if FLASH_CTRL_LANG_EN
        /* 这里绕过了 langDrive，必须自己把灯语的状态缓存同步成末态 */
        if(FLASH_CTRL_MODE_LANG == handle->mode) handle->u.lang.isOn = 1;
#endif
        clearProgress(handle);
        break;

    case FLASH_CTRL_STS_OFF:
        handle->turn_off();
#if FLASH_CTRL_LANG_EN
        if(FLASH_CTRL_MODE_LANG == handle->mode) handle->u.lang.isOn = 0;
#endif
        clearProgress(handle);
        break;

    case FLASH_CTRL_STS_KEEP:
    default:
        break;
    }
}

/**
  * @brief  节拍调度函数，由调用方按固定周期调用，遍历所有已启动的句柄，
  *         并按句柄当前模式分派到简单模式 / 灯语模式的节拍处理。
  * @param  None
  * @retval None
  */
void flashCtrl_ticks(void) {
    FlashCtrl* target;
    uint16_t   n;

    /* 1) 先把主循环提交的启动/停止请求执行掉：
       链表只在这里被改动，因此调用方不必自己做临界区，也不存在
       “ticks 正在遍历、主循环同时摘链”的竞争 */
    serviceRequests();

    /* 2) 再推进所有已挂链句柄的节拍（同样加槽位上界，防 next 被写坏时死循环） */
    for(n = 0, target = head_handle; target && n < (uint16_t)FLASH_CTRL_ACTUATOR_NUM;
        n++, target = target->next){
#if FLASH_CTRL_LANG_EN
        if(FLASH_CTRL_MODE_LANG == target->mode){
            flashCtrl_toggleLang(target);
        } else
#endif
        {
            flashCtrl_toggle(target);
        }
    }
}

/* 参数请求处理：拷贝影子 → 复核命令码 → 认领 → 落库。
 * 拷贝前后都读一次 cfgReq，只要变了（说明调用方正在写新的影子）就放弃这一拍，
 * 下一拍重来；已经落库的数据全部来自本地副本，不会再被调用方改写。
 * 本函数只在 ticks 上下文里跑，调用方（主循环）同时可以随便写影子。 */
static void serviceCfg(FlashCtrl* handle) {
    uint8_t  code;
    uint16_t a, b, c;
    uint8_t  d;

    if(0 == handle->inUse) return;

    code = handle->cfgReq;
    if(FLASH_CFG_NONE == code || (FLASH_CFG_BUSY & code)) return;

    switch(code){
    case FLASH_CFG_SET_SIMPLE:
        a = handle->cfg.simple.on_ms;
        b = handle->cfg.simple.off_ms;
        c = handle->cfg.simple.cycles;
        d = handle->cfg.simple.endState;
        if(code != handle->cfgReq) return;
        handle->cfgReq = FLASH_CFG_NONE;
        applySimple(handle, a, b, c, d);
        break;

#if FLASH_CTRL_LANG_EN
    case FLASH_CFG_START_LANG:
        a = (uint16_t)handle->cfg.lang.longTotal;
        b = (uint16_t)handle->cfg.lang.shortTotal;
        c = (uint16_t)handle->cfg.lang.cycles;
        if(code != handle->cfgReq) return;
        handle->cfgReq = FLASH_CFG_NONE;
        applyLangStart(handle, (uint8_t)a, (uint8_t)b, (uint8_t)c);
        break;

    case FLASH_CFG_EXIT_LANG:
        handle->cfgReq = FLASH_CFG_NONE;
        applyLangExit(handle);
        break;

    case FLASH_CFG_RESET_LANG_CYCLE:
        handle->cfgReq = FLASH_CFG_NONE;
        applyResetLangCycle(handle);
        break;
#endif

    case FLASH_CFG_RESET:
        handle->cfgReq = FLASH_CFG_NONE;
        clearProgress(handle);
        break;

    default:                            /* 未知码：丢弃，免得卡在同一个值上 */
        handle->cfgReq = FLASH_CFG_NONE;
        break;
    }
}

/* 请求处理：逐槽位读取 pendingReq（单字节读，不会读到“半个指针”）。
 * 因链表操作全部收在本函数里，本函数与调用方必须同上下文（即 ticks 所在的中断/主循环），
 * 与调用方提交请求的上下文可以不同。 */
static void serviceRequests(void) {
    uint16_t i;
    FlashCtrl* h;
    uint8_t req, off;

    ensurePoolInited();

#if FLASH_CTRL_LANG_EN
    /* 灯语全局时长先落库：这样"同一拍里先 setLangCfgMs 再 startLang"用的是新时长 */
    langCfgApply();
#endif

    for(i = 0; i < (uint16_t)FLASH_CTRL_ACTUATOR_NUM; i++){
        h = &flashPool[i];

        /* 顺序：先落参数、再处理启停 —— 这样"setThdMs + start"同拍提交时，
         * 挂链那一刻用的已经是新参数 */
        serviceCfg(h);

        req = h->pendingReq;
        if(FLASH_REQ_NONE == req) continue;

        /* 先认领（清掉）再执行：
         * 执行期间调用方若又提交了一个请求，会写回 pendingReq、留到下一拍；
         * 反过来"执行完再比较值是否相同"的写法，会把执行期间提交的同值请求
         * （例如两次 stop 只是末态参数不同）当成"本次已处理"而静默丢弃，
         * 结果执行器停在旧的末态。 */
        h->pendingReq = FLASH_REQ_NONE;

        if(0 == h->inUse){                     /* 槽位空闲（未分配）：请求已丢弃 */
            continue;
        }

        off = h->pendingOffState;
        switch(req){
        case FLASH_REQ_START:  doStart(h);                            break;
        case FLASH_REQ_STOP:   doStop(h, (flashCtrlState_t)off);      break;
        default:                                                      break;
        }
    }
}
