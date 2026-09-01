#ifndef PALA_RTC_H
#define PALA_RTC_H
#include <Arduino.h>
#include <time.h>

/* PCF85063 real-time clock, at 0x51 on this board's I2C bus.

   The board has always had this chip and the firmware has never used it, so
   the device only knew the time while NTP had recently told it - which meant
   notes made before a sync were named rec_<milliseconds since boot>, a number
   that means nothing and sorts wrongly. The clock keeps running on its own
   backup across power loss, so once it is set the device knows the time from
   the moment it boots.

   Register map and BCD masks taken from Waveshare's own SensorLib for this
   board rather than from memory, after a day in which the board's header file
   was wrong about the flash size, the touch panel and the power button. */

bool rtcBegin();                     /* true if the chip answered */

/* Reads the clock into the system time. False when the chip has never been set
   or lost power - its oscillator-stop flag says so, and a wrong time is worse
   than an obviously missing one. */
bool rtcRestoreSystemTime();

/* Writes the current system time into the chip, so it survives a power cut. */
bool rtcSaveSystemTime();

/* True when the chip holds a time it believes in. */
bool rtcHasTime();

#endif
