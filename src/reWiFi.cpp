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
#include "esp_timer.h"
#include "project_config.h"
#include "def_consts.h"

static const char * logTAG = "WiFi";
static const char * wifiNvsGroup = "wifi";
static const char * wifiNvsIndex = "index";

static const int _WIFI_TCPIP_INIT             = BIT0;
static const int _WIFI_LOWLEVEL_INIT          = BIT1;
static const int _WIFI_STA_ENABLED            = BIT2;
static const int _WIFI_STA_STARTED            = BIT3;
static const int _WIFI_STA_CONNECTED          = BIT4;
static const int _WIFI_STA_GOT_IP             = BIT5;
static const int _WIFI_STA_DISCONNECT_STOP    = BIT6;
static const int _WIFI_STA_DISCONNECT_RESTART = BIT7;

static uint32_t _wifiAttemptCount = 0;
static EventGroupHandle_t _wifiStatusBits = nullptr;
static esp_netif_t *_wifiNetif = nullptr;
#ifndef CONFIG_WIFI_SSID
static uint8_t _wifiMaxIndex = 0;
static uint8_t _wifiCurrIndex = 0;
static bool _wifiIndexNeedChange = false;
static bool _wifiIndexWasChanged = false;
#endif // CONFIG_WIFI_SSID

#if CONFIG_WIFI_STATIC_ALLOCATION
StaticEventGroup_t _wifiStatusBitsBuffer;
#endif // CONFIG_WIFI_STATIC_ALLOCATION

#define WIFI_ERROR_CHECK_LOG(x, msg) do {                                               \
  esp_err_t __err_rc = (x);                                                             \
  if (__err_rc != ESP_OK) {                                                             \
    rlog_e(logTAG, "Failed to %s: %d (%s)", msg, __err_rc, esp_err_to_name(__err_rc) ); \
  };                                                                                    \
} while(0)

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
  return wifiStatusCheck(_WIFI_STA_ENABLED, false);
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
    return xEventGroupWaitBits(_wifiStatusBits, bits, clearOnExit, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & bits; 
  };
}

char* wifiStatusGetJson()
{
  EventBits_t bits = wifiStatusGet();
  return malloc_stringf("{\"init_tcpip\":%d,\"init_low\":%d,\"sta_enabled\":%d,\"sta_started\":%d,\"sta_connected\":%d,\"sta_got_ip\":%d,\"disconnect_and_stop\":%d,\"disconnect_and_restart\":%d}",
    (bits & _WIFI_TCPIP_INIT) == _WIFI_TCPIP_INIT,
    (bits & _WIFI_LOWLEVEL_INIT) == _WIFI_LOWLEVEL_INIT,
    (bits & _WIFI_STA_ENABLED) == _WIFI_STA_ENABLED,
    (bits & _WIFI_STA_STARTED) == _WIFI_STA_STARTED,
    (bits & _WIFI_STA_CONNECTED) == _WIFI_STA_CONNECTED,
    (bits & _WIFI_STA_GOT_IP) == _WIFI_STA_GOT_IP,
    (bits & _WIFI_STA_DISCONNECT_STOP) == _WIFI_STA_DISCONNECT_STOP,
    (bits & _WIFI_STA_DISCONNECT_RESTART) == _WIFI_STA_DISCONNECT_RESTART);
};
 

// -----------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------- Low-level WiFi functions ----------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static bool wifiRegisterEventHandlers();
static void wifiUnregisterEventHandlers();
#if CONFIG_PINGER_ENABLE
static void wifiEventHandlerPing(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
#endif // CONFIG_PINGER_ENABLE

// Wi-Fi/LwIP Init Phase
// https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi.html#wi-fi-lwip-init-phase

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

bool wifiLowLevelInit()
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
    _wifiNetif = esp_netif_create_default_wifi_sta();

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
    #ifdef CONFIG_WIFI_STORAGE
      WIFI_ERROR_CHECK_BOOL(esp_wifi_set_storage(CONFIG_WIFI_STORAGE));
    #endif // CONFIG_WIFI_STORAGE

    // Register event handlers
    if (wifiRegisterEventHandlers()) {
      // Set initialization bit
      return wifiStatusSet(_WIFI_LOWLEVEL_INIT);
    };
  };
  return false;
}

bool wifiLowLevelDeinit()
{
  if (wifiStatusCheck(_WIFI_LOWLEVEL_INIT, false)) {
    rlog_d(logTAG, "WiFi low level finalization");

    // Clear wifi mode
    WIFI_ERROR_CHECK_BOOL(esp_wifi_set_mode(WIFI_MODE_NULL), "clear the WiFi operating mode");

    // Unregister event handlers
    wifiUnregisterEventHandlers();

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
// ------------------------------------------------------- Timeout -------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static esp_timer_handle_t _wifiTimer = nullptr;

bool wifiReconnectWiFi();
static void wifiTimeoutEnd(void* arg)
{
  rlog_e(logTAG, "Time-out!");
  wifiReconnectWiFi();
}

static void wifiTimeoutCreate() {
  if (_wifiTimer) {
    if (esp_timer_is_active(_wifiTimer)) {
      esp_timer_stop(_wifiTimer);
    };
  } else {
    esp_timer_create_args_t timer_args;
    memset(&timer_args, 0, sizeof(esp_timer_create_args_t));
    timer_args.callback = &wifiTimeoutEnd;
    timer_args.name = "timer_wifi";
    if (esp_timer_create(&timer_args, &_wifiTimer) != ESP_OK) {
      rlog_e(logTAG, "Failed to create timeout timer");
    };
  };
  rlog_v(logTAG, "WiFi timer was created");
}

static void wifiTimeoutStart(uint32_t ms_timeout) {
  if (!_wifiTimer) {
    wifiTimeoutCreate();
  };
  if (_wifiTimer) {
    if (esp_timer_is_active(_wifiTimer)) {
      esp_timer_stop(_wifiTimer);
    };
    if (esp_timer_start_once(_wifiTimer, ms_timeout * 1000) == ESP_OK) {
      rlog_v(logTAG, "WiFi timer was started");
    } else {  
      rlog_e(logTAG, "Failed to start timeout timer");
    };
  };
}

static void wifiTimeoutStop() {
  if (_wifiTimer) {
    if (esp_timer_is_active(_wifiTimer)) {
      if (esp_timer_stop(_wifiTimer) == ESP_OK) {
        rlog_v(logTAG, "WiFi timer was stoped");
      } else {  
        rlog_e(logTAG, "Failed to stop timeout timer");
      };
    };
  };
}

static void wifiTimeoutDelete()
{
  if (_wifiTimer) {
    if (esp_timer_is_active(_wifiTimer)) {
      esp_timer_stop(_wifiTimer);
    };
    esp_timer_delete(_wifiTimer);
    _wifiTimer = nullptr;
    rlog_v(logTAG, "WiFi timer was deleted");
  };
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

bool wifiConnectSTA()
{
  // Wi-Fi Configuration Phase
  // https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi.html#wi-fi-configuration-phase

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
      _wifiIndexNeedChange = false;
      _wifiIndexWasChanged = false;
      nvsRead(wifiNvsGroup, wifiNvsIndex, OPT_TYPE_U8, &_wifiCurrIndex);
      if (_wifiCurrIndex == 0) {
        _wifiCurrIndex = 1;
        _wifiIndexNeedChange = true;
        _wifiIndexWasChanged = true;
      };
    } else {
      if (_wifiIndexNeedChange) {
        if (++_wifiCurrIndex > _wifiMaxIndex) {
          _wifiCurrIndex = 1;
        };
        rlog_d(logTAG, "Attempting to connect to another access point: %d", _wifiCurrIndex);
        _wifiIndexWasChanged = true;
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

  // Configure WiFi
  WIFI_ERROR_CHECK_BOOL(esp_wifi_set_config(WIFI_IF_STA, &conf), "set the configuration of the ESP32 STA");
  #ifdef CONFIG_WIFI_BANDWIDTH
    // Theoretically the HT40 can gain better throughput because the maximum raw physicial 
    // (PHY) data rate for HT40 is 150Mbps while it’s 72Mbps for HT20. 
    // However, if the device is used in some special environment, e.g. there are too many other Wi-Fi devices around the ESP32 device, 
    // the performance of HT40 may be degraded. So if the applications need to support same or similar scenarios, 
    // it’s recommended that the bandwidth is always configured to HT20.
    // https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi.html#wi-fi-ht20-40
    WIFI_ERROR_CHECK_BOOL(esp_wifi_set_bandwidth(WIFI_IF_STA, CONFIG_WIFI_BANDWIDTH), "set the bandwidth");
  #endif // CONFIG_WIFI_BANDWIDTH
  #ifdef CONFIG_WIFI_LONGRANGE
    // Long Range (LR). Since LR is Espressif unique Wi-Fi mode, only ESP32 devices can transmit and receive the LR data
    // more info: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi.html#wi-fi-protocol-mode
    WIFI_ERROR_CHECK_BOOL(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR), "set protocol Long Range");
  #endif // CONFIG_WIFI_LONGRANGE

  // Wi-Fi Connect Phase
  // https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi.html#wi-fi-connect-phase
  _wifiAttemptCount++;
  rlog_i(logTAG, "Connecting to WiFi network [ %s ], attempt %d...", reinterpret_cast<char*>(conf.sta.ssid), _wifiAttemptCount);
  wifiTimeoutStart(CONFIG_WIFI_TIMEOUT);
  WIFI_ERROR_CHECK_BOOL(esp_wifi_connect(), "сonnect the ESP32 WiFi station to the AP");

  return true;
}

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Internal functions ------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool _wifiStartSTA()
{
  rlog_d(logTAG, "Start WiFi STA mode...");
  WIFI_ERROR_CHECK_BOOL(esp_wifi_set_mode(WIFI_MODE_STA), "set the WiFi operating mode");
  WIFI_ERROR_CHECK_BOOL(esp_wifi_start(), "start WiFi");
  wifiTimeoutStart(CONFIG_WIFI_TIMEOUT);
  return true;
}

bool _wifiDisconnectSTA(EventBits_t next_stage)
{
  rlog_d(logTAG, "Disconnect from AP...");
  if (next_stage > 0) wifiStatusSet(next_stage);
  WIFI_ERROR_CHECK_BOOL(esp_wifi_disconnect(), "WiFi disconnect");
  wifiTimeoutStart(CONFIG_WIFI_TIMEOUT);
  return true;
}

bool _wifiStopSTA()
{
  rlog_d(logTAG, "Stop WiFi STA mode...");
  WIFI_ERROR_CHECK_BOOL(esp_wifi_stop(), "WiFi stop");
  return true;
}

bool _wifiResetSTA()
{
  rlog_w(logTAG, "Restore WiFi stack persistent settings to default values!");
  WIFI_ERROR_CHECK_BOOL(esp_wifi_restore(), "restore WiFi stack persistent settings to default values");
  wifiTimeoutStart(CONFIG_WIFI_TIMEOUT);
  return true;
}

bool wifiStartWiFi()
{
  if (!wifiStatusCheck(_WIFI_STA_STARTED, false)) {
    return _wifiStartSTA();
  };
  return true;
};

bool wifiStopWiFi()
{
  if (wifiStatusCheck(_WIFI_STA_CONNECTED, false)) {
    return _wifiDisconnectSTA(_WIFI_STA_DISCONNECT_STOP);
  } else {
    if (wifiStatusCheck(_WIFI_STA_STARTED, false)) {
      return _wifiStopSTA();
    };
  };
  return true;
}

bool wifiRestartWiFi()
{
  if (wifiStatusCheck(_WIFI_STA_CONNECTED, false)) {
    return _wifiDisconnectSTA(_WIFI_STA_DISCONNECT_RESTART);
  } else {
    if (wifiStatusCheck(_WIFI_STA_STARTED, false)) {
      return _wifiResetSTA();
    } else {
      return _wifiStartSTA();
    };
  };
}

bool wifiReconnectWiFi()
{
  if (wifiStatusCheck(_WIFI_STA_DISCONNECT_STOP, true)) {
    return _wifiStopSTA();
  } else if (wifiStatusCheck(_WIFI_STA_DISCONNECT_RESTART, true)) {
    return _wifiResetSTA();
  } else {
    if (wifiStatusCheck(_WIFI_STA_ENABLED, false)) {
      if (_wifiAttemptCount > CONFIG_WIFI_RESTART_ATTEMPTS) {
        return wifiRestartWiFi();
      } else {
        if (_wifiAttemptCount > CONFIG_WIFI_RECONNECT_ATTEMPTS) {
          _wifiIndexNeedChange = true;
        };
        if (!_wifiIndexNeedChange) {
          vTaskDelay(pdMS_TO_TICKS(CONFIG_WIFI_RECONNECT_DELAY));
        };
        return wifiConnectSTA();
      };
    } else {
      wifiStopWiFi();
    };
  };
  return false;
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------- WiFi event handlers -------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static void wifiEventHandler_Start(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  // Set status bits
  wifiStatusSet(_WIFI_STA_ENABLED | _WIFI_STA_STARTED);
  wifiStatusClear(_WIFI_STA_CONNECTED | _WIFI_STA_GOT_IP | _WIFI_STA_DISCONNECT_STOP | _WIFI_STA_DISCONNECT_RESTART);
  // Reset attempts count
  _wifiAttemptCount = 0;
  // Re-dispatch event to another loop
  eventLoopPost(RE_WIFI_EVENTS, RE_WIFI_STA_STARTED, nullptr, 0, portMAX_DELAY);  
  // Log
  rlog_i(logTAG, "WiFi STA started");
  // Start connection
  wifiConnectSTA();
}

static void wifiEventHandler_Connect(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  // Set status bits
  wifiStatusSet(_WIFI_STA_CONNECTED);
  wifiStatusClear(_WIFI_STA_GOT_IP | _WIFI_STA_DISCONNECT_STOP | _WIFI_STA_DISCONNECT_RESTART);
  // Save successful connection number
  #ifndef CONFIG_WIFI_SSID
    _wifiIndexNeedChange = false;
    if (_wifiIndexWasChanged) {
      nvsWrite(wifiNvsGroup, wifiNvsIndex, OPT_TYPE_U8, &_wifiCurrIndex);
      _wifiIndexWasChanged = false;
    };
  #endif // CONFIG_WIFI_SSID
  // Log
  #if CONFIG_RLOG_PROJECT_LEVEL >= RLOG_LEVEL_INFO
    wifi_event_sta_connected_t * data = (wifi_event_sta_connected_t*)event_data;
    rlog_i(logTAG, "WiFi connection [ %s ] established, RSSI: %d dBi", (char*)data->ssid, wifiRSSI());
  #endif
  // Restart timer
  wifiTimeoutStart(CONFIG_WIFI_TIMEOUT);
}

static void wifiEventHandler_Disconnect(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  // Check current status
  EventBits_t prevStatusBits = wifiStatusGet();
  bool isWasConnected = (prevStatusBits & _WIFI_STA_CONNECTED) == _WIFI_STA_CONNECTED;
  bool isWasIP = (prevStatusBits & _WIFI_STA_GOT_IP) == _WIFI_STA_GOT_IP;
  // Reset status bits
  wifiStatusClear(_WIFI_STA_CONNECTED | _WIFI_STA_GOT_IP);
  // Stop timer
  wifiTimeoutStop();
  // Check for forced (manual) WiFi disconnection
  if (wifiStatusCheck(_WIFI_STA_ENABLED, false)) {
    // Different reconnection scenarios
    if (event_id == WIFI_EVENT_STA_BEACON_TIMEOUT) {
      if (isWasConnected && isWasIP) {
        // Re-dispatch event to another loop
        eventLoopPost(RE_WIFI_EVENTS, RE_WIFI_STA_DISCONNECTED, nullptr, 0, portMAX_DELAY);  
        rlog_e(logTAG, "WiFi connection [ %s ] lost: beacon timeout!", wifiGetSSID());
      } else {
        rlog_e(logTAG, "Failed to connect to WiFi network: beacon timeout!");
      };
      // Next connection attempt
      wifiReconnectWiFi();
    } else if (event_id == IP_EVENT_STA_LOST_IP) {
      // Re-dispatch event to another loop
      eventLoopPost(RE_WIFI_EVENTS, RE_WIFI_STA_DISCONNECTED, nullptr, 0, portMAX_DELAY);
      rlog_e(logTAG, "WiFi connection [ %s ] lost WiFi IP address!", wifiGetSSID());
      // Next connection attempt
      wifiReconnectWiFi();
    } else {
      wifi_event_sta_disconnected_t * data = (wifi_event_sta_disconnected_t*)event_data;
      if (isWasConnected && isWasIP) {
        // Re-dispatch event to another loop
        eventLoopPost(RE_WIFI_EVENTS, RE_WIFI_STA_DISCONNECTED, data, sizeof(wifi_event_sta_disconnected_t), portMAX_DELAY);  
        rlog_e(logTAG, "WiFi connection [ %s ] lost: #%d!", wifiGetSSID(), data->reason);
      } else {
        rlog_e(logTAG, "Failed to connect to WiFi network: #%d!", data->reason);
      };
      // Next connection attempt
      wifiReconnectWiFi();
    };
  } else {
    // Stop WiFi
    wifiStopWiFi();
  }
}

static void wifiEventHandler_Stop(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  // Reset status bits
  wifiStatusClear(_WIFI_STA_STARTED | _WIFI_STA_CONNECTED | _WIFI_STA_GOT_IP);
  // Re-dispatch event to another loop
  eventLoopPost(RE_WIFI_EVENTS, RE_WIFI_STA_STOPPED, nullptr, 0, portMAX_DELAY);  
  // Log
  rlog_w(logTAG, "WiFi STA stopped");
  // Delete timer
  wifiTimeoutDelete();
  // If WiFi is enabled, restart it
  if (wifiStatusCheck(_WIFI_STA_ENABLED, false)) {
    wifiStartWiFi();
  // ... otherwise we turn off everything
  } else {
    wifiLowLevelDeinit();
  };
}

static void wifiEventHandler_GotIP(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  // Set status bits
  wifiStatusSet(_WIFI_STA_GOT_IP);
  // Reset attempts count
  _wifiAttemptCount = 0;
  // Re-dispatch event to another loop
  ip_event_got_ip_t * data = (ip_event_got_ip_t*)event_data;
  eventLoopPost(RE_WIFI_EVENTS, RE_WIFI_STA_GOT_IP, data, sizeof(ip_event_got_ip_t), portMAX_DELAY);  
  // Log
  #if CONFIG_RLOG_PROJECT_LEVEL >= RLOG_LEVEL_INFO
    uint8_t * ip = (uint8_t*)&(data->ip_info.ip.addr);
    uint8_t * mask = (uint8_t*)&(data->ip_info.netmask.addr);
    uint8_t * gw = (uint8_t*)&(data->ip_info.gw.addr);
    rlog_i(logTAG, "Got IP-address: %d.%d.%d.%d, mask: %d.%d.%d.%d, gateway: %d.%d.%d.%d",
        ip[0], ip[1], ip[2], ip[3], mask[0], mask[1], mask[2], mask[3], gw[0], gw[1], gw[2], gw[3]);
  #endif
  // Delete timer
  wifiTimeoutDelete();
  #if !CONFIG_PINGER_ENABLE
    // If PINGER service is not available, send an event that the Internet is available immediately
    eventLoopPost(RE_WIFI_EVENTS, RE_WIFI_STA_PING_OK, nullptr, 0, portMAX_DELAY);  
  #endif // CONFIG_PINGER_ENABLE
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

static bool wifiRegisterEventHandlers()
{
  WIFI_ERROR_CHECK_BOOL(
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START, &wifiEventHandler_Start, nullptr), 
    "register an event handler for WIFI_EVENT_STA_START");
  WIFI_ERROR_CHECK_BOOL(
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &wifiEventHandler_Connect, nullptr), 
    "register an event handler for WIFI_EVENT_STA_CONNECTED");
  WIFI_ERROR_CHECK_BOOL(
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &wifiEventHandler_Disconnect, nullptr), 
    "register an event handler for WIFI_EVENT_STA_DISCONNECTED");
  WIFI_ERROR_CHECK_BOOL(
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_BEACON_TIMEOUT, &wifiEventHandler_Disconnect, nullptr), 
    "register an event handler for WIFI_EVENT_STA_BEACON_TIMEOUT");
  WIFI_ERROR_CHECK_BOOL(
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_STOP, &wifiEventHandler_Stop, nullptr), 
    "register an event handler for WIFI_EVENT_STA_STOP");
  WIFI_ERROR_CHECK_BOOL(
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler_GotIP, nullptr), 
    "register an event handler for IP_EVENT_STA_GOT_IP");
  WIFI_ERROR_CHECK_BOOL(
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &wifiEventHandler_Disconnect, nullptr), 
    "register an event handler for IP_EVENT_STA_LOST_IP");

  // Register ping event handlers
  #if CONFIG_PINGER_ENABLE
    eventHandlerRegister(RE_PING_EVENTS, RE_PING_INET_AVAILABLE, &wifiEventHandlerPing, nullptr);
    eventHandlerRegister(RE_PING_EVENTS, RE_PING_INET_UNAVAILABLE, &wifiEventHandlerPing, nullptr);
  #endif // CONFIG_PINGER_ENABLE

  return true;
}

static void wifiUnregisterEventHandlers()
{
  WIFI_ERROR_CHECK_LOG(
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_START, &wifiEventHandler_Start), 
    "unregister an event handler for WIFI_EVENT_STA_START");
  WIFI_ERROR_CHECK_LOG(
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &wifiEventHandler_Connect), 
    "unregister an event handler for WIFI_EVENT_STA_CONNECTED");
  WIFI_ERROR_CHECK_LOG(
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &wifiEventHandler_Disconnect), 
    "unregister an event handler for WIFI_EVENT_STA_DISCONNECTED");
  WIFI_ERROR_CHECK_LOG(
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_BEACON_TIMEOUT, &wifiEventHandler_Disconnect), 
    "unregister an event handler for WIFI_EVENT_STA_BEACON_TIMEOUT");
  WIFI_ERROR_CHECK_LOG(
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_STOP, &wifiEventHandler_Stop), 
    "unregister an event handler for WIFI_EVENT_STA_STOP");
  WIFI_ERROR_CHECK_LOG(
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler_GotIP), 
    "unregister an event handler for IP_EVENT_STA_GOT_IP");
  WIFI_ERROR_CHECK_LOG(
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_LOST_IP, &wifiEventHandler_Disconnect), 
    "unregister an event handler for IP_EVENT_STA_LOST_IP");

  // Register ping event handlers
  #if CONFIG_PINGER_ENABLE
    eventHandlerUnregister(RE_PING_EVENTS, RE_PING_INET_AVAILABLE, &wifiEventHandlerPing);
    eventHandlerUnregister(RE_PING_EVENTS, RE_PING_INET_UNAVAILABLE, &wifiEventHandlerPing);
  #endif // CONFIG_PINGER_ENABLE
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

bool wifiStart()
{
  bool ret = true;
  // Initialization WiFi, if not done earlier
  if (!_wifiStatusBits) ret = wifiInit();
  // Stop the previous mode if it was activated
  if (ret) ret = wifiStop();
  // Low level init
  if (ret) ret = wifiLowLevelInit();
  // Allow reconnection
  if (ret) ret = wifiStatusSet(_WIFI_STA_ENABLED);
  // Start WiFi
  if (ret) ret = wifiStartWiFi();
  return ret;
}

bool wifiStop()
{
  wifiStatusClear(_WIFI_STA_ENABLED);
  return wifiStopWiFi();
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
  
  if (wifiMode() == WIFI_MODE_NULL) {
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
  };
  return ESP_ERR_INVALID_ARG;
}