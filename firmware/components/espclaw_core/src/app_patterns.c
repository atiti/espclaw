#include "espclaw/app_patterns.h"

#include <stdio.h>

typedef struct {
    const char *name;
    const char *summary;
    const char *when_to_use;
    const char *shape;
} espclaw_app_pattern_t;

static const char *RULES[] = {
    "A component is reusable Lua code plus metadata. Install it under /workspace/components and expose it through /workspace/lib for require(...).",
    "An app is a user-facing feature bundle under /workspace/apps/<app_id> that owns product logic and triggers.",
    "A task is a live schedule for an app. Use it when work should run now but does not need to survive reboot.",
    "A behavior is a persisted task definition. Use it when work should autostart or survive reboot.",
    "An event is a named signal. Use it only when producers and consumers should be decoupled.",
    "Default to component plus app. Introduce events only when multiple live consumers need the same hardware or sensor stream.",
};

static const espclaw_app_pattern_t PATTERNS[] = {
    {
        "single_app_only",
        "One app owns the entire feature and talks to hardware directly.",
        "Use for one-off demos or local features that are not reused elsewhere.",
        "app.install -> optional task.start or behavior.register"
    },
    {
        "shared_component_plus_apps",
        "A reusable component provides low-level driver or math logic and multiple apps require it.",
        "Use for drivers, filters, mixers, or sensor adapters such as MS5611.",
        "component.install -> app.install(weather_station) + app.install(vario)"
    },
    {
        "sampler_behavior_and_event_consumers",
        "One sampler app owns the hardware and emits events while downstream apps consume those readings.",
        "Use when multiple live consumers need the same sensor updates without duplicate bus traffic.",
        "component.install(driver) -> app.install(sampler) -> behavior.register(sampler) -> event-driven consumers"
    },
};

size_t espclaw_render_app_patterns_json(char *buffer, size_t buffer_size)
{
    size_t used = 0;
    size_t index;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"rules\":[");
    for (index = 0; index < sizeof(RULES) / sizeof(RULES[0]) && used + 8 < buffer_size; ++index) {
        used += (size_t)snprintf(buffer + used, buffer_size - used, "%s\"%s\"", index == 0 ? "" : ",", RULES[index]);
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "],\"patterns\":[");
    for (index = 0; index < sizeof(PATTERNS) / sizeof(PATTERNS[0]) && used + 32 < buffer_size; ++index) {
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "%s{\"name\":\"%s\",\"summary\":\"%s\",\"when_to_use\":\"%s\",\"shape\":\"%s\"}",
            index == 0 ? "" : ",",
            PATTERNS[index].name,
            PATTERNS[index].summary,
            PATTERNS[index].when_to_use,
            PATTERNS[index].shape
        );
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "]}");
    return used;
}

size_t espclaw_render_app_patterns_markdown(char *buffer, size_t buffer_size)
{
    size_t used = 0;
    size_t index;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "# App Patterns\n\n## Rules\n\n");
    for (index = 0; index < sizeof(RULES) / sizeof(RULES[0]) && used + 8 < buffer_size; ++index) {
        used += (size_t)snprintf(buffer + used, buffer_size - used, "- %s\n", RULES[index]);
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "\n## Patterns\n");
    for (index = 0; index < sizeof(PATTERNS) / sizeof(PATTERNS[0]) && used + 16 < buffer_size; ++index) {
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "\n### %s\n- %s\n- Use when: %s\n- Shape: `%s`\n",
            PATTERNS[index].name,
            PATTERNS[index].summary,
            PATTERNS[index].when_to_use,
            PATTERNS[index].shape
        );
    }
    return used;
}

size_t espclaw_render_app_patterns_prompt_snapshot(char *buffer, size_t buffer_size)
{
    size_t used = 0;
    size_t index;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "# App Architecture Contract\n");
    for (index = 0; index < sizeof(RULES) / sizeof(RULES[0]) && used + 8 < buffer_size; ++index) {
        used += (size_t)snprintf(buffer + used, buffer_size - used, "- %s\n", RULES[index]);
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "\n# Recommended Patterns\n");
    for (index = 0; index < sizeof(PATTERNS) / sizeof(PATTERNS[0]) && used + 16 < buffer_size; ++index) {
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "- %s: %s Shape: %s\n",
            PATTERNS[index].name,
            PATTERNS[index].summary,
            PATTERNS[index].shape
        );
    }
    return used;
}
