#include "artnet.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "lwip/sockets.h"

static const char *TAG = "artnet";

// ---- Protocol constants ----
static const uint8_t  ARTNET_ID[8]     = {'A', 'r', 't', '-', 'N', 'e', 't', 0};
static const uint16_t OP_POLL          = 0x2000;
static const uint16_t OP_POLL_REPLY    = 0x2100;
static const uint16_t OP_DMX           = 0x5000;
static const uint8_t  PROTOCOL_VERSION = 14;
static const size_t   DMX_HEADER_LEN   = 18;
static const size_t   POLL_REPLY_LEN   = 239;

static const uint16_t FIRMWARE_VERSION = 1;
static const uint16_t OEM_CODE         = 0x00FF;  // "unknown / not registered"
static const uint16_t ESTA_CODE        = 0x7FF0;  // ESTA prototyping/experimental ID

// ArtDmx packet layout:
//  0..7   "Art-Net\0"
//  8..9   OpCode, little-endian (0x5000)
// 10..11  Protocol version, big-endian (14)
// 12      Sequence
// 13      Physical input port
// 14      SubUni (low byte of Port-Address)
// 15      Net    (high 7 bits of Port-Address)
// 16..17  Data length, big-endian (2..512, even)
// 18..    DMX data

static int               s_sock = -1;
static artnet_dmx_cb_t   s_on_dmx = NULL;
static SemaphoreHandle_t s_tx_lock = NULL;
static StaticSemaphore_t s_buffer;
static uint8_t           s_tx_seq = 0;

static artnet_config_t   s_cfg;
static char              s_short_name[18];
static char              s_long_name[64];
static uint32_t          s_poll_count = 0;

// ---------- helpers ----------

static void write_header(uint8_t *pkt, uint16_t opcode)
{
    memcpy(pkt, ARTNET_ID, sizeof(ARTNET_ID));
    pkt[8] = opcode & 0xFF;
    pkt[9] = opcode >> 8;
}

static esp_err_t send_raw(const uint8_t *pkt, size_t len, uint32_t dest_ip /* network order */)
{
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(ARTNET_PORT);
    dst.sin_addr.s_addr = dest_ip;

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int sent = sendto(s_sock, pkt, len, 0, (struct sockaddr *)&dst, sizeof(dst));
    xSemaphoreGive(s_tx_lock);

    if (sent < 0) {
        ESP_LOGW(TAG, "sendto failed: errno %d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ---------- ArtPollReply ----------

static esp_err_t send_poll_reply(uint32_t dest_ip)
{
    esp_netif_ip_info_t ip_info;
    uint8_t mac[6] = {0};
    if (esp_netif_get_ip_info(s_cfg.netif, &ip_info) != ESP_OK) return ESP_FAIL;
    esp_netif_get_mac(s_cfg.netif, mac);

    uint8_t pkt[POLL_REPLY_LEN];
    memset(pkt, 0, sizeof(pkt));
    write_header(pkt, OP_POLL_REPLY);

    memcpy(&pkt[10], &ip_info.ip.addr, 4);     // IP address (network order)
    pkt[14] = ARTNET_PORT & 0xFF;              // Port, little-endian
    pkt[15] = ARTNET_PORT >> 8;
    pkt[16] = FIRMWARE_VERSION >> 8;           // VersInfo hi/lo
    pkt[17] = FIRMWARE_VERSION & 0xFF;
    pkt[18] = 0;                               // NetSwitch (no fixed universes)
    pkt[19] = 0;                               // SubSwitch
    pkt[20] = OEM_CODE >> 8;                   // Oem hi/lo
    pkt[21] = OEM_CODE & 0xFF;
    pkt[22] = 0;                               // UBEA version
    pkt[23] = 0xD0;                            // Status1: indicators normal, address set locally
    pkt[24] = ESTA_CODE & 0xFF;                // EstaMan lo/hi
    pkt[25] = ESTA_CODE >> 8;
    memcpy(&pkt[26], s_short_name, strlen(s_short_name));   // ShortName[18]
    memcpy(&pkt[44], s_long_name, strlen(s_long_name));     // LongName[64]
    snprintf((char *)&pkt[108], 64, "#0001 [%04lu] OK",      // NodeReport[64]
             (unsigned long)(s_poll_count % 10000));
    // NumPorts, PortTypes, GoodInput/Output, SwIn/SwOut (bytes 172..193) stay 0:
    // the node advertises no specific universes and accepts all of them.

    pkt[200] = 0x00;                           // Style: StNode
    memcpy(&pkt[201], mac, 6);                 // MAC
    memcpy(&pkt[207], &ip_info.ip.addr, 4);    // BindIp (this is the root device)
    pkt[211] = 1;                              // BindIndex
    pkt[212] = 0x08;                           // Status2: supports 15-bit Port-Address

    return send_raw(pkt, sizeof(pkt), dest_ip);
}

// ---------- receive path ----------

static void handle_packet(const uint8_t *pkt, int len, uint32_t src_ip)
{
    if (len < 10 || memcmp(pkt, ARTNET_ID, sizeof(ARTNET_ID)) != 0) {
        return;  // not Art-Net
    }

    uint16_t opcode = pkt[8] | (pkt[9] << 8);
    switch (opcode) {
    case OP_POLL: {
        s_poll_count++;
        char ip_str[16];
        inet_ntoa_r(src_ip, ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "ArtPoll from %s, replying", ip_str);
        send_poll_reply(src_ip);  // unicast reply to the controller
        break;
    }
    case OP_DMX: {
        if (len < (int)DMX_HEADER_LEN) return;
        uint16_t universe = pkt[14] | ((pkt[15] & 0x7F) << 8);
        uint16_t dmx_len  = (pkt[16] << 8) | pkt[17];
        if (dmx_len < 1 || dmx_len > ARTNET_MAX_DMX || len < (int)(DMX_HEADER_LEN + dmx_len)) {
            return;  // malformed
        }
        if (s_on_dmx) {
            s_on_dmx(universe, pkt + DMX_HEADER_LEN, dmx_len, pkt[12], src_ip);
        }
        break;
    }
    default:
        // Other opcodes (ArtSync, ArtAddress, ...) are ignored for now.
        break;
    }
}

static void artnet_rx_task(void *arg)
{
    static uint8_t buf[DMX_HEADER_LEN + ARTNET_MAX_DMX + 64];

    while (true) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &src_len);
        if (n < 0) {
            ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        handle_packet(buf, n, src.sin_addr.s_addr);
    }
}

// ---------- public API ----------

esp_err_t artnet_init(const artnet_config_t *cfg, artnet_dmx_cb_t on_dmx)
{
    if (!cfg || !cfg->netif) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg = *cfg;
    strlcpy(s_short_name, cfg->short_name ? cfg->short_name : "ESP32 Art-Net", sizeof(s_short_name));
    strlcpy(s_long_name, cfg->long_name ? cfg->long_name : "ESP32 Art-Net node", sizeof(s_long_name));
    s_cfg.short_name = s_short_name;
    s_cfg.long_name = s_long_name;

    s_on_dmx = on_dmx;

    s_tx_lock = xSemaphoreCreateMutexStatic(&s_buffer);
    if (!s_tx_lock) return ESP_ERR_NO_MEM;

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return ESP_FAIL;
    }

    int opt = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);  // receives unicast and broadcast
    addr.sin_port = htons(ARTNET_PORT);
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind() failed: errno %d", errno);
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    if (xTaskCreate(artnet_rx_task, "artnet_rx", 4096, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Node \"%s\" listening on UDP port %d", s_short_name, ARTNET_PORT);
    return ESP_OK;
}

esp_err_t artnet_announce(void)
{
    if (s_sock < 0) return ESP_ERR_INVALID_STATE;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_cfg.netif, &ip_info) != ESP_OK) return ESP_FAIL;
    uint32_t bcast = ip_info.ip.addr | ~ip_info.netmask.addr;  // e.g. 192.168.1.255
    return send_poll_reply(bcast);
}

esp_err_t artnet_send_dmx(uint16_t universe, const uint8_t *data, uint16_t length,
                          const struct in_addr &dest_ip)
{
    if (s_sock < 0) return ESP_ERR_INVALID_STATE;
    if (!data || length < 1 || length > ARTNET_MAX_DMX) return ESP_ERR_INVALID_ARG;

    uint16_t wire_len = (length + 1) & ~1;  // spec requires an even length
    uint8_t pkt[DMX_HEADER_LEN + ARTNET_MAX_DMX];

    write_header(pkt, OP_DMX);
    pkt[10] = 0;
    pkt[11] = PROTOCOL_VERSION;
    pkt[13] = 0;                              // physical port
    pkt[14] = universe & 0xFF;                // SubUni
    pkt[15] = (universe >> 8) & 0x7F;         // Net
    pkt[16] = wire_len >> 8;
    pkt[17] = wire_len & 0xFF;
    memcpy(pkt + DMX_HEADER_LEN, data, length);
    if (wire_len != length) pkt[DMX_HEADER_LEN + length] = 0;

    // 1..255, 0 means "no sequencing". Not under the TX lock, but a rare
    // duplicate sequence number between two sending tasks is harmless.
    s_tx_seq = (s_tx_seq == 255) ? 1 : s_tx_seq + 1;
    pkt[12] = s_tx_seq;

    return send_raw(pkt, DMX_HEADER_LEN + wire_len, dest_ip.s_addr);
}