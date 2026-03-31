#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <Wire.h>
#include <Adafruit_INA219.h>

const char* WIFI_SSID = "BergHome";
const char* WIFI_PASS = "skipperdog";

// Textbelt
const char* TEXTBELT_KEY = "4f51282a53a858e6600f182132255eb847e297de6BDpKCkgMtNea6GDc1TY3du79";

const char* ALERT_TO_1 = "12064629834"; // David
// const char* ALERT_TO_2 = "12064279090"; // Seth

// Hardware - Adafruit HUZZAH Feather ESP8266
const int FLOAT_PIN = 12;
const int RED_LED_PIN = 0;
const int BLUE_LED_PIN = 2;
const int BUZZER_PIN = 13;  // GPIO13 (D7)

// INA219
Adafruit_INA219 ina219;

// Timing for Bilge Alarm resend
const unsigned long DEBOUNCE_MS = 3000;
const unsigned long REMINDER_MS = 2UL * 60UL * 60UL * 1000UL;  // 2 hours

// Duplicate-send protection
const unsigned long MIN_SMS_GAP_MS = 60000;  // 1 minute minimum gap between SMS attempts

// Energy-conscious timing
const unsigned long WIFI_RETRY_MS = 30000;
const unsigned long LOOP_DELAY_MS = 100;

// LED timing
const unsigned long STATUS_PERIOD_MS = 2000;       // blue pulse every 2 seconds when WiFi connected
const unsigned long STATUS_ON_MS = 60;
const unsigned long WATER_HIGH_PERIOD_MS = 400;    // red flash when float switch closed
const unsigned long WATER_HIGH_ON_MS = 80;

// WiFi disconnected alert pattern:
// red pulse, then blue pulse slightly later, repeat every 2000ms
const unsigned long WIFI_ALERT_PERIOD_MS = 2000;
const unsigned long WIFI_ALERT_ON_MS = 120;
const unsigned long WIFI_ALERT_BLUE_OFFSET_MS = 180;

// Voltage alarm
const float LOW_VOLTAGE_THRESHOLD = 11.4;
const float LOW_VOLTAGE_RECOVERY = 11.6;  // hysteresis to keep rapid bouncing at 11.4v
const unsigned long LOW_VOLTAGE_DEBOUNCE_MS = 10UL * 60UL * 1000UL; // 10 minutes
const unsigned long VOLTAGE_SAMPLE_MS = 5000;
const unsigned long VOLTAGE_COUNTDOWN_PRINT_MS = 5000;

// Bilge alarm state
bool alarmActive = false;
bool smsSentThisCycle = false;

// Shared SMS / WiFi state
bool smsSendInProgress = false;
bool lastWiFiConnected = false;
bool lastWaterHighState = false;

unsigned long triggerStart = 0;
unsigned long lastSmsTime = 0;
unsigned long lastSmsAttemptTime = 0;
unsigned long lastWiFiAttempt = 0;

// Voltage alarm state
float lastBusVoltage = 0.0;
unsigned long lastVoltageSampleTime = 0;
unsigned long lowVoltageStart = 0;
unsigned long lastVoltageCountdownPrint = 0;
bool lowVoltageActive = false;
bool lowVoltageSmsSent = false;

// LED phase anchors so patterns restart cleanly on state changes
unsigned long statusPhaseStart = 0;
unsigned long waterHighPhaseStart = 0;
unsigned long wifiAlertPhaseStart = 0;

// Active LOW LEDs
void ledOn(int pin) { digitalWrite(pin, LOW); }
void ledOff(int pin) { digitalWrite(pin, HIGH); }

// Buzzer helpers
void buzzerOn() { digitalWrite(BUZZER_PIN, HIGH); }
void buzzerOff() { digitalWrite(BUZZER_PIN, LOW); }

void allLedsOff() {
  ledOff(RED_LED_PIN);
  ledOff(BLUE_LED_PIN);
}

void monitorWiFi() {
  bool currentConnected = (WiFi.status() == WL_CONNECTED);

  if (currentConnected != lastWiFiConnected) {
    lastWiFiConnected = currentConnected;

    if (currentConnected) {
      Serial.println("WiFi CONNECTED");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
      statusPhaseStart = millis();
    } else {
      Serial.println("WiFi DISCONNECTED");
      wifiAlertPhaseStart = millis();
    }
  }
}

void updateStatusLEDs(bool waterHigh) {
  unsigned long now = millis();

  // Default buzzer state: off unless water-high alarm pulse is active
  buzzerOff();

  // Highest priority: float switch closed -> flash red continuously until open again
  if (waterHigh) {
    unsigned long phase = (now - waterHighPhaseStart) % WATER_HIGH_PERIOD_MS;
    ledOff(BLUE_LED_PIN);

    if (phase < WATER_HIGH_ON_MS) {
      ledOn(RED_LED_PIN);
      buzzerOn();   // beep in sync with red flash
    } else {
      ledOff(RED_LED_PIN);
      buzzerOff();
    }
    return;
  }

  // Low voltage active -> slow red blink, no buzzer
  if (lowVoltageActive) {
    unsigned long phase = (now / 1000UL) % 2UL;
    ledOff(BLUE_LED_PIN);

    if (phase == 0) {
      ledOn(RED_LED_PIN);
    } else {
      ledOff(RED_LED_PIN);
    }
    return;
  }

  // WiFi disconnected -> short red then blue pulses, slightly offset
  if (WiFi.status() != WL_CONNECTED) {
    unsigned long phase = (now - wifiAlertPhaseStart) % WIFI_ALERT_PERIOD_MS;

    bool redPulse =
      (phase < WIFI_ALERT_ON_MS);

    bool bluePulse =
      (phase >= WIFI_ALERT_BLUE_OFFSET_MS &&
       phase < WIFI_ALERT_BLUE_OFFSET_MS + WIFI_ALERT_ON_MS);

    if (redPulse) ledOn(RED_LED_PIN);
    else ledOff(RED_LED_PIN);

    if (bluePulse) ledOn(BLUE_LED_PIN);
    else ledOff(BLUE_LED_PIN);

    return;
  }

  // WiFi connected -> short blue pulse every 2 seconds
  unsigned long phase = (now - statusPhaseStart) % STATUS_PERIOD_MS;
  ledOff(RED_LED_PIN);
  if (phase < STATUS_ON_MS) {
    ledOn(BLUE_LED_PIN);
  } else {
    ledOff(BLUE_LED_PIN);
  }
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_LIGHT_SLEEP);

  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
    bool waterHigh = (digitalRead(FLOAT_PIN) == LOW);
    updateStatusLEDs(waterHigh);
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected (initial/reconnect).");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    statusPhaseStart = millis();
  } else {
    Serial.println("WiFi failed.");
    wifiAlertPhaseStart = millis();
  }
}

String urlencode(const String& s) {
  String out = "";
  char c;
  char buf[4];

  for (unsigned int i = 0; i < s.length(); i++) {
    c = s.charAt(i);
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else if (c == ' ') {
      out += "%20";
    } else {
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      out += buf;
    }
  }
  return out;
}

bool sendTextbeltSMS(const String& message) {
  unsigned long now = millis();

  if (smsSendInProgress) {
    return false;
  }

  if (lastSmsAttemptTime != 0 && (now - lastSmsAttemptTime < MIN_SMS_GAP_MS)) {
    return false;
  }

  smsSendInProgress = true;
  lastSmsAttemptTime = now;

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) {
      smsSendInProgress = false;
      return false;
    }
  }

  bool overallSuccess = true;

  auto sendOne = [&](const char* phone) -> bool {
    std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
    client->setInsecure();

    HTTPClient https;
    if (!https.begin(*client, "https://textbelt.com/text")) {
      return false;
    }

    https.addHeader("Content-Type", "application/x-www-form-urlencoded");

    String postData =
        "phone=" + urlencode(String(phone)) +
        "&message=" + urlencode(message) +
        "&key=" + urlencode(String(TEXTBELT_KEY));

    int httpCode = https.POST(postData);
    String response = https.getString();
    https.end();

    Serial.print("HTTP code: ");
    Serial.println(httpCode);

    Serial.print("Full response: ");
    Serial.println(response);

    int idx = response.indexOf("quotaRemaining");
    if (idx != -1) {
      int start = response.indexOf(":", idx) + 1;
      int end = response.indexOf(",", start);
      if (end == -1) end = response.indexOf("}", start);
      String quota = response.substring(start, end);
      quota.trim();

      Serial.print("Quota remaining: ");
      Serial.println(quota);
    } else {
      Serial.println("Quota remaining: NOT FOUND");
    }

    bool ok = (httpCode >= 200 && httpCode < 300 && response.indexOf("\"success\":true") != -1);

    Serial.print("SMS result: ");
    Serial.println(ok ? "SUCCESS" : "FAIL");

    return ok;
  };

  if (!sendOne(ALERT_TO_1)) {
    overallSuccess = false;
  }

  // Uncomment ALERT_TO_2 above to enable Seth
  // if (!sendOne(ALERT_TO_2)) {
  //   overallSuccess = false;
  // }

  smsSendInProgress = false;
  return overallSuccess;
}

void resetAlarmState() {
  triggerStart = 0;
  alarmActive = false;
  smsSentThisCycle = false;
}

void sampleVoltage() {
  unsigned long now = millis();

  if (now - lastVoltageSampleTime < VOLTAGE_SAMPLE_MS) {
    return;
  }
  lastVoltageSampleTime = now;

  lastBusVoltage = ina219.getBusVoltage_V();

  Serial.print("Battery Voltage: ");
  Serial.print(lastBusVoltage, 2);
  Serial.println(" V");
}

void handleVoltageAlarm() {
  // Only act if we have a plausible reading
  if (lastBusVoltage <= 0.1) {
    return;
  }

  unsigned long now = millis();

  if (lastBusVoltage >= LOW_VOLTAGE_RECOVERY) {
    if (lowVoltageStart != 0 || lowVoltageActive) {
      Serial.print("Voltage timer stopped, voltage above ");
      Serial.print(LOW_VOLTAGE_RECOVERY, 2);
      Serial.println("V");
    }

    lowVoltageStart = 0;
    lastVoltageCountdownPrint = 0;
    lowVoltageActive = false;
    lowVoltageSmsSent = false;
    return;
  }

  if (lastBusVoltage < LOW_VOLTAGE_THRESHOLD) {
    if (lowVoltageStart == 0) {
      lowVoltageStart = now;
      lastVoltageCountdownPrint = 0;
      Serial.println("Low voltage timer started.");
    }

    if (!lowVoltageActive) {
      unsigned long elapsedMs = now - lowVoltageStart;

      if (elapsedMs < LOW_VOLTAGE_DEBOUNCE_MS) {
        unsigned long remainingMs = LOW_VOLTAGE_DEBOUNCE_MS - elapsedMs;

        if (lastVoltageCountdownPrint == 0 ||
            now - lastVoltageCountdownPrint >= VOLTAGE_COUNTDOWN_PRINT_MS) {
          lastVoltageCountdownPrint = now;

          unsigned long remainingTotalSec = remainingMs / 1000UL;
          unsigned long remainingMin = remainingTotalSec / 60UL;
          unsigned long remainingSec = remainingTotalSec % 60UL;

          Serial.print("Low voltage alert in ");
          Serial.print(remainingMin);
          Serial.print("m ");
          Serial.print(remainingSec);
          Serial.println("s");
        }
      } else {
        lowVoltageActive = true;
        Serial.println("Low voltage alarm ACTIVE");
      }
    }

    if (lowVoltageActive && !lowVoltageSmsSent) {
      String msg = "Voltage alert: battery voltage has been below 11.4v for more than 10 minutes. Current voltage: ";
      msg += String(lastBusVoltage, 2);
      msg += "V.";

      if (sendTextbeltSMS(msg)) {
        lowVoltageSmsSent = true;
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("Bilge alarm starting...");

  pinMode(FLOAT_PIN, INPUT_PULLUP);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(BLUE_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  allLedsOff();
  buzzerOff();

  unsigned long now = millis();
  statusPhaseStart = now;
  waterHighPhaseStart = now;
  wifiAlertPhaseStart = now;

  Wire.begin(4, 5);  // SDA, SCL
  if (!ina219.begin()) {
    Serial.println("INA219 not found");
  } else {
    Serial.println("INA219 ready");
  }

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi CONNECTED (startup)");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi DISCONNECTED (startup)");
  }

  lastWiFiConnected = (WiFi.status() == WL_CONNECTED);
}

void loop() {
  unsigned long now = millis();
  bool waterHigh = (digitalRead(FLOAT_PIN) == LOW);

  // Detect water state transitions so LED patterns restart cleanly on state changes
  if (waterHigh != lastWaterHighState) {
    lastWaterHighState = waterHigh;
    Serial.print("Water high: ");
    Serial.println(waterHigh ? "YES" : "NO");

    if (waterHigh) {
      waterHighPhaseStart = now;
    } else {
      statusPhaseStart = now;
      wifiAlertPhaseStart = now;
    }
  }

  monitorWiFi();
  sampleVoltage();
  handleVoltageAlarm();
  updateStatusLEDs(waterHigh);

  if (WiFi.status() != WL_CONNECTED && now - lastWiFiAttempt >= WIFI_RETRY_MS) {
    lastWiFiAttempt = now;
    connectWiFi();
  }

  if (waterHigh) {
    if (!alarmActive) {
      if (triggerStart == 0) {
        triggerStart = now;
      } else if (now - triggerStart >= DEBOUNCE_MS) {
        alarmActive = true;
        Serial.println("Alarm ACTIVE");
      }
    }

    if (alarmActive) {
      if (!smsSentThisCycle) {
        if (sendTextbeltSMS("Bilge alert: Alibi is experiencing high bilge water.")) {
          smsSentThisCycle = true;
          lastSmsTime = now;
        }
      } else if (now - lastSmsTime >= REMINDER_MS) {
        if (sendTextbeltSMS("Bilge alert reminder: high water still detected in Alibi.")) {
          lastSmsTime = now;
        }
      }
    }
  } else {
    resetAlarmState();
  }

  delay(LOOP_DELAY_MS);
}