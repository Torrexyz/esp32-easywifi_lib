#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "easywifi_lib.h"

// ##### //

static const char *TAG = "app";

typedef struct {
  gpio_num_t led;
  bool       led_on;
  int        clientes;
} App_t;

static App_t app = {
  .led = GPIO_NUM_2
};

static const char INDEX_HTML[] =
  "<!DOCTYPE html><html><head><meta charset='utf-8'>"
  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>ESP32 LED</title><style>"
  "body{font-family:sans-serif;text-align:center;margin-top:40px}"
  "button{font-size:22px;padding:14px 32px;margin:8px;border:0;border-radius:10px;color:#fff}"
  "#on{background:#2e7d32}#off{background:#c62828}"
  "</style></head><body>"
  "<h2>Control del LED</h2><p>Estado: <b id='st'>?</b></p>"
  "<button id='on'  onclick=\"led('on')\">Encender</button>"
  "<button id='off' onclick=\"led('off')\">Apagar</button>"
  "<script>"
  "function led(a){fetch('/led?state='+a).then(r=>r.text()).then(t=>st.textContent=t);}"
  "fetch('/led').then(r=>r.text()).then(t=>st.textContent=t);"
  "</script></body></html>"
;

// ##### //

static void app_on_connected(const char *ip, void *ctx) {
  ESP_LOGI(TAG, "Connected, my IP is %s", ip);
}

static void app_on_disconnected(void *ctx) {
  ESP_LOGW(TAG, "Router lost");
}

static void app_on_connect_fail(void *ctx) {
  ESP_LOGW(TAG, "Router unreachable, the library keeps retrying");
}

static void app_on_internet(bool ok, void *ctx) {
  ESP_LOGI(TAG, "Internet access: %s", ok ? "YES" : "NO");
}

static void app_on_client(const uint8_t mac[6], bool joined, void *ctx) {
  App_t *a = (App_t *)ctx;
  a->clientes += joined ? 1 : -1;
  ESP_LOGI(TAG, "Clients in AP: %d", a->clientes);
}

// ##### //

static const char *app_route_led(const char *params, void *ctx) {
  App_t *a = (App_t *)ctx;
  char state[8];

  if(easywifi_query_get(params, "state", state, sizeof(state))) {
    a->led_on = (strcmp(state, "on") == 0);
    gpio_set_level(a->led, a->led_on);
  }
  return a->led_on ? "ENCENDIDO" : "APAGADO";
}

static const char *app_route_status(const char *params, void *ctx) {
  App_t *a = (App_t *)ctx;
  static char json[96];
  snprintf(json, sizeof(json),
    "{\"led\":%s,\"clients\":%d,\"internet\":%s}",
    a->led_on ? "true" : "false", a->clientes,
    easywifi_has_internet() ? "true" : "false"
  );
  return json;
}

// ##### //

static const easywifi_route_t routes[] = {
  EASYWIFI_ROUTE("/led", app_route_led),
  EASYWIFI_ROUTE_EX("/status", app_route_status, EASYWIFI_GET, "application/json"),
};

//..........//

void app_main(void) {

  gpio_reset_pin(app.led);
  gpio_set_direction(app.led, GPIO_MODE_OUTPUT);
  gpio_set_level(app.led, 0);

  easywifi_config_t WifiLibConfig = {
    .AP_SSID           = "YOUR_SSID",
    .AP_PASS           = "YOUR_PASSWORD",

    .on_connected      = app_on_connected,
    .on_disconnected   = app_on_disconnected,
    .on_connect_fail   = app_on_connect_fail,
    .on_internet       = app_on_internet,
    .on_ap_client      = app_on_client,

    EASYWIFI_ROUTES(routes),
    .index_html        = INDEX_HTML,
    .ctx               = &app,
  };

  ESP_ERROR_CHECK(easywifi_init(&WifiLibConfig));

  while(1) {

    if(easywifi_has_internet()) {
      // ...
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
