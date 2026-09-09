/* The board's two LEDs (schematic "LED" block): D1 blue and D2 green, each from VDDIO
 * through 1 k to an MCU pin -- the pin sinks, so a LOW turns the LED on.
 *   blue  (PC3): read/write activity, blinks while the host moves data
 *   green (PC2): an image is mounted */
#ifndef LEDS_H
#define LEDS_H
void leds_init(void);
void leds_activity(void);   /* call per READ / WRITE command */
void leds_poll(void);       /* from the idle loop */
#endif
