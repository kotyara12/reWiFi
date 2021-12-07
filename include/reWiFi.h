/* 
   EN: Module for automatically maintaining a constant connection to WiFi
   RU: Модуль для автоматического поддержания постоянного подключения к WiFi
   --------------------------
   (с) 2020-2021 Разживин Александр | Razzhivin Alexander
   kotyara12@yandex.ru | https://kotyara12.ru | tg: @kotyara1971
*/

#ifndef __RE_WIFI_H__
#define __RE_WIFI_H__ 

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_netif.h"
#include "esp_wifi_types.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/ip_addr.h"

bool wifiInit();
bool wifiStart();
bool wifiStop();
bool wifiFree();
bool wifiIsConnected();

wifi_mode_t wifiMode();
wifi_ap_record_t wifiInfo();
int8_t wifiRSSI();
const char* wifiGetSSID();
esp_netif_ip_info_t wifiLocalIP();
char* wifiGetLocalIP();
char* wifiGetGatewayIP();
const char* wifiGetHostname();
esp_err_t wifiHostByName(const char* hostname, ip_addr_t* hostaddr);

#ifdef __cplusplus
}
#endif

#endif // __RE_WIFI_H__