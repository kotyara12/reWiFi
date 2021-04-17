/* 
   EN: Module for automatically maintaining a constant connection to WiFi
   RU: Модуль для автоматического поддержания постоянного подключения к WiFi
   --------------------------
   (с) 2020-2021 Разживин Александр | Razzhivin Alexander
   kotyara12@yandex.ru | https://kotyara12.ru | tg: @kotyara1971
*/

#ifndef RWIFI32_H_
#define RWIFI32_H_ 

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_wifi_types.h"
#include "lwip/ip_addr.h"

#if __has_include("esp_netif.h")
// New version
#define __WIFI_ADAPTER_NETIF__ 1
#include "esp_netif.h"
#else 
// Old version
#define __WIFI_ADAPTER_NETIF__ 0
#include "tcpip_adapter.h"
#endif

typedef enum {
  wifiDisabled     = 0, 
  wifiDisconnected = 1, 
  wifiConnFailed   = 2,
  wifiConnecting   = 3, 
  wifiConnectIdle  = 4,
  wifiConnectInit  = 5, 
  wifiConnectSNTP  = 6, 
  wifiConnected    = 7
} wifiState_t; 

#define WIFI_REASON_PING_FAILED 0xFF

// Функции обратного вызова для отработки реакции на события

// cbWiFiConnectionInit_t - соединение с WiFi-AP установлено, 
// но еще не завершены все внутренние процедуры (синхронизация с SNTP, например)
typedef void (*cbWiFiConnectionInit_t) (const bool isFirstConnect);
// cbWiFiConnectionCheck_t - для проверки доступа к сети интернет, например с помощью ping
// (подключение к AP есть, но доступ в интернет при этом может отсутствовать)
typedef bool (*cbWiFiConnectionCheck_t) (const bool isConnect, TickType_t *nextCheckTimeout);
// cbWiFiConnectionСompleted_t - соединение с WiFi установлено, проверено, время SNTP получено,
// можно запускать зависимые от интернета задачи (MQTT, TS, TG и т.д.)
typedef void (*cbWiFiConnectionCompleted_t) (const bool isFirstConnect);
// cbWiFiConnectionAttemptFailed_t - вызывается при очередной неудачной попытке подключения к WiFi
// (можно записать сообщение в LOG или отправить SMS через SIM800)
typedef void (*cbWiFiConnectionAttemptFailed_t) (const uint16_t tryAttempt, const uint8_t reason);
// cbWiFiConnectionAttemptsExceeded_t - вызывается при исчерпании количества попыток подключения к WiFi
// (можно перегрузить ESP или отправить SMS через SIM800)
typedef void (*cbWiFiConnectionAttemptsExceeded_t) ();
// cbWiFiConnectionLost_t - вызывается при потере подключения к WiFi
// ("упала" точка доступа, или пропал доступ в интернет)
typedef void (*cbWiFiConnectionLost_t) (const uint8_t reason);

void wifiSetCallback_ConnectionInit(cbWiFiConnectionInit_t cb);
void wifiSetCallback_ConnectionCheck(cbWiFiConnectionCheck_t cb);
void wifiSetCallback_ConnectionCompleted(cbWiFiConnectionCompleted_t cb);
void wifiSetCallback_ConnectionAttemptFailed(cbWiFiConnectionAttemptFailed_t cb);
void wifiSetCallback_ConnectionAttemptsExceeded(cbWiFiConnectionAttemptsExceeded_t cb);
void wifiSetCallback_ConnectionLost(cbWiFiConnectionLost_t cb);

void wifiInit();
bool wifiStart();
bool wifiStop();

wifi_mode_t wifiMode();
wifiState_t wifiStatus();
bool wifiIsConnected();
bool wifiWaitConnection(const uint32_t timeout_ms);
wifi_ap_record_t wifiInfo();
int8_t wifiRSSI();
#if __WIFI_ADAPTER_NETIF__
esp_netif_ip_info_t wifiLocalIP();
#else
tcpip_adapter_ip_info_t wifiLocalIP();
#endif // __WIFI_ADAPTER_NETIF__
const char* wifiGetHostname();
ip_addr_t wifiHostByName(const char* aHostname);

#ifdef __cplusplus
}
#endif

#endif // RWIFI32_H_