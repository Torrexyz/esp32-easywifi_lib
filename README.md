# easywifi_lib

Componente para ESP-IDF que encapsula la gestión de Wi-Fi del ESP32: punto de acceso propio, conexión a un router con reconexión automática, servidor web con rutas definidas por la aplicación y monitor de acceso a internet.

La aplicación interactúa con la librería exclusivamente a través de una estructura de configuración y un conjunto de callbacks. No requiere incluir ni utilizar directamente las APIs de red de ESP-IDF (`esp_wifi`, `esp_netif`, `esp_event`, `esp_http_server`, `esp_http_client`).

---

> [!IMPORTANT]
> **El componente debe ubicarse en una carpeta llamada `easywifi_lib`.**
> ESP-IDF asigna a cada componente el nombre de su carpeta, y las aplicaciones lo declaran como `easywifi_lib`. Dado que el repositorio se llama `esp32-easywifi_lib`, debe indicarse siempre el nombre de destino:
>
> ```bash
> # Clonar
> git clone https://github.com/torrexyz/esp32-easywifi_lib.git easywifi_lib
>
> # Submódulo dentro de un proyecto existente
> git submodule add https://github.com/torrexyz/esp32-easywifi_lib.git components/easywifi_lib
> ```
>
> Con el gestor de componentes de ESP-IDF, el nombre lo define la clave del manifiesto (`main/idf_component.yml`):
>
> ```yaml
> dependencies:
>   easywifi_lib:
>     git: https://github.com/torrexyz/esp32-easywifi_lib.git
>     version: v1.0.0
> ```
>
> Si la carpeta tiene otro nombre, la compilación falla con `Failed to resolve component 'easywifi_lib'`.

--

## Tabla de contenidos

1. [Características](#1-características)
2. [Requisitos](#2-requisitos)
3. [Instalación](#3-instalación)
4. [Inicio rápido](#4-inicio-rápido)
5. [Convención de nombres](#5-convención-de-nombres)
6. [Referencia de configuración](#6-referencia-de-configuración)
7. [Rutas web](#7-rutas-web)
8. [Referencia de la API](#8-referencia-de-la-api)
9. [Arquitectura](#9-arquitectura)
10. [Ciclo de vida](#10-ciclo-de-vida)
11. [Gestión de la conexión STA](#11-gestión-de-la-conexión-sta)
12. [Punto de acceso (AP)](#12-punto-de-acceso-ap)
13. [Servidor web](#13-servidor-web)
14. [Monitor de internet](#14-monitor-de-internet)
15. [Modelo de concurrencia](#15-modelo-de-concurrencia)
16. [Gestión de errores](#16-gestión-de-errores)
17. [Recursos del sistema](#17-recursos-del-sistema)
18. [Limitaciones y consideraciones](#18-limitaciones-y-consideraciones)
19. [Seguridad](#19-seguridad)
20. [Solución de problemas](#20-solución-de-problemas)
21. [Extensiones previstas](#21-extensiones-previstas)

---

## 1. Características

- Modo **AP + STA** simultáneo: red propia siempre disponible y conexión opcional a un router.
- Modo **solo AP** cuando no se define un router.
- **Reconexión automática indefinida** con espera exponencial (*backoff*) configurable.
- **Servidor web** con página principal configurable y rutas `GET`/`POST` definidas por la aplicación.
- **Monitor de internet** en tarea propia, con notificación por cambio de estado.
- **Callbacks opcionales** para conexión, desconexión, fallo de conexión, clientes del AP y acceso a internet.
- **Contexto de aplicación** (`ctx`) entregado a todos los callbacks.
- **Validación de la configuración** antes de inicializar el hardware.
- **Liberación completa de recursos** ante fallos de inicialización y mediante `easywifi_deinit()`.
- **No bloqueante**: la inicialización retorna de inmediato y no ocupa la tarea de la aplicación.

---

## 2. Requisitos

| Elemento | Requisito |
|---|---|
| Framework | ESP-IDF 5.x o superior (desarrollada sobre 6.1) |
| Chip | ESP32 y variantes con Wi-Fi 2.4 GHz |
| Lenguaje | C (C99 o superior) |
| Banda | 2.4 GHz |

Componentes de ESP-IDF utilizados internamente (declarados como dependencias privadas):

`esp_wifi`, `esp_hw_support`, `esp_netif`, `esp_event`, `esp_timer`, `nvs_flash`, `esp_http_client`, `esp_http_server`.

---

## 3. Instalación

### 3.1 Estructura del proyecto

```
proyecto/
├── CMakeLists.txt
├── main/
│   ├── CMakeLists.txt
│   └── main.c
└── components/
    └── easywifi_lib/
        ├── CMakeLists.txt
        ├── README.md
        ├── easywifi_lib.c
        └── include/
            └── easywifi_lib.h
```

ESP-IDF detecta automáticamente los componentes ubicados en `components/`.

### 3.2 CMakeLists.txt de la raíz

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Opcional: compila solo los componentes requeridos por main
set(COMPONENTS main)

project(nombre_proyecto)
```

### 3.3 CMakeLists.txt de `main`

```cmake
idf_component_register(
  SRCS         "main.c"
  INCLUDE_DIRS "."
  PRIV_REQUIRES
    easywifi_lib
    esp_driver_gpio      # solo si la aplicación usa GPIO (en IDF < 5.3: "driver")
)
```

La aplicación únicamente declara `easywifi_lib`. Las dependencias de red quedan encapsuladas en el componente.

### 3.4 CMakeLists.txt del componente

```cmake
idf_component_register(
  SRCS         "easywifi_lib.c"
  INCLUDE_DIRS "include"
  PRIV_REQUIRES
    esp_wifi
    esp_hw_support
    esp_netif
    esp_event
    esp_timer
    nvs_flash
    esp_http_client
    esp_http_server
)
```

Tras agregar o modificar componentes se recomienda ejecutar **Full Clean** antes de compilar.

---

## 4. Inicio rápido

```c
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "easywifi_lib.h"

typedef struct {
  gpio_num_t led;
  bool       led_on;
} App_t;

static App_t app = { .led = GPIO_NUM_2 };

static const char *route_led(const char *params, void *ctx) {
  App_t *a = (App_t *)ctx;
  char state[8];
  if(easywifi_query_get(params, "state", state, sizeof(state))) {
    a->led_on = (strcmp(state, "on") == 0);
    gpio_set_level(a->led, a->led_on);
  }
  return a->led_on ? "ON" : "OFF";
}

static const easywifi_route_t routes[] = {
  EASYWIFI_ROUTE("/led", route_led),
};

void app_main(void) {

  gpio_reset_pin(app.led);
  gpio_set_direction(app.led, GPIO_MODE_OUTPUT);

  easywifi_config_t cfg = {
    .STA_SSID              = "MiRouter",
    .STA_PASS              = "clave_router",
    .STA_INTERNET_CHECK_MS = 30000,
    .AP_SSID               = "ESP32",
    .AP_PASS               = "12345678",
    EASYWIFI_ROUTES(routes),
    .ctx                   = &app,
  };

  ESP_ERROR_CHECK(easywifi_init(&cfg));

  while(1) {
    // Lógica de la aplicación
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
```

Acceso a la página:

- Desde la red propia: conectarse a `ESP32` y abrir `http://192.168.4.1`.
- Desde la red del router: abrir la IP asignada, visible en el log (`Connected to the router. IP: ...`).
- Control directo: `http://<ip>/led?state=on`.

---

## 5. Convención de nombres

Todos los símbolos públicos comparten el prefijo `easywifi`, a modo de espacio de nombres.

| Categoría | Formato | Ejemplos |
|---|---|---|
| Funciones | `easywifi_<acción>()` | `easywifi_init()`, `easywifi_has_internet()` |
| Tipos | `easywifi_<nombre>_t` | `easywifi_config_t`, `easywifi_route_t` |
| Tipos de callback | `easywifi_<nombre>_cb_t` | `easywifi_conn_cb_t`, `easywifi_route_cb_t` |
| Macros y constantes | `EASYWIFI_<NOMBRE>` | `EASYWIFI_ROUTE()`, `EASYWIFI_GET` |
| Componente, archivos y log | `easywifi_lib` | `easywifi_lib.h`, etiqueta de log `easywifi_lib` |

Los campos de configuración de red se escriben en mayúsculas (`STA_SSID`, `AP_PASS`, ...) para distinguirlos visualmente de los callbacks y punteros de la misma estructura.

---

## 6. Referencia de configuración

Tipo: `easywifi_config_t`. Los campos no definidos quedan en cero y adoptan su valor por defecto.

### 6.1 Parámetros de red

| Campo | Tipo | Descripción | Valor por defecto / vacío |
|---|---|---|---|
| `STA_SSID` | `char[33]` | SSID del router | Vacío: modo solo AP |
| `STA_PASS` | `char[65]` | Clave del router (8–64 caracteres) | Vacío: red abierta |
| `STA_MAX_RETRIES` | `uint8_t` | Fallos consecutivos antes de invocar `on_connect_fail` | `5` |
| `STA_RETRY_MAX_MS` | `uint32_t` | Espera máxima entre reintentos (ms) | `60000` |
| `STA_INTERNET_CHECK_MS` | `uint32_t` | Intervalo del monitor de internet (ms). Requiere conexión STA | `0`: monitor desactivado |
| `AP_SSID` | `char[33]` | SSID de la red propia (1–32 caracteres) | **Obligatorio** |
| `AP_PASS` | `char[65]` | Clave de la red propia (8–64 caracteres) | Vacío: AP abierto |
| `AP_CHANNEL` | `uint8_t` | Canal del AP (1–13). Solo aplica sin conexión STA (ver [12](#12-punto-de-acceso-ap)) | `1` |
| `AP_MAX_CONN` | `uint8_t` | Clientes simultáneos en el AP | `4` |

### 6.2 Callbacks

Todos son opcionales. Un valor `NULL` indica que la aplicación no requiere la notificación.

| Campo | Tipo | Firma | Se invoca cuando |
|---|---|---|---|
| `on_connected` | `easywifi_conn_cb_t` | `void (const char *ip, void *ctx)` | Se obtiene IP del router |
| `on_disconnected` | `easywifi_event_cb_t` | `void (void *ctx)` | Se pierde una conexión establecida con el router |
| `on_connect_fail` | `easywifi_event_cb_t` | `void (void *ctx)` | Se alcanzan `STA_MAX_RETRIES` fallos consecutivos |
| `on_ap_client` | `easywifi_client_cb_t` | `void (const uint8_t mac[6], bool joined, void *ctx)` | Un dispositivo entra (`true`) o sale (`false`) del AP |
| `on_internet` | `easywifi_net_cb_t` | `void (bool ok, void *ctx)` | Cambia el acceso a internet; también con el primer resultado |

### 6.3 Contenido web y contexto

| Campo | Tipo | Descripción |
|---|---|---|
| `routes` / `n_routes` | `const easywifi_route_t *` / `uint8_t` | Rutas de la aplicación. Se asignan con `EASYWIFI_ROUTES(arr)` |
| `index_html` | `const char *` | Página servida en `GET /`. `NULL`: página por defecto |
| `ctx` | `void *` | Puntero al estado de la aplicación, entregado a todos los callbacks y rutas |

### 6.4 Tiempo de vida de los datos

La librería almacena una **copia superficial** de la configuración. La estructura `easywifi_config_t` puede ser local, pero los datos referenciados por puntero deben existir durante toda la ejecución:

| Dato | Requisito |
|---|---|
| Arreglo de rutas | `static const` o global |
| `index_html` | `static const` o literal |
| Objeto apuntado por `ctx` | `static` o global |
| Texto devuelto por una ruta | Literal o buffer `static` |

### 6.5 Constantes de compilación

| Macro | Valor | Descripción |
|---|---|---|
| `EASYWIFI_MAX_PARAMS_LEN` | `256` | Longitud máxima del *query string* (GET) o del cuerpo (POST) |

El valor se define en `easywifi_lib.h` y se modifica editando ese archivo. Determina el tamaño de un buffer local en la tarea del servidor HTTP, por lo que valores elevados incrementan el uso de su pila.

---

## 7. Rutas web

### 7.1 Tipo de ruta

```c
typedef struct {
  const char          *uri;           // Debe comenzar con '/'
  easywifi_route_cb_t  handler;       // Función de la aplicación
  easywifi_method_t    method;        // EASYWIFI_GET (defecto) | EASYWIFI_POST
  const char          *content_type;  // NULL: "text/plain"
} easywifi_route_t;
```

### 7.2 Firma del handler

```c
typedef const char *(*easywifi_route_cb_t)(const char *params, void *ctx);
```

| Parámetro | GET | POST |
|---|---|---|
| `params` | *Query string* (`state=on&x=1`) | Cuerpo de la petición |
| `ctx` | `ctx` de la configuración | `ctx` de la configuración |

Si no hay parámetros, `params` es una cadena vacía (`""`), nunca `NULL`.

**Retorno:** texto de la respuesta. `NULL` produce una respuesta vacía. El texto se envía después de que el handler retorna, por lo que debe ser un literal o un buffer `static`.

### 7.3 Macros de definición

```c
EASYWIFI_ROUTE(uri, handler)
EASYWIFI_ROUTE_EX(uri, handler, method, content_type)
EASYWIFI_ROUTES(arreglo)          // dentro de la configuración
```

Las macros emplean inicializadores designados, lo que evita el error `-Werror=missing-field-initializers` habilitado por defecto en ESP-IDF. `EASYWIFI_ROUTES` calcula `n_routes` con `sizeof` en el punto de uso, donde el tamaño del arreglo es conocido.

### 7.4 Ejemplo

```c
static const char *route_status(const char *params, void *ctx) {
  static char json[64];
  snprintf(json, sizeof(json), "{\"internet\":%s}",
           easywifi_has_internet() ? "true" : "false");
  return json;
}

static const char *route_config(const char *params, void *ctx) {
  char value[16];
  if(easywifi_query_get(params, "speed", value, sizeof(value))) {
    // aplicar valor...
  }
  return "OK";
}

static const easywifi_route_t routes[] = {
  EASYWIFI_ROUTE("/led", route_led),
  EASYWIFI_ROUTE_EX("/status", route_status, EASYWIFI_GET,  "application/json"),
  EASYWIFI_ROUTE_EX("/config", route_config, EASYWIFI_POST, "text/plain"),
};
```

Petición POST de formulario compatible con `easywifi_query_get`:

```javascript
fetch('/config', {
  method: 'POST',
  headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
  body: 'speed=50'
});
```

### 7.5 Reglas de validación

`easywifi_init()` devuelve `ESP_ERR_INVALID_ARG` si:

- Una ruta no comienza con `/` o su `uri` es `NULL`.
- Una ruta no tiene `handler`.
- El método no es `EASYWIFI_GET` ni `EASYWIFI_POST`.
- Se define `GET /` (reservada para `index_html`).
- `n_routes > 0` y `routes == NULL`.

Una ruta duplicada (mismo `uri` y método) provoca un error de registro y la inicialización falla.

---

## 8. Referencia de la API

### `esp_err_t easywifi_init(const easywifi_config_t *config)`

Valida la configuración e inicia NVS, Wi-Fi, servidor web y monitor de internet. No bloquea.

| Retorno | Causa |
|---|---|
| `ESP_OK` | Inicialización correcta |
| `ESP_ERR_INVALID_ARG` | Configuración inválida (detalle en el log) |
| `ESP_ERR_INVALID_STATE` | La librería ya está inicializada |
| `ESP_ERR_NO_MEM` | Memoria insuficiente |
| Otros `esp_err_t` | Error propagado desde ESP-IDF |

Ante cualquier error, los recursos creados se liberan y la función puede invocarse de nuevo.

### `esp_err_t easywifi_deinit(void)`

Detiene todos los servicios y libera los recursos de la librería. Tras su ejecución puede llamarse nuevamente `easywifi_init()`.

| Retorno | Causa |
|---|---|
| `ESP_OK` | Liberación correcta |
| `ESP_ERR_INVALID_STATE` | La librería no estaba inicializada |

Puede bloquear hasta que finalice una verificación de internet en curso (máximo ~4 s más la resolución DNS).

### `bool easywifi_wait_connected(uint32_t timeout_ms)`

Bloquea la tarea llamadora hasta que ocurra una de estas condiciones:

| Condición | Retorno |
|---|---|
| Conexión al router | `true` |
| `STA_MAX_RETRIES` fallos consecutivos | `false` |
| Modo solo AP | `false` (inmediato) |
| Vence `timeout_ms` | `false` |

El retorno `false` no detiene los reintentos en segundo plano.

### `bool easywifi_is_connected(void)`

Indica si existe conexión con el router en este momento. Instantánea.

### `bool easywifi_has_internet(void)`

Devuelve el último resultado del monitor de internet. Instantánea. Siempre `false` si el monitor está desactivado.

### `bool easywifi_internet_ok(void)`

Realiza una verificación HTTP inmediata. Bloquea hasta ~4 s. No modifica el estado del monitor. No debe invocarse desde callbacks.

### `esp_err_t easywifi_reconnect(void)`

Reinicia el contador de reintentos, cancela la espera pendiente e intenta conectar de inmediato.

| Retorno | Causa |
|---|---|
| `ESP_OK` | Intento lanzado, o ya conectado |
| `ESP_ERR_INVALID_STATE` | No inicializada o modo solo AP |

### `bool easywifi_query_get(const char *params, const char *key, char *out, size_t out_len)`

Extrae el valor de `key` de una cadena con formato `clave=valor&clave2=valor2`. Aplica tanto a *query strings* como a cuerpos POST de formulario. Devuelve `false` si la clave no existe o los argumentos son inválidos.

### Resumen

| Función | Bloquea | Uso desde callbacks |
|---|---|---|
| `easywifi_init()` | No | No |
| `easywifi_deinit()` | Hasta ~4 s | No |
| `easywifi_wait_connected()` | Hasta `timeout_ms` | No |
| `easywifi_is_connected()` | No | Sí |
| `easywifi_has_internet()` | No | Sí |
| `easywifi_internet_ok()` | Hasta ~4 s | No |
| `easywifi_reconnect()` | No | Sí |
| `easywifi_query_get()` | No | Sí |

---

## 9. Arquitectura

### 9.1 Separación de responsabilidades

| Librería (mecanismo) | Aplicación (lógica) |
|---|---|
| Inicialización y configuración del radio | Acciones ante conexión o desconexión |
| Conexión y reconexión al router | Respuesta de cada ruta web |
| Punto de acceso y DHCP | Contenido de la página principal |
| Servidor HTTP y despacho de rutas | Estado de la aplicación (`ctx`) |
| Verificación de internet | Lógica propia en `app_main` y tareas |
| Liberación de recursos | Decisión ante errores |

### 9.2 Diagrama de componentes

```
┌──────────────────────────────── ESP-IDF ───────────────────────────────┐
│   wifi (radio)      tcpip (lwIP)      sys_evt (bucle de eventos)       │
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │ eventos WIFI_EVENT / IP_EVENT
┌──────────────────────────── easywifi_lib ──────────────────────────────┐
│  wifi_event_handler ───────────────► callbacks de conexión / AP        │
│        │                                                               │
│        ├──► esp_timer "easywifi_retry" ──► esp_wifi_connect()          │
│        └──► notificación ──► tarea "easywifi_net" ──► on_internet      │
│                                                                        │
│  tarea "httpd" ──► root_handler / generic_route_handler ──► rutas      │
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │ callbacks + ctx
┌────────────────────────────── Aplicación ──────────────────────────────┐
│  app_main: easywifi_init() + bucle propio                              │
│  App_t: estado compartido                                              │
└────────────────────────────────────────────────────────────────────────┘
```

### 9.3 Organización del código fuente

| Sección en `easywifi_lib.c` | Contenido |
|---|---|
| 0. Configuration checks | `validate_config()` |
| 1. Reconnection with backoff | `backoff_ms()`, `retry_timer_cb()`, `schedule_reconnect()` |
| 2. Events → callbacks | `wifi_event_handler()`, `notify_monitor()` |
| 3. Wi-Fi initialization | `nvs_init()`, `wifi_start()` |
| 4. Web server | `root_handler()`, `read_body()`, `generic_route_handler()`, `start_webserver()` |
| 5. Internet monitor | `internet_probe()`, `set_internet_state()`, `internet_monitor_task()`, `start_internet_monitor()` |
| 6. Resource release | `release_resources()` |
| 7. Public API | Funciones `easywifi_*` |

Todas las funciones y variables internas son `static`. El header público solo depende de `esp_err.h`.

### 9.4 Estado interno

**Recursos** (valor `NULL`/`false` indica que no han sido creados):

| Variable | Recurso |
|---|---|
| `s_events` | Event group de sincronización |
| `s_netif_sta`, `s_netif_ap` | Interfaces de red |
| `s_wifi_handler`, `s_ip_handler` | Registros de manejadores de eventos |
| `s_retry_timer` | Temporizador de reconexión |
| `s_server` | Servidor HTTP |
| `s_net_task` | Tarea del monitor |
| `s_wifi_inited`, `s_wifi_started` | Estado del driver |

**Servicios globales** (se inicializan una sola vez y no se liberan): `s_nvs_ready`, `s_netif_ready`.

**Bits del event group:**

| Bit | Significado |
|---|---|
| `WIFI_CONNECTED_BIT` | Conexión activa con el router |
| `WIFI_FAIL_BIT` | `STA_MAX_RETRIES` fallos consecutivos, o modo solo AP |
| `NET_TASK_DONE_BIT` | La tarea del monitor ha finalizado |

**Variables de estado:** `s_initialized`, `s_stopping`, `s_has_internet`, `s_attempts`.

---

## 10. Ciclo de vida

### 10.1 Inicialización

```
easywifi_init(config)
 ├─ 1. validate_config()          Verificación completa, sin tocar hardware
 ├─ 2. s_cfg = *config            Copia superficial
 ├─ 3. nvs_init()                 NVS (requerido por el driver Wi-Fi)
 ├─ 4. wifi_start()
 │     ├─ event group
 │     ├─ esp_netif_init (una vez)
 │     ├─ bucle de eventos por defecto (tolera que ya exista)
 │     ├─ netif STA (si aplica) y AP
 │     ├─ esp_wifi_init + almacenamiento en RAM
 │     ├─ registro de manejadores de eventos
 │     ├─ temporizador de reconexión (si aplica)
 │     └─ modo, configuración y esp_wifi_start
 ├─ 5. start_webserver()          Servidor, "/" y rutas
 ├─ 6. start_internet_monitor()   Tarea "easywifi_net" (si aplica)
 └─ 7. s_initialized = true
```

### 10.2 Fallo durante la inicialización

Cada paso utiliza `ESP_GOTO_ON_ERROR`. Ante un error:

1. Se registra el error en el log.
2. Se ejecuta `release_resources()`, que libera únicamente los recursos ya creados.
3. Se devuelve el código de error.

El sistema queda en el mismo estado que antes de la llamada.

### 10.3 Liberación de recursos

`release_resources()` es utilizada por `easywifi_deinit()` y por el camino de error de `easywifi_init()`. Orden de liberación:

1. `s_stopping = true`: callbacks internos y temporizador dejan de actuar.
2. Notificación a la tarea del monitor y espera de `NET_TASK_DONE_BIT`. La tarea finaliza por sí misma; no se elimina de forma forzada.
3. Detención del servidor HTTP.
4. Desregistro de los manejadores de eventos.
5. Detención y eliminación del temporizador.
6. `esp_wifi_stop()` y `esp_wifi_deinit()`.
7. Destrucción de las interfaces de red.
8. Eliminación del event group.

No se liberan NVS, `esp_netif` ni el bucle de eventos por defecto, por tratarse de servicios globales que pueden estar en uso por otros componentes.

---

## 11. Gestión de la conexión STA

### 11.1 Secuencia de eventos

```
esp_wifi_start()
   └─► WIFI_EVENT_STA_START ─────────► esp_wifi_connect()
                                            │
              ┌─────────────────────────────┴──────────────────────────┐
              ▼                                                        ▼
   IP_EVENT_STA_GOT_IP                                 WIFI_EVENT_STA_DISCONNECTED
   ├─ s_attempts = 0                                   ├─ Si había conexión:
   ├─ detiene temporizador                             │   ├─ notifica al monitor
   ├─ limpia FAIL, activa CONNECTED                    │   └─ on_disconnected()
   ├─ on_connected(ip)                                 └─ schedule_reconnect()
   └─ notifica al monitor                                        │
                                                                 ▼
                                               esp_timer (backoff) ──► esp_wifi_connect()
```

`WIFI_EVENT_STA_DISCONNECTED` se emite tanto al perder una conexión establecida como al fallar cada intento. `on_disconnected` solo se invoca en el primer caso.

### 11.2 Backoff exponencial

```
espera(n) = min(1000 ms × 2^(n-1), STA_RETRY_MAX_MS)
```

| Intento | 1 | 2 | 3 | 4 | 5 | 6 | 7+ |
|---|---|---|---|---|---|---|---|
| Espera (máx. 60 s) | 1 s | 2 s | 4 s | 8 s | 16 s | 32 s | 60 s |

Los reintentos son indefinidos. La espera se implementa con `esp_timer` para que el manejador de eventos, que se ejecuta en la tarea de eventos del sistema, nunca se bloquee.

### 11.3 Notificación de fallo

Al alcanzar `s_attempts == STA_MAX_RETRIES`:

1. Se activa `WIFI_FAIL_BIT` (libera a `easywifi_wait_connected()`).
2. Se invoca `on_connect_fail` una sola vez por racha de fallos.
3. Los reintentos continúan.

Tras una conexión exitosa el contador se reinicia, por lo que la notificación puede repetirse en una desconexión posterior.

---

## 12. Punto de acceso (AP)

| Aspecto | Comportamiento |
|---|---|
| Disponibilidad | Siempre activo mientras la librería esté inicializada |
| Dirección | `192.168.4.1` |
| DHCP | Incluido, asigna direcciones a los clientes |
| Autenticación | WPA2-PSK con `AP_PASS`, o abierta si está vacía |
| Modo | `WIFI_MODE_APSTA` con router; `WIFI_MODE_AP` sin router |
| Almacenamiento | `WIFI_STORAGE_RAM`: la configuración no se escribe en flash |
| Acceso a internet de los clientes | No se proporciona: el ESP32 no reenvía tráfico entre AP y STA (sin NAT). Los clientes del AP solo acceden a la página local |
| Canal efectivo | `AP_CHANNEL` se aplica solo sin conexión STA; al conectarse, el AP adopta el canal del router |

En modo solo AP no se crean la interfaz STA, el temporizador ni el monitor de internet, y `WIFI_FAIL_BIT` se activa de inmediato.

---

## 13. Servidor web

### 13.1 Despacho de rutas

Todas las rutas de la aplicación se registran con el mismo manejador interno (`generic_route_handler`). La identidad de cada ruta se transmite mediante `user_ctx`:

```c
httpd_uri_t u = {
  .uri      = r->uri,
  .method   = HTTP_GET | HTTP_POST,
  .handler  = generic_route_handler,
  .user_ctx = (void *)r,             // puntero al easywifi_route_t
};
```

Procesamiento de una petición:

1. Se obtiene la ruta desde `req->user_ctx`.
2. Se leen los parámetros: *query string* (GET) o cuerpo completo (POST).
3. Se invoca `route->handler(params, ctx)`.
4. Se establece `Content-Type` según `route->content_type` (por defecto `text/plain`).
5. Se envía la respuesta.

### 13.2 Configuración del servidor

| Parámetro | Valor |
|---|---|
| Puerto | 80 |
| `max_uri_handlers` | `n_routes + 1` si supera el valor por defecto (8) |
| `lru_purge_enable` | `true`: cierra los sockets más antiguos si se agotan |

### 13.3 Respuestas de error

| Situación | Código HTTP |
|---|---|
| *Query string* mayor que `EASYWIFI_MAX_PARAMS_LEN` | 414 URI Too Long |
| Cuerpo POST mayor que `EASYWIFI_MAX_PARAMS_LEN` | 400 Bad Request |
| Ruta inexistente | 404 (gestionado por `esp_http_server`) |

### 13.4 Ejecución

El servidor atiende las peticiones de forma secuencial en una única tarea. En consecuencia:

- Un buffer `static` de respuesta no presenta conflictos entre peticiones.
- Un handler lento retrasa a todos los clientes.

---

## 14. Monitor de internet

### 14.1 Criterio de verificación

La conexión al router no garantiza salida a internet. La verificación se realiza mediante:

```
GET http://connectivitycheck.gstatic.com/generate_204
```

Solo una respuesta **HTTP 204** se considera acceso válido. Un portal cautivo devuelve otro código (habitualmente 200 o una redirección), y la ausencia de salida produce un error o un *timeout* (4 s).

### 14.2 Funcionamiento de la tarea

| Parámetro | Valor |
|---|---|
| Nombre | `easywifi_net` |
| Pila | 4096 bytes |
| Prioridad | 3 |
| Núcleo | 0 |

```
bucle:
  esperar notificación o STA_INTERNET_CHECK_MS
  si se solicita detener          → salir
  si no hay router                → estado = NO; esperar indefinidamente
  verificar internet (HTTP)
  estado = resultado (si sigue conectado)
  esperar STA_INTERNET_CHECK_MS
al salir: activar NET_TASK_DONE_BIT y eliminarse
```

| Evento | Efecto sobre la tarea |
|---|---|
| Sin router | Bloqueada indefinidamente (sin consumo de CPU) |
| `IP_EVENT_STA_GOT_IP` | Despertada; verificación inmediata |
| `WIFI_EVENT_STA_DISCONNECTED` | Despertada; reporta estado `NO` |
| Intervalo cumplido | Verificación periódica |
| `easywifi_deinit()` | Despertada; finaliza |

### 14.3 Escritor único

La tarea del monitor es el único escritor de `s_has_internet` y el único invocador de `on_internet`. El resto de componentes solo la notifican. Esto garantiza el orden de las notificaciones y elimina condiciones de carrera sobre el estado.

`on_internet` se invoca:

- Con el primer resultado obtenido tras la inicialización.
- En cada cambio de estado posterior.

### 14.4 Activación

El monitor se crea solo si `STA_INTERNET_CHECK_MS > 0` y `STA_SSID` no está vacío.

---

## 15. Modelo de concurrencia

### 15.1 Contexto de ejecución

| Código | Tarea | Restricción |
|---|---|---|
| `on_connected`, `on_disconnected`, `on_connect_fail`, `on_ap_client` | Eventos de ESP-IDF (`sys_evt`) | No bloquear; pila reducida |
| `on_internet` | `easywifi_net` | No bloquear |
| Handlers de rutas | `httpd` | No bloquear |
| `retry_timer_cb` (interno) | `esp_timer` | No bloquear |
| `app_main` y tareas de la aplicación | Propias | Sin restricción |

### 15.2 Reglas

1. **Los callbacks no deben bloquear.** No deben usar `vTaskDelay`, peticiones de red, esperas sobre colas o semáforos, ni operaciones prolongadas. El trabajo pesado se delega a una tarea de la aplicación mediante colas, notificaciones o *event groups*.
2. **Datos compartidos.** Los callbacks se ejecutan en tareas distintas a `app_main`. La lectura o escritura de un `bool` o `int` individual es atómica en el ESP32. Las actualizaciones de varios campos que deben mantenerse coherentes requieren un mutex (`xSemaphoreCreateMutex`).
3. **Inicialización y liberación.** `easywifi_init()` y `easywifi_deinit()` deben invocarse desde la misma tarea y nunca desde un callback de la librería.
4. **Funciones bloqueantes.** `easywifi_internet_ok()` y `easywifi_wait_connected()` no deben llamarse desde callbacks.

### 15.3 Integración con otros periféricos

| Tipo de trabajo | Ubicación recomendada |
|---|---|
| Periódico y lento | Bucle de `app_main` |
| Periódico y rápido (ADC, control) | Tarea propia, preferentemente en el núcleo 1 |
| Temporización precisa | `esp_timer` |
| Entradas digitales | ISR → cola → tarea |
| Interacción web | Handler de ruta que lee o escribe el estado de la aplicación |

---

## 16. Gestión de errores

La librería informa y la aplicación decide.

| Ámbito | Mecanismo | Efecto |
|---|---|---|
| Interno | `ESP_RETURN_ON_ERROR`, `ESP_RETURN_ON_FALSE`, `ESP_GOTO_ON_ERROR` | Registro en log y propagación del error |
| Aplicación | `ESP_ERROR_CHECK` | Reinicio del chip ante error |
| Aplicación | Evaluación del `esp_err_t` | Tratamiento personalizado |

La librería no invoca `ESP_ERROR_CHECK` ni `abort()`.

Ejemplo de tratamiento no fatal:

```c
esp_err_t err = easywifi_init(&cfg);
if(err != ESP_OK) {
  ESP_LOGE(TAG, "Wi-Fi no disponible: %s", esp_err_to_name(err));
  // Continuar sin red, indicar error, reintentar más tarde...
}
```

Los mensajes de la librería se emiten con la etiqueta de log `easywifi_lib`, lo que permite filtrarlos o ajustar su nivel:

```c
esp_log_level_set("easywifi_lib", ESP_LOG_WARN);
```

---

## 17. Recursos del sistema

| Recurso | Cantidad | Observación |
|---|---|---|
| Tareas propias | 1 (`easywifi_net`, 4 KB) | Solo si el monitor está activo |
| Tareas de ESP-IDF | `wifi`, `tcpip`, `sys_evt`, `httpd`, `esp_timer` | Creadas por los componentes del sistema |
| Temporizadores | 1 (`easywifi_retry`) | Solo con router configurado |
| Event groups | 1 | |
| Interfaces de red | 1–2 | AP, y STA si aplica |
| Pila del handler HTTP | `EASYWIFI_MAX_PARAMS_LEN` bytes por petición | Buffer local en la tarea `httpd` |
| RAM total | Decenas de KB | Dominada por el stack Wi-Fi y lwIP |
| Escrituras en flash | Ninguna por configuración Wi-Fi | `WIFI_STORAGE_RAM` |

---

## 18. Limitaciones y consideraciones

### 18.1 Hardware

- **Banda de 2.4 GHz únicamente.**
- **Canal compartido.** El ESP32 dispone de un único radio; en modo APSTA el AP adopta el canal del router al conectarse, lo que puede provocar una desconexión momentánea de los clientes del AP.
- **ADC2 inutilizable con Wi-Fi activo** (ESP32 clásico). Deben emplearse canales de ADC1 (GPIO 32–39).
- **Ruido en lecturas analógicas** durante la transmisión. Se recomienda promediar muestras.

### 18.2 Diseño

- **Instancia única.** El estado es estático; solo puede existir una instancia activa.
- **Copia superficial de la configuración** (ver [6.4](#64-tiempo-de-vida-de-los-datos)).
- **Rutas síncronas.** Un handler lento bloquea el servidor.
- **Dependencia externa del monitor.** Si `connectivitycheck.gstatic.com` está bloqueado en la red, el monitor reporta ausencia de internet.
- **`easywifi_internet_ok()` y el monitor** pueden ejecutar verificaciones simultáneas; son independientes y no comparten estado.

---

## 19. Seguridad

La librería está orientada a desarrollo, prototipado y redes de confianza. Antes de un despliegue en producción deben considerarse los siguientes puntos:

| Aspecto | Situación actual | Riesgo |
|---|---|---|
| Transporte | HTTP sin cifrar | Peticiones visibles en la red |
| Autenticación | Inexistente | Cualquier cliente con acceso a la red puede invocar las rutas |
| Credenciales | Definidas en el código fuente | Extraíbles del binario en flash |
| AP abierto (`AP_PASS` vacío) | Permitido | Acceso sin restricción a la red propia |

Las rutas que actúan sobre cargas de potencia o elementos críticos no deben exponerse sin autenticación.

---

## 20. Solución de problemas

| Síntoma | Causa | Solución |
|---|---|---|
| `expected ')' before 'MACSTR'` | Falta `esp_mac.h` (IDF ≥ 6) | El componente ya lo incluye; verificar que se compila la versión actual |
| `missing initializer for field 'method'` | Inicialización posicional de `easywifi_route_t` | Usar `EASYWIFI_ROUTE()` / `EASYWIFI_ROUTE_EX()` o inicializadores designados |
| `undefined reference to 'easywifi_*'` | Componente no declarado, o nombres distintos entre `.h` y `.c` | Añadir `easywifi_lib` a `PRIV_REQUIRES` de `main`; verificar los nombres de las funciones |
| `easywifi_lib.h: No such file` | Componente no detectado | Verificar la ruta `components/easywifi_lib/include/` y ejecutar Full Clean |
| `invalid configuration` en el log | Configuración rechazada | Leer el mensaje anterior en el log, que indica el campo concreto |
| `AP_PASS must be ... 8-64 characters` | Clave del AP demasiado corta | Usar al menos 8 caracteres o dejarla vacía |
| `could not register route ... (duplicated?)` | Dos rutas con el mismo `uri` y método | Eliminar el duplicado |
| Reintentos continuos sin conexión | SSID o clave incorrectos, red de 5 GHz o fuera de alcance | Verificar credenciales y banda; revisar `reason` en el log de desconexión |
| `Internet: NO` estando conectado | Router sin salida, portal cautivo o bloqueo del dominio de verificación | Comprobar la salida a internet desde otro dispositivo de la misma red |
| `task_wdt: Task watchdog got triggered` | Bucle sin ceder la CPU | Incluir `vTaskDelay` o una espera bloqueante en todo `while(1)` |
| Respuesta de ruta con texto corrupto | El handler devuelve un buffer local | Devolver un literal o un buffer `static` |
| Caracteres extraños (`├│`) en el monitor serie | Codificación de la consola de Windows | Usar `chcp 65001` o evitar tildes en los logs |

---

## 21. Extensiones previstas

| Extensión | Descripción | Impacto en la API |
|---|---|---|
| Credenciales en NVS | Almacenar SSID y clave; configuración desde una página del AP | Nuevos campos opcionales y funciones de aprovisionamiento |
| mDNS | Acceso mediante `http://<nombre>.local` | Campo `HOSTNAME` |
| Autenticación | HTTP Basic Auth por ruta | Campo opcional en `easywifi_route_t` |
| WebSocket | Envío de datos en tiempo real desde el ESP32 | Nuevo tipo de ruta y función de publicación |
| HTTPS / MQTT con TLS | Comunicación cifrada con servicios externos | Módulo adicional |

Las extensiones previstas se incorporan como campos opcionales o funciones adicionales, sin alterar el contrato existente.
