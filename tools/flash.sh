#!/bin/sh
# Flash over the display's own USB cable (Type-C1). See tools/usb_flash.py.
#   tools/flash.sh               - flash build/ (after idf.py build)
#   tools/flash.sh image.bin     - flash a merged release image at 0x0
# The container gets the host's /dev (hotplugged ttyACM and /dev/bus/usb nodes) via device cgroup rules:
# 166 = ttyACM, 189 = USB device nodes.
set -e
cd "$(dirname "$0")/.."

if [ -n "$1" ]; then
    image=$(realpath "$1")
    [ -f "$image" ] || { echo "No such file: $1" >&2; exit 1; }
    exec docker run --rm -t \
        -v /dev:/dev \
        --device-cgroup-rule='c 166:* rmw' \
        --device-cgroup-rule='c 189:* rmw' \
        -v "$PWD":/project -w /project \
        -v "$(dirname "$image")":/image:ro \
        espressif/idf:v5.3.4 python /project/tools/usb_flash.py "/image/$(basename "$image")"
fi

exec docker run --rm -t \
    -v /dev:/dev \
    --device-cgroup-rule='c 166:* rmw' \
    --device-cgroup-rule='c 189:* rmw' \
    -v "$PWD":/project -w /project/build \
    espressif/idf:v5.3.4 python /project/tools/usb_flash.py
