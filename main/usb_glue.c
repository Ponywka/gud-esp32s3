/*
 * USB composite device on the ESP32-S3 native USB port (Full-Speed):
 *   interface 0: HID multitouch touchscreen (EP IN 0x81)
 *   interface 1: GUD vendor interface        (EP OUT 0x02)
 */
#include <string.h>
#include "usb_glue.h"
#include "board.h"
#include "gud_driver.h"
#include "screen.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"

static const char *TAG = "usb";

#define USB_VID   0x1d50   /* GUD vendor/product id expected by the Linux gud driver */
#define USB_PID   0x614d

/* ------------------------------ descriptors ------------------------------ */

static const tusb_desc_device_t s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0,     /* per-interface classes */
    .bDeviceSubClass    = 0,
    .bDeviceProtocol    = 0,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

/* HID multitouch: 5 contacts, one report (ID 1) carrying all slots + contact count. */
#define TOUCH_FINGER_DESC(_xmax, _ymax, _xphys, _yphys) \
    HID_USAGE_PAGE(HID_USAGE_PAGE_DIGITIZER), \
    HID_USAGE(0x22),                    /* Finger */ \
    HID_COLLECTION(HID_COLLECTION_LOGICAL), \
        HID_USAGE(0x42),                /* Tip Switch */ \
        HID_LOGICAL_MIN(0), HID_LOGICAL_MAX(1), \
        HID_REPORT_SIZE(1), HID_REPORT_COUNT(1), \
        HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE), \
        HID_REPORT_SIZE(7), HID_REPORT_COUNT(1), \
        HID_INPUT(HID_CONSTANT | HID_VARIABLE | HID_ABSOLUTE), \
        HID_USAGE(0x51),                /* Contact Identifier */ \
        HID_LOGICAL_MIN(0), HID_LOGICAL_MAX_N(255, 2), \
        HID_REPORT_SIZE(8), HID_REPORT_COUNT(1), \
        HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE), \
        HID_USAGE_PAGE(HID_USAGE_PAGE_DESKTOP), \
        HID_USAGE(HID_USAGE_DESKTOP_X), \
        HID_LOGICAL_MIN(0), HID_LOGICAL_MAX_N(_xmax, 2), \
        HID_PHYSICAL_MIN(0), HID_PHYSICAL_MAX_N(_xphys, 2), \
        HID_UNIT(0x11), HID_UNIT_EXPONENT(0x0E),   /* cm * 10^-2 -> 0.1 mm units */ \
        HID_REPORT_SIZE(16), HID_REPORT_COUNT(1), \
        HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE), \
        HID_USAGE(HID_USAGE_DESKTOP_Y), \
        HID_LOGICAL_MAX_N(_ymax, 2), \
        HID_PHYSICAL_MAX_N(_yphys, 2), \
        HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE), \
        HID_PHYSICAL_MIN(0), HID_PHYSICAL_MAX(0), \
        HID_UNIT(0), HID_UNIT_EXPONENT(0), \
    HID_COLLECTION_END

static const uint8_t s_hid_report_desc[] = {
    HID_USAGE_PAGE(HID_USAGE_PAGE_DIGITIZER),
    HID_USAGE(0x04),                    /* Touch Screen */
    HID_COLLECTION(HID_COLLECTION_APPLICATION),
        HID_REPORT_ID(1)
        TOUCH_FINGER_DESC(BOARD_LCD_W - 1, BOARD_LCD_H - 1, 954, 544),
        TOUCH_FINGER_DESC(BOARD_LCD_W - 1, BOARD_LCD_H - 1, 954, 544),
        TOUCH_FINGER_DESC(BOARD_LCD_W - 1, BOARD_LCD_H - 1, 954, 544),
        TOUCH_FINGER_DESC(BOARD_LCD_W - 1, BOARD_LCD_H - 1, 954, 544),
        TOUCH_FINGER_DESC(BOARD_LCD_W - 1, BOARD_LCD_H - 1, 954, 544),
        HID_USAGE_PAGE(HID_USAGE_PAGE_DIGITIZER),
        HID_USAGE(0x54),                /* Contact Count */
        HID_LOGICAL_MIN(0), HID_LOGICAL_MAX(TOUCH_MAX_POINTS),
        HID_REPORT_SIZE(8), HID_REPORT_COUNT(1),
        HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
        HID_REPORT_ID(2)
        HID_USAGE(0x55),                /* Contact Count Maximum */
        HID_LOGICAL_MAX(TOUCH_MAX_POINTS),
        HID_FEATURE(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
    HID_COLLECTION_END,
};

typedef struct __attribute__((packed)) {
    uint8_t  tip;      /* bit0 = tip switch */
    uint8_t  id;
    uint16_t x;
    uint16_t y;
} hid_finger_t;

typedef struct __attribute__((packed)) {
    hid_finger_t finger[TOUCH_MAX_POINTS];
    uint8_t      count;
} hid_touch_report_t;

#define ITF_HID   0
#define ITF_GUD   GUD_ITF_NUM
#define EPNUM_HID_IN   0x81
#define EPNUM_GUD_OUT  0x02

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + 9 + 7)

static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CONFIG_TOTAL_LEN, 0x80 /* bus powered */, 500),
    /* HID touch: EP IN 64 bytes, 4 ms poll */
    TUD_HID_DESCRIPTOR(ITF_HID, 0, HID_ITF_PROTOCOL_NONE, sizeof(s_hid_report_desc), EPNUM_HID_IN, 64, 4),
    /* GUD: vendor class interface with one bulk OUT endpoint */
    9, TUSB_DESC_INTERFACE, ITF_GUD, 0, 1, TUSB_CLASS_VENDOR_SPECIFIC, 0x00, 0x00, 0,
    7, TUSB_DESC_ENDPOINT, EPNUM_GUD_OUT, TUSB_XFER_BULK, U16_TO_U8S_LE(64), 0,
};

uint8_t const *tud_descriptor_device_cb(void) { return (uint8_t const *) &s_device_desc; }
uint8_t const *tud_descriptor_configuration_cb(uint8_t index) { (void) index; return s_config_desc; }
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) { (void) instance; return s_hid_report_desc; }

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    static uint16_t desc[33];
    char serial[13];
    const char *str;
    (void) langid;

    switch (index) {
    case 0:
        desc[1] = 0x0409;
        desc[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | 4);
        return desc;
    case 1: str = "ESP32-S3"; break;
    case 2: str = "GUD Touch Display"; break;
    case 3: {
        uint8_t mac[6];
        esp_efuse_mac_get_default(mac);
        snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        str = serial;
        break;
    }
    default:
        return NULL;
    }

    size_t len = strlen(str);
    if (len > 31) len = 31;
    for (size_t i = 0; i < len; i++)
        desc[1 + i] = (uint16_t) str[i];
    desc[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc;
}

/* Host deconfigured the device (disconnect, host reboot, bus reset) */
void tud_umount_cb(void)
{
    screen_signal_lost();
}

/* ------------------------------ HID callbacks ------------------------------ */

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void) instance;
    if (report_type == HID_REPORT_TYPE_FEATURE && report_id == 2 && reqlen >= 1) {
        buffer[0] = TOUCH_MAX_POINTS;
        return 1;
    }
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void) instance; (void) report_id; (void) report_type; (void) buffer; (void) bufsize;
}

/* ------------------------------ touch -> HID ------------------------------ */

static touch_point_t s_prev[TOUCH_MAX_POINTS];
static int s_prev_n;

static void send_report(const hid_finger_t *f, int n)
{
    hid_touch_report_t rep;
    memset(&rep, 0, sizeof(rep));
    for (int i = 0; i < n && i < TOUCH_MAX_POINTS; i++)
        rep.finger[i] = f[i];
    rep.count = (uint8_t) n;

    for (int tries = 0; tries < 20; tries++) {
        if (!tud_mounted())
            return;
        if (tud_hid_n_ready(0)) {
            tud_hid_n_report(0, 1, &rep, sizeof(rep));
            return;
        }
        vTaskDelay(1);
    }
}

void usb_glue_touch(const touch_point_t *pts, int n)
{
    hid_finger_t cur[TOUCH_MAX_POINTS];
    hid_finger_t rel[TOUCH_MAX_POINTS];
    int nrel = 0;

    for (int i = 0; i < n; i++) {
        cur[i] = (hid_finger_t){ .tip = 1, .id = pts[i].id, .x = pts[i].x, .y = pts[i].y };
    }
    /* contacts present before but gone now: report them once with tip = 0 */
    for (int i = 0; i < s_prev_n; i++) {
        bool still = false;
        for (int j = 0; j < n; j++)
            if (pts[j].id == s_prev[i].id) { still = true; break; }
        if (!still)
            rel[nrel++] = (hid_finger_t){ .tip = 0, .id = s_prev[i].id, .x = s_prev[i].x, .y = s_prev[i].y };
    }

    if (n + nrel <= TOUCH_MAX_POINTS) {
        hid_finger_t all[TOUCH_MAX_POINTS];
        memcpy(all, cur, sizeof(hid_finger_t) * (size_t) n);
        memcpy(all + n, rel, sizeof(hid_finger_t) * (size_t) nrel);
        send_report(all, n + nrel);
    } else {
        if (nrel)
            send_report(rel, nrel);
        send_report(cur, n);
    }

    memcpy(s_prev, pts, sizeof(touch_point_t) * (size_t) n);
    s_prev_n = n;
}

/* ------------------------------ stack bring-up ------------------------------ */

static void usb_task(void *arg)
{
    (void) arg;

    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_FULL,
    };
    usb_phy_handle_t phy;
    ESP_ERROR_CHECK(usb_new_phy(&phy_conf, &phy));

    tusb_rhport_init_t init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL,
    };
    if (!tusb_rhport_init(0, &init)) {
        ESP_LOGE(TAG, "tusb_rhport_init failed");
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "TinyUSB device started");

    /* Watched by the task WDT (panics -> reboot -> USB download window), so a hung USB stack
     * can't lock a built-in display out of tools/flash.sh */
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        tud_task_ext(100, false);
        esp_task_wdt_reset();
    }
}

void usb_glue_start(void)
{
    xTaskCreatePinnedToCore(usb_task, "usb", 6144, NULL, 5, NULL, 1);
}

/* ------------------------------ reboot request ----------------------------- */

static void reboot_cb(void *arg)
{
    (void) arg;
    esp_restart();      /* bootloader hands the port to USB-Serial-JTAG (download window) */
}

void usb_glue_reboot(void)
{
    static esp_timer_handle_t s_timer;
    const esp_timer_create_args_t args = { .callback = reboot_cb, .name = "reboot" };

    ESP_LOGW(TAG, "reboot requested over USB");
    tud_disconnect();   /* drop D+ now so the host sees the device go away before the bootloader window */
    if (!s_timer && esp_timer_create(&args, &s_timer) != ESP_OK)
        esp_restart();
    esp_timer_start_once(s_timer, 300 * 1000);
}
