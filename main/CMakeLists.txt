idf_component_register(
    SRCS
        "main.c"
        "usb_descriptors.c"
        "ctaphid.c"
        "ctap2.c"
        "ck_p256.c"
        "ck_credid.c"
        "ck_store.c"
        "ck_touch.c"
        "ck_batt.c"
        "lcd.c"
        "ui_draw.c"
        "ui_screens.c"
        "fonts.c"
        "ui.c"
        "ckvp.c"
        "session.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_driver_gpio esp_driver_i2c esp_adc noisec mbedtls esp_hw_support nvs_flash
)
