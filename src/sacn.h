#pragma once

#include <stdint.h>
#include "esp_err.h"

// sACN (ANSI E1.31 / Streaming ACN) sender.
//
// Packets go to UDP port 5568, by default to the universe's multicast group
// 239.255.<universe hi>.<universe lo>, so every receiver listening to that
// universe gets them without knowing this node's IP.

#define SACN_PORT          5568
#define SACN_MAX_DMX       512

typedef struct {
    const char    *source_name;   // shown by receivers, up to 63 chars
    uint8_t        priority;      // 0-200, default 100 (higher wins when merging sources)
    const uint8_t *cid;           // 16-byte unique source ID, or NULL to derive one from the MAC
} sacn_config_t;

#define SACN_DEFAULT_CONFIG(_name) { (_name), 100, NULL }

// Create the sending socket. Call once, after the network is up.
esp_err_t sacn_init(const sacn_config_t *cfg);

// Change the priority used by all following packets (0-200).
// Safe to call from any task at any time.
esp_err_t sacn_set_priority(uint8_t priority);
uint8_t   sacn_get_priority(void);

// Send one DMX frame.
//   universe : 1..63999 (sACN universes start at 1, unlike Art-Net)
//   sequence : sequence number for this packet. Keep one counter per universe and
//              increment it on every packet (wrapping 255 -> 0 is expected).
//   data     : DMX channel values (data[0] = channel 1)
//   length   : 1..512
//   dest_ip  : NULL for multicast (normal case), or e.g. "192.168.1.100" for unicast
// Safe to call from any task.
esp_err_t sacn_send_dmx(uint16_t universe, uint8_t sequence, const uint8_t *data,
                        uint16_t length, const struct in_addr *dest_ip);

// Tell receivers this source is stopping on `universe` (sends the 3
// "stream terminated" packets the spec requires), so they drop it
// immediately instead of waiting for their 2.5 s timeout.
// The packets use sequence, sequence+1 and sequence+2: pass the next number
// of that universe's counter.
esp_err_t sacn_terminate(uint16_t universe, uint8_t sequence, const struct in_addr *dest_ip);
