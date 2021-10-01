/* 
   EN: Module for automatically maintaining a constant connection to WiFi in STA mode
   RU: Модуль для автоматического поддержания постоянного подключения к WiFi в режиме STA
   --------------------------
   (с) 2020-2021 Разживин Александр | Razzhivin Alexander
   kotyara12@yandex.ru | https://kotyara12.ru | tg: @kotyara1971
*/

#include <cstring>
#include <sys/time.h> 
#include "rLog.h"
#include "rTypes.h"
#include "rStrings.h"
#include "reWiFi.h"
#include "reNvs.h"
#include "reEvents.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "project_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

static const char * logTAG = "WiFi";
static const char * wifiNvsGroup = "wifi";
static const char * wifiNvsIndex = "index";

static const int _WIFI_TCPIP_INIT         = BIT0;
static const int _WIFI_LOWLEVEL_INIT      = BIT1;
static const int _WIFI_ENABLED            = BIT2;
static const int _WIFI_STOPPED            = BIT3;
static const int _WIFI_DISCONNECTED       = BIT4;
static const int _WIFI_STA_STARTED        = BIT5;
static const int _WIFI_STA_CONNECTED      = BIT6;
static const int _WIFI_STA_GOT_IP         = BIT7;
static const int _WIFI_AP_STARTED         = BIT8;

static uint32_t _wifiAttemptCount = 0;
static EventGroupHandle_t _wifiStatusBits = nullptr;
static esp_netif_t *_wifiNetif = nullptr;
#ifndef CONFIG_WIFI_SSID
static uint8_t _wifiMaxIndex = 0;
static uint8_t _wifiCurrIndex = 0;
static bool _wifiIndexChanged = false;
#endif // CONFIG_WIFI_SSID

#if CONFIG_WIFI_STATIC_ALLOCATION
StaticEventGroup_t _wifiStatusBitsBuffer;
#endif // CONFIG_WIFI_STATIC_ALLOCATION

#define WIFI_ERROR_CHECK_BOOL(x, msg) do {                                              \
  esp_err_t __err_rc = (x);                                                             \
  if (__err_rc != ESP_OK) {                                                             \
    rlog_e(logTAG, "Failed to %s: %d (%s)", msg, __err_rc, esp_err_to_name(__err_rc) ); \
    return false;                                                                       \
  };                                                                                    \
} while(0)

// -----------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------------- Status bits -----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

EventBits_t wifiStatusGet() 
{
  if (!_wifiStatusBits) {
    return 0;
  };
  return xEventGroupGetBits(_wifiStatusBits);
}

bool wifiStatusSet(EventBits_t bits)
{
  if (!_wifiStatusBits) {
    rlog_e(logTAG, "Failed to set status bits: %X, _wifiStatusBits is null!", bits);
    return false;
  };

  EventBits_t afterSet = xEventGroupSetBits(_wifiStatusBits, bits);
  if ((afterSet & bits) != bits) {
    rlog_e(logTAG, "Failed to set status bits: %X, current value: %X", bits, afterSet);
    return false;
  };
  return true;
}

bool wifiStatusClear(const EventBits_t bits)
{
  if (!_wifiStatusBits) {
    return false;
  };
  EventBits_t prevClear = xEventGroupClearBits(_wifiStatusBits, bits);
  if ((prevClear & bits) != 0) {
    EventBits_t afterClear = wifiStatusGet();
    if ((afterClear & bits) != 0) {
      rlog_e(logTAG, "Failed to clear status bits: %X, current value: %X", bits, afterClear);
      return false;
    };
  };
  return true;
}

bool wifiStatusCheck(const EventBits_t bits, const bool clearOnExit) 
{
  if (!_wifiStatusBits) {
    return false;
  };
  if (clearOnExit) {
    return (xEventGroupClearBits(_wifiStatusBits, bits) & bits) == bits;
  } else {
    return (xEventGroupGetBits(_wifiStatusBits) & bits) == bits;
  };
}

bool wifiIsEnabled()
{
  return wifiStatusCheck(_WIFI_ENABLED, false);
}

bool wifiIsConnected()
{
  return wifiStatusCheck(_WIFI_STA_CONNECTED | _WIFI_STA_GOT_IP, false);
}

EventBits_t wifiStatusWait(const EventBits_t bits, const BaseType_t clearOnExit, const uint32_t timeout_ms)
{
  if (!_wifiStatusBits) {
    return 0;
  };  
  if (timeout_ms == 0) {
    return xEventGroupWaitBits(_wifiStatusBits, bits, clearOnExit, pdTRUE, portMAX_DELAY) & bits; 
  }
  else {
    return xEventGroupWaitBits(_wifiStatusBits, bits, clearOnExit, pdTRUE, timeout_ms / portTICK_PERIOD_MS) & bits; 
  };
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------- WiFi mode -----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

wifi_mode_t wifiMode()
{
  if(!wifiStatusCheck(_WIFI_LOWLEVEL_INIT, false)) {
    return WIFI_MODE_NULL;
  };

  wifi_mode_t mode;
  if (esp_wifi_get_mode(&mode) == ESP_ERR_WIFI_NOT_INIT) {
    rlog_w(logTAG, "WiFi not started!");
    return WIFI_MODE_NULL;
  };

  return mode;
}

bool wifiSetMode(wifi_mode_t newMode)
{
  if (wifiStatusCheck(_WIFI_LOWLEVEL_INIT, false)) {
    rlog_v(logTAG, "Set WiFi mode: %d, previous mode: %d...", newMode, wifiMode());

    // Checking current mode
    if (wifiMode() == newMode) {
      return true;
    };

    // Installing a new mode
    WIFI_ERROR_CHECK_BOOL(esp_wifi_set_mode(newMode), "set WiFi mode");

    // Some new protocol???
    #ifdef CONFIG_WIFI_LONG_RANGE
      if (newMode & WIFI_MODE_STA) {
        WIFI_ERROR_CHECK_BOOL(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR), "set protocol");
      };
      if (newMode & WIFI_MODE_AP) {
        WIFI_ERROR_CHECK_BOOL(esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_LR), "set protocol");
      };
    #endif // CONFIG_WIFI_LONG_RANGE

    return true;
  };
  return newMode == WIFI_MODE_NULL;
}

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Configure STA mode ------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

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

const char* wifiGetSSID()
{
  #ifdef CONFIG_WIFI_SSID
    // Single network mode
    return CONFIG_WIFI_SSID;
  #else
    // Multi-network mode
    switch (_wifiCurrIndex) {
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

bool wifiConnectSTA(bool changeIndex)
{
  wifi_config_t conf;
  memset(&conf, 0, sizeof(wifi_config_t));
  #ifdef CONFIG_WIFI_SSID
    // Single network mode
    strcpy(reinterpret_cast<char*>(conf.sta.ssid), CONFIG_WIFI_SSID);
    strcpy(reinterpret_cast<char*>(conf.sta.password), CONFIG_WIFI_PASS);
  #else
    // Multi-network mode
    if (_wifiCurrIndex == 0) {
      _wifiMaxIndex = wifiGetMaxIndex();
      nvsRead(wifiNvsGroup, wifiNvsIndex, OPT_TYPE_U8, &_wifiCurrIndex);
      if (_wifiCurrIndex == 0) {
        _wifiCurrIndex = 1;
        _wifiIndexChanged = true;
      };
    } else {
      if (changeIndex) {
        if (++_wifiCurrIndex > _wifiMaxIndex) {
          _wifiCurrIndex = 1;
        };
        rlog_d(logTAG, "Select WiFi index: %d", _wifiCurrIndex);
        _wifiIndexChanged = true;
      };
    };

    switch (_wifiCurrIndex) {
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
  
  // Support for Protected Management Frame
  conf.sta.pmf_cfg.capable = true;
  conf.sta.pmf_cfg.required = false;

  // Apply configuration
  _wifiAttemptCount++;
  rlog_i(logTAG, "Connecting to WiFi network [ %s ]...", reinterpret_cast<char*>(conf.sta.ssid));
  WIFI_ERROR_CHECK_BOOL(esp_wifi_set_config(WIFI_IF_STA, &conf), "set config");
  WIFI_ERROR_CHECK_BOOL(esp_wifi_connect(), "WiFi connect");

  return true;
}

// -----------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------- Configure AP mode ------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool wifiSetConfigAP()
{
  rlog_e(logTAG, "Sorry, but this has not been released yet :(");
  return false;
}

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Internal functions ------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool wifiStartWiFi()
{
  if (!(wifiStatusCheck(_WIFI_STA_STARTED, false) || wifiStatusCheck(_WIFI_AP_STARTED, false))) {
    rlog_d(logTAG, "Start WiFi...");
    WIFI_ERROR_CHECK_BOOL(esp_wifi_start(), "WiFi start");
  };
  return true;
};

bool wifiStopWiFi()
{
  if (wifiStatusCheck(_WIFI_STA_STARTED, false) || wifiStatusCheck(_WIFI_AP_STARTED, false)) {
    rlog_d(logTAG, "Stop WiFi mode...");
    wifiStatusClear(_WIFI_STOPPED);
    uint8_t tryCnt = 0;
    do {
      tryCnt++;
      WIFI_ERROR_CHECK_BOOL(esp_wifi_stop(), "WiFi stop");
      if ((tryCnt > 10) || (wifiStatusWait(_WIFI_STOPPED, pdFALSE, 10000) & _WIFI_STOPPED)) {
        break;
      } else {
        rlog_d(logTAG, "Waiting for WiFi to stop...");
      };
    } while (1);
  };
  wifiStatusClear(_WIFI_STOPPED);
  return true;
}

bool wifiDisconnectSTA()
{
  if (wifiStatusCheck(_WIFI_STA_CONNECTED, false)) {
    rlog_d(logTAG, "Disconnect from AP...");
    wifiStatusClear(_WIFI_DISCONNECTED);
    uint8_t tryCnt = 0;
    do {
      tryCnt++;
      WIFI_ERROR_CHECK_BOOL(esp_wifi_disconnect(), "WiFi disconnect");
      if ((tryCnt > 10) || (wifiStatusWait(_WIFI_DISCONNECTED, pdFALSE, 10000) & _WIFI_DISCONNECTED)) {
        break;
      } else {
        rlog_d(logTAG, "Waiting for WiFi to disconnect...");
      };
    } while (1);
  };
  wifiStatusClear(_WIFI_DISCONNECTED);
  return true;
}

bool wifiReconnectSTA(bool forcedReconnect, bool changeIndex)
{
  if (wifiIsEnabled()) {
    if (forcedReconnect || (_wifiAttemptCount >= CONFIG_WIFI_CONNECT_ATTEMPTS)) {
      // In order not to cause an endless loop, transfer control to restart "itself" to another process
      eventLoopPost(RE_WIFI_EVENTS, RE_WIFI_STA_RESTART, nullptr, 0, portMAX_DELAY);
    }
    else {
      wifiConnectSTA(changeIndex);
    };
  };
  return false;
}


// -----------------------------------------------------------------------------------------------------------------------
// -------------------------------------------------- WiFi event handler -------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static void wifiEventHandlerWiFi(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  // STA started
  if (event_id == WIFI_EVENT_STA_START) {
    _wifiAttemptCount = 0;
    wifiStatusSet(_WIFI_ENABLED | _WIFI_STA_STARTED);
    wifiStatusClear(_WIFI_STOPPED | _WIFI_DISCONNECTED | _WIFI_STA_CONNECTED | _WIFI_STA_GOT_IP);
    eventLoopPost(RE_WIFI_EVENTS, RE_WIFI_STA_STARTED, nullptr, 0, portMAX_DELAY);  
    rlog_i(logTAG, "WiFi STA started");
    wifiConnectSTA(false);
  } 

  // STA connected
  else if (event_id == WIFI_EVENT_STA_CONNECTED) {
    wifi_event_sta_connected_t * data = (wifi_event_sta_connected_t*)event_data;
    wifiStatusSet(_WIFI_STA_CONNECTED);
    wifiStatusClear(_WIFI_DISCONNECTED | _WIFI_STA_GOT_IP);
    rlog_i(logTAG, "WiFi connection [ %s ] established, RSSI: %d dBi", (char*)data->ssid, wifiRSSI());
    #ifndef CONFIG_WIFI_SSID
      if (_wifiIndexChanged) {
        nvsWrite(wifiNvsGroup, wifiNvsIndex, OPT_TYPE_U8, &_wifiCurrIndex);
      };
    #endif // CONFIG_WIFI_SSID
  }

  // STA disconnected
  else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
    EventBits_t prevStatusBits = wifiStatusGet();
    wifi_event_sta_disconnected_t * data = (wifi_event_sta_disconnected_t*)event_data;
    wifiStatusSet(_WIFI_DISCONNECTED);
    wifiStatusClear(_WIFI_STA_CONNECTED | _WIFI_STA_GOT_IP);
    if (((prevStatusBits & _WIFI_STA_CONNECTED) == _WIFI_STA_CONNECTED)
     && ((prevStatusBits & (_WIFI_STA_GOT_IP))==(_WIFI_STA_GOT_IP))) {
      rlog_e(logTAG, "WiFi connection [ %s ] lost: %d!", wifiGetSSID(), data->reason);
      eventLoopPost(RE_WIFI_EVENTS, RE_WIFI_STA_DISCONNECTED, data, sizeof(wifi_event_sta_disconnected_t), portMAX_DELAY);  
      wifiReconnectSTA(true, false);
    } else {
      rlog_e(logTAG, "Failed to connect to WiFi network: #%d!", data->reason);
      wifiReconnectSTA(false, true);
    };
  }

  // STA beacon timeout
  else if (event_id == WIFI_EVENT_STA_BEACON_TIMEOUT) {
    wifiStatusSet(_WIFI_DISCONNECTED);
    wifiStatusClear(_WIFI_STA_CONNECTED | _WIFI_STA_GOT_IP);
    rlog_e(logTAG, "WiFi connection [ %s ] lost: beacon timeout", wifiGetSSID());
    eventLoopPost(RE_WIFI_EVENTS, RE_WIFI_STA_DISCONNECTED, nullptr, 0, portMAX_DELAY);  
    wifiReconnectSTA(false, true);
  }

  // STA stopped
  else if (event_id == WIFI_EVENT_STA_STOP) {
    wifiStatusSet(_WIFI_STOPPED);
    wifiStatusClear(_WIFI_DISCONNECTED | _WIFI_STA_STARTED | _WIFI_STA_CONNECTED | _WIFI_STA_GOT_IP);
    eventLoopPost(RE_WIFI_EVENTS, RE_WIFI_STA_STOPPED, nullptr, 0, portMAX_DELAY);  
    rlog_i(logTAG, "WiFi STA stopped");
  }

  // Other
  else {
    rlog_d(logTAG, "Unsupported event type: %d", event_id);
  };
}

static void wifiEventHandlerIP(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  // Received IP address
  if (event_id == IP_EVENT_STA_GOT_IP) {
    _wifiAttemptCount = 0;
    ip_event_got_ip_t * data = (ip_event_got_ip_t*)event_data;
    uint8_t * ip = (uint8_t*)&(data->ip_info.ip.addr);
    uint8_t * mask = (uint8_t*)&(data->ip_info.netmask.addr);
    uint8_t * gw = (uint8_t*)&(data->ip_info.gw.addr);
    wifiStatusSet(_WIFI_STA_GOT_IP);
    eventLoopPost(RE_WIFI_EVENTS, RE_WIFI_STA_GOT_IP, data, sizeof(ip_event_got_ip_t), portMAX_DELAY);  
    rlog_i(logTAG, "Got IP-address: %d.%d.%d.%d, mask: %d.%d.%d.%d, gateway: %d.%d.%d.%d",
        ip[0], ip[1], ip[2], ip[3], mask[0], mask[1], mask[2], mask[3], gw[0], gw[1], gw[2], gw[3]);
    #if !CONFIG_PINGER_ENABLE
      // If PINGER service is not available, send an event that the Internet is available immediately
      eventLoopPost(RE_WIFI_EVENTS, RE_WIFI_STA_PING_OK, nullptr, 0, portMAX_DELAY);  
    #endif // CONFIG_PINGER_ENABLE
  } 

  // Lost IP address
  else if (event_id == IP_EVENT_STA_LOST_IP) {
    rlog_e(logTAG, "Lost WiFi IP address!");
    wifiStatusClear(_WIFI_STA_GOT_IP);
    wifiReconnectSTA(true, false);
  }

  // Other
  else {
    rlog_d(logTAG, "Unsupported event type: %d", event_id);
  };
}

#if CONFIG_PINGER_ENABLE
// Relaying PING_EVENT events to WIFI_EVENT
static void wifiEventHandlerPing(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  ping_inet_data_t* data = (ping_inet_data_t*)event_data;

  if (event_id == RE_PING_INET_AVAILABLE) {
    if ((data) && (data->time_unavailable > 1000000000)) {
      eventLoopPost(RE_WIFI_EVENTS, RE_WIFI_STA_PING_OK, &(data->time_unavailable), sizeof(time_t), portMAX_DELAY);
    } else {
      eventLoopPost(RE_WIFI_EVENTS, RE_WIFI_STA_PING_OK, nullptr, 0, portMAX_DELAY);
    };
  } 
  else if (event_id == RE_PING_INET_UNAVAILABLE) {
    if ((data) && (data->time_unavailable > 1000000000)) {
      eventLoopPost(RE_WIFI_EVENTS, RE_WIFI_STA_PING_FAILED, &(data->time_unavailable), sizeof(time_t), portMAX_DELAY);
    } else {
      eventLoopPost(RE_WIFI_EVENTS, RE_WIFI_STA_PING_FAILED, nullptr, 0, portMAX_DELAY);
    };
  };
}
#endif // CONFIG_PINGER_ENABLE

static void wifiEventHandlerRestart(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  if (event_id == RE_WIFI_STA_RESTART) {
    rlog_i(logTAG, "Reconnecting to WiFi network...");
    wifiStatusClear(_WIFI_ENABLED);
    wifiDisconnectSTA();
    wifiStopWiFi();
    wifiStatusSet(_WIFI_ENABLED);
    wifiStartWiFi();
  };
}

// -----------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------- Low-level WiFi functions ----------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool wifiTcpIpInit()
{
  rlog_d(logTAG, "TCP-IP initialization...");

  // MAC address initialization
  uint8_t mac[8];
  if (esp_efuse_mac_get_default(mac) == ESP_OK) {
    WIFI_ERROR_CHECK_BOOL(esp_base_mac_addr_set(mac), "set MAC address");
  };

  // Start the system events task
  esp_err_t err = esp_event_loop_create_default();
  if (!((err == ESP_OK) || (err == ESP_ERR_INVALID_STATE))) {
    rlog_e(logTAG, "Failed to create event loop: %d", err);
    return false;
  };

  // Initializing the TCP/IP stack
  WIFI_ERROR_CHECK_BOOL(esp_netif_init(), "esp netif init");

  // Set initialization bit
  return wifiStatusSet(_WIFI_TCPIP_INIT);
};

bool wifiLowLevelInit(wifi_mode_t mode)
{
  if (!wifiStatusCheck(_WIFI_LOWLEVEL_INIT, false)) {
    rlog_d(logTAG, "WiFi low level initialization...");

    eventLoopPost(RE_WIFI_EVENTS, RE_WIFI_STA_INIT, nullptr, 0, portMAX_DELAY);  

    // Initializing TCP-IP and system task
    if (!wifiStatusCheck(_WIFI_TCPIP_INIT, false)) {
      if (!wifiTcpIpInit()) return false;
    };

    // Remove netif if it existed (e.g. when changing mode)
    if (_wifiNetif) {
      esp_netif_destroy(_wifiNetif);
      _wifiNetif = nullptr;
    };

    // Initializing netif
    switch (mode) {
      case WIFI_MODE_STA:
        _wifiNetif = esp_netif_create_default_wifi_sta();
        break;
      case WIFI_MODE_AP:
        _wifiNetif = esp_netif_create_default_wifi_ap();
        break;
      default:
        rlog_e(logTAG, "Mode is not supported!");
        return false;
    };

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
      rlog_e(logTAG, "Error esp_wifi_init: %d", err);
      return false;
    };

    // Set the storage type of the Wi-Fi configuration in memory
    // WIFI_ERROR_CHECK_BOOL(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    // Register default event handlers
    WIFI_ERROR_CHECK_BOOL(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandlerWiFi, nullptr), "handler register");
    WIFI_ERROR_CHECK_BOOL(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandlerIP, nullptr), "handler register");

    // Register feedback event handlers
    eventHandlerRegister(RE_WIFI_EVENTS, RE_WIFI_STA_RESTART, &wifiEventHandlerRestart, nullptr);
    #if CONFIG_PINGER_ENABLE
    eventHandlerRegister(RE_PING_EVENTS, RE_PING_INET_AVAILABLE, &wifiEventHandlerPing, nullptr);
    eventHandlerRegister(RE_PING_EVENTS, RE_PING_INET_UNAVAILABLE, &wifiEventHandlerPing, nullptr);
    #endif // CONFIG_PINGER_ENABLE

    // Set initialization bit
    return wifiStatusSet(_WIFI_LOWLEVEL_INIT);
  };
  return false;
}

bool wifiLowLevelDeinit()
{
  if (wifiStatusCheck(_WIFI_LOWLEVEL_INIT, false)) {
    rlog_d(logTAG, "WiFi low level finalization");

    // Unregister default event handlers
    WIFI_ERROR_CHECK_BOOL(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandlerWiFi), "handler unregister");
    WIFI_ERROR_CHECK_BOOL(esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandlerIP), "handler unregister");

    // Unregister feedback event handlers
    eventHandlerUnregister(RE_WIFI_EVENTS, RE_WIFI_STA_RESTART, &wifiEventHandlerRestart);
    #if CONFIG_PINGER_ENABLE
    eventHandlerUnregister(RE_PING_EVENTS, RE_PING_INET_AVAILABLE, &wifiEventHandlerPing);
    eventHandlerUnregister(RE_PING_EVENTS, RE_PING_INET_UNAVAILABLE, &wifiEventHandlerPing);
    #endif // CONFIG_PINGER_ENABLE

    // We free up WiFi resources, we don’t tamper with the TCP-IP stack
    WIFI_ERROR_CHECK_BOOL(esp_wifi_deinit(), "WiFi deinit");

    // Free netif
    if (_wifiNetif) {
      esp_netif_destroy(_wifiNetif);
      _wifiNetif = nullptr;
    };

    // Clear initialization bit
    return wifiStatusClear(_WIFI_LOWLEVEL_INIT);
  };

  return true;
}

// -----------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------- Public functions -------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool wifiInit()
{
  if (!_wifiStatusBits) {
    #if NETWORK_EVENT_STATIC_ALLOCATION
      _wifiStatusBits = xEventGroupCreateStatic(&_wifiStatusBitsBuffer);
    #else
      _wifiStatusBits = xEventGroupCreate();
    #endif // NETWORK_EVENT_STATIC_ALLOCATION
    if (!_wifiStatusBits) {
      rlog_e(logTAG, "Error creating WiFi state group!");
      return false;
    };
    xEventGroupClearBits(_wifiStatusBits, 0x00FFFFFF);
  };

  return true;
}

bool wifiStart(wifi_mode_t mode)
{
  bool ret = true;
  // Initialization WiFi, if not done earlier
  if (!_wifiStatusBits) ret = wifiInit();
  // Stop the previous mode if it was activated
  if (ret) ret = wifiStop();
  // Low level init
  if (ret) ret = wifiLowLevelInit(mode);
  // Set WiFi mode
  if (ret) ret = wifiSetMode(mode);
  // Allow reconnection
  if (ret) ret = wifiStatusSet(_WIFI_ENABLED);
  // Start WiFi
  if (ret) ret = wifiStartWiFi();
  return ret;
}

bool wifiStartSTA()
{
  return wifiStart(WIFI_MODE_STA); 
}

bool wifiStartAP()
{
  return wifiStart(WIFI_MODE_AP); 
}

bool wifiStop()
{
  bool ret = true;
  // Block reconnection
  if (ret) ret = wifiStatusClear(_WIFI_ENABLED);
  // Disconnect WiFi
  if (ret) ret = wifiDisconnectSTA();
  // Stop WiFi
  if (ret) ret = wifiStopWiFi();
  // Clear WiFi mode
  if (ret) ret = wifiSetMode(WIFI_MODE_NULL);
  // Low level deinit
  if (ret) ret = wifiLowLevelDeinit();
  return ret;
}

bool wifiRestart()
{
  wifi_mode_t mode = wifiMode();
  if (mode != WIFI_MODE_NULL) {
    return wifiStop() && wifiStart(mode);
  };
  return false;
}

bool wifiFree()
{
  if (!wifiStop()) {
    return false;
  };
  if (_wifiStatusBits) {
    vEventGroupDelete(_wifiStatusBits);
    _wifiStatusBits = nullptr;
  };
  return true;
}

// -----------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------- Other functions --------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

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

esp_netif_ip_info_t wifiLocalIP()
{
  esp_netif_ip_info_t ip;
  memset(&ip, 0, sizeof(esp_netif_ip_info_t));

  if (wifiMode() != WIFI_MODE_STA) {
    return ip;
  };

  esp_netif_get_ip_info(_wifiNetif, &ip);
  return ip;
}

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

esp_err_t wifiHostByName(const char* hostname, ip_addr_t* hostaddr)
{
  if ((hostname) && (hostaddr)) {
    rlog_d(logTAG, "Resolving address for host [ %s ]...", hostname);

    struct addrinfo addrHint;
    struct addrinfo *addrRes = nullptr;
    memset(&addrHint, 0, sizeof(addrHint));
    if (getaddrinfo(hostname, nullptr, &addrHint, &addrRes) != 0) {
      rlog_e(logTAG, "Unknown host [ %s ]", hostname);
      return ESP_ERR_NOT_FOUND;
    };

    struct in_addr addr4 = ((struct sockaddr_in *) (addrRes->ai_addr))->sin_addr;
    inet_addr_to_ip4addr(ip_2_ip4(hostaddr), &addr4);
    freeaddrinfo(addrRes);

    if (IP_IS_V4(hostaddr)) {
      uint8_t * ip = (uint8_t*)&(hostaddr->u_addr.ip4.addr);
      rlog_d(logTAG, "IP address obtained for host [ %s ]: %d.%d.%d.%d", hostname, ip[0], ip[1], ip[2], ip[3]);
    };

    return ESP_OK;
  } else {
    return ESP_ERR_INVALID_ARG;
  };
}