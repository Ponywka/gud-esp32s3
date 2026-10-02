#pragma once
#include <stdbool.h>

/* Show "<NO SIGNAL>" with the backlight on and arm the no-signal sleep timeout. Call after lcd_init(). */
void screen_init(void);

/* The host is sending a picture (GUD SET_BUFFER). */
void screen_picture(void);

/* Host display enable/disable (DPMS). */
void screen_dpms(bool on);

/* The host stopped driving the display (USB deconfigured, gud controller disabled). */
void screen_signal_lost(void);
