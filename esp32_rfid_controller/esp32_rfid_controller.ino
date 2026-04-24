// =====================================================
// KuetTrack RFID Controller — ESP32 + RC522
// =====================================================
// Hardware:
// - ESP32 Dev Board
// - RC522 RFID Reader
// - 16x2 LCD with I2C backpack
// - Green LED (GPIO32), Red LED (GPIO33)
// - Active Buzzer (GPIO25)
// - Servo Motor (GPIO26)
// =====================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <ESP32Servo.h>
#include "soc/soc.h"           // brownout disable
#include "soc/rtc_cntl_reg.h"  // brownout disable

// ─── WiFi ─────────────────────────────────────────────
#define WIFI_SSID     "KJAH_201"
#define WIFI_PASSWORD "36140343"

// ─── Backend ──────────────────────────────────────────
#define API_BASE_URL  "http://192.168.0.120:5000"
#define API_TIMEOUT   5000

// ─── RC522 Pins (SPI) ─────────────────────────────────
#define SS_PIN   5    // SDA/CS → GPIO5
#define RST_PIN  14   // RST    → GPIO14
// SCK  → GPIO18  (hardware SPI)
// MOSI → GPIO23  (hardware SPI)
// MISO → GPIO19  (hardware SPI)

// ─── LCD I2C ──────────────────────────────────────────
#define LCD_ADDR  0x27
#define LCD_COLS  16
#define LCD_ROWS  2
// SDA → GPIO21
// SCL → GPIO22

// ─── Output Pins ──────────────────────────────────────
#define GREEN_LED   32
#define RED_LED     33
#define BUZZER_PIN  25
#define SERVO_PIN   13   // GPIO13 — safe PWM, avoids DAC/SPI conflict of GPIO26

// ─── Servo Positions ──────────────────────────────────
// ⚠️ Tune SERVO_UNLOCKED if door doesn't open fully (try 90,120,150,180)
#define SERVO_LOCKED    10   // slightly above 0 — avoids buzzing at hard stop
#define SERVO_UNLOCKED  120  // increase until door opens completely

// ─── Ride Config ──────────────────────────────────────
#define BIKE_ID     "BIKE-001"
#define STATION_ID  "STATION-001"

// ─── Objects ──────────────────────────────────────────
MFRC522 rfid(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
Preferences preferences;
Servo doorServo;

// ─── State ────────────────────────────────────────────
String lastRfidUid     = "";
String currentRideId   = "";
String lastAuthStatus  = "";
String currentUserName = "";
bool   rideActive      = false;
unsigned long lastReadTime = 0;
const unsigned long READ_COOLDOWN = 2000;

// ─── Forward Declarations ─────────────────────────────
void lockDoor();
void unlockDoor();
void reinitRFID();
void connectToWiFi();
void authenticateRfidCard(String rfidUid);
void startRide(String rfidUid);
void endRide(String rfidUid);
bool parseUserData(String response);
String getRfidUid();
void beep(int duration);
void displayWelcome();
void displayWiFiConnected();
void displayWiFiFailed();
void displayReady();
void displayScanning();
void displayAuthSuccess();
void displayAuthFailed(String reason);
void displayRideEnded();

// ═══════════════════════════════════════════════════════
void setup() {
  // ── Disable brownout — servo current spikes cause resets ─
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(500);  // longer delay so Serial stabilizes before any output
  Serial.println("\n\n=== KuetTrack RFID Controller Starting ===\n");

  // ── LEDs & Buzzer ───────────────────────────────────
  pinMode(GREEN_LED,  OUTPUT);
  pinMode(RED_LED,    OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(GREEN_LED,  LOW);
  digitalWrite(RED_LED,    LOW);
  digitalWrite(BUZZER_PIN, LOW);

  // ── LCD ─────────────────────────────────────────────
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  displayWelcome();

  // ── SPI + RC522 — init BEFORE servo to avoid bus conflict ─
  SPI.begin(18, 19, 23, 5);   // SCK, MISO, MOSI, SS
  rfid.PCD_Init();
  delay(100);

  byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.println("RFID Firmware: 0x" + String(version, HEX));

  if (version == 0x00 || version == 0xFF) {
    Serial.println("RC522 NOT DETECTED! Check wiring.");
  } else {
    Serial.println("RC522 initialized OK!");
  }

  rfid.PCD_WriteRegister(MFRC522::RFCfgReg, 0x3F);
  Serial.println("RFID Antenna ON");

  // ── Servo — init AFTER SPI so PWM doesn't corrupt SPI bus ─
  // Signal wire moved to GPIO13 (GPIO26 has DAC that clashes with SPI)
  pinMode(SERVO_PIN, OUTPUT);
  digitalWrite(SERVO_PIN, LOW);
  delay(200);
  doorServo.attach(SERVO_PIN, 544, 2400);
  doorServo.write(SERVO_LOCKED);
  delay(800);
  doorServo.detach();
  digitalWrite(SERVO_PIN, LOW);
  Serial.println("Door locked on boot.");

  // ── Preferences ─────────────────────────────────────
  preferences.begin("kuettrack", false);

  // ── WiFi ────────────────────────────────────────────
  connectToWiFi();

  Serial.println("\n🚀 Ready! Waiting for RFID card...\n");
  displayReady();
}

// ═══════════════════════════════════════════════════════
void loop() {
  // WiFi watchdog
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ WiFi dropped, reconnecting...");
    connectToWiFi();
  }

  // Cooldown between reads
  if (millis() - lastReadTime < READ_COOLDOWN) {
    delay(50);
    return;
  }

  if (!rfid.PICC_IsNewCardPresent()) return;

  Serial.println("🟢 Card detected! Reading...");

  if (!rfid.PICC_ReadCardSerial()) {
    Serial.println("❌ Failed to read serial — hold card still & flat");
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    // Count consecutive failures and re-init RC522 if stuck
    static int failCount = 0;
    failCount++;
    if (failCount >= 3) {
      Serial.println("🔄 RC522 re-initializing...");
      rfid.PCD_Init();
      rfid.PCD_WriteRegister(MFRC522::RFCfgReg, 0x3F);
      failCount = 0;
      delay(200);
    }
    delay(500);
    return;
  }

  // Successful read — reset fail counter
  static int failCount = 0;
  failCount = 0;

  lastReadTime = millis();

  String rfidUid = getRfidUid();
  Serial.println("📷 Card UID: " + rfidUid);

  displayScanning();
  beep(100);

  if (rfidUid == lastRfidUid && rideActive) {
    endRide(rfidUid);
  } else {
    authenticateRfidCard(rfidUid);
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

// ═══════════════════════════════════════════════════════
// Servo — instant position, no sweep, detach after move
// Detaching stops PWM which would otherwise corrupt SPI
// ═══════════════════════════════════════════════════════
void reinitRFID() {
  delay(100);
  rfid.PCD_Init();
  rfid.PCD_WriteRegister(MFRC522::RFCfgReg, 0x3F);
  delay(50);
  Serial.println("🔄 RC522 re-initialized after servo.");
}

void unlockDoor() {
  doorServo.attach(SERVO_PIN, 544, 2400);
  doorServo.write(SERVO_UNLOCKED);
  delay(800);          // longer delay = more torque time to pull door
  doorServo.detach();
  digitalWrite(SERVO_PIN, LOW);  // kill signal to stop jitter
  reinitRFID();
  Serial.println("Door UNLOCKED");
}

void lockDoor() {
  doorServo.attach(SERVO_PIN, 544, 2400);
  doorServo.write(SERVO_LOCKED);
  delay(800);
  doorServo.detach();
  digitalWrite(SERVO_PIN, LOW);
  reinitRFID();
  Serial.println("Door LOCKED");
}

// ═══════════════════════════════════════════════════════
void connectToWiFi() {
  Serial.print("🔌 Connecting to: " + String(WIFI_SSID));
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500); Serial.print("."); attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi connected! IP: " + WiFi.localIP().toString());
    displayWiFiConnected();
    delay(2000);
  } else {
    Serial.println("\n❌ WiFi failed!");
    displayWiFiFailed();
    delay(2000);
  }
}

// ═══════════════════════════════════════════════════════
String getRfidUid() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    uid += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

// ═══════════════════════════════════════════════════════
void authenticateRfidCard(String rfidUid) {
  Serial.println("🔐 Authenticating with backend...");
  String url = String(API_BASE_URL) + "/api/auth/user-by-rfid/" + rfidUid;
  HTTPClient http;
  http.setTimeout(API_TIMEOUT);
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int code = http.GET();
  String response = http.getString();
  http.end();

  Serial.println("📊 Response: " + String(code));
  Serial.println("📄 Body: " + response);

  if (code == 200) {
    if (parseUserData(response)) {
      lastRfidUid    = rfidUid;
      rideActive     = true;
      lastAuthStatus = "SUCCESS";
      startRide(rfidUid);
      unlockDoor();
      digitalWrite(GREEN_LED, HIGH);
      beep(200); delay(200); beep(200);
      delay(1000);
      digitalWrite(GREEN_LED, LOW);
      displayAuthSuccess();
    }
  } else if (code == 404) {
    Serial.println("❌ Card not recognized!");
    lastAuthStatus = "INVALID_CARD";
    digitalWrite(RED_LED, HIGH);
    beep(100); delay(100); beep(100); delay(100); beep(100);
    delay(1000);
    digitalWrite(RED_LED, LOW);
    displayAuthFailed("Card not valid");
  } else {
    Serial.println("⚠️ Server error: " + String(code));
    lastAuthStatus = "SERVER_ERROR";
    digitalWrite(RED_LED, HIGH);
    beep(100); delay(200); beep(100);
    delay(1000);
    digitalWrite(RED_LED, LOW);
    displayAuthFailed("Server error");
  }
}

// ═══════════════════════════════════════════════════════
void startRide(String rfidUid) {
  Serial.println("🚴 Starting ride...");
  String url = String(API_BASE_URL) + "/api/rides/start";
  HTTPClient http;
  http.setTimeout(API_TIMEOUT);
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  String payload = "{\"rfidUid\":\"" + rfidUid + "\",\"stationId\":\"" +
                   String(STATION_ID) + "\",\"bikeId\":\"" + String(BIKE_ID) + "\"}";
  int code = http.POST(payload);
  String response = http.getString();
  http.end();

  Serial.println("📊 Start Ride: " + String(code));
  if (code == 201) {
    Serial.println("✅ Ride started!");
    if (response.indexOf("rideId") != -1) {
      int s = response.indexOf("\"rideId\":\"") + 10;
      int e = response.indexOf("\"", s);
      currentRideId = response.substring(s, e);
      Serial.println("📍 Ride ID: " + currentRideId);
    }
  } else {
    Serial.println("⚠️ Failed to start ride: " + String(code));
  }
}

// ═══════════════════════════════════════════════════════
void endRide(String rfidUid) {
  Serial.println("🛑 Ending ride...");
  String url = String(API_BASE_URL) + "/api/rides/end";
  HTTPClient http;
  http.setTimeout(API_TIMEOUT);
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  String payload = "{\"rfidUid\":\"" + rfidUid + "\",\"rideId\":\"" + currentRideId +
                   "\",\"stationId\":\"" + String(STATION_ID) + "\"}";
  int code = http.POST(payload);
  String response = http.getString();
  http.end();

  Serial.println("📊 End Ride: " + String(code));
  if (code == 200) {
    Serial.println("✅ Ride ended!");
    lockDoor();
    digitalWrite(GREEN_LED, HIGH);
    beep(100); delay(100); beep(100); delay(100); beep(100);
    delay(1000);
    digitalWrite(GREEN_LED, LOW);
    displayRideEnded();
    rideActive    = false;
    lastRfidUid   = "";
    currentRideId = "";
  } else {
    Serial.println("⚠️ Failed to end ride: " + String(code));
  }
}

// ═══════════════════════════════════════════════════════
bool parseUserData(String response) {
  if (response.indexOf("\"authorized\":true") != -1) {
    if (response.indexOf("\"firstName\"") != -1) {
      int s = response.indexOf("\"firstName\":\"") + 13;
      int e = response.indexOf("\"", s);
      currentUserName = response.substring(s, e);
      if (response.indexOf("\"lastName\"") != -1) {
        int ls = response.indexOf("\"lastName\":\"") + 12;
        int le = response.indexOf("\"", ls);
        currentUserName += " " + response.substring(ls, le);
      }
      Serial.println("👤 User: " + currentUserName);
    }
    return true;
  }
  return false;
}

// ═══════════════════════════════════════════════════════
void beep(int duration) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(duration);
  digitalWrite(BUZZER_PIN, LOW);
}

// ═══════════════════════════════════════════════════════
void displayWelcome() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("== KuetTrack ==");
  lcd.setCursor(0, 1); lcd.print("RFID Door Lock");
  delay(3000);
}

void displayWiFiConnected() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("WiFi Connected");
  lcd.setCursor(0, 1); lcd.print(WiFi.localIP().toString().c_str());
}

void displayWiFiFailed() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("WiFi Failed!");
  lcd.setCursor(0, 1); lcd.print("Retrying...");
}

void displayReady() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Door: LOCKED");
  lcd.setCursor(0, 1); lcd.print("Tap your card");
}

void displayScanning() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Scanning...");
  lcd.setCursor(0, 1); lcd.print("Please wait");
}

void displayAuthSuccess() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Welcome!");
  lcd.setCursor(0, 1);
  String name = currentUserName.substring(0, min((int)currentUserName.length(), 16));
  lcd.print(name);
  delay(3000);
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Auth Success!");
  lcd.setCursor(0, 1); lcd.print("Door: UNLOCKED");
  delay(2000);
  displayReady();
}

void displayAuthFailed(String reason) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Auth Failed!");
  lcd.setCursor(0, 1); lcd.print("Access DENIED!");
  delay(2000);
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Reason:");
  lcd.setCursor(0, 1); lcd.print(reason.c_str());
  delay(2000);
  displayReady();
}

void displayRideEnded() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Ride Completed!");
  lcd.setCursor(0, 1); lcd.print("Door: LOCKED");
  delay(3000);
  displayReady();
}
