#include "configStore.h"

#include <string.h>
#include <stdio.h>
#include <algorithm>
#include <memory>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#if __has_include(<cJSON.h>)
#include <cJSON.h>
#else
#include <cjson/cJSON.h>
#endif

#include "fakeShutter.h"
#include "sacn.h"

static const char *TAG = "config";

static const char *NVS_PARTITION = "config";   // see partitions.csv
static const char *NVS_NAMESPACE = "shutter";
static const char *NVS_KEY       = "cfg";

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

void config_defaults(AppConfig *cfg)
{
    cfg->network.ip      = inet_addr("2.0.3.1");
    cfg->network.netmask = inet_addr("255.0.0.0");
    cfg->network.gateway = inet_addr("2.0.1.1");
    cfg->framerate       = 44;
    cfg->sacnPriority    = 100;
    cfg->doubleOn        = false;
    cfg->constTrans       = false;
    cfg->outputEnable     = true;
    cfg->universes.clear();
    cfg->shutters.clear();
}

// ---------------------------------------------------------------------------
// Small JSON helpers
// ---------------------------------------------------------------------------

static std::string ip_to_string(uint32_t addr)
{
    if (addr == 0) return "";
    struct in_addr a;
    a.s_addr = addr;
    char buf[16];
    inet_ntoa_r(a, buf, sizeof(buf));
    return buf;
}

// Parse "a.b.c.d" (network byte order). Empty string -> 0 if allowEmpty.
static bool parse_ip(const char *s, uint32_t *out, bool allowEmpty)
{
    if (!s || !*s) {
        if (!allowEmpty) return false;
        *out = 0;
        return true;
    }
    struct in_addr a;
    if (inet_pton(AF_INET, s, &a) != 1) return false;
    *out = a.s_addr;
    return true;
}

// Read an integer field and check its range
static bool get_int(const cJSON *obj, const char *key, int min, int max, int *out,
                    std::string &err, const std::string &where)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item)) {
        err = where + ": missing or invalid \"" + key + "\"";
        return false;
    }
    double v = item->valuedouble;
    if (v != (int)v || v < min || v > max) {
        err = where + ": \"" + key + "\" must be an integer between " +
              std::to_string(min) + " and " + std::to_string(max);
        return false;
    }
    *out = (int)v;
    return true;
}

// A channel is serialized as [universe, address]
static bool parse_channel(const cJSON *item, ChannelRef *out, std::string &err,
                          const std::string &where)
{
    if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) != 2) {
        err = where + ": channel must be [universe, address]";
        return false;
    }
    const cJSON *u = cJSON_GetArrayItem(item, 0);
    const cJSON *a = cJSON_GetArrayItem(item, 1);
    if (!cJSON_IsNumber(u) || !cJSON_IsNumber(a)) {
        err = where + ": channel values must be numbers";
        return false;
    }
    double uv = u->valuedouble, av = a->valuedouble;
    if (uv != (int)uv || uv < 0 || uv > CONFIG_ARTNET_MAX_UNI) {
        err = where + ": universe must be 0.." + std::to_string(CONFIG_ARTNET_MAX_UNI);
        return false;
    }
    if (av != (int)av || av < 1 || av > 512) {
        err = where + ": DMX address must be 1..512";
        return false;
    }
    out->universe = (uint16_t)uv;
    out->address = (uint16_t)av;
    return true;
}

static cJSON *channel_to_json(const ChannelRef &c)
{
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(c.universe));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(c.address));
    return arr;
}

// Unique key of a DMX channel across all universes
static inline uint32_t channel_key(const ChannelRef &c)
{
    return (uint32_t)c.universe * 512u + (c.address - 1);
}

// Same notation as the web UI: universes are shown 1-based (1 = Art-Net Port-Address 0)
static std::string channel_str(const ChannelRef &c)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%03u", (unsigned)c.universe + 1, c.address);
    return buf;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

cJSON *config_to_json(const AppConfig *cfg)
{
    cJSON *root = cJSON_CreateObject();

    cJSON *net = cJSON_AddObjectToObject(root, "network");
    cJSON_AddStringToObject(net, "ip", ip_to_string(cfg->network.ip).c_str());
    cJSON_AddStringToObject(net, "netmask", ip_to_string(cfg->network.netmask).c_str());
    cJSON_AddStringToObject(net, "gateway", ip_to_string(cfg->network.gateway).c_str());

    cJSON_AddNumberToObject(root, "framerate", cfg->framerate);
    cJSON_AddNumberToObject(root, "sacnPriority", cfg->sacnPriority);
    cJSON_AddBoolToObject(root, "doubleOn", cfg->doubleOn);
    cJSON_AddBoolToObject(root, "constTrans", cfg->constTrans);
    cJSON_AddBoolToObject(root, "outputEnable", cfg->outputEnable);

    cJSON *unis = cJSON_AddArrayToObject(root, "universes");
    for (const UniverseRow &r : cfg->universes) {
        cJSON *row = cJSON_CreateObject();
        cJSON_AddNumberToObject(row, "in", r.in);
        cJSON_AddNumberToObject(row, "count", r.count);
        cJSON_AddStringToObject(row, "proto", r.artnetOut ? "artnet" : "sacn");
        cJSON_AddNumberToObject(row, "out", r.out);
        cJSON_AddStringToObject(row, "addr", ip_to_string(r.outAddr).c_str());
        cJSON_AddItemToArray(unis, row);
    }

    cJSON *shutters = cJSON_AddArrayToObject(root, "shutters");
    for (const ShutterRow &s : cfg->shutters) {
        cJSON *row = cJSON_CreateObject();
        cJSON_AddStringToObject(row, "name", s.name.c_str());
        cJSON_AddItemToObject(row, "src", channel_to_json(s.source));
        cJSON *dst = cJSON_AddArrayToObject(row, "dst");
        for (const ChannelRef &d : s.dests) cJSON_AddItemToArray(dst, channel_to_json(d));
        cJSON_AddItemToArray(shutters, row);
    }

    return root;
}

// ---------------------------------------------------------------------------
// Parsing + validation
// ---------------------------------------------------------------------------

esp_err_t config_parse_network(const cJSON *root, AppConfig *cfg, std::string &err)
{
    const cJSON *net = cJSON_GetObjectItemCaseSensitive(root, "network");
    if (!cJSON_IsObject(net)) {
        err = "missing \"network\" object";
        return ESP_ERR_INVALID_ARG;
    }

    NetworkConfig n;
    const cJSON *ip = cJSON_GetObjectItemCaseSensitive(net, "ip");
    const cJSON *mask = cJSON_GetObjectItemCaseSensitive(net, "netmask");
    const cJSON *gw = cJSON_GetObjectItemCaseSensitive(net, "gateway");

    if (!parse_ip(cJSON_GetStringValue(ip), &n.ip, false) || n.ip == 0 || n.ip == 0xFFFFFFFF) {
        err = "invalid IP address";
        return ESP_ERR_INVALID_ARG;
    }
    if (!parse_ip(cJSON_GetStringValue(mask), &n.netmask, false) || n.netmask == 0) {
        err = "invalid netmask";
        return ESP_ERR_INVALID_ARG;
    }
    // A valid mask is a run of 1s followed by 0s: inverting it (host order) gives 2^k - 1
    uint32_t hostMask = ~ntohl(n.netmask);
    if ((hostMask & (hostMask + 1)) != 0) {
        err = "netmask must be contiguous (e.g. 255.255.255.0)";
        return ESP_ERR_INVALID_ARG;
    }
    if ((n.ip & ~n.netmask) == 0 || (n.ip & ~n.netmask) == ~n.netmask) {
        err = "IP address cannot be the network or broadcast address of its subnet";
        return ESP_ERR_INVALID_ARG;
    }
    if (!parse_ip(cJSON_GetStringValue(gw), &n.gateway, true)) {
        err = "invalid gateway (leave empty for none)";
        return ESP_ERR_INVALID_ARG;
    }

    cfg->network = n;
    return ESP_OK;
}

esp_err_t config_parse_io(const cJSON *root, AppConfig *cfg, std::string &err)
{
    int framerate, priority;
    if (!get_int(root, "framerate", 1, CONFIG_MAX_FRAMERATE, &framerate, err, "settings") ||
        !get_int(root, "sacnPriority", 0, 200, &priority, err, "settings")) {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *dbl;
    // Optional: configurations saved before this setting existed don't have it
    bool doubleOn = false;
    dbl = cJSON_GetObjectItemCaseSensitive(root, "doubleOn");
    if (dbl) {
        if (!cJSON_IsBool(dbl)) {
            err = "settings: \"doubleOn\" must be true or false";
            return ESP_ERR_INVALID_ARG;
        }
        doubleOn = cJSON_IsTrue(dbl);
    }

    // Optional: configurations saved before this setting existed don't have it
    bool constTrans = false;
    dbl = cJSON_GetObjectItemCaseSensitive(root, "constTrans");
    if (dbl) {
        if (!cJSON_IsBool(dbl)) {
            err = "settings: \"constTrans\" must be true or false";
            return ESP_ERR_INVALID_ARG;
        }
        constTrans = cJSON_IsTrue(dbl);
    }

    // Optional: configurations saved before this setting existed don't have it -> output stays on
    bool outputEnable = true;
    dbl = cJSON_GetObjectItemCaseSensitive(root, "outputEnable");
    if (dbl) {
        if (!cJSON_IsBool(dbl)) {
            err = "settings: \"outputEnable\" must be true or false";
            return ESP_ERR_INVALID_ARG;
        }
        outputEnable = cJSON_IsTrue(dbl);
    }

    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "universes");
    if (!cJSON_IsArray(arr)) {
        err = "missing \"universes\" array";
        return ESP_ERR_INVALID_ARG;
    }

    std::vector<UniverseRow> rows;
    std::vector<uint16_t> inputs;   // every expanded input universe, to detect overlaps
    int total = 0;
    int idx = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        idx++;
        std::string where = "row " + std::to_string(idx);
        if (!cJSON_IsObject(item)) {
            err = where + ": not an object";
            return ESP_ERR_INVALID_ARG;
        }

        UniverseRow r;
        int in, count, out;
        if (!get_int(item, "in", 0, CONFIG_ARTNET_MAX_UNI, &in, err, where) ||
            !get_int(item, "count", 1, CONFIG_MAX_UNIVERSES, &count, err, where)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (in + count - 1 > CONFIG_ARTNET_MAX_UNI) {
            err = where + ": input range goes past universe " + std::to_string(CONFIG_ARTNET_MAX_UNI);
            return ESP_ERR_INVALID_ARG;
        }

        const char *proto = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "proto"));
        if (proto && strcmp(proto, "artnet") == 0) {
            r.artnetOut = true;
            if (!get_int(item, "out", 0, CONFIG_ARTNET_MAX_UNI, &out, err, where)) return ESP_ERR_INVALID_ARG;
            if (out + count - 1 > CONFIG_ARTNET_MAX_UNI) {
                err = where + ": Art-Net output range goes past universe " + std::to_string(CONFIG_ARTNET_MAX_UNI);
                return ESP_ERR_INVALID_ARG;
            }
        } else if (proto && strcmp(proto, "sacn") == 0) {
            r.artnetOut = false;
            if (!get_int(item, "out", 1, CONFIG_SACN_MAX_UNI, &out, err, where)) return ESP_ERR_INVALID_ARG;
            if (out + count - 1 > CONFIG_SACN_MAX_UNI) {
                err = where + ": sACN output range goes past universe " + std::to_string(CONFIG_SACN_MAX_UNI);
                return ESP_ERR_INVALID_ARG;
            }
        } else {
            err = where + ": \"proto\" must be \"artnet\" or \"sacn\"";
            return ESP_ERR_INVALID_ARG;
        }

        const cJSON *addr = cJSON_GetObjectItemCaseSensitive(item, "addr");
        if (!parse_ip(cJSON_GetStringValue(addr), &r.outAddr, true)) {
            err = where + ": invalid destination IP (leave empty for broadcast/multicast)";
            return ESP_ERR_INVALID_ARG;
        }

        r.in = in;
        r.count = count;
        r.out = out;
        total += count;
        if (total > CONFIG_MAX_UNIVERSES) {
            err = "too many universes (max " + std::to_string(CONFIG_MAX_UNIVERSES) + ")";
            return ESP_ERR_INVALID_ARG;
        }
        for (int k = 0; k < count; k++) inputs.push_back(in + k);
        rows.push_back(r);
    }

    std::sort(inputs.begin(), inputs.end());
    auto dup = std::adjacent_find(inputs.begin(), inputs.end());
    if (dup != inputs.end()) {
        err = "input universe " + std::to_string(*dup) + " is used by more than one row";
        return ESP_ERR_INVALID_ARG;
    }

    cfg->framerate = framerate;
    cfg->sacnPriority = priority;
    cfg->doubleOn = doubleOn;
    cfg->constTrans = constTrans;
    cfg->outputEnable = outputEnable;
    cfg->universes = std::move(rows);
    return ESP_OK;
}

esp_err_t config_parse_shutters(const cJSON *root, AppConfig *cfg, std::string &err)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "shutters");
    if (!cJSON_IsArray(arr)) {
        err = "missing \"shutters\" array";
        return ESP_ERR_INVALID_ARG;
    }
    if (cJSON_GetArraySize(arr) > CONFIG_MAX_SHUTTERS) {
        err = "too many shutters (max " + std::to_string(CONFIG_MAX_SHUTTERS) + ")";
        return ESP_ERR_INVALID_ARG;
    }

    std::vector<ShutterRow> rows;
    int idx = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        idx++;
        std::string where = "shutter " + std::to_string(idx);
        if (!cJSON_IsObject(item)) {
            err = where + ": not an object";
            return ESP_ERR_INVALID_ARG;
        }

        ShutterRow s;
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "name"));
        s.name = name ? name : "";
        if (s.name.size() > CONFIG_MAX_NAME_LEN) s.name.resize(CONFIG_MAX_NAME_LEN);

        if (!parse_channel(cJSON_GetObjectItemCaseSensitive(item, "src"), &s.source, err,
                           where + " source")) {
            return ESP_ERR_INVALID_ARG;
        }

        const cJSON *dst = cJSON_GetObjectItemCaseSensitive(item, "dst");
        if (!cJSON_IsArray(dst)) {
            err = where + ": missing \"dst\" array";
            return ESP_ERR_INVALID_ARG;
        }
        if (cJSON_GetArraySize(dst) > CONFIG_MAX_DESTS) {
            err = where + ": too many destinations (max " + std::to_string(CONFIG_MAX_DESTS) + ")";
            return ESP_ERR_INVALID_ARG;
        }
        const cJSON *d;
        cJSON_ArrayForEach(d, dst) {
            ChannelRef c;
            if (!parse_channel(d, &c, err, where + " destination")) return ESP_ERR_INVALID_ARG;
            s.dests.push_back(c);
        }
        rows.push_back(std::move(s));
    }

    // Global checks: a destination channel may be driven by one shutter only,
    // and must not be the source channel of any shutter.
    std::vector<std::pair<uint32_t, int>> dests;  // channel key, shutter index
    std::vector<uint32_t> sources;
    for (size_t i = 0; i < rows.size(); i++) {
        sources.push_back(channel_key(rows[i].source));
        for (const ChannelRef &c : rows[i].dests) dests.push_back({channel_key(c), (int)i});
    }
    std::sort(dests.begin(), dests.end());
    std::sort(sources.begin(), sources.end());
    for (size_t i = 0; i < dests.size(); i++) {
        ChannelRef c = { (uint16_t)(dests[i].first / 512), (uint16_t)(dests[i].first % 512 + 1) };
        if (i > 0 && dests[i].first == dests[i - 1].first) {
            err = "destination " + channel_str(c) + " is used more than once (shutters " +
                  std::to_string(dests[i - 1].second + 1) + " and " +
                  std::to_string(dests[i].second + 1) + ")";
            return ESP_ERR_INVALID_ARG;
        }
        if (std::binary_search(sources.begin(), sources.end(), dests[i].first)) {
            err = "channel " + channel_str(c) + " is both a source and a destination";
            return ESP_ERR_INVALID_ARG;
        }
    }

    cfg->shutters = std::move(rows);
    return ESP_OK;
}

esp_err_t config_from_json(const cJSON *root, AppConfig *cfg, std::string &err)
{
    AppConfig tmp;
    config_defaults(&tmp);
    if (config_parse_network(root, &tmp, err) != ESP_OK) return ESP_ERR_INVALID_ARG;
    if (config_parse_io(root, &tmp, err) != ESP_OK) return ESP_ERR_INVALID_ARG;
    if (config_parse_shutters(root, &tmp, err) != ESP_OK) return ESP_ERR_INVALID_ARG;
    *cfg = std::move(tmp);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Flash storage
// ---------------------------------------------------------------------------

static esp_err_t mount_partition(void)
{
    static bool mounted = false;
    if (mounted) return ESP_OK;

    esp_err_t err = nvs_flash_init_partition(NVS_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Config partition needs to be erased (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase_partition(NVS_PARTITION));
        err = nvs_flash_init_partition(NVS_PARTITION);
    }
    if (err == ESP_OK) mounted = true;
    return err;
}

esp_err_t config_load(AppConfig *cfg)
{
    config_defaults(cfg);

    esp_err_t err = mount_partition();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot mount \"%s\" partition: %s (using defaults)",
                 NVS_PARTITION, esp_err_to_name(err));
        return err;
    }

    nvs_handle_t h;
    err = nvs_open_from_partition(NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No stored configuration, using defaults");
        return ESP_OK;
    }

    size_t len = 0;
    err = nvs_get_blob(h, NVS_KEY, NULL, &len);
    if (err != ESP_OK || len == 0) {
        nvs_close(h);
        ESP_LOGI(TAG, "No stored configuration, using defaults");
        return ESP_OK;
    }

    std::unique_ptr<char, decltype(&free)> buf((char *)malloc(len), &free);
    if (!buf) {
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_blob(h, NVS_KEY, buf.get(), &len);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Reading configuration failed: %s (using defaults)", esp_err_to_name(err));
        return err;
    }

    cJSON *root = cJSON_ParseWithLength(buf.get(), len);
    if (!root) {
        ESP_LOGE(TAG, "Stored configuration is not valid JSON (using defaults)");
        return ESP_ERR_INVALID_STATE;
    }
    std::string why;
    err = config_from_json(root, cfg, why);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Stored configuration rejected: %s (using defaults)", why.c_str());
        config_defaults(cfg);
        return err;
    }

    ESP_LOGI(TAG, "Loaded configuration: %u universe rows, %u shutters (%u bytes)",
             (unsigned)cfg->universes.size(), (unsigned)cfg->shutters.size(), (unsigned)len);
    return ESP_OK;
}

esp_err_t config_save(const AppConfig *cfg)
{
    esp_err_t err = mount_partition();
    if (err != ESP_OK) return err;

    cJSON *root = config_to_json(cfg);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    nvs_handle_t h;
    err = nvs_open_from_partition(NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, json, strlen(json));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Configuration saved (%u bytes)", (unsigned)strlen(json));
    } else {
        ESP_LOGE(TAG, "Saving configuration failed: %s", esp_err_to_name(err));
    }
    cJSON_free(json);
    return err;
}

// ---------------------------------------------------------------------------
// Applying to the running engine
// ---------------------------------------------------------------------------

void config_apply_io(const AppConfig *cfg)
{
    // Expand every row into one entry per universe
    int count = 0;
    for (const UniverseRow &r : cfg->universes) count += r.count;

    std::unique_ptr<uint16_t[]> inIds(new uint16_t[count > 0 ? count : 1]);
    std::unique_ptr<uint16_t[]> outIds(new uint16_t[count > 0 ? count : 1]);
    std::unique_ptr<in_addr[]>  outAddrs(new in_addr[count > 0 ? count : 1]);
    std::unique_ptr<bool[]>     isArtnet(new bool[count > 0 ? count : 1]);

    int i = 0;
    for (const UniverseRow &r : cfg->universes) {
        for (int k = 0; k < r.count; k++, i++) {
            inIds[i] = r.in + k;
            outIds[i] = r.out + k;
            outAddrs[i].s_addr = r.outAddr;
            isArtnet[i] = r.artnetOut;
        }
    }

    setFramerate(cfg->framerate);
    sacn_set_priority(cfg->sacnPriority);
    setDoubleOn(cfg->doubleOn);
    setContinuousTransmission(cfg->constTrans);
    setOutputEnable(cfg->outputEnable);
    reallocUniverses(inIds.get(), outIds.get(), outAddrs.get(), isArtnet.get(), count);
    ESP_LOGI(TAG, "Applied %d universes, %d fps, sACN priority %u",
             count, cfg->framerate, cfg->sacnPriority);
}

void config_apply_shutters(const AppConfig *cfg)
{
    const int count = (int)cfg->shutters.size();

    // fakeShutter wants 0-based DMX addresses
    std::vector<UniverseAddressPair> sources(count);
    std::vector<std::vector<UniverseAddressPair>> dests(count);
    std::vector<UniverseAddressPair *> destPtrs(count);
    std::vector<int> destCounts(count);

    for (int i = 0; i < count; i++) {
        const ShutterRow &s = cfg->shutters[i];
        sources[i].universe = s.source.universe;
        sources[i].address = s.source.address - 1;
        for (const ChannelRef &d : s.dests) {
            dests[i].push_back({ d.universe, (uint16_t)(d.address - 1) });
        }
        destPtrs[i] = dests[i].data();
        destCounts[i] = (int)dests[i].size();
    }

    reallocShutters(sources.data(), destPtrs.data(), destCounts.data(), count);
    ESP_LOGI(TAG, "Applied %d shutters", count);
}

esp_err_t config_apply_network(const AppConfig *cfg, esp_netif_t *netif)
{
    esp_netif_ip_info_t info = {};
    info.ip.addr = cfg->network.ip;
    info.netmask.addr = cfg->network.netmask;
    info.gw.addr = cfg->network.gateway;

    esp_netif_dhcpc_stop(netif);  // no-op if already stopped
    esp_err_t err = esp_netif_set_ip_info(netif, &info);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Network: IP " IPSTR "  mask " IPSTR "  gw " IPSTR,
                 IP2STR(&info.ip), IP2STR(&info.netmask), IP2STR(&info.gw));
    } else {
        ESP_LOGE(TAG, "Setting IP failed: %s", esp_err_to_name(err));
    }
    return err;
}
