idf_component_register(SRCS "main.c"
                            "voice_io.c"
                            "llm_client.c"
                            "key_talk.c"
                            "prov_service.c"
                            "lan_link.c"
                            "panel_min.c"
                       INCLUDE_DIRS "."
                       REQUIRES oneye-dev-sdk
                                esp_http_client
                                audio_board
                                audio_hal
                                audio_pipeline
                                audio_stream
                                esp-adf-libs
                                esp-sr
                                esp_peripherals
                                input_key_service
                                esp_http_server
                                esp_websocket_client
                                esp_wifi
                                esp_netif
                                esp_event
                                nvs_flash
                                json
                                spiffs
                                fatfs
                                vfs)
