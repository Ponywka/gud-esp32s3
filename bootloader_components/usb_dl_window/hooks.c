/*
 * USB download window: on every boot the native USB port (Type-C1) is handed back to the
 * USB-Serial-JTAG controller for a moment. While it's there, esptool can reset the chip into
 * ROM download mode over the same cable (DTR/RTS of the USB-Serial-JTAG CDC port), without the
 * BOOT button and even if the application is broken. tools/flash.sh relies on this.
 *
 * esp_restart()/panics only reset the CPUs: the OTG controller keeps running and holding the bus, and its
 * PHY routing lives in RTC_CNTL_USB_CONF (RTC domain). So here OTG is reset and clock-gated, the PHY is
 * routed back to USB-Serial-JTAG, and D+ is released for a moment so the host sees a new device.
 */
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/usb_serial_jtag_reg.h"
#include "soc/system_reg.h"

#define USB_DL_DISCONNECT_MS  200

/* Backlight PWM pin (BOARD_BL_PWM_GPIO in main/board.h); R4 would light the panel at full brightness
 * while it floats, so keep it low until the application takes over */
#define BL_PWM_GPIO           6
#define USB_DL_WINDOW_MS      1500

/* Referenced by the build so the linker keeps the (weak) hooks below */
void bootloader_hooks_include(void)
{
}

void bootloader_after_init(void)
{
    esp_rom_gpio_pad_select_gpio(BL_PWM_GPIO);
    esp_rom_gpio_connect_out_signal(BL_PWM_GPIO, SIG_GPIO_OUT_IDX, false, false);
    REG_WRITE(GPIO_OUT_W1TC_REG, BIT(BL_PWM_GPIO));
    REG_WRITE(GPIO_ENABLE_W1TS_REG, BIT(BL_PWM_GPIO));

    /* Stop the OTG controller left running by the application (not reset by esp_restart) */
    REG_SET_BIT(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_USB_RST);
    REG_CLR_BIT(SYSTEM_PERIP_CLK_EN0_REG, SYSTEM_USB_CLK_EN);
    REG_CLR_BIT(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_USB_RST);

    /* Internal PHY -> USB-Serial-JTAG (sw_usb_phy_sel = 0), regardless of the USB_PHY_SEL eFuse */
    REG_SET_BIT(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL);
    REG_CLR_BIT(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_USB_PHY_SEL);

    /* USB-Serial-JTAG isn't reset by esp_restart either: after the app took the PHY away it is left
     * in its old (enumerated) state. Reset it to power-on defaults. */
    REG_SET_BIT(SYSTEM_PERIP_CLK_EN1_REG, SYSTEM_USB_DEVICE_CLK_EN);
    REG_SET_BIT(SYSTEM_PERIP_RST_EN1_REG, SYSTEM_USB_DEVICE_RST);
    REG_CLR_BIT(SYSTEM_PERIP_RST_EN1_REG, SYSTEM_USB_DEVICE_RST);

    /* Release D+ so the host sees a disconnect, then restore the default pull-up control */
    REG_SET_BIT(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PAD_PULL_OVERRIDE);
    REG_CLR_BIT(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP);
    esp_rom_delay_us(USB_DL_DISCONNECT_MS * 1000);
    REG_SET_BIT(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP);
    REG_CLR_BIT(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PAD_PULL_OVERRIDE);

    ESP_LOGI("usb_dl", "USB-Serial-JTAG download window, %d ms", USB_DL_WINDOW_MS);
    esp_rom_delay_us(USB_DL_WINDOW_MS * 1000);
}
