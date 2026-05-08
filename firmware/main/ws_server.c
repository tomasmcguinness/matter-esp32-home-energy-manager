#include "ws_server.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"

static const char *TAG = "ws_server";

static httpd_handle_t s_server = NULL;

// ── Async send ────────────────────────────────────────────────────────────────

typedef struct {
    httpd_handle_t hd;
    int            fd;
    char          *payload;
    size_t         len;
} ws_send_arg_t;

static void ws_async_send(void *arg)
{
    ws_send_arg_t *a = (ws_send_arg_t *)arg;
    httpd_ws_frame_t pkt = {
        .final      = true,
        .fragmented = false,
        .type       = HTTPD_WS_TYPE_TEXT,
        .payload    = (uint8_t *)a->payload,
        .len        = a->len,
    };
    esp_err_t err = httpd_ws_send_frame_async(a->hd, a->fd, &pkt);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "send to fd=%d failed: 0x%x", a->fd, err);
    }
    free(a->payload);
    free(a);
}

// ── WebSocket handler ─────────────────────────────────────────────────────────

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Client connected, fd=%d", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    // Drain any incoming frame (clients may send pings or text)
    httpd_ws_frame_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK) return ret;

    if (pkt.len > 0 && pkt.len <= 4096) {
        pkt.payload = malloc(pkt.len + 1);
        if (!pkt.payload) return ESP_ERR_NO_MEM;
        ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
        free(pkt.payload);
        if (ret != ESP_OK) return ret;
    }
    return ESP_OK;
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t ws_server_init(httpd_handle_t server)
{
    s_server = server;

    static const httpd_uri_t ws_uri = {
        .uri                = "/ws",
        .method             = HTTP_GET,
        .handler            = ws_handler,
        .is_websocket       = true,
        .handle_ws_control_frames = false,
    };
    return httpd_register_uri_handler(server, &ws_uri);
}

esp_err_t ws_server_broadcast(const char *json, size_t len)
{
    if (!s_server || !json || len == 0) return ESP_ERR_INVALID_ARG;

    size_t max_clients = CONFIG_LWIP_MAX_SOCKETS;
    int fds[CONFIG_LWIP_MAX_SOCKETS];

    esp_err_t ret = httpd_get_client_list(s_server, &max_clients, fds);
    if (ret != ESP_OK) return ret;

    for (size_t i = 0; i < max_clients; i++) {
        if (httpd_ws_get_fd_info(s_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;

        ws_send_arg_t *arg = malloc(sizeof(ws_send_arg_t));
        if (!arg) continue;

        arg->payload = malloc(len);
        if (!arg->payload) { free(arg); continue; }

        memcpy(arg->payload, json, len);
        arg->len = len;
        arg->hd  = s_server;
        arg->fd  = fds[i];

        ret = httpd_queue_work(s_server, ws_async_send, arg);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "queue_work failed for fd=%d: 0x%x", fds[i], ret);
            free(arg->payload);
            free(arg);
        }
    }
    return ESP_OK;
}
