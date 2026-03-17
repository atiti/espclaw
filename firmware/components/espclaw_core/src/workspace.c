#include "espclaw/workspace.h"

#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static const espclaw_workspace_file_t WORKSPACE_FILES[] = {
    {
        "AGENTS.md",
        "# Agent Instructions\n\n"
        "You are ESPClaw, an embedded AI assistant.\n"
        "Prefer concise, hardware-aware actions and keep the user informed.\n",
    },
    {
        "IDENTITY.md",
        "# Identity\n\n"
        "ESPClaw is an ESP32-native assistant with SD-backed workspace files and local hardware tools.\n",
    },
    {
        "USER.md",
        "# User Preferences\n\n"
        "- Capture stable preferences here.\n"
        "- Prefer durable notes over transient chat history.\n",
    },
    {
        "HEARTBEAT.md",
        "# Heartbeat\n\n"
        "- Check pending maintenance tasks.\n"
        "- Report only when there is meaningful work to do.\n",
    },
    {
        "memory/MEMORY.md",
        "# Memory\n\n"
        "Long-term user and system memory belongs here.\n",
    },
    {
        "config/board.json",
        "{\n"
        "  \"variant\": \"auto\",\n"
        "  \"pins\": {\n"
        "    \"buzzer\": -1,\n"
        "    \"power_ctl\": -1,\n"
        "    \"battery_adc_pin\": -1\n"
        "  },\n"
        "  \"i2c\": {\n"
        "    \"default\": {\n"
        "      \"port\": 0,\n"
        "      \"sda\": -1,\n"
        "      \"scl\": -1,\n"
        "      \"frequency_hz\": 400000\n"
        "    }\n"
        "  },\n"
        "  \"uart\": {\n"
        "    \"console\": {\n"
        "      \"port\": 0,\n"
        "      \"tx\": -1,\n"
        "      \"rx\": -1,\n"
        "      \"baud_rate\": 115200\n"
        "    }\n"
        "  },\n"
        "  \"adc\": {\n"
        "    \"battery\": {\n"
        "      \"unit\": 1,\n"
        "      \"channel\": -1\n"
        "    }\n"
        "  }\n"
        "}\n",
    },
};

size_t espclaw_workspace_file_count(void)
{
    return sizeof(WORKSPACE_FILES) / sizeof(WORKSPACE_FILES[0]);
}

const espclaw_workspace_file_t *espclaw_workspace_file_at(size_t index)
{
    if (index >= espclaw_workspace_file_count()) {
        return NULL;
    }
    return &WORKSPACE_FILES[index];
}

const espclaw_workspace_file_t *espclaw_find_workspace_file(const char *relative_path)
{
    size_t index;

    if (relative_path == NULL) {
        return NULL;
    }

    for (index = 0; index < espclaw_workspace_file_count(); ++index) {
        if (strcmp(WORKSPACE_FILES[index].relative_path, relative_path) == 0) {
            return &WORKSPACE_FILES[index];
        }
    }

    return NULL;
}

bool espclaw_workspace_is_control_file(const char *relative_path)
{
    return espclaw_find_workspace_file(relative_path) != NULL;
}

int espclaw_workspace_resolve_path(
    const char *workspace_root,
    const char *relative_path,
    char *buffer,
    size_t buffer_size
)
{
    int written;

    if (workspace_root == NULL || relative_path == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    if (relative_path[0] == '/' || strstr(relative_path, "..") != NULL) {
        return -1;
    }

    written = snprintf(buffer, buffer_size, "%s/%s", workspace_root, relative_path);
    if (written < 0 || (size_t)written >= buffer_size) {
        return -1;
    }

    return 0;
}

static int ensure_directory(const char *path)
{
    if (mkdir(path, 0x1ED) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
}

static int bootstrap_file(const char *workspace_root, const espclaw_workspace_file_t *file)
{
    char path[512];
    FILE *handle;

    if (workspace_root == NULL || file == NULL) {
        return -1;
    }

    if (espclaw_workspace_resolve_path(workspace_root, file->relative_path, path, sizeof(path)) != 0) {
        return -1;
    }

    handle = fopen(path, "r");
    if (handle != NULL) {
        fclose(handle);
        return 0;
    }

    handle = fopen(path, "w");
    if (handle == NULL) {
        return -1;
    }

    fputs(file->default_content, handle);
    fclose(handle);
    return 0;
}

int espclaw_workspace_bootstrap(const char *workspace_root)
{
    char path[512];
    size_t index;

    if (workspace_root == NULL || workspace_root[0] == '\0') {
        return -1;
    }

    if (ensure_directory(workspace_root) != 0) {
        return -1;
    }

    if (espclaw_workspace_resolve_path(workspace_root, "memory", path, sizeof(path)) != 0 || ensure_directory(path) != 0) {
        return -1;
    }
    if (espclaw_workspace_resolve_path(workspace_root, "sessions", path, sizeof(path)) != 0 || ensure_directory(path) != 0) {
        return -1;
    }
    if (espclaw_workspace_resolve_path(workspace_root, "media", path, sizeof(path)) != 0 || ensure_directory(path) != 0) {
        return -1;
    }
    if (espclaw_workspace_resolve_path(workspace_root, "config", path, sizeof(path)) != 0 || ensure_directory(path) != 0) {
        return -1;
    }
    if (espclaw_workspace_resolve_path(workspace_root, "apps", path, sizeof(path)) != 0 || ensure_directory(path) != 0) {
        return -1;
    }
    if (espclaw_workspace_resolve_path(workspace_root, "behaviors", path, sizeof(path)) != 0 || ensure_directory(path) != 0) {
        return -1;
    }

    for (index = 0; index < espclaw_workspace_file_count(); ++index) {
        if (bootstrap_file(workspace_root, &WORKSPACE_FILES[index]) != 0) {
            return -1;
        }
    }

    return 0;
}

int espclaw_workspace_read_file(
    const char *workspace_root,
    const char *relative_path,
    char *buffer,
    size_t buffer_size
)
{
    char path[512];
    FILE *handle;
    size_t bytes_read;

    if (workspace_root == NULL || relative_path == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    if (espclaw_workspace_resolve_path(workspace_root, relative_path, path, sizeof(path)) != 0) {
        return -1;
    }

    handle = fopen(path, "r");
    if (handle == NULL) {
        return -1;
    }

    bytes_read = fread(buffer, 1, buffer_size - 1, handle);
    buffer[bytes_read] = '\0';
    fclose(handle);
    return 0;
}

int espclaw_workspace_write_file(
    const char *workspace_root,
    const char *relative_path,
    const char *content
)
{
    char path[512];
    FILE *handle;

    if (workspace_root == NULL || relative_path == NULL || content == NULL) {
        return -1;
    }

    if (espclaw_workspace_resolve_path(workspace_root, relative_path, path, sizeof(path)) != 0) {
        return -1;
    }

    handle = fopen(path, "w");
    if (handle == NULL) {
        return -1;
    }

    fputs(content, handle);
    fclose(handle);
    return 0;
}
