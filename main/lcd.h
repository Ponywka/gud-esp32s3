#pragma once
#include <stdint.h>
#include "esp_err.h"

esp_err_t lcd_init(void);
/* RGB565 framebuffer in PSRAM that the RGB peripheral scans out (BOARD_LCD_W * BOARD_LCD_H * 2 bytes) */
uint8_t *lcd_framebuffer(void);
void lcd_draw_test_pattern(void);
