#include <Arduino.h>
#include <WiFi.h>
#include <ESP8266FtpServer.h>
#include <FS.h>
#include <SPIFFS.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <ESP32_MailClient.h>
#include <FastLED.h>

TaskHandle_t WiFiConnect_h, ReadEmailCount_h;
QueueHandle_t MsgCountQueueHandle, FReadQueueHandle, TestLEDQueueHandle;

// Заголовки функций
void WiFiConnect(void* param);
void ReadEmailCount(void* param);
int readEmail();
void readCallback(ReadStatus msg);

//The Email Reading data object contains config and data that received
IMAPData imapData;

#define NUM_LEDS 4
#define LED_PIN 18
CRGB leds[NUM_LEDS];

typedef struct {
  uint8_t index;
  uint8_t count;
} EMailData;

const char web_user[] = "admin";
const char web_pass[] = "notifier1977";

String email[4];
String email_srv[4];
String email_pass[4];
String email_col[4];
int email_coli[4];
bool wifiEn = false;
BaseType_t ret;
EMailData rxED;
bool FRead = false;
bool TestLED = false;
bool ftpEn = false;

#define PARAM_ADDR  (0)
struct Param {
  uint64_t FuseMac;         
  char ssid[20];
  char pass[20];  
  uint8_t interval;     
} Param;  

WebServer server;
FtpServer ftpSrv;
const char* ap_ssid = "E-mail";
const char* ap_pass = "1234567890";
const char* ftpUser = "esp32";
const char* ftpPass = "esp32";

void save_param() {
  int eSize = sizeof(Param);
  EEPROM.begin(eSize);
  EEPROM.put(PARAM_ADDR, Param);
  EEPROM.end();
}

void eeprom_init() {
  Serial.println("EEPROM initialization");
  int eSize = sizeof(Param);
  EEPROM.begin(eSize);
  Param.FuseMac = ESP.getEfuseMac();
  Param.interval = 5;
  EEPROM.put(PARAM_ADDR, Param);
  EEPROM.end();
  Serial.println("EEPROM initialization completed");
  Serial.println("");
}

/* Функция формирования строки XML данных */
String build_XML() {
  String xmlStr;
  xmlStr = F("<?xml version='1.0'?>");
  xmlStr +=    F("<xml>");
  xmlStr += F("<interval>");
  xmlStr += Param.interval;
  xmlStr += F("</interval>");
  for (int i = 0; i < 4; i++) {
    xmlStr += F("<email>");
    xmlStr += email[i];
    xmlStr += F("</email>");

    xmlStr += F("<email_srv>");
    xmlStr += email_srv[i];
    xmlStr += F("</email_srv>");

    xmlStr += F("<email_col>");
    xmlStr += email_col[i];
    xmlStr += F("</email_col>");
  }
  xmlStr += F("</xml>");
  return xmlStr;
}

bool loadFromSpiffs(String path) {
  Serial.print(F("Request File: "));
  Serial.println(path);
  String dataType = F("text/plain");
  if(path.endsWith(F("/"))) path += F("index.html");

  if(path.endsWith(F(".src"))) path = path.substring(0, path.lastIndexOf(F(".")));
  else if(path.endsWith(F(".html"))) dataType = F("text/html");
  else if(path.endsWith(F(".htm"))) dataType = F("text/html");
  else if(path.endsWith(F(".css"))) dataType = F("text/css");
  else if(path.endsWith(F(".js"))) dataType = F("application/javascript");
  else if(path.endsWith(F(".png"))) dataType = F("image/png");
  else if(path.endsWith(F(".gif"))) dataType = F("image/gif");
  else if(path.endsWith(F(".jpg"))) dataType = F("image/jpeg");
  else if(path.endsWith(F(".ico"))) dataType = F("image/x-icon");
  else if(path.endsWith(F(".xml"))) dataType = F("text/xml");
  else if(path.endsWith(F(".pdf"))) dataType = F("application/pdf");
  else if(path.endsWith(F(".zip"))) dataType = F("application/zip");
  File dataFile = SPIFFS.open(path.c_str(), "r");
  Serial.print(F("Load File: "));
  Serial.println(dataFile.name());
  if (server.hasArg(F("download"))) dataType = F("application/octet-stream");
  if (server.streamFile(dataFile, dataType) != dataFile.size()) {}
  dataFile.close();
  return true;
}

void print_remote_IP() {
  Serial.print("Remote IP address: ");
  Serial.println(server.client().remoteIP().toString());
}

/* Обработчик/handler запроса XML данных */
void h_XML() {
  Serial.println(F("h_XML"));
  print_remote_IP();
  server.send(200, F("text/xml"), build_XML());
}

void h_wifi_param() {
  Serial.println(F("h_wifi_param"));
  print_remote_IP();
  if(!server.authenticate(web_user, web_pass)) return server.requestAuthentication();
  String ssid = server.arg(F("wifi_ssid"));
  String pass = server.arg(F("wifi_pass"));
  Serial.print(F("Wi-Fi SSID: ")); Serial.println(ssid);
  Serial.print(F("Wi-Fi pass: ")); Serial.println(pass);
  server.send(200, F("text/html"), F("Wi-Fi setting is updated, module will be rebooting..."));
  delay(100);
  WiFi.softAPdisconnect(true);
  delay(100);
  strcpy(Param.ssid, ssid.c_str());
  strcpy(Param.pass, pass.c_str());
  save_param();
  Serial.print(F("Param SSID: ")); Serial.println(Param.ssid);
  Serial.print(F("Param pass: ")); Serial.println(Param.pass);
  WiFi.mode(WIFI_STA);
  WiFi.begin(Param.ssid, Param.pass);
  delay(500);
  SPIFFS.end();
  Serial.println(F("Resetting ESP..."));
  ESP.restart();
}

int str2int(String strValue) {
  String str;
  str = "0x" + strValue.substring(1);
  return strtol(str.c_str(), NULL, 0);
}

void h_Email_param() {
  Serial.println(F("h_Email_param"));
  print_remote_IP();
  if(!server.authenticate(web_user, web_pass)) return server.requestAuthentication();

  for(int i = 0; i < 4; i++) {
    email[i] = server.arg(String("email") + (i + 1));
    email_srv[i] = server.arg(String("email_srv") + (i + 1));
    if(server.arg(String("email_pass") + (i + 1)) != "") {
      email_pass[i] = server.arg(String("email_pass") + (i + 1));
    }
    email_col[i] = server.arg(String("email_col") + (i + 1));
  }

  /*email[0] = server.arg(F("email1"));
  email[1] = server.arg(F("email2"));
  email[2] = server.arg(F("email3"));
  email[3] = server.arg(F("email4"));

  email_srv[0] = server.arg(F("email_srv1"));
  email_srv[1] = server.arg(F("email_srv2"));
  email_srv[2] = server.arg(F("email_srv3"));
  email_srv[3] = server.arg(F("email_srv4"));

  if(server.arg(F("email_pass1")) != "") email_pass[0] = server.arg(F("email_pass1"));
  if(server.arg(F("email_pass2")) != "") email_pass[1] = server.arg(F("email_pass2"));
  if(server.arg(F("email_pass3")) != "") email_pass[2] = server.arg(F("email_pass3"));
  if(server.arg(F("email_pass4")) != "") email_pass[3] = server.arg(F("email_pass4"));

  email_col[0] = server.arg(F("email_col1"));
  email_col[1] = server.arg(F("email_col2"));
  email_col[2] = server.arg(F("email_col3"));
  email_col[3] = server.arg(F("email_col4"));*/

  Param.interval = server.arg(F("interval")).toInt();
  EEPROM.put(PARAM_ADDR, Param);
  EEPROM.end();

  for (int i = 0; i < 4; i++) {
    email_coli[i] = str2int(email_col[i]);
  }

  File f = SPIFFS.open("/accounts.txt", FILE_WRITE);
  if (!f) {
    Serial.println(F("File open failed."));
  } 
  else {
    for (int i = 0; i < 4; i++) {
      f.println(email[i] + ";" + email_srv[i] + ";" + email_pass[i] + ";" + email_col[i]);
    }
    f.flush();
    f.close();
  }

  server.send(200, F("text/html"), F("Save E-mail accounts..."));
}

void h_pushButt() {
  Serial.println(F("h_pushButt"));
  print_remote_IP();
  if(!server.authenticate(web_user, web_pass)) return server.requestAuthentication();
  Serial.print(F("Server has: ")); Serial.print(server.args()); Serial.println(F(" argument(s):"));
  for(int i = 0; i < server.args(); i++) {
    Serial.print(F("arg name: ")); Serial.print(server.argName(i)); Serial.print(F(", value: ")); Serial.println(server.arg(i));
  }
  if(server.hasArg(F("buttID"))) {
    if(server.arg(F("buttID")) == F("butFRead")) {
      xQueueSend(FReadQueueHandle, &FRead, portMAX_DELAY);
    }
    if(server.arg(F("buttID")) == F("butTestLED")) {
      TestLED = !TestLED;      
      xQueueSend(TestLEDQueueHandle, &TestLED, portMAX_DELAY);
    }
  }
  server.send(200, F("text/xml"), build_XML());
}

void h_WebRequests() {
  Serial.println("h_WebRequests");
  print_remote_IP();
  if(!server.authenticate(web_user, web_pass)) return server.requestAuthentication();
  if(loadFromSpiffs(server.uri())) return;
  Serial.println(F("File Not Detected"));
  String message = F("File Not Detected\n\n");
  message += F("URI: ");
  message += server.uri();
  message += F("\nMethod: ");
  message += (server.method() == HTTP_GET)?F("GET"):F("POST");
  message += F("\nArguments: ");
  message += server.args();
  message += "\n";
  for (uint8_t i=0; i<server.args(); i++){
    message += " NAME:" + server.argName(i) + "\n VALUE:" + server.arg(i) + "\n";
  }
  server.send(404, F("text/plain"), message);
  Serial.println(message);
}

/* Обработчик/handler запроса HTML страницы */
/* Отправляет клиенту(браузеру) HTML страницу */
void h_Website() {
  Serial.println("h_Website");
  print_remote_IP();
  if(!server.authenticate(web_user, web_pass)) return server.requestAuthentication();
  server.sendHeader(F("Location"), F("/index.html"), true);   //Redirect to our html web page
  server.send(302, F("text/plane"), "");
}

void ap_config() {
  Serial.println(F("Wi-Fi AP mode"));
  Serial.println(F("AP configuring..."));
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_pass); 
  Serial.println(F("done"));
  IPAddress myIP = WiFi.softAPIP();
  Serial.print(F("AP IP address: "));
  Serial.println(myIP);    
}

void parse_accounts(String str, uint8_t set) { 
  uint8_t si = 0, ei = 0;
  ei = str.indexOf(";", si);
  email[set] = str.substring(si, ei);

  si = ei + 1;
  ei = str.indexOf(";", si);
  email_srv[set] = str.substring(si, ei);

  si = ei + 1;
  ei = str.indexOf(";", si);
  email_pass[set] = str.substring(si, ei);

  si = ei + 1;
  ei = str.length() - 1;
  email_col[set] = str.substring(si, ei);
}

void read_accounts(String file) {
  File f = SPIFFS.open(file, "r");
  uint8_t i = 0;
  if (!f) {
    Serial.println(F("E-mail accounts file open failed."));
  } else {
    while(f.available()) {
      String line = f.readStringUntil('\n');
      parse_accounts(line, i);
      email_coli[i] = str2int(email_col[i]);
      i++;
    } 
    f.close();
  }
}

void led_flash(uint32_t colorCode) {
  for (int i = 0; i < 4; i++) leds[i].setColorCode(colorCode);
  for (int i = 0; i < 256; i++){
    FastLED.setBrightness(i);
    FastLED.show();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  vTaskDelay(pdMS_TO_TICKS(250));
  for (int i = 255; i > -1; i--) {
    FastLED.setBrightness(i);
    FastLED.show();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  for (int i = 0; i < 4; i++) leds[i].setColorCode(0x000000);
  FastLED.setBrightness(255);
}

void led_init() {
  FastLED.addLeds<WS2812, LED_PIN, GRB>(leds,  NUM_LEDS);
  FastLED.setBrightness(255);
  led_flash(0xFFFFFF);
  FastLED.setBrightness(255);
}

void setup() {
  Serial.begin(115200);
  Serial.println(String("CPU frequncy: ") + ets_get_cpu_frequency());
  Serial.println();

  led_init();

  MsgCountQueueHandle = xQueueCreate(4, sizeof(rxED));  
  FReadQueueHandle = xQueueCreate(1, sizeof(FRead));
  TestLEDQueueHandle = xQueueCreate(1, sizeof(TestLED));

  xTaskCreatePinnedToCore(
            WiFiConnect,   
            "WiFi Connect",     
            8192,       
            NULL,         
            1,            
            &WiFiConnect_h,  
            0);
}

void loop() {
  ret = xQueueReceive(MsgCountQueueHandle, &rxED, 10);
  if(ret == pdPASS) {
    if(rxED.count > 0) leds[rxED.index].setColorCode(email_coli[rxED.index]);
    else leds[rxED.index].setRGB(0, 0, 0);
    FastLED.show();
    ftpEn = false;
  }
  ret = xQueueReceive(TestLEDQueueHandle, &TestLED, 10);
  if(ret == pdPASS) {
    for(uint8_t i = 0; i < 4; i++) {
      if(TestLED) {
        leds[i].setColorCode(email_coli[i]);
        ftpEn = true;
      }
      else {
        leds[i].setRGB(0, 0, 0);
        ftpEn = false;
      }
    }
    FastLED.show();
  }
}

//Callback function to get the Email reading status
void readCallback(ReadStatus msg) {  
}

void WiFiConnect(void* param) {
  uint8_t WiFiConnTimeOut = 50;

  Serial.println();
  if(SPIFFS.begin(true)) {
    Serial.println(F("SPIFFS opened!"));
    float spiffsUsedP = SPIFFS.usedBytes() / (SPIFFS.totalBytes() / 100.0);
    Serial.print(F("SPIFFS total bytes: ")); Serial.println(SPIFFS.totalBytes());
    Serial.print(F("SPIFFS used bytes: ")); Serial.print(SPIFFS.usedBytes()); Serial.print(F("(")); 
    Serial.print(spiffsUsedP); Serial.println(F("%)"));  
    read_accounts("/accounts.txt");
  }

  Serial.println("");
  Serial.println(F("--- Wi-Fi config ---"));
  
  Serial.println("");
  int eSize = sizeof(Param);
  EEPROM.begin(eSize);
  EEPROM.get(PARAM_ADDR, Param);
  if(Param.FuseMac != ESP.getEfuseMac()) {
    eeprom_init();
    ap_config();
    led_flash(0xFFFF00);
  }
  else {
    Serial.println(F("Wi-Fi STA mode"));
    WiFi.mode(WIFI_STA);
    WiFi.begin(Param.ssid, Param.pass);
    while (WiFi.status() != WL_CONNECTED) {      
      delay(190);
      Serial.print(".");
      if(--WiFiConnTimeOut == 0) break;
    }
    if(WiFiConnTimeOut == 0) {
      Serial.println("");
      Serial.println(F("Wi-Fi connected timeout"));
      ap_config();   
      led_flash(0xFFFF00);  
    }
    else {
      Serial.println("");
      Serial.print(F("STA connected to: ")); 
      Serial.println(WiFi.SSID());
      IPAddress myIP = WiFi.localIP();
      Serial.print(F("Local IP address: ")); Serial.println(myIP);
      WiFi.enableAP(false);

      xTaskCreatePinnedToCore(
        ReadEmailCount,   
        "Read Email count",     
        8192,       
        NULL,         
        1,            
        &ReadEmailCount_h,  
        0);
        led_flash(0x00FF00);

        xQueueSend(FReadQueueHandle, &FRead, portMAX_DELAY);
    }
  }

  ftpSrv.begin(ftpUser, ftpPass); 

  // Регистрация обработчиков
  server.on(F("/"), h_Website);    
  server.on(F("/wifi_param"), h_wifi_param); 
  server.on(F("/email_param"), h_Email_param);
  server.on(F("/pushButt"), h_pushButt);    
  server.onNotFound(h_WebRequests);       
  server.on(F("/xml"), h_XML);
  server.begin();

  for (;;) {
    /* Обработка запросов HTML клиента */
    server.handleClient();
    /* Обработка запросов FTP клиента */
    if(ftpEn) ftpSrv.handleFTP();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

int readEmail() {
  uint8_t emailCount = 0;
  //Serial.print("heap before read: "); Serial.println(esp_get_free_heap_size());
  MailClient.readMail(imapData);
  emailCount = imapData.availableMessages();
  //Serial.print("heap after read: "); Serial.println(esp_get_free_heap_size());
  imapData.clearMessageData();
  //Serial.print("heap after clear read: "); Serial.println(esp_get_free_heap_size());
  return emailCount;
}

void ReadEmailCount(void* param) {
  EMailData txED;
  imapData.setFolder("INBOX");
  imapData.setSearchCriteria("UID SEARCH UNSEEN");
  bool fread = false;
  for (;;) {
    xQueueReceive(FReadQueueHandle, &fread, ((Param.interval == 0)?(30000):(Param.interval * 60000)));
    for (txED.index = 0; txED.index < 4; txED.index++) {
      Serial.println("Reading: " + email[txED.index]);
      imapData.setLogin(email_srv[txED.index], 993, email[txED.index], email_pass[txED.index]);
      txED.count = readEmail();
      xQueueSend(MsgCountQueueHandle, &txED, portMAX_DELAY);
    }
  }
}
