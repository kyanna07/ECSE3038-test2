// ECSE3038 Test 2: the bathroom fan (board)
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

#if __has_include("secrets.h")
#include "secrets.h"       
#endif
#ifndef DEVICE_KEY
#define DEVICE_KEY ""
#endif

#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASSWORD ""
#define WIFI_CHANNEL 6

const char *API_URL = "http://iot-kknight.northcentralus.cloudapp.azure.com/readings";
const char *MQTT_NAME = "kyanna";
const char *BROKER_HOST = "135.232.203.144";
const uint16_t BROKER_PORT = 1883;

const int DHT_PIN = 15;
const int FAN_PIN = 2;
const unsigned long POST_EVERY_MS = 10000;   // B1: POST every 10 s
const unsigned long SENSE_EVERY_MS = 2000;   // read the DHT22 every 2 s for the fan
const unsigned long RETRY_EVERY_MS = 5000;

DHT dht(DHT_PIN, DHT22);
WiFiClient net;
PubSubClient mqtt(net);

String mac;
String settingsTopic, stateTopic, statusTopic;

// Settings (B3/B4): until the first valid settings arrive, the fan is off
bool haveSettings = false;
String mode = "off";
float maxHumidity = 100.0;

float humidity = NAN;
bool fanOn = false;
bool statePublished = false;

unsigned long lastPostMs = 0;
unsigned long lastSenseMs = 0;
unsigned long lastRetryMs = 0;

// B4 and B5: drive the fan LED, publish only on change 
void updateFan() {
  bool want = false;
  if (haveSettings) {
    if (mode == "on") {
      want = true;
    } else if (mode == "auto") {
      want = !isnan(humidity) && humidity > maxHumidity;
    }
  }
  digitalWrite(FAN_PIN, want ? HIGH : LOW);

  if (want != fanOn || !statePublished) {
    if (mqtt.connected() && mqtt.publish(stateTopic.c_str(), want ? "on" : "off", true)) {
      statePublished = true;
      Serial.printf("fan/state -> %s\n", want ? "on" : "off");
    }
  }
  fanOn = want;
}

// B3: read fan/settings, refuse anything invalid
void onMessage(char *topic, byte *payload, unsigned int length) {
  if (String(topic) != settingsTopic) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.println("settings refused: not JSON");
    return;
  }
  if (!doc["mode"].is<const char *>()) {
    Serial.println("settings refused: mode missing");
    return;
  }
  if (!(doc["max_humidity"].is<float>() || doc["max_humidity"].is<int>())) {
    Serial.println("settings refused: max_humidity missing");
    return;
  }
  String newMode = doc["mode"].as<const char *>();
  if (newMode != "auto" && newMode != "on" && newMode != "off") {
    Serial.println("settings refused: bad mode");
    return;
  }

  mode = newMode;
  maxHumidity = doc["max_humidity"].as<float>();
  haveSettings = true;
  Serial.printf("settings: mode=%s max_humidity=%.1f\n", mode.c_str(), maxHumidity);
  updateFan();   // react straight away, not at the next reading
}

// B2: broker with a last will message, subscribe to settings
void connectMqtt() {
  String clientId = "test2-" + String(MQTT_NAME) + "-" + String((uint32_t)esp_random(), HEX);
  Serial.print("Connecting to the broker... ");
  if (mqtt.connect(clientId.c_str(), statusTopic.c_str(), 1, true, "offline")) {
    Serial.println("connected");
    mqtt.publish(statusTopic.c_str(), "online", true);
    mqtt.subscribe(settingsTopic.c_str());
    updateFan();
  } else {
    Serial.print("failed, state ");
    Serial.println(mqtt.state());
  }
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);
  Serial.print("Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.println(" connected");
  Serial.print("MAC address: ");
  Serial.println(WiFi.macAddress());
}

// B1: POST the humidity reading to the server, if valid and connected 
void postReading() {
  if (isnan(humidity) || WiFi.status() != WL_CONNECTED) return;
  mqtt.loop();
  HTTPClient http;
  http.setTimeout(5000);
  http.begin(API_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", DEVICE_KEY);
  String body = "{\"mac\":\"" + mac + "\",\"humidity\":" + String(humidity, 1) + "}";
  int status = http.POST(body);
  Serial.printf("POST /readings %s -> %d\n", body.c_str(), status);
  http.end();
  mqtt.loop();
}

void readSensor() {
  float h = dht.readHumidity();
  if (!isnan(h)) {
    humidity = h;
  }
  updateFan();
}

void setup() {
  Serial.begin(115200);
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(FAN_PIN, LOW);
  dht.begin();

  connectWiFi();
  mac = WiFi.macAddress();

  String base = "ecse3038/" + String(MQTT_NAME);
  settingsTopic = base + "/fan/settings";
  stateTopic = base + "/fan/state";
  statusTopic = base + "/status";

  mqtt.setServer(BROKER_HOST, BROKER_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setKeepAlive(20);        // a slow HTTP POST must not make the broker drop us
  mqtt.setSocketTimeout(10);
  connectMqtt();

  delay(2000);          // the DHT22 needs a few seconds to warm up
  readSensor();
  postReading();
  lastPostMs = lastSenseMs = millis();
}

void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  if (!mqtt.connected() && now - lastRetryMs >= RETRY_EVERY_MS) {
    lastRetryMs = now;
    connectMqtt();
  }
  mqtt.loop();

  if (now - lastSenseMs >= SENSE_EVERY_MS) {
    lastSenseMs = now;
    readSensor();
  }
  if (now - lastPostMs >= POST_EVERY_MS) {
    lastPostMs = now;
    postReading();
  }
}
