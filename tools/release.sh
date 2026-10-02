#!/bin/sh
# Build release images for every "<NO SIGNAL>" orientation into dist/:
#   dist/gud-esp32s3-<version>-<variant>.bin  - merged image (bootloader + partition table + app), flash at 0x0
#   dist/SHA256SUMS
# Usage: tools/release.sh [version]   (default: git describe, e.g. v0.1.0)
# Each variant is built in its own directory under build-release/, build/ is left alone.
set -e
cd "$(dirname "$0")/.."

version=${1:-$(git describe --tags --always --dirty)}

rm -rf dist
mkdir -p dist

for variant in ccw cw ud landscape; do
    case $variant in
        ccw)       rotation=BOARD_UI_ROTATE_CCW ;;
        cw)        rotation=BOARD_UI_ROTATE_CW ;;
        ud)        rotation=BOARD_UI_ROTATE_UD ;;
        landscape) rotation=BOARD_UI_ROTATE_NONE ;;
    esac
    out=gud-esp32s3-$version-$variant.bin
    echo "=== $out ($rotation)"
    docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -e IDF_COMPONENT_MANAGER=0 \
        -v "$PWD":/project -w /project espressif/idf:v5.3.4 sh -ec "
            git config --global --add safe.directory '*'
            idf.py -B build-release/$variant -DSDKCONFIG=build-release/$variant/sdkconfig \
                -DBOARD_UI_ROTATION=$rotation build >/dev/null
            cd build-release/$variant
            python -m esptool --chip esp32s3 merge_bin -o /project/dist/$out @flash_args >/dev/null"
done

(cd dist && sha256sum *.bin > SHA256SUMS)
ls -l dist
