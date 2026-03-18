#include "espclaw/runtime.h"

#include <stdio.h>

static bool s_host_yolo_mode = true;

bool espclaw_runtime_get_yolo_mode(void)
{
    return s_host_yolo_mode;
}

esp_err_t espclaw_runtime_set_yolo_mode(bool enabled, char *message, size_t message_size)
{
    s_host_yolo_mode = enabled;
    if (message != NULL && message_size > 0) {
        snprintf(message, message_size, "YOLO mode %s.", enabled ? "enabled" : "disabled");
    }
    return 0;
}
