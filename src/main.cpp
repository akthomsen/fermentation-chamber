#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_BME280.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// --- Sensor pins ---
#define DS18B20_PIN 4
#define SDA_PIN 6
#define SCL_PIN 5

// --- Encoder pins ---
#define ENC_CLK 0
#define ENC_DT 7
#define ENC_SW 1

// --- Output pins ---
#define FAN_PIN 21        // fan via MOSFET (HIGH = on, see example sketch)
#define HUMIDIFIER_PIN 23 // humidifier
#define HEATER_PIN 12     // heater relay

// On/off levels (flip these if a device is wired the other way round)
#define FAN_ON HIGH
#define FAN_OFF LOW
#define HEATER_ON HIGH
#define HEATER_OFF LOW
#define HUMIDIFIER_ON HIGH
#define HUMIDIFIER_OFF LOW

// --- Display ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
#define BME280_ADDRESS 0x76

// --- Control settings ---
#define HYSTERESIS 0.5 // dead-band to stop rapid switching

// Heater safety / behaviour
#define HEATER_OFFSET 0.5                      // turn off this far BELOW target (residual heat keeps rising)
#define DEFAULT_CEILING 45.0                   // default hard over-temp cutoff (editable in menu)
#define TEMP_VALID_MIN 0.0                     // below this = sensor fault (DS18B20 returns -127 if unplugged)
#define TEMP_VALID_MAX 70.0                    // above this = sensor fault / runaway
#define MAX_HEAT_MS (15UL * 60UL * 1000UL)     // max continuous heater-on time (15 min)
#define HEAT_COOLDOWN_MS (5UL * 60UL * 1000UL) // forced off period after a max-on trip (5 min)

// --- Menu ---
// Encoder scrolls between screens; button toggles edit on the setpoint screens.
// 0 = overview, 1 = sensors, 2 = actuators, 3 = set temp, 4 = set humid, 5 = max temp, 6 = run time
const char *menuLabels[] = {"Overview", "Sensors", "Actuators", "Set Temp", "Set Humid", "Max Temp", "Run Time"};
const int NUM_ITEMS = 7;
#define SCREEN_SET_TEMP 3
#define SCREEN_SET_HUMID 4
#define SCREEN_SET_CEIL 5
#define SCREEN_SET_RUN 6

volatile int menuIndex = 0;
volatile bool menuChanged = true; // flag: redraw display
volatile bool editing = false;    // editing a setpoint with the knob?

// --- Setpoints (changed with the encoder) ---
volatile float targetTemp = 36.0;               // degrees C
volatile float targetHumidity = 70.0;           // percent
volatile float targetCeiling = DEFAULT_CEILING; // hard over-temp cutoff, degrees C
volatile long runMinutes = 0;                   // shut everything off after this many minutes (0 = no limit)

// --- Encoder ISR state ---
volatile unsigned long lastButtonTime = 0;  // button debounce
volatile unsigned long lastEncoderTime = 0; // encoder debounce

// Quadrature decode table: maps a 4-bit (prev<<2 | now) state to a step.
// Invalid transitions (contact bounce) decode to 0, so noise is ignored.
const int8_t QDEC[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
volatile uint8_t encState = 0;
volatile int8_t encAccum = 0;

void IRAM_ATTR encoderISR()
{
    encState = ((encState << 2) | (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT)) & 0x0F;
    encAccum = encAccum + QDEC[encState];

    int dir = 0;
    if (encAccum >= 4) // one full detent clockwise
        dir = +1;
    else if (encAccum <= -4) // one full detent counter-clockwise
        dir = -1;
    if (dir == 0)
        return;
    encAccum = 0;

    if (!editing)
    {
        // Scroll between screens
        menuIndex = (menuIndex + dir + NUM_ITEMS) % NUM_ITEMS;
        menuChanged = true;
        return;
    }

    // Editing a setpoint
    unsigned long now = micros();
    int mult = (now - lastEncoderTime < 60000) ? 5 : 1; // fast spin = bigger steps
    lastEncoderTime = now;

    if (menuIndex == SCREEN_SET_TEMP)
        targetTemp = targetTemp + dir * 0.5 * mult; // adjust temperature
    else if (menuIndex == SCREEN_SET_HUMID)
        targetHumidity = targetHumidity + dir * 1.0 * mult; // adjust humidity
    else if (menuIndex == SCREEN_SET_CEIL)
        targetCeiling = targetCeiling + dir * 0.5 * mult; // adjust hard cutoff
    else if (menuIndex == SCREEN_SET_RUN)
    {
        runMinutes = runMinutes + dir * 5 * mult; // adjust run duration (5 min steps)
        if (runMinutes < 0)
            runMinutes = 0;
    }

    menuChanged = true;
}

void IRAM_ATTR buttonISR()
{
    unsigned long now = millis();
    if (now - lastButtonTime < 250)
        return; // ignore bounces
    lastButtonTime = now;

    // Only the setpoint screens are editable; button toggles edit mode there.
    if (menuIndex == SCREEN_SET_TEMP || menuIndex == SCREEN_SET_HUMID ||
        menuIndex == SCREEN_SET_CEIL || menuIndex == SCREEN_SET_RUN)
        editing = !editing;
    menuChanged = true;
}

// --- Objects ---
OneWire oneWire(DS18B20_PIN);
DallasTemperature tempSensor(&oneWire);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_BME280 bme;

// --- Sensor values (updated every second) ---
float ds_temp, bme_temp, humidity, pressure;

unsigned long lastSensorRead = 0;
const unsigned long SENSOR_INTERVAL = 1000;

void readSensors()
{
    tempSensor.requestTemperatures();
    ds_temp = tempSensor.getTempCByIndex(0);
    bme_temp = bme.readTemperature();
    humidity = bme.readHumidity();
    pressure = bme.readPressure() / 100.0F;
}

// --- Actuator states (for display) ---
bool fanOn = false;
bool heaterOn = false;
bool humidOn = false;
bool heaterFault = false;   // sensor invalid / over ceiling -> heater forced off
bool heaterLockout = false; // tripped max-on time, in forced cooldown
bool runComplete = false;   // run duration elapsed -> everything off

// Heater timing state
bool heaterWasOn = false;
unsigned long heaterOnSince = 0;
unsigned long lockoutSince = 0;

void updateOutputs()
{
    unsigned long now = millis();

    // Run-duration limit: once the configured time has elapsed, shut everything
    // off and stay off (power-cycle to start a new run).
    if (runMinutes > 0 && now >= (unsigned long)runMinutes * 60000UL)
        runComplete = true;

    if (runComplete)
    {
        fanOn = heaterOn = humidOn = false;
        digitalWrite(FAN_PIN, FAN_OFF);
        digitalWrite(HEATER_PIN, HEATER_OFF);
        digitalWrite(HUMIDIFIER_PIN, HUMIDIFIER_OFF);
        return;
    }

    // Fan is always on during the run
    fanOn = true;
    digitalWrite(FAN_PIN, FAN_ON);

    // Expire the cooldown lockout once enough time has passed
    if (heaterLockout && now - lockoutSince > HEAT_COOLDOWN_MS)
        heaterLockout = false;

    // Normal hysteresis, but shut off slightly BELOW target so residual
    // heat carries the temperature the rest of the way up.
    if (ds_temp <= targetTemp - HEATER_OFFSET - HYSTERESIS)
        heaterOn = true;
    else if (ds_temp >= targetTemp - HEATER_OFFSET)
        heaterOn = false;

    // Safety: bad sensor reading or above the hard ceiling -> force off.
    heaterFault = (ds_temp < TEMP_VALID_MIN) || (ds_temp > TEMP_VALID_MAX) ||
                  (ds_temp >= targetCeiling);
    if (heaterFault || heaterLockout)
        heaterOn = false;

    // Max continuous-on backstop: if the heater has run too long without the
    // temperature being satisfied, force a cooldown (catches stuck/sluggish states).
    if (heaterOn)
    {
        if (!heaterWasOn)
            heaterOnSince = now; // just turned on
        else if (now - heaterOnSince > MAX_HEAT_MS)
        {
            heaterOn = false;
            heaterLockout = true;
            lockoutSince = now;
        }
    }
    heaterWasOn = heaterOn;

    digitalWrite(HEATER_PIN, heaterOn ? HEATER_ON : HEATER_OFF);

    // Humidifier: add moisture if too dry, stop once above target
    if (humidity < targetHumidity - HYSTERESIS)
        humidOn = true;
    else if (humidity > targetHumidity + HYSTERESIS)
        humidOn = false;
    digitalWrite(HUMIDIFIER_PIN, humidOn ? HUMIDIFIER_ON : HUMIDIFIER_OFF);
}

void drawDisplay()
{
    display.clearDisplay();

    // --- Top bar: screen title ---
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.printf("[ %s ]", menuLabels[menuIndex]);

    // --- Divider ---
    display.drawFastHLine(0, 12, SCREEN_WIDTH, SSD1306_WHITE);

    if (menuIndex == 0)
    {
        // Overview: current vs target + run timer
        display.setTextSize(1);
        display.setCursor(0, 16);
        display.printf("Temp %.1f / %.1f C", ds_temp, targetTemp);
        display.setCursor(0, 30);
        display.printf("Hum  %.0f / %.0f %%", humidity, targetHumidity);
        display.setCursor(0, 48);
        if (runComplete)
            display.printf("Time FINISHED");
        else if (runMinutes > 0)
        {
            long left = runMinutes - (long)(millis() / 60000UL);
            if (left < 0)
                left = 0;
            display.printf("Left %ldh %02ldm", left / 60, left % 60);
        }
        else
        {
            long up = (long)(millis() / 60000UL);
            display.printf("Up   %ldh %02ldm", up / 60, up % 60);
        }
    }
    else if (menuIndex == 1)
    {
        // Sensors
        display.setTextSize(1);
        display.setCursor(0, 16);
        display.printf("DS Temp : %.1f C", ds_temp);
        display.setCursor(0, 28);
        display.printf("BME Temp: %.1f C", bme_temp);
        display.setCursor(0, 40);
        display.printf("Humidity: %.1f %%", humidity);
        display.setCursor(0, 52);
        display.printf("Pressure: %.0f hPa", pressure);
    }
    else if (menuIndex == 2)
    {
        // Actuators
        const char *heaterStat = heaterFault ? "FAULT" : (heaterLockout ? "COOLDN" : (heaterOn ? "ON" : "OFF"));
        display.setTextSize(1);
        display.setCursor(0, 16);
        display.printf("Fan       : %s", fanOn ? "ON" : "OFF");
        display.setCursor(0, 28);
        display.printf("Heater    : %s", heaterStat);
        display.setCursor(0, 40);
        display.printf("Humidifier: %s", humidOn ? "ON" : "OFF");
        display.setCursor(0, 52);
        display.printf(runComplete ? "RUN FINISHED" : "Ceiling   : %.1f C", targetCeiling);
    }
    else if (menuIndex == SCREEN_SET_TEMP)
    {
        // Set temperature
        display.setTextSize(2);
        display.setCursor(0, 24);
        display.printf("%.1f C", targetTemp);
        display.setTextSize(1);
        display.setCursor(0, 56);
        display.printf(editing ? "EDIT: turn knob" : "push to edit");
    }
    else if (menuIndex == SCREEN_SET_HUMID)
    {
        // Set humidity
        display.setTextSize(2);
        display.setCursor(0, 24);
        display.printf("%.0f %%", targetHumidity);
        display.setTextSize(1);
        display.setCursor(0, 56);
        display.printf(editing ? "EDIT: turn knob" : "push to edit");
    }
    else if (menuIndex == SCREEN_SET_CEIL)
    {
        // Set max temp (hard cutoff)
        display.setTextSize(2);
        display.setCursor(0, 24);
        display.printf("%.1f C", targetCeiling);
        display.setTextSize(1);
        display.setCursor(0, 56);
        display.printf(editing ? "EDIT: turn knob" : "push to edit");
    }
    else
    {
        // Set run duration (hours + minutes)
        display.setTextSize(2);
        display.setCursor(0, 22);
        if (runMinutes <= 0)
            display.printf("OFF");
        else
            display.printf("%ldh %02ldm", runMinutes / 60, runMinutes % 60);

        display.setTextSize(1);
        display.setCursor(0, 44);
        if (runComplete)
            display.printf("FINISHED");
        else if (runMinutes > 0)
        {
            long left = runMinutes - (long)(millis() / 60000UL);
            if (left < 0)
                left = 0;
            display.printf("left: %ldh %02ldm", left / 60, left % 60);
        }

        display.setCursor(0, 56);
        display.printf(editing ? "EDIT: turn knob" : "push to edit");
    }

    display.display();
}

void setup()
{
    Serial.begin(115200);
    Wire.begin(SDA_PIN, SCL_PIN);

    // Encoder
    pinMode(ENC_CLK, INPUT);
    pinMode(ENC_DT, INPUT);
    pinMode(ENC_SW, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(ENC_CLK), encoderISR, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENC_DT), encoderISR, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENC_SW), buttonISR, FALLING);

    // Outputs
    pinMode(FAN_PIN, OUTPUT);
    pinMode(HEATER_PIN, OUTPUT);
    pinMode(HUMIDIFIER_PIN, OUTPUT);
    digitalWrite(FAN_PIN, FAN_ON); // fan always on
    digitalWrite(HEATER_PIN, HEATER_OFF);
    digitalWrite(HUMIDIFIER_PIN, HUMIDIFIER_OFF);

    // Sensors
    tempSensor.begin();

    while (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS))
    {
        Serial.println("SSD1306 failed");
        delay(1000);
    }
    while (!bme.begin(BME280_ADDRESS, &Wire))
    {
        Serial.println("BME280 not found!");
        delay(1000);
    }

    display.setTextColor(SSD1306_WHITE);
    readSensors();
    updateOutputs();
    drawDisplay();
    Serial.println("Ready.");
}

void loop()
{
    // Update sensors and outputs every second
    if (millis() - lastSensorRead >= SENSOR_INTERVAL)
    {
        lastSensorRead = millis();
        readSensors();
        updateOutputs();
        menuChanged = true; // refresh display with new data
        Serial.printf("Temp: %.2f C (set %.1f) | Humid: %.1f%% (set %.0f)\n",
                      ds_temp, targetTemp, humidity, targetHumidity);
    }

    // Redraw only when something changed (saves flicker)
    if (menuChanged)
    {
        menuChanged = false;
        drawDisplay();
    }
}
