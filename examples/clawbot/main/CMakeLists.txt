idf_component_register(
    SRCS "clawbot_main.c"
         "qmi8658.c"
         "motion.c"
         "lcd.c"
         "esp_hidd_prf_api.c"
         "hid_dev.c"
         "hid_device_le_prf.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash esp_timer esp_driver_gpio esp_driver_i2c esp_driver_spi esp_lcd lvgl esp_lvgl_port esp_adc
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-const-variable -Wno-unused-variable)
