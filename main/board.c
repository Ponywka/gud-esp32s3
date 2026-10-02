#include "board.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/ledc.h"

static const char *TAG = "board";

/* CH422G uses several 7-bit I2C addresses instead of registers */
#define CH422G_ADDR_SET    0x24   /* system parameter register */
#define CH422G_ADDR_OC     0x23   /* open-drain outputs OC0..OC3 */
#define CH422G_ADDR_IO     0x38   /* push-pull outputs IO0..IO7 */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev_set, s_dev_io, s_dev_oc;
static SemaphoreHandle_t s_lock;
static uint8_t s_io_shadow;

i2c_master_bus_handle_t board_i2c(void) { return s_bus; }

static esp_err_t ch422g_write(i2c_master_dev_handle_t dev, uint8_t v)
{
    return i2c_master_transmit(dev, &v, 1, 100);
}

esp_err_t board_exio_set(uint8_t mask, uint8_t value)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_io_shadow = (uint8_t) ((s_io_shadow & ~mask) | (value & mask));
    s_io_shadow &= (uint8_t) ~EXIO_USB_SEL;
    esp_err_t err = ch422g_write(s_dev_io, s_io_shadow);
    xSemaphoreGive(s_lock);
    return err;
}

/* ------------------------------- backlight -------------------------------- */

#define BL_PWM_HZ        20000
#define BL_PWM_BITS      LEDC_TIMER_11_BIT
#define BL_PWM_MAX       ((1u << 11) - 1)
#define BL_VDD_MV        3300
/* MP3302 EN dimming range per datasheet: 0.655..0.845 V = no current, 1.275..1.425 V = full current */
#define BL_EN_MIN_MV     800
#define BL_EN_MAX_MV     1450

static bool s_bl_on;
static uint8_t s_bl_level = BOARD_BL_DEFAULT_PERCENT;
static bool s_panel_on;

static void backlight_apply(void)
{
#if BOARD_BL_PWM_GPIO >= 0
    /* Panel enable (DISP) follows the backlight: dark screen = panel off as well */
    bool lit = s_bl_on && s_bl_level;
    if (lit && !s_panel_on) {
        board_exio_set(EXIO_DISP, EXIO_DISP);
        s_panel_on = true;
    }

    uint32_t duty = 0;
    if (s_bl_on && s_bl_level >= 100) {
        duty = BL_PWM_MAX + 1;              /* constantly high: EN = 3.3 V, full current like the stock board */
    } else if (s_bl_on && s_bl_level) {
        uint32_t mv = BL_EN_MIN_MV + (BL_EN_MAX_MV - BL_EN_MIN_MV) * (s_bl_level - 1) / 98;
        duty = mv * BL_PWM_MAX / BL_VDD_MV;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

    if (!lit && s_panel_on) {
        board_exio_set(EXIO_DISP, 0);
        s_panel_on = false;
    }
#else
    board_exio_set(EXIO_DISP, s_bl_on && s_bl_level ? EXIO_DISP : 0);
#endif
}

void board_backlight(bool on)
{
    s_bl_on = on;
    backlight_apply();
}

void board_backlight_brightness(uint8_t percent)
{
    s_bl_level = percent > 100 ? 100 : percent;
    backlight_apply();
}

static esp_err_t backlight_init(void)
{
#if BOARD_BL_PWM_GPIO >= 0
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_PWM_BITS,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = BL_PWM_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "LEDC timer");
    const ledc_channel_config_t ch = {
        .gpio_num = BOARD_BL_PWM_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "LEDC channel");
#endif
    return ESP_OK;
}

esp_err_t board_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(backlight_init());      /* first: R4 pulls DISP (full brightness) while IO6 floats */

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz = 400000,
    };
    dev_cfg.device_address = CH422G_ADDR_SET;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev_set));
    dev_cfg.device_address = CH422G_ADDR_IO;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev_io));
    dev_cfg.device_address = CH422G_ADDR_OC;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev_oc));

    /* IO0..IO7 as outputs (bit0 = IO_OE) */
    esp_err_t err = ch422g_write(s_dev_set, 0x01);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CH422G not responding (%s)", esp_err_to_name(err));
        return err;
    }
    ch422g_write(s_dev_oc, 0x00);

    /* CTP_RST low, LCD_RST high, SD_CS high (deselected), DISP (panel/backlight) off, USB_SEL = 0 (native USB) */
    s_io_shadow = EXIO_LCD_RST | EXIO_SD_CS;
    err = ch422g_write(s_dev_io, s_io_shadow);
    ESP_LOGI(TAG, "CH422G ready, outputs=0x%02x", s_io_shadow);
    return err;
}
