#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// ==============================
// CONFIGURATION
// ==============================
const int in1 = 18;
const int in2 = 19;
const int in3 = 17;
const int in4 = 16;
const int irSensorPin = 23;
const int buzzerPin = 2;
const int ledPin = 5;

const long STEPS_PER_REV = 4096;
const long STEPS_45_DEG = STEPS_PER_REV / 8;
const int stepDelayMs = 2;

// WiFi credentials
const char* ssid = "sudo -rm -rf /*";
const char* password = "FJTe5aWqz9hvvg2y";

// Firebase REST
#define FIREBASE_PROJECT_ID "test-6c45a"
#define FIREBASE_API_KEY "AIzaSyDZMtTK6_lma8855D81yGhkeMvzc27Aut4"   
#define FIREBASE_COLLECTION "dispenser"
String FIRESTORE_URL = "https://firestore.googleapis.com/v1/projects/" FIREBASE_PROJECT_ID "/databases/(default)/documents/" FIREBASE_COLLECTION "/";

// NTP settings
const char* ntpServer = "time.google.com";
const long gmtOffset_sec = 19800;
const int daylightOffset_sec = 0;

// ==============================
// OBJECTS & GLOBALS
// ==============================
LiquidCrystal_I2C lcd(0x27, 16, 2);
WebServer server(80);

struct Schedule {
  int morningHour = 8;
  int morningMinute = 0;
  int eveningHour = 18;
  int eveningMinute = 0;
  int nightHour = 22;
  int nightMinute = 0;
} schedule;

bool dispenseDoneMorning = false;
bool dispenseDoneEvening = false;
bool dispenseDoneNight = false;
bool ntpReady = false;

String lastDispenseTime = "None";
String lastPickupTime = "None";

// ==============================
// STEPPER CONTROL
// ==============================
const int seq[8][4] = {
  {1,0,0,0}, {1,1,0,0}, {0,1,0,0}, {0,1,1,0},
  {0,0,1,0}, {0,0,1,1}, {0,0,0,1}, {1,0,0,1}
};

void stepMotor(int idx) {
  digitalWrite(in1, seq[idx][0]);
  digitalWrite(in2, seq[idx][1]);
  digitalWrite(in3, seq[idx][2]);
  digitalWrite(in4, seq[idx][3]);
}

void stepSteps(long steps, int direction) {
  static int stepIndex = 0;
  for (long s = 0; s < steps; s++) {
    stepIndex = (stepIndex + (direction == 1 ? 1 : -1)) % 8;
    if (stepIndex < 0) stepIndex += 8;
    stepMotor(stepIndex);
    delay(stepDelayMs);
  }
  digitalWrite(in1, LOW);
  digitalWrite(in2, LOW);
  digitalWrite(in3, LOW);
  digitalWrite(in4, LOW);
}

// ==============================
// TIME UTILS
// ==============================
String getTimeString() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char buffer[20];
    strftime(buffer, sizeof(buffer), "%H:%M:%S", &timeinfo);
    return String(buffer);
  }
  return "00:00:00";
}

// ==============================
// FIREBASE FUNCTIONS
// ==============================
void uploadToFirestore() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = FIRESTORE_URL + "deviceStatus?key=" + FIREBASE_API_KEY;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(512);
  JsonObject fields = doc.createNestedObject("fields");

  fields["status"]["stringValue"] = "Online";
  fields["timestamp"]["stringValue"] = getTimeString();
  fields["lastDispense"]["stringValue"] = lastDispenseTime;
  fields["lastPickup"]["stringValue"] = lastPickupTime;

  String jsonBody;
  serializeJson(doc, jsonBody);

  int httpCode = http.PATCH(jsonBody);
  Serial.printf("[Firestore] Upload code: %d\n", httpCode);
  http.end();
}

void fetchScheduleFromFirestore() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = FIRESTORE_URL + "schedule?key=" + FIREBASE_API_KEY;
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, payload);

    JsonObject fields = doc["fields"];
    schedule.morningHour = fields["morningHour"]["integerValue"].as<int>();
    schedule.morningMinute = fields["morningMinute"]["integerValue"].as<int>();
    schedule.eveningHour = fields["eveningHour"]["integerValue"].as<int>();
    schedule.eveningMinute = fields["eveningMinute"]["integerValue"].as<int>();
    schedule.nightHour = fields["nightHour"]["integerValue"].as<int>();
    schedule.nightMinute = fields["nightMinute"]["integerValue"].as<int>();

    Serial.println("[Firestore] Schedule updated successfully");
  } else {
    Serial.printf("[Firestore] Failed to fetch schedule: %d\n", httpCode);
  }

  http.end();
}

// ==============================
// DISPENSE LOGIC + IR CHECK
// ==============================
void waitForTabletPickup() {
  lcd.clear();
  lcd.print("Take Tablet!");
  Serial.println("Waiting for tablet pickup...");

  unsigned long start = millis();
  bool taken = false;

  while (millis() - start < 120000) {
    int irValue = digitalRead(irSensorPin);
    lcd.setCursor(0, 1);
    lcd.print("IR:");
    lcd.print(irValue);
    lcd.print("   ");
    delay(200);

    if (irValue == 1) {
      digitalWrite(buzzerPin, HIGH);
      taken = true;
      lastPickupTime = getTimeString();
      Serial.println("Tablet taken at: " + lastPickupTime);
      lcd.clear();
      lcd.print("Taken @ ");
      lcd.print(lastPickupTime);
      delay(2000);
      break;
    }
  }

  if (!taken) {
    digitalWrite(buzzerPin, HIGH);
    lcd.clear();
    lcd.print("Not Taken!");
    Serial.println("Tablet not taken in 2 min");
    delay(2000);
  }
}

void dispense(int rotations) {
  lcd.clear();
  lcd.print("Dispensing...");
  Serial.println("Dispensing...");

  digitalWrite(buzzerPin, LOW);

  for (int i = 0; i < rotations; i++) {
    stepSteps(STEPS_45_DEG, 1);
    delay(300);
  }

  lastDispenseTime = getTimeString();
  Serial.println("Dispensed at: " + lastDispenseTime);
  lcd.clear();
  lcd.print("Done!");
  delay(1000);

  waitForTabletPickup();
  uploadToFirestore();
}

// ==============================
// NTP SYNC
// ==============================
bool syncNTP() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 5000)) {
    Serial.println("NTP sync failed");
    return false;
  }
  Serial.println("NTP time synced");
  return true;
}

void getCurrentTime(int &hour, int &minute, int &second) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    hour = timeinfo.tm_hour;
    minute = timeinfo.tm_min;
    second = timeinfo.tm_sec;
  } else hour = minute = second = 0;
}

// ==============================
// WEB SERVER HANDLERS (JSON)
// ==============================
void sendJSON(int code, String json) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(code, "application/json", json);
}

void handleRoot() { sendJSON(200, "{\"status\":\"ESP32 Online\"}"); }

void handleStatus() {
  String json = String("{") +
    "\"morning\":\"" + String(schedule.morningHour) + ":" + String(schedule.morningMinute) + "\"," +
    "\"evening\":\"" + String(schedule.eveningHour) + ":" + String(schedule.eveningMinute) + "\"," +
    "\"night\":\"" + String(schedule.nightHour) + ":" + String(schedule.nightMinute) + "\"," +
    "\"lastDispense\":\"" + lastDispenseTime + "\"," +
    "\"lastPickup\":\"" + lastPickupTime + "\"" +
  "}";
  sendJSON(200, json);
}

// ==============================
// SETUP
// ==============================
void setup() {
  Serial.begin(115200);
  lcd.init();
  lcd.backlight();
  lcd.print("Smart Dispenser");

  pinMode(in1, OUTPUT);
  pinMode(in2, OUTPUT);
  pinMode(in3, OUTPUT);
  pinMode(in4, OUTPUT);
  pinMode(irSensorPin, INPUT_PULLUP);
  pinMode(buzzerPin, OUTPUT);
  pinMode(ledPin, OUTPUT);

  digitalWrite(buzzerPin, HIGH);
  digitalWrite(ledPin, HIGH);

  WiFi.begin(ssid, password);
  lcd.setCursor(0, 1);
  lcd.print("WiFi...");
  Serial.print("Connecting to WiFi");

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 30) {
    delay(500);
    Serial.print(".");
    retry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    lcd.clear();
    lcd.print("WiFi OK");
    Serial.println("\nConnected to WiFi");
    Serial.println(WiFi.localIP());
    delay(2000);
    ntpReady = syncNTP();
    fetchScheduleFromFirestore();  // initial pull
  } else {
    lcd.print("WiFi Fail");
    Serial.println("\nWiFi failed.");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.begin();
  Serial.println("HTTP server started");
}

// ==============================
// MAIN LOOP
// ==============================
unsigned long lastFirestoreUpload = 0;
unsigned long lastScheduleFetch = 0;

void loop() {
  server.handleClient();

  int hour, minute, second;
  getCurrentTime(hour, minute, second);

  lcd.setCursor(0, 0);
  char buf[9];
  sprintf(buf, "%02d:%02d:%02d", hour, minute, second);
  lcd.print(buf);
  lcd.setCursor(0, 1);
  lcd.print("IR:");
  lcd.print(digitalRead(irSensorPin));

  if (hour == schedule.morningHour && minute == schedule.morningMinute && !dispenseDoneMorning) {
    dispense(3);
    dispenseDoneMorning = true;
  }
  if (hour == schedule.eveningHour && minute == schedule.eveningMinute && !dispenseDoneEvening) {
    dispense(3);
    dispenseDoneEvening = true;
  }
  if (hour == schedule.nightHour && minute == schedule.nightMinute && !dispenseDoneNight) {
    dispense(2);
    dispenseDoneNight = true;
  }
  if (hour == 0 && minute == 1) dispenseDoneMorning = dispenseDoneEvening = dispenseDoneNight = false;

  unsigned long now = millis();
  if (now - lastFirestoreUpload > 60000) { uploadToFirestore(); lastFirestoreUpload = now; }
  if (now - lastScheduleFetch > 300000) { fetchScheduleFromFirestore(); lastScheduleFetch = now; }

  delay(500);
}
