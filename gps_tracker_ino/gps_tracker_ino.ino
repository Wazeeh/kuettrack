#include <TinyGPS++.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ═══════════════════════════════════════════════════════
// GPS TRACKER — ESP32 + NEO-6M + WiFi  (KuetTrack)
// ═══════════════════════════════════════════════════════

// ─── WiFi Configuration ───────────────────────────────
const char* SSID       = "KJAH_201";
const char* PASSWORD   = "36140343";
const char* SERVER_URL = "http://192.168.0.103:5000/api/gps/update";

// ─── GPS Configuration ────────────────────────────────
TinyGPSPlus gps;
HardwareSerial gpsSerial(2);   // UART2: RX=GPIO16, TX=GPIO17

// ─── RFID Card UID — must match assigned card in DB ───
const char* RFID_UID = "C33B51FE";

// ─── Timing ───────────────────────────────────────────
unsigned long lastSendTime         = 0;
unsigned long lastConnectionCheck  = 0;
unsigned long lastNoFixWarning     = 0;
unsigned long rawBytesReceived     = 0;

const unsigned long SEND_INTERVAL        = 5000;   // Send every 5 s when fix OK
const unsigned long NO_FIX_SEND_INTERVAL = 15000;  // Keepalive every 15 s (no fix)
const unsigned long CONNECTION_CHECK     = 10000;  // WiFi check every 10 s
const unsigned long NO_FIX_WARN_INTERVAL = 5000;   // Serial warning every 5 s

bool hadFix = false;

void connectToWiFi();
void sendGPSData(bool hasFix);
void printGpsStatus();

// ═══════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n╔═══════════════════════════════════════════════════╗");
  Serial.println("║  KuetTrack — ESP32 GPS Tracker (NEO-6M)          ║");
  Serial.println("╚═══════════════════════════════════════════════════╝");

  // NEO-6M default baud = 9600
  gpsSerial.begin(9600, SERIAL_8N1, 16, 17);
  Serial.println("✓ GPS UART2 started (RX=GPIO16, TX=GPIO17)");
  Serial.println("  Wiring: NEO-6M TX → GPIO16 | NEO-6M RX → GPIO17");

  connectToWiFi();

  Serial.println("\n✓ Setup complete. Waiting for NEO-6M NMEA data…");
  Serial.println("  (Cold-start fix: 30 s – 3 min. Go outdoors!)\n");
}

// ═══════════════════════════════════════════════════════
void loop() {
  // Feed all available GPS bytes into TinyGPS++
  while (gpsSerial.available() > 0) {
    char c = gpsSerial.read();
    gps.encode(c);
    rawBytesReceived++;
  }

  // Periodic WiFi reconnect check
  if (millis() - lastConnectionCheck > CONNECTION_CHECK) {
    lastConnectionCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("⚠ WiFi dropped — reconnecting…");
      connectToWiFi();
    }
  }

  // No NMEA bytes at all → wiring/power problem
  if (rawBytesReceived == 0 && millis() > 5000) {
    if (millis() - lastNoFixWarning > NO_FIX_WARN_INTERVAL) {
      lastNoFixWarning = millis();
      Serial.println("✗ NO NMEA bytes from NEO-6M! Check wiring/power.");
    }
    return;
  }

  // NMEA bytes received but no satellite fix yet
  if (!gps.location.isValid()) {
    if (millis() - lastNoFixWarning > NO_FIX_WARN_INTERVAL) {
      lastNoFixWarning = millis();
      Serial.printf("⏳ Waiting for fix… bytes=%lu sats=%u\n",
                    rawBytesReceived, gps.satellites.value());
    }
    // Send keepalive (no-fix) so dashboard shows "GPS searching"
    if (millis() - lastSendTime > NO_FIX_SEND_INTERVAL) {
      sendGPSData(false);
      lastSendTime = millis();
    }
    return;
  }

  // Valid fix acquired
  if (!hadFix) {
    hadFix = true;
    Serial.println("🛰️  GPS FIX ACQUIRED!");
    printGpsStatus();
  }

  // Time-based send — do NOT rely on isUpdated() alone
  // isUpdated() fires only once per NMEA parse cycle and can be missed
  if (millis() - lastSendTime > SEND_INTERVAL) {
    printGpsStatus();
    sendGPSData(true);
    lastSendTime = millis();
  }
}

// ═══════════════════════════════════════════════════════
void connectToWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("🔌 Connecting to: %s\n", SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500); Serial.print("."); attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n✓ WiFi connected! ESP32 IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.println("─────────────────────────────────────────────────");
    Serial.println("  ⚠️  Make sure SERVER_URL matches your PC LAN IP.");
    Serial.printf( "  Current SERVER_URL: %s\n", SERVER_URL);
    Serial.println("  Run \'node server.js\' — it will print your PC IP.");
    Serial.println("─────────────────────────────────────────────────");
  } else {
    Serial.println("\n✗ WiFi failed. Check SSID/PASSWORD and retry.");
  }
}

// ═══════════════════════════════════════════════════════
void printGpsStatus() {
  Serial.println("──── GPS DATA ───────────────────────────────────");
  Serial.printf("  Lat:  %.6f  Lon: %.6f\n",
                gps.location.lat(), gps.location.lng());
  Serial.printf("  Speed: %.1f km/h  Alt: %.1f m  Sats: %u\n",
                gps.speed.kmph(), gps.altitude.meters(), gps.satellites.value());
  Serial.println("─────────────────────────────────────────────────");
}

// ═══════════════════════════════════════════════════════
void sendGPSData(bool hasFix) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("✗ No WiFi — skipping send.");
    return;
  }

  DynamicJsonDocument doc(256);
  doc["deviceId"]   = RFID_UID;
  doc["hasFix"]     = hasFix;
  doc["satellites"] = (int)gps.satellites.value();

  if (hasFix) {
    doc["lat"]      = gps.location.lat();
    doc["lon"]      = gps.location.lng();
    doc["speed"]    = gps.speed.kmph();
    doc["altitude"] = gps.altitude.meters();
  } else {
    // Omit lat/lon when no fix so server ignores coordinates
    doc["lat"]      = 0.0;
    doc["lon"]      = 0.0;
    doc["speed"]    = 0.0;
    doc["altitude"] = 0.0;
  }

  String body;
  serializeJson(doc, body);

  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(8000);   // Don't hang if server is unreachable

  int code = http.POST(body);
  if (code > 0) {
    Serial.printf("✓ Server %d  (hasFix=%s)\n", code, hasFix ? "true" : "false");
  } else {
    Serial.printf("✗ HTTP error %d\n", code);
    Serial.println("  Causes:");
    Serial.println("  1. Wrong IP — run \'node server.js\' to see your PC LAN IP");
    Serial.printf( "  2. Server not running at %s\n", SERVER_URL);
    Serial.println("  3. Windows Firewall blocking port 5000");
    Serial.println("     Fix: run as Admin: netsh advfirewall firewall add rule name=\"KuetTrack\" dir=in action=allow protocol=TCP localport=5000");
    Serial.println("  4. ESP32 and PC on different WiFi networks");
  }
  http.end();
}
