#pragma once
#include "touch.h"

/* Start the TinyUSB device stack (task pinned to core 1). */
void usb_glue_start(void);

/* Forward the current set of pressed contacts to the host as a HID multitouch report. */
void usb_glue_touch(const touch_point_t *pts, int n);

/* Vendor request (bmRequestType 0x40, wValue/wIndex 0, no data) sent by tools/flash.sh */
#define USB_REQ_VENDOR_REBOOT  0xB0

/* Restart shortly (after the current control transfer has been acknowledged). */
void usb_glue_reboot(void);
