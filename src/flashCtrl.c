#include "flashCtrl.h"

typedef void(io_ctrl_func)();

/*
我想把关于闪烁的控制逻辑封装成一个通用的函数，方便在不同的项目中复用，不关心控制的是LED灯，还是蜂鸣器，还是其它执行器。

关于执行器的控制动作，由调用方传入一个函数指针（类型：io_ctrl_func），执行的具体动作由调用方维护。

下面的函数 led0_toggle() 是一个控制led灯的具体的实现逻辑，按此实现逻辑，封装成一个通用的函数 flashCtrl_toggle()，由调用方传入一个函数指针，执行具体的动作。

控制过程需要用到的参数（led0_on_thd， led0_off_thd， led0_on_cnt， led0_off_cnt）通过注册函数由库内维护，模式参考：f:\Users\TmpFolder\multi_button\multi_button.c/h。

控制过程需要用到的方法（set_P42， clear_P42， getP42Sts），同样通过注册函数由库内登记记录。

*/

/* 板载 LED 节拍函数：由调用方按固定周期调用（如主循环每 10ms 一次）
 * 按 led0_setThd 设置的阈值更新 LED 状态：
 *   - onThd=0：不点亮（当前熄灭则保持熄灭）；
 *   - offThd=0：不熄灭（当前点亮则保持点亮）；
 *   - 两者均 0：仅累计亮/灭节拍数，不改变 LED 状态；
 *   - 均非 0：灭 offThd 拍后点亮、亮 onThd 拍后熄灭，循环闪烁。 */
void led0_toggle(void){
    uint8_t P42Sts = getP42Sts();  // 获取板载LED当前状态（0：点亮，1：熄灭）
    if(led0_on_thd == 0 && led0_off_thd == 0){
        // 未设置阈值（0,0）：仅统计亮/灭节拍数，不改变 LED 状态
        if(1 == P42Sts){  // 板载LED当前处于熄灭状态
            led0_off_cnt ++;
        } else {  // 板载LED当前处于点亮状态
            led0_on_cnt ++;
        }
    }else if(1 == P42Sts){   // 板载LED当前处于熄灭状态
        if(led0_on_thd == 0){   // 点亮阈值为0，则保持熄灭状态，不点亮
            led0_off_cnt ++;
        }else if(led0_off_cnt >= led0_off_thd){
            clear_P42 = 0;    // P42拉低，板载LED点亮

            led0_off_cnt = 0;
            led0_on_cnt = 0;
        }else{
            led0_off_cnt++;
        }
    } else {  // 板载LED当前处于点亮状态
        if(led0_off_thd == 0){   // 熄灭阈值为0，则保持点亮状态，不熄灭
            led0_on_cnt ++;
        }else if(led0_on_cnt >= led0_on_thd){
            set_P42 = 1;    // P42拉高，板载LED熄灭

            led0_off_cnt = 0;
            led0_on_cnt = 0;
        } else {
            led0_on_cnt++;
        }
    }
}