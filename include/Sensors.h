#pragma once

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_BME280.h>

// Latest readings from all environmental sensors.
struct SensorReadings
{
    float dsTemp = 0.0f;   // DS18B20 probe temperature, degrees C
    float bmeTemp = 0.0f;  // BME280 air temperature, degrees C
    float humidity = 0.0f; // relative humidity, percent
    float pressure = 0.0f; // barometric pressure, hPa
};

// Owns the DS18B20 probe and the BME280, and exposes their latest readings.
// I2C (Wire) must be initialised by the caller before begin().
class Sensors
{
public:
    Sensors();

    // Initialise both sensors. Returns false if the BME280 is not found.
    bool begin();

    // Sample every sensor once and cache the results.
    void read();

    const SensorReadings &readings() const { return readings_; }

private:
    OneWire oneWire_;
    DallasTemperature dsSensor_;
    Adafruit_BME280 bme_;
    SensorReadings readings_;
};
