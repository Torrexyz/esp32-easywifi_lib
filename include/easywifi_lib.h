#ifndef EASYWIFI_LIB_H
#define EASYWIFI_LIB_H

/*
 * easywifi_lib — Wi-Fi (AP + STA), web server and internet monitor for ESP-IDF.
 *
 * WHERE EACH CALLBACK RUNS (important: none of them may block)
 * ------------------------------------------------------------
 *  on_connected, on_disconnected,   -> ESP-IDF event task ("sys_evt", small stack).
 *  on_connect_fail, on_ap_client       Keep them short: set a flag, send to a queue...
 *
 *  on_internet                      -> easywifi monitor task ("easywifi_net").
 *
 *  route handlers                   -> HTTP server task ("httpd").
 *
 * They all run in tasks different from app_main. If a callback and your own
 * loop modify several related fields of your state, protect them with a mutex.
 *
 * RULES
 * -----
 *  - easywifi_init() / easywifi_deinit() must be called from the same
 *    task (e.g. app_main), never from inside a callback.
 *  - The library keeps a shallow copy of the config: 'routes', 'index_html',
 *    'ctx' and the strings returned by the routes must stay alive (static/const).
 */

//..........//

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

//..........//

// Max length of the GET query string or POST body received by a route
#define EASYWIFI_MAX_PARAMS_LEN 256

//..........//

typedef void (*easywifi_conn_cb_t)(const char *ip, void *ctx);
typedef void (*easywifi_event_cb_t)(void *ctx);
typedef void (*easywifi_client_cb_t)(const uint8_t mac[6], bool joined, void *ctx);
typedef void (*easywifi_net_cb_t)(bool ok, void *ctx);

// params: GET -> query string ("state=on"), POST -> request body. "" if empty.
// Returns the response text (must stay alive after returning). NULL = empty response.
typedef const char *(*easywifi_route_cb_t)(const char *params, void *ctx);

typedef enum {
  EASYWIFI_GET = 0,                   // default when omitted
  EASYWIFI_POST,
} easywifi_method_t;

typedef struct {
  const char          *uri;           // must start with '/'. GET "/" is reserved for index_html
  easywifi_route_cb_t  handler;
  easywifi_method_t    method;        // optional, default EASYWIFI_GET
  const char          *content_type;  // optional, default "text/plain"
} easywifi_route_t;

// Route helpers (designated initializers: no -Wmissing-field-initializers)
#define EASYWIFI_ROUTE(u, h)           { .uri = (u), .handler = (h) }
#define EASYWIFI_ROUTE_EX(u, h, m, ct) { .uri = (u), .handler = (h), .method = (m), .content_type = (ct) }

// Fills .routes and .n_routes from an array declared by the app.
// Use it in the config: EASYWIFI_ROUTES(routes),
#define EASYWIFI_ROUTES(arr) .routes = (arr), .n_routes = sizeof(arr) / sizeof((arr)[0])

//..........//

typedef struct {

  // --- Configuration ---
  char     STA_SSID[32 + 1];              // "" = AP only (no router)
  char     STA_PASS[64 + 1];              // "" = open network, otherwise 8-64 chars
  uint8_t  STA_MAX_RETRIES;               // failed attempts before on_connect_fail (0 = 5). It keeps retrying in background anyway.
  uint32_t STA_RETRY_MAX_MS;              // max wait between retries, backoff 1s,2s,4s... (0 = 60000)
  uint32_t STA_INTERNET_CHECK_MS;         // 0 = internet monitor disabled
  char     AP_SSID[32 + 1];               // required, 1-32 chars
  char     AP_PASS[64 + 1];               // "" = open AP, otherwise 8-64 chars
  uint8_t  AP_CHANNEL;                    // 1-13 (0 = 1)
  uint8_t  AP_MAX_CONN;                   // 0 = 4

  // --- App's logic (all optional, NULL = not interested) ---
  easywifi_conn_cb_t    on_connected;     // got IP from the router
  easywifi_event_cb_t   on_disconnected;  // lost the router
  easywifi_event_cb_t   on_connect_fail;  // STA_MAX_RETRIES failed attempts in a row
  easywifi_client_cb_t  on_ap_client;     // a device joined/left the AP
  easywifi_net_cb_t     on_internet;      // internet access changed (+ first result)

  const easywifi_route_t *routes;           // set both with EASYWIFI_ROUTES(arr)
  uint8_t               n_routes;
  const char           *index_html;       // page served at "/" (NULL = default page)

  void *ctx;                              // app state, passed to every callback
} easywifi_config_t;

//..........//

// Validates the config and starts Wi-Fi, web server and internet monitor. Does not block.
// On error everything created so far is released, so it can be called again.
esp_err_t easywifi_init(const easywifi_config_t *config);

// Stops everything and frees all resources. easywifi_init() can be called again later.
esp_err_t easywifi_deinit(void);

// Waits until connected to the router (true) or STA_MAX_RETRIES failures / timeout (false).
bool easywifi_wait_connected(uint32_t timeout_ms);

// Connected to the router right now? Instant.
bool easywifi_is_connected(void);

// Last result of the internet monitor. Instant, does not block.
bool easywifi_has_internet(void);

// Checks internet right now with an HTTP request. Blocks up to ~4 s.
// Do not call it from a callback.
bool easywifi_internet_ok(void);

// Resets the backoff and tries to connect to the router immediately.
esp_err_t easywifi_reconnect(void);

// Extracts a value from a query/body ("state=on&x=1", "state" -> "on").
bool easywifi_query_get(const char *params, const char *key, char *out, size_t out_len);

//..........//

#endif
