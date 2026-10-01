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
#include "lwip/inet.h"

#include "artnet.h"
#include "fakeShutter.h"
#include "configStore.h"
#include "webServer.h"

static const char *TAG = "main";

// Waveshare ESP32-S3-ETH: W5500 wired over SPI
#define ETH_SCK_GPIO   13
#define ETH_MISO_GPIO  12
#define ETH_MOSI_GPIO  11
#define ETH_CS_GPIO    14
#define ETH_INT_GPIO   10
#define ETH_RST_GPIO   9

// Hold this button for RECOVERY_HOLD_MS while the device runs to restore the
// factory network settings. (Holding it at power-on would enter the ROM
// download mode instead: GPIO0 is a strapping pin.)
#define RECOVERY_BUTTON_GPIO  GPIO_NUM_0   // BOOT button
#define RECOVERY_HOLD_MS      5000

static AppConfig  s_config;      // the live configuration (edited by the web UI)
static WebServer *s_web = nullptr;

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

// ---------- Recovery ----------

static void recovery_task(void *arg)
{
    esp_netif_t *netif = (esp_netif_t *)arg;

    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << RECOVERY_BUTTON_GPIO;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);

    int heldMs = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level(RECOVERY_BUTTON_GPIO) != 0) {   // released (pressed = low)
            heldMs = 0;
            continue;
        }
        heldMs += 100;
        if (heldMs != RECOVERY_HOLD_MS) continue;          // fire once per press

        AppConfig defaults;
        config_defaults(&defaults);
        s_config.network = defaults.network;
        ESP_LOGW(TAG, "BOOT held %d s: restoring factory network settings", RECOVERY_HOLD_MS / 1000);
        config_save(&s_config);
        if (config_apply_network(&s_config, netif) == ESP_OK) artnet_announce();
    }
}

// ---------- Ethernet setup ----------

static esp_netif_t *start_ethernet(const AppConfig *cfg)
{
    // Network interface with static IP (no DHCP)
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    ESP_ERROR_CHECK(config_apply_network(cfg, eth_netif));

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

    // Configuration stored in flash (defaults on first boot)
    config_load(&s_config);

    esp_netif_t *eth_netif = start_ethernet(&s_config);
    xTaskCreate(recovery_task, "recovery", 4096, eth_netif, 2, NULL);

    initFakeShutter(eth_netif);
    config_apply_io(&s_config);
    config_apply_shutters(&s_config);

    s_web = new WebServer(&s_config, eth_netif);
    ESP_ERROR_CHECK(s_web->start());

    // Let controllers know we're here once the link is up
    vTaskDelay(pdMS_TO_TICKS(2000));
    artnet_announce();
}