#include <Arduino.h>
#include <EEPROM.h>
#include <SPIFFS.h>
#include <WebServer.h>
#include <ESP32_MailClient.h>
#include <FastLED.h>
#include <FS.h>
#include <SPIFFS.h>
#include <ESP8266FtpServer.h>
#include <ElegantOTA.h>
#include <DNSServer.h>

#define FW_VERSION          "2.2"

#define CPU0  0
#define CPU1  1

#define T_WIFIConn_CPU      CPU0
#define T_WIFIConn_PRIOR    0
#define T_WIFIConn_STACK    2048
#define T_WIFIConn_NAME     "Wi-Fi connect"

#define T_EMailRead_CPU     CPU1
#define T_EMailRead_PRIOR   0
#define T_EMailRead_STACK   8192
#define T_EMailRead_NAME     "E-Mail read"

#define T_WEBServer_CPU     CPU0
#define T_WEBServer_PRIOR   0
#define T_WEBServer_STACK   16384
#define T_WEBServer_NAME    "WEBServer"

#define T_LED_CPU           CPU1
#define T_LED_PRIOR         0
#define T_LED_STACK         2048
#define T_LED_NAME          "LED"

#define T_FTP_CPU           CPU0
#define T_FTP_PRIOR         0
#define T_FTP_STACK         8192
#define T_FTP_NAME          "FTP"

#define T_UPTIME_CPU        CPU1
#define T_UPTIME_PRIOR      0
#define T_UPTIME_STACK      1024
#define T_UPTIME_NAME       "Up time"

TaskHandle_t th_WiFiConnect, th_WEBServer, th_EMailRead, th_LED, th_FTPSrv, th_UpTime;
QueueHandle_t qh_EMailRead, qh_TestLED, qh_MsgCount;

#define START_FCOLOR        0xFFFFFF
#define STA_MODE_FCOLOR     0x00FF00
#define AP_MODE_FCOLOR      0xFFFF00

#define AP_SSID             "Notifier"
#define AP_PASS             "0123456789"
#define FTP_USER            "esp32"
#define FTP_PASS            "esp32"

/* 
* Заголовки функций
*/

/* Задачи */
void task_WiFiConn(void *param);
void task_WEBServer(void *param);
void task_EMailRead(void *param);
void task_LED(void *param);
void task_FTPSrv(void *param);
void task_UpTimeCounter(void *param);

/* Прочие функции */
void print_task_header(String taskName);
void eeprom_init();
void ap_config();
void eeprom_init();
void read_accounts(String file);
void parse_accounts(String str, uint8_t set);
int str2int(String strValue);
String build_XML();
bool loadFromSpiffs(String path);
void print_remote_IP();
void save_param();
int readEmail(String email_srv, String email, String email_pass);
void led_flash(uint32_t colorCode);
void led_init();
void print_task_state(String taskName, int state);

/* Обработчики WEB-запросов */
void hw_Website();    
void hw_wifi_param(); 
void hw_Email_param();
void hw_pushButt();    
void hw_WebRequests();       
void hw_XML();

/*
* Global variables
*/
const char cch_web_user[] = "admin";
const char cch_web_pass[] = "Notifier2020";
WebServer server;
String s_email[4];
String s_email_srv[4];
String s_email_pass[4];
String s_email_col[4];
int i_email_coli[4];
int i_email_count[4];
bool b_TestLED = false;
bool b_FTPEn = false;
uint32_t ui32_UpTime = 0;
String s_snackBarMsg = "...";
bool b_Restart = false;
bool b_EMailRead = false;
uint8_t testLEDCode = 0;
const String fwVersion = FW_VERSION;
IPAddress myIP;

#define NUM_LEDS 4
#define LED_PIN 18
CRGB leds[NUM_LEDS];

#define PARAM_ADDR  (0)
struct Param {
  uint64_t FuseMac;         
  char ssid[20];
  char pass[20];  
  uint8_t interval;     
} Param; 

typedef struct {
  uint8_t index;
  uint8_t count;
} EMailData;

void setup() {
  EMailData rxED;
  Serial.begin(115200);
  Serial.println();

  Serial.println(F("--------------------------| START PROGRAMM |--------------------------"));

  qh_TestLED = xQueueCreate(1, sizeof(b_TestLED));
  qh_EMailRead = xQueueCreate(1, sizeof(b_EMailRead));
  qh_MsgCount = xQueueCreate(4, sizeof(rxED));

  xTaskCreatePinnedToCore(task_UpTimeCounter, T_UPTIME_NAME, T_UPTIME_STACK, NULL, T_UPTIME_PRIOR, &th_UpTime, T_UPTIME_CPU);  
  vTaskDelay(pdMS_TO_TICKS(10));

  xTaskCreatePinnedToCore(task_LED, T_LED_NAME, T_LED_STACK, NULL, T_LED_PRIOR, &th_LED, T_LED_CPU);  

  Serial.println();    
};

/*** TASKs ********************************************************************************************************************************/ 
void task_WEBServer(void* param) {
  print_task_header(T_WEBServer_NAME);

  DNSServer dnsServer;
  const char *server_name = "www.notifier.net";

  // Регистрация обработчиков
  server.on(F("/"), hw_Website);    
  server.on(F("/wifi_param"), hw_wifi_param); 
  server.on(F("/email_param"), hw_Email_param);
  server.on(F("/pushButt"), hw_pushButt);    
  server.onNotFound(hw_WebRequests);       
  server.on(F("/xml"), hw_XML);

  ElegantOTA.begin(&server); 

  server.begin();  
  const byte DNS_PORT = 53;
  Serial.print(F("IP for DNS: ")); Serial.println(myIP);
  dnsServer.start(DNS_PORT, server_name, myIP);

  for (;;) {
    yield();
    /* Обработка запросов HTML клиента */
    dnsServer.processNextRequest();
    server.handleClient();
  }
}

void task_FTPSrv(void *param) {
  print_task_header(T_FTP_NAME);
  print_task_state(T_WIFIConn_NAME, eTaskGetState(th_WiFiConnect));
  FtpServer ftpSrv;
  const char cch_ftpUser[] = FTP_USER;
  const char cch_ftpPass[] = FTP_PASS;
  ftpSrv.begin(cch_ftpUser, cch_ftpPass); 
  for(;;) {
    yield();
    ftpSrv.handleFTP();
  }
}

void task_EMailRead(void *param) {
  print_task_header(T_EMailRead_NAME);  
  EMailData txED;
  bool b_fread = false; 
  String str = "";
  for (;;) {
    yield();
    xQueueReceive(qh_EMailRead, &b_fread, ((Param.interval == 0)?(30000):(Param.interval * 60000)));
    if(b_TestLED) {
      b_TestLED = false;
      s_snackBarMsg = "LED Test Off";
      vTaskDelay(pdMS_TO_TICKS(2000));
    }
    for (txED.index = 0; txED.index < 4; txED.index++) {
      txED.count = readEmail(s_email_srv[txED.index], s_email[txED.index], s_email_pass[txED.index]);
      Serial.println("Reading: " + s_email[txED.index]);
      str = "Reading: " + s_email[txED.index];
      s_snackBarMsg = str;
      txED.count = readEmail(s_email_srv[txED.index], s_email[txED.index], s_email_pass[txED.index]);
      vTaskDelay(pdMS_TO_TICKS(2000));
      i_email_count[txED.index] = txED.count;
      xQueueSend(qh_MsgCount, &txED, 0);
    }
  }
}

void task_UpTimeCounter(void *param) {
  print_task_header(T_UPTIME_NAME); 
  ui32_UpTime = 0;
  for(;;) {
    yield();
    vTaskDelay(pdMS_TO_TICKS(1000));
    ui32_UpTime++;
  }
}

void task_LED(void *param) {
  print_task_header(T_LED_NAME);
  led_init();
  BaseType_t qMsgRet;
  EMailData rxED;
  
  xTaskCreatePinnedToCore(task_WiFiConn, T_WIFIConn_NAME, T_WIFIConn_STACK, NULL, T_WIFIConn_PRIOR, &th_WiFiConnect, T_WIFIConn_CPU);

  for(;;) {
    yield();
    qMsgRet = xQueueReceive(qh_MsgCount, &rxED, 0);
    if(qMsgRet == pdPASS) {
      if(rxED.count > 0) {
        leds[rxED.index].setColorCode(i_email_coli[rxED.index]);
      }
      else leds[rxED.index].setRGB(0, 0, 0);
      FastLED.show();
    }

    qMsgRet = xQueueReceive(qh_TestLED, &b_TestLED, 0);
    if(qMsgRet == pdPASS) {
      if(b_TestLED) {
        for(uint8_t i = 0; i < 4; i++) {
          leds[i].setColorCode(i_email_coli[i]);
          i_email_count[i] = 99;
        }
        s_snackBarMsg = "LED Test On";
      }
      else {
        for(uint8_t i = 0; i < 4; i++) {
          leds[i].setRGB(0, 0, 0);
          i_email_count[i] = 0;
        }
        s_snackBarMsg = "LED Test Off";
      }
      FastLED.show();
    }
  }
}

void task_WiFiConn(void* param) {
  print_task_header(T_WIFIConn_NAME);
  uint8_t WiFiConnTimeOut = 50;
  Serial.println();

  int eSize = sizeof(Param);
  EEPROM.begin(eSize);
  EEPROM.get(PARAM_ADDR, Param);
  if(Param.FuseMac != ESP.getEfuseMac()) {
    eeprom_init();
    ap_config();
    led_flash(AP_MODE_FCOLOR);
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
      led_flash(AP_MODE_FCOLOR);  
    }
    else {
      Serial.println("");
      Serial.print(F("STA connected to: ")); 
      Serial.println(WiFi.SSID());
      myIP = WiFi.localIP();
      Serial.print(F("Local IP address: ")); Serial.println(myIP);
      WiFi.enableAP(false);            
      led_flash(STA_MODE_FCOLOR);
    }
  }  

  if(SPIFFS.begin(true)) {
    Serial.println(F("SPIFFS opened!"));
    float spiffsUsedP = SPIFFS.usedBytes() / (SPIFFS.totalBytes() / 100.0);
    Serial.print(F("SPIFFS total bytes: ")); Serial.println(SPIFFS.totalBytes());
    Serial.print(F("SPIFFS used bytes: ")); Serial.print(SPIFFS.usedBytes()); Serial.print(F("(")); 
    Serial.print(spiffsUsedP); Serial.println(F("%)"));  
    read_accounts("/accounts.txt");
  }
  
  xTaskCreatePinnedToCore(task_WEBServer, T_WEBServer_NAME, T_WEBServer_STACK, NULL, T_WEBServer_PRIOR, &th_WEBServer, T_WEBServer_CPU);
  vTaskDelay(pdMS_TO_TICKS(10));

  xTaskCreatePinnedToCore(task_FTPSrv, T_FTP_NAME, T_FTP_STACK, NULL, T_FTP_PRIOR, &th_FTPSrv, T_FTP_CPU);
  vTaskDelay(pdMS_TO_TICKS(10));  

  vTaskSuspend(th_FTPSrv);
  print_task_state(T_FTP_NAME, eTaskGetState(th_FTPSrv));

  if(WiFi.getMode() == WIFI_STA) {
    xTaskCreatePinnedToCore(task_EMailRead, T_EMailRead_NAME, T_EMailRead_STACK, NULL, T_EMailRead_PRIOR, &th_EMailRead, T_EMailRead_CPU);
    vTaskDelay(pdMS_TO_TICKS(10));
    xQueueSend(qh_EMailRead, &b_EMailRead, portMAX_DELAY);
  }
  vTaskDelete(NULL);
}
/*******************************************************************************************/

/*-----------------------------------------------------------------------------------------*/
void print_task_header(String taskName) {
  char taskStr[4] = {'T', 'a', 's', 'k'};
  int strLenght = (taskName.length() <= 11)?(15):(taskName.length() + 4);
  Serial.println();
  for(int i = 0; i < strLenght; i++) {
    if(i < 4) Serial.print(taskStr[i]);
    else Serial.print(F("-"));
  }
  Serial.println();
  Serial.print(F("| "));
  Serial.print(taskName);
  if(taskName.length() <= 11) {
    for(int i = 0; i < 11 - taskName.length(); i++) Serial.print(F(" "));
  }  
  Serial.print(F(" |"));
  Serial.println();
  Serial.print(F("| "));
  Serial.print(F("is running!"));
  for(int i = 0; i < strLenght - 15; i++) Serial.print(F(" "));
  Serial.print(F(" |"));
  Serial.println();
  for(int i = 0; i < strLenght; i++) Serial.print(F("-"));
  Serial.println();
}

void ap_config() {
  const char cch_APssid[] = AP_SSID;
  const char cch_APpass[] = AP_PASS;
  Serial.println(F("Wi-Fi AP mode"));
  Serial.println(F("AP configuring..."));
  WiFi.mode(WIFI_AP);
  WiFi.softAP(cch_APssid, cch_APpass); 
  Serial.println(F("done"));
  myIP = WiFi.softAPIP();
  Serial.print(F("AP IP address: "));
  Serial.println(myIP);    
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

int str2int(String strValue) {
  String str;
  str = "0x" + strValue.substring(1);
  return strtol(str.c_str(), NULL, 0);
}

void parse_accounts(String str, uint8_t set) { 
  uint8_t si = 0, ei = 0;
  ei = str.indexOf(";", si);
  s_email[set] = str.substring(si, ei);

  si = ei + 1;
  ei = str.indexOf(";", si);
  s_email_srv[set] = str.substring(si, ei);

  si = ei + 1;
  ei = str.indexOf(";", si);
  s_email_pass[set] = str.substring(si, ei);

  si = ei + 1;
  ei = str.length() - 1;
  s_email_col[set] = str.substring(si, ei);
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
      i_email_coli[i] = str2int(s_email_col[i]);
      i++;
    } 
    f.close();
  }
}

/* Функция формирования строки XML данных */
String build_XML() {
  String xmlStr;
  xmlStr = F("<?xml version='1.0'?>");
  xmlStr +=    F("<xml>");
  xmlStr += F("<interval>");
  xmlStr += Param.interval;
  xmlStr += F("</interval>");
  xmlStr += F("<uptime>");
  xmlStr += ui32_UpTime;
  xmlStr += F("</uptime>");
  for (int i = 0; i < 4; i++) {
    xmlStr += F("<email>");
    xmlStr += s_email[i];
    xmlStr += F("</email>");

    xmlStr += F("<email_srv>");
    xmlStr += s_email_srv[i];
    xmlStr += F("</email_srv>");

    xmlStr += F("<email_col>");
    xmlStr += s_email_col[i];
    xmlStr += F("</email_col>");

    xmlStr += F("<email_count>");
    xmlStr += i_email_count[i];
    xmlStr += F("</email_count>");
  }
  xmlStr += F("<freeHeap>");
  xmlStr += ESP.getFreeHeap();
  xmlStr += F("</freeHeap>");
  xmlStr += F("<heapSize>");
  xmlStr += ESP.getHeapSize();
  xmlStr += F("</heapSize>");
  xmlStr += F("<snackBarMsg>");
  xmlStr += s_snackBarMsg;
  xmlStr += F("</snackBarMsg>");
  s_snackBarMsg = "...";

  xmlStr += F("<fwVer>");
  xmlStr += fwVersion;
  xmlStr += F("</fwVer>");

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

void save_param() {
  int eSize = sizeof(Param);
  EEPROM.begin(eSize);
  EEPROM.put(PARAM_ADDR, Param);
  EEPROM.end();
}

int readEmail(String email_srv, String email, String email_pass) {
  IMAPData imapData;
  imapData.setFolder("INBOX");
  imapData.setSearchCriteria("UID SEARCH UNSEEN");
  imapData.setLogin(email_srv, 993, email, email_pass);

  uint8_t emailCount = 0;
  MailClient.readMail(imapData);
  emailCount = imapData.availableMessages();
  imapData.clearMessageData();
  imapData.empty();
  return emailCount;
}

void led_flash(uint32_t colorCode) {
  for (int i = 0; i < 4; i++) leds[i].setColorCode(colorCode);
  for (int i = 0; i < 256; i++){
    FastLED.setBrightness(i);
    FastLED.show();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  vTaskDelay(pdMS_TO_TICKS(250));
  for (int i = 255; i > -1; i--) {
    FastLED.setBrightness(i);
    FastLED.show();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  for (int i = 0; i < 4; i++) leds[i].setColorCode(0x000000);
  FastLED.setBrightness(255);
}

void led_init() {
  FastLED.addLeds<WS2812, LED_PIN, GRB>(leds,  NUM_LEDS);
  FastLED.setBrightness(255);
  led_flash(START_FCOLOR);
  FastLED.setBrightness(255);
}

//	eRunning = 0,	/*!< A task is querying the state of itself, so must be running. */
//	eReady,			/*!< The task being queried is in a read or pending ready list. */
//	eBlocked,		/*!< The task being queried is in the Blocked state. */
//	eSuspended,		/*!< The task being queried is in the Suspended state, or is in the Blocked state with an infinite time out. */
//	eDeleted		/*!< The task being queried has been deleted, but its TCB has not yet been freed. */

void print_task_state(String taskName, int state) {
  const String tStae[] = {"eRunning", "eReady", "eBlocked", "eSuspended", "eDeleted"};
  Serial.print(F("Task "));
  Serial.print(taskName);
  Serial.print(F(" state: "));
  Serial.println(tStae[state]);
}

void loop() {
  vTaskDelete(NULL);
};


/* WEB Server handles *****************************************************************************/
/* Обработчик запроса XML данных */
void hw_XML() {
  server.send(200, F("text/xml"), build_XML());
  if(b_Restart) {
    ESP.restart();
  }
}

void hw_wifi_param() {
  Serial.println(F("h_wifi_param"));
  print_remote_IP();
  if(!server.authenticate(cch_web_user, cch_web_pass)) return server.requestAuthentication();
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

void hw_Email_param() {
  Serial.println(F("h_Email_param"));
  print_remote_IP();
  if(!server.authenticate(cch_web_user, cch_web_pass)) return server.requestAuthentication();

  for(int i = 0; i < 4; i++) {
    s_email[i] = server.arg(String("email") + (i + 1));
    s_email_srv[i] = server.arg(String("email_srv") + (i + 1));
    if(server.arg(String("email_pass") + (i + 1)) != "") {
      s_email_pass[i] = server.arg(String("email_pass") + (i + 1));
    }
    s_email_col[i] = server.arg(String("email_col") + (i + 1));
  }

  Param.interval = server.arg(F("interval")).toInt();
  EEPROM.put(PARAM_ADDR, Param);
  EEPROM.end();

  for (int i = 0; i < 4; i++) {
    i_email_coli[i] = str2int(s_email_col[i]);
  }

  File f = SPIFFS.open("/accounts.txt", FILE_WRITE);
  if (!f) {
    Serial.println(F("File open failed."));
  } 
  else {
    for (int i = 0; i < 4; i++) {
      f.println(s_email[i] + ";" + s_email_srv[i] + ";" + s_email_pass[i] + ";" + s_email_col[i]);
    }
    f.flush();
    f.close();
  }
  server.sendHeader(F("Location"), F("/index.html"), true);   //Redirect to our html web page
  server.send(302, F("text/plane"), "");
  s_snackBarMsg = "E-mail accounts is saved!";
}

void hw_pushButt() {
  Serial.println(F("h_pushButt"));
  print_remote_IP();
  if(!server.authenticate(cch_web_user, cch_web_pass)) return server.requestAuthentication();
  Serial.print(F("Server has: ")); Serial.print(server.args()); Serial.println(F(" argument(s):"));
  for(int i = 0; i < server.args(); i++) {
    Serial.print(F("arg name: ")); Serial.print(server.argName(i)); Serial.print(F(", value: ")); Serial.println(server.arg(i));
  }
  if(server.hasArg(F("buttID"))) {
    if(server.arg(F("buttID")) == F("butFRead")) {
      xQueueSend(qh_EMailRead, &b_EMailRead, 0);
    }
    if(server.arg(F("buttID")) == F("butTestLED")) {
      b_TestLED = !b_TestLED;      
      xQueueSend(qh_TestLED, &b_TestLED, 0);
    }
    if(server.arg(F("buttID")) == F("butReboot")) {
      s_snackBarMsg = "Notifier will be rebooted!";
      b_Restart = true;
    }
    if(server.arg(F("buttID")) == F("butFTPEn")) {
      if(!b_FTPEn) {
        b_FTPEn = true;
        s_snackBarMsg = "FTP Enabled";
        vTaskResume(th_FTPSrv);        
      }
      else {
        b_FTPEn = false;
        s_snackBarMsg = "FTP Disabled";
        vTaskSuspend(th_FTPSrv);
      }
      print_task_state(T_FTP_NAME, eTaskGetState(th_FTPSrv));
    }
  }
  server.send(200, F("text/xml"), build_XML());
}

void hw_WebRequests() {
  Serial.println("h_WebRequests");
  print_remote_IP();
  if(!server.authenticate(cch_web_user, cch_web_pass)) return server.requestAuthentication();  
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

/* 
* Обработчик / handler запроса HTML страницы
* Отправляет клиенту(браузеру) HTML страницу
*/
void hw_Website() {
  Serial.println("h_Website");
  print_remote_IP();
  if(!server.authenticate(cch_web_user, cch_web_pass)) return server.requestAuthentication();
  server.sendHeader(F("Location"), F("/index.html"), true);   //Redirect to our html web page
  server.send(302, F("text/plane"), "");
}