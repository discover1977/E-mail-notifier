#ifndef TFTP_H
#define TFTP_H

#include <Arduino.h>
#include <ESP8266FtpServer.h>
#include <FS.h>
#include <SPIFFS.h>

#define FTP_USER    "esp32"
#define FTP_PASS    "esp32"

#define CPU0        0
#define CPU1        1

#define T_FTP_CPU       CPU0
#define T_FTP_PRIOR     0
#define T_FTP_STACK     8192
#define T_FTP_NAME      "FTP"


class TFTP
{
    public:
        TaskHandle_t begin(String user = FTP_USER, String pass = FTP_PASS);      
             
    private:        
        friend void taskWrap(void* param);
        void taskMain();    
        TaskHandle_t th;    
        String _user;
        String _pass;
};

#endif