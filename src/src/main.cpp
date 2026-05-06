#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include "sensors.h"

// ====== 用户配置 (按需修改) ======
const char* WIFI_SSID   = "YOUR_SSID";
const char* WIFI_PASS   = "YOUR_PASS";
const char* MQTT_BROKER = "broker.emqx.io";
const int   MQTT_PORT   = 1883;
const char* MQTT_TOPIC_SUB  = "astrbot/esp32/control";
const char* MQTT_TOPIC_PUB  = "astrbot/esp32/status";
const char* MQTT_TOPIC_LED  = "astrbot/esp32/led";
const char* MQTT_TOPIC_DHT  = "astrbot/esp32/dht11";   // DHT11 数据上报主题
const char* DEVICE_ID   = "esp32_01";

// ====== LED 流水灯配置 ======
#define LED_COUNT  8
const int LED_PINS[LED_COUNT] = {2, 3, 4, 5, 6, 7, 8, 9};
const unsigned long LED_SPEED_MS  = 150;
const unsigned long LED_REPORT_MS = 3000;

int        ledIndex   = 0;
int        ledDir     = 1;
unsigned long lastLedTick   = 0;
unsigned long lastLedReport = 0;

// ====== DHT11 上报 ======
unsigned long lastDhtRead   = 0;
unsigned long lastDhtReport = 0;

// ====== 基础设施 ======
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
unsigned long lastStatusReport = 0;

// ---------- WiFi / MQTT ----------
void connectWiFi() {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 30) {
        delay(1000);
        retry++;
    }
}

void handleMqttMessage(char* topic, byte* payload, unsigned int len) {
    char buf[2048] = {};
    memcpy(buf, payload, min(len, (unsigned int)2047));

    // === ArduinoJson 7 语法 ===
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf);
    if (err) return;

    const char* cmd = doc["cmd"].as<const char*>();
    if (!cmd) return;

    if (!strcmp(cmd, "ota")) {
        const char* url = doc["url"].as<const char*>();
        if (!url || !url[0]) return;

        HTTPClient http;
        http.begin(String(url));
        int code = http.GET();
        if (code == 200) {
            int total = http.getSize();
            Update.begin(total);
            WiFiClient* stream = http.getStreamPtr();
            Update.writeStream(*stream);
            if (Update.end()) {
                delay(500);
                ESP.restart();
            }
        }
        http.end();
    }
}

void connectMQTT() {
    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setCallback([](char* topic, byte* payload, unsigned int len) {
        handleMqttMessage(topic, payload, len);
    });
    while (!mqtt.connected()) {
        mqtt.connect(DEVICE_ID);
        delay(2000);
    }
    mqtt.subscribe(MQTT_TOPIC_SUB);
}

// ---------- MQTT 上报 ----------
void reportStatus() {
    String json = "{\"device_id\":\"" + String(DEVICE_ID) + "\""
                  ",\"free_heap\":" + String(ESP.getFreeHeap()) +
                  ",\"uptime\":" + String(millis() / 1000) +
                  ",\"rssi\":" + String(WiFi.RSSI()) +
                  ",\"led_active\":" + String(ledIndex) +
                  ",\"led_dir\":" + String(ledDir) +
                  ",\"led_speed\":" + String(LED_SPEED_MS) + "}";
    mqtt.publish(MQTT_TOPIC_PUB, json.c_str());
}

void reportLedStatus() {
    String json = "{\"device_id\":\"" + String(DEVICE_ID) + "\""
                  ",\"led_mode\":\"flow\""
                  ",\"led_count\":" + String(LED_COUNT) +
                  ",\"active_led\":" + String(ledIndex) +
                  ",\"direction\":" + (ledDir == 1 ? "\"forward\"" : "\"backward\"") +
                  ",\"speed_ms\":" + String(LED_SPEED_MS) +
                  ",\"uptime\":" + String(millis() / 1000) + "}";
    mqtt.publish(MQTT_TOPIC_LED, json.c_str());
}

void reportDhtStatus(const DHTData& data) {
    if (!data.valid) return;
    String json = "{\"device_id\":\"" + String(DEVICE_ID) + "\""
                  ",\"temperature\":" + String(data.temperature, 1) +
                  ",\"humidity\":" + String(data.humidity, 1) +
                  ",\"uptime\":" + String(millis() / 1000) + "}";
    mqtt.publish(MQTT_TOPIC_DHT, json.c_str());
    Serial.println("[MQTT] DHT11 数据已上报");
}

// ---------- 用户功能 ----------
void userSetup() {
    // 流水灯初始化
    for (int i = 0; i < LED_COUNT; i++) {
        pinMode(LED_PINS[i], OUTPUT);
        digitalWrite(LED_PINS[i], LOW);
    }
    digitalWrite(LED_PINS[ledIndex], HIGH);

    // DHT11 传感器初始化
    dhtInit();
}

void userLoop() {
    unsigned long now = millis();

    // --- 流水灯 ---
    if (now - lastLedTick >= LED_SPEED_MS) {
        lastLedTick = now;
        digitalWrite(LED_PINS[ledIndex], LOW);
        ledIndex += ledDir;
        if (ledIndex >= LED_COUNT) {
            ledIndex = LED_COUNT - 2;
            ledDir = -1;
        } else if (ledIndex < 0) {
            ledIndex = 1;
            ledDir = 1;
        }
        digitalWrite(LED_PINS[ledIndex], HIGH);
    }

    // LED 状态上报
    if (now - lastLedReport >= LED_REPORT_MS) {
        lastLedReport = now;
        reportLedStatus();
    }

    // --- DHT11 定时读取 & 上报 ---
    if (now - lastDhtRead >= DHT_READ_INTERVAL_MS) {
        lastDhtRead = now;
        DHTData data = dhtRead();
        if (data.valid) {
            reportDhtStatus(data);
        }
    }
}

// ====== Arduino 入口 ======
void setup() {
    Serial.begin(115200);
    connectWiFi();
    connectMQTT();
    userSetup();
}

void loop() {
    mqtt.loop();
    if (millis() - lastStatusReport > 30000) {
        reportStatus();
        lastStatusReport = millis();
    }
    userLoop();
}
