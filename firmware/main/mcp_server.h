#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers a Model Context Protocol server at POST /mcp on an already-started
// server, so an AI assistant can query the HEM's topology, logged power,
// forecasts, schedule and diagnostics as tools.
//
// Transport is MCP "Streamable HTTP" in its simplest form: each JSON-RPC request
// is one POST answered with one application/json body. There are no sessions and
// no server-initiated messages, so GET /mcp answers 405.
//
// Like the rest of the HTTP API there is no authentication; it is meant for the
// local network only. Requests carrying an Origin that does not match the Host
// are refused, which stops a web page elsewhere from driving it.
//
// Call this from web_server_start() before the "/*" catch-all is registered.
// It costs two slots in httpd_config_t::max_uri_handlers.
esp_err_t mcp_server_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
