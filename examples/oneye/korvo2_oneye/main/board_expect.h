/*
 * board_expect.h —— ESP32-S3-Korvo-2 板级硬件参数的**编译期断言**
 *
 * 事实源（两条，缺一不可）：
 *   ① 硬件文档：docs/zh_CN/design-guide/dev-boards/user-guide-esp32-s3-korvo-2.rst（V3.1）
 *      —— 管脚分配列表 / IO 扩展器分配 / 编解码器测试点 J15 / ADC 测试点 J16 /
 *         摄像头连接器 / LCD 连接器 / AEC 电路。
 *   ② 板级实现：components/audio_board/esp32_s3_korvo2_v3/{board_def.h,board_pins_config.c,board.c}
 *
 * 口径：本头文件只做"文档期望值 ↔ 板级实现宏"的**一致性断言**。
 *   断言失败 = 文档与实现已发生偏离，必须人工确认后改断言或改实现——**不得静默放过**。
 *   文档与实现都在、但无对应宏的项（如 TCA9554 P5 = PERI_PWR_ON）无法编译期校验，
 *   登记在 README「不可断言项」与 backend 映射页 §6，不在此处臆造。
 */

#ifndef _BOARD_EXPECT_H_
#define _BOARD_EXPECT_H_

/* board_def.h 的 `CODEC_ADC_I2S_PORT` / `CODEC_ADC_BITS_PER_SAMPLE` 宏体内含强制类型转换，
 * 展开它们需要 i2s 类型可见（board.c 自身不展开这两个宏，故上游头文件未引入）：
 *   i2s_port_t          → driver/i2s_types.h（esp_driver_i2s 组件）
 *   i2s_data_bit_width_t → hal/i2s_types.h
 */
#include "driver/i2s_types.h"
#include "hal/i2s_types.h"

#include "board.h"      /* board_def.h：板级宏 */
#include "es7210.h"     /* ES7210_INPUT_MIC1..4 */

/* ---------------------------------------------------------------- 音频编码/采集 */

/* rst「管脚分配列表」：IO48 = PA_CTRL（V3.1 保留与模组连接）；board_def.h:119 */
_Static_assert(PA_ENABLE_GPIO == GPIO_NUM_48,
               "Korvo-2: PA_CTRL 期望 GPIO48（rst 管脚分配列表 / board_def.h:119）");

/* rst「管脚分配列表」：无耳机检测；board_def.h:118 */
_Static_assert(HEADPHONE_DETECT == -1,
               "Korvo-2: 本板无耳机检测（board_def.h:118）");

/* rst「功能框图」：NS4150 功放；板级增益 6 dB（board_def.h:117） */
_Static_assert(BOARD_PA_GAIN == 6,
               "Korvo-2: 板级 PA 增益期望 6 dB（board_def.h:117）");

/* rst「管脚分配列表」：IO16 = I2S0_MCLK；ES8311 MCLK 源 = ESP（board_def.h:120） */
_Static_assert(ES8311_MCLK_SOURCE == 0,
               "Korvo-2: ES8311 MCLK 源期望 0（来自 ESP32 MCLK，board_def.h:120）");

/* rst「组件介绍」：ES7210 为麦克风阵列 ADC，AEC 参考信号由 ADC_MIC3P/N 采回
 * （rst「AEC 电路」）⇒ 板级选择 MIC1|MIC2|MIC3（MIC3 = 回声参考通路）；
 * board_def.h:121 */
_Static_assert(ES7210_MIC_SELECT == (ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2 | ES7210_INPUT_MIC3),
               "Korvo-2: ES7210 期望选择 MIC1|MIC2|MIC3（board_def.h:121）");

/* rst「管脚分配列表」/「ADC 测试点 J16」：I2S0 端口 0、ADC 32 bit、48 kHz；
 * 硬件 AEC 开启（board_def.h:113-116,126） */
_Static_assert(CODEC_ADC_I2S_PORT == 0,
               "Korvo-2: ADC 期望使用 I2S 端口 0（board_def.h:113 / board_pins_config.c:50-72）");
_Static_assert(CODEC_ADC_BITS_PER_SAMPLE == 32,
               "Korvo-2: ADC 期望 32 bit（16 bit 麦 + 16 bit 回采；board_def.h:114）");
_Static_assert(CODEC_ADC_SAMPLE_RATE == 48000,
               "Korvo-2: ADC 期望 48 kHz（board_def.h:115）");
_Static_assert(RECORD_HARDWARE_AEC,
               "Korvo-2: 期望启用硬件 AEC（board_def.h:116）");

/* rst「电气功能框图」：ES8311 回放 + ES7210 采集，两枚编解码器件均在板 */
_Static_assert(FUNC_AUDIO_CODEC_EN == 1,
               "Korvo-2: FUNC_AUDIO_CODEC_EN 期望 1（board_def.h:112）");

/* ---------------------------------------------------------------- 按键 */

/* rst「组件介绍」：六个功能按键 REC/MUTE/PLAY/SET/VOL-/VOL+（board_def.h:146-153） */
_Static_assert(FUNC_BUTTON_EN == 1, "Korvo-2: FUNC_BUTTON_EN 期望 1（board_def.h:146）");
_Static_assert(INPUT_KEY_NUM == 6, "Korvo-2: 期望 6 个按键（board_def.h:147）");
_Static_assert(BUTTON_VOLUP_ID == 0 && BUTTON_VOLDOWN_ID == 1 && BUTTON_SET_ID == 2 &&
               BUTTON_PLAY_ID == 3 && BUTTON_MODE_ID == 4 && BUTTON_REC_ID == 5,
               "Korvo-2: 按键 ID 期望 0..5（board_def.h:148-153）");

/* ---------------------------------------------------------------- microSD */

/* rst「管脚分配列表」：IO4 = microSD DATA0、IO7 = CMD、IO15 = CLK；
 * 仅一线模式（board.c:173-201）、无卡检测（CD/WP = -1） */
_Static_assert(FUNC_SDCARD_EN == 1, "Korvo-2: FUNC_SDCARD_EN 期望 1（board_def.h:59）");
_Static_assert(ESP_SD_PIN_CLK == GPIO_NUM_15, "Korvo-2: SD CLK 期望 IO15（board_def.h:64）");
_Static_assert(ESP_SD_PIN_CMD == GPIO_NUM_7, "Korvo-2: SD CMD 期望 IO7（board_def.h:65）");
_Static_assert(ESP_SD_PIN_D0 == GPIO_NUM_4, "Korvo-2: SD D0 期望 IO4（board_def.h:66）");
_Static_assert(ESP_SD_PIN_D1 == -1 && ESP_SD_PIN_D2 == -1 && ESP_SD_PIN_D3 == -1,
               "Korvo-2: SD 期望一线模式（D1-D3 = -1；board_def.h:67-69）");
_Static_assert(ESP_SD_PIN_CD == -1 && ESP_SD_PIN_WP == -1,
               "Korvo-2: SD 期望无卡检测/写保护引脚（board_def.h:74-75）");
_Static_assert(SDCARD_INTR_GPIO == -1,
               "Korvo-2: SD 期望无中断/检测引脚（board_def.h:61）");
_Static_assert(SDCARD_OPEN_FILE_NUM_MAX == 5,
               "Korvo-2: SD 最大打开文件数期望 5（board_def.h:60）");

/* ---------------------------------------------------------------- LCD + IO 扩展器 */

/* rst「LCD 连接器」：LCD_SPI_SDA = IO0、LCD_SPI_DC = IO2、LCD_SPI_CLK = IO1；
 * rst 图注：分辨率 240x320（面板竖屏 240x320，ADF 以 H=320/V=240 + 不交换表达）；
 * CS/RST/BL 经 TCA9554（P3/P2/P1） */
_Static_assert(FUNC_LCD_SCREEN_EN == 1, "Korvo-2: FUNC_LCD_SCREEN_EN 期望 1（board_def.h:39）");
_Static_assert(LCD_CLK_GPIO == GPIO_NUM_1, "Korvo-2: LCD CLK 期望 IO1（board_def.h:45）");
_Static_assert(LCD_MOSI_GPIO == GPIO_NUM_0, "Korvo-2: LCD SDA/MOSI 期望 IO0（board_def.h:46）");
_Static_assert(LCD_DC_GPIO == GPIO_NUM_2, "Korvo-2: LCD DC 期望 IO2（board_def.h:44）");
_Static_assert(LCD_H_RES == 320 && LCD_V_RES == 240,
               "Korvo-2: LCD 期望 320x240（board_def.h:48-49）");
_Static_assert(LCD_SWAP_XY == false && LCD_MIRROR_X == true && LCD_MIRROR_Y == true &&
               LCD_COLOR_INV == false,
               "Korvo-2: LCD 方向/镜像/反色期望值见 board_def.h:50-53");
_Static_assert(LCD_CTRL_GPIO == (1 << 1) && LCD_RST_GPIO == (1 << 2) && LCD_CS_GPIO == (1 << 3),
               "Korvo-2: LCD BL/RST/CS 期望 TCA9554 P1/P2/P3（board_def.h:40-42）");

/* rst「IO 扩展器 GPIO 分配」：P6 = LED1、P7 = LED2（蓝/红） */
_Static_assert(BLUE_LED_GPIO == (1 << 7), "Korvo-2: 蓝灯期望 TCA9554 P7（board_def.h:32）");
_Static_assert(RED_LED_GPIO == (1 << 6), "Korvo-2: 红灯期望 TCA9554 P6（board_def.h:33）");
_Static_assert(GREEN_LED_GPIO == -1, "Korvo-2: 无绿灯 GPIO（board_def.h:31）");

/* rst「组件介绍」：LCD 扩展板带触摸（TP_I2C + TP_INT）——仅参数宏，board.c 无触摸初始化 */
_Static_assert(FUNC_LCD_TOUCH_EN == 1, "Korvo-2: FUNC_LCD_TOUCH_EN 期望 1（board_def.h:104）");

/* ---------------------------------------------------------------- 摄像头（DVP 8 位并口） */

/* rst「摄像头连接器」：SIOD=IO17、SIOC=IO18、PCLK=IO11、VSYNC=IO21、HREF=IO38、XCLK=IO40；
 * 数据脚（rst 命名 D2..D9 ↔ ADF 命名 D0..D7，同一物理网络、命名偏移两位，见 README 核对表）
 * board_def.h:82-98 */
_Static_assert(FUNC_CAMERA_EN == 1, "Korvo-2: FUNC_CAMERA_EN 期望 1（board_def.h:81）");
_Static_assert(CAM_PIN_XCLK == GPIO_NUM_40, "Korvo-2: CAM XCLK 期望 IO40（board_def.h:84）");
_Static_assert(CAM_PIN_SIOD == GPIO_NUM_17, "Korvo-2: CAM SIOD 期望 IO17（board_def.h:85）");
_Static_assert(CAM_PIN_SIOC == GPIO_NUM_18, "Korvo-2: CAM SIOC 期望 IO18（board_def.h:86）");
_Static_assert(CAM_PIN_VSYNC == GPIO_NUM_21, "Korvo-2: CAM VSYNC 期望 IO21（board_def.h:96）");
_Static_assert(CAM_PIN_HREF == GPIO_NUM_38, "Korvo-2: CAM HREF 期望 IO38（board_def.h:97）");
_Static_assert(CAM_PIN_PCLK == GPIO_NUM_11, "Korvo-2: CAM PCLK 期望 IO11（board_def.h:98）");
_Static_assert(CAM_PIN_D0 == GPIO_NUM_13 && CAM_PIN_D1 == GPIO_NUM_47 &&
               CAM_PIN_D2 == GPIO_NUM_14 && CAM_PIN_D3 == GPIO_NUM_3 &&
               CAM_PIN_D4 == GPIO_NUM_12 && CAM_PIN_D5 == GPIO_NUM_42 &&
               CAM_PIN_D6 == GPIO_NUM_41 && CAM_PIN_D7 == GPIO_NUM_39,
               "Korvo-2: CAM D0-D7 期望 13/47/14/3/12/42/41/39（board_def.h:88-95）");
_Static_assert(CAM_PIN_PWDN == -1 && CAM_PIN_RESET == -1,
               "Korvo-2: CAM 期望无 PWDN/RESET（board_def.h:82-83）");

/* ---------------------------------------------------------------- 运行期断言（见 korvo2_oneye_main.c）

 * 以下期望值无编译期宏，改为运行期调用板级查询 API 比对：
 *   get_i2c_pins()            → SDA 17 / SCL 18   （rst I2C 测试点 J18；board_pins_config.c:35-48）
 *   get_i2s_pins(0)           → MCK 16 / BCK 9 / WS 45 / DOUT 8 / DIN 10
 *                                                （rst J15/J16；board_pins_config.c:50-72）
 *   get_pa_enable_gpio()      → 48                （board_pins_config.c:115-118）
 *   get_headphone_detect_gpio() → -1              （board_pins_config.c:110-113）
 *   get_sdcard_intr_gpio()    → -1                （board_pins_config.c:93-96）
 *   get_sdcard_power_ctrl_gpio() → -1             （board_pins_config.c:103-106）
 *   get_green_led_gpio()      → -1                （board_pins_config.c:154-157）
 *   get_blue_led_gpio()       → BIT(7)            （board_pins_config.c:159-162）
 *   get_es8311_mclk_src()     → 0                 （board_pins_config.c:164-167）
 *   AUDIO_ADC_INPUT_CH_FORMAT → "RMNM"            （rst「AEC 电路」；board_def.h:126）
 */

#endif /* _BOARD_EXPECT_H_ */
