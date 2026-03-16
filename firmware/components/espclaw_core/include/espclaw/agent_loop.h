#ifndef ESPCLAW_AGENT_LOOP_H
#define ESPCLAW_AGENT_LOOP_H

#include <stdbool.h>
#include <stddef.h>

#include "espclaw/auth_store.h"

#define ESPCLAW_AGENT_SESSION_ID_MAX 63
#define ESPCLAW_AGENT_RESPONSE_ID_MAX 95
#define ESPCLAW_AGENT_TEXT_MAX 8191
#define ESPCLAW_AGENT_TOOL_CALL_MAX 8
#define ESPCLAW_AGENT_TOOL_NAME_MAX 63
#define ESPCLAW_AGENT_TOOL_ARGS_MAX 1023

typedef struct {
    char call_id[ESPCLAW_AGENT_RESPONSE_ID_MAX + 1];
    char name[ESPCLAW_AGENT_TOOL_NAME_MAX + 1];
    char arguments_json[ESPCLAW_AGENT_TOOL_ARGS_MAX + 1];
} espclaw_agent_tool_call_t;

typedef struct {
    bool ok;
    bool used_tools;
    bool hit_iteration_limit;
    unsigned int iterations;
    char response_id[ESPCLAW_AGENT_RESPONSE_ID_MAX + 1];
    char final_text[ESPCLAW_AGENT_TEXT_MAX + 1];
} espclaw_agent_run_result_t;

typedef int (*espclaw_agent_http_adapter_t)(
    const char *url,
    const espclaw_auth_profile_t *profile,
    const char *body,
    char *response,
    size_t response_size,
    void *user_data
);

int espclaw_agent_loop_run(
    const char *workspace_root,
    const char *session_id,
    const char *user_message,
    bool allow_mutations,
    espclaw_agent_run_result_t *result
);

void espclaw_agent_set_http_adapter(espclaw_agent_http_adapter_t adapter, void *user_data);

#endif
