/*
  ============================================================
  🚲 BIKE RENTAL SYSTEM — ESP32 Firmware
  ------------------------------------------------------------
  Hardware:
    - ESP32 Dev Board
    - RC522 RFID Module  → SPI
    - GPS NEO-6M         → UART2 (GPIO16/17)
    - 12V Solenoid Lock  → 5V Relay → GPIO26

  Server API (KuetTrack):
    POST /api/rfid/scan    → Validate RFID + get lockAction
    POST /api/gps/update   → Send GPS coordinates
  ============================================================
*/

#include <SPI.h>
#include <MFRC522.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <HTTPClient.h>

// ─── WiFi Credentials ────────────────────────────────────────
const char* WIFI_SSID     = "KJAH_201";      // 🔁 Replace
const char* WIFI_PASSWORD = "36140343";  // 🔁 Replace

// ─── Server URLs (KuetTrack backend) ─────────────────────────
// 🔁 Replace YOUR_SERVER_IP with the LAN IP printed when server starts
// Example: "http://192.168.1.105:5000/api/rfid/scan"
const char* RFID_SCAN_URL  = "http://192.168.0.120:5000/api/rfid/scan";
const char* GPS_UPDATE_URL = "http://192.168.0.120:5000/api/gps/update";

// ─── Pin Definitions ─────────────────────────────────────────
#define RST_PIN    22
#define SS_PIN     21
#define RELAY_PIN  26   // LOW = relay ON = solenoid UNLOCKED

// ─── GPS Serial ──────────────────────────────────────────────
#define GPS_RX    16
#define GPS_TX    17
#define GPS_BAUD  9600

// ─── Objects ─────────────────────────────────────────────────
MFRC522 rfid(SS_PIN, RST_PIN);
TinyGPSPlus gps;
HardwareSerial gpsSerial(2);

// ─── State ───────────────────────────────────────────────────
bool isUnlocked         = false;
String currentRenterUID = "";
unsigned long lastGPSSend  = 0;
unsigned long lastCardScan = 0;
const unsigned long GPS_INTERVAL  = 10000; // Send GPS every 10 seconds
const unsigned long SCAN_COOLDOWN = 3000;  // Prevent double-scan within 3 seconds

// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n🚲 KuetTrack Bike Node Starting...");

  // Relay — HIGH = relay OFF = solenoid LOCKED
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  Serial.println("🔒 Solenoid: LOCKED");

  // SPI + RFID
  SPI.begin();
  rfid.PCD_Init();
  Serial.println("📡 RFID ready");

  // ─── RC522 Diagnostic ────────────────────────────────────
  // Reads the RC522 version register over SPI.
  // 0x91 or 0x92 = chip found and wired correctly ✅
  // 0x00 or 0xFF = SPI wiring problem ❌
  byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.print("🔍 RC522 Version Register: 0x");
  Serial.println(v, HEX);
  if (v == 0x91 || v == 0x92) {
    Serial.println("✅ RC522 detected and working!");
  } else if (v == 0x00) {
    Serial.println("❌ RC522 not found — check SPI wiring (likely MOSI/MISO/SCK/SS)");
  } else if (v == 0xFF) {
    Serial.println("❌ RC522 not responding — check 3.3V power and GND connection");
  } else {
    Serial.println("⚠️  RC522 returned unexpected value — possible loose wire");
  }
  // ─────────────────────────────────────────────────────────

  // GPS
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.println("🛰️  GPS ready");

  // WiFi
  connectWiFi();
}

// ─────────────────────────────────────────────────────────────
void loop() {
  // 1. Feed GPS data continuously
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }

  // 2. Check for RFID card (with cooldown to prevent double-scan)
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    if (millis() - lastCardScan > SCAN_COOLDOWN) {
      handleRFID();
      lastCardScan = millis();
    }
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
  }

  // 3. Send GPS while bike is rented
  if (isUnlocked && millis() - lastGPSSend >= GPS_INTERVAL) {
    sendGPS();
    lastGPSSend = millis();
  }

  // 4. Reconnect WiFi if dropped
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️  WiFi lost. Reconnecting...");
    connectWiFi();
  }
}

// ─────────────────────────────────────────────────────────────
void handleRFID() {
  // Build UID string — uppercase, no spaces (e.g. "C33B51FE")
  // This matches how your server stores rfidUid in MongoDB
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();

  Serial.print("\n📛 Card scanned: ");
  Serial.println(uid);

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️  No WiFi — cannot verify card with server");
    return;
  }

  // POST to /api/rfid/scan
  // Server validates card, handles lock/unlock toggle, returns lockAction
  HTTPClient http;
  http.begin(RFID_SCAN_URL);
  http.addHeader("Content-Type", "application/json");

  String payload = "{\"uid\":\"" + uid + "\",\"stationId\":\"BIKE-001\"}";
  int code = http.POST(payload);

  if (code == 200) {
    String response = http.getString();
    Serial.print("✅ Server: ");
    Serial.println(response);

    // Parse lockAction from JSON response
    // Server returns: {"authorized":true,"lockAction":"unlock"/"lock"/"denied",...}
    if (response.indexOf("\"lockAction\":\"unlock\"") >= 0) {
      isUnlocked = true;
      currentRenterUID = uid;
      digitalWrite(RELAY_PIN, LOW);   // Relay ON → solenoid gets 12V → UNLOCKS
      Serial.println("🔓 Bike UNLOCKED — Rental started");
      lastGPSSend = 0; // Trigger GPS send immediately

    } else if (response.indexOf("\"lockAction\":\"lock\"") >= 0) {
      isUnlocked = false;
      currentRenterUID = "";
      digitalWrite(RELAY_PIN, HIGH);  // Relay OFF → solenoid loses power → LOCKS
      Serial.println("🔒 Bike LOCKED — Rental ended");

    } else if (response.indexOf("\"lockAction\":\"denied\"") >= 0) {
      Serial.println("⚠️  Denied — Cannot lock a bike rented by someone else");
    }

  } else if (code == 404) {
    Serial.println("❌ Access Denied — Card not registered in system");
  } else {
    Serial.print("❌ Server error: HTTP ");
    Serial.println(code);
  }

  http.end();
}

// ─────────────────────────────────────────────────────────────
void sendGPS() {
  if (WiFi.status() != WL_CONNECTED) return;

  float lat = 0.0, lon = 0.0, spd = 0.0, alt = 0.0;
  int   sats = 0;
  bool  hasFix = false;

  if (gps.location.isValid()) {
    lat    = gps.location.lat();
    lon    = gps.location.lng();
    hasFix = true;
  }
  if (gps.speed.isValid())      spd  = gps.speed.kmph();
  if (gps.altitude.isValid())   alt  = gps.altitude.meters();
  if (gps.satellites.isValid()) sats = gps.satellites.value();

  // JSON payload matching your server's GpsLocation schema exactly:
  // { lat, lon, speed, altitude, satellites, deviceId, hasFix }
  // deviceId = RFID UID of the current renter (links GPS to user in MongoDB)
  String payload = "{";
  payload += "\"lat\":"         + String(lat, 6) + ",";
  payload += "\"lon\":"         + String(lon, 6) + ",";
  payload += "\"speed\":"       + String(spd, 1) + ",";
  payload += "\"altitude\":"    + String(alt, 1) + ",";
  payload += "\"satellites\":"  + String(sats)   + ",";
  payload += "\"deviceId\":\""  + currentRenterUID + "\",";
  payload += "\"hasFix\":"      + String(hasFix ? "true" : "false");
  payload += "}";

  HTTPClient http;
  http.begin(GPS_UPDATE_URL);
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(payload);

  if (code == 200) {
    Serial.print("📤 GPS → Lat:");
    Serial.print(lat, 5);
    Serial.print(" Lon:");
    Serial.print(lon, 5);
    Serial.print(" Sats:");
    Serial.print(sats);
    Serial.print(" Fix:");
    Serial.println(hasFix ? "✅" : "❌");
  } else {
    Serial.print("❌ GPS POST failed: HTTP ");
    Serial.println(code);
  }

  http.end();
}

// ─────────────────────────────────────────────────────────────
void connectWiFi() {
  Serial.print("📶 Connecting to: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi connected!");
    Serial.print("📍 ESP32 IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n❌ WiFi failed. Will retry in loop...");
  }
}
