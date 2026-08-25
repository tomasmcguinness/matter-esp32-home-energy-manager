#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ws_server_init(httpd_handle_t server);
esp_err_t ws_server_broadcast(const char *json, size_t len);

#ifdef __cplusplus
}
#endif
