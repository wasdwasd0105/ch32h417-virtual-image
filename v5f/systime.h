/* Free-running 1 MHz 32-bit time base on TIM9 (TIM2..5 are 16-bit on this part). */
#ifndef SYSTIME_H
#define SYSTIME_H
#include <stdint.h>
void systime_init(void);
uint32_t systime_us(void);
static inline uint32_t systime_elapsed_us(uint32_t since) { return systime_us() - since; }
#endif
