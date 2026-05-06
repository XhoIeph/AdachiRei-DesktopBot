#include "sensors.h"

static DHT dht(DHT_PIN, DHT_TYPE);

void dhtInit() {
    dht.begin();
    delay(500);  // DHT11 启动稳定时间
    Serial.println("[DHT] 传感器初始化完成");
}

DHTData dhtRead() {
    DHTData data;
    data.humidity    = dht.readHumidity();
    data.temperature = dht.readTemperature();
    data.valid = !isnan(data.temperature) && !isnan(data.humidity);

    if (data.valid) {
        Serial.printf("[DHT] 温度: %.1f°C  湿度: %.1f%%\n",
                      data.temperature, data.humidity);
    } else {
        Serial.println("[DHT] 读取失败!");
    }
    return data;
}
