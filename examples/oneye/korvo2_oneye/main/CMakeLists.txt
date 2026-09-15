idf_component_register(SRCS "korvo2_oneye_main.c"
                            "panel_api.c"
                            "media_api.c"
                            "aec_capture.c"
                       INCLUDE_DIRS "."
                       REQUIRES oneye-dev-sdk
                                audio_board
                                audio_hal
                                audio_pipeline
                                audio_stream
                                esp-adf-libs
                                esp-sr
                                esp_peripherals
                                input_key_service
                                esp_http_server
                                esp_wifi
                                esp_netif
                                esp_event
                                nvs_flash
                                json
                                spiffs
                                fatfs
                                vfs)
