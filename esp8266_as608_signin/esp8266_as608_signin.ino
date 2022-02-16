#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <FS.h>
#include <Adafruit_Fingerprint.h>
#include <Wire.h>
#include <Ticker.h>
#include <ArduinoJson.h>
#include "SSD1306Wire.h"

#define PRESS_FR_KEY D6   // AS608 touch 触发引脚

SSD1306Wire display(0x3c, D3, D5); // SDA SCL

StaticJsonDocument<500> settings; // 配置文件
DynamicJsonDocument userInfo(2048); // 所有用户信息

IPAddress apIP(192,169,1,1);  // AP 模式下 ip 地址
ESP8266WiFiMulti wifiMulti; // 实例化 ESP8266WiFiMulti 对象
ESP8266WebServer webServer(80); // 实例化网路服务器对象监听 80 端口

SoftwareSerial mySerial(D2, D1);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

// 状态标志位
bool spiffsState = false;
bool wifiConnectionState = false;

/** 触摸事件状态位
 *  0 签到打卡
 *  1 录入指纹
 */
u8 touchEventState = 0;
bool registState = false;
String registerUserName = "";  // 暂存注册时的用户名
String registerPosition = "";  // 职位
String registerDate = "";   // 时间
String registerEmail = "";  // 邮箱 :: 用于通知签到消息
u8 userCount = 0;

// 一些计时器
Ticker touchEventStateReset;

void setup() {
  finger.begin(57600);
  Serial.begin(9600);

  if (SPIFFS.begin()){
    spiffsState = true;
    Serial.println("SPIFFS started.");
  } else {
    spiffsState = false;
    Serial.println("SPIFFS Failed to start.");
  }

  loadSettings(); // 加载配置文件
  loadUserInfo(); // 加载用户信息 以及 userCount 计数器
  
  // 初始化 i2c 屏幕
  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);
  display.clear();
  
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP((const char*)settings["ap"]["ssid"], (const char*)settings["ap"]["password"]);  // AP 模式
  
  wifiMulti.addAP(settings["wifi"]["ssid"], settings["wifi"]["password"]);
  
  // 尝试进行wifi连接
  int i = 0;
  while (wifiMulti.run() != WL_CONNECTED) {
    delay(1000);
    Serial.print(i++); Serial.print('.');
  }
  wifiConnectionState = true;
  
  pinMode(PRESS_FR_KEY, INPUT);

  webServer.on("/api/getAllState", HTTP_GET, handleGetAllState);  // 注册api调用处理函数
  webServer.on("/api/regist", HTTP_GET, handleRegist);    // 注册 录入指纹处理过程
  webServer.on("/api/getRegistState", HTTP_GET, handleGetRegistState);  // 注册查询指纹录入结果处理过程
  webServer.on("/api/modifyUserInfo", HTTP_GET, handleModifyUserInfo);  // 注册 修改用户信息处理过程
  webServer.on("/api/getSettings", HTTP_GET, handleGetSettings);    // 获取所有配置信息
  webServer.on("/api/changeSettings", HTTP_GET, handleChangeSettings);      // 修改设置
  webServer.on("/api/scanWiFi", HTTP_GET, handleScanWiFi);  // 扫描周围可用WiFi 
  webServer.onNotFound(handleUserRequest);
  webServer.begin();
  Serial.println("HTTP server started.");
}

// 保存 json 文件
void saveJsonData(JsonDocument &doc, String path) {
  File file = SPIFFS.open(path, "w");
  serializeJson(doc, file);
  file.close();
}

// 保存(更新)配置文件
void saveSettings() {
  saveJsonData(settings, "/settings.json");
}

// 保存用户信息
void saveUserInfo() {
  saveJsonData(userInfo, "/userinfo.json");
}

// 读取 Json 文件
void loadJsonData(JsonDocument &doc, String path) {
  File file = SPIFFS.open(path, "r");
  auto error = deserializeJson(doc, file.readString());
  if (error) {
    // 读取失败
    Serial.println("JsonData load failed ------ path: " + path);
  }
  file.close();
}

// 加载配置文件
void loadSettings() {
  loadJsonData(settings, "/settings.json");
}

// 加载用户信息文件 /userinfo.json
void loadUserInfo() {
  loadJsonData(userInfo, "/userinfo.json");
  userCount = userInfo["count"].as<uint8_t>();
}

// 重置长时间没有响应的 touch 事件
void handleTouchEventStateReset() {
  if (touchEventState != 0) {
    touchEventState = 0;
  }
  touchEventStateReset.detach();
}

// 显示信息
void displayInfo(String str) {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawStringMaxWidth(0, 0, 128, str);
  display.display();
}

// 绘制进度条
void displayProgressBar(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t progress) {
  progress %= 100;
  display.drawProgressBar(x, y, width, height, progress);
}

// API:: 修改设置
void handleChangeSettings() {

  String tmp = webServer.arg("apSSID");
  if (!tmp.equals((const char*)settings["ap"]["ssid"]) && !tmp.isEmpty()) {
    settings["ap"]["ssid"] = tmp;
    Serial.println(tmp);
  }
  tmp = webServer.arg("apPassword");
  if (!tmp.equals((const char*)settings["ap"]["password"]) && !tmp.isEmpty()) {
    settings["ap"]["password"] = tmp;
  }
  tmp = webServer.arg("wifiSSID");
  if (!tmp.equals((const char*)settings["wifi"]["ssid"]) && !tmp.isEmpty()) {
    settings["wifi"]["ssid"] = tmp;
  }
  tmp = webServer.arg("wifiPassword");
  if (!tmp.equals((const char*)settings["wifi"]["password"]) && !tmp.isEmpty()) {
    settings["wifi"]["password"] = tmp;
  }
  tmp = webServer.arg("dataServerHost");
  if (!tmp.equals((const char*)settings["dataServer"]["host"]) && !tmp.isEmpty()) {
    settings["dataServer"]["host"] = tmp;
  }
  tmp = webServer.arg("dataServerPort");
  if (!tmp.equals((const char*)settings["dataServer"]["port"]) && !tmp.isEmpty()) {
    settings["dataServer"]["port"] = tmp;
    Serial.println(tmp);
  }

  saveSettings();   // 更新配置文件内容
  webServer.send(200, "json/application", "{\"status\": 200}");
}

// API:: 扫描可用WiFi
void handleScanWiFi() {
  uint8_t n = WiFi.scanNetworks();
  
  if (n == 0){
    webServer.send(200, "json/html", "{\"count\": 0}");    
  }
  else {
    DynamicJsonDocument doc(2048);
    doc["count"] = n;
    for (uint8_t i=0; i < n; i++) {
      doc["data"][i]["ssid"] = WiFi.SSID(i);
      doc["data"][i]["channel"] =  WiFi.channel(i);
      doc["data"][i]["strength"] =  WiFi.RSSI(i);
      doc["data"][i]["encryption"] =  (WiFi.encryptionType(i) == ENC_TYPE_NONE)?"open":"*";
    }
    String retString;
    serializeJson(doc, retString);
    webServer.send(200, "json/html", retString); 
  }
}

// API:: 修改用户名
void handleModifyUserInfo() {
  uint8_t id = webServer.arg("id").toInt() - 1;
  if (userInfo["data"][id].isNull()) {
    webServer.send(200, "json/application", "{\"status\":404,\"msg\":\"fail\"}");
    return;
  }
  
  String field = webServer.arg("name");
  if (!field.isEmpty() && !field.equals((const char*)userInfo["data"][id]["name"]))
    userInfo["data"][id]["name"] = field;
    
  field = webServer.arg("position");
  if (!field.isEmpty() && !field.equals((const char*)userInfo["data"][id]["position"]))
    userInfo["data"][id]["position"] = field;
    
  field = webServer.arg("email");
  if (!field.isEmpty() && !field.equals((const char*)userInfo["data"][id]["email"]))
    userInfo["data"][id]["email"] = field;

  saveUserInfo();

  webServer.send(200, "json/application", "{\"status\":200,\"msg\":\"success\"}");
}

// API:: 查询指纹录入状态
void handleGetRegistState() {
  String retString = "{\"status\":" + (String)(registState ? "true}" : "false}");
  if (registState == true){
    registState = false;
  }
  webServer.send(200, "json/application", retString);
}

// 处理指纹录入
void handleRegist() {
  registerUserName = webServer.arg("name");
  registerPosition = webServer.arg("position");
  registerDate = webServer.arg("date");
  registerEmail = webServer.arg("email");
  
  if (!registerUserName.equals("") && 
      !registerPosition.equals("") && 
      !registerDate.equals("") &&
      !registerEmail.equals("")
     ) {
    touchEventState = 1;

    touchEventStateReset.attach(30, handleTouchEventStateReset);  // 定时重置事件状态
    
    webServer.send(200, "json/application", "{\"status\": 200, \"msg\": \"success\"}");
  } else {
    registerUserName = "";
    registerPosition = "";
    registerDate = "";
    webServer.send(200, "json/application", "{\"status\": 404, \"msg\": \"fail\"}");
  }

  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 0, "Please Press");
  display.display();
}

// API:: 返回设置信息
void handleGetSettings() {
  String retString;
  serializeJson(settings, retString);
  webServer.send(200, "json/application", retString);
}

// API:: 返回基本状态信息
void handleGetAllState() {
  DynamicJsonDocument doc(400);
  doc["spiffsState"] = spiffsState;
  doc["wifiConnectionState"] = wifiConnectionState;
  doc["ssid"] = WiFi.SSID();
  doc["ip"] = WiFi.localIP().toString();

  String retString;
  serializeJson(doc, retString);
  Serial.println(retString);
  webServer.send(200, "json/application", retString);
}

void handleUserRequest() {
  String requestUri = webServer.uri();

  bool fileReadOk = handleFileRead(requestUri);

  if (!fileReadOk) {
    webServer.send(200, "text/html", "<h1>404 Not Found</h1>");
  }
}

bool handleFileRead(String path) {
  if (path.length() == 1 && path.endsWith("/")){
    path = "/index.html";
  }

  String contentType = getContentType(path);
  if (SPIFFS.exists(path)) {
    File file = SPIFFS.open(path, "r");
    webServer.streamFile(file, contentType);
    file.close();
    return true;
  } else {
    return false;
  }
}

// 获取文件类型
String getContentType(String filename){
  if(filename.endsWith(".htm")) return "text/html";
  else if(filename.endsWith(".html")) return "text/html";
  else if(filename.endsWith(".css")) return "text/css";
  else if(filename.endsWith(".js")) return "application/javascript";
  else if(filename.endsWith(".png")) return "image/png";
  else if(filename.endsWith(".gif")) return "image/gif";
  else if(filename.endsWith(".jpg")) return "image/jpeg";
  else if(filename.endsWith(".ico")) return "image/x-icon";
  else if(filename.endsWith(".xml")) return "text/xml";
  else if(filename.endsWith(".pdf")) return "application/x-pdf";
  else if(filename.endsWith(".zip")) return "application/x-zip";
  else if(filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
}

// 验证指纹
void verifyFinger()
{
  u8 ensure, i, j = 0;
  //请按手指
  Serial.println("---------------------------");
  Serial.print("Please Press Fingerprint...");
  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 0, "Verify...");
  display.display();
  for (i = 0; i < 15; i++)
  {
    ensure = finger.getImage();
    if (ensure == 0x00) //获取图像成功
    {
      ensure = finger.image2Tz();
      if (ensure == 0x00) //生成特征成功
      {
        ensure = finger.fingerFastSearch();
        if (ensure == 0x00) //搜索成功
        {
          //指纹验证成功
          Serial.println(" ");
          Serial.println("Fingerprint verification successful !!!");
          display.drawString(64, 28, "Success!!");
          delay(400);
          display.drawString(64, 48, "Welcome ID : " + String(finger.fingerID));
          Serial.println(" ");
          display.display();
          j = 0;
          delay(2000);
          break;
        }
        else
        {
          //未搜索到指纹
          Serial.println(" ");
          Serial.println("!!! Fingerprint not found !!!");
          Serial.println(" ");
          display.drawString(64, 28, "Failed!!");
          display.drawString(64, 48, "Not Found");
          display.display();
          delay(2000);
          break;
        }
      }
    }
    j++;
    delay(200);

    if (j >= 15)
    {
      //等待超时
      Serial.println("Timeout !!!");
      Serial.println(" ");
      display.drawString(64, 28, "Timeout");
    }
  }
}

// 录入指纹
void registFinger()
{
  u8 i = 0, ensure, processnum = 0;
  Serial.println("---------------------------");
  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 0, "Register...");
  display.display();

  while (1)
  {
    switch (processnum)
    {
    case 0:
      i++;
      //请按手指
      Serial.println("Please Press...");
      display.drawString(64, 28, "Please Press...");
      display.display();
      ensure = finger.getImage();
      if (ensure == FINGERPRINT_OK)
      {
        ensure = finger.image2Tz(1); //生成特征
        if (ensure == FINGERPRINT_OK)
        {
          //指纹正常
          display.drawString(64, 48, "Press OK");
          display.display();
          i = 0;
          processnum = 1; //跳到第二步
        }
      }
      break;

    case 1:
      i++;
      //再按一次
      Serial.println("Please Press Again...");
      display.drawString(64, 48, "                 ");
      display.display();
      display.drawString(64, 28, "Press Again...");
      display.display();
      ensure = finger.getImage();
      if (ensure == FINGERPRINT_OK)
      {
        ensure = finger.image2Tz(2); //生成特征
        if (ensure == FINGERPRINT_OK)
        {
          //指纹正常
          Serial.println("Press Again OK");
          display.drawString(64, 48, "                 ");
          display.display();
          display.drawString(64, 48, "Press OK");
          display.display();
          i = 0;
          processnum = 2; //跳到第三步
        }
      }
      break;

    case 2:
      // 创建模板
      Serial.print("Finger Create...");
      ensure = finger.createModel();
      if (ensure == FINGERPRINT_OK)
      {
        Serial.println("Success");
        processnum = 3; //跳到第四步
      }
      else
      {
        //创建模板失败
        Serial.println("Fail");
        i = 0;
        processnum = 0; //跳回第一步
      }
      delay(500);
      break;
    case 3:
      //写入指纹ID
      userCount++;
      ensure = finger.storeModel(userCount); //储存模板
      if (ensure == 0x00)
      {
        //录入指纹成功 ID打印
        Serial.print("Add Fingerprint Success --- ");
        display.drawString(64, 28, "                 ");
        display.drawString(64, 48, "                 ");
        display.display();
        display.drawString(64, 28, "Success!!");
        display.drawString(64, 48, "User ID: " + String(userCount));
        display.display();
        userInfo["count"] = userCount;
        userInfo["data"][userCount-1]["id"] = userCount;
        userInfo["data"][userCount-1]["name"] = registerUserName;
        userInfo["data"][userCount-1]["position"] = registerPosition;
        userInfo["data"][userCount-1]["date"] = registerDate;
        userInfo["data"][userCount-1]["email"] = registerEmail;

        saveUserInfo();   // 保存更新后的 userInfo 
        delay(1500);

        registState = true;   // 注册成功 状态位设置为 true
        
        touchEventState = 0;
        return;
      }
      else
      {
        processnum = 0;
      }
      break;
    }
    delay(400);
    if (i == 10) //超过5次没有按手指则退出
    {
      Serial.println("Timeout !!!");
      display.drawString(64, 28, "                 ");
      display.drawString(64, 48, "                 ");
      display.display();
      display.drawString(64, 28, "Timeout!!");
      display.display();
      break;
    }
  }
  touchEventState = 0;
  touchEventStateReset.detach();  // 关闭
}

void loop() {
  webServer.handleClient();
  // 触摸事件触发
  if (digitalRead(PRESS_FR_KEY) == HIGH) {
    // 检查状态位 判断事件意图
    switch(touchEventState) {
      case 0:
        // 签到打卡
        verifyFinger();
        break;
            
      case 1:
        // 录入指纹
        registFinger();
        break;
    }
  }
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString(125, 54, String(touchEventState));
  display.display();
}
