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
#include <Adafruit_CCS811.h>
#include <Adafruit_Si7021.h>
#include <Wire.h>

#define CPU0  0
#define CPU1  1

#define T_WIFIConn_CPU      CPU0
#define T_WIFIConn_PRIOR    0
#define T_WIFIConn_STACK    2048

#define T_EMailRead_CPU     CPU1
#define T_EMailRead_PRIOR   2
#define T_EMailRead_STACK   8192

#define T_WEBServer_CPU     CPU0
#define T_WEBServer_PRIOR   1
#define T_WEBServer_STACK   16384

#define T_LED_CPU           CPU1
#define T_LED_PRIOR         1
#define T_LED_STACK         1024

#define T_FTP_CPU           CPU0
#define T_FTP_PRIOR         0
#define T_FTP_STACK         8192

#define T_UPTIME_CPU        CPU1
#define T_UPTIME_PRIOR      0
#define T_UPTIME_STACK      1024

#define T_CC811_CPU         CPU0
#define T_CC811_PRIOR       0
#define T_CC811_STACK       2048

TaskHandle_t th_WiFiConnect, th_WEBServer, th_EMailRead, th_LED, th_FTPSrv, th_UpTime, th_ReadSensor;
QueueHandle_t qh_EMailRead, qh_TestLED, qh_MsgCount;

#define START_FCOLOR        0xFFFFFF
#define STA_MODE_FCOLOR     0x00FF00
#define AP_MODE_FCOLOR      0xFFFF00

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
void task_ReadSensor(void *param);

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
void print_task_info(xTaskHandle *tHandle);
void i2c_scaner();
uint16_t expRunningAverage(uint16_t newVal);

/* Обработчики WEB-запросов */
void h_Website();    
void h_wifi_param(); 
void h_Email_param();
void h_pushButt();  
void h_WebRequests();       
void h_XML();

/*
* Global variables
*/
const float k = 0.1;
bool b_FWUpdate = false;
bool b_EMailRead = false;
const char cch_APssid[] = "E-mail";
const char cch_APpass[] = "1234567890";
const char cch_web_user[] = "admin";
const char cch_web_pass[] = "Notifier2020";
const char cch_ftpUser[] = "esp32";
const char cch_ftpPass[] = "esp32";
WebServer server;
String s_email[4];
String s_email_srv[4];
String s_email_pass[4];
String s_email_col[4];
String s_updStatusStr = "---";
int i_email_coli[4];
bool b_TestLED = false;
bool b_ftpEn = false;
uint8_t ui8_MsgCount = 0;
IMAPData imapData;
uint32_t ui32_UpTime = 0;
uint16_t ui16_CO2 = 0;
uint16_t ui16_TVOC = 0;
float f_Temperature = 0.0;
float f_Humidity = 0.0;

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
  Serial.println(F("--------------------------| START PROGRAMM |--------------------------"));

  qh_TestLED = xQueueCreate(1, sizeof(b_TestLED));
  qh_EMailRead = xQueueCreate(1, sizeof(b_EMailRead));
  qh_MsgCount = xQueueCreate(4, sizeof(rxED));  

  xTaskCreatePinnedToCore(task_UpTimeCounter, "Up Time Counter", T_UPTIME_STACK, NULL, T_UPTIME_PRIOR, &th_UpTime, T_UPTIME_CPU);  
  vTaskDelay(pdMS_TO_TICKS(10));

  xTaskCreatePinnedToCore(task_LED, "LED", T_LED_STACK, NULL, T_LED_PRIOR, &th_LED, T_LED_CPU);   
};

/*** TASKs ********************************************************************************************************************************/ 
void task_WEBServer(void* param) {
  print_task_header("WEB Server");

  // Регистрация обработчиков
  server.on(F("/"), h_Website);    
  server.on(F("/wifi_param"), h_wifi_param); 
  server.on(F("/email_param"), h_Email_param);
  server.on(F("/pushButt"), HTTP_PUT, h_pushButt);    
  server.onNotFound(h_WebRequests);       
  server.on(F("/xml"), HTTP_PUT, h_XML);

  ElegantOTA.begin(&server); 

  server.begin();  

  for (;;) {
    yield();
    /* Обработка запросов HTML клиента */
    server.handleClient();
    vTaskDelay(1);
  }
}

void task_FTPSrv(void *param) {
  print_task_header("FTP Server");
  FtpServer ftpSrv;
  ftpSrv.begin(cch_ftpUser, cch_ftpPass); 
  for(;;) {
    ftpSrv.handleFTP();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void task_EMailRead(void *param) {
  print_task_header("Read E-mail Count");  
  EMailData txED;
  bool b_fread = false; 
  for (;;) {
    yield();
    xQueueReceive(qh_EMailRead, &b_fread, ((Param.interval == 0)?(30000):(Param.interval * 60000)));
    for (txED.index = 0; txED.index < 4; txED.index++) {
      Serial.println("Reading: " + s_email[txED.index]);
      txED.count = readEmail(s_email_srv[txED.index], s_email[txED.index], s_email_pass[txED.index]);
      xQueueSend(qh_MsgCount, &txED, portMAX_DELAY);
    }
  }
}

void task_UpTimeCounter(void *param) {
  print_task_header("Up Time Counter"); 
  ui32_UpTime = 0;
  for(;;) {
    yield();
    vTaskDelay(pdMS_TO_TICKS(1000));
    ui32_UpTime++;
  }
}

void task_ReadSensor(void *param) {
  print_task_header("Read Sensor");
  Adafruit_CCS811 cc811;
  Adafruit_Si7021 si7021 = Adafruit_Si7021();
  //bool b_HeatEn = false;
  //uint8_t ui8_heatCnt = 0;

  Wire.begin(SDA, SCL, 100000);

  i2c_scaner();

  yield();
  if(!cc811.begin(0x5A)) {
    Serial.println("Failed to start CC811! Please check your wiring.");
  }

  while(!cc811.available()) {
    yield();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  
  for(;;) {
    yield();
    if(cc811.available()) {
      if(!cc811.readData()) {        
        ui16_CO2 = expRunningAverage(cc811.geteCO2());
        ui16_TVOC = cc811.getTVOC();
        cc811.setEnvironmentalData((uint8_t)f_Humidity, (double)f_Temperature);
      }
      else {
        Serial.println(F("CC811 ERROR!"));
      }
    }

    f_Temperature = si7021.readTemperature();
    f_Humidity = si7021.readHumidity();

    // Toggle heater enabled state every 30 seconds
    // An ~1.8 degC temperature increase can be noted when heater is enabled
    /*if (++ui8_heatCnt == 10) {
      b_HeatEn = !b_HeatEn;
      si7021.heater(b_HeatEn);
      Serial.print("Heater Enabled State: ");
      if (si7021.isHeaterEnabled()) Serial.println("ENABLED");
      else Serial.println("DISABLED");
      ui8_heatCnt = 0;
    }*/
    
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void task_LED(void *param) {
  print_task_header("LED");
  led_init();
  BaseType_t qMsgRet;
  EMailData rxED;
  
  xTaskCreatePinnedToCore(task_WiFiConn, "WiFi Connect", T_WIFIConn_STACK, NULL, T_WIFIConn_PRIOR, &th_WiFiConnect, T_WIFIConn_CPU);

  for(;;) {
    qMsgRet = xQueueReceive(qh_MsgCount, &rxED, pdMS_TO_TICKS(10));
    if(qMsgRet == pdPASS) {
      if(rxED.count > 0) leds[rxED.index].setColorCode(i_email_coli[rxED.index]);
      else leds[rxED.index].setRGB(0, 0, 0);
      FastLED.show();
      b_ftpEn = false;
    }
    qMsgRet = xQueueReceive(qh_TestLED, &b_TestLED, pdMS_TO_TICKS(10));
    if(qMsgRet == pdPASS) {

      if(b_TestLED) {
        for(uint8_t i = 0; i < 4; i++) leds[i].setColorCode(i_email_coli[i]);
        vTaskResume(th_FTPSrv);
        print_task_state("FTPSrv", eTaskGetState(th_FTPSrv));
      }
      else {
        for(uint8_t i = 0; i < 4; i++) leds[i].setRGB(0, 0, 0);
        vTaskSuspend(th_FTPSrv);
        print_task_state("FTPSrv", eTaskGetState(th_FTPSrv));
      }
      FastLED.show();
    }
  }
}

void task_WiFiConn(void* param) {
  print_task_header("Wi-Fi Connect");
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
      IPAddress myIP = WiFi.localIP();
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
  
  xTaskCreatePinnedToCore(task_WEBServer, "WEB Server", T_WEBServer_STACK, NULL, T_WEBServer_PRIOR, &th_WEBServer, T_WEBServer_CPU);
  vTaskDelay(pdMS_TO_TICKS(10));

  xTaskCreatePinnedToCore(task_EMailRead, "E-mail Read", T_EMailRead_STACK, NULL, T_EMailRead_PRIOR, &th_EMailRead, T_EMailRead_CPU);
  vTaskDelay(pdMS_TO_TICKS(10));

  xTaskCreatePinnedToCore(task_FTPSrv, "FTP", T_FTP_STACK, NULL, T_FTP_PRIOR, &th_FTPSrv, T_FTP_CPU);
  vTaskDelay(pdMS_TO_TICKS(10));  

  print_task_state("WiFiConnect", eTaskGetState(th_WiFiConnect));

  vTaskSuspend(th_FTPSrv);
  print_task_state("FTPSrv", eTaskGetState(th_FTPSrv));

  xTaskCreatePinnedToCore(task_ReadSensor, "CC811", T_CC811_STACK, NULL, T_CC811_PRIOR, &th_ReadSensor, T_CC811_CPU);
  vTaskDelay(pdMS_TO_TICKS(10));  

  //xQueueSend(qh_EMailRead, &b_EMailRead, portMAX_DELAY);

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
  Serial.println(F("Wi-Fi AP mode"));
  Serial.println(F("AP configuring..."));
  WiFi.mode(WIFI_AP);
  WiFi.softAP(cch_APssid, cch_APpass); 
  Serial.println(F("done"));
  IPAddress myIP = WiFi.softAPIP();
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
  }
  xmlStr += F("<freeHeap>");
  xmlStr += ESP.getFreeHeap();
  xmlStr += F("</freeHeap>");
  xmlStr += F("<heapSize>");
  xmlStr += ESP.getHeapSize();
  xmlStr += F("</heapSize>");

  xmlStr += F("<co2>");
  xmlStr += ui16_CO2;
  xmlStr += F("</co2>");

  xmlStr += F("<tvoc>");
  xmlStr += ui16_TVOC;
  xmlStr += F("</tvoc>");

  xmlStr += F("<temperature>");
  xmlStr += f_Temperature;
  xmlStr += F("</temperature>");

  xmlStr += F("<humidity>");
  xmlStr += f_Humidity;
  xmlStr += F("</humidity>");

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
  if (server.streamFile(dataFile, dataType) != dataFile.size()) {
    Serial.println(F("Write file size != dataFile.size() !!!"));
  }
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
    delay(2);
  }
  vTaskDelay(pdMS_TO_TICKS(250));
  for (int i = 255; i > -1; i--) {
    FastLED.setBrightness(i);
    FastLED.show();
    delay(2);
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

void print_task_info(xTaskHandle *tHandle) {
  const String tStateStr[] = {"eRunning", "eReady", "eBlocked", "eSuspended", "eDeleted"};
  const char taskStr[4] = {'T', 'a', 's', 'k'};
  //TaskStatus_t xTaskDetails;

  /*vTaskGetInfo( tHandle,
              &xTaskDetails,
              pdTRUE, // Include the high water mark in xTaskDetails.
              eInvalid ); // Include the task state in xTaskDetails.*/

  /*int strLenght = (taskName.length() <= 11)?(15):(taskName.length() + 4);
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
  Serial.println();*/
}

// бегущее среднее
uint16_t expRunningAverage(uint16_t newVal) {
  static float filVal = 0;
  filVal += (newVal - filVal) * k;
  return (uint16_t)filVal;
}

void i2c_scaner() {
  byte error, address;
  int nDevices;
 
  Serial.println("Scanning...");
 
  nDevices = 0;
  for(address = 1; address < 127; address++ ) {
    // The i2c_scanner uses the return value of
    // the Write.endTransmisstion to see if
    // a device did acknowledge to the address.
    Wire.beginTransmission(address);
    error = Wire.endTransmission();
 
    if (error == 0) {
      Serial.print("I2C device found at address 0x");
      if (address < 16) Serial.print("0");
      Serial.print(address,HEX);
      Serial.println("  !");
      nDevices++;
    } else if (error == 4) {
      Serial.print("Unknown error at address 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address,HEX);
    }    
  }
  if (nDevices == 0) Serial.println("No I2C devices found\n");
  else Serial.println("done\n");
}


void loop() {
  vTaskDelete(NULL);
};


/* WEB Server handles *****************************************************************************/
/* Обработчик запроса XML данных */
void h_XML() {
  server.send(200, F("text/xml"), build_XML());
}

void h_wifi_param() {
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

void h_Email_param() {
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

  server.send(200, F("text/html"), F("Save E-mail accounts..."));
}

void h_pushButt() {
  Serial.println(F("h_pushButt"));
  print_remote_IP();
  if(!server.authenticate(cch_web_user, cch_web_pass)) return server.requestAuthentication();
  Serial.print(F("Server has: ")); Serial.print(server.args()); Serial.println(F(" argument(s):"));
  for(int i = 0; i < server.args(); i++) {
    Serial.print(F("arg name: ")); Serial.print(server.argName(i)); Serial.print(F(", value: ")); Serial.println(server.arg(i));
  }
  if(server.hasArg(F("buttID"))) {
    if(server.arg(F("buttID")) == F("butFRead")) {
      xQueueSend(qh_EMailRead, &b_EMailRead, portMAX_DELAY);
    }
    if(server.arg(F("buttID")) == F("butTestLED")) {
      b_TestLED = !b_TestLED;      
      xQueueSend(qh_TestLED, &b_TestLED, portMAX_DELAY);
    }
    if(server.arg(F("buttID")) == F("butReboot")) {
      ESP.restart();
    }
  }
  server.send(200, F("text/xml"), build_XML());
}

// void h_WebRequests() {
void h_WebRequests() {
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
void h_Website() {
  Serial.println("h_Website");
  print_remote_IP();
  if(!server.authenticate(cch_web_user, cch_web_pass)) return server.requestAuthentication();
  server.sendHeader(F("Location"), F("/index.html"), true);   //Redirect to our html web page
  server.send(302, F("text/plane"), "");
}