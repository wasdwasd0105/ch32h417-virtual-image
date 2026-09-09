/* Status page on the kit's 1.54" LCD: card, mounted image, USB link, live rates. */
#ifndef DISPLAY_H
#define DISPLAY_H
void display_init(void);   /* bring the panel up and show the boot banner */
void display_poll(void);   /* call from the idle loops; redraws at most every DISPLAY_PERIOD_MS */
void display_reinit(void);        /* re-initialise the panel and redraw everything */
void display_test_pattern(void);  /* red / green / blue full screens, then the page */
void display_toggle_diag(void);   /* console 'D': continuous red/green/blue/page cycle on/off */
void display_cycle_clock(void);   /* halve the SPI clock limit (wraps to 50 MHz) and re-init */
#endif
