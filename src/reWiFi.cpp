/* 
   EN: Module for automatically maintaining a constant connection to WiFi in STA mode
   RU: Модуль для автоматического поддержания постоянного подключения к WiFi в режиме STA
   --------------------------
   (с) 2020-2021 Разживин Александр | Razzhivin Alexander
   kotyara12@yandex.ru | https://kotyara12.ru | tg: @kotyara1971
*/

#include "rLog.h"
#include "rTypes.h"
#include "reWiFi.h"
#include "reEsp32.h"
#include "rStrings.h"
#include "reNvs.h"
#include "reLedSys.h"
#include "project_config.h"
#include <cstring>
#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "lwip/opt.h"
#include "lwip/err.h"
#include "lwip/dns.h"
#include <time.h>
#include <sys/time.h> 
#if __has_include("lwip/apps/sntp.h")
#include "lwip/apps/sntp.h"
#else
#include "esp_sntp.h"
#endif // #include "lwip/apps/sntp.h"
#if CONFIG_TELEGRAM_ENABLE
#include "reTgSend.h"
#endif // CONFIG_TELEGRAM_ENABLE

static const char * wifiTAG = "WiFi";
static const char * sntpTAG = "SNTP";

static const int STA_STARTED_BIT    = BIT0;
static const int STA_CONNECTED_BIT  = BIT1;
static const int STA_HAS_IP_BIT     = BIT2;
static const int STA_HAS_IP6_BIT    = BIT3;
static const int WIFI_DNS_IDLE_BIT  = BIT4;
static const int WIFI_DNS_DONE_BIT  = BIT5;
static const int WIFI_SNTP_SYNC_BIT = BIT6;
static const int INET_AVAILABLE_BIT = BIT7;

// -----------------------------------------------------------------------------------------------------------------------
// -------------------------------------------- Callback functions -------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

cbWiFiConnectionInit_t _cbWiFi_Init;
cbWiFiConnectionCheck_t _cbWiFi_Check;
cbWiFiConnectionCompleted_t _cbWiFi_Completed;
cbWiFiConnectionAttemptFailed_t _cbWiFi_AttemptFailed;
cbWiFiConnectionAttemptsExceeded_t _cbWiFi_AttemptsExceeded;
cbWiFiConnectionLost_t _cbWiFi_Lost;

void wifiSetCallback_ConnectionInit(cbWiFiConnectionInit_t cb)
{
  _cbWiFi_Init = cb;
}

void wifiSetCallback_ConnectionCheck(cbWiFiConnectionCheck_t cb)
{
  _cbWiFi_Check = cb;
}

void wifiSetCallback_ConnectionCompleted(cbWiFiConnectionCompleted_t cb)
{
  _cbWiFi_Completed = cb;
}

void wifiSetCallback_ConnectionAttemptFailed(cbWiFiConnectionAttemptFailed_t cb)
{
  _cbWiFi_AttemptFailed = cb;
}

void wifiSetCallback_ConnectionAttemptsExceeded(cbWiFiConnectionAttemptsExceeded_t cb)
{
  _cbWiFi_AttemptsExceeded = cb;
}

void wifiSetCallback_ConnectionLost(cbWiFiConnectionLost_t cb)
{
  _cbWiFi_Lost = cb;
}

// -----------------------------------------------------------------------------------------------------------------------
// -------------------------------------------- Network status (STA) -----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static wifiState_t _staStatus = wifiDisabled;
static EventGroupHandle_t _sta_status_group = NULL;
#if __WIFI_ADAPTER_NETIF__
static esp_netif_t *netif;
#endif // __WIFI_ADAPTER_NETIF__

void setStatus(wifiState_t newStatus)
{
  if (!_sta_status_group) {
    _sta_status_group = xEventGroupCreate();
    if (!_sta_status_group) {
      rlog_e(wifiTAG, "Error creating STA status group!");
      _staStatus = newStatus;
      return;
    };
  };

  xEventGroupClearBits(_sta_status_group, 0x00FFFFFF);
  xEventGroupSetBits(_sta_status_group, newStatus);
}

wifiState_t wifiStatus()
{
  if (!_sta_status_group) {
    return _staStatus;
  };

  return (wifiState_t)xEventGroupClearBits(_sta_status_group, 0);
}

bool wifiIsConnected()
{
  return (wifiStatus() == wifiConnected);
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------- Network event group -------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool wifiConnectSTA();
bool wifiDisconnectSTA();
bool wifiReconnectSTA();

static EventGroupHandle_t _network_event_group = NULL;

int setStatusBits(int bits)
{
  if (!_network_event_group) {
    return 0;
  };
  return xEventGroupSetBits(_network_event_group, bits);
}

int clearStatusBits(int bits)
{
  if (!_network_event_group) {
    return 0;
  };
  return xEventGroupClearBits(_network_event_group, bits);
}

int getStatusBits() 
{
  if (!_network_event_group) {
    return 0;
  };
  return xEventGroupGetBits(_network_event_group);
}

int waitStatusBits (int bits, uint32_t timeout_ms)
{
  if (!_network_event_group) {
    return 0;
  };  
  if (timeout_ms == 0) {
    return xEventGroupWaitBits (_network_event_group, bits, pdFALSE, pdTRUE, portMAX_DELAY ) & bits; 
  }
  else {
    return xEventGroupWaitBits (_network_event_group, bits, pdFALSE, pdTRUE, timeout_ms / portTICK_PERIOD_MS ) & bits; 
  };
}

bool wifiWaitConnection(const uint32_t timeout_ms)
{
  return waitStatusBits(STA_CONNECTED_BIT && STA_HAS_IP_BIT && INET_AVAILABLE_BIT, timeout_ms) > 0;
}

// -----------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------------- DNS -------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static void _wifiDnsFoundCallback(const char *name, const ip_addr_t *ipaddr, void *callback_arg)
{
  if (ipaddr) {
    (*reinterpret_cast<ip_addr_t*>(callback_arg)) = *ipaddr;
  }
  setStatusBits(WIFI_DNS_DONE_BIT);
}

ip_addr_t wifiHostByName(const char* aHostname)
{
  ip_addr_t addr = IPADDR4_INIT(0);
  // ip_addr_t addr_dns = IPADDR4_INIT_BYTES(8, 8, 8, 8);
  rlog_d(wifiTAG, "Determining address for host [%s]...", aHostname);
  waitStatusBits(WIFI_DNS_IDLE_BIT, 5000);
  clearStatusBits(WIFI_DNS_IDLE_BIT);

  // dns_setserver(1, &addr_dns);
  err_t err = dns_gethostbyname(aHostname, &addr, &_wifiDnsFoundCallback, &addr);
  if (err == ERR_INPROGRESS) {
    waitStatusBits(WIFI_DNS_DONE_BIT, 4000);
    clearStatusBits(WIFI_DNS_DONE_BIT);
  };
  setStatusBits(WIFI_DNS_IDLE_BIT);

  if (addr.u_addr.ip4.addr == 0) {
    rlog_e(wifiTAG, "Error getting DNS address for host [%s]", aHostname);
  }
  else {
    uint8_t * ip = (uint8_t*)&(addr.u_addr.ip4.addr);
    rlog_d(wifiTAG, "IP address obtained for host [ %s ]: %d.%d.%d.%d", aHostname, ip[0], ip[1], ip[2], ip[3]);
  };

  return addr;
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------- SNTP synchronization ------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

void sntpSyncNotification(struct timeval *tv)
{
  time_t now = 0;
  struct tm timeinfo;
  char strftime_buf[20];

  time(&now);
  localtime_r(&now, &timeinfo);
  if (timeinfo.tm_year < (1970 - 1900)) {
    clearStatusBits(WIFI_SNTP_SYNC_BIT);
    rlog_e(sntpTAG, "Time synchronization failed!");
  }
  else {
    setStatusBits(WIFI_SNTP_SYNC_BIT);
    strftime(strftime_buf, sizeof(strftime_buf), "%d.%m.%Y %H:%M:%S", &timeinfo);
    ledSysFlashOn(3, 100, 100);
    rlog_i(sntpTAG, "Time synchronization completed, current time: %s", strftime_buf);
  };
}

void sntpStopSNTP()
{
  if (sntp_enabled()) {
    sntp_stop();
    rlog_i(sntpTAG, "Time synchronization stopped");
  };
}

bool sntpStartSNTP()
{
  rlog_i(sntpTAG, "Starting time synchronization with SNTP servers for a zone %s...", CONFIG_SNTP_TIMEZONE);

  // Stop time synchronization if it was started
  sntpStopSNTP();

  // Configuring synchronization parameters
  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  #if defined(CONFIG_SNTP_TIMEZONE)
  setenv("TZ", CONFIG_SNTP_TIMEZONE, 1);
  tzset(); 
  #endif
  #if defined(CONFIG_SNTP_SERVER0)
  sntp_setservername(0, (char*)CONFIG_SNTP_SERVER0);
  #endif
  #if defined(CONFIG_SNTP_SERVER1)
  sntp_setservername(1, (char*)CONFIG_SNTP_SERVER1);
  #endif
  #if defined(CONFIG_SNTP_SERVER2)
  sntp_setservername(2, (char*)CONFIG_SNTP_SERVER2);
  #endif
  #if defined(CONFIG_SNTP_SERVER3)
  sntp_setservername(3, (char*)CONFIG_SNTP_SERVER3);
  #endif
  #if defined(CONFIG_SNTP_SERVER4)
  sntp_setservername(4, (char*)CONFIG_SNTP_SERVER4);
  #endif
  sntp_set_time_sync_notification_cb(sntpSyncNotification); 

  // Start synchronization and wait 180 seconds for completion
  clearStatusBits(WIFI_SNTP_SYNC_BIT);
  sntp_init();
  return waitStatusBits(WIFI_SNTP_SYNC_BIT, 180000) > 0;
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------- Network events task -------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

typedef struct {
  esp_event_base_t event_base;
  int32_t event_id;
  void* event_data;
} wifiEvent_t;

#define NETWORK_EVENT_TASK_NAME "network_event"
#define NETWORK_EVENT_TASK_PRIO ESP_TASKD_EVENT_PRIO - 1

#ifdef CONFIG_WIFI_STATIC_ALLOCATION
#define NETWORK_EVENT_STATIC_ALLOCATION CONFIG_WIFI_STATIC_ALLOCATION
#else
#define NETWORK_EVENT_STATIC_ALLOCATION 0
#endif

#ifdef CONFIG_ARDUINO_EVENT_RUNNING_CORE
#define NETWORK_EVENT_RUNNING_CORE CONFIG_ARDUINO_EVENT_RUNNING_CORE
#else
#define NETWORK_EVENT_RUNNING_CORE 1
#endif

#ifdef CONFIG_SYSTEM_EVENT_QUEUE_SIZE
#define NETWORK_EVENT_QUEUE_SIZE CONFIG_SYSTEM_EVENT_QUEUE_SIZE
#else
#define NETWORK_EVENT_QUEUE_SIZE 32
#endif

#define NETWORK_EVENT_QUEUE_ITEM_SIZE sizeof(wifiEvent_t)

#ifdef CONFIG_SYSTEM_EVENT_TASK_STACK_SIZE
#define NETWORK_EVENT_TASK_STACK_SIZE CONFIG_SYSTEM_EVENT_TASK_STACK_SIZE
#else
#define NETWORK_EVENT_TASK_STACK_SIZE 3072
#endif

#define CONFIG_WIFI_NVS_GROUP "wifi"
#define CONFIG_WIFI_NVS_INDEX "index"

static xQueueHandle _network_event_queue;
static TaskHandle_t _network_event_task_handle = NULL;

#if NETWORK_EVENT_STATIC_ALLOCATION
StaticEventGroup_t _network_event_group_buffer;
StaticQueue_t _network_event_queue_buffer;
uint8_t _network_event_queue_storage[NETWORK_EVENT_QUEUE_SIZE * NETWORK_EVENT_QUEUE_ITEM_SIZE];
StaticTask_t _network_event_task_buffer;
StackType_t _network_event_task_stack[NETWORK_EVENT_TASK_STACK_SIZE];
#endif // NETWORK_EVENT_STATIC_ALLOCATION

uint8_t wifiGetMaxIndex()
{
  #if defined(CONFIG_WIFI_SSID)
    return (uint8_t)0;
  #elif defined(CONFIG_WIFI_5_SSID)
    return (uint8_t)5;
  #elif defined(CONFIG_WIFI_4_SSID)
    return (uint8_t)4;
  #elif defined(CONFIG_WIFI_3_SSID)
    return (uint8_t)3;
  #elif defined(CONFIG_WIFI_2_SSID)
    return (uint8_t)2;
  #elif defined(CONFIG_WIFI_1_SSID)
    return (uint8_t)1;
  #endif
}

const char* wifiGetSSID(const uint8_t index)
{
  #ifdef CONFIG_WIFI_SSID
    // Single network mode
    return CONFIG_WIFI_SSID;
  #else
    // Multi-network mode
    switch (index) {
      case 1: return CONFIG_WIFI_1_SSID;
      #ifdef CONFIG_WIFI_2_SSID
      case 2: return CONFIG_WIFI_2_SSID;
      #endif // CONFIG_WIFI_2_SSID
      #ifdef CONFIG_WIFI_3_SSID
      case 3: return CONFIG_WIFI_3_SSID;
      #endif // CONFIG_WIFI_3_SSID
      #ifdef CONFIG_WIFI_4_SSID
      case 4: return CONFIG_WIFI_4_SSID;
      #endif // CONFIG_WIFI_4_SSID
      #ifdef CONFIG_WIFI_5_SSID
      case 5: return CONFIG_WIFI_5_SSID;
      #endif // CONFIG_WIFI_5_SSID
      default: return CONFIG_WIFI_1_SSID;
    };
  #endif // CONFIG_WIFI_SSID
}

void wifiSetConfig(const uint8_t index)
{
  wifi_config_t conf;
  memset(&conf, 0, sizeof(wifi_config_t));
  #ifdef CONFIG_WIFI_SSID
    // Single network mode
    strcpy(reinterpret_cast<char*>(conf.sta.ssid), CONFIG_WIFI_SSID);
    strcpy(reinterpret_cast<char*>(conf.sta.password), CONFIG_WIFI_PASS);
  #else
    // Multi-network mode
    switch (index) {
      case 1:
        strcpy(reinterpret_cast<char*>(conf.sta.ssid), CONFIG_WIFI_1_SSID);
        strcpy(reinterpret_cast<char*>(conf.sta.password), CONFIG_WIFI_1_PASS);
        break;
      #ifdef CONFIG_WIFI_2_SSID
      case 2:
        strcpy(reinterpret_cast<char*>(conf.sta.ssid), CONFIG_WIFI_2_SSID);
        strcpy(reinterpret_cast<char*>(conf.sta.password), CONFIG_WIFI_2_PASS);
        break;
      #endif // CONFIG_WIFI_2_SSID
      #ifdef CONFIG_WIFI_3_SSID
      case 3:
        strcpy(reinterpret_cast<char*>(conf.sta.ssid), CONFIG_WIFI_3_SSID);
        strcpy(reinterpret_cast<char*>(conf.sta.password), CONFIG_WIFI_3_PASS);
        break;
      #endif // CONFIG_WIFI_3_SSID
      #ifdef CONFIG_WIFI_4_SSID
      case 4:
        strcpy(reinterpret_cast<char*>(conf.sta.ssid), CONFIG_WIFI_4_SSID);
        strcpy(reinterpret_cast<char*>(conf.sta.password), CONFIG_WIFI_4_PASS);
        break;
      #endif // CONFIG_WIFI_4_SSID
      #ifdef CONFIG_WIFI_5_SSID
      case 5:
        strcpy(reinterpret_cast<char*>(conf.sta.ssid), CONFIG_WIFI_5_SSID);
        strcpy(reinterpret_cast<char*>(conf.sta.password), CONFIG_WIFI_5_PASS);
        break;
      #endif // CONFIG_WIFI_5_SSID
      default:
        strcpy(reinterpret_cast<char*>(conf.sta.ssid), CONFIG_WIFI_1_SSID);
        strcpy(reinterpret_cast<char*>(conf.sta.password), CONFIG_WIFI_1_PASS);
        break;
    };
  #endif // CONFIG_WIFI_SSID
  esp_wifi_set_config(WIFI_IF_STA, &conf);
}

bool wifiDoInternetAvailable(const bool isFirstConnect)
{
  setStatusBits(INET_AVAILABLE_BIT);

  // If we got here, we have Internet access, we get time
  setStatus(wifiConnectSNTP);
  if (sntpStartSNTP()) {
    // We set marks that the connection was successfully completed
    setStatus(wifiConnected);
    ledSysStateSet(SYSLED_WIFI_CONNECTED | SYSLED_WIFI_INET_AVAILABLE, false);

    // Calling the callback function when establishing a connection
    if (_cbWiFi_Completed) _cbWiFi_Completed(isFirstConnect); 

    return true;
  };
  return false;
}

void wifiDoInternetUnavailable()
{
  rlog_e(wifiTAG, "Lost access to the Internet!");

  // Stop synchronizing time
  sntpStopSNTP();

  clearStatusBits(INET_AVAILABLE_BIT);
  setStatus(wifiConnectInit);
  ledSysStateClear(SYSLED_WIFI_INET_AVAILABLE, false);

  // Calling the callback function when the connection is lost
  if (_cbWiFi_Lost) { _cbWiFi_Lost(WIFI_REASON_PING_FAILED); };
}

void wifiDoGatewayUnavailable()
{
  rlog_e(wifiTAG, "Lost access to gateway!");

  // Stop synchronizing time
  sntpStopSNTP();

  clearStatusBits(INET_AVAILABLE_BIT | STA_HAS_IP_BIT | STA_CONNECTED_BIT);
  setStatus(wifiConnectInit);
  ledSysStateClear(SYSLED_WIFI_CONNECTED | SYSLED_WIFI_INET_AVAILABLE, false);

  // Calling the callback function when the connection is lost
  if (_cbWiFi_Lost) { _cbWiFi_Lost(WIFI_REASON_GATEWAY_FAILED); };
}

static void wifiEventTask(void * arg) 
{
  wifiEvent_t event;
  uint16_t tryConnect = 0;
  bool isFisrtConnect = true;
  TickType_t waitTimeout = portMAX_DELAY;
  #ifndef CONFIG_WIFI_SSID
  uint8_t wifiIndex = 1;
  uint8_t wifiMaxIndex = wifiGetMaxIndex();
  bool wifiIndexChanged = false;
  #endif // CONFIG_WIFI_SSID
  wifiCheckResult_t inetStatus = wifiCheckOk;

  #define wifiIndexSetNext() do { \
    wifiIndexChanged = true; \
    if (++wifiIndex > wifiMaxIndex) wifiIndex = 1; \
    rlog_w(wifiTAG, "Switching to another network # %d", wifiIndex); \
    wifiSetConfig(wifiIndex); \
  } while(0);

  #define nextTryConnect() do { \
    tryConnect++; \
    if (tryConnect <= CONFIG_WIFI_MAX_ATTEMPTS) { \
      rlog_i(wifiTAG, "Connecting to WiFi network [ %s ], trying %d...", wifiGetSSID(wifiIndex), tryConnect); \
      waitTimeout = CONFIG_WIFI_TIMEOUT / portTICK_PERIOD_MS; \
      wifiReconnectSTA(); \
    } \
    else { \
      rlog_e(wifiTAG, "Connecting to WiFi is not done - exceeded limit of number of connection attempts (%d)!", CONFIG_WIFI_MAX_ATTEMPTS); \
      ledSysStateSet(SYSLED_ERROR, false); \
      if (_cbWiFi_AttemptsExceeded) { _cbWiFi_AttemptsExceeded(); }; \
      vTaskDelay(CONFIG_WIFI_EXCEEDING_MAX_ATTEMPTS_DELAY * 1000 / portTICK_PERIOD_MS); \
      rlog_i(wifiTAG, "Connecting to WiFi network [ %s ], trying %d...", wifiGetSSID(wifiIndex), tryConnect); \
      waitTimeout = CONFIG_WIFI_TIMEOUT / portTICK_PERIOD_MS; \
      wifiReconnectSTA(); \
    }; \
  } while(0);

  #ifndef CONFIG_WIFI_SSID
    // Multi-network mode
    nvsRead(CONFIG_WIFI_NVS_GROUP, CONFIG_WIFI_NVS_INDEX, OPT_TYPE_U8, &wifiIndex);
    wifiIndexChanged = false;
  #endif // CONFIG_WIFI_SSID

  while (true) {
    if (xQueueReceive(_network_event_queue, &event, waitTimeout) == pdTRUE) {
      if (event.event_base == WIFI_EVENT) {
        // WIFI_EVENT :: WIFI_EVENT_STA_START :: ESP32 station start
        if (event.event_id ==  WIFI_EVENT_STA_START) {
          setStatusBits(STA_STARTED_BIT);
          setStatus(wifiConnecting);
          ledSysStateClear(SYSLED_WIFI_CONNECTED | SYSLED_WIFI_INET_AVAILABLE | SYSLED_WIFI_ERROR, false);
          
          rlog_i(wifiTAG, "WiFi STA started");
          
          // Start DHCP client
          if (tcpip_adapter_dhcpc_start(TCPIP_ADAPTER_IF_STA) == ESP_ERR_TCPIP_ADAPTER_DHCPC_START_FAILED) {
            ledSysStateSet(SYSLED_WIFI_ERROR, true);
            rlog_e(wifiTAG, "DHCP client start failed!");
            waitTimeout = portMAX_DELAY;
            wifiStop();
          }
          else {
            tryConnect = 1;
            rlog_i(wifiTAG, "Connecting to WiFi network [ %s ]...", wifiGetSSID(wifiIndex));

            // Initializing configuration WiFi STA
            #ifdef CONFIG_WIFI_SSID
              wifiSetConfig(0);
            #else
              wifiSetConfig(wifiIndex);
            #endif // CONFIG_WIFI_SSID

            // We start the connection
            waitTimeout = CONFIG_WIFI_TIMEOUT / portTICK_PERIOD_MS;
            wifiConnectSTA();
          };
        } 
        // WIFI_EVENT :: WIFI_EVENT_STA_START :: ESP32 station start

        // WIFI_EVENT :: WIFI_EVENT_STA_STOP :: ESP32 station stop
        else if (event.event_id ==  WIFI_EVENT_STA_STOP) {
          int prevStatusBits = getStatusBits();

          rlog_d(wifiTAG, "WiFi station stop...");

          sntpStopSNTP();
          clearStatusBits(STA_STARTED_BIT | STA_CONNECTED_BIT | STA_HAS_IP_BIT | STA_HAS_IP6_BIT | INET_AVAILABLE_BIT);
          setStatus(wifiDisabled);
          ledSysStateClear(SYSLED_WIFI_CONNECTED | SYSLED_WIFI_INET_AVAILABLE, false);
          ledSysStateSet(SYSLED_WIFI_ERROR, true);
          waitTimeout = 0;
          
          // Disable DHCP client
          tcpip_adapter_dhcpc_stop(TCPIP_ADAPTER_IF_STA);

          rlog_i(wifiTAG, "WiFi is disabled!");

          // Checking if the connection has been completed until now
          if ((prevStatusBits & STA_HAS_IP_BIT) || (prevStatusBits & STA_HAS_IP6_BIT)) {
            // Calling the callback function when the connection is lost
            if (_cbWiFi_Lost) { _cbWiFi_Lost(WIFI_REASON_UNSPECIFIED); };
          };
        }
        // WIFI_EVENT :: WIFI_EVENT_STA_STOP :: ESP32 station stop

        // WIFI_EVENT :: WIFI_EVENT_STA_CONNECTED :: ESP32 station connected to AP
        else if (event.event_id ==  WIFI_EVENT_STA_CONNECTED) {
          setStatusBits(STA_CONNECTED_BIT);
          setStatus(wifiConnectIdle);
          ledSysStateClear(SYSLED_WIFI_ERROR, false);
          if (tryConnect >= CONFIG_WIFI_MAX_ATTEMPTS) {
            ledSysStateClear(SYSLED_ERROR, false);
          };
          waitTimeout = CONFIG_WIFI_TIMEOUT / portTICK_PERIOD_MS;

          rlog_i(wifiTAG, "WiFi connection [ %s ] established, RSSI: %d dBi", wifiGetSSID(wifiIndex), wifiRSSI());

          #ifndef CONFIG_WIFI_SSID
            // Multi-network mode
            if (wifiIndexChanged) {
              nvsWrite(CONFIG_WIFI_NVS_GROUP, CONFIG_WIFI_NVS_INDEX, OPT_TYPE_U8, &wifiIndex);
            };
          #endif // CONFIG_WIFI_SSID
        }
        // WIFI_EVENT :: WIFI_EVENT_STA_CONNECTED :: ESP32 station connected to AP

        // WIFI_EVENT :: WIFI_EVENT_STA_DISCONNECTED :: ESP32 station disconnected from AP
        else if (event.event_id ==  WIFI_EVENT_STA_DISCONNECTED) {
          int prevStatusBits = getStatusBits();
          wifi_event_sta_disconnected_t * data = (wifi_event_sta_disconnected_t*)event.event_data;

          rlog_d(wifiTAG, "WiFi station disconnected from AP...");
          
          sntpStopSNTP();
          clearStatusBits(STA_CONNECTED_BIT | STA_HAS_IP_BIT | STA_HAS_IP6_BIT | INET_AVAILABLE_BIT);
          ledSysStateClear(SYSLED_WIFI_CONNECTED | SYSLED_WIFI_INET_AVAILABLE, false);
          
          // Checking if the connection has been completed until now
          if ((prevStatusBits & STA_HAS_IP_BIT) | (prevStatusBits & STA_HAS_IP6_BIT)) {
            setStatus(wifiDisconnected);
            rlog_e(wifiTAG, "WiFi connection [ %s ] lost: %d!", wifiGetSSID(wifiIndex), data->reason);
            
            // Calling the callback function when the connection is lost
            if (_cbWiFi_Lost) { _cbWiFi_Lost(data->reason); };
          }
          else {
            setStatus(wifiConnFailed);
            rlog_e(wifiTAG, "WiFi connection error: #%d!", data->reason);
            // rlog_e(wifiTAG, "WiFi connection error: %s!", reason2str(data->reason));
           
            #ifndef CONFIG_WIFI_SSID
              // Multi-network mode
              wifiIndexSetNext();
            #endif // CONFIG_WIFI_SSID

            // Calling the callback function on unsuccessful connection attempt
            if (_cbWiFi_AttemptFailed) { _cbWiFi_AttemptFailed(tryConnect, data->reason); };
          };
          // Next connection attempt
          nextTryConnect();
        }
        // WIFI_EVENT :: WIFI_EVENT_STA_DISCONNECTED :: ESP32 station disconnected from AP

        // WIFI_EVENT :: OTHER (WIFI_EVENT_WIFI_READY, WIFI_EVENT_SCAN_DONE, WIFI_EVENT_STA_AUTHMODE_CHANGE, WIFI_EVENT_STA_WPS_..., WIFI_EVENT_AP_...) 
        else {
          rlog_d(wifiTAG, "Unsupported event type: %d", event.event_id);
        }
        // WIFI_EVENT :: OTHER (WIFI_EVENT_WIFI_READY, WIFI_EVENT_SCAN_DONE, WIFI_EVENT_STA_AUTHMODE_CHANGE, WIFI_EVENT_STA_WPS_..., WIFI_EVENT_AP_...) 
      }
      else if (event.event_base == IP_EVENT) {
        // IP_EVENT :: IP_EVENT_STA_GOT_IP :: ESP32 station got IP from connected AP
        if (event.event_id == IP_EVENT_STA_GOT_IP) {
          ip_event_got_ip_t * data = (ip_event_got_ip_t*)event.event_data;
          uint8_t * ip = (uint8_t*)&(data->ip_info.ip.addr);
          uint8_t * mask = (uint8_t*)&(data->ip_info.netmask.addr);
          uint8_t * gw = (uint8_t*)&(data->ip_info.gw.addr);

          setStatusBits(STA_HAS_IP_BIT);
          setStatus(wifiConnectInit);
          ledSysStateSet(SYSLED_WIFI_CONNECTED, false);
          waitTimeout = portMAX_DELAY;

          rlog_i(wifiTAG, "Got IP-address: %d.%d.%d.%d, mask: %d.%d.%d.%d, gateway: %d.%d.%d.%d",
              ip[0], ip[1], ip[2], ip[3], mask[0], mask[1], mask[2], mask[3], gw[0], gw[1], gw[2], gw[3]);
          
          // Calling the callback function when initializing the connection
          if (_cbWiFi_Init) _cbWiFi_Init(isFisrtConnect); 

          // We check the availability of the Internet (ping, for example): the connection is established, but there may not be access to the Internet
          if (_cbWiFi_Check) {
            inetStatus = _cbWiFi_Check(true, &waitTimeout);
            // If the Internet is not available, we wait until it appears
            while (inetStatus == wifiCheckFailed) {
              vTaskDelay(waitTimeout);
              inetStatus = _cbWiFi_Check(true, &waitTimeout);
            };
            // The gateway does not ping, you need to reconnect
            if (inetStatus != wifiCheckOk) {
              ledSysStateSet(SYSLED_WIFI_ERROR, false);
              // Calling the callback function on unsuccessful connection attempt
              if (_cbWiFi_AttemptFailed) { _cbWiFi_AttemptFailed(tryConnect, WIFI_REASON_UNSPECIFIED); };
              // Switching to another network in multi-mode
              #ifndef CONFIG_WIFI_SSID
                wifiIndexSetNext();
              #endif // CONFIG_WIFI_SSID
              // Next connection attempt
              nextTryConnect();
            };
          };
          
          // If we got here, we have Internet access, we get SNTP time
          if (wifiDoInternetAvailable(isFisrtConnect)) {
            isFisrtConnect = false;
            tryConnect = 0;
          } else {
            ledSysStateSet(SYSLED_WIFI_ERROR, false);
            // Calling the callback function on unsuccessful connection attempt
            if (_cbWiFi_AttemptFailed) { _cbWiFi_AttemptFailed(tryConnect, WIFI_REASON_UNSPECIFIED); };
            // Next connection attempt
            nextTryConnect();
          };
        } 
        // IP_EVENT :: IP_EVENT_STA_GOT_IP :: ESP32 station got IP from connected AP

        // IP_EVENT :: IP_EVENT_GOT_IP6 :: ESP32 station or ap or ethernet interface v6IP addr is preferred
        else if (event.event_id == IP_EVENT_GOT_IP6) {
          setStatusBits(STA_HAS_IP6_BIT);

          rlog_i(wifiTAG, "Received IP version 6");
        } 
        // IP_EVENT :: IP_EVENT_GOT_IP6 :: ESP32 station or ap or ethernet interface v6IP addr is preferred

        // IP_EVENT :: IP_EVENT_STA_LOST_IP ::  ESP32 station lost IP and the IP is reset to 0
        else if (event.event_id == IP_EVENT_STA_LOST_IP) {
          sntpStopSNTP();
          setStatus(wifiConnectIdle);
          clearStatusBits(STA_HAS_IP_BIT | STA_HAS_IP6_BIT | INET_AVAILABLE_BIT);

          ledSysStateClear(SYSLED_WIFI_CONNECTED | SYSLED_WIFI_INET_AVAILABLE, false);
          rlog_e(wifiTAG, "Lost WiFi IP address!");

          // Calling the callback function when the connection is lost
          if (_cbWiFi_Lost) { _cbWiFi_Lost(WIFI_REASON_UNSPECIFIED); };
        }
        // IP_EVENT :: IP_EVENT_STA_LOST_IP ::  ESP32 station lost IP and the IP is reset to 0

        // IP_EVENT :: OTHER (IP_EVENT_ETH_GOT_IP, IP_EVENT_AP_STAIPASSIGNED) 
        else {
          rlog_d(wifiTAG, "Unsupported event type: %d", event.event_id);
        };
        // IP_EVENT :: OTHER (IP_EVENT_ETH_GOT_IP, IP_EVENT_AP_STAIPASSIGNED) 
      }
      // OTHER (UNSUPPORTED)
      else {
        rlog_w(wifiTAG, "Unsupported event category!");
      };
      // OTHER (UNSUPPORTED)
    } else {
      // Timeout for waiting for an event from the queue
      if (wifiStatus() >= wifiConnectInit) {
        if (_cbWiFi_Check) {
          // Periodic check of Internet availability
          wifiCheckResult_t inetNewStatus = _cbWiFi_Check(false, &waitTimeout);
          // If the status has been changed...
          if (inetNewStatus != inetStatus) {
            inetStatus = inetNewStatus;
            switch (inetStatus) {
              // All is well with network access, you can continue
              case wifiCheckOk:
                if (!wifiDoInternetAvailable(false)) {
                  ledSysStateSet(SYSLED_WIFI_ERROR, false);
                  // Calling the callback function on unsuccessful connection attempt
                  if (_cbWiFi_AttemptFailed) { _cbWiFi_AttemptFailed(tryConnect, WIFI_REASON_UNSPECIFIED); };
                  // Next connection attempt
                  nextTryConnect();
                };              
                break;

              // Internet is not available, network processes need to be suspended
              case wifiCheckFailed:
                wifiDoInternetUnavailable();
                break;

              // Gateway is not available, you need to reconnect
              case wifiCheckBadGateway:
                wifiDoGatewayUnavailable();
                nextTryConnect();
                break;
            };
          };
        };
      } else {
        // Hung on connection
        if (_cbWiFi_AttemptFailed) { _cbWiFi_AttemptFailed(tryConnect, WIFI_REASON_UNSPECIFIED); };
        nextTryConnect();
      };
    };
  };

  vTaskDelete(NULL);
  _network_event_task_handle = NULL;
}

static void wifiEventCallback(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) 
{
  wifiEvent_t event;
  event.event_base = event_base;
  event.event_id = event_id;
  event.event_data = event_data;

  if (xQueueSend(_network_event_queue, &event, portMAX_DELAY) != pdPASS) 
  {
    rlog_w(wifiTAG, "Error sending network queue event!");
  };
}

static bool wifiEventTaskStart()
{
  if (!_network_event_group) {
    
    #if NETWORK_EVENT_STATIC_ALLOCATION
      _network_event_group = xEventGroupCreateStatic(&_network_event_group_buffer);
    #else
      _network_event_group = xEventGroupCreate();
    #endif // NETWORK_EVENT_STATIC_ALLOCATION
    if (!_network_event_group) {
      rlog_e(wifiTAG, "Error creating network events group!");
      return false;
    };
    xEventGroupSetBits(_network_event_group, WIFI_DNS_IDLE_BIT);
  };

  if (!_network_event_queue) {
    #if NETWORK_EVENT_STATIC_ALLOCATION
      _network_event_queue = xQueueCreateStatic(NETWORK_EVENT_QUEUE_SIZE, NETWORK_EVENT_QUEUE_ITEM_SIZE, &(_network_event_queue_storage[0]), &_network_event_queue_buffer);
    #else
      _network_event_queue = xQueueCreate(NETWORK_EVENT_QUEUE_SIZE, NETWORK_EVENT_QUEUE_ITEM_SIZE);
    #endif // NETWORK_EVENT_STATIC_ALLOCATION
    if (!_network_event_queue) {
      rlog_e(wifiTAG, "Error creating network events queue!");
      return false;
    };
  };

  if (!_network_event_task_handle) {
    #if NETWORK_EVENT_STATIC_ALLOCATION
      _network_event_task_handle = xTaskCreateStaticPinnedToCore(
        wifiEventTask, NETWORK_EVENT_TASK_NAME, NETWORK_EVENT_TASK_STACK_SIZE, NULL, 
        NETWORK_EVENT_TASK_PRIO, _network_event_task_stack, &_network_event_task_buffer, NETWORK_EVENT_RUNNING_CORE); 
    #else
      xTaskCreatePinnedToCore(wifiEventTask, NETWORK_EVENT_TASK_NAME, NETWORK_EVENT_TASK_STACK_SIZE, NULL, 
        NETWORK_EVENT_TASK_PRIO, &_network_event_task_handle, NETWORK_EVENT_RUNNING_CORE);
    #endif // NETWORK_EVENT_STATIC_ALLOCATION

    if (!_network_event_task_handle) {
      rlog_e(wifiTAG, "Error starting network events task!");
      return false;
    };

    return (esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventCallback, NULL) == ESP_OK)
        && (esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifiEventCallback, NULL) == ESP_OK);
  };

  return true;
}

static void wifiEventTaskStop()
{
  if (_network_event_task_handle) {
    esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, &wifiEventCallback);
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventCallback); 

    vTaskDelete(_network_event_task_handle);
    _network_event_task_handle = NULL;
  };

  if (_network_event_queue) {
    vQueueDelete(_network_event_queue);
    _network_event_queue = NULL;
  };

  if (_network_event_group) {
    vEventGroupDelete(_network_event_group);
    _network_event_group = NULL;
  };
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------- Generic WiFi function -----------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

void tcpipInit()
{
  static bool tcpipInitDone = false;

  if (!tcpipInitDone) {
    tcpipInitDone = true;

    // MAC address initialization
    uint8_t mac[8];
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
      esp_base_mac_addr_set(mac);
    };

    
    #if __WIFI_ADAPTER_NETIF__
    // Initializing the TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    // Run the system network task
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // Initializing WiFi after initializing TCP/IP stack and event loop
    netif = esp_netif_create_default_wifi_sta();
    #else
    // Initializing the TCP/IP stack
    ESP_ERROR_CHECK(tcpip_adapter_init());
    // Run the system network task
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    #endif // __WIFI_ADAPTER_NETIF__
  };
};

static bool _wifiLowLevelInitDone = false;

static bool wifiLowLevelInit()
{
  if (!_wifiLowLevelInitDone) {
    // Initializing TCP-IP and system task
    tcpipInit();

    // We start the task of processing network events
    if (wifiEventTaskStart()) {
      // WiFi initialization with default parameters
      wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
      esp_err_t err = esp_wifi_init(&cfg);
      // In case of error 4353, you need to erase NVS partition 
      if (err == 4353) {
        // ESP32 WiFi driver saves the configuration in NVS, and after changing esp-idf version, 
        // conflicting configuration may exist. You need to erase it and try again
        nvsInit();
        err = esp_wifi_init(&cfg);
      };
      if (err != ESP_OK) {
        rlog_e(wifiTAG, "Error esp_wifi_init: %d", err);
        return false;
      };

      // Set the storage type of the Wi-Fi configuration in memory
      // esp_wifi_set_storage(WIFI_STORAGE_RAM);

      _wifiLowLevelInitDone = true;
    };
  };
  
  return _wifiLowLevelInitDone;
}

static bool wifiLowLevelDeinit()
{
  if (_wifiLowLevelInitDone) {
    // Stopping the task of processing network events
    wifiEventTaskStop();

    // We free up WiFi resources, we don’t tamper with the TCP-IP stack
    esp_wifi_deinit();

    _wifiLowLevelInitDone = false;
  };

  return true;
}

static bool _esp_wifi_started = false;

static bool espWiFiStart()
{
  // If it's already running, exit
  if(_esp_wifi_started) {
    return true;
  };

  // Launch WiFi
  esp_err_t err = esp_wifi_start();
  if (err != ESP_OK) {
    rlog_e(wifiTAG, "Error esp_wifi_start: %d", err);
    return false;
  };

  _esp_wifi_started = true;
  
  // system_event_t event;
  // event.event_id = SYSTEM_EVENT_WIFI_READY;
  // WiFiGenericClass::_eventCallback(nullptr, &event);

  return true;
};

static bool espWiFiStop()
{
  // If already stopped - exit
  if (!_esp_wifi_started) {
    return true;
  };

  _esp_wifi_started = false;
  
  // Stop WiFi
  esp_err_t err;
  err = esp_wifi_stop();
  if (err) {
    rlog_e(wifiTAG, "Could not stop WiFi! %d", err);
    _esp_wifi_started = true;
    return false;
  };

  // Stopping the task and freeing resources
  return wifiLowLevelDeinit();
}

wifi_mode_t wifiMode()
{
  if(!_wifiLowLevelInitDone){
      return WIFI_MODE_NULL;
  };

  wifi_mode_t mode;
  if (esp_wifi_get_mode(&mode) == ESP_ERR_WIFI_NOT_INIT) {
    rlog_w(wifiTAG, "WiFi not started");
    return WIFI_MODE_NULL;
  };

  return mode;
}

bool wifiSetMode(wifi_mode_t newMode)
{
  // Checking current mode
  wifi_mode_t currMode = wifiMode();
  if (currMode == newMode) {
    return true;
  };

  // The current mode is off, you need to turn it on ...
  if (!currMode && newMode) {
    if (!wifiLowLevelInit()) {
      return false;
    };
  // Current mode - on, you need to turn it off
  } else if (currMode && !newMode) {
    return espWiFiStop();
  };

  // Installing a new mode
  esp_err_t err;
  err = esp_wifi_set_mode(newMode);
  if (err != ESP_OK) {
    rlog_e(wifiTAG, "Could not set mode! %d", err);
    return false;
  };

  // Some new protocol???
  #ifdef CONFIG_WIFI_LONG_RANGE
    if (newMode & WIFI_MODE_STA) {
      err = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
      if (err != ESP_OK) {
        rlog_e(wifiTAG, "Could not enable long range on STA! %d", err);
        return false;
      };
    };

    if (newMode & WIFI_MODE_AP) {
      err = esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_LR);
      if (err != ESP_OK) {
        rlog_e(wifiTAG, "Could not enable long range on AP! %d", err);
        return false;
      };
    };
    #endif

  // Launch WiFi
  if (!espWiFiStart()) {
    return false;
  };

  return true;
}

bool wifiEnableSTA(bool enable)
{
  wifi_mode_t currentMode = wifiMode();
  bool isEnabled = ((currentMode & WIFI_MODE_STA) != 0);

  if (isEnabled != enable) {
    if (enable) {
      return wifiSetMode((wifi_mode_t)(currentMode | WIFI_MODE_STA));
    }
    else {
      return wifiSetMode((wifi_mode_t)(currentMode & (~WIFI_MODE_STA)));
    };
  };

  return true;
}

bool wifiConnectSTA()
{
  if (esp_wifi_connect()) {
    ledSysStateSet(SYSLED_WIFI_ERROR, false);
    rlog_e(wifiTAG, "Connect failed!");
    return false;
  };

  return true;
}

bool wifiDisconnectSTA()
{
  if (esp_wifi_disconnect() != ESP_OK) {
    rlog_e(wifiTAG, "Disconnect failed!");
    return false;
  };

  return true;
}

bool wifiReconnectSTA()
{
  if (wifiMode() & WIFI_MODE_STA) {
    wifiDisconnectSTA();
  };

  return wifiConnectSTA();
}


// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------- Public WiFi function ------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool wifiStop()
{
  if (wifiMode() & WIFI_MODE_STA) {
    return wifiDisconnectSTA() & wifiEnableSTA(false);
  };

  return false;
}

void wifiInit()
{
  // Disconnect if there was any "default" connection
  wifiStop();
}

bool wifiStart()
{
  ledSysStateClear(SYSLED_WIFI_CONNECTED | SYSLED_WIFI_INET_AVAILABLE | SYSLED_WIFI_ERROR, true);

  // Set STA mode (client connection)
  // (all low-level WiFi initialization happens here too)
  if (!wifiEnableSTA(true)) {
    ledSysStateSet(SYSLED_WIFI_ERROR, true);
    rlog_e(wifiTAG, "STA enable failed!");
    return false;
  };

  return true;  
}

wifi_ap_record_t wifiInfo()
{
  wifi_ap_record_t info;
  memset(&info, 0, sizeof(wifi_ap_record_t));

  if (wifiMode() == WIFI_MODE_NULL){
    return info;
  };

  if (!esp_wifi_sta_get_ap_info(&info)) {
    return info;
  };

  return info;
}

int8_t wifiRSSI()
{
  if (wifiMode() == WIFI_MODE_NULL) {
    return 0;
  };

  wifi_ap_record_t info;
  if (!esp_wifi_sta_get_ap_info(&info)) {
    return info.rssi;
  };

  return 0;
}

#if __WIFI_ADAPTER_NETIF__

esp_netif_ip_info_t wifiLocalIP()
{
  esp_netif_ip_info_t ip;
  memset(&ip, 0, sizeof(esp_netif_ip_info_t));

  if (wifiMode() == WIFI_MODE_NULL) {
    return ip;
  };

  esp_netif_get_ip_info(netif, &ip);
  return ip;
}

#else

tcpip_adapter_ip_info_t wifiLocalIP()
{
  tcpip_adapter_ip_info_t ip;
  memset(&ip, 0, sizeof(tcpip_adapter_ip_info_t));

  if (wifiMode() == WIFI_MODE_NULL) {
    return ip;
  };

  tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip);
  return ip;
}

#endif // __WIFI_ADAPTER_NETIF__

char* wifiGetLocalIP()
{
  esp_netif_ip_info_t local_ip = wifiLocalIP();
  if (local_ip.ip.addr != 0) {
    uint8_t * loc_ip = (uint8_t*)&(local_ip.ip);
    return malloc_stringf("%d.%d.%d.%d", loc_ip[0], loc_ip[1], loc_ip[2], loc_ip[3]);
  } else {
    return nullptr;
  }
}

char* wifiGetGatewayIP()
{
  esp_netif_ip_info_t local_ip = wifiLocalIP();
  if (local_ip.ip.addr != 0) {
    uint8_t * gw_ip = (uint8_t*)&(local_ip.gw);
    return malloc_stringf("%d.%d.%d.%d", gw_ip[0], gw_ip[1], gw_ip[2], gw_ip[3]);
  } else {
    return nullptr;
  };
}

const char* wifiGetHostname()
{
  const char* hostname = NULL;
  
  if (wifiMode() == WIFI_MODE_NULL){
    return hostname;
  };

  if (tcpip_adapter_get_hostname(TCPIP_ADAPTER_IF_STA, &hostname)) {
    return NULL;
  };

  return hostname;
}
