# Vendored TinyUSB (espressif/tinyusb, 0.21.x) - device stack + HID + DWC2 port only.
idf_component_register(
    SRCS
        src/tusb.c
        src/common/tusb_fifo.c
        src/device/usbd.c
        src/class/hid/hid_device.c
        src/portable/synopsys/dwc2/dcd_dwc2.c
        src/portable/synopsys/dwc2/dwc2_common.c
    INCLUDE_DIRS
        src
        .
    REQUIRES freertos soc esp_hw_support
)
target_compile_definitions(${COMPONENT_LIB} PUBLIC CFG_TUSB_MCU=OPT_MCU_ESP32S3)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-parameter -Wno-missing-field-initializers)
