#include "Sensors.h"

#include <Wire.h>
#include "Config.h"

Sensors::Sensors()
    : oneWire_(PIN_DS18B20), dsSensor_(&oneWire_)
{
}

bool Sensors::begin()
{
    dsSensor_.begin();
    return bme_.begin(BME280_I2C_ADDRESS, &Wire);
}

void Sensors::read()
{
    dsSensor_.requestTemperatures();
    readings_.dsTemp = dsSensor_.getTempCByIndex(0);
    readings_.bmeTemp = bme_.readTemperature();
    readings_.humidity = bme_.readHumidity();
    readings_.pressure = bme_.readPressure() / 100.0f; // Pa -> hPa
}
