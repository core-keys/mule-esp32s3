idf_component_register(
    SRCS
        "main.c"
        "usb_descriptors.c"
        "ctaphid.c"
        "lcd.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_driver_gpio noisec
)
