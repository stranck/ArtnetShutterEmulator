#include "periodic_task.h"

#include <stdlib.h>
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "periodic_task";

struct periodic_task_s {
    esp_timer_handle_t timer;
    TaskHandle_t       task;
    periodic_fn_t      fn;
    void              *ctx;
    volatile uint32_t  overruns;
    volatile bool      stopping;
};

// Runs in the shared esp_timer task: only wake our own task, nothing else.
static void timer_cb(void *arg)
{
    periodic_task_s *t = (periodic_task_s *)arg;
    xTaskNotifyGive(t->task);
}

static void task_main(void *arg)
{
    periodic_task_s *t = (periodic_task_s *)arg;

    while (true) {
        uint32_t pending = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (t->stopping) break;
        if (pending > 1) t->overruns += pending - 1;  // fn was too slow, periods were missed
        t->fn(t->ctx);
        if (t->stopping) break;
    }

    free(t);
    vTaskDelete(NULL);
}

esp_err_t periodic_task_start(const periodic_task_config_t *cfg, periodic_task_handle_t *out)
{
    if (!cfg || !cfg->fn || cfg->period_us == 0 || !out) return ESP_ERR_INVALID_ARG;

    periodic_task_s *t = (periodic_task_s *)calloc(1, sizeof(periodic_task_s));
    if (!t) return ESP_ERR_NO_MEM;
    t->fn = cfg->fn;
    t->ctx = cfg->ctx;

    const char *name = cfg->name ? cfg->name : "periodic";

    if (xTaskCreatePinnedToCore(task_main, name, cfg->stack_size, t,
                                cfg->priority, &t->task, cfg->core) != pdPASS) {
        free(t);
        return ESP_ERR_NO_MEM;
    }

    esp_timer_create_args_t args = {};
    args.callback = timer_cb;
    args.arg = t;
    args.name = name;
    esp_err_t err = esp_timer_create(&args, &t->timer);
    if (err == ESP_OK) err = esp_timer_start_periodic(t->timer, cfg->period_us);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: timer setup failed: %s", name, esp_err_to_name(err));
        if (t->timer) esp_timer_delete(t->timer);
        t->stopping = true;           // let the task free itself
        xTaskNotifyGive(t->task);
        return err;
    }

    *out = t;
    return ESP_OK;
}

esp_err_t periodic_task_set_period(periodic_task_handle_t h, uint64_t period_us)
{
    if (!h || period_us == 0) return ESP_ERR_INVALID_ARG;
    return esp_timer_restart(h->timer, period_us);
}

uint32_t periodic_task_overruns(periodic_task_handle_t h)
{
    return h ? h->overruns : 0;
}

void periodic_task_stop(periodic_task_handle_t h)
{
    if (!h) return;
    esp_timer_stop(h->timer);
    esp_timer_delete(h->timer);
    h->stopping = true;
    xTaskNotifyGive(h->task);  // wake the task so it can exit and free itself
}
