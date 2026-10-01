#include "webServer.h"

#include <string.h>
#include <memory>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "lwip/inet.h"

#if __has_include(<cJSON.h>)
#include <cJSON.h>
#else
#include <cjson/cJSON.h>
#endif

#include "artnet.h"
#include "webIndex.h"   // generated from web/index.html by tools/embed_web.py

static const char *TAG = "web";

static const size_t MAX_BODY_SIZE = 256 * 1024;

WebServer::WebServer(AppConfig *config, esp_netif_t *netif)
    : config_(config), netif_(netif) {}

WebServer::~WebServer() { stop(); }

esp_err_t WebServer::registerUri(const char *uri, httpd_method_t method,
                                 esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t u = {};
    u.uri = uri;
    u.method = method;
    u.handler = handler;
    u.user_ctx = this;
    return httpd_register_uri_handler(server_, &u);
}

esp_err_t WebServer::start(uint16_t port)
{
    if (server_) return ESP_OK;

    esp_timer_create_args_t targs = {};
    targs.callback = applyNetworkTimer;
    targs.arg = this;
    targs.name = "net_apply";
    esp_err_t err = esp_timer_create(&targs, &netTimer_);
    if (err != ESP_OK) return err;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = port;
    cfg.stack_size = 8192;          // JSON parsing + std::vector/string in handlers
    cfg.max_uri_handlers = 8;
    cfg.max_open_sockets = 5;       // browsers open several connections; old ones get purged
    cfg.lru_purge_enable = true;    // drop idle connections instead of refusing new ones

    err = httpd_start(&server_, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        server_ = nullptr;
        return err;
    }

    registerUri("/", HTTP_GET, handleIndex);
    registerUri("/index.html", HTTP_GET, handleIndex);
    registerUri("/api/config", HTTP_GET, handleGetConfig);
    registerUri("/api/status", HTTP_GET, handleGetStatus);
    registerUri("/api/io", HTTP_POST, handlePostIo);
    registerUri("/api/shutters", HTTP_POST, handlePostShutters);
    registerUri("/api/network", HTTP_POST, handlePostNetwork);

    ESP_LOGI(TAG, "Web interface on port %u", port);
    return ESP_OK;
}

void WebServer::stop()
{
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
    }
    if (netTimer_) {
        esp_timer_stop(netTimer_);
        esp_timer_delete(netTimer_);
        netTimer_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

esp_err_t WebServer::readJsonBody(httpd_req_t *req, cJSON **out)
{
    *out = nullptr;
    size_t len = req->content_len;
    if (len == 0) return sendError(req, "400 Bad Request", "empty request body");
    if (len > MAX_BODY_SIZE) return sendError(req, "413 Payload Too Large", "request body too large");

    // Large bodies go to PSRAM
    std::unique_ptr<char, decltype(&free)> buf(
        (char *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT), &free);
    if (!buf) buf.reset((char *)malloc(len));
    if (!buf) return sendError(req, "500 Internal Server Error", "out of memory");

    size_t received = 0;
    while (received < len) {
        int r = httpd_req_recv(req, buf.get() + received, len - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;   // slow client, keep waiting
        if (r <= 0) return ESP_FAIL;                  // connection closed, nothing to answer
        received += r;
    }

    cJSON *json = cJSON_ParseWithLength(buf.get(), len);
    if (!json) return sendError(req, "400 Bad Request", "invalid JSON");
    *out = json;
    return ESP_OK;
}

esp_err_t WebServer::sendJson(httpd_req_t *req, cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) return sendError(req, "500 Internal Server Error", "out of memory");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    cJSON_free(text);
    return err;
}

esp_err_t WebServer::sendError(httpd_req_t *req, const char *status, const std::string &msg)
{
    ESP_LOGW(TAG, "%s %s -> %s: %s",
             req->method == HTTP_POST ? "POST" : "GET", req->uri, status, msg.c_str());
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", msg.c_str());
    httpd_resp_set_status(req, status);
    sendJson(req, json);
    return ESP_OK;   // the request was answered: keep the connection
}

esp_err_t WebServer::sendOk(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", true);
    return sendJson(req, json);
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

esp_err_t WebServer::handleIndex(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)WEB_INDEX_HTML_GZ, WEB_INDEX_HTML_GZ_LEN);
}

esp_err_t WebServer::handleGetConfig(httpd_req_t *req)
{
    WebServer *self = (WebServer *)req->user_ctx;
    return sendJson(req, config_to_json(self->config_));
}

esp_err_t WebServer::handleGetStatus(httpd_req_t *req)
{
    WebServer *self = (WebServer *)req->user_ctx;
    cJSON *json = cJSON_CreateObject();

    uint8_t mac[6] = {0};
    esp_netif_get_mac(self->netif_, mac);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(json, "mac", macStr);

    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(self->netif_, &info) == ESP_OK) {
        char ip[16];
        esp_ip4addr_ntoa(&info.ip, ip, sizeof(ip));
        cJSON_AddStringToObject(json, "ip", ip);
    }

    cJSON_AddNumberToObject(json, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(json, "freeHeap", (double)esp_get_free_heap_size());
    return sendJson(req, json);
}

esp_err_t WebServer::handlePostIo(httpd_req_t *req)
{
    WebServer *self = (WebServer *)req->user_ctx;
    cJSON *root;
    esp_err_t rd = readJsonBody(req, &root);
    if (rd != ESP_OK) return rd;      // connection broken: let httpd close it
    if (!root) return ESP_OK;         // an error reply was already sent

    AppConfig next = *self->config_;
    std::string err;
    esp_err_t res = config_parse_io(root, &next, err);
    cJSON_Delete(root);
    if (res != ESP_OK) return sendError(req, "400 Bad Request", err);

    *self->config_ = std::move(next);
    config_apply_io(self->config_);
    if (config_save(self->config_) != ESP_OK) {
        return sendError(req, "500 Internal Server Error", "applied, but saving to flash failed");
    }
    return sendOk(req);
}

esp_err_t WebServer::handlePostShutters(httpd_req_t *req)
{
    WebServer *self = (WebServer *)req->user_ctx;
    cJSON *root;
    esp_err_t rd = readJsonBody(req, &root);
    if (rd != ESP_OK) return rd;      // connection broken: let httpd close it
    if (!root) return ESP_OK;         // an error reply was already sent

    AppConfig next = *self->config_;
    std::string err;
    esp_err_t res = config_parse_shutters(root, &next, err);
    cJSON_Delete(root);
    if (res != ESP_OK) return sendError(req, "400 Bad Request", err);

    *self->config_ = std::move(next);
    config_apply_shutters(self->config_);
    if (config_save(self->config_) != ESP_OK) {
        return sendError(req, "500 Internal Server Error", "applied, but saving to flash failed");
    }
    return sendOk(req);
}

esp_err_t WebServer::handlePostNetwork(httpd_req_t *req)
{
    WebServer *self = (WebServer *)req->user_ctx;
    cJSON *root;
    esp_err_t rd = readJsonBody(req, &root);
    if (rd != ESP_OK) return rd;      // connection broken: let httpd close it
    if (!root) return ESP_OK;         // an error reply was already sent

    AppConfig next = *self->config_;
    std::string err;
    esp_err_t res = config_parse_network(root, &next, err);
    cJSON_Delete(root);
    if (res != ESP_OK) return sendError(req, "400 Bad Request", err);

    *self->config_ = std::move(next);
    if (config_save(self->config_) != ESP_OK) {
        return sendError(req, "500 Internal Server Error", "saving to flash failed");
    }

    // Answer first, switch IP shortly after: changing it now would cut this
    // connection before the browser gets the reply.
    esp_err_t sent = sendOk(req);
    esp_timer_stop(self->netTimer_);
    esp_timer_start_once(self->netTimer_, 500 * 1000);
    return sent;
}

// Runs in the esp_timer task, 500 ms after a successful POST /api/network
void WebServer::applyNetworkTimer(void *arg)
{
    WebServer *self = (WebServer *)arg;
    if (config_apply_network(self->config_, self->netif_) == ESP_OK) {
        artnet_announce();   // tell controllers about the new address
    }
}
