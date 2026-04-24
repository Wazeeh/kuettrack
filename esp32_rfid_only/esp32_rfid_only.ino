// =====================================================
// KuetTrack — ESP32 + RC522 RFID Only
// =====================================================
// Wiring:
// RC522 VCC  -> ESP32 3.3V  (NOT 5V)
// RC522 GND  -> ESP32 GND
// RC522 SDA  -> GPIO5
// RC522 SCK  -> GPIO18
// RC522 MOSI -> GPIO23
// RC522 MISO -> GPIO19
// RC522 RST  -> GPIO14
// =====================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <MFRC522.h>

// ─── WiFi ─────────────────────────────────────────────
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ─── Backend ──────────────────────────────────────────
#define API_BASE_URL  "http://192.168.0.120:5000"
#define API_TIMEOUT   8000

// ─── RC522 Pins ───────────────────────────────────────
#define SS_PIN   5
#define RST_PIN  14

// ─── Objects ──────────────────────────────────────────
MFRC522 rfid(SS_PIN, RST_PIN);

// ─── State ────────────────────────────────────────────
String lastRfidUid = "";
bool   rideActive  = false;
unsigned long lastReadTime = 0;
const unsigned long READ_COOLDOWN = 2000;

// ═══════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== KuetTrack RFID ===\n");

  // ── SPI + RC522 ─────────────────────────────────────
  SPI.begin(18, 19, 23, 5);  // SCK, MISO, MOSI, SS
  rfid.PCD_Init();
  delay(100);

  byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.print("RC522 Firmware: 0x");
  Serial.println(ver, HEX);

  if (ver == 0x00 || ver == 0xFF) {
    Serial.println("ERROR: RC522 not detected! Check wiring.");
  } else {
    Serial.println("RC522 OK");
  }

  rfid.PCD_WriteRegister(MFRC522::RFCfgReg, 0x3F);

  // ── WiFi ────────────────────────────────────────────
  connectToWiFi();

  Serial.println("\nReady — tap RFID card\n");
}

// ═══════════════════════════════════════════════════════
void loop() {

  // WiFi watchdog
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost — reconnecting...");
    connectToWiFi();
  }

  // Cooldown between reads
  if (millis() - lastReadTime < READ_COOLDOWN) {
    delay(50);
    return;
  }

  if (!rfid.PICC_IsNewCardPresent()) return;

  if (!rfid.PICC_ReadCardSerial()) {
    Serial.println("Read failed — hold card flat and still");
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    static int failCount = 0;
    failCount++;
    if (failCount >= 3) {
      rfid.PCD_Init();
      rfid.PCD_WriteRegister(MFRC522::RFCfgReg, 0x3F);
      failCount = 0;
    }
    delay(500);
    return;
  }

  lastReadTime = millis();
  String rfidUid = getRfidUid();
  Serial.println("Card UID: " + rfidUid);

  // Same card tapped again while ride active = end ride
  if (rfidUid == lastRfidUid && rideActive) {
    endRide(rfidUid);
  } else {
    authenticateRfidCard(rfidUid);
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

// ═══════════════════════════════════════════════════════
String getRfidUid() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

// ═══════════════════════════════════════════════════════
void authenticateRfidCard(String rfidUid) {
  Serial.println("Authenticating: " + rfidUid);

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("No WiFi — cannot authenticate");
    return;
  }

  String url = String(API_BASE_URL) + "/api/auth/user-by-rfid/" + rfidUid;
  HTTPClient http;
  http.setTimeout(API_TIMEOUT);
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int    code     = http.GET();
  String response = http.getString();
  http.end();

  Serial.println("HTTP: " + String(code));
  Serial.println("Body: " + response);

  if (code == 200) {
    String userName = "";
    int s = response.indexOf("\"firstName\":\"");
    if (s != -1) {
      s += 13;
      userName = response.substring(s, response.indexOf("\"", s));
      int ls = response.indexOf("\"lastName\":\"");
      if (ls != -1) {
        ls += 12;
        userName += " " + response.substring(ls, response.indexOf("\"", ls));
      }
    }
    Serial.println("ACCESS GRANTED — " + userName);
    lastRfidUid = rfidUid;
    rideActive  = true;
    startRide(rfidUid);

  } else if (code == 404) {
    Serial.println("ACCESS DENIED — card not registered");

  } else if (code < 0) {
    Serial.println("Connection error: " + String(code));
    Serial.println("Check server is running at: " + String(API_BASE_URL));

  } else {
    Serial.println("Server error: " + String(code));
  }
}

// ═══════════════════════════════════════════════════════
void startRide(String rfidUid) {
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.println("Starting ride...");
  HTTPClient http;
  http.setTimeout(API_TIMEOUT);
  http.begin(String(API_BASE_URL) + "/api/rides/start");
  http.addHeader("Content-Type", "application/json");

  String payload = "{\"rfidUid\":\"" + rfidUid +
                   "\",\"stationId\":\"STATION-001\",\"bikeId\":\"BIKE-001\"}";

  int    code     = http.POST(payload);
  String response = http.getString();
  http.end();

  Serial.println("Start ride HTTP: " + String(code));
  if (code == 201) {
    Serial.println("Ride started");
  } else {
    Serial.println("Failed: " + String(code));
  }
}

// ═══════════════════════════════════════════════════════
void endRide(String rfidUid) {
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.println("Ending ride...");
  HTTPClient http;
  http.setTimeout(API_TIMEOUT);
  http.begin(String(API_BASE_URL) + "/api/rides/end");
  http.addHeader("Content-Type", "application/json");

  String payload = "{\"rfidUid\":\"" + rfidUid +
                   "\",\"stationId\":\"STATION-001\"}";

  int code = http.POST(payload);
  http.end();

  Serial.println("End ride HTTP: " + String(code));
  if (code == 200) {
    Serial.println("Ride ended");
    rideActive  = false;
    lastRfidUid = "";
  } else {
    Serial.println("Failed: " + String(code));
  }
}

// ═══════════════════════════════════════════════════════
void connectToWiFi() {
  Serial.print("Connecting to " + String(WIFI_SSID));
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi OK — IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi FAILED");
  }
}
