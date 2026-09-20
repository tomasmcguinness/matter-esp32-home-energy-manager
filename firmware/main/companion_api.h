#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers the companion app's REST surface on an already-started server.
//
// This is the contract the MCC iOS app (Matter Controller Companion) expects of any
// self-hosted Matter controller. It is documented in firmware/COMPANION_API.md.
//
// Call this from web_server_start() before the "/*" catch-all is registered, or the
// static file handler swallows the GETs. Each route costs a slot in
// httpd_config_t::max_uri_handlers.
esp_err_t companion_api_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
