#include "systime.h"
#include "debug.h"

void systime_init(void)
{
    RCC_ClocksTypeDef clocks;
    TIM9_12_TimeBaseInitTypeDef tb = {0};
    RCC_HB2PeriphClockCmd(RCC_HB2Periph_TIM9, ENABLE);
    RCC_GetClocksFreq(&clocks);
    tb.TIM_Period = 0xFFFFFFFFu;
    tb.TIM_Prescaler = (uint16_t)(clocks.HCLK_Frequency / 1000000u - 1u);
    tb.TIM_ClockDivision = TIM_CKD_DIV1;
    tb.TIM_CounterMode = TIM_CounterMode_Up;
    TIM9_12_TimeBaseInit(TIM9, &tb);
    TIM9->CTLR1 |= TIM_CEN;
}

uint32_t systime_us(void)
{
    return TIM9->CNT_32;
}
