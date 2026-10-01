#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Runs a function at a fixed period with microsecond accuracy.
//
// An esp_timer provides the timing; the function itself runs in a dedicated
// FreeRTOS task, so it may block, log, take mutexes, etc. without disturbing
// other timers in the system.

typedef void (*periodic_fn_t)(void *ctx);

typedef struct periodic_task_s *periodic_task_handle_t;

typedef struct {
    const char   *name;         // task / timer name (for debugging)
    uint64_t      period_us;    // period in microseconds (e.g. 3000 = 3 ms)
    periodic_fn_t fn;           // function to run every period
    void         *ctx;          // passed to fn
    uint32_t      stack_size;   // task stack in bytes
    UBaseType_t   priority;     // FreeRTOS priority (higher = more important)
    BaseType_t    core;         // 0, 1, or tskNO_AFFINITY
} periodic_task_config_t;

// Sensible defaults: 8 KB stack, priority 10, pinned to core 1 (away from networking)
#define PERIODIC_TASK_DEFAULT_CONFIG(_name, _period_us, _fn, _ctx) \
    { (_name), (_period_us), (_fn), (_ctx), 8192, 10, 1 }

// Create the task and start the timer. The first run happens one period from now.
esp_err_t periodic_task_start(const periodic_task_config_t *cfg, periodic_task_handle_t *out);

// Change the period while running (restarts the timer).
esp_err_t periodic_task_set_period(periodic_task_handle_t h, uint64_t period_us);

// Number of periods skipped because fn took longer than one period.
uint32_t periodic_task_overruns(periodic_task_handle_t h);

// Stop the timer and delete the task. The handle is invalid afterwards.
// Safe to call from inside fn itself (the task exits once fn returns).
void periodic_task_stop(periodic_task_handle_t h);
