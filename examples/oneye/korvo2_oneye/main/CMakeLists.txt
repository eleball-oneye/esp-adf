idf_component_register(SRCS "korvo2_oneye_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES oneye-dev-sdk
                                audio_board
                                audio_hal
                                esp_peripherals
                                input_key_service
                                esp_wifi
                                esp_netif
                                esp_event
                                nvs_flash)
