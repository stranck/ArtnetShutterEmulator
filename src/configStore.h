#pragma once

// Persistent configuration of the shutter emulator.
//
// The whole configuration is kept in RAM as plain structs, serialized to JSON
// and stored as a single blob in the "config" NVS partition. The same JSON
// format is used by the web interface (GET /api/config), so there is only one
// parser and one serializer.
//
// Addresses in this module are what a user types: DMX addresses are 1..512.
// They are converted to the 0-based indexes fakeShutter expects only when the
// config is applied.

#include <stdint.h>
#include <string>
#include <vector>
#include "esp_err.h"
#include "esp_netif.h"

struct cJSON;

#define CONFIG_MAX_NAME_LEN     32
#define CONFIG_MAX_UNIVERSES    256   // after expanding the "count" of every row
#define CONFIG_MAX_SHUTTERS     1024
#define CONFIG_MAX_DESTS        513    // destinations per shutter
#define CONFIG_MAX_FRAMERATE    200
#define CONFIG_ARTNET_MAX_UNI   32767 // 15-bit Port-Address
#define CONFIG_SACN_MAX_UNI     63999

// Network settings, all in network byte order (as esp_netif uses them)
struct NetworkConfig {
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
};

// One row of the "Inputs & outputs" table. A row with count N maps the N
// consecutive Art-Net input universes in..in+N-1 to the outputs out..out+N-1.
struct UniverseRow {
    uint16_t in;          // Art-Net input Port-Address (0..32767)
    uint16_t count;       // how many consecutive universes (>= 1)
    bool     artnetOut;   // true: Art-Net output, false: sACN output
    uint16_t out;         // Art-Net Port-Address, or sACN universe (1..63999)
    uint32_t outAddr;     // unicast destination, network byte order; 0 = broadcast/multicast
};

// A DMX channel: Art-Net input universe + 1-based DMX address.
// `universe` is the real 0-based Port-Address; only the web UI shows it +1.
struct ChannelRef {
    uint16_t universe;
    uint16_t address;     // 1..512
};

struct ShutterRow {
    std::string             name;
    ChannelRef              source;
    std::vector<ChannelRef> dests;
};

struct AppConfig {
    NetworkConfig            network;
    int                      framerate;     // frames per second
    uint8_t                  sacnPriority;  // 0..200
    bool                     doubleOn;      // "force min DMX frame = 2" (setDoubleOn)
    bool                     constTrans;    // Always transmit artnet, not only on changes
    std::vector<UniverseRow> universes;
    std::vector<ShutterRow>  shutters;
};

// Factory defaults (used when nothing is stored, or when the BOOT button is
// held while powering up).
void config_defaults(AppConfig *cfg);

// Mount the "config" NVS partition and load the stored configuration.
// Falls back to defaults if nothing valid is stored.
esp_err_t config_load(AppConfig *cfg);

// Write the configuration to flash.
esp_err_t config_save(const AppConfig *cfg);

// Push the configuration into the running fake shutter engine.
void config_apply_io(const AppConfig *cfg);        // reallocUniverses + framerate + sACN priority + doubleOn + constTrans
void config_apply_shutters(const AppConfig *cfg);  // reallocShutters
esp_err_t config_apply_network(const AppConfig *cfg, esp_netif_t *netif);

// ---- JSON <-> structs ----
// All parse functions validate everything they read. On failure they return
// ESP_ERR_INVALID_ARG and write a human readable reason into `err`.

cJSON    *config_to_json(const AppConfig *cfg);
esp_err_t config_from_json(const cJSON *root, AppConfig *cfg, std::string &err);

esp_err_t config_parse_io(const cJSON *root, AppConfig *cfg, std::string &err);
esp_err_t config_parse_shutters(const cJSON *root, AppConfig *cfg, std::string &err);
esp_err_t config_parse_network(const cJSON *root, AppConfig *cfg, std::string &err);
