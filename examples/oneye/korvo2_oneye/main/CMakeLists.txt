# =============================================================================
# 一机一密 mTLS：可选的证书材料嵌入（**私钥不入库**）
#
# 用 `-DONEYE_FW_CERT_DIR=<目录>` 指定一个含 ca.crt / client.crt / client.key 的目录
# （PEM），本组件把它们以二进制形式编进固件，main.c 据此填 base_cfg 的
# credential / credential_key / tls_ca_pem ⇒ 设备以**自己的**证书接入 8883。
# 目录缺省取 main/certs（已在 .gitignore 忽略：私钥绝不入库）。
#
# 为什么用构建期嵌入而不是运行期读文件：SD 卡/文件系统形态在产线不可控，
# 且"证书投放"必须与固件批次绑定（一机一密）；同时避免把私钥写进 sdkconfig（明文、易泄）。
#
# 未提供证书目录时行为不变：不嵌入、不定义 ONEYE_FW_EMBED_CERTS，main.c 走匿名/明文形态。
# =============================================================================
set(ONEYE_FW_CERT_DIR "${CMAKE_CURRENT_LIST_DIR}/certs" CACHE PATH
    "含 ca.crt/client.crt/client.key 的目录（非空且三文件齐备才嵌入证书）")

set(_oneye_cert_files "")
if(EXISTS "${ONEYE_FW_CERT_DIR}/ca.crt"
   AND EXISTS "${ONEYE_FW_CERT_DIR}/client.crt"
   AND EXISTS "${ONEYE_FW_CERT_DIR}/client.key")
    set(_oneye_cert_files
        "${ONEYE_FW_CERT_DIR}/ca.crt"
        "${ONEYE_FW_CERT_DIR}/client.crt"
        "${ONEYE_FW_CERT_DIR}/client.key")
    message(STATUS "korvo2_oneye: 嵌入设备证书材料（ca.crt/client.crt/client.key）⇒ 启用一机一密 mTLS")
else()
    message(STATUS "korvo2_oneye: 未提供证书材料（${ONEYE_FW_CERT_DIR}）⇒ 不嵌入证书（匿名/明文形态）")
endif()

idf_component_register(SRCS "korvo2_oneye_main.c"
                            "panel_api.c"
                            "media_api.c"
                            "aec_capture.c"
                            "player.c"
                            "lcd_ui.c"
                            "camera_api.c"
                            "wifi_prov.c"
                            "net_probe.c"
                                  "device_creds.c"
                            "prov_attest.c"
                            "sntp_boot.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES ${_oneye_cert_files}
                       REQUIRES oneye-dev-sdk
                                mbedtls
                                audio_board
                                audio_hal
                                audio_pipeline
                                audio_stream
                                esp-adf-libs
                                esp-sr
                                esp_peripherals
                                input_key_service
                                esp_http_server
                                esp32-camera
                                esp_mm
                                esp_wifi
                                esp_netif
                                lwip
                                esp_event
                                nvs_flash
                                json
                                spiffs
                                fatfs
                                vfs)

if(_oneye_cert_files)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ONEYE_FW_EMBED_CERTS=1)
endif()
