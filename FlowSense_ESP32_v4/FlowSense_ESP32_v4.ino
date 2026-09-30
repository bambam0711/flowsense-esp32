/*************
  FlowSense - Firebase + Telegram
  ESP32 v4 — Hysteresis Edition

  Sensor pins:
    RAIN_SENSOR  → GPIO 32 (ANALOG)
    WATER_SENSOR → GPIO 13 (DIGITAL - XKC)
    ALERT_BUTTON → GPIO 27

  LED pins:
    RAIN_LED_GREEN  → GPIO 15
    RAIN_LED_RED    → GPIO 4
    WATER_LED_GREEN → GPIO 19
    WATER_LED_RED   → GPIO 17
    ALERT_LED       → GPIO 16

  RAIN SENSOR:
    - Direct analogRead, no delay
    - Hysteresis: dua threshold untuk elak Telegram spam
      - Bawah 2800 = ada air
      - Atas  3200 = tiada air
      - Antara 2800-3200 = kekal status lama

  BROKEN FLAG LOGIC:
    - User tekan button sekejap → brokeFlag = true → status kekal "broken"
    - Status kekal broken walaupun button dilepas / sensor detect air / ESP32 reboot
    - Hanya admin boleh clear dari website → broken_confirmed = false di Firebase
    - ESP32 check Firebase setiap 5 saat untuk detect admin clear
*************/

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>

// Local credentials are not committed. Copy secrets.example.h to secrets.h.
#include "secrets.h"

// ===== TOILET INFO =====
#define TOILET_CODE  "A1T01"
#define ESP_ID       "ESP32-FlowSense"
#define BLOCK        "A"
#define FLOOR_NUM    1
#define TOILET_NUM   1

// ===== PIN =====
#define RAIN_SENSOR     32   // ADC1 — analog read
#define RAIN_LED_GREEN  15
#define RAIN_LED_RED     4
#define WATER_SENSOR    13   // XKC — digital read
#define WATER_LED_GREEN 19
#define WATER_LED_RED   17
#define ALERT_BUTTON    27
#define ALERT_LED       16

// ===== HYSTERESIS THRESHOLD =====
// Turun bawah 2800 → ada air
// Naik atas  3200 → tiada air
// Antara 2800-3200 → kekal status lama (elak spam)
#define RAIN_THRESHOLD_WET  2800
#define RAIN_THRESHOLD_DRY  3200

// ===== GLOBALS =====
WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);

int rainStatus  = -1;  // -1=unknown, 0=dry, 1=wet
int waterStatus = -1;

// ===== BROKEN FLAG =====
bool brokeFlag       = false;
bool lastButtonState = false;
bool brokeReported   = false;

// ===== TIMING =====
String lastFirebaseStatus = "";
unsigned long lastSendTime      = 0;
unsigned long lastFirebaseCheck = 0;
#define SEND_INTERVAL           1000
#define FIREBASE_CHECK_INTERVAL 5000

// ===== READ RAIN SENSOR — direct, no delay =====
int readRainSensor() {
  return analogRead(RAIN_SENSOR);
}

// ===== RAIN HYSTERESIS =====
// Elak flip-flop / spam bila nilai kat sempadan threshold
bool getRainHasWater(int rawValue) {
  if (rawValue < RAIN_THRESHOLD_WET)      return true;   // confirm ada air
  else if (rawValue > RAIN_THRESHOLD_DRY) return false;  // confirm tiada air
  else                                     return (rainStatus == 1); // kekal status lama
}

// ===== DETERMINE STATUS =====
String determineStatus(bool rainHasWater, bool waterHasWater) {
  if (brokeFlag)                      return "broken";
  if (rainHasWater && waterHasWater)  return "available";
  return "no-water";
}

// ===== SEND TO FIREBASE =====
void sendToFirebase(bool rain, bool water, String status) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;

  String url = String(FIREBASE_HOST) + "/toilets/" + TOILET_CODE + ".json";
  if (strlen(FIREBASE_AUTH) > 0) {
    url += "?auth=" + String(FIREBASE_AUTH);
  }

  StaticJsonDocument<300> doc;
  doc["toilet_code"]      = TOILET_CODE;
  doc["block"]            = BLOCK;
  doc["floor"]            = FLOOR_NUM;
  doc["toilet_num"]       = TOILET_NUM;
  doc["raindrop_sensor"]  = rain;
  doc["xkc_sensor"]       = water;
  doc["button_pressed"]   = brokeFlag;
  doc["status"]           = status;
  doc["broken_confirmed"] = brokeFlag;
  doc["esp_id"]           = ESP_ID;
  doc["last_update"]      = millis();

  String payload;
  serializeJson(doc, payload);

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int code = http.PATCH(payload);

  if (code == 200 || code == 204) {
    Serial.println("✅ Firebase updated! Status: " + status);
  } else {
    Serial.println("❌ Firebase FAILED. HTTP: " + String(code));
    Serial.println(http.getString());
  }

  http.end();
}

// ===== CHECK FIREBASE — admin clear broken =====
void checkFirebaseForAdminClear() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!brokeFlag) return;

  HTTPClient http;

  String url = String(FIREBASE_HOST) + "/toilets/" + TOILET_CODE + "/broken_confirmed.json";
  if (strlen(FIREBASE_AUTH) > 0) {
    url += "?auth=" + String(FIREBASE_AUTH);
  }

  http.begin(url);
  int code = http.GET();

  if (code == 200) {
    String response = http.getString();
    response.trim();
    Serial.println("🔍 broken_confirmed = " + response);

    if (response == "false") {
      Serial.println("✅ Admin cleared broken flag!");
      brokeFlag     = false;
      brokeReported = false;

      digitalWrite(ALERT_LED, LOW);

      bot.sendMessage(CHAT_ID,
        "✅ Toilet " + String(TOILET_CODE) + " telah ditandakan FIXED oleh admin.\n"
        "🚽 Sistem kembali monitor sensor."
      );

      int rainRaw      = readRainSensor();
      bool rain        = getRainHasWater(rainRaw);
      bool water       = (digitalRead(WATER_SENSOR) == LOW);
      String newStatus = determineStatus(rain, water);
      sendToFirebase(rain, water, newStatus);
      lastFirebaseStatus = newStatus;
    }
  } else {
    Serial.println("⚠️ Firebase GET failed. HTTP: " + String(code));
  }

  http.end();
}

// ===== CHECK SYSTEM =====
void checkSystem() {

  // ===== RAIN SENSOR — direct read + hysteresis =====
  int rainRaw       = readRainSensor();
  bool rainHasWater = getRainHasWater(rainRaw);

  Serial.println("🌧 RAIN RAW: " + String(rainRaw) +
                 " | hasWater: " + String(rainHasWater ? "YES ✅" : "NO ❌"));

  // ===== XKC SENSOR — digital =====
  bool waterHasWater = (digitalRead(WATER_SENSOR) == LOW);

  // ===== BUTTON =====
  bool buttonNow = (digitalRead(ALERT_BUTTON) == LOW);

  // ===== DETECT BUTTON EDGE =====
  if (buttonNow && !lastButtonState) {
    if (!brokeFlag) {
      brokeFlag     = true;
      brokeReported = false;
      Serial.println("\n🚨 BUTTON PRESSED — Setting brokeFlag = true");
    } else {
      Serial.println("\n⚠️ Button pressed again — already broken, ignoring");
    }
  }
  lastButtonState = buttonNow;

  // ===== ALERT LED =====
  digitalWrite(ALERT_LED, brokeFlag ? HIGH : LOW);

  // ===== TELEGRAM — broken alert sekali je =====
  if (brokeFlag && !brokeReported) {
    bot.sendMessage(CHAT_ID,
      "⚠️ ALERT: Toilet Rosak Dilaporkan!\n"
      "🚽 Toilet: " + String(TOILET_CODE) + "\n"
      "🏢 Blok " + String(BLOCK) + ", Tingkat " + String(FLOOR_NUM) + "\n"
      "📌 Sila semak dan mark as fixed dari website."
    );
    brokeReported = true;
    Serial.println("📨 Telegram alert sent.");
  }

  // ===== RAIN LED + TELEGRAM — hantar sekali je bila status berubah =====
  if (rainHasWater) {
    digitalWrite(RAIN_LED_GREEN, HIGH);
    digitalWrite(RAIN_LED_RED,   LOW);
    if (rainStatus != 1) {
      bot.sendMessage(CHAT_ID, "🌧 Rain Sensor: WATER DETECTED (Toilet: " + String(TOILET_CODE) + ")");
      rainStatus = 1;
    }
  } else {
    digitalWrite(RAIN_LED_GREEN, LOW);
    digitalWrite(RAIN_LED_RED,   HIGH);
    if (rainStatus != 0) {
      bot.sendMessage(CHAT_ID, "🚨 Rain Sensor: NO WATER (Toilet: " + String(TOILET_CODE) + ")");
      rainStatus = 0;
    }
  }

  // ===== WATER LED + TELEGRAM (XKC) =====
  if (waterHasWater) {
    digitalWrite(WATER_LED_GREEN, HIGH);
    digitalWrite(WATER_LED_RED,   LOW);
    if (waterStatus != 1) {
      bot.sendMessage(CHAT_ID, "💧 Water Sensor: WATER DETECTED (Toilet: " + String(TOILET_CODE) + ")");
      waterStatus = 1;
    }
  } else {
    digitalWrite(WATER_LED_GREEN, LOW);
    digitalWrite(WATER_LED_RED,   HIGH);
    if (waterStatus != 0) {
      bot.sendMessage(CHAT_ID, "🚨 Water Sensor: NO WATER (Toilet: " + String(TOILET_CODE) + ")");
      waterStatus = 0;
    }
  }

  // ===== STATUS & FIREBASE =====
  String status = determineStatus(rainHasWater, waterHasWater);

  if (status != lastFirebaseStatus) {
    Serial.println("\n🔄 STATUS CHANGED: " + lastFirebaseStatus + " → " + status);
    sendToFirebase(rainHasWater, waterHasWater, status);
    lastFirebaseStatus = status;
    lastSendTime = millis();
  }
  else if (millis() - lastSendTime >= SEND_INTERVAL) {
    sendToFirebase(rainHasWater, waterHasWater, status);
    lastSendTime = millis();
  }
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n========================================");
  Serial.println("  FlowSense - Firebase + Telegram v4");
  Serial.println("  Hysteresis Edition — No Delay");
  Serial.println("========================================");

  pinMode(RAIN_LED_GREEN,  OUTPUT);
  pinMode(RAIN_LED_RED,    OUTPUT);
  pinMode(WATER_SENSOR,    INPUT);
  pinMode(WATER_LED_GREEN, OUTPUT);
  pinMode(WATER_LED_RED,   OUTPUT);
  pinMode(ALERT_BUTTON,    INPUT_PULLUP);
  pinMode(ALERT_LED,       OUTPUT);
  // RAIN_SENSOR (GPIO32) — no pinMode needed

  // Connect WiFi
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, pass);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi Connected!");
    Serial.println("IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n❌ WiFi GAGAL!");
  }

  client.setInsecure();

  // ===== CHECK FIREBASE ON BOOT — recover brokeFlag =====
  Serial.println("🔍 Checking Firebase for previous broken state...");
  HTTPClient http;
  String url = String(FIREBASE_HOST) + "/toilets/" + TOILET_CODE + "/broken_confirmed.json";
  if (strlen(FIREBASE_AUTH) > 0) url += "?auth=" + String(FIREBASE_AUTH);
  http.begin(url);
  int code = http.GET();
  if (code == 200) {
    String response = http.getString();
    response.trim();
    Serial.println("broken_confirmed on boot = " + response);
    if (response == "true") {
      brokeFlag     = true;
      brokeReported = true;
      digitalWrite(ALERT_LED, HIGH);
      Serial.println("⚠️ Recovered broken state from Firebase!");
    }
  }
  http.end();

  // Boot test rain sensor
  Serial.println("\n📋 BOOT TEST Rain Sensor:");
  int bootRaw = readRainSensor();
  Serial.println("   RAW: " + String(bootRaw) +
                 (bootRaw < RAIN_THRESHOLD_WET ? " → ADA AIR ✅" :
                  bootRaw > RAIN_THRESHOLD_DRY ? " → TIADA AIR 🔴" :
                  " → ZONE TENGAH (kekal status lama)"));

  bot.sendMessage(CHAT_ID,
    "✅ FlowSense Online!\n"
    "📡 Firebase + Telegram Active\n"
    "🌧 Rain Sensor: ANALOG | Wet<" + String(RAIN_THRESHOLD_WET) +
    " | Dry>" + String(RAIN_THRESHOLD_DRY) + "\n"
    "🚽 Monitoring: " + String(TOILET_CODE) + "\n" +
    (brokeFlag ? "⚠️ Status: MASIH BROKEN (belum di-fixed)" : "✅ Status: Normal")
  );

  lastSendTime = 0;
}

// ===== LOOP =====
void loop() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck >= 1000) {
    checkSystem();
    lastCheck = millis();
  }

  if (millis() - lastFirebaseCheck >= FIREBASE_CHECK_INTERVAL) {
    checkFirebaseForAdminClear();
    lastFirebaseCheck = millis();
  }

  // Reconnect WiFi kalau putus
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi putus, reconnecting...");
    WiFi.begin(ssid, pass);
    delay(3000);
  }
}
