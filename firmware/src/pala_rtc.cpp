#include "pala_rtc.h"
#include "user_config.h"
#include <Wire.h>
#include <sys/time.h>

/* Register addresses and masks are the PCF85063's own. Time lives in seven
   consecutive registers from 0x04.

   This talks over Arduino's Wire, the same bus object the touch panel and the
   PMU use. It used to go through a second I2C driver on the same port, which
   on this board meant two drivers fighting over one bus - and after the move
   to the AMOLED board nothing created that second bus at all, so rtcBegin()
   returned false on the first line and the clock was simply never read. */
#define PCF_SEC_REG   0x04
#define PCF_CTRL1_REG 0x00

static bool rtcPresent = false;

static uint8_t bcd2dec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static bool readRegs(uint8_t reg, uint8_t* buf, size_t n) {
  Wire.beginTransmission(I2C_RTC_DEV_Address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)I2C_RTC_DEV_Address, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static bool writeRegs(uint8_t reg, const uint8_t* buf, size_t n) {
  Wire.beginTransmission(I2C_RTC_DEV_Address);
  Wire.write(reg);
  for (size_t i = 0; i < n; i++) Wire.write(buf[i]);
  return Wire.endTransmission() == 0;
}

bool rtcBegin() {
  /* Harmless if the touch panel or the PMU already brought the bus up; this
     must not depend on which of them ran first. */
  Wire.begin(ESP32_I2C_SDA_PIN, ESP32_I2C_SCL_PIN);
  uint8_t ctrl = 0;
  /* A read that comes back is the only proof the chip is there. */
  rtcPresent = readRegs(PCF_CTRL1_REG, &ctrl, 1);
  return rtcPresent;
}

static bool readRaw(uint8_t buf[7]) {
  if (!rtcPresent) return false;
  return readRegs(PCF_SEC_REG, buf, 7);
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

/* tm-in-UTC to epoch. timegm is not in this toolchain's newlib, and the
   alternative - setting TZ to UTC around a mktime - is a global side effect on
   a device that has a time zone of its own to keep. This is the standard
   days-from-civil calculation and is exact for any date this clock can hold. */
static time_t utcToEpoch(const struct tm& t) {
  int y = t.tm_year + 1900;
  const int m = t.tm_mon + 1;
  y -= (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u
                     + (unsigned)t.tm_mday - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  const long days = (long)era * 146097L + (long)doe - 719468L;
  return (time_t)days * 86400L + t.tm_hour * 3600L + t.tm_min * 60L + t.tm_sec;
}

/* The chip holds UTC, not local time.
 *
 * It used to hold local time, written with localtime_r and read back with
 * mktime, which round-trips correctly only while the time zone never changes.
 * The zone can now be set on the device, so that assumption no longer holds:
 * changing it would have shifted every stored timestamp by the difference. */
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

  const time_t e = utcToEpoch(t);
  if (e <= 0) return false;
  struct timeval tv = { .tv_sec = e, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  return true;
}

bool rtcSaveSystemTime() {
  if (!rtcPresent) return false;
  const time_t now = time(nullptr);
  if (now < 1700000000) return false;         /* system clock not set yet */
  struct tm t;
  gmtime_r(&now, &t);

  uint8_t b[7];
  b[0] = dec2bcd((uint8_t)t.tm_sec) & 0x7F;   /* clears the stop flag */
  b[1] = dec2bcd((uint8_t)t.tm_min);
  b[2] = dec2bcd((uint8_t)t.tm_hour);
  b[3] = dec2bcd((uint8_t)t.tm_mday);
  b[4] = (uint8_t)t.tm_wday;
  b[5] = dec2bcd((uint8_t)(t.tm_mon + 1));
  b[6] = dec2bcd((uint8_t)(t.tm_year % 100));
  return writeRegs(PCF_SEC_REG, b, 7);
}
