/* 
   EN: Module for automatically maintaining a constant connection to WiFi
   RU: Модуль для автоматического поддержания постоянного подключения к WiFi
   --------------------------
   (с) 2020-2021 Разживин Александр | Razzhivin Alexander
   kotyara12@yandex.ru | https://kotyara12.ru | tg: @kotyara1971
*/

#include "project_config.h"
#include "def_consts.h"

#if !defined(CONFIG_WIFI_ENABLED) || (CONFIG_WIFI_ENABLED == 1)

#ifndef __RE_WIFI_H__
#define __RE_WIFI_H__ 

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <cstring>
#include <time.h> 
#include "esp_wifi_types.h"
#include "esp_wifi.h"
#include "rLog.h"
#include "rTypes.h"
#include "rStrings.h"
#include "reNvs.h"
#include "reEsp32.h"
#include "reEvents.h"
#include "reParams.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"


bool wifiInit();
bool wifiStart();
bool wifiStop();
bool wifiFree();
bool wifiIsConnected();

EventBits_t wifiStatusGet();
char* wifiStatusGetJson();
#if CONFIG_WIFI_DEBUG_ENABLE
char* wifiGetDebugInfo();
#endif // CONFIG_WIFI_DEBUG_ENABLE
wifi_mode_t wifiMode();
wifi_ap_record_t wifiInfo();
int8_t wifiRSSI();
bool wifiRSSIIsOk();
const char* wifiGetSSID();
esp_netif_ip_info_t wifiLocalIP();
char* wifiGetLocalIP();
char* wifiGetGatewayIP();
const char* wifiGetHostname();
// esp_err_t wifiHostByName(const char* hostname, ip_addr_t* hostaddr);

#ifdef __cplusplus
}
#endif

#endif // __RE_WIFI_H__

#endif // CONFIG_WIFI_ENABLED