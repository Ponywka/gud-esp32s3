#include <string.h>
#include "lcd.h"
#include "board.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"

static const char *TAG = "lcd";
static esp_lcd_panel_handle_t s_panel;
static uint8_t *s_fb;

/* Pixel clock. Waveshare's demo uses 16 MHz; 14 MHz leaves headroom for the PSRAM bounce path
 * while USB updates write to the framebuffer (~34 Hz refresh). */
#define LCD_PCLK_HZ  (14 * 1000 * 1000)

esp_err_t lcd_init(void)
{
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = LCD_PCLK_HZ,
            .h_res = BOARD_LCD_W,
            .v_res = BOARD_LCD_H,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags = { .pclk_active_neg = 1 },
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,
        .bounce_buffer_size_px = BOARD_LCD_W * 20,   /* 2 x 32 KB internal SRAM; 480 / 20 divides evenly */
        .psram_trans_align = 64,
        .hsync_gpio_num = 46,
        .vsync_gpio_num = 3,
        .de_gpio_num = 5,
        .pclk_gpio_num = 7,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            14, 38, 18, 17, 10,        /* B3..B7 */
            39, 0, 45, 48, 47, 21,     /* G2..G7 */
            1, 2, 42, 41, 40,          /* R3..R7 */
        },
        .flags = { .fb_in_psram = 1 },
    };

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    void *fb = NULL;
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, &fb));
    s_fb = fb;
    ESP_LOGI(TAG, "RGB panel up, framebuffer %p", s_fb);
    return ESP_OK;
}

uint8_t *lcd_framebuffer(void) { return s_fb; }

void lcd_draw_test_pattern(void)
{
    static const uint16_t bars[8] = { 0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000 };
    uint16_t *px = (uint16_t *) s_fb;
    for (int y = 0; y < BOARD_LCD_H; y++) {
        for (int x = 0; x < BOARD_LCD_W; x++) {
            uint16_t c = bars[x * 8 / BOARD_LCD_W];
            if (y >= BOARD_LCD_H * 3 / 4) {          /* bottom quarter: grey ramp */
                uint16_t g = (uint16_t) (x * 32 / BOARD_LCD_W);
                c = (uint16_t) ((g << 11) | ((g * 2) << 5) | g);
            }
            if (x == 0 || y == 0 || x == BOARD_LCD_W - 1 || y == BOARD_LCD_H - 1)
                c = 0xFFFF;                             /* 1px border: shows any cropping */
            px[y * BOARD_LCD_W + x] = c;
        }
    }
}
