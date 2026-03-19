#include "espclaw/admin_api.h"
#include "espclaw/agent_loop.h"
#include "espclaw/board_config.h"
#include "espclaw/board_profile.h"
#include "espclaw/hardware.h"
#include "espclaw/log_buffer.h"
#include "espclaw/task_policy.h"
#include "espclaw/workspace.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <emscripten/emscripten.h>

#define ESPCLAW_WASM_WORKSPACE_ROOT "/workspace"
#define ESPCLAW_WASM_RESPONSE_MAX 131072

static espclaw_board_profile_t s_profile;
static bool s_initialized = false;
static char s_response[ESPCLAW_WASM_RESPONSE_MAX];

static const char *render_error_json(const char *message)
{
    snprintf(
        s_response,
        sizeof(s_response),
        "{\"ok\":false,\"error\":\"%s\"}",
        message != NULL ? message : "unknown error"
    );
    return s_response;
}

static int wasm_runtime_bootstrap(void)
{
    if (s_initialized) {
        return 0;
    }

    s_profile = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32CAM);
    espclaw_log_buffer_init();
    espclaw_log_buffer_reset();
    espclaw_hw_sim_reset();
    if (espclaw_workspace_bootstrap(ESPCLAW_WASM_WORKSPACE_ROOT) != 0) {
        return -1;
    }
    espclaw_task_policy_select(&s_profile);
    if (espclaw_board_configure_current(ESPCLAW_WASM_WORKSPACE_ROOT, &s_profile) != 0) {
        return -1;
    }
    if (espclaw_hw_apply_board_boot_defaults() != 0) {
        return -1;
    }
    espclaw_log_buffer_append("[wasm] runtime boot complete\n");
    s_initialized = true;
    return 0;
}

EMSCRIPTEN_KEEPALIVE
const char *espclaw_wasm_reset(void)
{
    s_initialized = false;
    return wasm_runtime_bootstrap() == 0
               ? "{\"ok\":true}"
               : render_error_json("failed to bootstrap wasm runtime");
}

EMSCRIPTEN_KEEPALIVE
const char *espclaw_wasm_status_json(void)
{
    if (wasm_runtime_bootstrap() != 0) {
        return render_error_json("failed to bootstrap wasm runtime");
    }

    espclaw_render_admin_status_json(
        &s_profile,
        s_profile.default_storage_backend,
        "browser_lab",
        "browser_lab",
        true,
        true,
        NULL,
        s_response,
        sizeof(s_response)
    );
    return s_response;
}

EMSCRIPTEN_KEEPALIVE
const char *espclaw_wasm_logs_json(int tail_bytes)
{
    if (wasm_runtime_bootstrap() != 0) {
        return render_error_json("failed to bootstrap wasm runtime");
    }
    if (espclaw_log_buffer_render_json(
            tail_bytes > 0 ? (size_t)tail_bytes : 4096U,
            s_response,
            sizeof(s_response)
        ) != 0) {
        return render_error_json("failed to render log buffer");
    }
    return s_response;
}

EMSCRIPTEN_KEEPALIVE
const char *espclaw_wasm_execute_tool(const char *tool_name, const char *arguments_json, int allow_mutations)
{
    if (wasm_runtime_bootstrap() != 0) {
        return render_error_json("failed to bootstrap wasm runtime");
    }
    if (tool_name == NULL || tool_name[0] == '\0') {
        return render_error_json("tool_name is required");
    }
    if (espclaw_agent_execute_tool(
            ESPCLAW_WASM_WORKSPACE_ROOT,
            tool_name,
            arguments_json != NULL ? arguments_json : "{}",
            allow_mutations != 0,
            s_response,
            sizeof(s_response)
        ) != 0) {
        return render_error_json("tool execution failed");
    }
    return s_response;
}
