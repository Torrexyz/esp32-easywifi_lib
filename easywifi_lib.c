#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_http_server.h"

#include "easywifi_lib.h"

//..........//

#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_FAIL_BIT         BIT1
#define NET_TASK_DONE_BIT     BIT2

#define DEFAULT_MAX_RETRIES   5
#define DEFAULT_RETRY_MAX_MS  60000
#define RETRY_BASE_MS         1000

#define NET_TASK_STACK        4096
#define NET_TASK_PRIO         3
#define NET_TASK_CORE         0

#define PROBE_URL             "http://connectivitycheck.gstatic.com/generate_204"
#define PROBE_TIMEOUT_MS      4000

static const char *TAG = "easywifi_lib";

//..........//

// --- Configuration (copy) ---
static easywifi_config_t          s_cfg;

// --- Resources owned by the library (NULL/false = not created) ---
static EventGroupHandle_t           s_events        = NULL;
static esp_netif_t                 *s_netif_sta     = NULL;
static esp_netif_t                 *s_netif_ap      = NULL;
static esp_event_handler_instance_t s_wifi_handler  = NULL;
static esp_event_handler_instance_t s_ip_handler    = NULL;
static esp_timer_handle_t           s_retry_timer   = NULL;
static httpd_handle_t               s_server        = NULL;
static TaskHandle_t                 s_net_task      = NULL;
static bool                         s_wifi_inited   = false;
static bool                         s_wifi_started  = false;

// --- Global ESP-IDF services: initialized once, never released (shared with the app) ---
static bool                         s_nvs_ready     = false;
static bool                         s_netif_ready   = false;

// --- State ---
static bool                         s_initialized   = false;
static volatile bool                s_stopping      = false;
static volatile bool                s_has_internet  = false;  // written ONLY by the monitor task
static uint32_t                     s_attempts      = 0;

static const char                   DEFAULT_HTML[]  =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32</title></head><body style='font-family:sans-serif;text-align:center'>"
"<h2>easywifi_lib running</h2><p>Set index_html in the configuration.</p>"
"</body></html>";

//..........//

// ======================== //
// 0. Configuration checks  //
// ======================== //

static esp_err_t validate_config(const easywifi_config_t *c) {

  ESP_RETURN_ON_FALSE(c != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");

  size_t ap_ssid  = strnlen(c->AP_SSID,  sizeof(c->AP_SSID));
  size_t ap_pass  = strnlen(c->AP_PASS,  sizeof(c->AP_PASS));
  size_t sta_pass = strnlen(c->STA_PASS, sizeof(c->STA_PASS));

  ESP_RETURN_ON_FALSE(ap_ssid > 0 && ap_ssid <= 32, ESP_ERR_INVALID_ARG, TAG,
    "AP_SSID must be 1-32 characters");
  ESP_RETURN_ON_FALSE(ap_pass == 0 || (ap_pass >= 8 && ap_pass <= 64), ESP_ERR_INVALID_ARG, TAG,
    "AP_PASS must be empty (open) or 8-64 characters");
  ESP_RETURN_ON_FALSE(sta_pass == 0 || (sta_pass >= 8 && sta_pass <= 64), ESP_ERR_INVALID_ARG, TAG,
    "STA_PASS must be empty (open) or 8-64 characters");
  ESP_RETURN_ON_FALSE(c->AP_CHANNEL <= 13, ESP_ERR_INVALID_ARG, TAG,
    "AP_CHANNEL must be 0 (default) or 1-13");
  ESP_RETURN_ON_FALSE(c->n_routes == 0 || c->routes != NULL, ESP_ERR_INVALID_ARG, TAG,
    "n_routes > 0 but routes is NULL");

  for(int i = 0; i < c->n_routes; i++) {
    const easywifi_route_t *r = &c->routes[i];
    ESP_RETURN_ON_FALSE(r->uri != NULL && r->uri[0] == '/', ESP_ERR_INVALID_ARG, TAG,
      "route %d: uri must start with '/'", i);
    ESP_RETURN_ON_FALSE(r->handler != NULL, ESP_ERR_INVALID_ARG, TAG,
      "route %d (%s): handler is NULL", i, r->uri);
    ESP_RETURN_ON_FALSE(r->method == EASYWIFI_GET || r->method == EASYWIFI_POST, ESP_ERR_INVALID_ARG, TAG,
      "route %d (%s): invalid method", i, r->uri);
    ESP_RETURN_ON_FALSE(!(r->method == EASYWIFI_GET && strcmp(r->uri, "/") == 0), ESP_ERR_INVALID_ARG, TAG,
      "route %d: GET \"/\" is reserved for index_html", i);
  }

  return ESP_OK;
}

//..........//

// ============================================= //
// 1. Reconnection with exponential backoff      //
// ============================================= //

static uint32_t backoff_ms(uint32_t attempt, uint32_t max_ms) {
  uint32_t shift = (attempt > 0) ? attempt - 1 : 0;
  if(shift > 16) shift = 16;
  uint32_t delay = RETRY_BASE_MS << shift;  // 1s, 2s, 4s, 8s...
  return (delay > max_ms) ? max_ms : delay;
}

static void retry_timer_cb(void *arg) {
  if(!s_stopping) esp_wifi_connect();  // non-blocking
}

// Called from the event handler: never blocks, just schedules the next attempt
static void schedule_reconnect(const easywifi_config_t *cfg) {

  s_attempts++;

  uint32_t max_ms       = cfg->STA_RETRY_MAX_MS ? cfg->STA_RETRY_MAX_MS : DEFAULT_RETRY_MAX_MS;
  uint32_t notify_after = cfg->STA_MAX_RETRIES  ? cfg->STA_MAX_RETRIES  : DEFAULT_MAX_RETRIES;
  uint32_t delay        = backoff_ms(s_attempts, max_ms);

  if(s_attempts == notify_after) {
    ESP_LOGE(TAG, "Could not connect to '%s' after %u attempts, retrying in background", cfg->STA_SSID, (unsigned)s_attempts);
    xEventGroupSetBits(s_events, WIFI_FAIL_BIT);
    if(cfg->on_connect_fail) cfg->on_connect_fail(cfg->ctx);
  }

  ESP_LOGW(TAG, "Retry %u in %u ms", (unsigned)s_attempts, (unsigned)delay);
  esp_timer_stop(s_retry_timer);  // ignore error if it was not running
  esp_timer_start_once(s_retry_timer, (uint64_t)delay * 1000);
}

//..........//

// ======================================= //
// 2. Events of ESP-IDF -> app's callbacks //
// ======================================= //

static void notify_monitor(void) {
  if(s_net_task) xTaskNotifyGive(s_net_task);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {

  const easywifi_config_t *cfg = (const easywifi_config_t *)arg;
  if(s_stopping) return;

  if(base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {

    s_attempts = 0;
    esp_wifi_connect();

  } else if(base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {

    wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
    bool was_connected = xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT;
    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);

    if(was_connected) {
      ESP_LOGW(TAG, "Router lost (reason %d)", (int)e->reason);
      notify_monitor();  // the monitor reports on_internet(false)
      if(cfg->on_disconnected) cfg->on_disconnected(cfg->ctx);
    }

    schedule_reconnect(cfg);

  } else if(base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {

    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    char ip[16];
    esp_ip4addr_ntoa(&e->ip_info.ip, ip, sizeof(ip));
    ESP_LOGI(TAG, "Connected to the router. IP: %s", ip);

    s_attempts = 0;
    esp_timer_stop(s_retry_timer);
    xEventGroupClearBits(s_events, WIFI_FAIL_BIT);
    xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);

    if(cfg->on_connected) cfg->on_connected(ip, cfg->ctx);
    notify_monitor();  // check internet right away

  } else if(base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {

    wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
    ESP_LOGI(TAG, "Client " MACSTR " joined the AP (AID=%d)", MAC2STR(e->mac), e->aid);
    if(cfg->on_ap_client) cfg->on_ap_client(e->mac, true, cfg->ctx);

  } else if(base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {

    wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
    ESP_LOGI(TAG, "Client " MACSTR " left the AP", MAC2STR(e->mac));
    if(cfg->on_ap_client) cfg->on_ap_client(e->mac, false, cfg->ctx);

  }
}

//..........//

// ======================= //
// 3. Wi-Fi initialization //
// ======================= //

static esp_err_t nvs_init(void) {
  if(s_nvs_ready) return ESP_OK;

  esp_err_t ret = nvs_flash_init();
  if(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase failed");
    ret = nvs_flash_init();
  }

  if(ret == ESP_OK) s_nvs_ready = true;
  return ret;
}

static esp_err_t wifi_start(const easywifi_config_t *cfg) {

  bool use_sta = cfg->STA_SSID[0] != '\0';

  s_events = xEventGroupCreate();
  ESP_RETURN_ON_FALSE(s_events != NULL, ESP_ERR_NO_MEM, TAG, "event group alloc failed");

  if(!s_netif_ready) {
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    s_netif_ready = true;
  }

  // If the app (or another component) already created it, that is fine
  esp_err_t err = esp_event_loop_create_default();
  ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG,
    "esp_event_loop_create_default failed");

  if(use_sta) {
    s_netif_sta = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_netif_sta != NULL, ESP_FAIL, TAG, "STA netif creation failed");
  }
  s_netif_ap = esp_netif_create_default_wifi_ap();
  ESP_RETURN_ON_FALSE(s_netif_ap != NULL, ESP_FAIL, TAG, "AP netif creation failed");

  wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "esp_wifi_init failed");
  s_wifi_inited = true;

  // The config comes from code on every boot: keep it in RAM (no flash wear, no stale config)
  ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "esp_wifi_set_storage failed");

  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
    WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, (void *)cfg, &s_wifi_handler),
    TAG, "WIFI_EVENT register failed");
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
    IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, (void *)cfg, &s_ip_handler),
    TAG, "IP_EVENT register failed");

  if(use_sta) {
    esp_timer_create_args_t targs = { .callback = retry_timer_cb, .name = "easywifi_retry" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_retry_timer), TAG, "retry timer creation failed");
  }

  // --- STA ---
  wifi_config_t sta_cfg = { 0 };
  strlcpy((char *)sta_cfg.sta.ssid,     cfg->STA_SSID, sizeof(sta_cfg.sta.ssid));
  strlcpy((char *)sta_cfg.sta.password, cfg->STA_PASS, sizeof(sta_cfg.sta.password));
  sta_cfg.sta.threshold.authmode = (cfg->STA_PASS[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

  // --- AP ---
  wifi_config_t ap_cfg = { 0 };
  strlcpy((char *)ap_cfg.ap.ssid,     cfg->AP_SSID, sizeof(ap_cfg.ap.ssid));
  strlcpy((char *)ap_cfg.ap.password, cfg->AP_PASS, sizeof(ap_cfg.ap.password));
  ap_cfg.ap.ssid_len       = strlen(cfg->AP_SSID);
  ap_cfg.ap.channel        = cfg->AP_CHANNEL ? cfg->AP_CHANNEL : 1;
  ap_cfg.ap.max_connection = cfg->AP_MAX_CONN ? cfg->AP_MAX_CONN : 4;
  ap_cfg.ap.authmode       = (cfg->AP_PASS[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(use_sta ? WIFI_MODE_APSTA : WIFI_MODE_AP), TAG, "esp_wifi_set_mode failed");
  if(use_sta) {
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG, "STA config failed");
  }
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "AP config failed");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");
  s_wifi_started = true;

  if(!use_sta) xEventGroupSetBits(s_events, WIFI_FAIL_BIT);   // wait_connected() returns at once

  ESP_LOGI(TAG, "AP '%s' active. Go to http://192.168.4.1", cfg->AP_SSID);
  return ESP_OK;
}

//..........//

// =================================================== //
// 4. Web server: a generic handler for all app routes //
// =================================================== //

static esp_err_t root_handler(httpd_req_t *req) {
  const char *html = s_cfg.index_html ? s_cfg.index_html : DEFAULT_HTML;
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

// Reads the POST body into buf (NUL-terminated)
static esp_err_t read_body(httpd_req_t *req, char *buf, size_t buf_len) {

  if(req->content_len >= buf_len) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
    return ESP_FAIL;
  }

  size_t received = 0;
  while(received < req->content_len) {
    int r = httpd_req_recv(req, buf + received, req->content_len - received);
    if(r == HTTPD_SOCK_ERR_TIMEOUT) continue;  // try again
    if(r <= 0) return ESP_FAIL;                // connection closed or error
    received += r;
  }

  buf[received] = '\0';
  return ESP_OK;
}

static esp_err_t generic_route_handler(httpd_req_t *req) {

  const easywifi_route_t *route = (const easywifi_route_t *)req->user_ctx;
  char params[EASYWIFI_MAX_PARAMS_LEN] = "";

  if(route->method == EASYWIFI_POST) {
    if(read_body(req, params, sizeof(params)) != ESP_OK) return ESP_FAIL;
  } else {
    esp_err_t err = httpd_req_get_url_query_str(req, params, sizeof(params));
    if(err == ESP_ERR_HTTPD_RESULT_TRUNC) {
      httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "Query too long");
      return ESP_FAIL;
    }
    // ESP_ERR_NOT_FOUND = no query: params stays ""
  }

  const char *resp = route->handler(params, s_cfg.ctx);

  httpd_resp_set_type(req, route->content_type ? route->content_type : "text/plain");
  return httpd_resp_sendstr(req, resp ? resp : "");
}

static esp_err_t start_webserver(const easywifi_config_t *cfg) {

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  if(cfg->n_routes + 1 > config.max_uri_handlers) {
    config.max_uri_handlers = cfg->n_routes + 1;
  }
  config.lru_purge_enable = true;  // free old sockets if clients pile up

  ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "httpd_start failed");

  httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &root), TAG, "could not register \"/\"");

  for(int i = 0; i < cfg->n_routes; i++) {
    const easywifi_route_t *r = &cfg->routes[i];

    httpd_uri_t u = {
      .uri      = r->uri,
      .method   = (r->method == EASYWIFI_POST) ? HTTP_POST : HTTP_GET,
      .handler  = generic_route_handler,
      .user_ctx = (void *)r,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &u), TAG,
      "could not register route %s (duplicated?)", r->uri);
    ESP_LOGI(TAG, "Route registered: %s %s", (r->method == EASYWIFI_POST) ? "POST" : "GET", r->uri);
  }

  return ESP_OK;
}

//..........//

// ============================== //
// 5. Internet monitor (own task) //
// ============================== //

static bool internet_probe(void) {

  esp_http_client_config_t cfg = {
    .url        = PROBE_URL,
    .timeout_ms = PROBE_TIMEOUT_MS,
  };

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if(client == NULL) return false;

  esp_err_t err = esp_http_client_perform(client);
  int status    = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  return (err == ESP_OK && status == 204);
}

// The ONLY writer of s_has_internet and the only caller of on_internet
static void set_internet_state(const easywifi_config_t *cfg, bool ok, bool force_notify) {
  if(ok == s_has_internet && !force_notify) return;
  s_has_internet = ok;
  ESP_LOGI(TAG, "Internet: %s", ok ? "YES" : "NO");
  if(cfg->on_internet) cfg->on_internet(ok, cfg->ctx);
}

static void internet_monitor_task(void *arg) {

  const easywifi_config_t *cfg = (const easywifi_config_t *)arg;
  TickType_t wait     = portMAX_DELAY;  // sleep until the first connection
  bool       reported = false;

  while(!s_stopping) {

    // Wakes up on: timeout (periodic check), connection, disconnection or deinit
    ulTaskNotifyTake(pdTRUE, wait);
    if(s_stopping) break;

    if(!(xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT)) {
      set_internet_state(cfg, false, false);  // no router = no internet
      wait = portMAX_DELAY;                   // 0 % CPU until it reconnects
      continue;
    }

    bool ok = internet_probe();
    if(s_stopping) break;

    // The router may have dropped while probing
    bool still_connected = xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT;
    set_internet_state(cfg, ok && still_connected, !reported);
    reported = true;

    wait = still_connected ? pdMS_TO_TICKS(cfg->STA_INTERNET_CHECK_MS) : portMAX_DELAY;
  }

  xEventGroupSetBits(s_events, NET_TASK_DONE_BIT);  // tell deinit we are out
  vTaskDelete(NULL);
}

static esp_err_t start_internet_monitor(const easywifi_config_t *cfg) {

  if(cfg->STA_INTERNET_CHECK_MS == 0 || cfg->STA_SSID[0] == '\0') return ESP_OK;  // disabled

  BaseType_t ok = xTaskCreatePinnedToCore(
    internet_monitor_task,
    "easywifi_net",
    NET_TASK_STACK, (void *)cfg,
    NET_TASK_PRIO, &s_net_task, NET_TASK_CORE
  );

  ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "could not create the monitor task");
  return ESP_OK;
}

//..........//

// ==================================================== //
// 6. Resource release (used by deinit and failed init) //
// ==================================================== //

// Frees whatever exists, in reverse order of creation. Safe with partial states.
static void release_resources(void) {

  s_stopping = true;  // callbacks and timer stop acting

  if(s_net_task) {
    xTaskNotifyGive(s_net_task);
    // Bounded: at most the duration of one probe (PROBE_TIMEOUT_MS + DNS)
    xEventGroupWaitBits(s_events, NET_TASK_DONE_BIT, pdTRUE, pdTRUE, portMAX_DELAY);
    s_net_task = NULL;
  }

  if(s_server) {
    httpd_stop(s_server);
    s_server = NULL;
  }

  if(s_wifi_handler) {
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
    s_wifi_handler = NULL;
  }
  if(s_ip_handler) {
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler);
    s_ip_handler = NULL;
  }

  if(s_retry_timer) {
    esp_timer_stop(s_retry_timer);
    esp_timer_delete(s_retry_timer);
    s_retry_timer = NULL;
  }

  if(s_wifi_started) { esp_wifi_stop();   s_wifi_started = false; }
  if(s_wifi_inited)  { esp_wifi_deinit(); s_wifi_inited  = false; }

  if(s_netif_sta) { esp_netif_destroy_default_wifi(s_netif_sta); s_netif_sta = NULL; }
  if(s_netif_ap)  { esp_netif_destroy_default_wifi(s_netif_ap);  s_netif_ap  = NULL; }

  if(s_events) {
    vEventGroupDelete(s_events);
    s_events = NULL;
  }

  s_has_internet = false;
  s_attempts     = 0;
}

//..........//

// ============= //
// 7. Public API //
// ============= //

esp_err_t easywifi_init(const easywifi_config_t *config) {

  if(s_initialized) return ESP_ERR_INVALID_STATE;
  ESP_RETURN_ON_ERROR(validate_config(config), TAG, "invalid configuration");  // before touching hardware

  s_cfg          = *config;
  s_stopping     = false;
  s_has_internet = false;
  s_attempts     = 0;

  esp_err_t ret = ESP_OK;
  ESP_GOTO_ON_ERROR(nvs_init(),                     fail, TAG, "NVS init failed");
  ESP_GOTO_ON_ERROR(wifi_start(&s_cfg),             fail, TAG, "Wi-Fi start failed");
  ESP_GOTO_ON_ERROR(start_webserver(&s_cfg),        fail, TAG, "web server start failed");
  ESP_GOTO_ON_ERROR(start_internet_monitor(&s_cfg), fail, TAG, "internet monitor start failed");

  s_initialized = true;
  return ESP_OK;

  fail:
    release_resources();  // leave everything clean to retry
    return ret;
}

esp_err_t easywifi_deinit(void) {
  if(!s_initialized) return ESP_ERR_INVALID_STATE;
  release_resources();
  s_initialized = false;
  ESP_LOGI(TAG, "Stopped and released");
  return ESP_OK;
}

bool easywifi_wait_connected(uint32_t timeout_ms) {
  if(!s_initialized) return false;

  EventBits_t bits = xEventGroupWaitBits(
    s_events,
    WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
    pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms)
  );

  return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool easywifi_is_connected(void) {
  if(!s_initialized) return false;
  return (xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT) != 0;
}

bool easywifi_has_internet(void) {
  return s_initialized && s_has_internet;
}

bool easywifi_internet_ok(void) {
  if(!easywifi_is_connected()) return false;
  return internet_probe();  // does not touch the monitor state
}

esp_err_t easywifi_reconnect(void) {
  if(!s_initialized || s_cfg.STA_SSID[0] == '\0') return ESP_ERR_INVALID_STATE;
  if(easywifi_is_connected()) return ESP_OK;

  esp_timer_stop(s_retry_timer);
  s_attempts = 0;
  xEventGroupClearBits(s_events, WIFI_FAIL_BIT);
  
  return esp_wifi_connect();
}

bool easywifi_query_get(const char *params, const char *key, char *out, size_t out_len) {
  if(params == NULL || key == NULL || out == NULL || out_len == 0) return false;
  return httpd_query_key_value(params, key, out, out_len) == ESP_OK;
}
