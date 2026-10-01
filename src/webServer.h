#pragma once

#include <string>
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "configStore.h"

struct cJSON;

// HTTP server for the configuration web interface.
//
//   GET  /               the web UI (single page, embedded in the firmware)
//   GET  /api/config     full configuration as JSON
//   GET  /api/status     MAC, current IP, uptime, free memory
//   POST /api/io         {framerate, sacnPriority, universes[]}  -> reallocUniverses
//   POST /api/shutters   {shutters[]}                            -> reallocShutters
//   POST /api/network    {network{ip, netmask, gateway}}         -> new IP (applied after the reply)
//
// Every POST is validated, applied to the running engine and saved to flash.
class WebServer {
public:
    WebServer(AppConfig *config, esp_netif_t *netif);
    ~WebServer();

    esp_err_t start(uint16_t port = 80);
    void stop();

private:
    static esp_err_t handleIndex(httpd_req_t *req);
    static esp_err_t handleGetConfig(httpd_req_t *req);
    static esp_err_t handleGetStatus(httpd_req_t *req);
    static esp_err_t handlePostIo(httpd_req_t *req);
    static esp_err_t handlePostShutters(httpd_req_t *req);
    static esp_err_t handlePostNetwork(httpd_req_t *req);

    static esp_err_t readJsonBody(httpd_req_t *req, cJSON **out);
    static esp_err_t sendJson(httpd_req_t *req, cJSON *json);   // takes ownership
    static esp_err_t sendError(httpd_req_t *req, const char *status, const std::string &msg);
    static esp_err_t sendOk(httpd_req_t *req);
    static void applyNetworkTimer(void *arg);

    esp_err_t registerUri(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *));

    AppConfig          *config_;
    esp_netif_t        *netif_;
    httpd_handle_t      server_ = nullptr;
    esp_timer_handle_t  netTimer_ = nullptr;
};
