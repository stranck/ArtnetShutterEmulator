#include "sacn.h"

#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "lwip/sockets.h"

static const char *TAG = "sacn";

// ---- E1.31 constants ----
static const uint8_t  ACN_PID[12]          = {'A', 'S', 'C', '-', 'E', '1', '.', '1', '7', 0, 0, 0};
static const uint32_t VECTOR_ROOT_E131     = 0x00000004;
static const uint32_t VECTOR_E131_DATA     = 0x00000002;
static const uint8_t  VECTOR_DMP_SET_PROP  = 0x02;
static const uint8_t  OPT_TERMINATED       = 0x40;
static const size_t   HEADER_LEN           = 126;  // everything before the first DMX slot

// Packet layout (offsets):
//   0  Root layer:     preamble, postamble, "ASC-E1.17", flags+len, vector, CID[16]
//  38  Framing layer:  flags+len, vector, source name[64], priority, sync addr,
//                      sequence, options, universe
// 115  DMP layer:      flags+len, vector, addr/data type, first addr, increment,
//                      value count, START code
// 126  DMX data

static int               s_sock = -1;
static SemaphoreHandle_t s_lock = NULL;
static uint8_t           s_cid[16];
static char              s_source_name[64];
static volatile uint8_t  s_priority = 100;  // single byte: reads/writes are atomic

// ---------- helpers ----------

static inline void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xFF; }
static inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = (v >> 16) & 0xFF; p[2] = (v >> 8) & 0xFF; p[3] = v & 0xFF;
}

static esp_err_t resolve_dest(uint16_t universe, const struct in_addr *dest_ip, struct sockaddr_in *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->sin_family = AF_INET;
    dst->sin_port = htons(SACN_PORT);
    if (dest_ip) {
        dst->sin_addr = *dest_ip;
    } else {
        // Multicast group 239.255.<hi>.<lo>
        dst->sin_addr.s_addr = htonl((239u << 24) | (255u << 16) | universe);
    }
    return ESP_OK;
}

static esp_err_t send_packet(uint16_t universe, uint8_t sequence, const uint8_t *data,
                             uint16_t length, uint8_t options, const struct sockaddr_in *dst)
{
    uint8_t pkt[HEADER_LEN + SACN_MAX_DMX];
    const size_t total = HEADER_LEN + length;
    memset(pkt, 0, HEADER_LEN);

    // Root layer
    put16(&pkt[0], 0x0010);                          // preamble size
    put16(&pkt[2], 0x0000);                          // postamble size
    memcpy(&pkt[4], ACN_PID, sizeof(ACN_PID));
    put16(&pkt[16], 0x7000 | (total - 16));          // flags + length
    put32(&pkt[18], VECTOR_ROOT_E131);
    memcpy(&pkt[22], s_cid, 16);

    // Framing layer
    put16(&pkt[38], 0x7000 | (total - 38));
    put32(&pkt[40], VECTOR_E131_DATA);
    memcpy(&pkt[44], s_source_name, strlen(s_source_name));  // rest stays zero
    pkt[108] = s_priority;
    put16(&pkt[109], 0);                             // synchronization address (unused)
    pkt[111] = sequence;
    pkt[112] = options;
    put16(&pkt[113], universe);

    // DMP layer
    put16(&pkt[115], 0x7000 | (total - 115));
    pkt[117] = VECTOR_DMP_SET_PROP;
    pkt[118] = 0xA1;                                 // address & data type
    put16(&pkt[119], 0x0000);                        // first property address
    put16(&pkt[121], 0x0001);                        // address increment
    put16(&pkt[123], length + 1);                    // value count (START code + slots)
    pkt[125] = 0x00;                                 // START code: DMX
    memcpy(&pkt[HEADER_LEN], data, length);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int sent = sendto(s_sock, pkt, total, 0, (const struct sockaddr *)dst, sizeof(*dst));
    xSemaphoreGive(s_lock);

    if (sent < 0) {
        ESP_LOGW(TAG, "sendto failed: errno %d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ---------- public API ----------

esp_err_t sacn_init(const sacn_config_t *cfg)
{
    if (!cfg || cfg->priority > 200) return ESP_ERR_INVALID_ARG;

    strlcpy(s_source_name, cfg->source_name ? cfg->source_name : "ESP32 sACN",
            sizeof(s_source_name));
    s_priority = cfg->priority;

    if (cfg->cid) {
        memcpy(s_cid, cfg->cid, 16);
    } else {
        // Stable per-device ID: fixed prefix + Ethernet MAC
        static const uint8_t prefix[10] = {0x53, 0x41, 0x43, 0x4E, 0x2D, 0x45,
                                           0x40, 0x00, 0x80, 0x00};  // version 4 / variant bits set
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_ETH);
        memcpy(s_cid, prefix, 10);
        memcpy(&s_cid[10], mac, 6);
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return ESP_FAIL;
    }

    // Multicast TTL: how many routers the packets may cross (1 = local network only
    // would also work; 16 leaves room for routed lighting networks)
    uint8_t ttl = 16;
    setsockopt(s_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    ESP_LOGI(TAG, "Source \"%s\" ready (priority %u)", s_source_name, s_priority);
    return ESP_OK;
}

esp_err_t sacn_set_priority(uint8_t priority)
{
    if (priority > 200) return ESP_ERR_INVALID_ARG;
    s_priority = priority;
    return ESP_OK;
}

uint8_t sacn_get_priority(void)
{
    return s_priority;
}

esp_err_t sacn_send_dmx(uint16_t universe, uint8_t sequence, const uint8_t *data,
                        uint16_t length, const struct in_addr *dest_ip)
{
    if (s_sock < 0) return ESP_ERR_INVALID_STATE;
    if (universe < 1 || universe > 63999) return ESP_ERR_INVALID_ARG;
    if (!data || length < 1 || length > SACN_MAX_DMX) return ESP_ERR_INVALID_ARG;

    struct sockaddr_in dst;
    esp_err_t err = resolve_dest(universe, dest_ip, &dst);
    if (err != ESP_OK) return err;

    return send_packet(universe, sequence, data, length, 0, &dst);
}

esp_err_t sacn_terminate(uint16_t universe, uint8_t sequence, const struct in_addr *dest_ip)
{
    if (s_sock < 0) return ESP_ERR_INVALID_STATE;
    if (universe < 1 || universe > 63999) return ESP_ERR_INVALID_ARG;

    struct sockaddr_in dst;
    esp_err_t err = resolve_dest(universe, dest_ip, &dst);
    if (err != ESP_OK) return err;

    static const uint8_t blank[1] = {0};
    for (uint8_t i = 0; i < 3; i++) {  // spec: send the terminated flag 3 times
        err = send_packet(universe, (uint8_t)(sequence + i), blank, 1, OPT_TERMINATED, &dst);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
