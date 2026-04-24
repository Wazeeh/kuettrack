// =====================================================
// NEO-6M Baud Rate Scanner
// Tries 4800, 9600, 19200, 38400, 57600, 115200
// Watch Serial Monitor — correct baud will show
// readable $GPRMC/$GPGGA sentences and high byte count
// =====================================================

HardwareSerial gpsSerial(2);

const long bauds[] = {4800, 9600, 19200, 38400, 57600, 115200};
const int  numBauds = 6;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== NEO-6M Baud Rate Scanner ===\n");
}

void loop() {
  for (int i = 0; i < numBauds; i++) {
    long baud = bauds[i];
    gpsSerial.begin(baud, SERIAL_8N1, 16, 17);
    delay(100);

    // Flush any garbage
    while (gpsSerial.available()) gpsSerial.read();
    delay(1100);  // wait 1 full second for NEO-6M to send

    int count = 0;
    String sample = "";
    while (gpsSerial.available()) {
      char c = gpsSerial.read();
      count++;
      if (sample.length() < 80) sample += c;
    }

    Serial.printf("Baud %6ld → %3d bytes  ", baud, count);
    if (count > 20) {
      Serial.println("<<< LIKELY CORRECT BAUD >>>");
      Serial.println("  Sample: " + sample);
    } else if (count > 0) {
      Serial.println("some bytes (partial/garbage)");
    } else {
      Serial.println("no bytes");
    }

    gpsSerial.end();
    delay(200);
  }

  Serial.println("\n--- scan complete, repeating ---\n");
  delay(3000);
}
