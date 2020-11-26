
#include "tFTP.h"

void taskWrap(void* param) {
  TFTP* ftp = static_cast<TFTP*>(param);
  ftp->taskMain();
};

TaskHandle_t TFTP::begin(String user, String pass) {
    _user = user;
    _pass = pass;
    xTaskCreatePinnedToCore(taskWrap, T_FTP_NAME, T_FTP_STACK, this, T_FTP_PRIOR, &th, T_FTP_CPU);
    return th;
}

void TFTP::taskMain() {  
  FtpServer ftpSrv;
  ftpSrv.begin(_user, _pass); 
  for(;;) {
    yield();
    ftpSrv.handleFTP();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}