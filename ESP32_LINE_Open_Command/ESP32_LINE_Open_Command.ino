// ==================================================
// ESP32 DEVKIT V1
// SOIL MOISTURE + 2 RELAYS + 2 x YF-S201
// LINE WEBHOOK THROUGH PYTHON CLOUD (RENDER)
// No JavaScript / No Google Apps Script
//
// Arduino libraries:
// 1) ArduinoJson (install from Library Manager)
// ==================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ---------------- WIFI ----------------
const char* WIFI_SSID = "iPhone";
const char* WIFI_PASSWORD = "11111111";

// ใส่ URL ของ Render หลัง Deploy เช่น https://esp32-line-cloud.onrender.com
const char* CLOUD_BASE_URL = "https://esp32-line-cloud1.onrender.com";

// ต้องตรงกับ DEVICE_KEY ที่ตั้งใน Render Environment
const char* DEVICE_KEY = "1123";

// ---------------- PINS ----------------
const int sensorPin = 36;
const int Relay1 = 22;
const int Relay2 = 23;
const int FLOW1_PIN = 32;
const int FLOW2_PIN = 33;

// ตรวจสอบให้ตรงกับบอร์ดรีเลย์จริง
#define RELAY_ON LOW
#define RELAY_OFF HIGH

// ---------------- SOIL SETTINGS ----------------
const int dry = 4095;
const int wet = 900;
const int moistureON = 50;
const int moistureOFF = 55;

// ---------------- FLOW SETTINGS ----------------
const float PULSES_PER_LITER = 450.0;
const unsigned long FLOW_GRACE_TIME = 3000;
const unsigned long FLOW_TIMEOUT = 10000;
const unsigned long FLOW_STOP_TIMEOUT = 5000;

// ---------------- TIMING ----------------
const unsigned long readInterval = 2000;
const unsigned long wifiRetryInterval = 10000;
const unsigned long flowSampleInterval = 1000;
const unsigned long cloudPollInterval = 3000;
const unsigned long reportRetryInterval = 5000;

// ---------------- VARIABLES ----------------
int rawSensorValue = 0;
int PercentageHumidity = 0;

bool relay1State = false;
bool relay2State = false;
String mode = "AUTO";

unsigned long lastReadTime = 0;
unsigned long lastWiFiAttempt = 0;
unsigned long lastCloudPollTime = 0;
unsigned long lastReportAttemptTime = 0;

volatile uint32_t flow1Pulses = 0;
volatile uint32_t flow2Pulses = 0;
volatile uint32_t flow1WindowPulses = 0;
volatile uint32_t flow2WindowPulses = 0;

portMUX_TYPE flowMux = portMUX_INITIALIZER_UNLOCKED;

float flow1Rate = 0.0;
float flow2Rate = 0.0;
float flow1Liter = 0.0;
float flow2Liter = 0.0;

bool flowCycleActive = false;
bool flowSuccessNotified = false;
bool flowFailureNotified = false;
bool flowLockout = false;

unsigned long flowStartTime = 0;
unsigned long lastFlowSampleTime = 0;
unsigned long lastFlowPulseSeen1 = 0;
unsigned long lastFlowPulseSeen2 = 0;

String pendingFlowEvent = "";

// แจ้งเตือนเมื่อเริ่มสั่งเปิดรีเลย์ทั้งสองตัวเท่านั้น
bool pendingRelayStart = false;
bool previousWaterCommand = false;

// ป้องกันคำสั่งเดิมถูกประมวลผลซ้ำระหว่างรอผลตอบกลับ
String lastProcessedCommandId = "";

// ==================================================
// FLOW INTERRUPTS
// ==================================================

void IRAM_ATTR flow1ISR() {
  portENTER_CRITICAL_ISR(&flowMux);
  flow1Pulses++;
  flow1WindowPulses++;
  portEXIT_CRITICAL_ISR(&flowMux);
}

void IRAM_ATTR flow2ISR() {
  portENTER_CRITICAL_ISR(&flowMux);
  flow2Pulses++;
  flow2WindowPulses++;
  portEXIT_CRITICAL_ISR(&flowMux);
}

// อ่านจำนวน pulse และล้างเฉพาะค่าหน้าต่างเวลา
void readFlowCounters(
  uint32_t &total1, uint32_t &total2,
  uint32_t &window1, uint32_t &window2
) {
  portENTER_CRITICAL(&flowMux);
  total1 = flow1Pulses;
  total2 = flow2Pulses;
  window1 = flow1WindowPulses;
  window2 = flow2WindowPulses;
  flow1WindowPulses = 0;
  flow2WindowPulses = 0;
  portEXIT_CRITICAL(&flowMux);
}

// อ่านยอดสะสมโดยไม่ล้าง window pulses
void getFlowTotals(uint32_t &total1, uint32_t &total2) {
  portENTER_CRITICAL(&flowMux);
  total1 = flow1Pulses;
  total2 = flow2Pulses;
  portEXIT_CRITICAL(&flowMux);
}

// ==================================================
// HTTPS REQUEST HELPERS
// ==================================================

bool cloudRequest(
  const char* method,
  const String &path,
  const String &payload,
  String &response
) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Cloud request skipped: WiFi disconnected");
    return false;
  }

  WiFiClientSecure client;
  // ใช้สำหรับทดสอบต้นแบบเท่านั้น
  // ก่อนใช้งานจริงควรติดตั้งและตรวจสอบ Root CA certificate
  client.setInsecure();

  HTTPClient http;
  String url = String(CLOUD_BASE_URL) + path;

  if (!http.begin(client, url)) {
    Serial.println("HTTP begin failed");
    return false;
  }

  http.setTimeout(15000);
  http.addHeader("X-Device-Key", DEVICE_KEY);
  http.addHeader("Content-Type", "application/json");

  int code = -1;

  if (String(method) == "GET") {
    code = http.GET();
  } else {
    code = http.POST(payload);
  }

  response = http.getString();

  Serial.print("Cloud HTTP code: ");
  Serial.println(code);

  if (response.length()) {
    Serial.println(response);
  }

  http.end();
  return code >= 200 && code < 300;
}

// ==================================================
// SEND REPORT TO CLOUD
// ==================================================

bool sendCloudReport(const String &message) {
  JsonDocument doc;
  doc["message"] = message;

  String payload;
  serializeJson(doc, payload);

  String response;
  return cloudRequest(
    "POST", "/device/report", payload, response
  );
}

// ==================================================
// READ SOIL MOISTURE
// ==================================================

void readSoilMoisture() {
  long total = 0;

  for (int i = 0; i < 10; i++) {
    total += analogRead(sensorPin);
    delay(10);
  }

  rawSensorValue = total / 10;

  PercentageHumidity = map(
    rawSensorValue, wet, dry, 100, 0
  );

  PercentageHumidity = constrain(
    PercentageHumidity, 0, 100
  );

  Serial.println("--------------------------");
  Serial.print("Sensor Raw: ");
  Serial.println(rawSensorValue);
  Serial.print("Soil Moisture: ");
  Serial.print(PercentageHumidity);
  Serial.println("%");
}

// ==================================================
// START FLOW CYCLE
// ==================================================

void startFlowCycle() {
  portENTER_CRITICAL(&flowMux);
  flow1Pulses = 0;
  flow2Pulses = 0;
  flow1WindowPulses = 0;
  flow2WindowPulses = 0;
  portEXIT_CRITICAL(&flowMux);

  flow1Rate = 0;
  flow2Rate = 0;
  flow1Liter = 0;
  flow2Liter = 0;

  flowSuccessNotified = false;
  flowFailureNotified = false;
  flowCycleActive = true;

  flowStartTime = millis();
  lastFlowSampleTime = millis();
  lastFlowPulseSeen1 = millis();
  lastFlowPulseSeen2 = millis();

  Serial.println("FLOW MONITOR STARTED");
}

// ==================================================
// UPDATE RELAYS
// ==================================================

void updateRelay() {
  // ส่งคำสั่งไปยังขารีเลย์
  digitalWrite(Relay1, relay1State ? RELAY_ON : RELAY_OFF);
  digitalWrite(Relay2, relay2State ? RELAY_ON : RELAY_OFF);

  // ระบบรดน้ำเริ่มเมื่อรีเลย์ทั้งสองตัวถูกสั่งเปิดพร้อมกัน
  bool shouldWater = relay1State && relay2State;

  // สร้างการแจ้งเตือนเฉพาะขอบขาขึ้น: OFF -> ON
  // ไม่แจ้งซ้ำในระหว่างที่รีเลย์ยังเปิดค้างอยู่
  if (shouldWater && !previousWaterCommand) {
    pendingRelayStart = true;
    Serial.println("RELAY START - LINE NOTIFICATION PENDING");
  }

  previousWaterCommand = shouldWater;

  if (shouldWater && !flowCycleActive) {
    startFlowCycle();
  }

  if (!shouldWater && flowCycleActive) {
    flowCycleActive = false;
    flow1Rate = 0;
    flow2Rate = 0;
    Serial.println("FLOW MONITOR STOPPED");
  }

  Serial.print("Valve 1: ");
  Serial.println(relay1State ? "OPEN" : "CLOSED");
  Serial.print("Valve 2: ");
  Serial.println(relay2State ? "OPEN" : "CLOSED");
}

// ==================================================
// AUTOMATIC CONTROL
// ==================================================

void automaticControl() {
  if (mode != "AUTO" || flowLockout) return;

  if (PercentageHumidity <= moistureON) {
    relay1State = true;
    relay2State = true;
    Serial.println("SOIL DRY: OPEN BOTH VALVES");
  }
  else if (PercentageHumidity >= moistureOFF) {
    relay1State = false;
    relay2State = false;
    Serial.println("SOIL WET: CLOSE BOTH VALVES");
  }

  updateRelay();
}

// ==================================================
// CHECK FLOW SENSORS
// ==================================================

void checkFlowSensors() {
  if (!flowCycleActive) return;

  unsigned long now = millis();
  if (now - lastFlowSampleTime < flowSampleInterval) return;

  unsigned long elapsedSample = now - lastFlowSampleTime;
  lastFlowSampleTime = now;

  uint32_t total1, total2, window1, window2;
  readFlowCounters(total1, total2, window1, window2);

  if (window1 > 0) lastFlowPulseSeen1 = now;
  if (window2 > 0) lastFlowPulseSeen2 = now;

  float frequency1 = (float)window1 * 1000.0 / elapsedSample;
  float frequency2 = (float)window2 * 1000.0 / elapsedSample;

  flow1Rate = frequency1 / 7.5;
  flow2Rate = frequency2 / 7.5;

  flow1Liter = (float)total1 / PULSES_PER_LITER;
  flow2Liter = (float)total2 / PULSES_PER_LITER;

  Serial.print("Flow 1: ");
  Serial.print(flow1Rate, 2);
  Serial.print(" L/min | Flow 2: ");
  Serial.print(flow2Rate, 2);
  Serial.println(" L/min");

  bool sensor1Detected = total1 > 0;
  bool sensor2Detected = total2 > 0;
  bool bothDetected = sensor1Detected && sensor2Detected;

  unsigned long elapsed = now - flowStartTime;

  if (
    bothDetected &&
    elapsed >= FLOW_GRACE_TIME &&
    !flowSuccessNotified
  ) {
    Serial.println("BOTH FLOW SENSORS SUCCESS");
    flowSuccessNotified = true;
    pendingFlowEvent = "SUCCESS";
  }

  if (
    elapsed >= FLOW_TIMEOUT &&
    !bothDetected &&
    !flowFailureNotified
  ) {
    Serial.println("FLOW SENSOR ERROR");
    flowFailureNotified = true;
    flowLockout = true;
    relay1State = false;
    relay2State = false;
    updateRelay();
    pendingFlowEvent = "FLOW_ERROR";
    return;
  }

  if (flowSuccessNotified && !flowFailureNotified) {
    bool flow1Stopped =
      now - lastFlowPulseSeen1 >= FLOW_STOP_TIMEOUT;
    bool flow2Stopped =
      now - lastFlowPulseSeen2 >= FLOW_STOP_TIMEOUT;

    if (flow1Stopped || flow2Stopped) {
      Serial.println("WATER FLOW STOPPED");
      flowFailureNotified = true;
      flowLockout = true;
      relay1State = false;
      relay2State = false;
      updateRelay();
      pendingFlowEvent = "FLOW_ERROR";
    }
  }
}

// ==================================================
// CHECK COMMAND FROM CLOUD
// ==================================================

void checkCloudCommand() {
  if (WiFi.status() != WL_CONNECTED) return;

  String response;
  if (!cloudRequest(
    "GET", "/device/command", "", response
  )) {
    return;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);

  if (error) {
    Serial.print("Command JSON error: ");
    Serial.println(error.c_str());
    return;
  }

  if (doc["command"].isNull()) return;

  String command = doc["command"].as<String>();
  String commandId = doc["command_id"].as<String>();

  if (commandId.length() == 0) return;
  if (commandId == lastProcessedCommandId) return;

  Serial.print("CLOUD COMMAND: ");
  Serial.println(command);

  // รองรับคำสั่งตรวจสอบความชื้น และคำสั่ง "เปิด" จาก LINE
  bool isHumidityCommand = (command == "CHECK_HUMIDITY");
  bool isOpenCommand = (command == "OPEN" || command == "เปิด");

  if (isHumidityCommand || isOpenCommand) {
    lastProcessedCommandId = commandId;

    // อ่านค่าความชื้นใหม่ก่อนตัดสินใจทุกครั้ง
    readSoilMoisture();

    String replyMessage = "";

    if (isOpenCommand) {
      if (PercentageHumidity > 50) {
        // ดินชื้นเพียงพอ ไม่เปิดรีเลย์
        replyMessage = "สถานะดินตอนนี้ชื้นเพียงพอแล้ว";
        Serial.println("LINE OPEN COMMAND: SOIL MOISTURE IS ABOVE 50%");
      }
      else if (flowLockout) {
        // ไม่สั่งเปิดซ้ำเมื่อระบบถูกล็อกจากความผิดปกติของ Flow Sensor
        replyMessage = "ระบบยังไม่สามารถทำงานได้ เนื่องจากตรวจพบความผิดปกติของ Flow Sensor กรุณาตรวจสอบระบบก่อน";
        Serial.println("LINE OPEN COMMAND BLOCKED: FLOW LOCKOUT");
      }
      else {
        // ความชื้น 50% หรือต่ำกว่า: เริ่มระบบรดน้ำตามการทำงานปกติ
        mode = "AUTO";
        relay1State = true;
        relay2State = true;
        updateRelay();

        replyMessage = "ความชื้นดิน " + String(PercentageHumidity) +
                       "% ระบบเริ่มทำงานแล้ว";
        Serial.println("LINE OPEN COMMAND: WATERING STARTED");
      }
    }
    else {
      // คำสั่ง CHECK_HUMIDITY เดิม
      replyMessage = "ความชื้นดินปัจจุบัน " +
                     String(PercentageHumidity) + "%";
    }

    JsonDocument resultDoc;
    resultDoc["command_id"] = commandId;
    resultDoc["humidity"] = PercentageHumidity;
    resultDoc["raw"] = rawSensorValue;
    resultDoc["relay1"] = relay1State ? "ON" : "OFF";
    resultDoc["relay2"] = relay2State ? "ON" : "OFF";
    resultDoc["mode"] = mode;

    // Cloud ต้องอ่านข้อความ reply นี้แล้วส่งกลับ LINE
    resultDoc["reply"] = replyMessage;

    String payload;
    serializeJson(resultDoc, payload);

    String resultResponse;
    bool sent = cloudRequest(
      "POST", "/device/result", payload, resultResponse
    );

    if (sent) {
      Serial.println("COMMAND RESULT SENT TO CLOUD");
      Serial.println(replyMessage);
    } else {
      // ให้ลองรับและประมวลผลคำสั่งเดิมอีกครั้งเมื่อส่งผลไม่สำเร็จ
      lastProcessedCommandId = "";
      Serial.println("COMMAND RESULT FAILED");
    }
  }
}

// ==================================================
// SEND SYSTEM REPORTS THROUGH CLOUD
// ==================================================

void sendReport() {
  if (WiFi.status() != WL_CONNECTED) return;

  // ส่งเฉพาะเหตุการณ์: เริ่มเปิดรีเลย์ หรือผลตรวจ Flow Sensor
  // ไม่ส่งเพียงเพราะค่าความชื้นเปลี่ยน และไม่มีรายงานตามเวลา
  bool hasPendingFlowEvent = pendingFlowEvent.length() > 0;
  bool hasPendingRelayEvent = pendingRelayStart;

  if (!hasPendingFlowEvent && !hasPendingRelayEvent) return;

  // เว้นระยะระหว่างการลองส่ง เพื่อไม่ยิงคำขอถี่เกินไป
  if (millis() - lastReportAttemptTime < reportRetryInterval) return;
  lastReportAttemptTime = millis();

  // หากมีเหตุการณ์ Flow ค้างอยู่ ให้ส่งก่อน แล้วค่อยส่งเหตุการณ์รีเลย์
  String event = hasPendingFlowEvent ? pendingFlowEvent : "RELAY_ON";

  uint32_t total1, total2;
  getFlowTotals(total1, total2);

  String message = "รายงานระบบรดน้ำ\n";
  message += "เหตุการณ์: ";

  if (event == "RELAY_ON") {
    message += "เริ่มทำงาน: ระบบสั่งเปิดรีเลย์ทั้ง 2 ตัว";
  } else if (event == "SUCCESS") {
    message += "ตรวจพบการไหลของน้ำทั้ง 2 เซนเซอร์";
  } else if (event == "FLOW_ERROR") {
    message += "ผิดปกติ: ตรวจไม่พบการไหลหรือการไหลหยุด";
  } else {
    message += "สถานะระบบ";
  }

  message += "\nความชื้นดิน: " + String(PercentageHumidity) + "%";
  message += "\nค่าเซนเซอร์: " + String(rawSensorValue);
  message += "\nวาล์ว 1: " + String(relay1State ? "เปิด" : "ปิด");
  message += "\nวาล์ว 2: " + String(relay2State ? "เปิด" : "ปิด");
  message += "\nโหมด: " + mode;
  message += "\nFlow 1: " + String(flow1Rate, 2) + " L/min";
  message += "\nFlow 2: " + String(flow2Rate, 2) + " L/min";
  message += "\nปริมาณ Flow 1: " + String(flow1Liter, 3) + " L";
  message += "\nปริมาณ Flow 2: " + String(flow2Liter, 3) + " L";
  message += "\nPulse 1: " + String(total1);
  message += "\nPulse 2: " + String(total2);

  Serial.print("Sending event to Cloud: ");
  Serial.println(event);

  if (sendCloudReport(message)) {
    Serial.println("CLOUD REPORT SUCCESS");

    // ล้างเฉพาะเหตุการณ์ที่ส่งสำเร็จแล้ว
    if (event == "RELAY_ON") {
      pendingRelayStart = false;
    }

    if (pendingFlowEvent == event) {
      pendingFlowEvent = "";
    }
  } else {
    Serial.println("CLOUD REPORT FAILED - WILL RETRY");
    // ไม่ล้าง pending event เพื่อให้ลองส่งใหม่ในรอบถัดไป
  }
}

// ==================================================
// WIFI
// ==================================================

void connectWiFi() {
  Serial.println("Connecting WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startTime = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - startTime < 20000
  ) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi Connected");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi Connection Failed");
  }
}

// ==================================================
// SETUP
// ==================================================

void setup() {
  Serial.begin(115200);

  pinMode(Relay1, OUTPUT);
  pinMode(Relay2, OUTPUT);
  digitalWrite(Relay1, RELAY_OFF);
  digitalWrite(Relay2, RELAY_OFF);

  relay1State = false;
  relay2State = false;

  pinMode(FLOW1_PIN, INPUT);
  pinMode(FLOW2_PIN, INPUT);

  attachInterrupt(
    digitalPinToInterrupt(FLOW1_PIN),
    flow1ISR,
    FALLING
  );

  attachInterrupt(
    digitalPinToInterrupt(FLOW2_PIN),
    flow2ISR,
    FALLING
  );

  analogReadResolution(12);
  analogSetPinAttenuation(sensorPin, ADC_11db);

  connectWiFi();

  readSoilMoisture();
  automaticControl();

  Serial.println("==========================");
  Serial.println("SYSTEM READY");
  Serial.println("FLOW SENSOR 1 READY");
  Serial.println("FLOW SENSOR 2 READY");
  Serial.println("PYTHON CLOUD WEBHOOK READY");
  Serial.println("==========================");
}

// ==================================================
// MAIN LOOP
// ==================================================

void loop() {
  if (millis() - lastReadTime >= readInterval) {
    lastReadTime = millis();
    readSoilMoisture();
    automaticControl();
  }

  checkFlowSensors();

  if (
    WiFi.status() != WL_CONNECTED &&
    millis() - lastWiFiAttempt >= wifiRetryInterval
  ) {
    lastWiFiAttempt = millis();
    Serial.println("Reconnecting WiFi...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  if (
    WiFi.status() == WL_CONNECTED &&
    millis() - lastCloudPollTime >= cloudPollInterval
  ) {
    lastCloudPollTime = millis();
    checkCloudCommand();
  }

  sendReport();

  delay(100);
}
