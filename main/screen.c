/*
 * Backlight / "<NO SIGNAL>" policy:
 *  - boot: "<NO SIGNAL>" with the backlight on; if no picture arrives within SCREEN_NO_SIGNAL_TIMEOUT_S,
 *    the backlight goes off (sleep) until one does;
 *  - a picture from the host (GUD SET_BUFFER) cancels that timer and lights the panel, also after sleep;
 *  - the host goes away (disconnect, reboot, gud unbound) after it had shown a picture: backlight off
 *    right away; before any picture it just keeps showing "<NO SIGNAL>";
 *  - host DPMS (GUD display enable) switches the backlight off/on.
 * Called from the USB task and the esp_timer task, hence the mutex.
 */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "board.h"
#include "screen.h"
#include "ui.h"

#define SCREEN_NO_SIGNAL_TIMEOUT_S  60

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_sleep_timer;
static bool s_lit;
static bool s_had_picture;
static bool s_dpms_off;

/* Only touch the CH422G (I2C) when the state actually changes */
static void set_lit(bool on)
{
    if (on != s_lit) {
        s_lit = on;
        board_backlight(on);
    }
}

static void sleep_timer_cb(void *arg)
{
    (void) arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_had_picture)
        set_lit(false);
    xSemaphoreGive(s_lock);
}

void screen_init(void)
{
    const esp_timer_create_args_t args = { .callback = sleep_timer_cb, .name = "screen_sleep" };

    s_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_sleep_timer));

    ui_show_no_signal();
    set_lit(true);
    ESP_ERROR_CHECK(esp_timer_start_once(s_sleep_timer, SCREEN_NO_SIGNAL_TIMEOUT_S * 1000000ULL));
}

void screen_picture(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_had_picture) {
        s_had_picture = true;
        esp_timer_stop(s_sleep_timer);
    }
    if (!s_dpms_off)
        set_lit(true);
    xSemaphoreGive(s_lock);
}

void screen_dpms(bool on)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_dpms_off = !on;
    set_lit(on);
    xSemaphoreGive(s_lock);
}

void screen_signal_lost(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Replace the stale frame either way, so it never reappears if the backlight comes back on */
    ui_show_no_signal();
    s_dpms_off = false;
    if (s_had_picture)
        set_lit(false);
    xSemaphoreGive(s_lock);
}
