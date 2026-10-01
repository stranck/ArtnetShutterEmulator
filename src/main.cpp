#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_http_server.h"
#include "lwip/inet.h"

#include "artnet.h"
#include "fakeShutter.h"

static const char *TAG = "main";

// Waveshare ESP32-S3-ETH: W5500 wired over SPI
#define ETH_SCK_GPIO   13
#define ETH_MISO_GPIO  12
#define ETH_MOSI_GPIO  11
#define ETH_CS_GPIO    14
#define ETH_INT_GPIO   10
#define ETH_RST_GPIO   9

// Static network config (adjust to match your network)
#define IP_ADDR   "2.0.3.1"
#define GW_ADDR   "2.0.1.1"
#define NETMASK   "255.0.0.0"

// ---------- Ethernet events ----------

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet link up");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet link down (check cable)");
        break;
    default:
        break;
    }
}

// ---------- HTTP server ----------

// Called for every GET request (any path)
static esp_err_t http_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "HTTP GET %s", req->uri);
    static const char html[] = "<!DOCTYPE html><html><body>test</body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();   // port 80
    config.uri_match_fn = httpd_uri_match_wildcard;   // allow "/*" patterns

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_uri_t get_uri = {};
    get_uri.uri = "/*";
    get_uri.method = HTTP_GET;
    get_uri.handler = http_get_handler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &get_uri));

    ESP_LOGI(TAG, "HTTP server listening on port %d", config.server_port);
}

// ---------- Ethernet setup ----------

static esp_netif_t *start_ethernet(esp_netif_ip_info_t *ip_info)
{
    // Network interface with static IP (no DHCP)
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);

    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(eth_netif));
    memset(ip_info, 0, sizeof(*ip_info));
    ip_info->ip.addr      = esp_ip4addr_aton(IP_ADDR);
    ip_info->gw.addr      = esp_ip4addr_aton(GW_ADDR);
    ip_info->netmask.addr = esp_ip4addr_aton(NETMASK);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(eth_netif, ip_info));

    // SPI bus
    spi_bus_config_t buscfg;
    memset(&buscfg, 0, sizeof(buscfg));
    buscfg.miso_io_num = ETH_MISO_GPIO;
    buscfg.mosi_io_num = ETH_MOSI_GPIO;
    buscfg.sclk_io_num = ETH_SCK_GPIO;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg;
    memset(&devcfg, 0, sizeof(devcfg));
    devcfg.mode = 0;
    devcfg.clock_speed_hz = 20 * 1000 * 1000;
    devcfg.spics_io_num = ETH_CS_GPIO;
    devcfg.queue_size = 20;

    // W5500 MAC + PHY (driver from the espressif/w5500 component)
    eth_w5500_config_t w5500_cfg = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &devcfg);
    w5500_cfg.base.int_gpio_num = ETH_INT_GPIO;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    mac_cfg.rx_task_stack_size = 4096;
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_cfg, &mac_cfg);

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = 1;
    phy_cfg.reset_gpio_num = ETH_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth_handle));

    // Use a MAC derived from the ESP32's own base MAC
    uint8_t mac_addr[6];
    ESP_ERROR_CHECK(esp_read_mac(mac_addr, ESP_MAC_ETH));
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr));

    // Attach to the TCP/IP stack and start
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, glue));
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
    return eth_netif;
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "PSRAM: %u KB", (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               &eth_event_handler, NULL));

    esp_netif_ip_info_t ip_info;
    esp_netif_t *eth_netif = start_ethernet(&ip_info);
    ESP_LOGI(TAG, "Static IP: " IPSTR, IP2STR(&ip_info.ip));

    start_http_server();
    initFakeShutter(eth_netif);

    // Let controllers know we're here once the link is up
    vTaskDelay(pdMS_TO_TICKS(2000));
    artnet_announce();

    // Example for later (sending):
    //   uint8_t dmx[512] = {255, 128, 0};
    //   artnet_send_dmx(ARTNET_PORT_ADDRESS(0, 0, 1), dmx, sizeof(dmx), "192.168.1.255");
}