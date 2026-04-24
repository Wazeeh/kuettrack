// =====================================================================
// KuetTrack — Merged Controller  (Single ESP32)
// RFID RC522 + GPS NEO-6M + 16×2 I2C LCD + Solenoid Lock (Relay)
// =====================================================================
// ── Wiring ────────────────────────────────────────────────────────────
//   RC522  SDA  → GPIO 5   | SCK  → GPIO 18 | MOSI → GPIO 23
//          MISO → GPIO 19  | RST  → GPIO 4  | VCC  → 3.3V  GND → GND
//
//   NEO-6M VCC  → 5V       | GND  → GND
//          TX   → GPIO 16  | RX   → GPIO 17
//          (NEO-6M TX goes to ESP32 RX pin = GPIO16)
//
//   LCD    VCC  → 5V       | GND  → GND
//          SDA  → GPIO 21  | SCL  → GPIO 22   (I2C addr 0x27 or 0x3F)
//
//   Relay  IN   → GPIO 26  | VCC  → 5V        | GND → GND
//   Solenoid Lock connected to relay COM/NO terminals
//   (Relay is active-LOW: LOW = energized/UNLOCKED, HIGH = de-energized/LOCKED)
//
//   Green LED → GPIO 32   Red LED → GPIO 33   Buzzer → GPIO 25
// =====================================================================
//
// ── STATE MACHINE ─────────────────────────────────────────────────────
//  IDLE          → waiting for RFID tap, LCD shows "Tap RFID card"
//  RFID_VERIFIED → card authenticated, polling server for unlock command
//                  (user must click "Start Ride" on dashboard)
//  RIDE_ACTIVE   → bike unlocked, GPS streaming, waiting for end tap
// =====================================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Preferences.h>
#include <TinyGPS++.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ─── WiFi ─────────────────────────────────────────────────────────────
#define WIFI_SSID      "Z60 plus"
#define WIFI_PASSWORD  "Symphony23"

// ─── Backend ──────────────────────────────────────────────────────────
#define API_BASE_URL   "https://kuettrack.onrender.com"
#define GPS_SERVER_URL "https://kuettrack.onrender.com/api/gps/update"
#define API_TIMEOUT_RFID  8000
#define API_TIMEOUT_GPS   2000
#define API_TIMEOUT_CMD   3000   // command poll is short

// ─── RC522 Pins ───────────────────────────────────────────────────────
#define SS_PIN   5
#define RST_PIN  4

// ─── Output Pins ──────────────────────────────────────────────────────
#define GREEN_LED   32
#define RED_LED     33
#define BUZZER_PIN  25
#define RELAY_PIN   26

// ─── Relay Logic (active-LOW relay module) ────────────────────────────
#define RELAY_UNLOCK  LOW
#define RELAY_LOCK    HIGH

// ─── GPS UART pins ────────────────────────────────────────────────────
#define GPS_RX_PIN  16
#define GPS_TX_PIN  17

#define BIKE_ID     "BIKE-001"
#define STATION_ID  "STATION-001"

// ─── Timing ───────────────────────────────────────────────────────────
#define GPS_SEND_INTERVAL       5000
#define GPS_NOFIX_INTERVAL      15000
#define CMD_POLL_INTERVAL       1000   // how often to poll for unlock command
#define CONNECTION_CHECK        10000
#define LCD_UPDATE_INTERVAL     3000
#define READ_COOLDOWN           2000   // min ms between RFID reads

// ─── State Machine ────────────────────────────────────────────────────
enum BikeState { STATE_IDLE, STATE_RFID_VERIFIED, STATE_RIDE_ACTIVE };
BikeState bikeState = STATE_IDLE;

// ─── Hardware Objects ─────────────────────────────────────────────────
MFRC522           rfid(SS_PIN, RST_PIN);
Preferences       preferences;
TinyGPSPlus       gps;
HardwareSerial    gpsSerial(2);
LiquidCrystal_I2C lcd(0x27, 16, 2);   // change 0x27 → 0x3F if LCD blank

// ─── RFID / Ride State ────────────────────────────────────────────────
String        currentRfidUid    = "";   // UID of the verified card
String        currentRideId     = "";
String        currentUserName   = "";
unsigned long lastReadTime      = 0;

// ─── GPS State ────────────────────────────────────────────────────────
unsigned long lastGpsSend          = 0;
unsigned long lastConnectionCheck  = 0;
unsigned long lastCmdPoll          = 0;
unsigned long lastLcdUpdate        = 0;
unsigned long lastRfidReinit       = 0;
unsigned long lastHeartbeat        = 0;
unsigned long rawBytesReceived     = 0;
int           rfidFailCount        = 0;

// ─── Forward Declarations ─────────────────────────────────────────────
void connectToWiFi();
void authenticateRfidCard(String uid);
void endRide(String uid);
bool parseUserData(String response);
String getRfidUid();
void reinitRFID();
void lockDoor();
void unlockDoor();
void solenoidTest();
void beep(int times, int duration);
void feedback(bool success);
void sendGPSData();
void lcdMsg(String line1, String line2);
void updateLcdForState();
void checkBikeCommand();
void drainGPS();

// ─── Drain GPS serial (non-blocking) ──────────────────────────────────
void drainGPS() {
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
    rawBytesReceived++;
  }
}

// ─── Short blocking wait that keeps draining GPS ──────────────────────
void gpsWait(unsigned long ms) {
  unsigned long t = millis();
  while (millis() - t < ms) {
    drainGPS();
    yield();
  }
}

// ═════════════════════════════════════════════════════════════════════
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== KuetTrack Merged Controller ===");

  // ── LEDs & Buzzer ─────────────────────────────────────────────────
  pinMode(GREEN_LED,  OUTPUT); digitalWrite(GREEN_LED,  LOW);
  pinMode(RED_LED,    OUTPUT); digitalWrite(RED_LED,    LOW);
  pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);

  // ── I2C LCD ───────────────────────────────────────────────────────
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcdMsg("KuetTrack", "Booting...");
  Serial.println("[LCD] OK");

  // ── GPS UART2 — start BEFORE SPI so UART interrupts settle first ──
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.printf("[GPS] UART2 started: RX=GPIO%d  TX=GPIO%d\n", GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("[GPS] GPS will only stream when a ride is active.");
  delay(200);   // let UART interrupt handler register before SPI starts

  // ── SPI + RC522 ───────────────────────────────────────────────────
  SPI.begin(18, 19, 23, 5);
  delay(50);
  rfid.PCD_Init();
  delay(200);
  rfid.PCD_AntennaOn();                               // explicit antenna enable
  rfid.PCD_WriteRegister(MFRC522::RFCfgReg, 0x3F);   // max RF gain

  byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("[RFID] RC522 firmware: 0x%02X  ", ver);
  if (ver == 0x00 || ver == 0xFF) {
    Serial.println("NOT DETECTED — check SPI wiring!");
  } else {
    Serial.println("OK");
  }

  // ── Relay Pin — start LOCKED ───────────────────────────────────────
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_LOCK);
  delay(300);
  solenoidTest();

  // ── Re-init RFID after solenoid test delays ────────────────────────
  // The long delays in solenoidTest() can upset the RC522 SPI state.
  rfid.PCD_Init();
  delay(150);
  rfid.PCD_AntennaOn();
  rfid.PCD_WriteRegister(MFRC522::RFCfgReg, 0x3F);
  Serial.println("[RFID] Re-initialised after solenoid test — READY.");

  // ── Preferences ───────────────────────────────────────────────────
  preferences.begin("kuettrack", false);

  // ── WiFi ──────────────────────────────────────────────────────────
  lcdMsg("WiFi connect", WIFI_SSID);
  connectToWiFi();

  lcdMsg("KuetTrack Ready", "Tap RFID card");
  Serial.println("\n[READY] Tap RFID card to authenticate.\n");
  feedback(true);
}

// ═════════════════════════════════════════════════════════════════════
void loop() {

  // ── Always drain GPS serial (so buffer never overflows) ──────────
  drainGPS();
  yield();

  unsigned long now = millis();

  // ── Serial heartbeat — confirms loop is running (every 8 s) ─────
  if (now - lastHeartbeat > 8000) {
    lastHeartbeat = now;
    const char* stateStr = (bikeState == STATE_IDLE) ? "IDLE" :
                           (bikeState == STATE_RFID_VERIFIED) ? "RFID_VERIFIED" : "RIDE_ACTIVE";
    Serial.printf("[LOOP] alive | state=%s | GPS_bytes=%lu | WiFi=%s\n",
                  stateStr, rawBytesReceived,
                  WiFi.status() == WL_CONNECTED ? "OK" : "DOWN");
  }

  // ── Periodic RFID re-init (antenna can drift, esp. with GPS active) ─
  // Re-init every 15 s when idle — keeps SPI link healthy
  if (bikeState == STATE_IDLE && now - lastRfidReinit > 15000) {
    lastRfidReinit = now;
    rfid.PCD_Init();
    delay(50);
    rfid.PCD_AntennaOn();
    rfid.PCD_WriteRegister(MFRC522::RFCfgReg, 0x3F);
    rfidFailCount = 0;
    Serial.println("[RFID] Periodic re-init OK");
  }

  // ── WiFi watchdog ─────────────────────────────────────────────────
  if (now - lastConnectionCheck > CONNECTION_CHECK) {
    lastConnectionCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Dropped — reconnecting...");
      connectToWiFi();
    }
  }

  // ── RFID polling ─────────────────────────────────────────────────
  if (now - lastReadTime >= READ_COOLDOWN) {
    if (rfid.PICC_IsNewCardPresent()) {
      if (!rfid.PICC_ReadCardSerial()) {
        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();
        if (++rfidFailCount >= 3) {
          Serial.println("[RFID] 3 failed reads — reinitialising...");
          reinitRFID();
          rfidFailCount = 0;
        }
      } else {
        lastReadTime = now;
        String uid = getRfidUid();
        Serial.println("[RFID] UID detected: " + uid);
        beep(1, 80);

        if (bikeState == STATE_IDLE) {
          // First tap — authenticate the card
          authenticateRfidCard(uid);

        } else if (bikeState == STATE_RFID_VERIFIED) {
          if (uid == currentRfidUid) {
            // Same card tapped again — cancel, go back to idle
            Serial.println("[RFID] Card tapped in VERIFIED state — cancelling, returning to IDLE.");
            lcdMsg("Cancelled", "Tap card again");
            feedback(false);
            bikeState = STATE_IDLE;
            currentRfidUid = "";
            currentUserName = "";
            gpsWait(1500);
            lcdMsg("KuetTrack Ready", "Tap RFID card");
          }
          // Different card — ignore

        } else if (bikeState == STATE_RIDE_ACTIVE) {
          if (uid == currentRfidUid) {
            // Second tap of same card — end the ride
            endRide(uid);
          } else {
            // Different card during active ride — deny
            lcdMsg("Ride in progress", "Use your card");
            feedback(false);
            gpsWait(1500);
          }
        }

        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();
      }
    }
  }

  // ── Command polling (only when waiting for dashboard to start ride) ─
  if (bikeState == STATE_RFID_VERIFIED) {
    if (now - lastCmdPoll > CMD_POLL_INTERVAL) {
      lastCmdPoll = now;
      checkBikeCommand();
    }
  }

  // ── GPS send — ONLY during active ride ───────────────────────────
  if (bikeState == STATE_RIDE_ACTIVE) {
    bool hasFix = gps.location.isValid();
    unsigned long interval = hasFix ? GPS_SEND_INTERVAL : GPS_NOFIX_INTERVAL;
    if (now - lastGpsSend > interval) {
      lastGpsSend = now;
      sendGPSData();
    }
  }

  // ── LCD refresh ───────────────────────────────────────────────────
  if (now - lastLcdUpdate > LCD_UPDATE_INTERVAL) {
    lastLcdUpdate = now;
    updateLcdForState();
  }
}

// ═════════════════════════════════════════════════════════════════════
// RFID Authentication — only verifies user, does NOT unlock
// ═════════════════════════════════════════════════════════════════════
void authenticateRfidCard(String uid) {
  Serial.println("[AUTH] Authenticating UID: " + uid);
  lcdMsg("Checking...", uid.substring(0, 16));

  if (WiFi.status() != WL_CONNECTED) {
    lcdMsg("No WiFi!", "Auth failed");
    feedback(false);
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(API_TIMEOUT_RFID);
  http.begin(client, String(API_BASE_URL) + "/api/auth/user-by-rfid/" + uid);
  http.addHeader("Content-Type", "application/json");
  int    code     = http.GET();
  String response = http.getString();
  http.end();
  Serial.printf("[AUTH] HTTP %d\n", code);

  if (code == 200 && parseUserData(response)) {
    currentRfidUid  = uid;
    bikeState       = STATE_RFID_VERIFIED;
    lastCmdPoll     = 0;  // force immediate poll on next loop
    Serial.println("[AUTH] GRANTED — " + currentUserName);
    Serial.println("[AUTH] Waiting for dashboard to start ride...");
    lcdMsg("Verified!", currentUserName.substring(0, 16));
    feedback(true);
    gpsWait(1000);
    lcdMsg("Open app to", "start your ride");

  } else if (code == 404) {
    Serial.println("[AUTH] DENIED — card not registered");
    lcdMsg("ACCESS DENIED", "Not registered");
    feedback(false);
    gpsWait(2000);
    lcdMsg("KuetTrack Ready", "Tap RFID card");

  } else {
    Serial.printf("[AUTH] Error %d\n", code);
    lcdMsg("Auth error", "Try again");
    feedback(false);
    gpsWait(2000);
    lcdMsg("KuetTrack Ready", "Tap RFID card");
  }

  reinitRFID();
}

// ═════════════════════════════════════════════════════════════════════
// Poll server for pending unlock command from dashboard
// ═════════════════════════════════════════════════════════════════════
void checkBikeCommand() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(API_TIMEOUT_CMD);
  http.begin(client, String(API_BASE_URL) + "/api/bikes/" + BIKE_ID + "/command");
  int    code     = http.GET();
  String response = http.getString();
  http.end();

  Serial.printf("[CMD] poll HTTP %d\n", code);
  if (code != 200) return;  // 204 = no command pending

  // Parse response: { "command": "unlock", "rfidUid": "...", "rideId": "..." }
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, response) != DeserializationError::Ok) return;

  String cmd    = doc["command"] | "";
  String rideId = doc["rideId"]  | "";

  // Only check command type — server already validated rfidUid when storing command
  if (cmd == "unlock") {
    currentRideId = rideId;
    Serial.println("[CMD] Unlock command received! Ride ID: " + rideId);
    bikeState = STATE_RIDE_ACTIVE;
    lastGpsSend = 0;  // send GPS immediately on next loop

    unlockDoor();
    feedback(true);

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(("Ride: " + currentUserName).substring(0, 16));
    lcd.setCursor(0, 1);
    lcd.print("GPS updating...");
    Serial.println("[RIDE] Active — GPS streaming started.");
  }

  reinitRFID();
}

// ═════════════════════════════════════════════════════════════════════
// End ride — called on second RFID tap
// ═════════════════════════════════════════════════════════════════════
void endRide(String uid) {
  if (WiFi.status() != WL_CONNECTED) {
    lcdMsg("No WiFi!", "Cannot end ride");
    feedback(false);
    return;
  }

  lcdMsg("Ending ride...", "Please wait");
  Serial.println("[RIDE] Ending ride for UID: " + uid);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(API_TIMEOUT_RFID);
  http.begin(client, String(API_BASE_URL) + "/api/rides/end");
  http.addHeader("Content-Type", "application/json");

  String payload = "{\"rfidUid\":\"" + uid +
                   "\",\"rideId\":\"" + currentRideId +
                   "\",\"stationId\":\"" + STATION_ID + "\"}";
  int    code     = http.POST(payload);
  String response = http.getString();
  http.end();
  Serial.printf("[RIDE] end HTTP %d\n", code);

  if (code == 200) {
    // Parse fare from response
    DynamicJsonDocument doc(256);
    deserializeJson(doc, response);
    String duration = doc["duration"] | "00:00";
    int    fare     = doc["fare"]     | 0;

    lockDoor();
    feedback(true);

    // Reset all state
    bikeState       = STATE_IDLE;
    currentRfidUid  = "";
    currentRideId   = "";
    currentUserName = "";

    Serial.printf("[RIDE] Ended. Duration: %s  Fare: ৳%d\n", duration.c_str(), fare);

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Ride Ended!");
    lcd.setCursor(0, 1);
    String fareStr = "Fare: Tk" + String(fare) + " " + duration;
    lcd.print(fareStr.substring(0, 16));
    gpsWait(3000);
    lcdMsg("KuetTrack Ready", "Tap RFID card");

  } else {
    Serial.printf("[RIDE] End failed HTTP %d: %s\n", code, response.c_str());
    lcdMsg("End failed", "Tap card again");
    feedback(false);
    gpsWait(2000);
  }

  reinitRFID();
}

// ═════════════════════════════════════════════════════════════════════
// LCD update based on current state
// ═════════════════════════════════════════════════════════════════════
void updateLcdForState() {
  if (bikeState == STATE_IDLE) {
    lcdMsg("KuetTrack Ready", "Tap RFID card");

  } else if (bikeState == STATE_RFID_VERIFIED) {
    // Blink between two messages so user knows system is waiting
    static bool toggle = false;
    toggle = !toggle;
    if (toggle) {
      lcdMsg("Card verified!", currentUserName.substring(0, 16));
    } else {
      lcdMsg("Open app to", "start your ride");
    }

  } else if (bikeState == STATE_RIDE_ACTIVE) {
    // Show GPS coordinates during active ride
    lcd.clear();
    lcd.setCursor(0, 0);
    String user16 = (">" + currentUserName).substring(0, 16);
    lcd.print(user16);
    lcd.setCursor(0, 1);
    if (gps.location.isValid()) {
      char buf[17];
      snprintf(buf, sizeof(buf), "%.4f,%.4f",
               gps.location.lat(), gps.location.lng());
      lcd.print(buf);
    } else {
      char s[17];
      snprintf(s, sizeof(s), "GPS fix... s:%u", (unsigned)gps.satellites.value());
      lcd.print(s);
    }
  }
}

// ═════════════════════════════════════════════════════════════════════
// GPS send — only called when RIDE_ACTIVE
// ═════════════════════════════════════════════════════════════════════
void sendGPSData() {
  if (WiFi.status() != WL_CONNECTED) return;
  bool hasFix = gps.location.isValid();

  DynamicJsonDocument doc(256);
  doc["deviceId"]   = currentRfidUid;   // RFID UID as device ID
  doc["hasFix"]     = hasFix;
  doc["satellites"] = (int)gps.satellites.value();
  doc["lat"]        = hasFix ? gps.location.lat()    : 0.0;
  doc["lon"]        = hasFix ? gps.location.lng()    : 0.0;
  doc["speed"]      = hasFix ? gps.speed.kmph()      : 0.0;
  doc["altitude"]   = hasFix ? gps.altitude.meters() : 0.0;

  String body;
  serializeJson(doc, body);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, GPS_SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(API_TIMEOUT_GPS);
  int code = http.POST(body);
  Serial.printf("[GPS] HTTP %d fix=%s lat=%.5f lon=%.5f sats=%u\n",
                code, hasFix ? "yes" : "no",
                hasFix ? gps.location.lat() : 0.0,
                hasFix ? gps.location.lng() : 0.0,
                (unsigned)gps.satellites.value());
  http.end();
}

// ═════════════════════════════════════════════════════════════════════
// Solenoid Lock via Relay
// ═════════════════════════════════════════════════════════════════════
void solenoidTest() {
  Serial.println("[RELAY] Boot test: lock→unlock→lock");
  lcdMsg("Lock Test", "Locking...");
  digitalWrite(RELAY_PIN, RELAY_LOCK);   delay(700);
  lcdMsg("Lock Test", "Unlocking...");
  digitalWrite(RELAY_PIN, RELAY_UNLOCK); delay(900);
  lcdMsg("Lock Test", "Locking...");
  digitalWrite(RELAY_PIN, RELAY_LOCK);   delay(900);
  Serial.println("[RELAY] Boot test complete");
}

void unlockDoor() {
  Serial.println("[RELAY] Unlocking solenoid...");
  lcdMsg("Unlocking...", "");
  digitalWrite(RELAY_PIN, RELAY_UNLOCK);
  Serial.println("[RELAY] UNLOCKED");
}

void lockDoor() {
  Serial.println("[RELAY] Locking solenoid...");
  lcdMsg("Locking...", "");
  digitalWrite(RELAY_PIN, RELAY_LOCK);
  Serial.println("[RELAY] LOCKED");
}

void reinitRFID() {
  delay(100);
  rfid.PCD_Init();
  delay(100);
  rfid.PCD_AntennaOn();                              // must re-enable antenna after every init
  rfid.PCD_WriteRegister(MFRC522::RFCfgReg, 0x3F);  // restore max gain
  delay(50);
}

// ═════════════════════════════════════════════════════════════════════
// LCD helper
// ═════════════════════════════════════════════════════════════════════
void lcdMsg(String line1, String line2) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(line1.substring(0, 16));
  lcd.setCursor(0, 1); lcd.print(line2.substring(0, 16));
}

// ═════════════════════════════════════════════════════════════════════
// WiFi
// ═════════════════════════════════════════════════════════════════════
void connectToWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("[WiFi] Connecting to " + String(WIFI_SSID));
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    drainGPS(); delay(400); Serial.print("."); attempts++;
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.println("\n[WiFi] Connected — " + WiFi.localIP().toString());
  else
    Serial.println("\n[WiFi] Failed — will retry");
}

// ═════════════════════════════════════════════════════════════════════
// RFID helpers
// ═════════════════════════════════════════════════════════════════════
String getRfidUid() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

bool parseUserData(String response) {
  if (response.indexOf("\"authorized\":true") == -1) return false;
  int s = response.indexOf("\"firstName\":\"");
  if (s != -1) {
    s += 13;
    currentUserName = response.substring(s, response.indexOf("\"", s));
    int ls = response.indexOf("\"lastName\":\"");
    if (ls != -1) {
      ls += 12;
      currentUserName += " " + response.substring(ls, response.indexOf("\"", ls));
    }
  }
  return true;
}

// ═════════════════════════════════════════════════════════════════════
// Feedback
// ═════════════════════════════════════════════════════════════════════
void beep(int times, int duration) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delay(duration);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < times - 1) delay(80);
  }
}

void feedback(bool success) {
  if (success) {
    digitalWrite(GREEN_LED, HIGH); beep(2, 150); delay(600); digitalWrite(GREEN_LED, LOW);
  } else {
    digitalWrite(RED_LED, HIGH);   beep(3, 100); delay(600); digitalWrite(RED_LED,   LOW);
  }
}
