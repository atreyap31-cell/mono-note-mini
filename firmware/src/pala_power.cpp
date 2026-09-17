#include "pala_power.h"
#include "user_config.h"
#include <Wire.h>
#include "XPowersLib.h"

static XPowersAXP2101 pmu;
static bool ready = false;

bool powerBegin() {
  /* Wire is already up if the touch panel started first; begin() twice on the
     same bus is harmless and this must not depend on initialisation order. */
  Wire.begin(ESP32_I2C_SDA_PIN, ESP32_I2C_SCL_PIN);
  ready = pmu.begin(Wire, AXP2101_I2C_ADDR, ESP32_I2C_SDA_PIN, ESP32_I2C_SCL_PIN);
  if (!ready) return false;

  /* None of these measurements are on by default, and without them the PMU
     answers every query with zero - which reads as a flat battery rather than
     as an unconfigured chip. */
  pmu.enableBattDetection();
  pmu.enableBattVoltageMeasure();
  pmu.enableVbusVoltageMeasure();
  pmu.enableSystemVoltageMeasure();
  return true;
}

int powerPercent() {
  if (!ready) return -1;
  int p = pmu.getBatteryPercent();
  /* The PMU reports -1 until it has taken a reading, and briefly after a
     charger is connected or removed. Passing that through unchanged is right:
     the screen can say nothing rather than claim zero. */
  if (p < 0 || p > 100) return -1;
  return p;
}

bool powerCharging()  { return ready && pmu.isCharging(); }
int  powerMillivolts(){ return ready ? pmu.getBattVoltage() : 0; }

bool  powerBatteryPresent()    { return ready && pmu.isBatteryConnect(); }
bool  powerUsbPresent()        { return ready && pmu.isVbusIn(); }
int   powerUsbMillivolts()     { return ready ? pmu.getVbusVoltage() : 0; }
int   powerSystemMillivolts()  { return ready ? pmu.getSystemVoltage() : 0; }
float powerTemperatureC()      { return ready ? pmu.getTemperature() : 0.0f; }

void powerOffNow() {
  if (!ready) return;
  pmu.shutdown();
}
