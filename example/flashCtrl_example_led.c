/* SPDX-License-Identifier: MIT */
/*
 * flashCtrl_example_led.c —— flashCtrl 库的调用方示例
 *
 * 本文件只作参考示例，不属于库本体，不参与库编译。
 * 示例等价于原来的 led0_toggle() 实现：库内只保留“闪烁”逻辑，
 * 具体的端口操作、状态读取全部由调用方（即本文件）注册进来；
 * 句柄由库内静态内存池分配，调用方只记录 flashCtrl_init 返回的指针。
 */

#include "flashCtrl.h"

/* ---------------- 平台相关部分：由调用方维护 ----------------
 * 这里以板载 LED 接在 P42（低电平点亮）为例，P42 为芯片 SFR，
 * 需要包含对应的器件头文件（如 STC8H.H）后使用。
 */
/* ---------------- 执行器一：板载 LED ---------------- */
#define LED0_ON()   (P42 = 0)                       /* P42 拉低，板载 LED 点亮 */
#define LED0_OFF()  (P42 = 1)                       /* P42 拉高，板载 LED 熄灭 */
#define LED0_ON_STS() ((uint8_t)(P42 ? 0 : 1))       /* 1：点亮（引脚为 0），0：熄灭 */

static FlashCtrl* led0 = 0;      /* 库内分配的句柄，外部只记录指针 */

static void led0_turn_on(void)    { LED0_ON();  }
static void led0_turn_off(void)   { LED0_OFF(); }
static uint8_t led0_is_on(void)   { return LED0_ON_STS(); }   /* 1=亮 / 0=灭，与 io_is_on_func 语义同向 */

/* ---------------- 执行器二：蜂鸣器（同一个库，换个执行器即可） ---------------- */
#define BEEP_ON()   (P10 = 0)
#define BEEP_OFF()  (P10 = 1)
#define BEEP_ON_STS() ((uint8_t)(P10 ? 0 : 1))     /* 1：鸣叫（引脚为 0），0：静默 */

static FlashCtrl* buzzer = 0;

static void buzzer_turn_on(void)    { BEEP_ON();  }
static void buzzer_turn_off(void)   { BEEP_OFF(); }
static uint8_t buzzer_is_on(void)  { return BEEP_ON_STS(); }

void main(void){
    led0 = flashCtrl_init(led0_turn_on, led0_turn_off, led0_is_on);   // 注册失败时返回 0（空指针），由调用方自行处理
    flashCtrl_setThdMs(led0, 50, 150);  // 亮 50ms、灭 150ms（10ms 一拍下即 5 拍 / 15 拍）
    flashCtrl_start(led0);              // 提交启动请求，由 flashCtrl_ticks() 在下一拍挂链（异步）

    buzzer = flashCtrl_init(buzzer_turn_on, buzzer_turn_off, buzzer_is_on);
    flashCtrl_setThdMs(buzzer, 20, 280);   // 鸣 20ms、停 280ms（10ms 一拍 → 2 拍 / 28 拍）
    flashCtrl_start(buzzer);            // 同上：请求式，下一拍生效

    // 配置灯语参数，长亮时间 / 短亮时间 / 闪烁间熄灭时间 / 首尾(两次灯语间间隔）熄灭时间
    flashCtrl_setLangCfgMs(300, 100, 200, 700);

    while(1){
        delay_ms(10);        /* 固定节拍周期，须与 FLASH_CTRL_CYCLE_MS 一致 */
        
        // 在实际的项目中，推荐在中断里调用 flashCtrl_ticks()，保证节拍周期稳定
        flashCtrl_ticks();   /* 所有已启动的执行器在此推进节拍 */

        // 运行中修改闪烁时长（按毫秒指定，内部按 FLASH_CTRL_CYCLE_MS 换算成节拍数）
        // flashCtrl_setThdMs(led0, on_ms, off_ms);

        // 需要“长亮 N 次 + 短亮 M 次”这类灯语报错时，同一个句柄直接切成灯语模式即可（模式互斥）
        // flashCtrl_langTrigger(led0, 3, 2, 2);        // 触发：3 次长亮 + 2 次短亮，报 2 轮后自停（cycles=0 为无限重复）

        // 灯语跑满设定轮数后自停（停在熄灭态），应用层在这里轮询：
        // 轮询慢一点只是让灯多灭一会儿，不会出现“报一半就被换掉”的情况
        // if (flashCtrl_isLangDone(led0)) {
        //     flashCtrl_langTrigger(led0, 3, 1, 2);     // 换下一个错误码，3长1短，报2次
        // }

        // 不再用灯语时切回周期闪烁（阈值沿用切走前的值，不用重设）
        // flashCtrl_exitLangMode(led0);

        // 可以随时停止闪烁，停在熄灭状态，句柄仍保留（可再次 start 继续闪）
        // flashCtrl_stop(buzzer, FLASH_CTRL_STS_IDLE);

        // 也可以彻底释放：先按指定末态停止（这里停成熄灭），再归还句柄（资源供其它执行器复用）
        // flashCtrl_deinit(led0, FLASH_CTRL_STS_IDLE);   /* 归还内存池资源；KEEP 则只回收资源、不动 LED */
    }
}
