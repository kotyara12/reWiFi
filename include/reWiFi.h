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

typedef enum {
  wifiCheckOk      = 0,
  wifiCheckFailed  = 1,
  wifiCheckBadGateway = 2
} wifiCheckResult_t;  

#define WIFI_REASON_GATEWAY_FAILED 0xFE
#define WIFI_REASON_PING_FAILED 0xFF

// Callback functions for handling event responses

// cbWiFiConnectionInit_t - connection with WiFi-AP is established,
// but not yet completed all internal procedures (synchronization with SNTP, for example)
typedef void (*cbWiFiConnectionInit_t) (const bool isFirstConnect);
// cbWiFiConnectionCheck_t - to check access to the Internet, for example, using ping 
// (there is a connection to the AP, but there may be no Internet access)
typedef wifiCheckResult_t (*cbWiFiConnectionCheck_t) (const bool isConnect, TickType_t *nextCheckTimeout);
// cbWiFiConnectionСompleted_t - WiFi connection established, verified, SNTP time received, 
// internet dependent tasks can be run (MQTT, TS, TG, etc.)
typedef void (*cbWiFiConnectionCompleted_t) (const bool isFirstConnect);
// cbWiFiConnectionAttemptFailed_t - called when another unsuccessful attempt to connect to WiFi 
// (you can write a message to LOG or send SMS via SIM800)
typedef void (*cbWiFiConnectionAttemptFailed_t) (const uint16_t tryAttempt, const uint8_t reason);
// cbWiFiConnectionAttemptsExceeded_t - called when the number of attempts to connect to WiFi is exhausted
// (you can write a message to LOG or send SMS via SIM800)
typedef void (*cbWiFiConnectionAttemptsExceeded_t) ();
// cbWiFiConnectionLost_t - is called when the connection to WiFi is lost 
// (the access point "dropped", or the Internet access is lost)
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
char* wifiGetLocalIP();
char* wifiGetGatewayIP();
const char* wifiGetHostname();
ip_addr_t wifiHostByName(const char* aHostname);

#ifdef __cplusplus
}
#endif

#endif // __RE_WIFI_H__