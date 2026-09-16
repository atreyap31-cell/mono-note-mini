#ifndef PALA_POWER_H
#define PALA_POWER_H
#include <Arduino.h>

/* Battery and power, via the AXP2101.
 *
 * The old board had no power management chip: the battery was read through a
 * divider on an ADC pin, and the reading needed the chip's factory calibration
 * applying by hand to be worth anything. This board has a proper PMU that
 * measures the cell itself and reports a percentage and a charge state.
 *
 * That also means the percentage is the PMU's, not a guess from a voltage
 * curve, so it can be shown as a real number rather than rounded to quarters
 * to hide how rough it was.
 */

bool powerBegin();

/* 0-100, or -1 if the PMU has no reading yet. */
int  powerPercent();
bool powerCharging();
int  powerMillivolts();

#endif
