#!/bin/sh
# Flash build/ over the display's own USB cable (Type-C1). See tools/usb_flash.py.
# The container gets the host's /dev (hotplugged ttyACM and /dev/bus/usb nodes) via device cgroup rules:
# 166 = ttyACM, 189 = USB device nodes.
set -e
cd "$(dirname "$0")/.."
exec docker run --rm -t \
    -v /dev:/dev \
    --device-cgroup-rule='c 166:* rmw' \
    --device-cgroup-rule='c 189:* rmw' \
    -v "$PWD":/project -w /project/build \
    espressif/idf:v5.3.4 python /project/tools/usb_flash.py "$@"
