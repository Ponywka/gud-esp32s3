idf_component_register(
    SRCS
        main.c
        board.c
        lcd.c
        touch.c
        screen.c
        ui.c
        usb_glue.c
        gud/gud.c
        gud/gud_driver.c
    INCLUDE_DIRS . gud
    REQUIRES esp_lcd driver esp_timer esp_hw_support
    PRIV_REQUIRES tinyusb usb esp_psram nvs_flash
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format -Wno-unused-function)
