#include "pala_rtc.h"
#include "i2c_bsp.h"
#include "user_config.h"
#include <sys/time.h>

/* Register addresses and masks are Waveshare's, from the SensorLib driver they
   ship for this board. Time lives in seven consecutive registers from 0x04. */
#define PCF_SEC_REG   0x04
#define PCF_CTRL1_REG 0x00

static i2c_master_dev_handle_t rtcDev = nullptr;
static bool rtcPresent = false;

static uint8_t bcd2dec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

bool rtcBegin() {
  if (!I2cMasterBus::instance_) return false;
  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address  = I2C_RTC_DEV_Address;
  cfg.scl_speed_hz    = 100000;
  if (i2c_master_bus_add_device(I2cMasterBus::instance_->Get_I2cBusHandle(), &cfg, &rtcDev) != ESP_OK)
    return false;
  /* A read that comes back is the only proof the chip is there. */
  uint8_t ctrl = 0;
  rtcPresent = (I2cMasterBus::instance_->i2c_read_buff(rtcDev, PCF_CTRL1_REG, &ctrl, 1) == ESP_OK);
  return rtcPresent;
}

static bool readRaw(uint8_t buf[7]) {
  if (!rtcPresent || !rtcDev) return false;
  return I2cMasterBus::instance_->i2c_read_buff(rtcDev, PCF_SEC_REG, buf, 7) == ESP_OK;
}

bool rtcHasTime() {
  uint8_t b[7];
  if (!readRaw(b)) return false;
  /* Bit 7 of the seconds register is the oscillator-stop flag: set means the
     clock lost power and whatever it holds is meaningless. */
  if (b[0] & 0x80) return false;
  uint16_t year = (uint16_t)(bcd2dec(b[6]) + 2000);
  return year >= 2024 && year < 2100;
}

bool rtcRestoreSystemTime() {
  uint8_t b[7];
  if (!readRaw(b)) return false;
  if (b[0] & 0x80) return false;              /* never set, or lost power */

  struct tm t = {};
  t.tm_sec  = bcd2dec(b[0] & 0x7F);
  t.tm_min  = bcd2dec(b[1] & 0x7F);
  t.tm_hour = bcd2dec(b[2] & 0x3F);           /* 24-hour mode */
  t.tm_mday = bcd2dec(b[3] & 0x3F);
  t.tm_mon  = bcd2dec(b[5] & 0x1F) - 1;
  t.tm_year = bcd2dec(b[6]) + 2000 - 1900;
  t.tm_isdst = 0;

  if (t.tm_year < 124 || t.tm_mon > 11 || t.tm_mday < 1 || t.tm_mday > 31) return false;

  time_t e = mktime(&t);
  if (e <= 0) return false;
  struct timeval tv = { .tv_sec = e, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  return true;
}

bool rtcSaveSystemTime() {
  if (!rtcPresent || !rtcDev) return false;
  time_t now = time(nullptr);
  if (now < 1700000000) return false;         /* system clock not set yet */
  struct tm t;
  localtime_r(&now, &t);

  uint8_t b[7];
  b[0] = dec2bcd((uint8_t)t.tm_sec) & 0x7F;   /* clears the stop flag */
  b[1] = dec2bcd((uint8_t)t.tm_min);
  b[2] = dec2bcd((uint8_t)t.tm_hour);
  b[3] = dec2bcd((uint8_t)t.tm_mday);
  b[4] = (uint8_t)t.tm_wday;
  b[5] = dec2bcd((uint8_t)(t.tm_mon + 1));
  b[6] = dec2bcd((uint8_t)(t.tm_year % 100));
  return I2cMasterBus::instance_->i2c_write_buff(rtcDev, PCF_SEC_REG, b, 7) == ESP_OK;
}
