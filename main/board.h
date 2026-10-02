#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* Waveshare ESP32-S3-Touch-LCD-4.3 (non-B, SKU 25948) */
#define BOARD_LCD_W        800
#define BOARD_LCD_H        480

#define BOARD_I2C_SDA      8
#define BOARD_I2C_SCL      9
#define BOARD_TP_INT       4

/*
 * Backlight brightness: PWM into the MP3302 EN pin through an RC filter (1 kOhm + C12 1 uF), giving the
 * analog dimming voltage (~0.7 V dark .. ~1.4 V full). Needs a board rework: R10 disconnected from DISP
 * and J6 pin 3 (AD = IO6) wired to the EN side of R10 through ~1 kOhm. DISP itself also enables the LCD
 * panel (LCD connector pin 31), so it must never carry PWM: it stays on CH422G EXIO2 (R21).
 * Set to -1 on a stock board: on/off via CH422G (DISP drives both the panel and MP3302 EN).
 * bootloader_components/usb_dl_window/hooks.c holds this pin low during boot as well.
 */
#define BOARD_BL_PWM_GPIO  6
/* Brightness after power-up, 0..100 (the host reads it as the initial backlight value) */
#define BOARD_BL_DEFAULT_PERCENT  10

/*
 * Orientation of the firmware's own screens ("<NO SIGNAL>"); the host's rotation is invisible to the
 * device, so match it here. Same meaning as the Xorg fbdev "Rotate" option: CCW = the picture is turned
 * 90 degrees counterclockwise on the panel.
 */
#define BOARD_UI_ROTATE_NONE  0
#define BOARD_UI_ROTATE_CCW   1
#define BOARD_UI_ROTATE_CW    2
#define BOARD_UI_ROTATE_UD    3
#ifndef BOARD_UI_ROTATION                     /* tools/release.sh builds every variant */
#define BOARD_UI_ROTATION     BOARD_UI_ROTATE_CCW
#endif

/* CH422G output bits: EXIOn is wired to CH422G IOn (schematic), IO0 is not connected */
#define EXIO_CTP_RST       (1u << 1)
#define EXIO_DISP          (1u << 2)   /* LCD panel enable (LCD pin 31); stock board: also MP3302 EN via R10 */
#define EXIO_LCD_RST       (1u << 3)
#define EXIO_SD_CS         (1u << 4)
#define EXIO_USB_SEL       (1u << 5)   /* MUST stay 0: 0 = ESP32-S3 native USB, 1 = CAN transceiver */

esp_err_t board_init(void);
i2c_master_bus_handle_t board_i2c(void);

/* Modify CH422G outputs: outputs = (outputs & ~mask) | (value & mask). USB_SEL is always forced to 0. */
esp_err_t board_exio_set(uint8_t mask, uint8_t value);
/* Backlight on/off (keeps the brightness level) */
void board_backlight(bool on);
/* Brightness 0..100 (0 = dark even when on); stock board: anything > 0 means on */
void board_backlight_brightness(uint8_t percent);
