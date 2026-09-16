#ifndef USER_CONFIG_H
#define USER_CONFIG_H
#include "driver/gpio.h"
#include "driver/i2c.h"

/* Waveshare ESP32-S3-Touch-AMOLED-2.16
 *
 * Every number here is from Waveshare's own pin_config.h and wiki, not guessed:
 *   https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16
 *   github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16
 *
 * The board before this one was a 1.54" e-paper panel with two buttons and a
 * single ES8311 doing both capture and playback. Almost nothing carries over:
 * the display is a 480x480 colour AMOLED on QSPI, input is a capacitive touch
 * panel, the microphones are a separate ES7210 array, and power is managed by
 * an AXP2101 rather than by switching rails from GPIOs.
 */

/* ---- display: CO5300 AMOLED, 480x480, QSPI ---- */
#define LCD_WIDTH        480
#define LCD_HEIGHT       480
#define LCD_SDIO0_PIN    4
#define LCD_SDIO1_PIN    5
#define LCD_SDIO2_PIN    6
#define LCD_SDIO3_PIN    7
#define LCD_SCLK_PIN     38
#define LCD_CS_PIN       12
#define LCD_RESET_PIN    39

/* ---- one I2C bus, four devices ----
   touch CST9220 @0x5A, PMU AXP2101 @0x34, RTC PCF85063 @0x51,
   IMU QMI8658 @0x6B. The RTC is the same part the old board had. */
#define ESP32_I2C_SDA_PIN  GPIO_NUM_15
#define ESP32_I2C_SCL_PIN  GPIO_NUM_14
#define ESP32_I2C_DEV_NUM  I2C_NUM_0
#define TOUCH_INT_PIN      11
#define TOUCH_RST_PIN      40
#define TOUCH_I2C_ADDR     0x5A
#define AXP2101_I2C_ADDR   0x34
#define I2C_RTC_DEV_Address 0x51   /* PCF85063, same part as the old board */

/* ---- audio ----
   Two chips, not one: ES8311 drives the speaker, ES7210 reads the dual
   microphone array. They share the I2S bus, so capture and playback cannot
   run at the same time - which suits a device that only ever does one. */
#define I2S_MCLK_PIN     42
#define I2S_BCLK_PIN     9
#define I2S_WS_PIN       45
#define I2S_DOUT_PIN     8        /* to ES8311, speaker */
#define I2S_DIN_PIN      10       /* from ES7210, microphones */
#define AUDIO_PA_PIN     46       /* speaker amplifier enable */

/* ---- microSD ----
   The wiki names these "MOSI/CMD", "SCK/CLK" and "MISO/D0", which are the same
   physical lines whether the socket is driven as SPI or as one-bit SDIO. One
   bit SDIO is what this firmware uses, as it did on the old board.

   GPIO41 is listed as CS. In one-bit mode that line is DAT3 and has to be held
   high, so it is driven rather than left floating - a floating DAT3 is read by
   some cards as a request to enter SPI mode, and the card then never answers. */
#define SDMMC_CLK_PIN    GPIO_NUM_2
#define SDMMC_CMD_PIN    GPIO_NUM_1
#define SDMMC_D0_PIN     GPIO_NUM_3
#define SDMMC_DAT3_PIN   GPIO_NUM_41

/* ---- buttons ----
   Two, and the firmware only needs one. PWR is wired to the AXP2101 rather
   than to a GPIO, so it cannot be read here - it powers the board on and off
   by itself. */
#define BOOT_BUTTON_PIN  GPIO_NUM_0
#define USER_BUTTON_PIN  GPIO_NUM_18

#endif
