idf_component_register(
    SRCS
        "main.c"
        "usb_descriptors.c"
        "ctaphid.c"
        "ctap2.c"
        "ck_p256.c"
        "ck_credid.c"
        "ck_store.c"
        "lcd.c"
        "ckvp.c"
        "session.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_driver_gpio noisec mbedtls esp_hw_support nvs_flash
)
