#if (!PLATFORMIO)
  // Enable Arduino-ESP32 logging in Arduino IDE
  #ifdef CORE_DEBUG_LEVEL
    #undef CORE_DEBUG_LEVEL
  #endif
  #ifdef LOG_LOCAL_LEVEL
    #undef LOG_LOCAL_LEVEL
  #endif

  #define CORE_DEBUG_LEVEL 3
  #define LOG_LOCAL_LEVEL CORE_DEBUG_LEVEL
#endif  

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"
#include <string.h>
#include <unistd.h>
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include <cmath>
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_sntp.h"
#include "mqtt_client.h"
#include "esp_heap_caps.h"
#include <string>
#include "esp_http_server.h"


#define LEN_MAC_ADDR 20
#define DEFAULT_SSID "kiwifi :3"
#define DEFAULT_PWD "mangotango"
#define API_HOST "192.168.1.96"

#if CONFIG_WIFI_ALL_CHANNEL_SCAN
#define DEFAULT_SCAN_METHOD WIFI_ALL_CHANNEL_SCAN
#elif CONFIG_WIFI_FAST_SCAN
#define DEFAULT_SCAN_METHOD WIFI_FAST_SCAN
#else
#define DEFAULT_SCAN_METHOD WIFI_FAST_SCAN
#endif /*CONFIG_SCAN_METHOD*/

#if CONFIG_WIFI_CONNECT_AP_BY_SIGNAL
#define DEFAULT_SORT_METHOD WIFI_CONNECT_AP_BY_SIGNAL
#elif CONFIG_WIFI_CONNECT_AP_BY_SECURITY
#define DEFAULT_SORT_METHOD WIFI_CONNECT_AP_BY_SECURITY
#else
#define DEFAULT_SORT_METHOD WIFI_CONNECT_AP_BY_SIGNAL
#endif /*CONFIG_SORT_METHOD*/

#if CONFIG_FAST_SCAN_THRESHOLD
#define DEFAULT_RSSI CONFIG_FAST_SCAN_MINIMUM_SIGNAL
#if CONFIG_EXAMPLE_OPEN
#define DEFAULT_AUTHMODE WIFI_AUTH_OPEN
#elif CONFIG_EXAMPLE_WEP
#define DEFAULT_AUTHMODE WIFI_AUTH_WEP
#elif CONFIG_EXAMPLE_WPA
#define DEFAULT_AUTHMODE WIFI_AUTH_WPA_PSK
#elif CONFIG_EXAMPLE_WPA2
#define DEFAULT_AUTHMODE WIFI_AUTH_WPA2_PSK
#else
#define DEFAULT_AUTHMODE WIFI_AUTH_OPEN
#endif
#else
#define DEFAULT_RSSI -127
#define DEFAULT_AUTHMODE WIFI_AUTH_OPEN
#endif /*CONFIG_FAST_SCAN_THRESHOLD*/
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define PING_END_BIT       BIT0

static const char *TAG = "scan";
static const char *topic = "/data";

typedef int func(void);	// Allow to declare function "hidden" in memory

typedef struct {
// Header of the raw packet
	/*unsigned frame_ctrl: 16;
	unsigned duration_id: 16;*/
	uint8_t subtype[1];
	uint8_t ctrl_field[1];
	uint8_t duration[2];
	uint8_t addr1[6]; /* receiver address */
	uint8_t addr2[6]; /* sender address */
	uint8_t addr3[6]; /* filtering address */

	uint8_t SC[2];
	uint64_t timestamp_abs;
	uint8_t beacon_interval[2];
	uint8_t capability_info[2];

} wifi_ieee80211_mac_hdr_t;

typedef struct {
// Packet is header + payload
	wifi_ieee80211_mac_hdr_t hdr;
	uint8_t payload[0]; /* network data ended with 4 bytes csum (CRC32) */
} wifi_ieee80211_packet_t;

static int MAXIMUM_RETRY = 10;
static int retry_num = 0;
static EventGroupHandle_t s_wifi_event_group;
static EventGroupHandle_t ping_event_group;
static esp_mqtt_client_config_t mqtt_client_cfg;
static esp_mqtt_client_handle_t mqtt_client;
static esp_http_client_config_t http_client_cfg;
static esp_ping_config_t ping_config;
static esp_ping_handle_t ping;
static TickType_t xDelay;

static char * message;
static char local_response_buffer[2049] ;
static char ip_addr[50];
int count = 10;

//WIFI_EVENT //IP_EVENT
static void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  ip_event_got_ip_t* got_ip_event;
  if (event_id == WIFI_EVENT_STA_START) {
    ESP_LOGI(TAG, "WIFI_EVENT_STA_START");
    ESP_ERROR_CHECK(esp_wifi_connect());
  }
  else if (event_id == IP_EVENT_STA_GOT_IP) {
    ESP_LOGI(TAG, "IP_EVENT_STA_GOT_IP");
    got_ip_event = (ip_event_got_ip_t*) event_data;
    ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&got_ip_event->ip_info.ip));
    sprintf(ip_addr, "{\"ip\": \"%d.%d.%d.%d\"}", IP2STR(&got_ip_event->ip_info.ip));
    retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
  else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
    ESP_LOGI(TAG, "WIFI_EVENT_STA_DISCONNECTED");
    if (retry_num < MAXIMUM_RETRY) {
      ESP_LOGI(TAG, "retries left: %d", MAXIMUM_RETRY - retry_num);
      ESP_ERROR_CHECK(esp_wifi_connect());
      retry_num++;
      ESP_LOGI(TAG, "retry to connect to the AP");
    } else {
      ESP_LOGI(TAG,"xEventGroupSetBits to the AP fail");
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
  }
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    static char *output_buffer;  // Buffer to store response of http request from event handler
    static int output_len;       // Stores number of bytes read
    switch(evt->event_id) {
        case HTTP_EVENT_ON_FINISH: {
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            if (output_buffer != NULL) {
                // Response is accumulated in output_buffer. Uncomment the below line to print the accumulated response
                // ESP_LOG_BUFFER_HEX(TAG, output_buffer, output_len);
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
        } break;
        case HTTP_EVENT_DISCONNECTED: {
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            int mbedtls_err = 0;
            esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data, &mbedtls_err, NULL);
            if (err != 0) {
                ESP_LOGD(TAG, "Last esp error code: 0x%x", err);
                ESP_LOGD(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
            if (output_buffer != NULL) {
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
        } break;

    }
    return ESP_OK;
}

void receive_csi_cb(void *ctx, wifi_csi_info_t *data) {
/* 
 * Goal : Get Channel State Information Packets and fill fields accordingly
 * In : Context (null), CSI packet
 * Out : Null, Fill fields of corresponding AP
 * 
 */ 
	wifi_csi_info_t received = data[0];
	char senddMacChr[LEN_MAC_ADDR] = {0}; // Sender
  char destdMacChr[LEN_MAC_ADDR] = {0}; // Destination
  //char filter[] = "10:C4:CA:10:13:44";
	sprintf(senddMacChr, "%02X:%02X:%02X:%02X:%02X:%02X", received.mac[0], received.mac[1], received.mac[2], received.mac[3], received.mac[4], received.mac[5]);
  sprintf(destdMacChr, "%02X:%02X:%02X:%02X:%02X:%02X", received.dmac[0], received.dmac[1], received.dmac[2], received.dmac[3], received.dmac[4], received.dmac[5]);
	if (received.rx_ctrl.sig_mode==1){
    // if (strcmp(senddMacChr, filter) != 0) { //filter to router IP
    //   return;
    // }
    char sigMode[15];
    if (received.rx_ctrl.sig_mode==0) strcpy(sigMode, "non HT(11bg)");
		if (received.rx_ctrl.sig_mode==1) strcpy(sigMode, "HT(11n)");
		if (received.rx_ctrl.sig_mode==3) strcpy(sigMode, "VHT(11ac)");
    uint8_t cwb = received.rx_ctrl.cwb == 1 ? 40 : 20;

    //form JSON data
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "senddMacChr", senddMacChr);
    cJSON_AddStringToObject(json, "destdMacChr", destdMacChr);
    cJSON_AddStringToObject(json, "sigMode", sigMode);
    cJSON_AddNumberToObject(json, "channel", received.rx_ctrl.channel);
    cJSON_AddNumberToObject(json, "channelSecondary", received.rx_ctrl.secondary_channel);
    cJSON_AddNumberToObject(json, "cwb", cwb);
    cJSON_AddBoolToObject(json, "stbc", received.rx_ctrl.stbc == 1);
    cJSON_AddNumberToObject(json, "length", received.len);
    cJSON_AddNumberToObject(json, "rssi", received.rx_ctrl.rssi);
    cJSON_AddNumberToObject(json, "noiseFloor", received.rx_ctrl.noise_floor);
    cJSON_AddNumberToObject(json, "timestamp", received.rx_ctrl.timestamp);
    cJSON *csiArray = cJSON_CreateArray();
		int8_t* my_ptr=data->buf;
		for(int i=0;i<data->len;i++){
      cJSON_AddItemToArray(csiArray, cJSON_CreateNumber(my_ptr[i]));
		}
    cJSON_AddItemToObject(json, "csi", csiArray);
    if (message == NULL || strlen(message) < 10) {
      message = cJSON_PrintUnformatted(json);
    }
  
    cJSON_Delete(json);
	} else {
		//printf("This is invalid CSI until Espressif fix issue https://github.com/espressif/esp-idf/issues/2909\n"); 
	}
}

static void cb_on_ping_success(esp_ping_handle_t hdl, void *args) {}

static void cb_on_ping_timeout(esp_ping_handle_t hdl, void *args) {}

//ping end cb
static void cb_on_ping_end(esp_ping_handle_t hdl, void *args) {
    xEventGroupSetBits(ping_event_group, PING_END_BIT);
}

//data uri handler
esp_err_t my_uri_handler(httpd_req_t *req)
{
    count = 0;
    return ESP_OK;
}

void setup() {
  Serial.begin(9600);
  esp_log_level_set(TAG, ESP_LOG_VERBOSE);
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  sleep(1);
  
  // event loop set up
  //ESP_EVENT_DEFINE_BASE(WIFI_EVENT);
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START, wifi_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_handler, NULL));
  
  //wifi set up
  esp_netif_init();
  s_wifi_event_group = xEventGroupCreate();
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  wifi_config_t wifi_config = {
    .sta = {
      .ssid = DEFAULT_SSID,
      .password = DEFAULT_PWD,
      .threshold = {
        .authmode = WIFI_AUTH_WPA_WPA2_PSK
      }
    }
  };
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  xDelay = 30000 / portTICK_PERIOD_MS;
  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, xDelay);
  if (bits & WIFI_CONNECTED_BIT) {
      ESP_LOGI(TAG, "connected to ap SSID:%s password:%s", DEFAULT_SSID, DEFAULT_PWD);
  } else if (bits & WIFI_FAIL_BIT) {
      ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s", DEFAULT_SSID, DEFAULT_PWD);
  } else {
      ESP_LOGE(TAG, "UNEXPECTED EVENT");
  }

  //csi set up
  ESP_ERROR_CHECK(esp_wifi_set_csi(1));
	wifi_csi_config_t configuration_csi; // CSI = Channel State Information
	configuration_csi.lltf_en = true;
	configuration_csi.htltf_en = true;
	configuration_csi.stbc_htltf2_en = true;
	configuration_csi.ltf_merge_en = true;
	configuration_csi.channel_filter_en = true;
	configuration_csi.manu_scale = false; // Automatic scalling
	//configuration_csi.shift= false; // 0->15
	ESP_ERROR_CHECK(esp_wifi_set_csi_config(&configuration_csi));
	ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(&receive_csi_cb, NULL));

  //ping set up
  ping_event_group = xEventGroupCreate();
  ping_config = ESP_PING_DEFAULT_CONFIG();
  esp_netif_ip_info_t local_ip;
  esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &local_ip);
  // ESP_LOGI(TAG, "got ip:" IPSTR ", gw: " IPSTR, IP2STR(&local_ip.ip), IP2STR(&local_ip.gw));
  // printf("got ip:" IPSTR ", gw: " IPSTR, IP2STR(&local_ip.ip), IP2STR(&local_ip.gw));
  ping_config.target_addr.u_addr.ip4.addr = ip4_addr_get_u32(&local_ip.gw);
  ping_config.target_addr.type = ESP_IPADDR_TYPE_V4;
  ping_config.interval_ms = 1000;
  ping_config.count = 1;

  esp_sntp_setservername(0, "0.de.pool.ntp.org");
  esp_sntp_setservername(1, "1.de.pool.ntp.org");
  esp_sntp_setservername(2, "2.de.pool.ntp.org");
  esp_sntp_init();

  //http client set up
  http_client_cfg = {
        .host = API_HOST,
        .port = 5000,
        .path = "/data",
        .method = HTTP_METHOD_POST,
        .event_handler = _http_event_handler,
        .user_data = local_response_buffer,
  };

  esp_http_client_config_t http_register_cfg = {
        .host = API_HOST,
        .port = 5000,
        .path = "/register",
        .method = HTTP_METHOD_POST,
        .event_handler = _http_event_handler,
        .user_data = local_response_buffer,
  };
  ESP_LOGD(TAG, "Registering device IP to API server: %s", ip_addr);
  //register device ip - one-time
  esp_http_client_handle_t http_client = esp_http_client_init(&http_register_cfg);
  esp_http_client_set_header(http_client, "Content-Type", "application/json");
  esp_http_client_set_post_field(http_client, ip_addr, strlen(ip_addr));
  esp_err_t err = esp_http_client_perform(http_client);
  if (err == ESP_OK) {
      ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %"PRId64, esp_http_client_get_status_code(http_client), esp_http_client_get_content_length(http_client));
  } else {
      ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
  }
  esp_http_client_cleanup(http_client);
  ESP_LOGD(TAG, "Device Registered");

  esp_ping_callbacks_t cbs = {
      .cb_args = NULL,
      .on_ping_success = cb_on_ping_success,
      .on_ping_timeout = cb_on_ping_timeout,
      .on_ping_end = cb_on_ping_end
  };
  ESP_ERROR_CHECK(esp_ping_new_session(&ping_config, &cbs, &ping));
  
  message = NULL; //initialize data structure for output

  //http server set up
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  httpd_handle_t server = NULL;
  if (httpd_start(&server, &config) == ESP_OK) {
    // Register URI handlers
    httpd_uri_t uri_get = {
        .uri       = "/start",
        .method    = HTTP_GET,
        .handler   = my_uri_handler,
        .user_ctx  = NULL
    };    
    httpd_register_uri_handler(server, &uri_get);
  }

}

void loop() {

  //ping router
  xEventGroupClearBits(ping_event_group, PING_END_BIT);
  ESP_ERROR_CHECK(esp_ping_start(ping));

  //wait until pings are completed
  EventBits_t pingBits = xEventGroupWaitBits(ping_event_group, PING_END_BIT, pdFALSE, pdFALSE, xDelay);
  if (pingBits & PING_END_BIT) {
      ESP_LOGD(TAG, "Completed pings.");
  } else {
      ESP_LOGE(TAG, "UNEXPECTED EVENT");
  }
  
  //upload data
  if (message != NULL && count < 10) {
    esp_http_client_handle_t http_client = esp_http_client_init(&http_client_cfg);
    esp_http_client_set_header(http_client, "Content-Type", "application/json");
    esp_http_client_set_post_field(http_client, message, strlen(message));
    esp_err_t err = esp_http_client_perform(http_client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %"PRId64, esp_http_client_get_status_code(http_client), esp_http_client_get_content_length(http_client));
        count += 1;
    } else {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(http_client);
  }
  // cleanup data structure memory
  // printf("%d\n", heap_caps_get_free_size(MALLOC_CAP_8BIT)); //debug metric for free heap memory
  free(message);
  message = NULL;
}