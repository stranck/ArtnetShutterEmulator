#pragma once

#include "esp_netif.h"
#include "lwip/sockets.h"

#define EXTRA_FRAMERATE_WAIT (5 * 1000)
#define LED_GPIO  21
#define LED_ARTNET_FRAME 0,15,0
#define LED_FRAMERATE 0,0,25
#define LED_ERROR 30,0,0
#define NO_SIGNAL_TIMEOUT (5 * 1000 * 1000)

typedef struct {
    uint16_t universeId;
    uint8_t sequenceNo;     // last input sequence (Art-Net)
    uint8_t outSequenceNo;  // sequence of the frames we send
    bool dirty;
    uint16_t outUniId;
    struct in_addr outAddr;
    bool isArtnetOut;
    uint8_t data[512];
} DmxUniverse;

typedef struct {
    uint16_t universe;
    uint16_t address;
} UniverseAddressPair;

typedef struct {
    int counter;
    int startCounter;
    bool isOpen;

    bool changed;
    uint8_t *originalValues;

    //This is for reconstruct the following pointers
    UniverseAddressPair sourceValue_;
    UniverseAddressPair *destValues_;
    int destValuesCount;

    //This is for faster access
    uint8_t *sourceValue;
    uint8_t **destValues;
    bool **destDirtyFlags;
    int destDirtyFlagsCount;
} FakeShutter;

typedef enum {
    NOPE = 0,
    YES_ARTNET = 1,
    YES_FRAMERATE = 2
} ShouldRunTick;

void initFakeShutter(esp_netif_t *eth);

void setFramerate(int frameRate);

void reallocUniverses(uint16_t* universesInId, uint16_t* universesOutId, in_addr* universesOutAddr, bool* isArtnetOut, int count);

void reallocShutters(UniverseAddressPair *sourceValues, UniverseAddressPair **destValues, int *destValuesCount, int count);