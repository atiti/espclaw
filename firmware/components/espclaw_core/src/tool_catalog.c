#include "espclaw/tool_catalog.h"

#include <stddef.h>
#include <string.h>

static const espclaw_tool_descriptor_t TOOLS[] = {
    {"fs.read", "Read file content from the SD-backed workspace.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Relative workspace path.\"}},\"required\":[\"path\"]}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"fs.write", "Write file content to the SD-backed workspace.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"fs.list", "List files in a workspace directory.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Relative directory path. Omit or use '.' for the workspace root.\"}}}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"fs.delete", "Delete a file from the workspace.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"system.info", "Inspect device, storage, and memory health.",
     "{\"type\":\"object\",\"properties\":{}}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"hardware.list", "List the current board descriptor, named pins and buses, and supported hardware capabilities.",
     "{\"type\":\"object\",\"properties\":{}}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"lua_api.list", "List the authoritative Lua app rules and espclaw.* function signatures.",
     "{\"type\":\"object\",\"properties\":{}}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"app_patterns.list", "List the recommended architecture patterns for components, apps, tasks, behaviors, and events.",
     "{\"type\":\"object\",\"properties\":{}}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"system.reboot", "Restart the device.",
     "{\"type\":\"object\",\"properties\":{}}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"wifi.status", "Inspect Wi-Fi status and current network.",
     "{\"type\":\"object\",\"properties\":{}}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"wifi.scan", "Scan for Wi-Fi networks.",
     "{\"type\":\"object\",\"properties\":{}}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"ble.scan", "Scan for nearby BLE devices.",
     "{\"type\":\"object\",\"properties\":{}}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"camera.capture", "Capture a JPEG frame and store it to SD.",
     "{\"type\":\"object\",\"properties\":{\"filename\":{\"type\":\"string\"}}}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"web.search", "Run a web search through the configured proxy backend and return compact ranked results.",
     "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"web.fetch", "Fetch and scrape a web page or document through the proxy backend and persist larger markdown content into the workspace when needed.",
     "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"]}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"gpio.read", "Read a GPIO pin value.",
     "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"}},\"required\":[\"pin\"]}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"gpio.write", "Write a GPIO pin value.",
     "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"},\"value\":{\"type\":\"integer\"}},\"required\":[\"pin\",\"value\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"pwm.write", "Drive an LEDC PWM channel or buzzer output.",
     "{\"type\":\"object\",\"properties\":{\"channel\":{\"type\":\"integer\"},\"duty\":{\"type\":\"number\"}},\"required\":[\"channel\",\"duty\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"ppm.write", "Drive an RMT-backed PPM output frame.",
     "{\"type\":\"object\",\"properties\":{\"channel\":{\"type\":\"integer\"},\"value_us\":{\"type\":\"integer\"}},\"required\":[\"channel\",\"value_us\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"adc.read", "Read a raw ADC sample.",
     "{\"type\":\"object\",\"properties\":{\"channel\":{\"type\":\"integer\"}},\"required\":[\"channel\"]}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"i2c.scan", "Scan the I2C bus for attached devices.",
     "{\"type\":\"object\",\"properties\":{\"port\":{\"type\":\"integer\"}}}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"i2c.read", "Read bytes from an I2C device.",
     "{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"integer\"},\"register\":{\"type\":\"integer\"},\"length\":{\"type\":\"integer\"}},\"required\":[\"address\",\"register\",\"length\"]}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"i2c.write", "Write bytes to an I2C device.",
     "{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"integer\"},\"register\":{\"type\":\"integer\"},\"data\":{\"type\":\"string\",\"description\":\"Hex bytes separated by spaces.\"}},\"required\":[\"address\",\"register\",\"data\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"temperature.read", "Read a typed temperature sensor through the I2C subsystem.",
     "{\"type\":\"object\",\"properties\":{\"sensor\":{\"type\":\"string\"}}}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"imu.read", "Read a typed IMU sample and derived attitude inputs.",
     "{\"type\":\"object\",\"properties\":{\"sensor\":{\"type\":\"string\"}}}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"buzzer.play", "Generate an audible tone through a PWM-capable pin.",
     "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"},\"frequency_hz\":{\"type\":\"integer\"},\"duration_ms\":{\"type\":\"integer\"}},\"required\":[\"pin\",\"frequency_hz\",\"duration_ms\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"pid.compute", "Compute a native PID control step for a Lua app.",
     "{\"type\":\"object\",\"properties\":{\"setpoint\":{\"type\":\"number\"},\"measurement\":{\"type\":\"number\"},\"dt_seconds\":{\"type\":\"number\"},\"kp\":{\"type\":\"number\"},\"ki\":{\"type\":\"number\"},\"kd\":{\"type\":\"number\"}},\"required\":[\"setpoint\",\"measurement\",\"dt_seconds\",\"kp\",\"ki\",\"kd\"]}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"control.mix", "Mix normalized rover or quad control outputs.",
     "{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"string\"},\"throttle\":{\"type\":\"number\"},\"steering\":{\"type\":\"number\"},\"roll\":{\"type\":\"number\"},\"pitch\":{\"type\":\"number\"},\"yaw\":{\"type\":\"number\"}}}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"spi.transfer", "Execute an SPI full-duplex transfer.",
     "{\"type\":\"object\",\"properties\":{\"bus\":{\"type\":\"integer\"},\"data\":{\"type\":\"string\"}},\"required\":[\"bus\",\"data\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"uart.read", "Read from an attached UART peripheral.",
     "{\"type\":\"object\",\"properties\":{\"port\":{\"type\":\"integer\"},\"length\":{\"type\":\"integer\"}}}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"uart.write", "Write to an attached UART peripheral.",
     "{\"type\":\"object\",\"properties\":{\"port\":{\"type\":\"integer\"},\"data\":{\"type\":\"string\"}},\"required\":[\"port\",\"data\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"ota.check", "Check whether a newer firmware image is available.",
     "{\"type\":\"object\",\"properties\":{}}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"ota.apply", "Apply a pending firmware image.",
     "{\"type\":\"object\",\"properties\":{}}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"tool.list", "List the currently available tools, their descriptions, and safety levels.",
     "{\"type\":\"object\",\"properties\":{}}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"app.list", "List Lua apps installed on the SD-backed workspace.",
     "{\"type\":\"object\",\"properties\":{}}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"component.list", "List reusable Lua components installed on the workspace.",
     "{\"type\":\"object\",\"properties\":{}}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"app.run", "Run an installed Lua app with a named trigger.",
     "{\"type\":\"object\",\"properties\":{\"app_id\":{\"type\":\"string\"},\"trigger\":{\"type\":\"string\"},\"payload\":{\"type\":\"string\"}},\"required\":[\"app_id\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"app.install", "Create or update a Lua app bundle in the workspace from Lua source, including permissions and triggers for persistent behaviors.",
     "{\"type\":\"object\",\"properties\":{\"app_id\":{\"type\":\"string\"},\"title\":{\"type\":\"string\"},\"source\":{\"type\":\"string\"},\"permissions_csv\":{\"type\":\"string\"},\"triggers_csv\":{\"type\":\"string\"}},\"required\":[\"app_id\",\"source\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"component.install", "Create or update a reusable Lua component with metadata and publish its module under /workspace/lib for require(...).",
     "{\"type\":\"object\",\"properties\":{\"component_id\":{\"type\":\"string\"},\"title\":{\"type\":\"string\"},\"module\":{\"type\":\"string\"},\"summary\":{\"type\":\"string\"},\"version\":{\"type\":\"string\"},\"source\":{\"type\":\"string\"}},\"required\":[\"component_id\",\"module\",\"source\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"app.remove", "Remove a Lua app bundle from the workspace.",
     "{\"type\":\"object\",\"properties\":{\"app_id\":{\"type\":\"string\"}},\"required\":[\"app_id\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"component.remove", "Remove a reusable Lua component bundle and its published module from the workspace.",
     "{\"type\":\"object\",\"properties\":{\"component_id\":{\"type\":\"string\"}},\"required\":[\"component_id\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"behavior.list", "List persisted autonomous behaviors and their runtime state.",
     "{\"type\":\"object\",\"properties\":{}}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"behavior.register", "Create or update a persisted autonomous behavior, optionally installing the Lua app source it should run.",
     "{\"type\":\"object\",\"properties\":{\"behavior_id\":{\"type\":\"string\"},\"app_id\":{\"type\":\"string\"},\"title\":{\"type\":\"string\"},\"source\":{\"type\":\"string\"},\"permissions_csv\":{\"type\":\"string\"},\"triggers_csv\":{\"type\":\"string\"},\"schedule\":{\"type\":\"string\",\"enum\":[\"periodic\",\"event\"]},\"trigger\":{\"type\":\"string\"},\"payload\":{\"type\":\"string\"},\"period_ms\":{\"type\":\"integer\"},\"iterations\":{\"type\":\"integer\"},\"autostart\":{\"type\":\"boolean\"}},\"required\":[\"behavior_id\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"behavior.start", "Start a persisted behavior locally using its saved schedule and trigger.",
     "{\"type\":\"object\",\"properties\":{\"behavior_id\":{\"type\":\"string\"}},\"required\":[\"behavior_id\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"behavior.stop", "Stop a running persisted behavior.",
     "{\"type\":\"object\",\"properties\":{\"behavior_id\":{\"type\":\"string\"}},\"required\":[\"behavior_id\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"behavior.remove", "Remove a persisted behavior definition from the workspace.",
     "{\"type\":\"object\",\"properties\":{\"behavior_id\":{\"type\":\"string\"}},\"required\":[\"behavior_id\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"task.list", "List running and completed background tasks.",
     "{\"type\":\"object\",\"properties\":{}}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"task.start", "Start a background Lua task using a periodic or event-driven schedule.",
     "{\"type\":\"object\",\"properties\":{\"task_id\":{\"type\":\"string\"},\"app_id\":{\"type\":\"string\"},\"schedule\":{\"type\":\"string\",\"enum\":[\"periodic\",\"event\"]},\"trigger\":{\"type\":\"string\"},\"payload\":{\"type\":\"string\"},\"period_ms\":{\"type\":\"integer\"},\"iterations\":{\"type\":\"integer\"}},\"required\":[\"task_id\",\"app_id\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"task.stop", "Request that a background Lua task stops.",
     "{\"type\":\"object\",\"properties\":{\"task_id\":{\"type\":\"string\"}},\"required\":[\"task_id\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"event.emit", "Emit a named event payload to event-driven Lua tasks.",
     "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"payload\":{\"type\":\"string\"}},\"required\":[\"name\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"event.watch_list", "List active hardware event watches that translate UART or sensor changes into local events.",
     "{\"type\":\"object\",\"properties\":{}}",
     ESPCLAW_TOOL_SAFETY_READ_ONLY},
    {"event.watch_add", "Register a UART or ADC threshold watch that emits named local events without consulting the LLM.",
     "{\"type\":\"object\",\"properties\":{\"watch_id\":{\"type\":\"string\"},\"kind\":{\"type\":\"string\",\"enum\":[\"uart\",\"adc_threshold\"]},\"event_name\":{\"type\":\"string\"},\"port\":{\"type\":\"integer\"},\"unit\":{\"type\":\"integer\"},\"channel\":{\"type\":\"integer\"},\"threshold\":{\"type\":\"integer\"},\"interval_ms\":{\"type\":\"integer\"}},\"required\":[\"watch_id\",\"kind\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
    {"event.watch_remove", "Remove a hardware event watch.",
     "{\"type\":\"object\",\"properties\":{\"watch_id\":{\"type\":\"string\"}},\"required\":[\"watch_id\"]}",
     ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED},
};

size_t espclaw_tool_count(void)
{
    return sizeof(TOOLS) / sizeof(TOOLS[0]);
}

const espclaw_tool_descriptor_t *espclaw_tool_at(size_t index)
{
    if (index >= espclaw_tool_count()) {
        return NULL;
    }
    return &TOOLS[index];
}

const espclaw_tool_descriptor_t *espclaw_find_tool(const char *name)
{
    size_t index;

    if (name == NULL) {
        return NULL;
    }

    for (index = 0; index < espclaw_tool_count(); ++index) {
        if (strcmp(TOOLS[index].name, name) == 0) {
            return &TOOLS[index];
        }
    }

    return NULL;
}

bool espclaw_tool_requires_confirmation(const char *name)
{
    const espclaw_tool_descriptor_t *tool = espclaw_find_tool(name);

    return tool != NULL && tool->safety == ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED;
}
