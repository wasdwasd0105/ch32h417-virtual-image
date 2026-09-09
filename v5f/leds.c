#include "ch32h417.h"
#include "app_config.h"
#include "leds.h"
#include "image_store.h"
#include "systime.h"

static uint32_t last_act_us;
static bool     act_on, img_on, inited;

static void set(GPIO_TypeDef *port, uint16_t pin, bool on)
{
    if (on ^ (LED_ACTIVE_LOW != 0))
        GPIO_SetBits(port, pin);
    else
        GPIO_ResetBits(port, pin);
}

void leds_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    RCC_HB2PeriphClockCmd(RCC_HB2Periph_GPIOC, ENABLE);
    gpio.GPIO_Pin   = GPIO_Pin_2 | GPIO_Pin_3;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_High;
    GPIO_Init(GPIOC, &gpio);
    set(GPIOC, GPIO_Pin_3, false);
    set(GPIOC, GPIO_Pin_2, false);
    last_act_us = systime_us() - 1000000u;
    inited = true;
}

void leds_activity(void)
{
    last_act_us = systime_us();
}

void leds_poll(void)
{
    bool busy, a, i;
    uint32_t now;
    if (!inited)
        return;
    now = systime_us();
    busy = (uint32_t)(now - last_act_us) < LED_ACT_HOLD_MS * 1000u;
    a = busy && ((now / (LED_BLINK_MS * 1000u)) & 1u);   /* blink while data moves */
    i = img_cur.mounted;
    if (a != act_on) { act_on = a; set(GPIOC, GPIO_Pin_3, a); }
    if (i != img_on) { img_on = i; set(GPIOC, GPIO_Pin_2, i); }
}
