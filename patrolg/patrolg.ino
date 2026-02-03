#include <WiFiNINA.h>
#include <DHT22.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// WiFi credentials
char ssid[] = "Fairy";
char pass[] = "1608z246<3";

// Pin definitions
#define buzzPin 9
#define flowPin A0
#define rledPin 2
#define gledPin 3
#define bledPin A3
#define gasPin A1
#define tem_huPin A2
#define pirPin 7

// Sensor objects
DHT22 dht22(tem_huPin);

// InfluxDB 2.x configuration
const char* influxHost = "10.197.56.165";  // CHANGE TO YOUR INFLUXDB IP
int influxPort = 8086;
String org = "MDX";
String bucket = "Prison_Data";
String measurementName = "prison_sensors";
String token = "_OTXDBpW35G-H70RjyKn2QHZAlwcSU7mETHX3vmn-7dnD1WAoXnC8jrzForcxTz7HJ6DZpQUUXn_alcPEYgE2Q==";

// NTP Client for timestamps
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");

// Thresholds
int waterThreshold = 200;

WiFiClient influxClient;

// ====== WIFI CONNECTION ======

void connectWiFi() {
  Serial.print(F("[WiFi] Connecting to "));
  Serial.println(ssid);
  
  WiFi.begin(ssid, pass);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("\n[WiFi] Connected!"));
    Serial.print(F("[WiFi] IP Address: "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("\n[WiFi] Connection failed!"));
  }
}

// ====== INFLUXDB HELPER FUNCTIONS ======

String readClientResponse(WiFiClient &c) {
  String resp = "";
  unsigned long start = millis();
  while (c.connected() || c.available()) {
    while (c.available()) {
      char ch = c.read();
      resp += ch;
      if (resp.length() > 8000) {
        resp += "\n[TRUNCATED]";
        return resp;
      }
    }
    if (millis() - start > 2000) break;
  }
  return resp;
}

bool measurementExists() {
  Serial.println(F("[InfluxDB] Checking if measurement exists..."));
  
  if (!influxClient.connect(influxHost, influxPort)) {
    Serial.println(F("[InfluxDB] ✗ Cannot connect for measurement check"));
    return false;
  }
  
  Serial.println(F("[InfluxDB] ✓ Connected for measurement check"));

  String flux = "{ \"query\": \"from(bucket: '" + bucket + "')"
                " |> range(start: -30d)"
                " |> filter(fn: (r) => r._measurement == '" + measurementName + "')"
                " |> limit(n:1)\" }";

  influxClient.println("POST /api/v2/query HTTP/1.1");
  influxClient.print("Host: "); influxClient.println(influxHost);
  influxClient.print("Authorization: Token "); influxClient.println(token);
  influxClient.println("Accept: application/csv");
  influxClient.println("Content-Type: application/json");
  influxClient.print("Content-Length: "); influxClient.println(flux.length());
  influxClient.println();
  influxClient.print(flux);

  delay(300);
  String resp = readClientResponse(influxClient);
  influxClient.stop();

  if (resp.indexOf(measurementName) != -1) {
    Serial.println(F("[InfluxDB] Measurement exists"));
    return true;
  }

  Serial.println(F("[InfluxDB] Measurement does not exist"));
  return false;
}

bool createMeasurement() {
  Serial.println(F("[InfluxDB] Creating measurement..."));
  
  if (!influxClient.connect(influxHost, influxPort)) {
    Serial.println(F("[InfluxDB] ✗ Cannot connect for dummy write"));
    return false;
  }

  timeClient.update();
  unsigned long unixTime = timeClient.getEpochTime();

  String data = measurementName + " temperature=0 " + String(unixTime);
  String url = "/api/v2/write?org=" + org + "&bucket=" + bucket + "&precision=s";

  influxClient.print("POST " + url + " HTTP/1.1\r\n");
  influxClient.print("Host: " + String(influxHost) + "\r\n");
  influxClient.print("Authorization: Token " + token + "\r\n");
  influxClient.print("Content-Type: text/plain; charset=utf-8\r\n");
  influxClient.print("Content-Length: " + String(data.length()) + "\r\n");
  influxClient.print("\r\n");
  influxClient.print(data);

  delay(300);
  String resp = readClientResponse(influxClient);
  influxClient.stop();

  if (resp.indexOf("204") != -1 || resp.length() == 0) {
    Serial.println(F("[InfluxDB] ✓ Dummy write successful"));
    return true;
  }
  
  Serial.println(F("[InfluxDB] ✗ Dummy write failed"));
  Serial.println(resp);
  return false;
}

bool sendToInflux(float temp, float hum, int gasVal, int waterVal, int pirState, int alertState) {
  Serial.println(F("\n--- InfluxDB Write ---"));
  Serial.print(F("Connecting to: "));
  Serial.print(influxHost);
  Serial.print(F(":"));
  Serial.println(influxPort);
  
  if (!influxClient.connect(influxHost, influxPort)) {
    Serial.println(F("✗ Connection FAILED"));
    return false;
  }
  
  Serial.println(F("✓ Connected"));

  timeClient.update();
  unsigned long unixTime = timeClient.getEpochTime();
  Serial.print(F("Timestamp: "));
  Serial.println(unixTime);

  // Build line protocol with all your data including alert state
  String data = measurementName;
  data += " temperature=" + String(temp, 2);
  data += ",humidity=" + String(hum, 2);
  data += ",gas=" + String(gasVal);
  data += ",water=" + String(waterVal);
  data += ",motion=" + String(pirState);
  data += ",alert_state=" + String(alertState);
  data += " " + String(unixTime);

  Serial.println(F("Data: "));
  Serial.println(data);

  String url = "/api/v2/write?org=" + org + "&bucket=" + bucket + "&precision=s";

  influxClient.print("POST " + url + " HTTP/1.1\r\n");
  influxClient.print("Host: " + String(influxHost) + "\r\n");
  influxClient.print("Authorization: Token " + token + "\r\n");
  influxClient.print("Content-Type: text/plain; charset=utf-8\r\n");
  influxClient.print("Content-Length: " + String(data.length()) + "\r\n");
  influxClient.print("\r\n");
  influxClient.print(data);

  delay(500);
  String resp = readClientResponse(influxClient);
  influxClient.stop();

  Serial.println(F("Response:"));
  if (resp.length() > 0) {
    Serial.println(resp.substring(0, min(500, (int)resp.length())));
  } else {
    Serial.println(F("(empty - usually means success)"));
  }

  if (resp.indexOf("204") != -1 || resp.length() == 0) {
    Serial.println(F("✓ WRITE SUCCESS"));
    return true;
  } else if (resp.indexOf("401") != -1) {
    Serial.println(F("✗ ERROR 401: Invalid token"));
    return false;
  } else if (resp.indexOf("404") != -1) {
    Serial.println(F("✗ ERROR 404: Bucket or org not found"));
    return false;
  } else {
    Serial.println(F("✗ WRITE FAILED"));
    return false;
  }
}

// ====== SETUP ======

void setup() {
  Serial.begin(9600);
  delay(2000);
  
  Serial.println(F("\n=== Patrol Guard System Starting ==="));
  
  // Initialize pins
  pinMode(rledPin, OUTPUT);
  pinMode(gledPin, OUTPUT);
  pinMode(bledPin, OUTPUT);
  pinMode(buzzPin, OUTPUT);
  pinMode(pirPin, INPUT);
  pinMode(gasPin, INPUT);
  pinMode(flowPin, INPUT);
  
  digitalWrite(buzzPin, HIGH);

  // Connect to WiFi
  connectWiFi();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("\n[NTP] Initializing time..."));
    timeClient.begin();
    timeClient.update();
    Serial.print(F("[NTP] Epoch time: "));
    Serial.println(timeClient.getEpochTime());

    // Check/create InfluxDB measurement
    bool exists = measurementExists();
    if (!exists) {
      createMeasurement();
    }
  } else {
    Serial.println(F("[ERROR] WiFi not connected - InfluxDB unavailable"));
  }

  Serial.println(F("\nCalibrating PIR sensor..."));
  delay(30000);
  Serial.println(F("PIR sensor ready!"));
  Serial.println(F("=== System Ready ===\n"));
}

// ====== MAIN LOOP ======

void loop() {
  // Read all sensors
  int gasValue = analogRead(gasPin);
  int waterValue = analogRead(flowPin);
  int presence = digitalRead(pirPin);
  int codeState = 0;

  float t = dht22.getTemperature();
  float h = dht22.getHumidity();

  // Print sensor readings
  Serial.print("Water: ");
  Serial.print(waterValue);
  Serial.print(" | Gas: ");
  Serial.print(gasValue);
  Serial.print(" | Temp: ");
  Serial.print(t, 1);
  Serial.print("°C | Humidity: ");
  Serial.print(h, 1);
  Serial.print("% | Motion: ");
  Serial.println(presence ? "YES" : "NO");

  // Determine alert state
  if (presence == LOW && waterValue < waterThreshold) {
    codeState = 1;  // Escape attempt
  }
  else if (t > 35) {
    codeState = 2;  // Fire risk
  }
  else if (gasValue > 500) {
    codeState = 3;  // Gas only
  }

  // Send data to InfluxDB with debug output
  if (WiFi.status() == WL_CONNECTED) {
    sendToInflux(t, h, gasValue, waterValue, presence, codeState);
  } else {
    Serial.println(F("[ERROR] WiFi disconnected!"));
  }

  // Handle LED and buzzer based on state
  if (codeState == 0) {
    // Normal state - white LED
    analogWrite(rledPin, 255);
    analogWrite(gledPin, 255);
    analogWrite(bledPin, 255);

    digitalWrite(buzzPin, LOW);
    delay(250);
    digitalWrite(buzzPin, HIGH);
    delay(1000);
  }
  else {
    // Problem detected - handle with switch
    switch (codeState) {
      case 1:  // Escape attempt
        analogWrite(rledPin, 0);
        analogWrite(gledPin, 0);
        analogWrite(bledPin, 255);  // Blue
        
        digitalWrite(buzzPin, LOW);
        delay(100);
        digitalWrite(buzzPin, HIGH);
        delay(100);
        digitalWrite(buzzPin, LOW);
        delay(250);
        digitalWrite(buzzPin, HIGH);
        delay(250);
        break;

      case 2:  // Fire risk
        analogWrite(rledPin, 255);
        analogWrite(gledPin, 0);
        analogWrite(bledPin, 0);  // Red
        
        digitalWrite(buzzPin, LOW);
        delay(2000);
        digitalWrite(buzzPin, HIGH);
        delay(500);
        digitalWrite(buzzPin, LOW);
        delay(4000);
        digitalWrite(buzzPin, HIGH);
        delay(500);
        break;

      case 3:  // Gas detected
        analogWrite(rledPin, 255);
        analogWrite(gledPin, 255);
        analogWrite(bledPin, 0);  // Yellow
        
        digitalWrite(buzzPin, LOW);
        delay(1000);
        digitalWrite(buzzPin, HIGH);
        delay(1000);
        digitalWrite(buzzPin, LOW);
        delay(1000);
        digitalWrite(buzzPin, HIGH);
        delay(1000);
        break;
    }
  }
  
  delay(2000);
}