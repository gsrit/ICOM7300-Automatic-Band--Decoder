#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h> // WiFiManager by tzapu

// 2004 LCD Setup (I2C address 0x27)
LiquidCrystal_I2C lcd(0x27, 20, 4);

// Web Server on Port 80 for Live Monitoring
ESP8266WebServer server(80);

// Hardware Pin Definitions
const int LED_PIN = 12;         // GPIO12 (External Indicator)
const int BUZZER_PIN = 13;      // GPIO13 (Piezo Buzzer)
const int BLUE_LED_PIN = LED_BUILTIN; // GPIO2 (Onboard Blue LED)
const int ADC_PIN = A0;

// --- ACCURATE BENCH CALIBRATION CONSTANTS ---
// ESP8266 NodeMCU board A0 input max range is 3.14V (Internal 220k/100k divider)
const float ADC_MAX_VOLTAGE = 3.136; 
const int ADC_MAX_VALUE = 1023;

// Ratio converting A0 pin voltage to actual ICOM ACC voltage
const float VOLTAGE_DIVIDER_RATIO = 11.0; 

struct BandInfo {
  String band;
  String frequency;
};

String previousBand = "";
float currentActualVoltage = 0.0;
float currentA0Voltage = 0.0;
BandInfo currentBandInfo = {"INIT", "INIT"};

// Metrics Tracking
unsigned long webRequestsHandled = 0;

// Corrected ICOM IC-7300 Band & Frequency Mapping based on exact A0 voltages
BandInfo getIcomBandInfo(float a0Voltage) {
  if (a0Voltage >= 0.00 && a0Voltage < 0.25) return {"10M / 12M", "28M / 24M"};
  if (a0Voltage >= 0.25 && a0Voltage < 0.34) return {"15M / 17M", "21M / 18M"};
  if (a0Voltage >= 0.34 && a0Voltage < 0.42) return {"20M",       "14M"};
  if (a0Voltage >= 0.42 && a0Voltage < 0.51) return {"40M",       "7M"};
  if (a0Voltage >= 0.51 && a0Voltage < 0.62) return {"80M",       "3.5M"};
  if (a0Voltage >= 0.62 && a0Voltage < 0.85) return {"160M",      "1.8M"};
  return {"N/A", "UNKNOWN"};
}

void playBeep(unsigned int frequency, unsigned long durationMs) {
  tone(BUZZER_PIN, frequency, durationMs);
  unsigned long start = millis();
  while (millis() - start < durationMs) {
    ESP.wdtFeed();
    yield();
  }
  noTone(BUZZER_PIN);
}

// Aviation Strobe Engine (tik-tak non-blocking state machine)
void updateAviationStrobe() {
  static unsigned long lastStrobeTime = 0;
  unsigned long currentMillis = millis();
  unsigned long cycleTime = (currentMillis - lastStrobeTime) % 1200; // 1.2 sec total pulse period

  // Inverted logic for ESP8266 onboard LED (LOW = ON, HIGH = OFF)
  if (cycleTime < 40) {
    digitalWrite(BLUE_LED_PIN, LOW);   // Flash 1 ON ("tik")
  } else if (cycleTime < 120) {
    digitalWrite(BLUE_LED_PIN, HIGH);  // OFF gap
  } else if (cycleTime < 160) {
    digitalWrite(BLUE_LED_PIN, LOW);   // Flash 2 ON ("tak")
  } else {
    digitalWrite(BLUE_LED_PIN, HIGH);  // Long pause before next double strobe
  }
}

// Serves web dashboard for HTTP monitoring
void handleRoot() {
  webRequestsHandled++;
  
  // Calculate percentage for 0V to 8V bar scale
  float pct = (currentActualVoltage / 8.0f) * 100.0f;
  if (pct > 100.0f) pct = 100.0f;
  if (pct < 0.0f) pct = 0.0f;

  String html = "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='2'>";
  html += "<title>ICOM 7300 - VU33OM Monitor</title>";
  html += "<style>body{font-family:Arial,sans-serif;background:#121212;color:#00ffcc;padding:20px;text-align:center;}";
  html += ".card{background:#1e1e1e;padding:15px;border-radius:10px;margin:15px auto;max-width:500px;box-shadow:0 0 10px #00ffcc33;text-align:left;}";
  html += ".big-band{font-size:48px;font-weight:bold;color:#ffcc00;margin:10px 0;text-align:center;}";
  html += ".bar-container{background:#333;border-radius:8px;height:24px;width:100%;overflow:hidden;margin-top:8px;}";
  html += ".bar-fill{background:#00ffcc;height:100%;transition:width 0.5s;}";
  html += "h2,h3{color:#fff;}span{color:#ffcc00;font-weight:bold;}</style></head><body>";
  
  html += "<h2>ICOM IC-7300 Radio Monitor (VU33OM)</h2>";
  
  // Large centered Band display
  html += "<div class='card' style='text-align:center;'>";
  html += "<h3>SELECTED BAND</h3>";
  html += "<div class='big-band'>" + currentBandInfo.band + "</div>";
  html += "<p style='font-size:18px;'>Frequency Range: <span>" + currentBandInfo.frequency + "</span></p>";
  html += "</div>";

  // Voltage Bar section (0 to 8 Volts scale)
  html += "<div class='card'>";
  html += "<h3>ACC Voltage Monitor</h3>";
  html += "<p>ACC Voltage: <span>" + String(currentActualVoltage, 2) + " V</span> (Scale: 0V - 8V)</p>";
  html += "<div class='bar-container'><div class='bar-fill' style='width:" + String(pct, 1) + "%;'></div></div>";
  html += "<p style='font-size:12px;color:#aaa;margin-top:8px;'>A0 Pin Voltage: " + String(currentA0Voltage, 2) + " V</p>";
  html += "</div>";

  // WiFi Metrics section
  html += "<div class='card'>";
  html += "<h3>Wi-Fi Status & Metrics</h3>";
  html += "<p>Connected SSID: <span>" + WiFi.SSID() + "</span></p>";
  html += "<p>Signal Strength (RSSI): <span>" + String(WiFi.RSSI()) + " dBm</span></p>";
  html += "<p>IP Address: <span>" + WiFi.localIP().toString() + "</span></p>";
  html += "<p>Free Heap RAM: <span>" + String(ESP.getFreeHeap()) + " bytes</span></p>";
  html += "<p>HTTP Stress Requests Handled: <span>" + String(webRequestsHandled) + "</span></p>";
  html += "<p>System Uptime: <span>" + String(millis() / 1000) + " seconds</span></p>";
  html += "</div>";

  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void setup() {
  ESP.wdtEnable(WDTO_8S);

  Serial.begin(115200);
  yield();

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BLUE_LED_PIN, OUTPUT);

  digitalWrite(LED_PIN, HIGH);
  digitalWrite(BLUE_LED_PIN, HIGH); // Inverted logic: OFF

  // Safe I2C Initialization
  Wire.begin(4, 5); 
  Wire.setClock(100000); 
  #if defined(ESP8266)
    Wire.setClockStretchLimit(15000);
  #endif

  ESP.wdtFeed();

  lcd.init();
  lcd.backlight();
  
  // --- BOOT WELCOME SCREEN ---
  lcd.setCursor(0, 0);
  lcd.print("====================");
  lcd.setCursor(0, 1);
  lcd.print("  CALLSIGN: VU33OM  ");
  lcd.setCursor(0, 2);
  lcd.print(" GAURAV SINGH ");
  lcd.setCursor(0, 3);
  lcd.print("====================");
  
  // Boot Chime
  playBeep(1000, 80);
  delay(50);
  playBeep(1500, 80);
  delay(50);
  playBeep(2000, 120);

  delay(1500);

  // --- CAPTIVE PORTAL AP SETUP VIA WIFIMANAGER ---
  WiFiManager wm;
  
  WiFi.setOutputPower(20.5);

  // Update LCD if AP mode gets triggered
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("AP MODE ACTIVE");
  lcd.setCursor(0, 1);
  lcd.print("SSID:ICOM-7300-VU33OM");
  lcd.setCursor(0, 2);
  lcd.print("IP: 192.168.4.1");
  lcd.setCursor(0, 3);
  lcd.print("Connect to Config...");

  // AutoConnect block: creates AP if no known Wi-Fi is found within reach
  bool res = wm.autoConnect("ICOM-7300-VU33OM"); 

  if (!res) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Wi-Fi Config Failed");
    lcd.setCursor(0, 1);
    lcd.print("Restarting...");
    delay(3000);
    ESP.restart();
  }

  // Once connected successfully
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("WiFi Connected!");
  lcd.setCursor(0, 1);
  lcd.print("IP: " + WiFi.localIP().toString());
  
  // Short confirmation beep after successful Wi-Fi connection
  playBeep(2400, 100);

  server.on("/", handleRoot);
  server.begin();

  delay(2000);

  digitalWrite(LED_PIN, LOW);
  lcd.clear();
}

unsigned long counter = 0;
unsigned long lastUpdate = 0;

void loop() {
  ESP.wdtFeed();
  yield();

  // Run aviation strobe continuously
  updateAviationStrobe();

  // Process Web Server Client Requests
  server.handleClient();

  // 500ms Display & ADC Sampling Cycle
  if (millis() - lastUpdate >= 500) {
    lastUpdate = millis();
    counter++;

    int rawADC = analogRead(A0);
    currentA0Voltage = ((float)rawADC / ADC_MAX_VALUE) * ADC_MAX_VOLTAGE;
    currentActualVoltage = currentA0Voltage * VOLTAGE_DIVIDER_RATIO;

    // Pass A0 voltage directly into calibrated band decoder
    currentBandInfo = getIcomBandInfo(currentA0Voltage);

    // Audio and LED indicator on band switch
    if (previousBand != "" && currentBandInfo.band != previousBand) {
      digitalWrite(LED_PIN, HIGH);
      playBeep(1800, 80);
      digitalWrite(LED_PIN, LOW);

      Serial.print("BAND CHANGED TO: ");
      Serial.println(currentBandInfo.band);
    }
    previousBand = currentBandInfo.band;

    // Line 1: Alternate title and WiFi IP
    lcd.setCursor(0, 0);
    if (counter % 4 < 2) {
      if (counter % 2 == 0) {
        lcd.print("* ICOM 7300 VU33OM *");
      } else {
        lcd.print("  ICOM 7300 VU33OM  ");
      }
    } else {
      String wifiStr = "IP:" + WiFi.localIP().toString();
      lcd.print(wifiStr);
      for (int i = 0; i < (20 - (int)wifiStr.length()); i++) lcd.print(" ");
    }

    // Line 2: BAND display
    lcd.setCursor(0, 1);
    lcd.print("BAND :");
    lcd.print(currentBandInfo.band);
    for (int i = 0; i < (14 - (int)currentBandInfo.band.length()); i++) lcd.print(" ");

    // Line 3: Frequency display
    lcd.setCursor(0, 2);
    lcd.print("Frequency: ");
    lcd.print(currentBandInfo.frequency);
    for (int i = 0; i < (9 - (int)currentBandInfo.frequency.length()); i++) lcd.print(" ");

    // Line 4: Voltage monitor (ICOM ACC & A0 Pin)
    lcd.setCursor(0, 3);
    lcd.print("IC-V:");
    lcd.print(currentActualVoltage, 2);
    lcd.print("V A0-V:");
    lcd.print(currentA0Voltage, 2);
    lcd.print("V");
  }
}