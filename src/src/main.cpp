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
#include <driver/ledc.h>
#include <EspLuaEngine.h>
#include <ArduinoJson.h>
#include <vector>

// [EXT_POINT:INCLUDES] 在此添加新外设头文件
#include <U8g2lib.h>

// ====== 用户配置 ======
const char* WIFI_SSID   = "你好";
const char* WIFI_PASS   = "789247847";
const char* MQTT_BROKER = "47.110.153.97";
const int   MQTT_PORT   = 1883;
const char* MQTT_TOPIC_SUB = "astrbot/esp32/control";
const char* MQTT_TOPIC_PUB = "astrbot/esp32/status";
const char* DEVICE_ID   = "esp32_01";
const int   LED_PIN     = 2;

// ====== Lua VM (勿修改) ======
lua_State* L = nullptr;

// --- 异步脚本队列 ---
bool luaExec(const String& script);  // 前置声明

QueueHandle_t _luaQueue = nullptr;

void luaTask(void* param) {
    while(true) {
        char* script = nullptr;
        if(xQueueReceive(_luaQueue, &script, portMAX_DELAY) == pdTRUE && script) {
            luaExec(String(script));
            delete[] script;
        }
    }
}

// ---------- C → Lua 绑定 ----------
static int l_led_on(lua_State* L)      { digitalWrite(LED_PIN, HIGH); return 0; }
static int l_led_off(lua_State* L)     { digitalWrite(LED_PIN, LOW);  return 0; }
static int l_led_toggle(lua_State* L)  { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); return 0; }

static int l_gpio_set(lua_State* L) {
    int p=lua_tointeger(L,1), v=lua_tointeger(L,2);
    ledcDetachPin(p);               // 强制释放 LEDC, 防止 PWM 占用导致 GPIO 无效
    pinMode(p, OUTPUT);
    digitalWrite(p, v?HIGH:LOW);
    return 0;
}
static int l_gpio_read(lua_State* L)   { lua_pushinteger(L, digitalRead(lua_tointeger(L,1))); return 1; }
static int l_delay_ms(lua_State* L)    { delay(lua_tointeger(L,1)); return 0; }
static int l_delay_us(lua_State* L)    { delayMicroseconds(lua_tointeger(L,1)); return 0; }

static bool _pwm0_setup = false;
static int l_pwm_duty(lua_State* L) {
    int pin=lua_tointeger(L,1), duty=lua_tointeger(L,2);
    if(!_pwm0_setup){ ledcSetup(0,5000,10); _pwm0_setup=true; ledcAttachPin(pin,0); }
    ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,constrain(duty,0,1023));
    ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0);
    return 0;
}
static int l_pwm_freq(lua_State* L) {
    ledcSetup(0,lua_tointeger(L,2),10); ledcAttachPin(lua_tointeger(L,1),0);
    _pwm0_setup = true;
    return 0;
}

static bool _fade_installed = false;
static int l_pwm_fade(lua_State* L) {
    int target=lua_tointeger(L,1), duration_ms=lua_tointeger(L,2);
    if(!_pwm0_setup){ ledcSetup(0,5000,10); _pwm0_setup=true; }
    if(!_fade_installed){ ledc_fade_func_install(0); _fade_installed=true; }
    ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,constrain(target,0,1023),duration_ms);
    ledc_fade_start(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,LEDC_FADE_WAIT_DONE);
    return 0;
}

static int l_analog_read(lua_State* L) { lua_pushinteger(L, analogRead(lua_tointeger(L,1))); return 1; }

// DHT11/DHT22 — 返回 Lua table: {temp=25.0, humidity=60.0}
static int l_dht_read(lua_State* L) {
    int pin=lua_tointeger(L,1), type=lua_tointeger(L,2);
    if(type!=11 && type!=22) type=11;
    DHT dht(pin, type); dht.begin();
    float t=dht.readTemperature(), h=dht.readHumidity();
    if(isnan(t)||isnan(h)){ return 0; }
    lua_newtable(L);
    lua_pushnumber(L, t); lua_setfield(L, -2, "temp");
    lua_pushnumber(L, h); lua_setfield(L, -2, "humidity");
    return 1;
}

// I2C
static int l_i2c_scan(lua_State* L) {
    String r="["; for(int a=8;a<120;a++){ Wire.beginTransmission((uint8_t)a); if(!Wire.endTransmission()){ if(r!="[")r+=","; r+=a; } } r+="]";
    lua_pushstring(L,r.c_str()); return 1;
}
static int l_i2c_write(lua_State* L) {
    Wire.beginTransmission((uint8_t)lua_tointeger(L,1));
    Wire.write((uint8_t*)lua_tostring(L,2),strlen(lua_tostring(L,2)));
    lua_pushinteger(L, Wire.endTransmission()==0); return 1;
}
static int l_i2c_read(lua_State* L) {
    int addr=lua_tointeger(L,1), len=lua_tointeger(L,2);
    Wire.requestFrom((uint8_t)addr,(size_t)len); String r;
    while(Wire.available()) r+=(char)Wire.read();
    lua_pushstring(L,r.c_str()); return 1;
}

// UART
static int l_uart_write(lua_State* L) {
    int p=lua_tointeger(L,1); const char* s=lua_tostring(L,2);
    if(p==0)Serial.print(s); else if(p==1)Serial1.print(s); else if(p==2)Serial2.print(s);
    return 0;
}

// SPI
static int l_spi_xfer(lua_State* L) {
    SPI.beginTransaction(SPISettings(1000000,MSBFIRST,SPI_MODE0));
    uint8_t b=SPI.transfer(lua_tointeger(L,1));
    SPI.endTransaction();
    lua_pushinteger(L,b); return 1;
}

// MQTT 发布 + 日志
WiFiClient wific; PubSubClient mqtt(wific);
static int l_mqtt_pub(lua_State* L) {
    mqtt.publish(lua_tostring(L,1), lua_tostring(L,2));
    return 0;
}
static int l_log(lua_State* L)        { Serial.println(lua_tostring(L,1)); return 0; }
static int l_device_id(lua_State* L)  { lua_pushstring(L,DEVICE_ID); return 1; }
static int l_free_heap(lua_State* L)  { lua_pushinteger(L,ESP.getFreeHeap()); return 1; }
static int l_millis(lua_State* L)     { lua_pushinteger(L,millis()); return 1; }

// [EXT_POINT:GLOBALS] 在此声明全局 C++ 外设对象
U8G2* u8g2_display = nullptr;

// [EXT_POINT:BINDINGS] 在此添加新的 Lua C 绑定函数
// 模式: 取参(lua_tointeger/lua_tostring) → 调用 C++ API → 返回(lua_pushxxx + return n)
static int l_oled_init(lua_State* L) {
    if(u8g2_display) { delete u8g2_display; u8g2_display = nullptr; }
    int addr = lua_gettop(L)>=1 ? lua_tointeger(L,1) : 0x3C;
    int sda  = lua_gettop(L)>=2 ? lua_tointeger(L,2) : -1;
    int scl  = lua_gettop(L)>=3 ? lua_tointeger(L,3) : -1;
    if(sda >= 0 && scl >= 0) {
        Wire1.begin(sda, scl);
        u8g2_display = new U8G2_SSD1306_128X64_NONAME_2ND_HW_I2C(U8G2_R0, U8X8_PIN_NONE);
    } else {
        u8g2_display = new U8G2_SSD1306_128X64_NONAME_F_HW_I2C(U8G2_R0, U8X8_PIN_NONE);
    }
    u8g2_display->setI2CAddress(addr);
    u8g2_display->begin();
    u8g2_display->setFont(u8g2_font_6x10_tf);
    return 0;
}
static int l_oled_print(lua_State* L) {
    if(!u8g2_display) return 0;
    u8g2_display->drawStr(lua_tointeger(L,1), lua_tointeger(L,2), lua_tostring(L,3));
    return 0;
}
static int l_oled_clear(lua_State* L) { if(u8g2_display) u8g2_display->clearBuffer(); return 0; }
static int l_oled_send(lua_State* L)  { if(u8g2_display) u8g2_display->sendBuffer(); return 0; }
static int l_oled_set_font(lua_State* L) {
    if(!u8g2_display) return 0;
    const char* f = lua_tostring(L,1);
    if(!strcmp(f,"large")) u8g2_display->setFont(u8g2_font_ncenB14_tr);
    else if(!strcmp(f,"medium")) u8g2_display->setFont(u8g2_font_ncenB08_tr);
    else u8g2_display->setFont(u8g2_font_6x10_tf);  // small / default
    return 0;
}
static int l_oled_draw_pixel(lua_State* L) {
    if(u8g2_display) u8g2_display->drawPixel(lua_tointeger(L,1), lua_tointeger(L,2));
    return 0;
}
static int l_oled_draw_line(lua_State* L) {
    if(u8g2_display) u8g2_display->drawLine(lua_tointeger(L,1),lua_tointeger(L,2),lua_tointeger(L,3),lua_tointeger(L,4));
    return 0;
}
static int l_oled_draw_rect(lua_State* L) {
    if(u8g2_display) u8g2_display->drawFrame(lua_tointeger(L,1),lua_tointeger(L,2),lua_tointeger(L,3),lua_tointeger(L,4));
    return 0;
}

// ---------- Lua VM 初始化 ----------
void luaSetup() {
    L = luaL_newstate();
    lua_gc(L, LUA_GCGEN, 0, 0);  // 分代 GC 模式

    // 按需加载标准库 (EspLuaEngine 仅编译了 base/string/table/math)
    luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(L, 1);

    // 注册硬件函数 (21 个核心 + 8 个 OLED)
    lua_register(L, "led_on",       l_led_on);
    lua_register(L, "led_off",      l_led_off);
    lua_register(L, "led_toggle",   l_led_toggle);
    lua_register(L, "gpio_set",     l_gpio_set);
    lua_register(L, "gpio_read",    l_gpio_read);
    lua_register(L, "delay_ms",     l_delay_ms);
    lua_register(L, "delay_us",     l_delay_us);
    lua_register(L, "pwm_duty",     l_pwm_duty);
    lua_register(L, "pwm_freq",     l_pwm_freq);
    lua_register(L, "pwm_fade",     l_pwm_fade);
    lua_register(L, "analog_read",  l_analog_read);
    lua_register(L, "dht_read",     l_dht_read);
    lua_register(L, "i2c_scan",    l_i2c_scan);
    lua_register(L, "i2c_write",   l_i2c_write);
    lua_register(L, "i2c_read",    l_i2c_read);
    lua_register(L, "uart_write",   l_uart_write);
    lua_register(L, "spi_xfer",    l_spi_xfer);
    lua_register(L, "mqtt_pub",     l_mqtt_pub);
    lua_register(L, "log",          l_log);
    lua_register(L, "device_id",    l_device_id);
    lua_register(L, "free_heap",    l_free_heap);
    lua_register(L, "millis",       l_millis);

    // [EXT_POINT:REGISTRATIONS] 在下方追加 lua_register(L, ...)
    lua_register(L, "oled_init",       l_oled_init);
    lua_register(L, "oled_print",      l_oled_print);
    lua_register(L, "oled_clear",      l_oled_clear);
    lua_register(L, "oled_send",       l_oled_send);
    lua_register(L, "oled_set_font",   l_oled_set_font);
    lua_register(L, "oled_draw_pixel", l_oled_draw_pixel);
    lua_register(L, "oled_draw_line",  l_oled_draw_line);
    lua_register(L, "oled_draw_rect",  l_oled_draw_rect);

    // 重定向 print → Serial
    lua_getglobal(L, "print");
    lua_pushcfunction(L, l_log);
    lua_setglobal(L, "print");
}

// [DO_NOT_MODIFY_BELOW] 核心 VM 执行 / 硬件 Manifest / SPIFFS / WiFi / MQTT / OTA — 禁止修改

bool luaExec(const String& script) {
    if(!L) return false;
    if(luaL_dostring(L, script.c_str()) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        Serial.print("[Lua] "); Serial.println(err ? err : "unknown error");
        lua_pop(L, 1);
        return false;
    }
    lua_gc(L, LUA_GCCOLLECT, 0);  // 脚本执行完后回收内存
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
    while(f){ if(!f.isDirectory()&&String(f.name()).endsWith(".lua")){
        SavedScript s; s.code=f.readString(); s.name=String(f.name()); s.last=0;
        // parse event trigger from script header: -- @event:boot  -- @event:timer_5s
        int ei=s.code.indexOf("-- @event:");
        if(ei>=0){ int en=ei+10, el=s.code.indexOf('\n',en); if(el>en) s.event=s.code.substring(en,el); }
        if(s.event.startsWith("timer_")){ s.interval=s.event.substring(6).toInt()*1000L; if(!s.interval)s.interval=5000; }
        scripts.push_back(s);
    } f=root.openNextFile(); }
    root.close();
}

void saveScript(const String& name, const String& code) {
    String path="/scripts/"+name; if(!path.endsWith(".lua")) path+=".lua";
    File f=SPIFFS.open(path,FILE_WRITE); if(f){ f.print(code); f.close(); }
    loadScripts();
}

void runEvent(const String& ev) {
    for(auto& s:scripts) if(s.event==ev && millis()-s.last>=s.interval){ s.last=millis(); luaExec(s.code); }
}

// ====== WiFi / MQTT / OTA ======
unsigned long lastStatus=0;
void wifiConnect() {
    WiFi.begin(WIFI_SSID,WIFI_PASS);
    for(int i=0;i<30&&WiFi.status()!=WL_CONNECTED;i++) delay(1000);
}

void otaTask(void* param) {
    String* url=(String*)param;
    HTTPClient h; h.begin(*url);
    if(h.GET()==200){ Update.begin(h.getSize()); Update.writeStream(*h.getStreamPtr()); if(Update.end()){ delay(500); ESP.restart(); } }
    h.end(); delete url; vTaskDelete(NULL);
}

void onMqtt(char* t, byte* p, unsigned int l) {
    // 尝试 JSON 命令 (save_lua / set_hardware / ota)
    JsonDocument* doc = new JsonDocument();
    DeserializationError err=deserializeJson(*doc,(const char*)p,min(l,(unsigned)4095));
    const char* jcmd = err ? nullptr : (*doc)["cmd"];
    if(jcmd){
        if(!strcmp(jcmd,"save_lua")){
            const char* n=(*doc)["name"]|"s"; const char* s=(*doc)["script"]|"";
            if(s[0]) saveScript(String(n),String(s));
        }
        else if(!strcmp(jcmd,"set_hardware")){
            const char* m=(*doc)["manifest"]; if(m) applyManifest(String(m));
        }
        else if(!strcmp(jcmd,"ota")){
            const char* u=(*doc)["url"]|""; if(u[0]){
                String* url=new String(u);
                xTaskCreate(otaTask,"ota",10240,url,1,NULL);
            }
        }
        delete doc; return;
    }
    delete doc;

    // payload 以 { 开头 → 损坏的 JSON, 拒绝执行, 防止 OTA/save 等指令被误当 Lua 运行
    if(l>0 && p[0]=='{') {
        if(err) { Serial.print("[MQTT] bad JSON: "); Serial.println(err.c_str()); }
        return;
    }

    // 非 JSON → 推入异步队列 (run_lua)
    char* script=new char[l+1]{};
    memcpy(script,p,l);
    xQueueSend(_luaQueue, &script, 0);
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
void userSetup() {
    // === 默认 SSD1306 128x64 I2C (地址 0x3C) — 取消注释以启用 ===
    // l_oled_init(L);
    // 或通过 Lua 脚本: oled_init(0x3C)  /  oled_init(0x3C, SDA, SCL)
    //
    // === 切换到其他显示驱动 — 替换上面的构造函数 ===
    // SH1106 128x64 I2C:
    //   u8g2_display = new U8G2_SH1106_128X64_NONAME_F_HW_I2C(U8G2_R0, U8X8_PIN_NONE);
    //   u8g2_display->begin();
    // SSD1306 128x32 I2C:
    //   u8g2_display = new U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C(U8G2_R0, U8X8_PIN_NONE);
    // ST7920 128x64 HW SPI (cs, reset):
    //   u8g2_display = new U8G2_ST7920_128X64_F_HW_SPI(U8G2_R0, /*cs*/5, /*reset*/17);
}
void userLoop() { /* >>> USER CODE: 循环 <<< */ }

// ====== 入口 ======
void setup() {
    Serial.begin(115200);
    SPIFFS.begin(true);
    luaSetup();
    _luaQueue = xQueueCreate(8, sizeof(char*));
    xTaskCreate(luaTask, "luaQ", 8192, NULL, 1, NULL);
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
