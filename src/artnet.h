#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_netif.h"

#define ARTNET_PORT       6454
#define ARTNET_MAX_DMX    512

// Build a 15-bit Port-Address from its parts (net 0-127, subnet 0-15, universe 0-15)
#define ARTNET_PORT_ADDRESS(net, subnet, uni) \
    ((uint16_t)((((net) & 0x7F) << 8) | (((subnet) & 0x0F) << 4) | ((uni) & 0x0F)))

typedef struct {
    esp_netif_t *netif;           // interface whose IP/MAC are reported in ArtPollReply
    const char  *short_name;      // up to 17 chars
    const char  *long_name;       // up to 63 chars
} artnet_config_t;

// Called for every valid ArtDmx packet received.
//   universe : 15-bit Port-Address (Net << 8 | SubNet << 4 | Universe)
//   data     : DMX channel values (data[0] = channel 1)
//   length   : number of channels in this packet (1..512)
//   sequence : Art-Net sequence number (0 = sequencing disabled)
//   src_ip   : sender IPv4 address, network byte order
//   ctx      : the pointer you passed to artnet_init()
//
// Runs in the Art-Net receive task: keep it short, don't block.
// `data` is only valid during the call, copy it if you need it later.
// Every ArtDmx packet is delivered, whatever its universe/Net/SubNet.
typedef void (*artnet_dmx_cb_t)(uint16_t universe, const uint8_t *data, uint16_t length, uint8_t sequence, uint32_t src_ip);

// Open the UDP socket on port 6454 and start the receive task.
// The config is copied, so it doesn't need to outlive the call.
esp_err_t artnet_init(const artnet_config_t *cfg, artnet_dmx_cb_t on_dmx);

// Send an ArtDmx packet.
//   dest_ip : e.g. "192.168.1.100" (unicast) or "192.168.1.255" (broadcast)
//   length  : 1..512 (odd lengths are padded with one zero byte, as the spec requires)
// Safe to call from any task.
esp_err_t artnet_send_dmx(uint16_t universe, const uint8_t *data, uint16_t length, const struct in_addr &dest_ip);

// Broadcast an unsolicited ArtPollReply (e.g. at startup, or after a config change)
// so controllers notice the node without waiting for their next ArtPoll.
esp_err_t artnet_announce(void);