#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>

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
const long STEPS_PER_SLOT = STEPS_PER_REV / 12;
const int stepDelayMs = 2;

// WiFi credentials
const char* ssid = "sudo rm -rf /*";
const char* password = "FJTe5aWqz9hvvg2y";

// Firebase REST
#define FIREBASE_PROJECT_ID "e-c-s-project-d7pu2p"
#define FIREBASE_API_KEY "AIzaSyDpOte-YKJeM366Xqrm500z8KO8LQ0vtyI"
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

ThreeWire myWire(13, 15, 12); // DAT, CLK, RST
RtcDS1302<ThreeWire> rtc(myWire);
bool rtcReady = false;
bool ntpReady = false;

struct Schedule {
  int morningHour;
  int morningMinute;
  int eveningHour;
  int eveningMinute;
  int nightHour;
  int nightMinute;
} schedule;

bool dispenseDoneMorning = false;
bool dispenseDoneEvening = false;
bool dispenseDoneNight = false;

int dayCounter = 1;
int slotCounter = 0;
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
// TIME UTILS (RTC + NTP)
// ==============================
bool syncNTP() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 5000)) {
    Serial.println("[TIME] NTP sync failed.");
    ntpReady = false;
    return false;
  }

  RtcDateTime now(
    timeinfo.tm_year + 1900,
    timeinfo.tm_mon + 1,
    timeinfo.tm_mday,
    timeinfo.tm_hour,
    timeinfo.tm_min,
    timeinfo.tm_sec
  );
  rtc.SetDateTime(now);
  rtcReady = true;
  ntpReady = true;

  Serial.println("[TIME] NTP synced & RTC updated.");
  return true;
}

void getCurrentTime(int &hour, int &minute, int &second) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    hour = timeinfo.tm_hour;
    minute = timeinfo.tm_min;
    second = timeinfo.tm_sec;
  } else if (rtcReady) {
    RtcDateTime now = rtc.GetDateTime();
    hour = now.Hour();
    minute = now.Minute();
    second = now.Second();
  } else {
    hour = minute = second = 0;
  }
}

String getTimeString() {
  int h, m, s;
  getCurrentTime(h, m, s);
  char buffer[9];
  sprintf(buffer, "%02d:%02d:%02d", h, m, s);
  return String(buffer);
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
  fields["dayCounter"]["integerValue"] = dayCounter;
  fields["slotCounter"]["integerValue"] = slotCounter;

  String jsonBody;
  serializeJson(doc, jsonBody);

  int httpCode = http.PATCH(jsonBody);
  Serial.printf("[Firestore] Upload code: %d\n", httpCode);
  http.end();
}

// ==============================
// FIRESTORE FETCH (NEW)
// ==============================
void fetchScheduleFromFirestore() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Firestore] WiFi not connected, skipping fetch.");
    return;
  }

  String userID = "OIKe2i8z6eSlloQe7ciwh1EqAto2";

  HTTPClient http;
  String url = "https://firestore.googleapis.com/v1/projects/" FIREBASE_PROJECT_ID
               "/databases/(default)/documents:runQuery?key=" FIREBASE_API_KEY;

  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  // Firestore structuredQuery JSON
  String query = "{\
    \"structuredQuery\": {\
      \"from\": [{\"collectionId\": \"medications\"}],\
      \"where\": {\
        \"fieldFilter\": {\
          \"field\": {\"fieldPath\": \"userID\"},\
          \"op\": \"EQUAL\",\
          \"value\": {\"stringValue\": \"" + userID + "\"}\
        }\
      }\
    }\
  }";

  Serial.println("[Firestore] Fetching schedule for user: " + userID);
  int httpCode = http.POST(query);

  if (httpCode == 200) {
    String payload = http.getString();
    Serial.println("[Firestore] Response received.");

    DynamicJsonDocument doc(16384);
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
      Serial.print("[Firestore] JSON parse failed: ");
      Serial.println(error.f_str());
      http.end();
      return;
    }

    bool found = false;
    for (JsonObject result : doc.as<JsonArray>()) {
      if (!result.containsKey("document")) continue;
      JsonObject fields = result["document"]["fields"];

      String session = fields["session"]["stringValue"].as<String>();
      int hour = fields["hour"]["integerValue"].as<int>();
      int minute = fields["minute"]["integerValue"].as<int>();

      if (session == "Morning") {
        schedule.morningHour = hour;
        schedule.morningMinute = minute;
      } else if (session == "Evening") {
        schedule.eveningHour = hour;
        schedule.eveningMinute = minute;
      } else if (session == "Night") {
        schedule.nightHour = hour;
        schedule.nightMinute = minute;
      }

      found = true;
      Serial.printf("[Firestore] %s -> %02d:%02d\n", session.c_str(), hour, minute);
    }

    if (found) {
      lcd.clear();
      lcd.print("Schedule Updated!");
      delay(2000);
    } else {
      lcd.clear();
      lcd.print("No schedule found");
      delay(2000);
    }
    lcd.clear();

  } else {
    Serial.printf("[Firestore] Fetch failed, code: %d\n", httpCode);
  }

  http.end();
}

// ==============================
// DISPENSE LOGIC
// ==============================
void waitForTabletPickup() {
  lcd.clear();
  lcd.print("Take Tablet!");
  Serial.println("Waiting for tablet pickup...");

  unsigned long start = millis();
  bool taken = false;

  while (millis() - start < 120000) {
    int irValue = digitalRead(irSensorPin);
    delay(200);
    if (irValue == 1) {
      digitalWrite(buzzerPin, HIGH);
      taken = true;
      lastPickupTime = getTimeString();
      Serial.println("Tablet taken at: " + lastPickupTime);
      lcd.clear();
      lcd.print("Taken @ ");
      lcd.print(lastPickupTime.substring(0, 5));
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

  lcd.clear();
}

void dispense(int slots) {
  lcd.clear();
  lcd.print("Dispensing...");
  Serial.println("Dispensing...");

  digitalWrite(buzzerPin, LOW);

  for (int i = 0; i < slots; i++) {
    stepSteps(STEPS_PER_SLOT, 1);
    delay(1000);
    slotCounter++;
  }

  lastDispenseTime = getTimeString();
  Serial.println("Dispensed at: " + lastDispenseTime);
  lcd.clear();
  lcd.print("Done!");
  delay(1000);
  lcd.clear();

  waitForTabletPickup();
  uploadToFirestore();
}

// ==============================
// WEB SERVER
// ==============================
void sendJSON(int code, String json) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(code, "application/json", json);
}

void handleRoot() { sendJSON(200, "{\"status\":\"ESP32 Online\"}"); }

void handleStatus() {
  String json = String("{") +
    "\"dayCounter\":\"" + String(dayCounter) + "\"," +
    "\"slotCounter\":\"" + String(slotCounter) + "\"," +
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

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  lcd.setCursor(0, 1);
  lcd.print("WiFi...");
  Serial.print("Connecting to WiFi");

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 30) {
    delay(500);
    Serial.print(".");
    retry++;
  }

  rtc.Begin();

  if (WiFi.status() == WL_CONNECTED) {
    lcd.clear();
    lcd.print("WiFi OK");
    Serial.println("\nConnected to WiFi");
    Serial.println(WiFi.localIP());
    delay(2000);
    fetchScheduleFromFirestore(); // 🔥 NEW
    ntpReady = syncNTP();
  } else {
    lcd.print("WiFi Fail");
    Serial.println("\nWiFi failed.");
    if (rtc.IsDateTimeValid()) {
      rtcReady = true;
      Serial.println("[RTC] Fallback active.");
    } else {
      Serial.println("[RTC] No valid time source.");
    }
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.begin();
  Serial.println("HTTP server started");
}

// ==============================
// LOOP
// ==============================
unsigned long lastFirestoreUpload = 0;
unsigned long lastScheduleFetch = 0;
const unsigned long SCHEDULE_FETCH_INTERVAL = 600000; // 10 minutes (in ms)


void loop() {
  server.handleClient();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Disconnected, retrying...");
    WiFi.reconnect();
    delay(5000);
  }

  int hour, minute, second;
  getCurrentTime(hour, minute, second);

  lcd.setCursor(0, 0);
  char buf[17];
  sprintf(buf, "Time:%02d:%02d:%02d", hour, minute, second);
  lcd.print(buf);

  lcd.setCursor(0, 1);
  lcd.print("Slot:");
  lcd.print(slotCounter);
  lcd.print(" IR:");
  lcd.print(digitalRead(irSensorPin));

  if (dayCounter > 4 || slotCounter >= 12) {
    lcd.clear();
    lcd.print("REFILL NEEDED!");
    Serial.println("All 12 slots used - refill required!");
    delay(3000);
    return;
  }

  if (hour == schedule.morningHour && minute == schedule.morningMinute && !dispenseDoneMorning) {
    dispense(1);
    dispenseDoneMorning = true;
  }
  if (hour == schedule.eveningHour && minute == schedule.eveningMinute && !dispenseDoneEvening) {
    dispense(1);
    dispenseDoneEvening = true;
  }
  if (hour == schedule.nightHour && minute == schedule.nightMinute && !dispenseDoneNight) {
    dispense(1);
    dispenseDoneNight = true;
  }

  if (hour == 0 && minute == 1 && second < 5) {
    dispenseDoneMorning = dispenseDoneEvening = dispenseDoneNight = false;
    dayCounter++;
    Serial.printf("New day: %d\n", dayCounter);
  }

  unsigned long now = millis();
  // Push data every 1 min
  if (now - lastFirestoreUpload > 60000) {
    uploadToFirestore();
    lastFirestoreUpload = now;
  }

  // Fetch schedule every 10 min
  if (now - lastScheduleFetch > SCHEDULE_FETCH_INTERVAL) {
    Serial.println("[Scheduler] Refreshing schedule from Firestore...");
    fetchScheduleFromFirestore();
    lastScheduleFetch = now;
  }

  delay(500);
}
