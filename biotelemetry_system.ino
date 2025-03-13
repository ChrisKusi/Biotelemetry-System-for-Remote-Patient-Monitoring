/**
   Project: Biotelemetry System for Remote Patient Monitoring
   Author: Christian Kusi
   Description:
   This Arduino sketch implements an advanced health monitoring system using an ESP32 TTGO T-Call SIM800L board,
   combining WiFi and GSM for reliable connectivity. It measures heart rate (BPM) and oxygen saturation (SPO2) using
   a MAX30102 sensor, displays results on a 20x4 I2C LCD, provides visual and audible alerts via an RGB LED and buzzer,
   and integrates with Blynk for remote monitoring. SMS alerts are sent via SIM800L when critical conditions are detected.

   Components:
   - ESP32 TTGO T-Call SIM800L Board: Main microcontroller with WiFi and SIM800L GSM module.
   - MAX30102 Sensor: Pulse oximeter and heart rate sensor.
   - 20x4 I2C LCD Display: Shows welcome messages, real-time readings, and results.
   - RGB LED Module: Visual alerts (Red: critical, Blue: low BPM, Green: normal).
   - Buzzer: Audio alerts based on health status.
   - Button: Triggers measurement.

   Pin Connections:
   - Button: Pin 33 (internal pull-up)
   - Buzzer: Pin 32
   - Red LED: Pin 19
   - Green LED: Pin 18
   - Blue LED: Pin 13
   - LCD & MAX30102: I2C bus (SDA: Pin 21, SCL: Pin 22)
   - SIM800L Modem: RST (5), PWKEY (4), POWER_ON (23), TX (27), RX (26)

   Features:
   - Startup: Displays a welcome screen on the LCD.
   - Measurement: Triggered by button press, runs for 15 seconds, updates LCD and Blynk in real-time.
   - Results: Shows final BPM and SPO2 readings.
   - Alerts: Visual (LEDs) and audible (buzzer) alerts based on thresholds (BPM > 100, BPM < 60, SPO2 < 95).
   - Remote Monitoring: Uses WiFi (preferred) or GSM (fallback) for Blynk connectivity.
   - SMS Alerts: Sends health reports via SIM800L for critical conditions.
   - Idle State: Prompts user to press the button when idle.

   Notes:
   - Update WiFi credentials (ssid, pass) and GSM APN (apn) as needed.
   - Invalid readings display as 0 (e.g., if finger not detected).
   - Requires a valid SIM card with data service for GSM.
*/

// Define GSM modem model before any TinyGSM includes
#define TINY_GSM_MODEM_SIM800

// TTGO T-Call pin definitions
#define MODEM_RST            5
#define MODEM_PWKEY          4
#define MODEM_POWER_ON       23
#define MODEM_TX             27
#define MODEM_RX             26
#define I2C_SDA              21
#define I2C_SCL              22

#include <Wire.h>
#include "MAX30105.h"          // MAX30102 sensor library
#include "spo2_algorithm.h"    // SPO2 calculation algorithm
#include <LiquidCrystal_I2C.h> // I2C LCD library
#include <Arduino.h>
#include <TinyGsmClient.h>     // TinyGSM library for SIM800L

#define BLYNK_PRINT Serial
#define BLYNK_HEARTBEAT 30
#include <BlynkSimpleEsp32.h>  // Blynk library for ESP32 (WiFi)

// Blynk credentials
#define BLYNK_TEMPLATE_ID "--------------"
#define BLYNK_TEMPLATE_NAME "------------------"
char auth[] = "------------------------------------";
char ssid[] = "-------------";
char pass[] = "---------------"; // WiFi password

// GSM settings
const char apn[] = "internet";
const char gprsUser[] = "";
const char gprsPass[] = "";
const char* phoneNumber = "000000000000000000";

// Pin definitions
#define BUTTON_PIN 33   // Button to trigger measurement
#define BUZZER_PIN 32   // Buzzer for alerts
#define RED_LED 19      // RGB Red pin
#define GREEN_LED 18    // RGB Green pin
#define BLUE_LED 13     // RGB Blue pin

// LCD setup (20x4 display)
LiquidCrystal_I2C lcd(0x27, 20, 4);

// MAX30102 sensor object
MAX30105 particleSensor;

// GSM modem setup
TinyGsm modem(Serial1);

// Buffer for sensor data (32-bit for ESP32)
uint32_t irBuffer[100];
uint32_t redBuffer[100];

// Measurement variables
int32_t bufferLength = 100;
int32_t spo2;
int8_t validSPO2;
int32_t heartRate;
int8_t validHeartRate;

// Timing constants
const long MEASUREMENT_DURATION = 15000; // 15 seconds measurement period
const long ALERT_DURATION = 5000;        // 5 seconds for alerts to stay on
unsigned long lastMeasurement = 0;
unsigned long alertStartTime = 0;

// Thresholds for alerts
#define BPM_HIGH 100
#define BPM_LOW 60
#define SPO2_LOW 95

// Debounce variables for button
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;
int lastButtonState = HIGH;

// State flags
bool isIdleMessageDisplayed = false;

// Function prototypes
void displayWelcomeScreen();
void measurePulseAndSpo2();
void displayResults();
void updateAlerts();
void sendSMS(String message);
void blinkBuzzer(int blinks);
void displayIdleMessage();


void updateLCD(String message, bool isLoading = false) {
  lcd.clear();  // Always clear before updating
  lcd.setCursor(0, 1);  // Use row 1 for the main message
  lcd.print(message);

  if (isLoading) {
    lcd.setCursor(0, 3);  // Use row 3 for loading animation
    lcd.print(F("Loading"));
    for (int i = 0; i < 3; i++) {
      lcd.print(".");
      delay(500);  // Creates a dot animation effect
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(10);

  Wire.begin(I2C_SDA, I2C_SCL);

  pinMode(MODEM_PWKEY, OUTPUT);
  pinMode(MODEM_RST, OUTPUT);
  pinMode(MODEM_POWER_ON, OUTPUT);
  digitalWrite(MODEM_PWKEY, LOW);
  digitalWrite(MODEM_RST, HIGH);
  digitalWrite(MODEM_POWER_ON, HIGH);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(BLUE_LED, OUTPUT);

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(RED_LED, LOW);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(BLUE_LED, LOW);

  lcd.init();
  lcd.backlight();
  displayWelcomeScreen();

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println(F("MAX30102 not found. Check wiring/power."));
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("Sensor Error"));
    while (1);
  }

  particleSensor.setup(60, 4, 2, 100, 411, 4096);

  Serial1.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(3000);
  Serial.println(F("Initializing modem..."));
  updateLCD(F("Initializing modem"), true);
  modem.restart();
  delay(1000);

  String modemInfo = modem.getModemInfo();
  Serial.print(F("Modem: "));
  Serial.println(modemInfo);

  updateLCD(F("Modem Ready"));
  delay(2000);

  // Network Connection
  Serial.print(F("Waiting for network..."));
  updateLCD(F("Waiting for network"), true);

  if (!modem.waitForNetwork(240000L)) {
    Serial.println(F(" fail"));
    updateLCD(F("Network Error"));
    delay(3000);
    return;
  }

  Serial.println(F(" OK"));
  updateLCD(F("Network OK"));
  delay(1000);

  // APN Connection
  Serial.print(F("Connecting to APN: "));
  Serial.print(apn);
  updateLCD(F("Connecting to APN"), true);

  if (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
    Serial.println(F(" fail"));
    updateLCD(F("GPRS Error"));
    delay(3000);
    return;
  }

  Serial.println(F(" OK"));
  updateLCD(F("GPRS Connected"));
  delay(1000);

  // WiFi & Blynk connection
  Serial.println(F("Connecting to WiFi..."));
  updateLCD(F("Connecting to WiFi"), true);

  Blynk.begin(auth, ssid, pass);

  updateLCD(F("Connected to Blynk"));
  delay(2000);

  lcd.clear();  // Final cleanup after all connections
  updateLCD(F("System Ready"));
  delay(2000);


  blinkBuzzer(3);
}

void loop() {
  Blynk.run();

  int buttonState = digitalRead(BUTTON_PIN);
  if (buttonState != lastButtonState) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (buttonState == LOW) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print(F("Measuring..."));
      measurePulseAndSpo2();
      displayResults();
      updateAlerts();
      String resultMsg = "Biotelemetry Report:\n" +
                         String("Heart Rate: ") + heartRate + " BPM\n" +
                         String("SPO2: ") + spo2 + "%\n" +
                         String("Condition: ") +
                         (heartRate > BPM_HIGH ? "High BPM" : heartRate < BPM_LOW ? "Low BPM" : "Normal BPM") + ", " +
                         (spo2 < SPO2_LOW ? "Low SPO2" : "Normal SPO2");
      sendSMS(resultMsg);
      isIdleMessageDisplayed = false; // Reset idle message flag
    }
  }
  lastButtonState = buttonState;

  // Display idle message only once
  if (millis() - lastMeasurement > MEASUREMENT_DURATION && !isIdleMessageDisplayed) {
    displayIdleMessage();
    isIdleMessageDisplayed = true;
  }

  // Turn off LEDs after ALERT_DURATION
  if (alertStartTime > 0 && millis() - alertStartTime > ALERT_DURATION) {
    digitalWrite(RED_LED, LOW);
    digitalWrite(GREEN_LED, LOW);
    digitalWrite(BLUE_LED, LOW);
    alertStartTime = 0; // Reset alert timer
  }
}

void displayWelcomeScreen() {
  lcd.clear();

  // Centered text for a 20x4 LCD
  lcd.setCursor(0, 0);  
  lcd.print(F("Biotelemetry System"));

  lcd.setCursor(1, 1);  
  lcd.print(F("for Remote Patient"));

  

  delay(5000);
  lcd.clear();
}


void measurePulseAndSpo2() {
  for (byte i = 0; i < bufferLength; i++) {
    while (!particleSensor.available()) particleSensor.check();
    redBuffer[i] = particleSensor.getRed();
    irBuffer[i] = particleSensor.getIR();
    particleSensor.nextSample();
  }
  maxim_heart_rate_and_oxygen_saturation(irBuffer, bufferLength, redBuffer, &spo2, &validSPO2, &heartRate, &validHeartRate);

  unsigned long startTime = millis();
  while (millis() - startTime < MEASUREMENT_DURATION) {
    for (byte i = 25; i < 100; i++) {
      redBuffer[i - 25] = redBuffer[i];
      irBuffer[i - 25] = irBuffer[i];
    }
    for (byte i = 75; i < 100; i++) {
      while (!particleSensor.available()) particleSensor.check();
      redBuffer[i] = particleSensor.getRed();
      irBuffer[i] = particleSensor.getIR();
      particleSensor.nextSample();
      maxim_heart_rate_and_oxygen_saturation(irBuffer, bufferLength, redBuffer, &spo2, &validSPO2, &heartRate, &validHeartRate);
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print(F("Measuring..."));
      lcd.setCursor(0, 1);
      lcd.print(F("BPM: "));
      lcd.print(heartRate);
      lcd.setCursor(0, 2);
      lcd.print(F("SPO2: "));
      lcd.print(spo2);
      lcd.print(F("%"));
      // Update Blynk in real-time
      Blynk.virtualWrite(V1, heartRate);
      Blynk.virtualWrite(V2, spo2);
      delay(50);
    }
  }
  lastMeasurement = millis();
}

void displayResults() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Final Results:"));
  lcd.setCursor(0, 1);
  lcd.print(F("Heart Rate: "));
  lcd.print(heartRate);
  lcd.print(F(" BPM"));
  lcd.setCursor(0, 2);
  lcd.print(F("SPO2: "));
  lcd.print(spo2);
  lcd.print(F("%"));
}

void updateAlerts() {
  digitalWrite(RED_LED, LOW);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(BLUE_LED, LOW);

  if (heartRate > BPM_HIGH || spo2 < SPO2_LOW) {
    digitalWrite(RED_LED, HIGH);
    blinkBuzzer(3);
  } else if (heartRate < BPM_LOW) {
    digitalWrite(BLUE_LED, HIGH);
    blinkBuzzer(2);
  } else {
    digitalWrite(GREEN_LED, HIGH);
    blinkBuzzer(1);
  }
  alertStartTime = millis(); // Start the alert timer
}

void sendSMS(String message) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Sending SMS..."));

  if (modem.sendSMS(phoneNumber, message)) {
    Serial.println(F("SMS sent successfully"));

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("SMS Sent!"));
    lcd.setCursor(0, 1);
    lcd.print(F("Message:"));
    lcd.setCursor(0, 2);
    lcd.print(message.substring(0, 20)); // Display first 20 characters
    lcd.setCursor(0, 3);
    if (message.length() > 20) {
      lcd.print(message.substring(20, 40)); // Display next 20 characters if needed
    }
  } else {
    Serial.println(F("SMS failed to send"));

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("SMS Failed!"));
  }

  delay(3000); // Wait 3 seconds before clearing
  lcd.clear();
}


void blinkBuzzer(int blinks) {
  for (int i = 0; i < blinks; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);
    delay(100);
  }
}

void displayIdleMessage() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Press button to"));
  lcd.setCursor(0, 1);
  lcd.print(F("start measurement"));
}
