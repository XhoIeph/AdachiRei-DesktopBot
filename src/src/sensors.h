#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>
#include <DHT.h>

// ====== DHT11 配置 ======
#define DHT_PIN   10          // DHT11 数据引脚 (避开LED 2~9)
#define DHT_TYPE  DHT11
#define DHT_READ_INTERVAL_MS  5000   // DHT11 最快1Hz，这里用5秒

// ====== 传感器数据结构 ======
struct DHTData {
    float temperature;   // 摄氏度
    float humidity;      // 相对湿度 %
    bool  valid;         // 本次读数是否有效
};

void   dhtInit();
DHTData dhtRead();

#endif
