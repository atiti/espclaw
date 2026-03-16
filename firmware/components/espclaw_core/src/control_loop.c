#include "espclaw/control_loop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#else
#include <pthread.h>
#endif

#include "espclaw/app_runtime.h"
#include "espclaw/hardware.h"
#include "espclaw/task_policy.h"

typedef struct {
    espclaw_control_loop_status_t status;
    char workspace_root[512];
    char payload[ESPCLAW_CONTROL_LOOP_PAYLOAD_MAX];
#ifdef ESP_PLATFORM
    TaskHandle_t task;
    SemaphoreHandle_t lock;
#else
    pthread_t thread;
    pthread_mutex_t lock;
    bool thread_created;
#endif
    bool lock_ready;
} espclaw_control_loop_slot_t;

static espclaw_control_loop_slot_t s_loops[ESPCLAW_CONTROL_LOOP_MAX];

static int loop_lock_init(espclaw_control_loop_slot_t *slot)
{
    if (slot == NULL) {
        return -1;
    }
    if (slot->lock_ready) {
        return 0;
    }

#ifdef ESP_PLATFORM
    slot->lock = xSemaphoreCreateMutex();
    if (slot->lock == NULL) {
        return -1;
    }
#else
    if (pthread_mutex_init(&slot->lock, NULL) != 0) {
        return -1;
    }
#endif

    slot->lock_ready = true;
    return 0;
}

static void loop_lock(espclaw_control_loop_slot_t *slot)
{
    if (slot == NULL || !slot->lock_ready) {
        return;
    }
#ifdef ESP_PLATFORM
    xSemaphoreTake(slot->lock, portMAX_DELAY);
#else
    pthread_mutex_lock(&slot->lock);
#endif
}

static void loop_unlock(espclaw_control_loop_slot_t *slot)
{
    if (slot == NULL || !slot->lock_ready) {
        return;
    }
#ifdef ESP_PLATFORM
    xSemaphoreGive(slot->lock);
#else
    pthread_mutex_unlock(&slot->lock);
#endif
}

static void copy_status_locked(const espclaw_control_loop_slot_t *slot, espclaw_control_loop_status_t *status)
{
    if (slot == NULL || status == NULL) {
        return;
    }
    *status = slot->status;
}

static int json_escape_copy(const char *input, char *output, size_t output_size)
{
    size_t written = 0;

    if (output == NULL || output_size == 0) {
        return -1;
    }

    while (input != NULL && *input != '\0' && written + 1 < output_size) {
        const char *replacement = NULL;

        switch (*input) {
        case '\\':
            replacement = "\\\\";
            break;
        case '"':
            replacement = "\\\"";
            break;
        case '\n':
            replacement = "\\n";
            break;
        case '\r':
            replacement = "\\r";
            break;
        case '\t':
            replacement = "\\t";
            break;
        default:
            break;
        }

        if (replacement != NULL) {
            size_t replacement_len = strlen(replacement);
            if (written + replacement_len >= output_size) {
                break;
            }
            memcpy(output + written, replacement, replacement_len);
            written += replacement_len;
        } else {
            output[written++] = *input;
        }
        input++;
    }

    output[written] = '\0';
    return 0;
}

static int find_loop_index(const char *loop_id)
{
    size_t index;

    if (loop_id == NULL || loop_id[0] == '\0') {
        return -1;
    }

    for (index = 0; index < ESPCLAW_CONTROL_LOOP_MAX; ++index) {
        if (s_loops[index].status.loop_id[0] != '\0' &&
            strcmp(s_loops[index].status.loop_id, loop_id) == 0) {
            return (int)index;
        }
    }

    return -1;
}

static int find_free_loop_index(void)
{
    size_t index;

    for (index = 0; index < ESPCLAW_CONTROL_LOOP_MAX; ++index) {
        if (!s_loops[index].status.active) {
            return (int)index;
        }
    }

    return -1;
}

static void mark_loop_finished(espclaw_control_loop_slot_t *slot, int last_status, const char *result)
{
    if (slot == NULL) {
        return;
    }

    loop_lock(slot);
    slot->status.active = false;
    slot->status.completed = true;
    slot->status.last_status = last_status;
    slot->status.last_finished_ms = espclaw_hw_ticks_ms();
    snprintf(slot->status.last_result, sizeof(slot->status.last_result), "%s", result != NULL ? result : "");
    loop_unlock(slot);
}

static void run_loop_worker(espclaw_control_loop_slot_t *slot)
{
    espclaw_app_vm_t *vm = NULL;
    char result[ESPCLAW_CONTROL_LOOP_RESULT_MAX];
    uint64_t next_deadline_ms = 0;
    uint32_t completed = 0;
    int open_status;

    if (slot == NULL) {
        return;
    }

    open_status = espclaw_app_vm_open(
        slot->workspace_root,
        slot->status.app_id,
        &vm,
        result,
        sizeof(result)
    );
    if (open_status != 0) {
        mark_loop_finished(slot, -1, result);
        return;
    }

    next_deadline_ms = espclaw_hw_ticks_ms();
    while (true) {
        uint64_t now_ms = espclaw_hw_ticks_ms();
        uint32_t period_ms;
        uint32_t max_iterations;
        bool stop_requested;
        int step_status;

        loop_lock(slot);
        stop_requested = slot->status.stop_requested;
        period_ms = slot->status.period_ms;
        max_iterations = slot->status.max_iterations;
        loop_unlock(slot);

        if (stop_requested || (max_iterations > 0 && completed >= max_iterations)) {
            break;
        }

        if (now_ms < next_deadline_ms) {
            espclaw_hw_sleep_ms((uint32_t)(next_deadline_ms - now_ms));
        }

        loop_lock(slot);
        slot->status.last_started_ms = espclaw_hw_ticks_ms();
        loop_unlock(slot);

        step_status = espclaw_app_vm_step(
            vm,
            slot->status.trigger,
            slot->payload,
            result,
            sizeof(result)
        );

        loop_lock(slot);
        completed = ++slot->status.iterations_completed;
        slot->status.last_status = step_status;
        slot->status.last_finished_ms = espclaw_hw_ticks_ms();
        snprintf(slot->status.last_result, sizeof(slot->status.last_result), "%s", result);
        stop_requested = slot->status.stop_requested;
        max_iterations = slot->status.max_iterations;
        period_ms = slot->status.period_ms;
        loop_unlock(slot);

        if (step_status != 0 || stop_requested || (max_iterations > 0 && completed >= max_iterations)) {
            break;
        }

        next_deadline_ms += period_ms;
        if (next_deadline_ms < espclaw_hw_ticks_ms()) {
            next_deadline_ms = espclaw_hw_ticks_ms() + period_ms;
        }
    }

    espclaw_app_vm_close(vm);
    mark_loop_finished(slot, slot->status.last_status, slot->status.last_result);
}

#ifdef ESP_PLATFORM
static void control_loop_task(void *argument)
{
    espclaw_control_loop_slot_t *slot = (espclaw_control_loop_slot_t *)argument;

    run_loop_worker(slot);
    if (slot != NULL) {
        slot->task = NULL;
    }
    vTaskDelete(NULL);
}
#else
static void *control_loop_thread(void *argument)
{
    espclaw_control_loop_slot_t *slot = (espclaw_control_loop_slot_t *)argument;

    run_loop_worker(slot);
    return NULL;
}
#endif

int espclaw_control_loop_start(
    const char *loop_id,
    const char *workspace_root,
    const char *app_id,
    const char *trigger,
    const char *payload,
    uint32_t period_ms,
    uint32_t max_iterations,
    char *buffer,
    size_t buffer_size
)
{
    espclaw_control_loop_slot_t *slot = NULL;
    int index;

    if (buffer != NULL && buffer_size > 0) {
        buffer[0] = '\0';
    }
    if (loop_id == NULL || workspace_root == NULL || app_id == NULL || trigger == NULL || period_ms == 0) {
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "invalid control loop arguments");
        }
        return -1;
    }

    index = find_loop_index(loop_id);
    if (index >= 0) {
        if (s_loops[index].status.active) {
            if (buffer != NULL && buffer_size > 0) {
                snprintf(buffer, buffer_size, "loop %s is already active", loop_id);
            }
            return -1;
        }
    } else {
        index = find_free_loop_index();
        if (index < 0) {
            if (buffer != NULL && buffer_size > 0) {
                snprintf(buffer, buffer_size, "no control loop slots available");
            }
            return -1;
        }
    }

    slot = &s_loops[index];
    if (loop_lock_init(slot) != 0) {
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "failed to initialize loop lock");
        }
        return -1;
    }

    loop_lock(slot);
    memset(&slot->status, 0, sizeof(slot->status));
    snprintf(slot->status.loop_id, sizeof(slot->status.loop_id), "%s", loop_id);
    snprintf(slot->status.app_id, sizeof(slot->status.app_id), "%s", app_id);
    snprintf(slot->status.trigger, sizeof(slot->status.trigger), "%s", trigger);
    slot->status.active = true;
    slot->status.completed = false;
    slot->status.stop_requested = false;
    slot->status.period_ms = period_ms;
    slot->status.max_iterations = max_iterations;
    slot->status.started_ms = espclaw_hw_ticks_ms();
    snprintf(slot->workspace_root, sizeof(slot->workspace_root), "%s", workspace_root);
    snprintf(slot->payload, sizeof(slot->payload), "%s", payload != NULL ? payload : "");
    loop_unlock(slot);

#ifdef ESP_PLATFORM
    int core = espclaw_task_policy_core_for(ESPCLAW_TASK_KIND_CONTROL_LOOP);

    if (xTaskCreatePinnedToCore(
            control_loop_task,
            "espclaw_loop",
            8192,
            slot,
            5,
            &slot->task,
            core >= 0 ? core : tskNO_AFFINITY) != pdPASS) {
        loop_lock(slot);
        slot->status.active = false;
        loop_unlock(slot);
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "failed to create control loop task");
        }
        return -1;
    }
#else
    if (pthread_create(&slot->thread, NULL, control_loop_thread, slot) != 0) {
        loop_lock(slot);
        slot->status.active = false;
        loop_unlock(slot);
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "failed to create control loop thread");
        }
        return -1;
    }
    pthread_detach(slot->thread);
    slot->thread_created = true;
#endif

    if (buffer != NULL && buffer_size > 0) {
        snprintf(buffer, buffer_size, "loop %s started", loop_id);
    }
    return 0;
}

int espclaw_control_loop_stop(const char *loop_id, char *buffer, size_t buffer_size)
{
    int index = find_loop_index(loop_id);

    if (buffer != NULL && buffer_size > 0) {
        buffer[0] = '\0';
    }
    if (index < 0) {
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "control loop %s not found", loop_id != NULL ? loop_id : "");
        }
        return -1;
    }

    loop_lock(&s_loops[index]);
    s_loops[index].status.stop_requested = true;
    loop_unlock(&s_loops[index]);
    if (buffer != NULL && buffer_size > 0) {
        snprintf(buffer, buffer_size, "loop %s stop requested", loop_id);
    }
    return 0;
}

size_t espclaw_control_loop_snapshot_all(espclaw_control_loop_status_t *statuses, size_t max_statuses)
{
    size_t count = 0;
    size_t index;

    if (statuses == NULL || max_statuses == 0) {
        return 0;
    }

    for (index = 0; index < ESPCLAW_CONTROL_LOOP_MAX && count < max_statuses; ++index) {
        if (s_loops[index].status.loop_id[0] == '\0') {
            continue;
        }
        loop_lock(&s_loops[index]);
        copy_status_locked(&s_loops[index], &statuses[count]);
        loop_unlock(&s_loops[index]);
        count++;
    }

    return count;
}

int espclaw_control_loop_render_json(char *buffer, size_t buffer_size)
{
    espclaw_control_loop_status_t statuses[ESPCLAW_CONTROL_LOOP_MAX];
    size_t count = espclaw_control_loop_snapshot_all(statuses, ESPCLAW_CONTROL_LOOP_MAX);
    size_t index;
    size_t written = 0;

    if (buffer == NULL || buffer_size == 0) {
        return -1;
    }

    written += (size_t)snprintf(buffer + written, buffer_size - written, "{\"loops\":[");
    for (index = 0; index < count && written < buffer_size; ++index) {
        char escaped_result[ESPCLAW_CONTROL_LOOP_RESULT_MAX * 2];

        json_escape_copy(statuses[index].last_result, escaped_result, sizeof(escaped_result));
        written += (size_t)snprintf(
            buffer + written,
            buffer_size - written,
            "%s{\"loop_id\":\"%s\",\"app_id\":\"%s\",\"trigger\":\"%s\",\"active\":%s,"
            "\"completed\":%s,\"stop_requested\":%s,\"period_ms\":%u,\"max_iterations\":%u,"
            "\"iterations_completed\":%u,\"started_ms\":%llu,\"last_started_ms\":%llu,"
            "\"last_finished_ms\":%llu,\"last_status\":%d,\"last_result\":\"%s\"}",
            index == 0 ? "" : ",",
            statuses[index].loop_id,
            statuses[index].app_id,
            statuses[index].trigger,
            statuses[index].active ? "true" : "false",
            statuses[index].completed ? "true" : "false",
            statuses[index].stop_requested ? "true" : "false",
            (unsigned int)statuses[index].period_ms,
            (unsigned int)statuses[index].max_iterations,
            (unsigned int)statuses[index].iterations_completed,
            (unsigned long long)statuses[index].started_ms,
            (unsigned long long)statuses[index].last_started_ms,
            (unsigned long long)statuses[index].last_finished_ms,
            statuses[index].last_status,
            escaped_result
        );
    }
    written += (size_t)snprintf(buffer + written, buffer_size - written, "]}");

    if (written >= buffer_size) {
        buffer[buffer_size - 1] = '\0';
    }
    return 0;
}

void espclaw_control_loop_shutdown_all(void)
{
    size_t index;

    for (index = 0; index < ESPCLAW_CONTROL_LOOP_MAX; ++index) {
        if (s_loops[index].status.loop_id[0] == '\0') {
            continue;
        }
        loop_lock(&s_loops[index]);
        s_loops[index].status.stop_requested = true;
        loop_unlock(&s_loops[index]);
    }
}
