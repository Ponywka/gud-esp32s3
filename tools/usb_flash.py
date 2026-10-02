#!/usr/bin/env python3
"""
Flash the display over its native USB port (Type-C1), no BOOT button or cable swapping.

1. If the GUD firmware is running, ask it to reboot (vendor request USB_REQ_VENDOR_REBOOT).
2. On every boot the bootloader gives the port to USB-Serial-JTAG for ~1 s (bootloader_components/usb_dl_window).
   Catch that window and let esptool reset the chip into ROM download mode through it.
3. Flash build/flash_args (or a merged image given as the argument, at 0x0) and hard-reset into the new firmware.

If the firmware is broken and doesn't answer, just replug the cable: the window opens on every boot.
Runs inside the espressif/idf container (see tools/flash.sh); needs /dev and /sys of the host.
"""
import ctypes
import fcntl
import glob
import os
import sys
import time

import esptool
import serial

GUD_VID, GUD_PID = 0x1D50, 0x614D
JTAG_VID, JTAG_PID = 0x303A, 0x1001
USB_REQ_VENDOR_REBOOT = 0xB0   # main/usb_glue.h


class UsbdevfsCtrltransfer(ctypes.Structure):
    _fields_ = [
        ("bRequestType", ctypes.c_uint8),
        ("bRequest", ctypes.c_uint8),
        ("wValue", ctypes.c_uint16),
        ("wIndex", ctypes.c_uint16),
        ("wLength", ctypes.c_uint16),
        ("timeout", ctypes.c_uint32),
        ("data", ctypes.c_void_p),
    ]


# _IOWR('U', 0, struct usbdevfs_ctrltransfer)
USBDEVFS_CONTROL = (3 << 30) | (ctypes.sizeof(UsbdevfsCtrltransfer) << 16) | (ord("U") << 8) | 0


def usb_devices(vid, pid):
    for d in glob.glob("/sys/bus/usb/devices/*"):
        try:
            with open(d + "/idVendor") as v, open(d + "/idProduct") as p:
                if int(v.read(), 16) == vid and int(p.read(), 16) == pid:
                    yield d
        except OSError:
            pass


def request_reboot():
    for d in usb_devices(GUD_VID, GUD_PID):
        with open(d + "/busnum") as b, open(d + "/devnum") as n:
            node = "/dev/bus/usb/%03d/%03d" % (int(b.read()), int(n.read()))
        req = UsbdevfsCtrltransfer(0x40, USB_REQ_VENDOR_REBOOT, 0, 0, 0, 1000, None)
        fd = os.open(node, os.O_RDWR)
        try:
            fcntl.ioctl(fd, USBDEVFS_CONTROL, req)
        except OSError as e:
            print("reboot request: %s (device may already be rebooting)" % e)
        finally:
            os.close(fd)
        return True
    return False


def jtag_port():
    for d in usb_devices(JTAG_VID, JTAG_PID):
        for tty in glob.glob(d + "/*/tty/tty*"):
            node = "/dev/" + os.path.basename(tty)
            if os.path.exists(node):
                return node
    return None


def wait_jtag_port(timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        port = jtag_port()
        if port:
            return port
        time.sleep(0.02)
    return None


def main():
    port = jtag_port()
    if not port:
        if request_reboot():
            print("GUD display found, rebooting it into the USB download window...")
            port = wait_jtag_port(10)
        if not port:
            print("No download window. Replug the display (Type-C1) - waiting up to 5 min...")
            port = wait_jtag_port(300)
    if not port:
        sys.exit("USB-Serial-JTAG port didn't show up")

    print("USB-Serial-JTAG at %s, flashing" % port)
    image = ["0x0", sys.argv[1]] if len(sys.argv) > 1 else ["@flash_args"]
    try:
        esptool.main(["--chip", "esp32s3", "-p", port, "--before", "default_reset", "--after", "hard_reset",
                      "write_flash"] + image)
    except (serial.SerialException, OSError) as e:
        # After the reset the port disappears (the app takes the PHY over), esptool may trip on reopening it
        print("esptool: %s (expected: the port went away when the board rebooted)" % e)

    end = time.monotonic() + 15
    while time.monotonic() < end:
        if any(usb_devices(GUD_VID, GUD_PID)):
            print("Done: GUD display is back")
            return
        time.sleep(0.1)
    sys.exit("GUD display didn't come back after flashing")


if __name__ == "__main__":
    main()
