#include <SPI.h>
#include <MFRC522.h>
#include <WiFiNINA.h>
#include <Servo.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

#define SS_PIN 11
#define RST_PIN 4
#define SERVO_PIN 7
#define IR_SENSOR_PIN 0
#define PIR_PIN 2
#define VIBRATION_PIN A2
#define BUZZER_PIN 6

MFRC522 rfid(SS_PIN, RST_PIN);
Servo doorServo;

char ssid[] = "Fairy";
char pass[] = "1608z246<3";

const char* influxHost = "10.197.56.165";
const int influxPort = 8086;
String org = "MDX";
String bucket = "Prison_Data";
String measurementName = "control_room";
String token = "qjY1cFVn4YcnpI3nmYZT454K9HHjDFaEKJ3ftgQqbg3sCkZcHKv8dD7ZHGhiOxm4cmYQ4viEEbNqQ_l8yiv5OA==";

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);

// =================== SEPARATE TIMING FOR DIFFERENT TASKS ===================
const unsigned long INFLUX_INTERVAL_MS = 5000UL;    // Send to InfluxDB every 5 seconds
unsigned long lastInfluxUpdate = 0;

const unsigned long SENSOR_CHECK_MS = 300UL;        // Check sensors every 300ms
unsigned long lastSensorCheck = 0;

volatile bool irTriggered = false;
volatile unsigned long lastIRTime = 0;
const unsigned long irDebounce = 500;

int peopleCount = 0;
bool doorOpen = false;
unsigned long doorOpenTime = 0;
bool countingActive = false;
String lastGuardName = "Nobody";

String currentDoorMessage = "CLOSED";
unsigned long deniedMessageUntil = 0;
const unsigned long DENIED_DISPLAY_TIME = 5000;

int vibThreshold = 300;
bool fenceAlertActive = false;
unsigned long lastFenceTrigger = 0;
const unsigned long fenceCooldown = 10000;

WiFiClient client; // kept as-is but not reused for network calls to avoid socket corruption

// =================== AUTHORIZED CARDS ===================
String validUID_Level2[] = {"13B7750F", "ABCD1234"};  // Senior guards → can open
String validUID_Level1[] = {"233400F8", "93F84E2A"};  // Normal guards → denied

void ensureWiFi() {
  // If already connected, nothing to do
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("WiFi disconnected, reconnecting...");

  // Hard-reset WiFi module state to avoid stuck state
  WiFi.disconnect();
  delay(250);

  WiFi.begin(ssid, pass);

  // Quick timeout to avoid blocking RFID too long
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(200);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi reconnected!");
  } else {
    Serial.println("\nWiFi connection failed - will retry in background");
  }
}

String readClientResponse(WiFiClient &c) {
  String resp = "";
  unsigned long start = millis();
  while (c.connected() || c.available()) {
    while (c.available()) resp += (char)c.read();
    if (millis() - start > 1000) break;  // Reduced from 2000ms to keep non-blocking
  }
  return resp;
}

// First, modify your Arduino code to test basic connectivity
// Add this test function and call it in setup:
void testInfluxWrite() {
  Serial.println("\n=== TEST INFLUX WRITE ===");

  String testData = measurementName + ",device=prison_mkr1010 test_value=123i";

  if (sendLineProtocol(testData)) {
    Serial.println("Test write succeeded!");
  } else {
    Serial.println("Test write failed!");
  }

  // Also test with curl-like command
  Serial.println("\nCurl command to test:");
  Serial.print("curl --request POST \"http://");
  Serial.print(influxHost);
  Serial.print(":8086/api/v2/write?org=");
  Serial.print(org);
  Serial.print("&bucket=");
  Serial.print(bucket);
  Serial.print("&precision=s\" \\\n");
  Serial.print("  --header \"Authorization: Token ");
  Serial.print(token);
  Serial.print("\" \\\n");
  Serial.print("  --header \"Content-Type: text/plain\" \\\n");
  Serial.print("  --data \"");
  Serial.print(testData);
  Serial.println(" $(date +%s)\"");
}

void testInfluxDBConnection() {
  Serial.println("\n=== Testing InfluxDB Connection ===");

  // Test 1: Check if we can reach the IP
  Serial.print("Pinging ");
  Serial.print(influxHost);
  Serial.print(":");
  Serial.print(influxPort);
  Serial.println("...");

  WiFiClient c; // local client for test
  if (c.connect(influxHost, influxPort)) {
    Serial.println("SUCCESS: Connected to port 8086");

    // Send a simple HTTP request
    c.println("GET / HTTP/1.1");
    c.println("Host: " + String(influxHost));
    c.println("Connection: close");
    c.println();

    unsigned long start = millis();
    while (!c.available() && millis() - start < 1000) delay(10);

    // Read response
    String response = "";
    while (c.available()) {
      response += (char)c.read();
    }

    Serial.println("Response:");
    if (response.length() > 0) Serial.println(response.substring(0, min(200, response.length()))); // First 200 chars
    else Serial.println("(no response body)");

    c.stop();

    if (response.indexOf("InfluxDB") != -1 || response.indexOf("Grafana") != -1 || response.indexOf("HTTP/1.1") != -1) {
      Serial.println("✓ InfluxDB (or HTTP) is responding!");
    } else {
      Serial.println("✗ Got response but not recognizable as InfluxDB HTTP banner");
    }
  } else {
    Serial.println("FAILED: Cannot connect to InfluxDB");
    Serial.println("Possible reasons:");
    Serial.println("1. InfluxDB not running");
    Serial.println("2. Wrong IP address");
    Serial.println("3. Firewall blocking port 8086");
    Serial.println("4. Server not on same network");
  }
}

bool sendLineProtocol(String lineProtocol) {
  Serial.println("\n[InfluxDB] Attempting to send data...");

  // Check WiFi
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[InfluxDB] ERROR: No WiFi connection");
    return false;
  }

  // Use a fresh local client for each request to avoid socket corruption
  WiFiClient c;

  Serial.print("[InfluxDB] Connecting to ");
  Serial.print(influxHost);
  Serial.print(":");
  Serial.println(influxPort);

  if (!c.connect(influxHost, influxPort)) {
    Serial.println("[InfluxDB] ERROR: Connection failed");
    return false;
  }

  Serial.println("[InfluxDB] Connected!");

  // Get timestamp
  timeClient.update();
  unsigned long unixTime = timeClient.getEpochTime();
  String data = lineProtocol + " " + String(unixTime);

  Serial.println("[InfluxDB] Sending: " + data);

  // Build and send HTTP request in small prints (avoid one huge String where possible)
  String url = "/api/v2/write?org=" + org + "&bucket=" + bucket + "&precision=s";
  c.print(String("POST ") + url + " HTTP/1.1\r\n");
  c.print(String("Host: ") + String(influxHost) + ":" + String(influxPort) + "\r\n");
  c.print(String("Authorization: Token ") + token + "\r\n");
  c.print("Content-Type: text/plain; charset=utf-8\r\n");
  c.print("Content-Length: " + String(data.length()) + "\r\n");
  c.print("Connection: close\r\n\r\n");
  c.print(data);

  Serial.println("[InfluxDB] Request sent, waiting for response...");

  // Wait for response with timeout
  unsigned long timeout = millis() + 3000;
  while (!c.available() && millis() < timeout) {
    delay(10);
  }

  // Read response
  String response = "";
  if (c.available()) {
    response = c.readString();
    Serial.print("[InfluxDB] Raw response: ");
    Serial.println(response);
  } else {
    Serial.println("[InfluxDB] No response (timeout)");
  }

  c.stop();

  // Check for success - 204 No Content is success
  bool success = false;
  if (response.length() > 0) {
    // Look for HTTP status in first line
    int firstLineEnd = response.indexOf('\r');
    String firstLine = (firstLineEnd != -1) ? response.substring(0, firstLineEnd) : response;
    Serial.print("[InfluxDB] First line: ");
    Serial.println(firstLine);

    if (firstLine.indexOf("204") != -1) success = true;

    // Also check headers for Influx server
    if (response.indexOf("X-Influxdb-Version") != -1) {
      Serial.println("[InfluxDB] Confirmed InfluxDB response");
    }
  }

  if (success) {
    Serial.println("[InfluxDB] ✓ SUCCESS: Data written!");
    return true;
  } else {
    Serial.println("[InfluxDB] ✗ FAILED to write data");
    return false;
  }
}

void irISR() {
  unsigned long now = millis();
  if (now - lastIRTime > irDebounce) {
    lastIRTime = now;
    irTriggered = true;
  }
}

void buzzerLevel2Granted() {
  for (int i = 0; i < 2; i++) {
    digitalWrite(BUZZER_PIN, LOW); delay(150);
    digitalWrite(BUZZER_PIN, HIGH); delay(150);
  }
  digitalWrite(BUZZER_PIN, HIGH);
}

void buzzerLevel1Denied() {
  for (int i = 0; i < 8; i++) {
    digitalWrite(BUZZER_PIN, LOW); delay(100);
    digitalWrite(BUZZER_PIN, HIGH); delay(100);
  }
  digitalWrite(BUZZER_PIN, HIGH);
}

void buzzerUnknownCard() {
  digitalWrite(BUZZER_PIN, LOW); delay(3000); digitalWrite(BUZZER_PIN, HIGH);
}

void openDoor() {
  if (doorOpen) return;
  Serial.println("DOOR OPENING");
  doorServo.write(90);
  doorOpen = true;
  doorOpenTime = millis();
  peopleCount = 0;
  countingActive = true;
}

void manageDoor() {
  if (doorOpen && (millis() - doorOpenTime >= 5000)) {
    Serial.println("DOOR CLOSING");
    doorServo.write(0);
    doorOpen = false;
    countingActive = false;
    currentDoorMessage = "CLOSED";
    peopleCount = 0;
  }
}

// =================== FIXED RFID FUNCTION ===================
void checkRFID() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  // Build UID
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    uid += (rfid.uid.uidByte[i] < 0x10 ? "0" : "");
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();

  Serial.println("RFID Detected: " + uid);

  String name = "Unknown";
  int level = 0;

  // Level 2 first (higher privilege)
  for (String id : validUID_Level2) {
    if (uid.indexOf(id) != -1) {
      level = 2;
      name = "Senior:" + uid;
      lastGuardName = name;
      Serial.println("LEVEL 2 AUTHORIZED");
      break;
    }
  }

  // Then Level 1
  if (level == 0) {
    for (String id : validUID_Level1) {
      if (uid.indexOf(id) != -1) {
        level = 1;
        name = "Guard:" + uid;
        lastGuardName = name;
        Serial.println("LEVEL 1 DETECTED");
        break;
      }
    }
  }

  // Access decision
  if (level == 2) {
    currentDoorMessage = "OPENED by " + name;
    deniedMessageUntil = 0;
    openDoor();
    buzzerLevel2Granted();
  }
  else if (level == 1) {
    Serial.println("ACCESS DENIED - LEVEL 1");
    currentDoorMessage = "LEVEL 1 DENIED";
    deniedMessageUntil = millis() + DENIED_DISPLAY_TIME;
    buzzerLevel1Denied();
  }
  else {
    Serial.println("ACCESS DENIED - UNKNOWN CARD");
    currentDoorMessage = "ACCESS DENIED";
    deniedMessageUntil = millis() + DENIED_DISPLAY_TIME;
    buzzerUnknownCard();
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  delay(1000);
}

void handleIRCounting() {
  if (irTriggered && countingActive && doorOpen) {
    irTriggered = false;
    peopleCount++;
    Serial.println("Person counted: " + String(peopleCount));
  }
}

void checkFenceSensors() {
  int pirVal = digitalRead(PIR_PIN);
  int vibVal = analogRead(VIBRATION_PIN);

  if (pirVal == HIGH && vibVal > vibThreshold) {
    if (!fenceAlertActive && (millis() - lastFenceTrigger > fenceCooldown)) {
      fenceAlertActive = true;
      lastFenceTrigger = millis();
      Serial.println("FENCE ALERT!");
    }
  } else if (fenceAlertActive && (millis() - lastFenceTrigger > 5000)) {
    fenceAlertActive = false;
    Serial.println("Fence alert cleared");
  }
}

void verifyInfluxData() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 30000) return; // Check every 30 seconds
  lastCheck = millis();

  Serial.println("\n[Verify] Checking if data exists in InfluxDB...");

  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClient c; // local client
  if (c.connect(influxHost, influxPort)) {
    // Query for recent data
    String query = "from(bucket:\"" + bucket + "\") |> range(start: -5m) |> filter(fn: (r) => r._measurement == \"" + measurementName + "\") |> last()";

    String request = "POST /api/v2/query?org=" + org + " HTTP/1.1\r\n";
    request += "Host: " + String(influxHost) + ":" + String(influxPort) + "\r\n";
    request += "Authorization: Token " + token + "\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + String(query.length() + 20) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += "{\"query\":\"" + query + "\"}";

    c.print(request);

    unsigned long start = millis();
    while (!c.available() && millis() - start < 1500) delay(10);

    String response = "";
    while (c.available()) {
      response += (char)c.read();
    }

    c.stop();

    if (response.length() > 0) {
      if (response.indexOf("prison_mkr1010") != -1) {
        Serial.println("[Verify] ✓ Data found in InfluxDB!");
        // Extract and print some data
        int dataStart = response.indexOf("\"values\":");
        if (dataStart != -1) {
          int dataEnd = response.indexOf("]", dataStart);
          if (dataEnd != -1 && dataEnd - dataStart < 500) {
            String data = response.substring(dataStart, dataEnd + 1);
            Serial.println("[Verify] Latest data: " + data);
          }
        }
      } else if (response.indexOf("error") != -1) {
        Serial.println("[Verify] Query error: " + response);
      } else {
        Serial.println("[Verify] No data yet (might be first run)");
      }
    } else {
      Serial.println("[Verify] No response to query");
    }
  } else {
    Serial.println("[Verify] ERROR: cannot connect to InfluxDB for verify");
  }
}

void sendToInfluxDB() {
  if (deniedMessageUntil != 0 && millis() >= deniedMessageUntil) {
    if (!doorOpen) currentDoorMessage = "CLOSED";
    deniedMessageUntil = 0;
  }

  int doorState = 0;
  if (currentDoorMessage.indexOf("OPENED") != -1) doorState = 1;
  else if (currentDoorMessage.indexOf("DENIED") != -1) doorState = -1;

  int pirVal = digitalRead(PIR_PIN);
  int vibVal = analogRead(VIBRATION_PIN);

  String lp = measurementName + ",device=prison_mkr1010 "
              "people_count=" + String(peopleCount) + "i,"
              "door_state=" + String(doorState) + "i,"
              "door_open=" + String(doorOpen ? 1 : 0) + "i,"
              "fence_alert=" + String(fenceAlertActive ? 1 : 0) + "i,"
              "pir_value=" + String(pirVal) + "i,"
              "vib_value=" + String(vibVal) + "i,"
              "vib_threshold=" + String(vibThreshold) + "i";

  sendLineProtocol(lp);
}

void setup() {
  Serial.begin(9600);
  delay(2000);
  Serial.println("\n=== PRISON RFID SYSTEM STARTING ===");

  SPI.begin();
  rfid.PCD_Init();
  Serial.println("RFID initialized");

  doorServo.attach(SERVO_PIN);
  doorServo.write(0);
  Serial.println("Door servo initialized");

  pinMode(IR_SENSOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(IR_SENSOR_PIN), irISR, FALLING);
  Serial.println("IR sensor initialized");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH);

  pinMode(PIR_PIN, INPUT);
  pinMode(VIBRATION_PIN, INPUT);

  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, pass);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
  } else {
    Serial.println("\nWiFi connection failed - will retry in background");
  }
  testInfluxDBConnection();

  timeClient.begin();
  timeClient.update();

  // Initial InfluxDB write
  sendLineProtocol(measurementName + ",device=prison_mkr1010 people_count=0i,door_open=0i,fence_alert=0i,pir_value=0i,vib_value=0i,vib_threshold=" + String(vibThreshold) + "i");

  lastInfluxUpdate = millis();
  lastSensorCheck = millis();

  Serial.println("=== SYSTEM READY ===\n");
}
void logToSerial() {
  static unsigned long lastLog = 0;
  if (millis() - lastLog > 5000) {
    lastLog = millis();

    Serial.println("\n=== SENSOR DATA ===");
    Serial.println("Timestamp: " + String(millis() / 1000) + "s");
    Serial.println("People Count: " + String(peopleCount));
    Serial.println("Door: " + String(doorOpen ? "OPEN" : "CLOSED"));
    Serial.println("Fence Alert: " + String(fenceAlertActive ? "ACTIVE" : "OK"));
    Serial.println("PIR: " + String(digitalRead(PIR_PIN)));
    Serial.println("Vibration: " + String(analogRead(VIBRATION_PIN)));
    Serial.println("WiFi: " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected"));
    Serial.println("===================\n");
  }
}
void loop() {
  // CRITICAL: Check RFID EVERY loop iteration - NO DELAYS!
  checkRFID();

  // Always manage door
  manageDoor();


  // Check sensors frequently (every 300ms)
  if (millis() - lastSensorCheck >= SENSOR_CHECK_MS) {
    lastSensorCheck = millis();
    handleIRCounting();
    checkFenceSensors();
  }
  // Send to Influx every 5 seconds
  if (millis() - lastInfluxUpdate >= INFLUX_INTERVAL_MS) {
    lastInfluxUpdate = millis();
    sendToInfluxDB();
  }


  logToSerial();
  verifyInfluxData();

  delay(10);

}
