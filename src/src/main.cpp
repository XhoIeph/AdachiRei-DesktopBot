#include <Arduino.h>
#include <WiFi.h>
#define MQTT_MAX_PACKET_SIZE 4096
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <Update.h>
#include <SPIFFS.h>
#define ARDUINOJSON_DEFAULT_POOL_SIZE 4096
#include <DHT.h>
#include <Wire.h>
#include <SPI.h>
#include <berry.h>
#include <ArduinoJson.h>
#include <vector>

// ====== 用户配置 ======
const char* WIFI_SSID   = "你好";
const char* WIFI_PASS   = "789247847";
const char* MQTT_BROKER = "47.110.153.97";
const int   MQTT_PORT   = 1883;
const char* MQTT_TOPIC_SUB = "astrbot/esp32/control";
const char* MQTT_TOPIC_PUB = "astrbot/esp32/status";
const char* DEVICE_ID   = "esp32_01";
const int   LED_PIN     = 2;

// ====== Berry VM (勿修改) ======
bvm* berry_vm = nullptr;

// ---------- C → Berry 绑定 ----------
static int b_led_on(bvm* vm)      { digitalWrite(LED_PIN, HIGH); be_return_nil(vm); }
static int b_led_off(bvm* vm)     { digitalWrite(LED_PIN, LOW);  be_return_nil(vm); }
static int b_led_toggle(bvm* vm)  { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); be_return_nil(vm); }

static int b_gpio_set(bvm* vm) {
    pinMode(be_toint(vm,1), OUTPUT);
    digitalWrite(be_toint(vm,1), be_toint(vm,2)?HIGH:LOW);
    be_return_nil(vm);
}
static int b_gpio_read(bvm* vm)   { be_pushint(vm, digitalRead(be_toint(vm,1))); be_return(vm); }
static int b_delay_ms(bvm* vm)    { delay(be_toint(vm,1)); be_return_nil(vm); }

static bool _pwm0_setup = false;
static int b_pwm_duty(bvm* vm) {
    int pin=be_toint(vm,1), duty=be_toint(vm,2);
    if(!_pwm0_setup){ ledcSetup(0,5000,10); _pwm0_setup=true; }
    ledcAttachPin(pin,0); ledcWrite(0,constrain(duty,0,1023));
    be_return_nil(vm);
}
static int b_pwm_freq(bvm* vm) {
    ledcSetup(0,be_toint(vm,2),10); ledcAttachPin(be_toint(vm,1),0);
    _pwm0_setup = true;
    be_return_nil(vm);
}

static int b_analog_read(bvm* vm) { be_pushint(vm, analogRead(be_toint(vm,1))); be_return(vm); }

// DHT11/DHT22 — 返回 Berry map: {"temp": 25.0, "humidity": 60.0}
static int b_dht_read(bvm* vm) {
    int pin=be_toint(vm,1), type=be_toint(vm,2);
    if(type!=11 && type!=22) type=11;
    DHT dht(pin, type); dht.begin();
    float t=dht.readTemperature(), h=dht.readHumidity();
    if(isnan(t)||isnan(h)){ be_return_nil(vm); }
    be_newmap(vm);
    be_pushstring(vm, "temp");     be_pushreal(vm, t); be_data_insert(vm, -3);
    be_pushstring(vm, "humidity"); be_pushreal(vm, h); be_data_insert(vm, -3);
    be_return(vm);
}

// I2C
static int b_i2c_scan(bvm* vm) {
    String r="["; for(int a=8;a<120;a++){ Wire.beginTransmission((uint8_t)a); if(!Wire.endTransmission()){ if(r!="[")r+=","; r+=a; } } r+="]";
    be_pushstring(vm,r.c_str()); be_return(vm);
}
static int b_i2c_write(bvm* vm) {
    Wire.beginTransmission((uint8_t)be_toint(vm,1));
    Wire.write((uint8_t*)be_tostring(vm,2),strlen(be_tostring(vm,2)));
    be_pushint(vm, Wire.endTransmission()==0); be_return(vm);
}
static int b_i2c_read(bvm* vm) {
    int addr=be_toint(vm,1), len=be_toint(vm,2);
    Wire.requestFrom((uint8_t)addr,(size_t)len); String r;
    while(Wire.available()) r+=(char)Wire.read();
    be_pushstring(vm,r.c_str()); be_return(vm);
}

// UART
static int b_uart_write(bvm* vm) {
    int p=be_toint(vm,1); const char* s=be_tostring(vm,2);
    if(p==0)Serial.print(s); else if(p==1)Serial1.print(s); else if(p==2)Serial2.print(s);
    be_return_nil(vm);
}

// SPI
static int b_spi_xfer(bvm* vm) {
    SPI.beginTransaction(SPISettings(1000000,MSBFIRST,SPI_MODE0));
    uint8_t b=SPI.transfer(be_toint(vm,1));
    SPI.endTransaction();
    be_pushint(vm,b); be_return(vm);
}

// MQTT 发布 + 日志
WiFiClient wific; PubSubClient mqtt(wific);
static int b_mqtt_pub(bvm* vm) {
    mqtt.publish(be_tostring(vm,1), be_tostring(vm,2));
    be_return_nil(vm);
}
static int b_log(bvm* vm)        { Serial.println(be_tostring(vm,1)); be_return_nil(vm); }
static int b_device_id(bvm* vm)  { be_pushstring(vm,DEVICE_ID); be_return(vm); }
static int b_free_heap(bvm* vm)  { be_pushint(vm,ESP.getFreeHeap()); be_return(vm); }
static int b_millis_(bvm* vm)    { be_pushint(vm,millis()); be_return(vm); }

// ---------- Berry VM 初始化 ----------
void berrySetup() {
    berry_vm = be_vm_new();
    be_regfunc(berry_vm, "led_on",       b_led_on);
    be_regfunc(berry_vm, "led_off",      b_led_off);
    be_regfunc(berry_vm, "led_toggle",   b_led_toggle);
    be_regfunc(berry_vm, "gpio_set",     b_gpio_set);
    be_regfunc(berry_vm, "gpio_read",    b_gpio_read);
    be_regfunc(berry_vm, "delay_ms",     b_delay_ms);
    be_regfunc(berry_vm, "pwm_duty",     b_pwm_duty);
    be_regfunc(berry_vm, "pwm_freq",     b_pwm_freq);
    be_regfunc(berry_vm, "analog_read",  b_analog_read);
    be_regfunc(berry_vm, "dht_read",     b_dht_read);
    be_regfunc(berry_vm, "i2c_scan",    b_i2c_scan);
    be_regfunc(berry_vm, "i2c_write",   b_i2c_write);
    be_regfunc(berry_vm, "i2c_read",    b_i2c_read);
    be_regfunc(berry_vm, "uart_write",   b_uart_write);
    be_regfunc(berry_vm, "spi_xfer",    b_spi_xfer);
    be_regfunc(berry_vm, "mqtt_pub",     b_mqtt_pub);
    be_regfunc(berry_vm, "log",          b_log);
    be_regfunc(berry_vm, "device_id",    b_device_id);
    be_regfunc(berry_vm, "free_heap",    b_free_heap);
    be_regfunc(berry_vm, "millis",       b_millis_);
}

bool berryExec(const String& script) {
    if(!berry_vm) return false;
    be_loadstring(berry_vm, script.c_str());
    be_call(berry_vm, 0);
    return true;
}

// ====== 硬件 Manifest ======
// 格式: {"dht11_pin":4, "i2c_sda":21, "i2c_scl":22, "spi_mosi":23, "spi_miso":19, "spi_sck":18}
String hwManifest;

void applyManifest(const String& json) {
    JsonDocument doc;
    if(deserializeJson(doc, json)) return;
    hwManifest = json;
    // 保存到 SPIFFS
    File f = SPIFFS.open("/hardware.json", FILE_WRITE);
    if(f){ f.print(json); f.close(); }
    // I2C 引脚
    if(!doc["i2c_sda"].isNull() && !doc["i2c_scl"].isNull())
        Wire.begin(doc["i2c_sda"].as<int>(), doc["i2c_scl"].as<int>());
    else Wire.begin();
    // SPI 引脚
    if(!doc["spi_mosi"].isNull())
        SPI.begin(doc["spi_sck"].as<int>()|18, doc["spi_miso"].as<int>()|19, doc["spi_mosi"].as<int>()|23, doc["spi_cs"].as<int>()|(-1));
    else SPI.begin();
}

void loadManifest() {
    File f = SPIFFS.open("/hardware.json");
    if(f){ hwManifest = f.readString(); f.close(); applyManifest(hwManifest); }
    else{ Wire.begin(); SPI.begin(); }
}

// ====== SPIFFS 脚本存储 ======
struct SavedScript { String name, code, event; unsigned long interval, last; };
std::vector<SavedScript> scripts;

void loadScripts() {
    scripts.clear();
    File root = SPIFFS.open("/scripts");
    if(!root||!root.isDirectory()){ SPIFFS.mkdir("/scripts"); return; }
    File f = root.openNextFile();
    while(f){ if(!f.isDirectory()&&String(f.name()).endsWith(".be")){
        SavedScript s; s.code=f.readString(); s.name=String(f.name()); s.last=0;
        // parse event trigger from script header: #@event:boot  #@event:timer_5s
        int ei=s.code.indexOf("#@event:");
        if(ei>=0){ int en=ei+8, el=s.code.indexOf('\n',en); if(el>en) s.event=s.code.substring(en,el); }
        if(s.event.startsWith("timer_")){ s.interval=s.event.substring(6).toInt()*1000L; if(!s.interval)s.interval=5000; }
        scripts.push_back(s);
    } f=root.openNextFile(); }
    root.close();
}

void saveScript(const String& name, const String& code) {
    String path="/scripts/"+name; if(!path.endsWith(".be")) path+=".be";
    File f=SPIFFS.open(path,FILE_WRITE); if(f){ f.print(code); f.close(); }
    loadScripts();
}

void runEvent(const String& ev) {
    for(auto& s:scripts) if(s.event==ev && millis()-s.last>=s.interval){ s.last=millis(); berryExec(s.code); }
}

// ====== WiFi / MQTT / OTA ======
unsigned long lastStatus=0;
void wifiConnect() {
    WiFi.begin(WIFI_SSID,WIFI_PASS);
    for(int i=0;i<30&&WiFi.status()!=WL_CONNECTED;i++) delay(1000);
}

void onMqtt(char* t, byte* p, unsigned int l) {
    char b[4096]={}; memcpy(b,p,min(l,(unsigned)4095));
    JsonDocument doc;
    if(deserializeJson(doc,b)) return;
    const char* cmd=doc["cmd"]; if(!cmd) return;
    if(!strcmp(cmd,"run_berry"))   { const char* s=doc["script"]|""; if(s[0]) berryExec(String(s)); }
    else if(!strcmp(cmd,"save_berry")){ const char* n=doc["name"]|"s"; const char* s=doc["script"]|""; if(s[0]) saveScript(String(n),String(s)); }
    else if(!strcmp(cmd,"set_hardware")){ const char* m=doc["manifest"]; if(m) applyManifest(String(m)); }
    else if(!strcmp(cmd,"ota")) {
        String url=doc["url"]|""; if(url.length()){
            HTTPClient h; h.begin(url); if(h.GET()==200){ Update.begin(h.getSize()); Update.writeStream(*h.getStreamPtr()); if(Update.end()){ delay(500); ESP.restart(); } } h.end();
        }
    }
}

void mqttConnect() {
    mqtt.setServer(MQTT_BROKER,MQTT_PORT); mqtt.setCallback(onMqtt);
    while(!mqtt.connected()){ mqtt.connect(DEVICE_ID); delay(2000); }
    mqtt.subscribe(MQTT_TOPIC_SUB);
}

void statusReport() {
    String j="{\\\"id\\\":\\\""+String(DEVICE_ID)+"\\\",\\\"heap\\\":"+String(ESP.getFreeHeap())+",\\\"uptime\\\":"+String(millis()/1000)+",\\\"rssi\\\":"+String(WiFi.RSSI())+",\\\"scripts\\\":"+String(scripts.size())+"}";
    mqtt.publish(MQTT_TOPIC_PUB,j.c_str());
}

// ====== 用户功能 ======
void userSetup() { /* >>> USER CODE: 初始化 <<< */ }
void userLoop() { /* >>> USER CODE: 循环 <<< */ }

// ====== 入口 ======
void setup() {
    Serial.begin(115200);
    SPIFFS.begin(true);
    berrySetup();
    loadManifest();
    loadScripts();
    pinMode(LED_PIN,OUTPUT);
    wifiConnect();
    mqttConnect();
    runEvent("boot");
    userSetup();
}

void loop() {
    mqtt.loop();
    unsigned long n=millis();
    if(n-lastStatus>30000){ statusReport(); lastStatus=n; }
    runEvent("timer_5s"); runEvent("timer_10s"); runEvent("timer_30s"); runEvent("timer_60s");
    userLoop();
}
