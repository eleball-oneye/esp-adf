# =============================================================================
# thermometer-t31 路 main 鈥斺€?涓荤粍浠舵敞鍐?#
# SRCS锛?#   main.c            鍙岃鑹插叆鍙ｏ紙缂栬瘧鏈熸寜 CONFIG_ONEYE_T31_ROLE_* 浜岄€変竴锛?#   t31_sim.c         瀛愯澶囪鑹诧細NimBLE 澶栬 + T31 甯фā鎷燂紙GATEWAY 鏋勫缓閲屾暣鏂囦欢涓虹┖ TU锛?#   gw_child.c        缃戝叧锛歝hild.pair / child.unpair 澶勭悊 + 瀛愯澶囩粦瀹氳惤 NVS +
#                     妗ｆ杩愯鏃舵敞鍐?+ sub.* 浜嬩欢涓婃姤锛圫UBDEV 鏋勫缓閲屾暣鏂囦欢涓虹┖ TU锛?#   ---- 浠ヤ笅 6 涓?.c 鏄粠 examples/oneye/devkitc_oneye/main/ **澶嶅埗**杩囨潵鐨勯厤缃戜欢锛堚洈 鍕垮垹锛?---
#   wifi_prov.c       Wi-Fi 鐢熷懡鍛ㄦ湡 + 鍏ョ綉鍚?*鍞竴鏀舵暃鐐?*锛坮eady_cb 瑙﹀彂杈癸級
#   wifi_prov_ble.c   BLE 閰嶇綉閫氶亾 C1/C2锛堥厤缃戝悗 deinit 鏁存爤 鈬?鎶?NimBLE 璁╃粰 link-sdk锛?#   adv_identity.c    韬唤鐨勫敮涓€璇诲彇鐐癸紙creds 鍒嗗尯鐨?node_id / sn 涓や覆锛涘崟鐙?TU 閬垮紑
#                     oneye_dev_base.h 涓?oneye_dev_link.h 鐨?`oneye_dev_link_status_t` 鍚屽悕涓嶅悓鏋勶級
#   prov_identity.c   浼氳瘽绾ц妭鐐硅韩浠斤紙prov.verify.req 浜ゆ潵鐨勬縺娲荤爜锛涙湰宸ョ▼鐢插瀷涓嶄緷璧栧畠锛屼絾
#                     wifi_prov_ble.c 鐨勯厤瀵?涓婃姤璺緞瑕侀摼鎺ュ畠锛?#   ble_sig_material.c 涔欐。 prov.pair.ok.sig 鐨勬潗鏂欐敞鍏ョ偣锛堢敳鍨嬩笉浜?sig锛屼繚鐣欎互渚跨紪璇戞湡涓€鑷达級
#   sntp_boot.c       鍚姩鏈熸巿鏃讹紙涓ユ牸 TLS 闇€"鍏堟湁閽熷啀寤洪摼"锛?#
# 鉀?涓嶅姞 EMBED_TXTFILES 璇佷功锛氱敳鍨嬬敤 creds 鍒嗗尯閲岀殑涓€鏈轰竴瀵嗘潗鏂欙紝**涓嶅唴宓?*浠讳綍璇佷功銆?#
# REQUIRES 鍙ｅ緞 = 涓?devkitc_oneye 鐩稿悓鐨勯浂闊抽闆嗗悎 + oneye-link-sdk锛圔LE 涓績渚э級銆?#   鉀?涓嶅惈 audio_board / audio_hal / esp_peripherals / esp-sr锛氭湰宸ョ▼闆堕煶棰戜緷璧栥€?# =============================================================================
idf_component_register(
    SRCS "main.c"
         "t31_sim.c"
         "gw_child.c"
         "wifi_prov.c"
         "wifi_prov_ble.c"
         "adv_identity.c"
         "prov_identity.c"
         "ble_sig_material.c"
         "sntp_boot.c"
         "board_key.c"
    INCLUDE_DIRS "."
    REQUIRES oneye-link-sdk
             oneye-dev-sdk
             esp_wifi
             esp_netif
             esp_event
             nvs_flash
             mbedtls
             json
             bt
             driver
             esp_timer
             esp_adc
             lwip)
