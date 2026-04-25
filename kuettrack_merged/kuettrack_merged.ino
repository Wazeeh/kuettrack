// =====================================================================
// KuetTrack — Merged Controller  (Single ESP32)
// RFID RC522 + GPS NEO-6M + 16×2 I2C LCD + Solenoid Lock (Relay)
// RFID auth via HTTP (primary, instant) — MQTT for GPS + dashboard commands
// =====================================================================
// ── Required Libraries (install in Arduino IDE Library Manager) ──────
//   • MFRC522          (by GithubCommunity)
//   • TinyGPS++        (by Mikal Hart)
//   • ArduinoJson      (by Benoit Blanchon)  v6.x
//   • LiquidCrystal_I2C(by Frank de Brabander)
//   • PubSubClient     (by Nick O'Leary)      ← NEW for MQTT
//
// ── Wiring ───────────────────────────────────────────────────────────
//   RC522  SDA→GPIO5  SCK→GPIO18  MOSI→GPIO23  MISO→GPIO19  RST→GPIO4
//   NEO-6M TX→GPIO16  RX→GPIO17  VCC→5V  GND→GND
//   LCD    SDA→GPIO21  SCL→GPIO22  (I2C 0x27)
//   Relay  IN→GPIO26  (active-LOW: LOW=UNLOCK, HIGH=LOCK)
//   Green LED→GPIO32  Red LED→GPIO33  Buzzer→GPIO25
// =====================================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>           // MQTT
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

// ─── MQTT Broker ──────────────────────────────────────────────────────
// broker.hivemq.com = free public broker, no account needed.
// For private broker: sign up at console.hivemq.cloud (free) and update below.
#define MQTT_BROKER    "broker.hivemq.com"
#define MQTT_PORT      1883          // plain TCP (no TLS, easiest for ESP32)
#define MQTT_USER      ""           // leave empty for public broker
#define MQTT_PASS      ""           // leave empty for public broker

// ─── MQTT Topics ──────────────────────────────────────────────────────
#define TOPIC_GPS      "kuettrack/BIKE-001/gps"     // ESP32 → publishes
#define TOPIC_RFID     "kuettrack/BIKE-001/rfid"    // ESP32 → publishes
#define TOPIC_AUTH     "kuettrack/BIKE-001/auth"    // server → ESP32 subscribes
#define TOPIC_CMD      "kuettrack/BIKE-001/cmd"     // server → ESP32 subscribes
#define TOPIC_STATUS   "kuettrack/BIKE-001/status"  // ESP32 → publishes (LWT)

// ─── HTTP Backend (RFID auth — primary path) ──────────────────────────────
#define API_BASE_URL   "https://kuettrack.onrender.com"

// ─── Hardware Pins ────────────────────────────────────────────────────
#define SS_PIN     5
#define RST_PIN    4
#define GREEN_LED  32
#define RED_LED    33
#define BUZZER_PIN 25
#define RELAY_PIN  26
#define GPS_RX_PIN 16
#define GPS_TX_PIN 17

// Relay logic (active-LOW module)
#define RELAY_UNLOCK LOW
#define RELAY_LOCK   HIGH

// ─── Identifiers ──────────────────────────────────────────────────────
#define BIKE_ID    "BIKE-001"
#define STATION_ID "STATION-001"

// ─── Timing ───────────────────────────────────────────────────────────
#define GPS_SEND_INTERVAL    5000
#define GPS_NOFIX_INTERVAL  15000
#define CONNECTION_CHECK    15000
#define LCD_UPDATE_INTERVAL  3000
#define READ_COOLDOWN        2000
#define AUTH_TIMEOUT        12000   // safety net only — HTTP call itself has its own 10 s timeout
#define MQTT_KEEPALIVE         60

// ─── State Machine ────────────────────────────────────────────────────
enum BikeState { STATE_IDLE, STATE_AUTH_PENDING, STATE_RFID_VERIFIED, STATE_RIDE_ACTIVE };
BikeState bikeState = STATE_IDLE;

// ─── Hardware Objects ─────────────────────────────────────────────────
MFRC522           rfid(SS_PIN, RST_PIN);
Preferences       preferences;
TinyGPSPlus       gps;
HardwareSerial    gpsSerial(2);
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ─── MQTT ─────────────────────────────────────────────────────────────
WiFiClient        wifiClient;       // plain TCP for MQTT (port 1883)
WiFiClientSecure  secureClient;     // TLS for HTTPS fallback only
PubSubClient      mqttClient(wifiClient);

// ─── State Variables ──────────────────────────────────────────────────
String        currentRfidUid    = "";
String        currentRideId     = "";
String        currentUserName   = "";
unsigned long lastReadTime      = 0;
unsigned long authRequestedAt   = 0;
bool          authPending       = false;

// ─── Timers ───────────────────────────────────────────────────────────
unsigned long lastGpsSend         = 0;
unsigned long lastConnectionCheck = 0;
unsigned long lastLcdUpdate       = 0;
unsigned long lastRfidReinit      = 0;
unsigned long lastHeartbeat       = 0;
unsigned long rawBytesReceived    = 0;
int           rfidFailCount       = 0;

// ─── LED state ────────────────────────────────────────────────────────
unsigned long ledOffAt       = 0;
bool          ledGreenActive = false;
bool          ledRedActive   = false;

// ─── Forward Declarations ─────────────────────────────────────────────
void connectToWiFi();
void connectMqtt();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void publishGps();
void httpRfidAuth(String uid);
void publishStatus(bool online);
void lockDoor();
void unlockDoor();
void solenoidTest();
void beep(int times, int duration);
void feedbackAsync(bool success);
void tickLed();
void lcdMsg(String line1, String line2);
void updateLcdForState();
String getRfidUid();
void   reinitRFID();
void   drainGPS();
void   gpsWait(unsigned long ms);

// ──────────────────────────────────────────────────────────────────────
void drainGPS() {
  while (gpsSerial.available()) { gps.encode(gpsSerial.read()); rawBytesReceived++; }
}
void gpsWait(unsigned long ms) {
  unsigned long t = millis();
  while (millis() - t < ms) { drainGPS(); mqttClient.loop(); yield(); }
}

// ═════════════════════════════════════════════════════════════════════
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200); delay(300);
  Serial.println("\n=== KuetTrack MQTT Controller ===");

  pinMode(GREEN_LED,  OUTPUT); digitalWrite(GREEN_LED,  LOW);
  pinMode(RED_LED,    OUTPUT); digitalWrite(RED_LED,    LOW);
  pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(21, 22);
  lcd.init(); lcd.backlight();
  lcdMsg("KuetTrack", "Booting...");

  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  SPI.begin(18, 19, 23, 5); delay(50);
  rfid.PCD_Init(); delay(200);
  rfid.PCD_AntennaOn();
  rfid.PCD_WriteRegister(MFRC522::RFCfgReg, 0x3F);
  byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("[RFID] RC522 firmware: 0x%02X\n", ver);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_LOCK);
  delay(300);
  solenoidTest();

  rfid.PCD_Init(); delay(150);
  rfid.PCD_AntennaOn();
  rfid.PCD_WriteRegister(MFRC522::RFCfgReg, 0x3F);

  secureClient.setInsecure();
  preferences.begin("kuettrack", false);

  lcdMsg("WiFi connect", WIFI_SSID);
  connectToWiFi();

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(MQTT_KEEPALIVE);
  mqttClient.setSocketTimeout(5);
  connectMqtt();

  lcdMsg("KuetTrack Ready", "Tap RFID card");
  feedbackAsync(true);
  Serial.println("[READY] MQTT connected. Tap RFID card.\n");
}

// ═════════════════════════════════════════════════════════════════════
void loop() {
  drainGPS();
  yield();
  tickLed();

  // ── MQTT keep-alive ───────────────────────────────────────────────
  if (!mqttClient.connected()) connectMqtt();
  mqttClient.loop();

  unsigned long now = millis();

  // ── Heartbeat ─────────────────────────────────────────────────────
  if (now - lastHeartbeat > 10000) {
    lastHeartbeat = now;
    const char* s = bikeState == STATE_IDLE          ? "IDLE" :
                    bikeState == STATE_AUTH_PENDING   ? "AUTH_PENDING" :
                    bikeState == STATE_RFID_VERIFIED  ? "RFID_VERIFIED" : "RIDE_ACTIVE";
    Serial.printf("[LOOP] %s | heap=%u | MQTT=%s\n",
                  s, ESP.getFreeHeap(), mqttClient.connected() ? "OK" : "DOWN");
  }

  // ── Periodic RFID re-init (idle only) ────────────────────────────
  if (bikeState == STATE_IDLE && now - lastRfidReinit > 15000) {
    lastRfidReinit = now;
    rfid.PCD_Init(); delay(30);
    rfid.PCD_AntennaOn();
    rfid.PCD_WriteRegister(MFRC522::RFCfgReg, 0x3F);
    rfidFailCount = 0;
  }

  // ── WiFi watchdog ─────────────────────────────────────────────────
  if (now - lastConnectionCheck > CONNECTION_CHECK) {
    lastConnectionCheck = now;
    if (WiFi.status() != WL_CONNECTED) { connectToWiFi(); connectMqtt(); }
  }

  // ── Auth timeout ──────────────────────────────────────────────────
  if (bikeState == STATE_AUTH_PENDING && now - authRequestedAt > AUTH_TIMEOUT) {
    Serial.println("[AUTH] Timeout");
    lcdMsg("Auth timeout", "Try again");
    feedbackAsync(false);
    bikeState = STATE_IDLE;
    authPending = false;
    currentRfidUid = "";
    gpsWait(1500);
    lcdMsg("KuetTrack Ready", "Tap RFID card");
    reinitRFID();
  }

  // ── RFID polling ─────────────────────────────────────────────────
  if (bikeState != STATE_AUTH_PENDING && now - lastReadTime >= READ_COOLDOWN) {
    if (rfid.PICC_IsNewCardPresent()) {
      if (!rfid.PICC_ReadCardSerial()) {
        rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
        if (++rfidFailCount >= 3) { reinitRFID(); rfidFailCount = 0; }
      } else {
        lastReadTime = now;
        String uid = getRfidUid();
        Serial.println("[RFID] UID: " + uid);
        beep(1, 80);

        if (bikeState == STATE_IDLE) {
          lcdMsg("Checking...", uid.substring(0, 16));
          currentRfidUid  = uid;
          bikeState       = STATE_AUTH_PENDING;
          authRequestedAt = now;
          authPending     = true;
          httpRfidAuth(uid);   // synchronous — result applied inside, state updated before returning

        } else if (bikeState == STATE_RFID_VERIFIED) {
          if (uid == currentRfidUid) {
            // Re-check auth — if user has selected a bike on the dashboard,
            // server will now return lockAction:"unlock" and solenoid opens.
            // If not yet selected, server returns no lockAction and we stay verified.
            lcdMsg("Checking...", uid.substring(0, 16));
            bikeState       = STATE_AUTH_PENDING;
            authRequestedAt = now;
            authPending     = true;
            httpRfidAuth(uid);
          }

        } else if (bikeState == STATE_RIDE_ACTIVE) {
          if (uid == currentRfidUid) {
            lcdMsg("Ending ride...", "Please wait");
            bikeState       = STATE_AUTH_PENDING;
            authRequestedAt = now;
            authPending     = true;
            httpRfidAuth(uid);   // synchronous — handles lock/fare inline
          } else {
            lcdMsg("Ride in progress", "Use your card");
            feedbackAsync(false);
          }
        }

        rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
      }
    }
  }

  // ── GPS publish during active ride ───────────────────────────────
  if (bikeState == STATE_RIDE_ACTIVE) {
    bool hasFix = gps.location.isValid();
    unsigned long interval = hasFix ? GPS_SEND_INTERVAL : GPS_NOFIX_INTERVAL;
    if (now - lastGpsSend > interval) {
      lastGpsSend = now;
      publishGps();
    }
  }

  // ── LCD refresh ──────────────────────────────────────────────────
  if (now - lastLcdUpdate > LCD_UPDATE_INTERVAL) {
    lastLcdUpdate = now;
    updateLcdForState();
  }
}

// ═════════════════════════════════════════════════════════════════════
// MQTT Callback
// ═════════════════════════════════════════════════════════════════════
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String topicStr(topic);
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.println("[MQTT RX] " + topicStr + " → " + msg);

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, msg) != DeserializationError::Ok) return;

  // ── Command from website (start/end ride) ───────────────────────
  if (topicStr == TOPIC_CMD) {
    String cmd    = doc["command"] | "";
    String rideId = doc["rideId"]  | "";

    if (cmd == "unlock") {
      // Guard: ignore stale QoS-1 broker message delivered after ride already ended via RFID
      if (bikeState == STATE_RIDE_ACTIVE) {
        Serial.println("[CMD] UNLOCK ignored — ride already active (stale broker msg)");
        reinitRFID(); return;
      }
      currentRideId = rideId;
      // Capture rfidUid + userName so GPS publishes with the correct deviceId
      String cmdRfid = doc["rfidUid"] | String("");
      cmdRfid.toUpperCase();
      if (cmdRfid.length() > 0) currentRfidUid = cmdRfid;
      String uname = doc["userName"] | String("Rider");
      if (uname.length() > 0) currentUserName = uname;
      bikeState     = STATE_RIDE_ACTIVE;
      lastGpsSend   = 0;
      unlockDoor(); feedbackAsync(true);
      lcd.clear();
      lcd.setCursor(0,0); lcd.print(("Ride: " + currentUserName).substring(0,16));
      lcd.setCursor(0,1); lcd.print("GPS updating...");
      Serial.println("[CMD] UNLOCK rfid=" + currentRfidUid + " ride=" + rideId);

    } else if (cmd == "lock") {
      lockDoor(); feedbackAsync(false);
      bikeState = STATE_IDLE;
      currentRfidUid = ""; currentRideId = ""; currentUserName = "";
      lcdMsg("Ride Ended", "Locked remotely");
      gpsWait(2500); lcdMsg("KuetTrack Ready", "Tap RFID card");
      Serial.println("[CMD] LOCK received (web end-ride)");
    }
    reinitRFID();
  }
}

// ═════════════════════════════════════════════════════════════════════
// Publish helpers
// ═════════════════════════════════════════════════════════════════════
// ─── httpRfidAuth — PRIMARY auth path ────────────────────────────────────
// POST /api/rfid/scan → { authorized, lockAction, userName, rideId, duration, fare }
// Synchronous: blocks until server responds or times out.
// Replaces the old MQTT publish→wait→mqttCallback pattern.
// Root cause of the timeout bug: Render free dyno sleeps → server MQTT client
// disconnects from HiveMQ → no auth response ever arrives. HTTP wakes the dyno.
void httpRfidAuth(String uid) {
  authPending = false;

  if (WiFi.status() != WL_CONNECTED) {
    lcdMsg("No WiFi", "Cannot verify");
    feedbackAsync(false);
    bikeState = STATE_IDLE; currentRfidUid = "";
    gpsWait(1500); lcdMsg("KuetTrack Ready", "Tap RFID card");
    reinitRFID(); return;
  }

  // Build request body
  StaticJsonDocument<128> reqDoc;
  reqDoc["uid"]       = uid;
  reqDoc["stationId"] = STATION_ID;
  char body[128]; serializeJson(reqDoc, body);

  HTTPClient http;
  String url = String(API_BASE_URL) + "/api/rfid/scan";
  http.begin(secureClient, url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);   // 10 s covers Render cold-start (~7-8 s)

  Serial.printf("[HTTP] POST %s uid=%s\n", url.c_str(), uid.c_str());
  int code = http.POST(body);

  if (code <= 0) {
    Serial.printf("[HTTP] Error: %s\n", http.errorToString(code).c_str());
    http.end();
    lcdMsg("Server error", "Try again");
    feedbackAsync(false);
    bikeState = STATE_IDLE; currentRfidUid = "";
    gpsWait(1500); lcdMsg("KuetTrack Ready", "Tap RFID card");
    reinitRFID(); return;
  }

  String respBody = http.getString();
  http.end();
  Serial.printf("[HTTP] %d → %s\n", code, respBody.c_str());

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, respBody) != DeserializationError::Ok) {
    lcdMsg("Parse error", "Try again");
    feedbackAsync(false);
    bikeState = STATE_IDLE; currentRfidUid = "";
    gpsWait(1500); lcdMsg("KuetTrack Ready", "Tap RFID card");
    reinitRFID(); return;
  }

  // ── Same logic that was in mqttCallback TOPIC_AUTH ───────────────────
  bool authorized = doc["authorized"] | false;

  if (!authorized) {
    lcdMsg("ACCESS DENIED", "Not registered");
    feedbackAsync(false);
    bikeState = STATE_IDLE; currentRfidUid = "";
    gpsWait(1500); lcdMsg("KuetTrack Ready", "Tap RFID card");
    reinitRFID(); return;
  }

  String lockAction   = doc["lockAction"] | String("");
  currentUserName     = doc["userName"]   | String("Rider");

  if (lockAction == "unlock") {
    currentRideId = doc["rideId"] | String("");
    bikeState     = STATE_RIDE_ACTIVE;
    lastGpsSend   = 0;
    unlockDoor(); feedbackAsync(true);
    lcd.clear();
    lcd.setCursor(0,0); lcd.print(("Ride: " + currentUserName).substring(0,16));
    lcd.setCursor(0,1); lcd.print("GPS updating...");

  } else if (lockAction == "lock") {
    String duration = doc["duration"] | String("00:00");
    int    fare     = doc["fare"]     | 0;
    lockDoor(); feedbackAsync(true);
    bikeState = STATE_IDLE;
    currentRfidUid = ""; currentRideId = ""; currentUserName = "";
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Ride Ended!");
    lcd.setCursor(0,1);
    String fs = "Fare:Tk" + String(fare) + " " + duration;
    lcd.print(fs.substring(0,16));
    gpsWait(3000); lcdMsg("KuetTrack Ready", "Tap RFID card");

  } else {
    // Authorized but no lockAction — user hasn't selected a bike yet
    bikeState = STATE_RFID_VERIFIED;
    lcdMsg("Verified!", currentUserName.substring(0,16));
    feedbackAsync(true);
    gpsWait(800); lcdMsg("Select bike in", "app, tap to unlock");
  }
  reinitRFID();
}

void publishGps() {
  if (!mqttClient.connected()) return;
  bool hasFix = gps.location.isValid();
  StaticJsonDocument<256> doc;
  doc["deviceId"]  = currentRfidUid;
  doc["bikeId"]    = BIKE_ID;
  doc["hasFix"]    = hasFix;
  doc["satellites"]= (int)gps.satellites.value();
  doc["lat"]       = hasFix ? gps.location.lat()    : 0.0;
  doc["lon"]       = hasFix ? gps.location.lng()    : 0.0;
  doc["speed"]     = hasFix ? gps.speed.kmph()      : 0.0;
  doc["altitude"]  = hasFix ? gps.altitude.meters() : 0.0;
  doc["timestamp"] = (unsigned long)millis();
  char buf[256]; serializeJson(doc, buf);
  mqttClient.publish(TOPIC_GPS, buf, false);
  Serial.printf("[MQTT TX] GPS fix=%s lat=%.5f lon=%.5f\n",
                hasFix?"yes":"no",
                hasFix?gps.location.lat():0.0,
                hasFix?gps.location.lng():0.0);
}

void publishStatus(bool online) {
  StaticJsonDocument<64> doc;
  doc["online"] = online; doc["bikeId"] = BIKE_ID;
  char buf[64]; serializeJson(doc, buf);
  mqttClient.publish(TOPIC_STATUS, buf, true);
}

// ═════════════════════════════════════════════════════════════════════
// MQTT Connect
// ═════════════════════════════════════════════════════════════════════
void connectMqtt() {
  int attempts = 0;
  while (!mqttClient.connected() && attempts < 5) {
    attempts++;
    String clientId = "esp32-kuet-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    Serial.print("[MQTT] Connecting as " + clientId + "...");
    bool ok;
    if (strlen(MQTT_USER) > 0)
      ok = mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS,
                              TOPIC_STATUS, 0, true, "{\"online\":false}");
    else
      ok = mqttClient.connect(clientId.c_str(),
                              TOPIC_STATUS, 0, true, "{\"online\":false}");
    if (ok) {
      Serial.println(" connected!");
      mqttClient.subscribe(TOPIC_CMD,  1);   // dashboard unlock/lock commands
      publishStatus(true);
    } else {
      Serial.printf(" failed rc=%d. Retry in 3s\n", mqttClient.state());
      lcdMsg("MQTT connect", "Retrying...");
      drainGPS(); delay(3000);
    }
  }
  if (!mqttClient.connected())
    Serial.println("[MQTT] Could not connect — running in HTTP-only mode");
}

// ═════════════════════════════════════════════════════════════════════
// Solenoid
// ═════════════════════════════════════════════════════════════════════
void solenoidTest() {
  lcdMsg("Lock Test", "Lock→Unlock→Lock");
  digitalWrite(RELAY_PIN, RELAY_LOCK);   delay(700);
  digitalWrite(RELAY_PIN, RELAY_UNLOCK); delay(900);
  digitalWrite(RELAY_PIN, RELAY_LOCK);   delay(900);
  Serial.println("[RELAY] Boot test complete");
}
void unlockDoor() {
  Serial.println("[RELAY] UNLOCKING");
  lcdMsg("Unlocking...", ""); digitalWrite(RELAY_PIN, RELAY_UNLOCK);
}
void lockDoor() {
  Serial.println("[RELAY] LOCKING");
  lcdMsg("Locking...", ""); digitalWrite(RELAY_PIN, RELAY_LOCK);
}

// ═════════════════════════════════════════════════════════════════════
// LED / Buzzer
// ═════════════════════════════════════════════════════════════════════
void beep(int times, int duration) {
  for(int i=0;i<times;i++){
    digitalWrite(BUZZER_PIN,HIGH); delay(duration);
    digitalWrite(BUZZER_PIN,LOW);
    if(i<times-1) delay(80);
  }
}
void feedbackAsync(bool success) {
  beep(success?2:3, success?150:100);
  if(success){digitalWrite(GREEN_LED,HIGH);ledGreenActive=true;ledRedActive=false;digitalWrite(RED_LED,LOW);}
  else       {digitalWrite(RED_LED,HIGH);ledRedActive=true;ledGreenActive=false;digitalWrite(GREEN_LED,LOW);}
  ledOffAt=millis()+800;
}
void tickLed() {
  if((ledGreenActive||ledRedActive)&&millis()>=ledOffAt){
    digitalWrite(GREEN_LED,LOW);digitalWrite(RED_LED,LOW);
    ledGreenActive=false;ledRedActive=false;
  }
}

// ═════════════════════════════════════════════════════════════════════
// LCD
// ═════════════════════════════════════════════════════════════════════
void lcdMsg(String l1, String l2) {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print(l1.substring(0,16));
  lcd.setCursor(0,1); lcd.print(l2.substring(0,16));
}
void updateLcdForState() {
  if(bikeState==STATE_IDLE)           lcdMsg("KuetTrack Ready","Tap RFID card");
  else if(bikeState==STATE_AUTH_PENDING) lcdMsg("Verifying...","Please wait");
  else if(bikeState==STATE_RFID_VERIFIED){
    static bool tog=false;tog=!tog;
    if(tog) lcdMsg("Card verified!",currentUserName.substring(0,16));
    else    lcdMsg("Open app to","start your ride");
  } else if(bikeState==STATE_RIDE_ACTIVE){
    lcd.clear();
    lcd.setCursor(0,0);lcd.print((">" + currentUserName).substring(0,16));
    lcd.setCursor(0,1);
    if(gps.location.isValid()){char b[17];snprintf(b,17,"%.4f,%.4f",gps.location.lat(),gps.location.lng());lcd.print(b);}
    else{char b[17];snprintf(b,17,"GPS... sats:%u",(unsigned)gps.satellites.value());lcd.print(b);}
  }
}

// ═════════════════════════════════════════════════════════════════════
// WiFi
// ═════════════════════════════════════════════════════════════════════
void connectToWiFi() {
  if(WiFi.status()==WL_CONNECTED) return;
  Serial.print("[WiFi] Connecting...");
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int a=0;
  while(WiFi.status()!=WL_CONNECTED&&a<30){drainGPS();delay(400);Serial.print(".");a++;}
  if(WiFi.status()==WL_CONNECTED) Serial.println("\n[WiFi] "+WiFi.localIP().toString());
  else Serial.println("\n[WiFi] Failed");
}

// ═════════════════════════════════════════════════════════════════════
// RFID helpers
// ═════════════════════════════════════════════════════════════════════
String getRfidUid() {
  String uid="";
  for(byte i=0;i<rfid.uid.size;i++){if(rfid.uid.uidByte[i]<0x10)uid+="0";uid+=String(rfid.uid.uidByte[i],HEX);}
  uid.toUpperCase(); return uid;
}
void reinitRFID() {
  delay(100);rfid.PCD_Init();delay(100);
  rfid.PCD_AntennaOn();rfid.PCD_WriteRegister(MFRC522::RFCfgReg,0x3F);delay(50);
}
