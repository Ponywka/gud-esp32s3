#include <string.h>
#include "touch.h"
#include "board.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "gt911";

/* Orientation fixes, applied after scaling to display pixels */
#define TOUCH_SWAP_XY   0
#define TOUCH_MIRROR_X  0
#define TOUCH_MIRROR_Y  0
#define TOUCH_POLL_MS   5

#define GT911_REG_STATUS   0x814E
#define GT911_REG_POINTS   0x814F
#define GT911_REG_XMAX     0x8048   /* X max lo, hi, Y max lo, hi */

static i2c_master_dev_handle_t s_dev;
static uint16_t s_xmax = BOARD_LCD_W, s_ymax = BOARD_LCD_H;
static touch_cb_t s_cb;

static esp_err_t gt_read(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t a[2] = { (uint8_t) (reg >> 8), (uint8_t) reg };
    return i2c_master_transmit_receive(s_dev, a, 2, buf, len, 50);
}

static esp_err_t gt_write8(uint16_t reg, uint8_t v)
{
    uint8_t d[3] = { (uint8_t) (reg >> 8), (uint8_t) reg, v };
    return i2c_master_transmit(s_dev, d, 3, 50);
}

esp_err_t touch_init(void)
{
    /* Address select: INT level while RST rises picks 0x5D (low) or 0x14 (high) */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_TP_INT,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(BOARD_TP_INT, 0);
    board_exio_set(EXIO_CTP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    board_exio_set(EXIO_CTP_RST, EXIO_CTP_RST);
    vTaskDelay(pdMS_TO_TICKS(10));
    io.mode = GPIO_MODE_INPUT;
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(60));

    uint16_t addr = 0x5D;
    if (i2c_master_probe(board_i2c(), addr, 50) != ESP_OK) {
        addr = 0x14;
        if (i2c_master_probe(board_i2c(), addr, 50) != ESP_OK) {
            ESP_LOGE(TAG, "GT911 not found on 0x5D/0x14");
            return ESP_ERR_NOT_FOUND;
        }
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(board_i2c(), &dev_cfg, &s_dev));

    uint8_t id[4] = { 0 };
    if (gt_read(0x8140, id, 4) == ESP_OK)
        ESP_LOGI(TAG, "GT911 @0x%02x product id '%c%c%c%c'", addr, id[0], id[1], id[2], id[3]);

    uint8_t cfg[4];
    if (gt_read(GT911_REG_XMAX, cfg, 4) == ESP_OK) {
        uint16_t xm = (uint16_t) (cfg[0] | (cfg[1] << 8));
        uint16_t ym = (uint16_t) (cfg[2] | (cfg[3] << 8));
        if (xm && ym) {
            s_xmax = xm;
            s_ymax = ym;
        }
    }
    ESP_LOGI(TAG, "touch range %ux%u", s_xmax, s_ymax);
    gt_write8(GT911_REG_STATUS, 0);
    return ESP_OK;
}

static void touch_task(void *arg)
{
    (void) arg;
    touch_point_t pts[TOUCH_MAX_POINTS];
    int last_n = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));

        uint8_t st;
        if (gt_read(GT911_REG_STATUS, &st, 1) != ESP_OK)
            continue;
        if (!(st & 0x80))
            continue;   /* no new data */

        int n = st & 0x0F;
        if (n > TOUCH_MAX_POINTS)
            n = TOUCH_MAX_POINTS;

        uint8_t raw[8 * TOUCH_MAX_POINTS];
        if (n > 0 && gt_read(GT911_REG_POINTS, raw, (size_t) n * 8) != ESP_OK)
            continue;
        gt_write8(GT911_REG_STATUS, 0);

        for (int i = 0; i < n; i++) {
            const uint8_t *p = &raw[i * 8];
            uint32_t x = (uint32_t) (p[1] | (p[2] << 8));
            uint32_t y = (uint32_t) (p[3] | (p[4] << 8));
            if (TOUCH_SWAP_XY) {
                uint32_t t = x; x = y; y = t;
            }
            x = x * (BOARD_LCD_W - 1) / (s_xmax > 1 ? s_xmax - 1 : 1);
            y = y * (BOARD_LCD_H - 1) / (s_ymax > 1 ? s_ymax - 1 : 1);
            if (x > BOARD_LCD_W - 1) x = BOARD_LCD_W - 1;
            if (y > BOARD_LCD_H - 1) y = BOARD_LCD_H - 1;
            if (TOUCH_MIRROR_X) x = BOARD_LCD_W - 1 - x;
            if (TOUCH_MIRROR_Y) y = BOARD_LCD_H - 1 - y;
            pts[i].id = p[0];
            pts[i].x = (uint16_t) x;
            pts[i].y = (uint16_t) y;
        }

        if (s_cb && (n > 0 || last_n > 0))
            s_cb(pts, n);
        last_n = n;
    }
}

void touch_start(touch_cb_t cb)
{
    s_cb = cb;
    xTaskCreatePinnedToCore(touch_task, "touch", 4096, NULL, 4, NULL, 1);
}
