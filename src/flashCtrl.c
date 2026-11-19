#include "flashCtrl.h"

/*
 * flashCtrl —— 通用节拍闪烁控制库
 *
 * 设计目标：把“闪烁”的控制逻辑封装成一个通用函数，方便在不同项目中复用，
 * 不关心控制的对象是 LED 灯、蜂鸣器还是其它执行器。
 *
 *   - 执行器的控制动作（点亮/熄灭、鸣叫/静默）由调用方通过函数指针注册，
 *     具体动作由调用方维护（见 flashCtrl_init 的 turn_on / turn_off）；
 *   - 执行器的当前状态由调用方通过函数指针注册（见 flashCtrl_init 的 get_sts）；
 *   - 控制过程用到的参数（on_thd、off_thd、on_cnt、off_cnt）由库内维护，
 *     通过注册函数 flashCtrl_setThdCnt（按节拍数）或 flashCtrl_setThdMs（按毫秒，
 *     内部用 FLASH_CTRL_CYCLE_MS 换算）设置，模式参考 multi_button 的
 *     button_init / button_attach / button_start / button_ticks 用法；
 *   - 句柄由库内静态内存池分配并返回，调用方只记录指针，避免同一句柄
 *     在多处被混用、重复初始化；
 *   - 内存池大小由用户项目的 userFlashCtrlCfg.h 中 FLASH_CTRL_ACTUATOR_NUM 决定，
 *     不需要动态内存，也不依赖 string.h 等库函数（不使用 memset）；
 *   - 对外只暴露必要接口：分配/释放、设置节拍、启停调度、查询用量；
 *     单个句柄的节拍处理 flashCtrl_toggle 是库内私有函数，不对外暴露。
 */

#define FLASH_CNT_MAX         0xFFFFu
#define FLASH_CTRL_SLOT_SIZE  ((uint16_t)sizeof(FlashCtrl))
#define FLASH_CTRL_POOL_SIZE  ((uint16_t)(FLASH_CTRL_ACTUATOR_NUM * FLASH_CTRL_SLOT_SIZE))

/* 节拍计数（饱和处理，防止长时间运行后回绕） */
#define FLASH_CNT_INC(cnt)    do { if((cnt) < FLASH_CNT_MAX) (cnt)++; } while(0)

/* 静态内存池：以 uint8_t 数组申请固定大小的内存（静态存储区），
 * 同时用联合体引入一个句柄成员，保证池首地址满足 FlashCtrl 的对齐要求。 */
typedef union {
    uint8_t   bytes[FLASH_CTRL_POOL_SIZE];   /* 按字节划分的内存池 */
    FlashCtrl alignUnit;                     /* 仅用于对齐，不参与使用 */
} FlashCtrlPool_t;

static FlashCtrlPool_t FLASH_CTRL_MEM_ATTR flashPool;   /* 静态内存池 */
static FlashCtrl* head_handle;                          /* 调度链表头 */

/* 取池中第 idx 个槽位的地址 */
static FlashCtrl* slotPtr(uint16_t idx) {
    return (FlashCtrl*)(void*)&flashPool.bytes[(uint16_t)(idx * FLASH_CTRL_SLOT_SIZE)];
}

/**
  * @brief  在静态内存池中分配一个闪烁控制句柄，并登记执行器函数。
  * @param  turn_on:  进入动作态的函数（如拉低端口点亮 LED），可传 NULL
  * @param  turn_off: 进入静止态的函数（如拉高端口熄灭 LED），可传 NULL
  * @param  get_sts:  读取当前状态的函数，返回 0：动作态，1：静止态，不可传 NULL
  * @retval 分配到的句柄；NULL：内存池已满（需调大 FLASH_CTRL_ACTUATOR_NUM）或参数非法
  */
FlashCtrl* flashCtrl_init(io_ctrl_func turn_on, io_ctrl_func turn_off, io_sts_func get_sts) {
    uint16_t i;
    FlashCtrl* handle;

    if(NULL == get_sts) return NULL;   // 状态读取函数是必须的

    for(i = 0; i < (uint16_t)FLASH_CTRL_ACTUATOR_NUM; i++){
        handle = slotPtr(i);
        if(0 == handle->inUse){
            // 显式逐项初始化，不依赖 memset
            handle->on_thd   = 0;
            handle->off_thd  = 0;
            handle->on_cnt   = 0;
            handle->off_cnt  = 0;
            handle->turn_on  = turn_on;
            handle->turn_off = turn_off;
            handle->get_sts  = get_sts;
            handle->next     = NULL;
            handle->inUse    = 1;
            return handle;
        }
    }

    return NULL;   // 池满
}

/**
  * @brief  释放句柄：先按 offState 停止节拍控制，再清空槽位，槽位可被后续 flashCtrl_init 复用。
  * @param  handle:   闪烁控制句柄
  * @param  offState: 停止时的执行器末态，口径与 flashCtrl_stop 一致；
  *                   FLASH_CTRL_STS_KEEP 表示只回收资源、不改动执行器状态
  * @retval None
  */
void flashCtrl_deinit(FlashCtrl* handle, flashCtrlOffState_t offState) {
    if(NULL == handle || 0 == handle->inUse) return;

    flashCtrl_stop(handle, offState);   // 先摘链（并按调用方要求处理执行器末态）

    handle->on_thd   = 0;
    handle->off_thd  = 0;
    handle->on_cnt   = 0;
    handle->off_cnt  = 0;
    handle->turn_on  = NULL;
    handle->turn_off = NULL;
    handle->get_sts  = NULL;
    handle->next     = NULL;
    handle->inUse    = 0;     // 槽位归还内存池
}

/**
  * @brief  查询内存池中已分配的句柄数量。
  * @param  None
  * @retval 已使用的槽位数
  */
uint16_t flashCtrl_getUsedCount(void) {
    uint16_t i;
    uint16_t used = 0;

    for(i = 0; i < (uint16_t)FLASH_CTRL_ACTUATOR_NUM; i++){
        if(slotPtr(i)->inUse) used++;
    }

    return used;
}

/**
  * @brief  查询内存池容量，即 FLASH_CTRL_ACTUATOR_NUM。
  * @param  None
  * @retval 可分配的执行器数量
  */
uint16_t flashCtrl_getCapacity(void) {
    return (uint16_t)FLASH_CTRL_ACTUATOR_NUM;
}

/**
  * @brief  按节拍数设置动作态/静止态的时长（注册过程参数），同时清零节拍计数。
  * @param  handle: 闪烁控制句柄
  * @param  on_thd: 动作态节拍数，0：不动作（保持静止）
  * @param  off_thd:静止态节拍数，0：不停止（保持动作）
  * @retval None
  */
void flashCtrl_setThdCnt(FlashCtrl* handle, uint16_t on_thd, uint16_t off_thd) {
    if(NULL == handle) return;

    handle->on_thd  = on_thd;
    handle->off_thd = off_thd;
    handle->on_cnt  = 0;
    handle->off_cnt = 0;
}

/**
  * @brief  按毫秒设置动作态/静止态时长，库内用 FLASH_CTRL_CYCLE_MS 换算成节拍数。
  *         换算向上取整，保证非 0 的毫秒数至少 1 拍；0 毫秒等价于 0 拍。
  * @param  handle: 闪烁控制句柄
  * @param  on_ms:  动作态时长，单位毫秒
  * @param  off_ms: 静止态时长，单位毫秒
  * @retval None
  */
void flashCtrl_setThdMs(FlashCtrl* handle, uint16_t on_ms, uint16_t off_ms) {
    if(NULL == handle) return;

    handle->on_thd  = FLASH_CTRL_CNT_OF_MS(on_ms);
    handle->off_thd = FLASH_CTRL_CNT_OF_MS(off_ms);
    handle->on_cnt  = 0;
    handle->off_cnt = 0;
}

/**
  * @brief  读取当前设置的节拍阈值。
  * @param  handle: 闪烁控制句柄
  * @param  on_thd: 输出动作态节拍数，可为 NULL
  * @param  off_thd:输出静止态节拍数，可为 NULL
  * @retval None
  */
void flashCtrl_getThdCnt(FlashCtrl* handle, uint16_t* on_thd, uint16_t* off_thd) {
    if(NULL == handle) return;

    if(NULL != on_thd)  *on_thd  = handle->on_thd;
    if(NULL != off_thd) *off_thd = handle->off_thd;
}

/**
  * @brief  清零节拍计数（不改变已设置的阈值与执行器当前状态）。
  * @param  handle: 闪烁控制句柄
  * @retval None
  */
void flashCtrl_reset(FlashCtrl* handle) {
    if(NULL == handle) return;

    handle->on_cnt  = 0;
    handle->off_cnt = 0;
}

/**
  * @brief  单个句柄的节拍处理（库内私有）：按 handle 中注册的阈值更新执行器状态，
  * @param  handle: 闪烁控制句柄
  * @retval None
  */
static void flashCtrl_toggle(FlashCtrl* handle) {
    uint8_t sts;

    if(NULL == handle || NULL == handle->get_sts) return;

    sts = handle->get_sts();   // 获取执行器当前状态（0：动作态，1：静止态）

    if(handle->on_thd == 0 && handle->off_thd == 0){
        // 未设置阈值（0,0）：仅统计动作/静止节拍数，不改变执行器状态
        if(1 == sts){
            FLASH_CNT_INC(handle->off_cnt);
        } else {
            FLASH_CNT_INC(handle->on_cnt);
        }
    } else if(1 == sts){   // 执行器当前处于静止态
        if(handle->on_thd == 0){   // 动作阈值为0，则保持静止状态，不进入动作态
            FLASH_CNT_INC(handle->off_cnt);
        } else if(handle->off_cnt >= handle->off_thd){
            if(NULL != handle->turn_on) handle->turn_on();   // 静止节拍已到，进入动作态

            handle->off_cnt = 0;
            handle->on_cnt  = 0;
        } else {
            FLASH_CNT_INC(handle->off_cnt);
        }
    } else {   // 执行器当前处于动作态
        if(handle->off_thd == 0){   // 停止阈值为0，则保持动作状态，不进入静止态
            FLASH_CNT_INC(handle->on_cnt);
        } else if(handle->on_cnt >= handle->on_thd){
            if(NULL != handle->turn_off) handle->turn_off();   // 动作节拍已到，进入静止态

            handle->off_cnt = 0;
            handle->on_cnt  = 0;
        } else {
            FLASH_CNT_INC(handle->on_cnt);
        }
    }
}

/**
  * @brief  启动闪烁控制，把句柄加入工作链表。
  * @param  handle: 闪烁控制句柄
  * @retval 0：成功；-1：句柄非法或已在链表中
  */
int flashCtrl_start(FlashCtrl* handle) {
    FlashCtrl* target;

    if(NULL == handle || 0 == handle->inUse) return -1;

    target = head_handle;
    while(target){
        if(target == handle) return -1;   // 已存在
        target = target->next;
    }

    handle->next = head_handle;
    head_handle  = handle;
    return 0;
}

/**
  * @brief  停止节拍控制：把句柄从工作链表中移除，并按 offState 指定执行器末态。
  * @param  handle:   闪烁控制句柄
  * @param  offState: FLASH_CTRL_STS_KEEP：保持执行器当前状态，不改动硬件；
  *                   FLASH_CTRL_STS_ACTIVE：停止后停在动作态（点亮/鸣叫）；
  *                   FLASH_CTRL_STS_IDLE：停止后停在静止态（熄灭/静默）。
  *                   指定 ACTIVE/IDLE 时会切换执行器状态并清零节拍计数。
  * @retval None
  */
void flashCtrl_stop(FlashCtrl* handle, flashCtrlOffState_t offState) {
    FlashCtrl** curr;

    if(NULL == handle) return;

    /* 先摘链，保证停止后 ticks 不再推进本句柄 */
    for(curr = &head_handle; *curr; ){
        FlashCtrl* entry = *curr;
        if(entry == handle){
            *curr = entry->next;
            entry->next = NULL;
            break;
        } else {
            curr = &entry->next;
        }
    }

    /* 再按调用方的要求决定停止时执行器的末态 */
    switch(offState){
    case FLASH_CTRL_STS_ACTIVE:
        if(NULL != handle->turn_on) handle->turn_on();
        handle->on_cnt  = 0;
        handle->off_cnt = 0;
        break;

    case FLASH_CTRL_STS_IDLE:
        if(NULL != handle->turn_off) handle->turn_off();
        handle->on_cnt  = 0;
        handle->off_cnt = 0;
        break;

    case FLASH_CTRL_STS_KEEP:
    default:
        break;   // 保持当前状态，不动执行器
    }
}

/**
  * @brief  节拍调度函数，由调用方按固定周期调用，遍历所有已启动的句柄。
  * @param  None
  * @retval None
  */
void flashCtrl_ticks(void) {
    FlashCtrl* target;

    for(target = head_handle; target; target = target->next){
        flashCtrl_toggle(target);
    }
}
