#include "fakeShutter.h"

#include <string.h>
#include <sys/param.h>
#include "artnet.h"
#include "sacn.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "periodic_task.h"
#include "led_strip.h"



static const char *TAG = "fake-shutter";

DmxUniverse **_universes = nullptr;
int _universeCount = 0;

FakeShutter *_shutters = nullptr;
int _shuttersCount = 0;

bool _doubleOn = false;
bool _continuousTransmission = false;

int64_t _lastPacketOut = 0;
int64_t _lastTickRun = 0;
int64_t _deltaTimeFramerateMicro = 0;

static led_strip_handle_t debugLed;

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

static int getUniverseIndex(uint16_t universe) {
    for (int i = 0; i < _universeCount; i++) {
        if (_universes[i]->universeId == universe) return i;
    }
    return -1;
}

inline ShouldRunTick shouldRunTick(int64_t currentTime) {
    //We are allowed to send only if enough time has passed since the last out frame, or all universes are dirty
    if ((currentTime - _lastTickRun) <= _deltaTimeFramerateMicro) {
        bool allDirty = true;
        //ESP_LOGI(TAG, "RunTick: Universe count %d", _universeCount);
        for (int i = 0; i < _universeCount; i++) {
            if (!_universes[i]->dirty) {
                allDirty = false;
                break;
            }
        }
        //ESP_LOGI(TAG, "RunTick: allDirty = %d", allDirty);
        if (!allDirty) return NOPE;
        //ESP_LOGI(TAG, "RunTick: Running for all uni received");
        _lastTickRun = currentTime + EXTRA_FRAMERATE_WAIT; //If we're sending because all universes are dirty, wait a bit more to stay synchronized with the source
        return YES_ARTNET;
    } else {
        //ESP_LOGI(TAG, "RunTick: Running for timeframe");
        _lastTickRun = currentTime;
        return YES_FRAMERATE;
    }
}

static void runShutterTick(void *ctx) {
    if (xSemaphoreTakeRecursive(configSemaphore, 0) != pdTRUE) return;
    int64_t currentTime = esp_timer_get_time();
    ShouldRunTick shouldRunTickRes = shouldRunTick(currentTime);
    if (!shouldRunTickRes) {
        xSemaphoreGiveRecursive(configSemaphore);
        return;
    }
    

    for (int i = 0; i < _shuttersCount; i++) {
        FakeShutter *shutter = &_shutters[i];
        if (shutter->sourceValue) {
            uint8_t val = *shutter->sourceValue;
            if (val >= 200) { //200+ is shutter open
                shutter->counter = -1;
                shutter->isOpen = true;
            } else {
                if (shutter->isOpen) {
                    //Set dirty flags
                    for (int j = 0; j < shutter->destDirtyFlagsCount; j++) *shutter->destDirtyFlags[j] = true;
                    int originalCounter = shutter->counter;
                    if (!_doubleOn || originalCounter != 0) { 
                        //Next tick will be closed
                        shutter->isOpen = false;
                    }
                    //Reroll counter
                    if (val < 100) { //Sync strobe
                        int newCount = 100 - val;
                        shutter->counter = newCount;
                        shutter->startCounter = newCount;
                    } else { //Rand strobe
                        int newCount = (esp_random() % (201 - val)) + 2;
                        shutter->counter = newCount;
                        shutter->startCounter = newCount;
                    }
                    if (_doubleOn && shutter->counter == 1 && shutter->startCounter == 1) {
                        shutter->counter = 2;
                        shutter->startCounter = 2;
                    }
                    //ESP_LOGI(TAG, "Open. NewCounter: %d OrgCounter: %d Val: %d", shutter->counter, originalCounter, val);
                    //if we come from a negative counter, it means we're coming from a shutter open. Go directly to a black round
                    if (originalCounter < 0) goto RunShutterTick_ShutterClosed; //ugly ass btw
                } else {
                    val = val >= 100 ? val - 100 : val;
                    shutter->counter = MIN(shutter->counter, (100 - val + 1));
                    RunShutterTick_ShutterClosed:
                    //Black out dest values
                    shutter->changed = true;
                    for (int j = 0; j < shutter->destValuesCount; j++) {
                        if (!shutter->destValues[j]) continue;  // NULL = universe not configured
                        shutter->originalValues[j] = *shutter->destValues[j];
                        *shutter->destValues[j] = 0;
                    }
                    //If it's the first tick we're black, set dirty flags
                    if (shutter->counter == shutter->startCounter) {
                        for (int j = 0; j < shutter->destDirtyFlagsCount; j++) *shutter->destDirtyFlags[j] = true;
                    }
                    //Decrease the counter. If it reached 0, next tick it will be open
                    //ESP_LOGI(TAG, "Closed. NewCounter: %d StartCounter: %d Eq: %d", (shutter->counter - 1), shutter->startCounter, (shutter->counter == shutter->startCounter));
                    if (--shutter->counter <= 0) shutter->isOpen = true;
                }
            }
        } 
    }


    bool atLeastOneUniverse = false;
    //Send universes and clean dirty flags
    for (int i = 0; i < _universeCount; i++) {
        DmxUniverse *uni = _universes[i];
        if (uni->dirty || _continuousTransmission) {
            struct in_addr *addr = &uni->outAddr;
            if (addr->s_addr == 0) addr = NULL;
            // Output has its own sequence: frames generated by the shutter (with no new input packet) would otherwise repeat the input's number and be dropped
            //ESP_LOGI(TAG, "Artnet send %d", uni->data[1]);
            uint8_t seq = ++uni->outSequenceNo;
            if (uni->isArtnetOut) {
                if (seq == 0) seq = ++uni->outSequenceNo;  // 0 means "no sequencing" in Art-Net
                artnet_send_dmx(uni->outUniId, uni->data, 512, seq, addr);
            } else {
                sacn_send_dmx(uni->outUniId, seq, uni->data, 512, addr);
            }
            if (uni->dirty) {
                atLeastOneUniverse = true;
            }
            uni->dirty = false;
        }
    }

    if(atLeastOneUniverse) {
        //Update last packet sent time
        _lastPacketOut = currentTime;

        //Update led color
        if (shouldRunTickRes == YES_FRAMERATE) {
            led_strip_set_pixel(debugLed, 0, LED_FRAMERATE);
        } else {
            led_strip_set_pixel(debugLed, 0, LED_ARTNET_FRAME);
        }
        led_strip_refresh(debugLed);
        
        //Restore changed values
        for (int i = 0; i < _shuttersCount; i++) {
            FakeShutter *shutter = &_shutters[i];
            if (shutter->changed) {
                shutter->changed = false;
                for (int j = 0; j < shutter->destValuesCount; j++) {
                    if (shutter->destValues[j]) *shutter->destValues[j] = shutter->originalValues[j];
                }
            }
        }
    } else {
        if(shouldRunTickRes == YES_FRAMERATE) {
            if ((currentTime - _lastPacketOut) > NO_SIGNAL_TIMEOUT) {
                led_strip_set_pixel(debugLed, 0, LED_ERROR);
                led_strip_refresh(debugLed);
            }
        }
    }

    xSemaphoreGiveRecursive(configSemaphore);
}



static void onArtnetReceive(uint16_t universeId, const uint8_t *data, uint16_t length, uint8_t sequence, uint32_t src_ip) {
    xSemaphoreTakeRecursive(configSemaphore, portMAX_DELAY);
    DmxUniverse* universe = getUniverse(universeId);
    if (!universe) goto FakeShutter_OnArtnetReceive_Exit;
    if (sequence < universe->sequenceNo && (universe->sequenceNo - sequence) < 127) goto FakeShutter_OnArtnetReceive_Exit;

    memcpy(universe->data, data, MIN(length, 512));
    universe->sequenceNo = sequence;
    universe->dirty = true;

    //ESP_LOGI(TAG, "Artnet received");

    FakeShutter_OnArtnetReceive_Exit:
    xSemaphoreGiveRecursive(configSemaphore);
}

void initFakeShutter(esp_netif_t *eth){
    configSemaphore = xSemaphoreCreateRecursiveMutexStatic(&configSemaphore_buffer);

    artnet_config_t artnet_cfg = {};
    artnet_cfg.netif      = eth;
    artnet_cfg.short_name = "ShutterEmulator";
    artnet_cfg.long_name  = "ShutterEmulator - ESP32-S3-ETH Art-Net node";
    ESP_ERROR_CHECK(artnet_init(&artnet_cfg, onArtnetReceive));

    sacn_config_t sacn_cfg = SACN_DEFAULT_CONFIG("ShutterEmulator");
    ESP_ERROR_CHECK(sacn_init(&sacn_cfg));

    periodic_task_config_t cfg = PERIODIC_TASK_DEFAULT_CONFIG("shutterTick", 1000, runShutterTick, NULL);
    ESP_ERROR_CHECK(periodic_task_start(&cfg, &shutterTick_task));

    led_strip_config_t strip = {};
    strip.strip_gpio_num = LED_GPIO;
    strip.max_leds = 1;
    strip.led_model = LED_MODEL_WS2812;
    strip.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

    led_strip_rmt_config_t rmt = {};
    rmt.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt.resolution_hz = 10 * 1000 * 1000;   // 10 MHz

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip, &rmt, &debugLed));
    led_strip_set_pixel(debugLed, 0, LED_ERROR);
    led_strip_refresh(debugLed);
}



void setFramerate(int frameRate) {
    if (frameRate < 1) frameRate = 1;
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
            int uniIdx = getUniverseIndex(shutter->destValues_[j].universe);
            if (uniIdx >= 0) {
                universeUsed[uniIdx] = true;
                destValues[j] = &_universes[uniIdx]->data[shutter->destValues_[j].address];
            }
        }

        int dirtyFlagCount = 0;
        for (int j = 0; j < _universeCount; j++) if(universeUsed[j]) dirtyFlagCount++;
        bool **dirtyFlags = (bool **) calloc(dirtyFlagCount, sizeof(bool*));
        for (int j = 0, dirtyFlagsIdx = 0; j < _universeCount; j++) {
            if(universeUsed[j]) {
                dirtyFlags[dirtyFlagsIdx++] = &_universes[j]->dirty;
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

            uint8_t *originalValues = shutter->originalValues;
            shutter->originalValues = nullptr;
            if (originalValues) free(originalValues);
        }
        free(shutters);
    }

    FakeShutter *shutters = (FakeShutter*) calloc(count, sizeof(FakeShutter));
    for (int i = 0; i < count; i++) {
        FakeShutter *shutter = &shutters[i];

        
        size_t destValuesSize = destValuesCount[i] * sizeof(UniverseAddressPair);
        shutter->destValues_ = (UniverseAddressPair*) malloc(destValuesSize);
        memcpy(shutter->destValues_, destValues[i], destValuesSize);
        
        shutter->sourceValue_ = sourceValues[i];
        shutter->destValuesCount = destValuesCount[i];
        shutter->originalValues = (uint8_t*) malloc(destValuesCount[i] * sizeof(uint8_t));
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
    for (int i = 0; i < count; i++) storedUniverses[i] = false;
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
        universes[i] = (DmxUniverse*) calloc(1, sizeof(DmxUniverse));
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

void setDoubleOn(bool doubleOn) {
    _doubleOn = doubleOn;
}

void setContinuousTransmission(bool continuousTranmission) {
    _continuousTransmission = continuousTranmission;
}