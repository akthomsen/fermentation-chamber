#pragma once

#include <Arduino.h>

// =============================================================================
// Config.h - Hardware pin map and tuning constants for the fermentation chamber
// =============================================================================
// Everything that depends on how the board is wired or how the chamber should
// behave lives here, so the logic modules stay free of magic numbers.

// --- Sensor pins ---
constexpr int PIN_DS18B20 = 4;
constexpr int PIN_SDA = 6;
constexpr int PIN_SCL = 5;

// --- Rotary encoder pins ---
constexpr int PIN_ENC_CLK = 0;
constexpr int PIN_ENC_DT = 7;
constexpr int PIN_ENC_SW = 1;

// --- Output pins ---
constexpr int PIN_FAN = 21;        // fan via MOSFET
constexpr int PIN_HUMIDIFIER = 23; // humidifier
constexpr int PIN_HEATER = 12;     // heater relay

// On/off levels (flip these if a device is wired the other way round)
constexpr int FAN_ON = HIGH; // documents active-high fan MOSFET; the fan is now
constexpr int FAN_OFF = LOW; // PWM-driven (see Fan PWM below) but these record wiring
constexpr int HEATER_ON = HIGH;
constexpr int HEATER_OFF = LOW;
constexpr int HUMIDIFIER_ON = HIGH;
constexpr int HUMIDIFIER_OFF = LOW;

// --- Fan PWM ---
// The fan MOSFET is driven with LEDC PWM instead of plain on/off. 25 kHz sits
// above human hearing, so the motor/MOSFET does not whine audibly. Duty is
// active-high (255 = full speed, 0 = off) to match FAN_ON above; invert the
// ledcWrite value only if the MOSFET is ever rewired active-low.
constexpr int FAN_PWM_FREQ = 25000;        // Hz, inaudible
constexpr int FAN_PWM_RES_BITS = 8;        // 0..255 duty range
constexpr int FAN_DUTY_MIN_PCT = 50;       // always-on AUTO circulation floor (above stall)
constexpr int FAN_DUTY_HUMID_PCT = 70;     // bumped speed while the humidifier is running (heater still wins)
constexpr int FAN_DUTY_MAX_PCT = 100;      // full speed (heater running, or sensor failsafe)
constexpr int FAN_MANUAL_AUTO = -1;        // fanManualPct sentinel: AUTO; 0..100 = manual duty %
constexpr unsigned long FAN_KICK_MS = 300; // full-speed kick at run start to beat stiction

// --- I2C addresses ---
// Note: the Adafruit BME280 library already #defines BME280_ADDRESS, so the
// names here are deliberately distinct to avoid clashing with that macro.
constexpr uint8_t OLED_I2C_ADDRESS = 0x3C;
constexpr uint8_t BME280_I2C_ADDRESS = 0x76;

// --- Display geometry ---
constexpr int SCREEN_WIDTH = 128;
constexpr int SCREEN_HEIGHT = 64;
constexpr int OLED_RESET = -1;

// --- Control settings ---
constexpr float HYSTERESIS = 0.5f; // dead-band to stop rapid switching

// --- Heater safety / behaviour ---
constexpr float HEATER_OFFSET = 0.5f;                           // turn off this far BELOW target (residual heat keeps rising)
constexpr float DEFAULT_CEILING = 60.0f;                        // default hard over-temp cutoff (editable in menu)
constexpr float TEMP_VALID_MIN = 0.0f;                          // below this = sensor fault (DS18B20 returns -127 if unplugged)
constexpr float TEMP_VALID_MAX = 70.0f;                         // above this = sensor fault / runaway
constexpr unsigned long MAX_HEAT_MS = 60UL * 60UL * 1000UL;     // max continuous heater-on time (60 min)
constexpr unsigned long HEAT_COOLDOWN_MS = 5UL * 60UL * 1000UL; // forced off period after a max-on trip (5 min)

// --- Default setpoints ---
constexpr float DEFAULT_TARGET_TEMP = 32.0f;     // degrees C
constexpr float DEFAULT_TARGET_HUMIDITY = 80.0f; // percent
constexpr long DEFAULT_RUN_MINUTES = 0;          // 0 = no time limit

// --- Timing ---
constexpr unsigned long SENSOR_INTERVAL_MS = 1000;

// --- Button (polled) ---
constexpr unsigned long BUTTON_DEBOUNCE_MS = 50; // min hold to count as a real press
constexpr unsigned long STOP_HOLD_MS = 3000;     // hold this long to stop everything
