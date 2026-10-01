#include "fakeShutter.h"

#include <sys/param.h>
#include "artnet.h"
#include "sacn.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "periodic_task.h"


static const char *TAG = "fake-shutter";

DmxUniverse **_universes = nullptr;
int _universeCount = 0;

FakeShutter *_shutters = nullptr;
int _shuttersCount = 0;

int64_t _lastPacketOut = 0;
int64_t _deltaTimeFramerateMicro = 0;

SemaphoreHandle_t configSemaphore;
StaticSemaphore_t configSemaphore_buffer;
periodic_task_handle_t shutterTick_task;


DmxUniverse* getUniverse(uint16_t universe) {
    //This could've improved with a binary search
    for (int i = 0; i < _universeCount; i++) {
        if (_universes[i]->universeId == universe) {
            return _universes[i];
        }
    }
    return nullptr;
}

inline bool shouldRunTick() {
    int64_t currentTime = esp_timer_get_time();
    //We are allowed to send only if enough time has passed since the last out frame, or all universes are dirty
    if ((currentTime - _lastPacketOut) <= _deltaTimeFramerateMicro) {
        bool allDirty = true;
        for (int i = 0; i < _universeCount; i++) {
            if (!_universes[i]->dirty) {
                allDirty = false;
                break;
            }
        }
        if (!allDirty) return false;
        _lastPacketOut = currentTime + EXTRA_FRAMERATE_WAIT; //If we're sending because all universes are dirty, wait a bit more to stay synchronized with the source
    } else {
        _lastPacketOut = currentTime;
    }
    return true;
}

static void runShutterTick(void *ctx) {
    if(!shouldRunTick()) return;
    if (xSemaphoreTake(configSemaphore, 0) != pdTRUE) return;

    for (int i = 0; i < _shuttersCount; i++) {
        FakeShutter *shutter = &_shutters[i];
        if (shutter->sourceValue) {
            uint8_t val = *shutter->sourceValue;
            if (val > 200) { //200+ is shutter open
                shutter->counter = -1;
                shutter->isOpen = true;
            } else {
                if (shutter->isOpen) {
                    //Set dirty flags
                    for (int j = 0; j < shutter->destDirtyFlagsCount; j++) *shutter->destDirtyFlags[j] = true;
                    //Next tick will be closed
                    shutter->isOpen = false;
                    int originalCounter = shutter->counter;
                    //Reroll counter
                    if (val < 100) { //Sync strobe
                        int newCount = 100 - val;
                        shutter->counter = newCount;
                        shutter->startCounter = newCount;
                    } else { //Rand strobe
                        int newCount = (esp_random() % (200 - val)) + 1;
                        shutter->counter = newCount;
                        shutter->startCounter = newCount;
                    }
                    //if we come from a negative counter, it means we're coming from a shutter open. Go directly to a black round
                    if (originalCounter < 0) goto RunShutterTick_ShutterClosed; //ugly ass btw
                } else {
                    val = val > 100 ? val - 100 : val;
                    shutter->counter = MIN(shutter->counter, val);
                    RunShutterTick_ShutterClosed:
                    //Black out dest values
                    for (int j = 0; j < shutter->destValuesCount; j++) *shutter->destValues[j] = 0;
                    //If it's the first tick we're black, set dirty flags
                    if (shutter->counter == shutter->startCounter) for (int j = 0; j < shutter->destDirtyFlagsCount; j++) *shutter->destDirtyFlags[j] = true;
                    //Decrease the counter. If it reached 0, next tick it will be open
                    if (--shutter->counter <= 0) shutter->isOpen = true;
                }
            }
        } 
    }


    //Send universes and clean dirty flags
    for (int i = 0; i < _universeCount; i++) {
        DmxUniverse *uni = _universes[i];
        if (uni->dirty) {
            struct in_addr *addr = &uni->outAddr;
            if (addr->s_addr == 0) addr = NULL;
            if (uni->isArtnetOut) {
                artnet_send_dmx(uni->outUniId, uni->data, 512, uni->sequenceNo, addr);
            } else {
                sacn_send_dmx(uni->outUniId, uni->sequenceNo, uni->data, 512, addr);
            }
            uni->dirty = false;
        }
    }

    xSemaphoreGive(configSemaphore);
}



static void onArtnetReceive(uint16_t universeId, const uint8_t *data, uint16_t length, uint8_t sequence, uint32_t src_ip) {
    xSemaphoreTake(configSemaphore, portMAX_DELAY);
    DmxUniverse* universe = getUniverse(universeId);
    if (!universe) goto FakeShutter_OnArtnetReceive_Exit;
    if (sequence < universe->sequenceNo && (universe->sequenceNo - sequence) < 127) goto FakeShutter_OnArtnetReceive_Exit;

    memcpy(universe->data, data, MIN(length, 512));
    universe->sequenceNo = sequence;
    universe->dirty = true;

    FakeShutter_OnArtnetReceive_Exit:
    xSemaphoreGive(configSemaphore);
}

void initFakeShutter(esp_netif_t *eth){
    artnet_config_t artnet_cfg = {};
    artnet_cfg.netif      = eth;
    artnet_cfg.short_name = "ShutterEmulator";
    artnet_cfg.long_name  = "ShutterEmulator - ESP32-S3-ETH Art-Net node";
    ESP_ERROR_CHECK(artnet_init(&artnet_cfg, onArtnetReceive));

    sacn_config_t sacn_cfg = SACN_DEFAULT_CONFIG("ShutterEmulator");
    ESP_ERROR_CHECK(sacn_init(&sacn_cfg));

    periodic_task_config_t cfg = PERIODIC_TASK_DEFAULT_CONFIG("shutterTick", 3000, runShutterTick, NULL);
    ESP_ERROR_CHECK(periodic_task_start(&cfg, &shutterTick_task));

    configSemaphore = xSemaphoreCreateMutexStatic(&configSemaphore_buffer);
}



void setFramerate(int frameRate) {
    _deltaTimeFramerateMicro = (1000 * 1000) / frameRate; 
}

void clearFakeShutterCache(FakeShutter* shutter) {
    xSemaphoreTakeRecursive(configSemaphore, portMAX_DELAY);
    shutter->sourceValue = nullptr;
    
    uint8_t **destValues = shutter->destValues;
    shutter->destValues = nullptr;
    if (destValues) free(destValues);

    bool **dirtyFlags = shutter->destDirtyFlags;
    shutter->destDirtyFlags = nullptr;
    if (dirtyFlags) free(dirtyFlags);
    xSemaphoreGiveRecursive(configSemaphore);
}
void clearFakeShutterCache() {
    xSemaphoreTakeRecursive(configSemaphore, portMAX_DELAY);
    for (int i = 0; i < _shuttersCount; i++) {
        clearFakeShutterCache(&_shutters[i]);
    }
    xSemaphoreGiveRecursive(configSemaphore);
}

void rebuildFakeShutterCache(FakeShutter* shutter) {
    xSemaphoreTakeRecursive(configSemaphore, portMAX_DELAY);
    clearFakeShutterCache(shutter);
    bool *universeUsed = (bool*) alloca(_universeCount * sizeof(bool));
    DmxUniverse *uni = getUniverse(shutter->sourceValue_.universe);
    if (uni) {
        for (int j = 0; j < _universeCount; j++) universeUsed[j] = false;

        uint8_t **destValues = (uint8_t **) calloc(shutter->destValuesCount, sizeof(uint8_t*));
        for (int j = 0; j < shutter->destValuesCount; j++) {
            uint16_t uniId = shutter->destValues_[j].universe;
            DmxUniverse *destUni = getUniverse(uniId);
            if (destUni) {
                universeUsed[uniId] = true;
                destValues[j] = &destUni->data[shutter->destValues_[j].address];
            }
        }

        int dirtyFlagCount = 0;
        for (int j = 0; j < _universeCount; j++) if(universeUsed[j]) dirtyFlagCount++;
        bool **dirtyFlags = (bool **) calloc(dirtyFlagCount, sizeof(bool*));
        for (int j = 0, dirtyFlagsIdx = 0; j < _universeCount; j++) {
            if(universeUsed[j]) {
                DmxUniverse *destUni = getUniverse(j);
                if (destUni) {
                    dirtyFlags[dirtyFlagsIdx++] = &destUni->dirty;
                }
            }
        }
        
        shutter->destDirtyFlags = dirtyFlags;
        shutter->destDirtyFlagsCount = dirtyFlagCount;
        shutter->destValues = destValues;
        shutter->sourceValue = &uni->data[shutter->sourceValue_.address];
    }
    xSemaphoreGiveRecursive(configSemaphore);
}
void rebuildFakeShutterCache() {
    xSemaphoreTakeRecursive(configSemaphore, portMAX_DELAY);
    clearFakeShutterCache();
    for (int i = 0; i < _shuttersCount; i++) {
        rebuildFakeShutterCache(&_shutters[i]);
    }
    xSemaphoreGiveRecursive(configSemaphore);
}
void reallocShutters(UniverseAddressPair *sourceValues, UniverseAddressPair **destValues, int *destValuesCount, int count) {
    xSemaphoreTakeRecursive(configSemaphore, portMAX_DELAY);
    //Deallocate old first
    int shutterCount = _shuttersCount;
    _shuttersCount = 0;
    if (_shutters) {
        FakeShutter *shutters = _shutters;
        _shutters = nullptr;
        for (int i = 0; i < shutterCount; i++) {
            FakeShutter *shutter = &shutters[i];
            clearFakeShutterCache(shutter);
            UniverseAddressPair *destValues = shutter->destValues_;
            shutter->destValues_ = nullptr;
            if (destValues) free(destValues);
        }
        free(shutters);
    }

    FakeShutter *shutters = (FakeShutter*) calloc(count, sizeof(FakeShutter));
    for (int i = 0; i < count; i++) {
        FakeShutter *shutter = &shutters[i];
        shutter->sourceValue_ = sourceValues[i];
        size_t destValuesSize = destValuesCount[i] * sizeof(UniverseAddressPair);
        shutter->destValues_ = (UniverseAddressPair*) malloc(destValuesSize);
        memcpy(shutter->destValues_, destValues[i], destValuesSize);
        rebuildFakeShutterCache(shutter);
    }
    _shutters = shutters;
    _shuttersCount = count;
    xSemaphoreGiveRecursive(configSemaphore);
}


void reallocUniverses(uint16_t* universesInId, uint16_t* universesOutId, in_addr* universesOutAddr, bool* isArtnetOut, int count) {
    xSemaphoreTakeRecursive(configSemaphore, portMAX_DELAY);
    //Deallocate old
    int oldUniCount = _universeCount;
    _universeCount = 0;
    clearFakeShutterCache();
    if (_universes) {
        for (int i = 0; i < oldUniCount; i++) {
            if (_universes[i]) {
                DmxUniverse* uni = _universes[i];
                _universes[i] = nullptr;
                if (uni) free(uni);
            }
        }
        DmxUniverse **universes = _universes;
        _universes = nullptr;
        free(universes);
    }

    DmxUniverse **universes = (DmxUniverse**) calloc(count, sizeof(DmxUniverse*));
    bool *storedUniverses = (bool*) alloca(count * sizeof(bool));
    for (int i = 0; i < count; i++) { //O(n^2) insertion sort but n should be low
        int minIndex = -1;
        uint16_t min = 0xffff;
        for (int j = 0; j < count; j++) {
            if(!storedUniverses[j] && universesInId[j] < min) {
                min = universesInId[j];
                minIndex = j;
            }
        }
        if (minIndex == -1) break;
        storedUniverses[minIndex] = true;
        universes[i] = (DmxUniverse*) malloc(sizeof(DmxUniverse));
        universes[i]->dirty = false;
        universes[i]->universeId = universesInId[minIndex];
        universes[i]->outAddr = universesOutAddr[minIndex];
        universes[i]->outUniId = universesOutId[minIndex];
        universes[i]->isArtnetOut = isArtnetOut[minIndex];
    }
    _universes = universes;
    _universeCount = count;
    rebuildFakeShutterCache();
    xSemaphoreGiveRecursive(configSemaphore);
}