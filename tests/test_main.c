#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "espclaw/admin_api.h"
#include "espclaw/admin_ops.h"
#include "espclaw/admin_ui.h"
#include "espclaw/agent_loop.h"
#include "espclaw/app_runtime.h"
#include "espclaw/auth_store.h"
#include "espclaw/board_profile.h"
#include "espclaw/channel.h"
#include "espclaw/config_render.h"
#include "espclaw/control_loop.h"
#include "espclaw/hardware.h"
#include "espclaw/ota_state.h"
#include "espclaw/provider.h"
#include "espclaw/provider_request.h"
#include "espclaw/session_store.h"
#include "espclaw/storage.h"
#include "espclaw/telegram_protocol.h"
#include "espclaw/tool_catalog.h"
#include "espclaw/workspace.h"

static void assert_true(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "assertion failed: %s\n", message);
        exit(1);
    }
}

static void assert_string_contains(const char *haystack, const char *needle, const char *message)
{
    assert_true(haystack != NULL, "haystack must not be null");
    assert_true(strstr(haystack, needle) != NULL, message);
}

static void make_temp_dir(char *buffer, size_t buffer_size)
{
    snprintf(buffer, buffer_size, "/tmp/espclaw-test-XXXXXX");
    assert_true(mkdtemp(buffer) != NULL, "mkdtemp failed");
}

static void write_text_file(const char *path, const char *content)
{
    FILE *file = fopen(path, "w");

    assert_true(file != NULL, "fopen for test fixture failed");
    assert_true(fputs(content, file) >= 0, "fputs for test fixture failed");
    fclose(file);
}

typedef struct {
    unsigned int calls;
} test_agent_adapter_state_t;

static int confirmation_http_adapter(
    const char *url,
    const espclaw_auth_profile_t *profile,
    const char *body,
    char *response,
    size_t response_size,
    void *user_data
)
{
    test_agent_adapter_state_t *state = (test_agent_adapter_state_t *)user_data;

    (void)url;
    (void)profile;

    if (state != NULL) {
        state->calls++;
    }

    assert_string_contains(body, "\"store\":false", "agent loop sends store=false");
    assert_string_contains(body, "\"stream\":true", "agent loop sends stream=true");

    if (state != NULL && state->calls > 1) {
        assert_string_contains(body, "\"instructions\":", "follow-up codex requests retain instructions");
        assert_string_contains(body, "\"type\":\"function_call\"", "follow-up codex requests retain function call items");
        assert_string_contains(body, "\"type\":\"function_call_output\"", "follow-up codex requests include tool outputs");
        snprintf(
            response,
            response_size,
            "{"
            "\"id\":\"resp_confirm_final\","
            "\"status\":\"completed\","
            "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"Please confirm before I run the app.\"}]}]"
            "}"
        );
        return 0;
    }

    assert_string_contains(body, "\"name\":\"system_x2E_info\"", "codex-safe tool names are encoded");

    snprintf(
        response,
        response_size,
        "{"
        "\"id\":\"resp_confirm_first\","
        "\"status\":\"completed\","
        "\"output\":["
        "{\"type\":\"function_call\",\"call_id\":\"call_run_app\",\"name\":\"app.run\",\"arguments\":\"{\\\"app_id\\\":\\\"demo_app\\\",\\\"trigger\\\":\\\"manual\\\",\\\"payload\\\":\\\"test\\\"}\",\"status\":\"completed\"}"
        "]"
        "}"
    );
    return 0;
}

static int tool_list_http_adapter(
    const char *url,
    const espclaw_auth_profile_t *profile,
    const char *body,
    char *response,
    size_t response_size,
    void *user_data
)
{
    test_agent_adapter_state_t *state = (test_agent_adapter_state_t *)user_data;

    (void)url;
    (void)profile;

    if (state != NULL) {
        state->calls++;
    }

    assert_string_contains(body, "\"store\":false", "tool list loop sends store=false");
    assert_string_contains(body, "\"stream\":true", "tool list loop sends stream=true");

    if (state != NULL && state->calls > 1) {
        assert_string_contains(body, "\"type\":\"function_call\"", "tool list follow-up retains function call item");
        assert_string_contains(body, "\"type\":\"function_call_output\"", "tool list follow-up includes output item");
        snprintf(
            response,
            response_size,
            "{"
            "\"id\":\"resp_tools_final\","
            "\"status\":\"completed\","
            "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"I can inspect files, apps, hardware, networking, and OTA state. Use tool.list details to choose the right operation.\"}]}]"
            "}"
        );
        return 0;
    }

    snprintf(
        response,
        response_size,
        "{"
        "\"id\":\"resp_tools_first\","
        "\"status\":\"completed\","
        "\"output\":["
        "{\"type\":\"function_call\",\"call_id\":\"call_tool_list\",\"name\":\"tool.list\",\"arguments\":\"{}\",\"status\":\"completed\"}"
        "]"
        "}"
    );
    return 0;
}

static void test_board_profiles(void)
{
    espclaw_board_profile_t s3 = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32S3);
    espclaw_board_profile_t cam = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32CAM);
    espclaw_board_profile_t c3 = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32C3);

    assert_true(strcmp(s3.id, "esp32s3") == 0, "esp32s3 profile id");
    assert_true(strcmp(s3.provisioning, "ble") == 0, "esp32s3 uses BLE provisioning");
    assert_true(s3.default_storage_backend == ESPCLAW_STORAGE_BACKEND_SD_CARD, "esp32s3 defaults to sd storage");
    assert_true(s3.supports_concurrent_capture, "esp32s3 supports concurrent capture");
    assert_true(strcmp(cam.id, "esp32cam") == 0, "esp32cam profile id");
    assert_true(strcmp(cam.provisioning, "softap") == 0, "esp32cam uses SoftAP provisioning");
    assert_true(cam.default_storage_backend == ESPCLAW_STORAGE_BACKEND_SD_CARD, "esp32cam defaults to sd storage");
    assert_true(!cam.supports_ble_provisioning, "esp32cam does not expose BLE provisioning");
    assert_true(strcmp(c3.id, "esp32c3") == 0, "esp32c3 profile id");
    assert_true(strcmp(c3.provisioning, "ble") == 0, "esp32c3 uses BLE provisioning");
    assert_true(c3.default_storage_backend == ESPCLAW_STORAGE_BACKEND_LITTLEFS, "esp32c3 defaults to littlefs");
    assert_true(!c3.has_camera, "esp32c3 has no camera");
}

static void test_workspace_manifest(void)
{
    const espclaw_workspace_file_t *memory_file = espclaw_find_workspace_file("memory/MEMORY.md");

    assert_true(espclaw_workspace_file_count() == 5, "workspace bootstrap file count");
    assert_true(memory_file != NULL, "memory file exists");
    assert_string_contains(memory_file->default_content, "Long-term", "memory template content");
    assert_true(espclaw_workspace_is_control_file("HEARTBEAT.md"), "heartbeat is control file");
    assert_true(!espclaw_workspace_is_control_file("sessions/demo.jsonl"), "session file is not control file");
}

static void test_provider_and_channel_registry(void)
{
    const espclaw_provider_descriptor_t *openai = espclaw_find_provider("openai_compat");
    const espclaw_provider_descriptor_t *codex = espclaw_find_provider("openai_codex");
    const espclaw_channel_descriptor_t *telegram = espclaw_find_channel("telegram");
    const espclaw_channel_descriptor_t *slack = espclaw_find_channel("slack");

    assert_true(espclaw_provider_count() == 3, "provider count");
    assert_true(openai != NULL && openai->supports_tool_calls, "openai-compatible provider supports tool calls");
    assert_true(codex != NULL && codex->requires_account_id, "codex provider requires account id");
    assert_true(telegram != NULL && telegram->enabled_in_v1, "telegram is enabled in v1");
    assert_true(slack != NULL && !slack->enabled_in_v1, "slack is deferred");
}

static void test_tool_catalog(void)
{
    assert_true(espclaw_tool_count() >= 20, "tool catalog size");
    assert_true(espclaw_find_tool("tool.list") != NULL, "tool list tool exists");
    assert_true(espclaw_tool_requires_confirmation("fs.write"), "fs.write requires confirmation");
    assert_true(!espclaw_tool_requires_confirmation("wifi.scan"), "wifi.scan is read-only");
    assert_true(espclaw_find_tool("camera.capture") != NULL, "camera capture tool exists");
}

static void test_default_config_render(void)
{
    char buffer[4096];
    size_t written;
    espclaw_board_profile_t s3 = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32S3);
    espclaw_board_profile_t c3 = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32C3);

    written = espclaw_render_default_config(&s3, buffer, sizeof(buffer));

    assert_true(written > 0, "config render produced data");
    assert_string_contains(buffer, "\"board_profile\": \"esp32s3\"", "board profile rendered");
    assert_string_contains(buffer, "\"backend\": \"sdcard\"", "storage backend rendered");
    assert_string_contains(buffer, "\"providers\": [", "providers section rendered");
    assert_string_contains(buffer, "\"telegram\": {", "telegram section rendered");
    assert_string_contains(buffer, "\"admin_auth_required\": true", "security section rendered");

    written = espclaw_render_default_config(&c3, buffer, sizeof(buffer));
    assert_true(written > 0, "c3 config render produced data");
    assert_string_contains(buffer, "\"board_profile\": \"esp32c3\"", "c3 board profile rendered");
    assert_string_contains(buffer, "\"backend\": \"littlefs\"", "c3 storage backend rendered");
    assert_string_contains(buffer, "\"enabled\": false", "c3 camera disabled in config");
}

static void test_storage_backend_description(void)
{
    assert_true(strcmp(espclaw_storage_describe_workspace_root("/workspace"), "littlefs") == 0, "littlefs path detected");
    assert_true(strcmp(espclaw_storage_describe_workspace_root("/sdcard/workspace"), "sdcard") == 0, "sdcard path detected");
    assert_true(strcmp(espclaw_storage_describe_workspace_root("/tmp/espclaw"), "host") == 0, "host path detected");
}

static void test_workspace_bootstrap_and_read(void)
{
    char temp_dir[128];
    char file_buffer[256];
    char path_buffer[256];
    struct stat file_stat;

    make_temp_dir(temp_dir, sizeof(temp_dir));

    assert_true(espclaw_workspace_bootstrap(temp_dir) == 0, "workspace bootstrap succeeded");
    assert_true(
        espclaw_workspace_resolve_path(temp_dir, "memory/MEMORY.md", path_buffer, sizeof(path_buffer)) == 0,
        "workspace path resolved"
    );
    assert_true(stat(path_buffer, &file_stat) == 0, "memory file created");
    assert_true(
        espclaw_workspace_read_file(temp_dir, "IDENTITY.md", file_buffer, sizeof(file_buffer)) == 0,
        "identity file readable"
    );
    assert_string_contains(file_buffer, "ESPClaw", "identity template content");
    assert_true(
        espclaw_workspace_resolve_path(temp_dir, "apps", path_buffer, sizeof(path_buffer)) == 0,
        "apps path resolved"
    );
    assert_true(stat(path_buffer, &file_stat) == 0, "apps directory created");
}

static void test_session_store(void)
{
    char temp_dir[128];
    char transcript[2048];

    make_temp_dir(temp_dir, sizeof(temp_dir));

    assert_true(espclaw_session_id_is_valid("chat_001"), "session id accepted");
    assert_true(!espclaw_session_id_is_valid("../oops"), "unsafe session id rejected");
    assert_true(
        espclaw_session_append_message(temp_dir, "chat_001", "user", "hello \"camera\"") == 0,
        "first message appended"
    );
    assert_true(
        espclaw_session_append_message(temp_dir, "chat_001", "assistant", "ready\nnow") == 0,
        "second message appended"
    );
    assert_true(
        espclaw_session_read_transcript(temp_dir, "chat_001", transcript, sizeof(transcript)) == 0,
        "transcript readable"
    );
    assert_string_contains(transcript, "\"role\":\"user\"", "user message persisted");
    assert_string_contains(transcript, "hello \\\"camera\\\"", "quotes escaped");
    assert_string_contains(transcript, "ready\\nnow", "newline escaped");
}

static void test_ota_state_machine(void)
{
    espclaw_ota_state_t state = espclaw_ota_state_init();

    assert_true(espclaw_ota_mark_downloaded(&state, 1), "ota download transition");
    assert_true(espclaw_ota_mark_pending_reboot(&state), "ota pending reboot transition");
    assert_true(espclaw_ota_mark_verifying(&state), "ota verifying transition");
    assert_true(state.rollback_allowed, "rollback enabled during verification");
    assert_true(espclaw_ota_require_rollback(&state), "rollback transition");
    assert_true(state.status == ESPCLAW_OTA_STATUS_ROLLBACK_REQUIRED, "rollback state reached");
}

static void test_admin_ui_asset(void)
{
    const char *html = espclaw_admin_ui_html();

    assert_true(espclaw_admin_ui_length() > 128, "admin ui is non-trivial");
    assert_string_contains(html, "<h1>ESPClaw Admin</h1>", "admin ui heading");
    assert_string_contains(html, "<h2>Workspace</h2>", "workspace section present");
    assert_string_contains(html, "<h2>Tools</h2>", "tools section present");
    assert_string_contains(html, "<h2>Apps</h2>", "apps section present");
    assert_string_contains(html, "Create App", "app mutation action present");
    assert_string_contains(html, "/api/apps/scaffold", "app api wired into ui");
    assert_string_contains(html, "body: $('app-source').value", "save source posts editor contents");
    assert_string_contains(html, "body: $('app-payload').value", "run app posts payload input");
    assert_string_contains(html, "/api/auth/codex", "auth api wired into ui");
    assert_string_contains(html, "/api/tools", "tools api wired into ui");
    assert_string_contains(html, "/api/chat/run", "chat api wired into ui");
    assert_string_contains(html, "Upload Firmware", "ota control present");
}

static void test_admin_ops(void)
{
    char decoded[128];
    char result_json[256];
    char run_json[256];

    assert_true(
        espclaw_admin_query_value("app_id=hello_app&trigger=manual+mode", "app_id", decoded, sizeof(decoded)),
        "app_id decoded from query"
    );
    assert_true(strcmp(decoded, "hello_app") == 0, "decoded app id matches");
    assert_true(
        espclaw_admin_query_value("app_id=hello_app&trigger=manual+mode", "trigger", decoded, sizeof(decoded)),
        "trigger decoded from query"
    );
    assert_true(strcmp(decoded, "manual mode") == 0, "plus-decoded trigger value");
    assert_true(
        espclaw_admin_query_value("payload=quoted%20value", "payload", decoded, sizeof(decoded)),
        "percent-decoded value found"
    );
    assert_true(strcmp(decoded, "quoted value") == 0, "percent-decoded payload");

    assert_true(
        espclaw_admin_render_result_json(true, "saved \"ok\"", result_json, sizeof(result_json)) > 0,
        "result json rendered"
    );
    assert_string_contains(result_json, "\"ok\":true", "result json ok flag");
    assert_string_contains(result_json, "saved \\\"ok\\\"", "result json escaped string");

    assert_true(
        espclaw_admin_render_run_result_json("demo", "manual", true, "done", run_json, sizeof(run_json)) > 0,
        "run result json rendered"
    );
    assert_string_contains(run_json, "\"app_id\":\"demo\"", "run result app id");
    assert_string_contains(run_json, "\"trigger\":\"manual\"", "run result trigger");
    assert_string_contains(run_json, "\"result\":\"done\"", "run result payload");
}

static void test_admin_api_json(void)
{
    char temp_dir[128];
    char status_json[512];
    char files_json[1024];
    char apps_json[2048];
    char tools_json[12288];
    char app_detail_json[4096];
    char auth_json[1024];
    char transcript_json[2048];
    espclaw_board_profile_t profile = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32S3);
    espclaw_ota_state_t state = espclaw_ota_state_init();
    espclaw_auth_profile_t auth_profile;

    make_temp_dir(temp_dir, sizeof(temp_dir));
    assert_true(espclaw_workspace_bootstrap(temp_dir) == 0, "workspace bootstrap for admin api");
    assert_true(espclaw_ota_mark_downloaded(&state, 1), "ota marked downloaded");
    assert_true(
        espclaw_app_scaffold_lua(temp_dir, "admin_demo", "Admin Demo", "fs.read,fs.write", "boot,telegram,manual") == 0,
        "admin api app scaffolded"
    );
    assert_true(
        espclaw_app_update_source(
            temp_dir,
            "admin_demo",
            "function handle(trigger, payload)\n  return \"quoted: \\\"\" .. payload\nend\n"
        ) == 0,
        "admin api source updated"
    );

    assert_true(
        espclaw_render_admin_status_json(
            &profile,
            profile.default_storage_backend,
            "openai_compat",
            "telegram",
            true,
            &state,
            status_json,
            sizeof(status_json)
        ) > 0,
        "status json rendered"
    );
    assert_string_contains(status_json, "\"board_profile\":\"esp32s3\"", "status board profile");
    assert_string_contains(status_json, "\"storage_backend\":\"sdcard\"", "status storage backend");
    assert_string_contains(status_json, "\"workspace_ready\":true", "status workspace flag");
    assert_string_contains(status_json, "\"status\":\"downloaded\"", "status ota state");

    assert_true(
        espclaw_render_workspace_files_json(temp_dir, files_json, sizeof(files_json)) > 0,
        "workspace files json rendered"
    );
    assert_string_contains(files_json, "\"path\":\"AGENTS.md\"", "agents file listed");
    assert_string_contains(files_json, "\"exists\":true", "workspace file exists");

    assert_true(
        espclaw_render_apps_json(temp_dir, apps_json, sizeof(apps_json)) > 0,
        "apps json rendered"
    );
    assert_string_contains(apps_json, "\"id\":\"admin_demo\"", "app id listed");
    assert_string_contains(apps_json, "\"triggers\":[\"boot\",\"telegram\",\"manual\"]", "app triggers listed");

    assert_true(
        espclaw_render_tools_json(tools_json, sizeof(tools_json)) > 0,
        "tools json rendered"
    );
    assert_string_contains(tools_json, "\"name\":\"tool.list\"", "tool list entry rendered");
    assert_string_contains(tools_json, "\"safety\":\"read_only\"", "tool safety rendered");

    assert_true(
        espclaw_render_app_detail_json(temp_dir, "admin_demo", app_detail_json, sizeof(app_detail_json)) > 0,
        "app detail json rendered"
    );
    assert_string_contains(app_detail_json, "\"source_available\":true", "app source availability");
    assert_string_contains(app_detail_json, "\"source\":\"function handle(trigger, payload)\\n", "app source included");
    assert_string_contains(app_detail_json, "quoted:", "app source content preserved");

    espclaw_auth_profile_default(&auth_profile);
    auth_profile.configured = true;
    snprintf(auth_profile.account_id, sizeof(auth_profile.account_id), "acc_demo");
    snprintf(auth_profile.source, sizeof(auth_profile.source), "test");
    assert_true(
        espclaw_render_auth_profile_json(&auth_profile, auth_json, sizeof(auth_json)) > 0,
        "auth profile json rendered"
    );
    assert_string_contains(auth_json, "\"configured\":true", "auth configured rendered");
    assert_string_contains(auth_json, "\"provider_id\":\"openai_codex\"", "auth provider rendered");
    assert_string_contains(auth_json, "\"account_id\":\"acc_demo\"", "auth account rendered");

    assert_true(
        espclaw_session_append_message(temp_dir, "admin_api", "assistant", "hello transcript") == 0,
        "admin transcript session appended"
    );
    assert_true(
        espclaw_render_session_transcript_json(temp_dir, "admin_api", transcript_json, sizeof(transcript_json)) > 0,
        "session transcript json rendered"
    );
    assert_string_contains(transcript_json, "\"session_id\":\"admin_api\"", "transcript session id rendered");
    assert_string_contains(transcript_json, "hello transcript", "transcript content rendered");
}

static void test_auth_store(void)
{
    char temp_dir[128];
    char codex_home[256];
    char auth_path[320];
    espclaw_auth_profile_t profile;
    espclaw_auth_profile_t loaded;
    char message[256];

    make_temp_dir(temp_dir, sizeof(temp_dir));
    assert_true(espclaw_workspace_bootstrap(temp_dir) == 0, "workspace bootstrap for auth store");
    assert_true(espclaw_auth_store_init(temp_dir) == 0, "auth store init");

    espclaw_auth_profile_default(&profile);
    profile.configured = true;
    snprintf(profile.model, sizeof(profile.model), "gpt-5.3-codex");
    snprintf(profile.base_url, sizeof(profile.base_url), "mock://tool-loop");
    snprintf(profile.access_token, sizeof(profile.access_token), "token_demo");
    snprintf(profile.refresh_token, sizeof(profile.refresh_token), "refresh_demo");
    snprintf(profile.account_id, sizeof(profile.account_id), "acc_demo");
    snprintf(profile.source, sizeof(profile.source), "unit_test");

    assert_true(espclaw_auth_profile_is_ready(&profile), "codex auth profile is ready");
    assert_true(espclaw_auth_store_save(&profile) == 0, "auth profile saved");

    memset(&loaded, 0, sizeof(loaded));
    assert_true(espclaw_auth_store_load(&loaded) == 0, "auth profile loaded");
    assert_true(strcmp(loaded.access_token, "token_demo") == 0, "access token loaded");
    assert_true(strcmp(loaded.account_id, "acc_demo") == 0, "account id loaded");
    assert_true(strcmp(loaded.source, "unit_test") == 0, "source loaded");

    espclaw_auth_profile_default(&profile);
    snprintf(profile.provider_id, sizeof(profile.provider_id), "openai_compat");
    snprintf(profile.model, sizeof(profile.model), "gpt-4.1-mini");
    snprintf(profile.base_url, sizeof(profile.base_url), "https://api.openai.com/v1");
    snprintf(profile.access_token, sizeof(profile.access_token), "api_key");
    profile.account_id[0] = '\0';
    profile.configured = true;
    assert_true(espclaw_auth_profile_is_ready(&profile), "openai compatible auth does not require account id");

    snprintf(codex_home, sizeof(codex_home), "%s/codex-home", temp_dir);
    assert_true(mkdir(codex_home, 0700) == 0, "codex home created");
    snprintf(auth_path, sizeof(auth_path), "%s/auth.json", codex_home);
    write_text_file(
        auth_path,
        "{"
        "\"access_token\":\"codex_access\","
        "\"refresh_token\":\"codex_refresh\","
        "\"account_id\":\"acc_codex\""
        "}\n"
    );

    memset(&loaded, 0, sizeof(loaded));
    assert_true(
        espclaw_auth_store_import_codex_cli(codex_home, &loaded, message, sizeof(message)) == 0,
        "codex cli credentials imported"
    );
    assert_true(strcmp(loaded.provider_id, "openai_codex") == 0, "codex provider imported");
    assert_true(strcmp(loaded.access_token, "codex_access") == 0, "codex access token imported");
    assert_true(strcmp(loaded.account_id, "acc_codex") == 0, "codex account imported");
    assert_string_contains(message, "imported Codex credentials", "codex import message");

    assert_true(espclaw_auth_store_clear() == 0, "auth store cleared");
    assert_true(espclaw_auth_store_clear() == 0, "auth store clear tolerates missing file");
}

static void test_provider_request_rendering(void)
{
    char openai_request[4096];
    char anthropic_request[4096];

    assert_true(
        espclaw_render_openai_chat_request(
            "gpt-4.1-mini",
            "You are ESPClaw.",
            "Read AGENTS.md",
            512,
            true,
            openai_request,
            sizeof(openai_request)
        ) > 0,
        "openai request rendered"
    );
    assert_string_contains(openai_request, "\"model\":\"gpt-4.1-mini\"", "openai model included");
    assert_string_contains(openai_request, "\"role\":\"system\"", "openai system message included");
    assert_string_contains(openai_request, "\"tools\":[{", "openai tools included");

    assert_true(
        espclaw_render_anthropic_messages_request(
            "claude-sonnet-4-5",
            "You are ESPClaw.",
            "Capture an image",
            512,
            true,
            anthropic_request,
            sizeof(anthropic_request)
        ) > 0,
        "anthropic request rendered"
    );
    assert_string_contains(anthropic_request, "\"system\":\"You are ESPClaw.\"", "anthropic system included");
    assert_string_contains(anthropic_request, "\"type\":\"text\"", "anthropic text block included");
    assert_string_contains(anthropic_request, "\"input_schema\":{", "anthropic tools included");
}

static void test_telegram_protocol(void)
{
    const char *update_json =
        "{\"ok\":true,\"result\":[{\"update_id\":9001,\"message\":{\"message_id\":12,"
        "\"from\":{\"id\":111,\"is_bot\":false},\"chat\":{\"id\":222,\"type\":\"private\"},"
        "\"date\":1710000000,\"text\":\"/status\"}}]}";
    char payload[1024];
    espclaw_telegram_update_t update;

    assert_true(espclaw_telegram_extract_update(update_json, &update), "telegram update extracted");
    assert_true(update.update_id == 9001, "telegram update id");
    assert_true(strcmp(update.chat_id, "222") == 0, "telegram chat id");
    assert_true(strcmp(update.from_id, "111") == 0, "telegram from id");
    assert_true(strcmp(update.text, "/status") == 0, "telegram text");

    assert_true(
        espclaw_telegram_build_send_message_payload("222", "hello\n\"world\"", payload, sizeof(payload)) > 0,
        "telegram payload built"
    );
    assert_string_contains(payload, "\"chat_id\":\"222\"", "telegram payload chat id");
    assert_string_contains(payload, "hello\\n\\\"world\\\"", "telegram payload escaped text");
}

static void test_app_runtime_manifest_and_scaffold(void)
{
    char temp_dir[128];
    char ids[4][ESPCLAW_APP_ID_MAX + 1];
    char output[256];
    char lua_source[512];
    char updated_source[512];
    char path_buffer[256];
    struct stat file_stat;
    size_t count = 0;
    espclaw_app_manifest_t manifest;
    const char *manifest_json =
        "{"
        "\"id\":\"hello_app\","
        "\"version\":\"0.1.0\","
        "\"title\":\"Hello App\","
        "\"entrypoint\":\"main.lua\","
        "\"permissions\":[\"fs.read\",\"fs.write\"],"
        "\"triggers\":[\"boot\",\"telegram\"]"
        "}";

    assert_true(espclaw_app_parse_manifest_json(manifest_json, &manifest) == 0, "manifest parsed");
    assert_true(strcmp(manifest.app_id, "hello_app") == 0, "manifest id parsed");
    assert_true(espclaw_app_has_permission(&manifest, "fs.write"), "manifest permission parsed");
    assert_true(espclaw_app_supports_trigger(&manifest, "telegram"), "manifest trigger parsed");
    assert_true(!espclaw_app_supports_trigger(&manifest, "manual"), "unsupported trigger rejected");

    make_temp_dir(temp_dir, sizeof(temp_dir));
    assert_true(
        espclaw_app_scaffold_lua(temp_dir, "hello_app", "Hello App", "fs.read,fs.write", "boot,telegram") == 0,
        "lua app scaffolded"
    );
    assert_true(espclaw_app_load_manifest(temp_dir, "hello_app", &manifest) == 0, "scaffold manifest load");
    assert_true(strcmp(manifest.entrypoint, "main.lua") == 0, "entrypoint loaded");
    assert_true(espclaw_app_collect_ids(temp_dir, ids, 4, &count) == 0, "apps collected");
    assert_true(count == 1, "one app collected");
    assert_true(strcmp(ids[0], "hello_app") == 0, "app id collected");
    assert_true(
        espclaw_workspace_read_file(temp_dir, "apps/hello_app/main.lua", lua_source, sizeof(lua_source)) == 0,
        "lua script readable"
    );
    assert_string_contains(lua_source, "function handle", "lua scaffold contains handle");
    assert_true(
        espclaw_app_read_source(temp_dir, "hello_app", updated_source, sizeof(updated_source)) == 0,
        "app source readable"
    );
    assert_string_contains(updated_source, "hello_app ready", "initial app source content");
    assert_true(
        espclaw_app_update_source(
            temp_dir,
            "hello_app",
            "function handle(trigger, payload)\n  return \"patched:\" .. payload\nend\n"
        ) == 0,
        "app source updated"
    );
    assert_true(
        espclaw_app_read_source(temp_dir, "hello_app", updated_source, sizeof(updated_source)) == 0,
        "updated source readable"
    );
    assert_string_contains(updated_source, "patched:", "updated source content");
    assert_true(
        espclaw_app_run(temp_dir, "hello_app", "telegram", "ping", output, sizeof(output))
#ifdef ESPCLAW_HOST_LUA
            == 0
#else
            != 0
#endif
        ,
        "app runtime result matches build capabilities"
    );
#ifdef ESPCLAW_HOST_LUA
    assert_string_contains(output, "patched:ping", "host lua runtime executed app");
#else
    assert_string_contains(output, "firmware builds", "host runtime reports limitation");
#endif
    assert_true(
        espclaw_workspace_resolve_path(temp_dir, "apps/hello_app", path_buffer, sizeof(path_buffer)) == 0,
        "app directory path resolved"
    );
    assert_true(stat(path_buffer, &file_stat) == 0, "app directory exists before delete");
    assert_true(espclaw_app_remove(temp_dir, "hello_app") == 0, "app removed");
    assert_true(stat(path_buffer, &file_stat) != 0, "app directory removed");
}

static void test_app_runtime_hardware_bindings(void)
{
    char temp_dir[128];
    char output[256];
    uint8_t uart_output[64];
    uint8_t i2c_registers[] = {0x12, 0x34};
    espclaw_hw_pwm_state_t pwm_state;
    size_t uart_output_length = 0;

    make_temp_dir(temp_dir, sizeof(temp_dir));
    assert_true(
        espclaw_app_scaffold_lua(
            temp_dir,
            "hardware_app",
            "Hardware App",
            "gpio.read,gpio.write,pwm.write,adc.read,i2c.read,i2c.write,buzzer.play,uart.read,uart.write",
            "manual"
        ) == 0,
        "hardware app scaffolded"
    );
    assert_true(
        espclaw_app_update_source(
            temp_dir,
            "hardware_app",
            "function handle(trigger, payload)\n"
            "  assert(espclaw.i2c.begin(0, 21, 22, 400000))\n"
            "  local devices = espclaw.i2c.scan(0)\n"
            "  local bytes = espclaw.i2c.read_reg(0, 0x48, 0x00, 2)\n"
            "  local adc = espclaw.adc.read_raw(1, 3)\n"
            "  assert(espclaw.gpio.write(4, 1))\n"
            "  local pin = espclaw.gpio.read(4)\n"
            "  assert(espclaw.pwm.setup(0, 12, 4000, 10))\n"
            "  assert(espclaw.pwm.write(0, 512))\n"
            "  local pwm = espclaw.pwm.state(0)\n"
            "  local uart_in = espclaw.uart.read(0, 8)\n"
            "  local uart_written = espclaw.uart.write(0, \"uart:ok\\n\")\n"
            "  local output = espclaw.pid.step(2000, adc, 0, 0, 0.5, 0.1, 0.0, 0.02, 0, 1023)\n"
            "  return string.format(\"adc=%d temp=%d pin=%d i2c=%d pwm=%d pid=%d uart_in=%s uart_written=%d\", adc, bytes[1] * 256 + bytes[2], pin, #devices, pwm.duty, math.floor(output), uart_in, uart_written)\n"
            "end\n"
        ) == 0,
        "hardware app source updated"
    );

#ifdef ESPCLAW_HOST_LUA
    espclaw_hw_sim_reset();
    espclaw_hw_sim_set_adc_raw(1, 3, 1234);
    espclaw_hw_sim_set_i2c_reg(0, 0x48, 0x00, i2c_registers, sizeof(i2c_registers));
    espclaw_hw_sim_uart_feed_input(0, (const uint8_t *)"host:in", strlen("host:in"));

    assert_true(
        espclaw_app_run(temp_dir, "hardware_app", "manual", "", output, sizeof(output)) == 0,
        "hardware app ran in host simulator"
    );
    assert_string_contains(output, "adc=1234", "adc reading returned to lua");
    assert_string_contains(output, "temp=4660", "i2c register bytes returned to lua");
    assert_string_contains(output, "pin=1", "gpio loopback returned to lua");
    assert_string_contains(output, "i2c=1", "i2c scan saw simulated device");
    assert_string_contains(output, "pwm=512", "pwm state returned to lua");
    assert_string_contains(output, "pid=384", "pid helper returned deterministic output");
    assert_string_contains(output, "uart_in=host:in", "uart input returned to lua");
    assert_string_contains(output, "uart_written=8", "uart write count returned to lua");
    assert_true(espclaw_hw_pwm_state(0, &pwm_state) == 0, "pwm state query succeeded");
    assert_true(pwm_state.configured, "pwm channel configured");
    assert_true(pwm_state.duty == 512, "pwm duty tracked");
    assert_true(
        espclaw_hw_sim_uart_take_output(0, uart_output, sizeof(uart_output), &uart_output_length) == 0,
        "uart output capture succeeded"
    );
    uart_output[uart_output_length] = '\0';
    assert_string_contains((const char *)uart_output, "uart:ok", "uart output reached simulator console capture");
#else
    assert_true(
        espclaw_app_run(temp_dir, "hardware_app", "manual", "", output, sizeof(output)) != 0,
        "hardware app requires host or firmware lua runtime"
    );
#endif

    assert_true(
        espclaw_app_scaffold_lua(temp_dir, "restricted_app", "Restricted App", "gpio.read", "manual") == 0,
        "restricted app scaffolded"
    );
    assert_true(
        espclaw_app_update_source(
            temp_dir,
            "restricted_app",
            "function handle(trigger, payload)\n"
            "  local value, err = espclaw.adc.read_raw(1, 0)\n"
            "  if value ~= nil then\n"
            "    return \"unexpected\"\n"
            "  end\n"
            "  return err\n"
            "end\n"
        ) == 0,
        "restricted app source updated"
    );
    assert_true(
        espclaw_app_run(temp_dir, "restricted_app", "manual", "", output, sizeof(output))
#ifdef ESPCLAW_HOST_LUA
            == 0
#else
            != 0
#endif
        ,
        "restricted app permission path matches build capabilities"
    );
#ifdef ESPCLAW_HOST_LUA
    assert_string_contains(output, "permission denied for adc.read", "hardware permission enforced");
#endif
}

static void test_app_runtime_vehicle_bindings(void)
{
    char temp_dir[128];
    char output[512];
    uint8_t tmp102_registers[] = {0x19, 0x10};
    uint8_t mpu6050_registers[] = {
        0x10, 0x00,
        0x20, 0x00,
        0x40, 0x00,
        0x0D, 0x48,
        0x05, 0x1E,
        0xFD, 0x71,
        0x0A, 0x3C
    };
    espclaw_hw_pwm_state_t pwm_state;
    espclaw_hw_ppm_state_t ppm_state;

    make_temp_dir(temp_dir, sizeof(temp_dir));
    assert_true(
        espclaw_app_scaffold_lua(
            temp_dir,
            "vehicle_app",
            "Vehicle App",
            "adc.read,pwm.write,ppm.write,imu.read,temperature.read",
            "manual"
        ) == 0,
        "vehicle app scaffolded"
    );
    assert_true(
        espclaw_app_update_source(
            temp_dir,
            "vehicle_app",
            "function handle(trigger, payload)\n"
            "  assert(espclaw.i2c.begin(0, 21, 22, 400000))\n"
            "  assert(espclaw.imu.mpu6050_begin(0, 0x68))\n"
            "  local sample = espclaw.imu.mpu6050_read(0, 0x68)\n"
            "  local roll, pitch = espclaw.imu.complementary_roll_pitch(sample, 0, 0, 0.98, 0.01)\n"
            "  local temp = espclaw.temperature.tmp102_c(0, 0x48)\n"
            "  local battery_mv = espclaw.adc.read_mv(1, 3)\n"
            "  local drive = espclaw.control.mix_differential(0.6, -0.2, -1, 1)\n"
            "  local quad = espclaw.control.mix_quad_x(0.5, 0.1, -0.05, 0.02, 0, 1)\n"
            "  assert(espclaw.servo.attach(1, 13))\n"
            "  local pulse = espclaw.servo.write_norm(1, drive.left, 1000, 2000)\n"
            "  assert(espclaw.ppm.begin(0, 14, 22500, 300))\n"
            "  assert(espclaw.ppm.write(0, {1000, 1500, 1500, 2000}))\n"
            "  local pwm = espclaw.pwm.state(1)\n"
            "  local ppm = espclaw.ppm.state(0)\n"
            "  return string.format(\"mv=%d temp=%.1f imu=%.1f roll=%.1f pitch=%.1f pulse=%d ppm=%d quad=%.2f/%.2f\", battery_mv, temp, sample.temperature_c, roll, pitch, pulse, ppm.outputs[4], quad.front_left, quad.rear_left)\n"
            "end\n"
        ) == 0,
        "vehicle app source updated"
    );

#ifdef ESPCLAW_HOST_LUA
    espclaw_hw_sim_reset();
    espclaw_hw_sim_set_adc_raw(1, 3, 1234);
    espclaw_hw_sim_set_i2c_reg(0, 0x48, 0x00, tmp102_registers, sizeof(tmp102_registers));
    espclaw_hw_sim_set_i2c_reg(0, 0x68, 0x3B, mpu6050_registers, sizeof(mpu6050_registers));

    assert_true(
        espclaw_app_run(temp_dir, "vehicle_app", "manual", "", output, sizeof(output)) == 0,
        "vehicle app ran in host simulator"
    );
    assert_string_contains(output, "mv=994", "adc millivolts converted");
    assert_string_contains(output, "temp=25.1", "tmp102 temperature converted");
    assert_string_contains(output, "imu=46.5", "mpu6050 temperature converted");
    assert_string_contains(output, "roll=0.6", "complementary roll estimate produced");
    assert_string_contains(output, "pitch=-0.3", "complementary pitch estimate produced");
    assert_string_contains(output, "pulse=1700", "servo normalized write mapped to pulse");
    assert_string_contains(output, "ppm=2000", "ppm frame tracked last channel");
    assert_string_contains(output, "quad=0.53/0.67", "quad mixer output produced");
    assert_true(espclaw_hw_pwm_state(1, &pwm_state) == 0, "servo pwm state query succeeded");
    assert_true(pwm_state.pulse_width_us == 1700, "servo pulse width tracked");
    assert_true(espclaw_hw_ppm_state(0, &ppm_state) == 0, "ppm state query succeeded");
    assert_true(ppm_state.output_count == 4, "ppm output count tracked");
    assert_true(ppm_state.outputs[0] == 1000, "ppm first output tracked");
    assert_true(ppm_state.outputs[3] == 2000, "ppm fourth output tracked");
#else
    assert_true(
        espclaw_app_run(temp_dir, "vehicle_app", "manual", "", output, sizeof(output)) != 0,
        "vehicle app requires host or firmware lua runtime"
    );
#endif
}

static void test_app_vm_and_control_loops(void)
{
    char temp_dir[128];
    char output[256];
    espclaw_app_vm_t *vm = NULL;
    espclaw_control_loop_status_t loops[ESPCLAW_CONTROL_LOOP_MAX];
    size_t count = 0;
    size_t index = 0;

    make_temp_dir(temp_dir, sizeof(temp_dir));
    assert_true(
        espclaw_app_scaffold_lua(temp_dir, "loop_app", "Loop App", "fs.read", "manual") == 0,
        "loop app scaffolded"
    );
    assert_true(
        espclaw_app_update_source(
            temp_dir,
            "loop_app",
            "counter = counter or 0\n"
            "function handle(trigger, payload)\n"
            "  counter = counter + 1\n"
            "  return string.format(\"step=%d payload=%s\", counter, payload)\n"
            "end\n"
        ) == 0,
        "loop app source updated"
    );

#ifdef ESPCLAW_HOST_LUA
    assert_true(
        espclaw_app_vm_open(temp_dir, "loop_app", &vm, output, sizeof(output)) == 0,
        "persistent vm opened"
    );
    assert_true(
        espclaw_app_vm_step(vm, "manual", "alpha", output, sizeof(output)) == 0,
        "first vm step succeeded"
    );
    assert_string_contains(output, "step=1 payload=alpha", "vm persisted first step");
    assert_true(
        espclaw_app_vm_step(vm, "manual", "beta", output, sizeof(output)) == 0,
        "second vm step succeeded"
    );
    assert_string_contains(output, "step=2 payload=beta", "vm preserved lua globals");
    espclaw_app_vm_close(vm);
    vm = NULL;

    assert_true(
        espclaw_control_loop_start("demo_loop", temp_dir, "loop_app", "manual", "tick", 5, 3, output, sizeof(output)) == 0,
        "control loop started"
    );

    for (index = 0; index < 50; ++index) {
        espclaw_hw_sleep_ms(5);
        count = espclaw_control_loop_snapshot_all(loops, ESPCLAW_CONTROL_LOOP_MAX);
        if (count > 0 && loops[0].completed) {
            break;
        }
    }

    assert_true(count > 0, "control loop snapshot returned data");
    assert_true(loops[0].completed, "control loop completed");
    assert_true(loops[0].iterations_completed == 3, "control loop iteration count tracked");
    assert_string_contains(loops[0].last_result, "step=3 payload=tick", "control loop reused persistent lua state");
    espclaw_control_loop_shutdown_all();
#else
    assert_true(
        espclaw_app_vm_open(temp_dir, "loop_app", &vm, output, sizeof(output)) != 0,
        "persistent vm is only available in host or firmware lua runtime"
    );
#endif
}

static void test_agent_loop(void)
{
    char temp_dir[128];
    char transcript[8192];
    espclaw_auth_profile_t profile;
    espclaw_agent_run_result_t result;
    test_agent_adapter_state_t adapter_state = {0};

    make_temp_dir(temp_dir, sizeof(temp_dir));
    assert_true(espclaw_workspace_bootstrap(temp_dir) == 0, "workspace bootstrap for agent loop");
    assert_true(espclaw_auth_store_init(temp_dir) == 0, "agent loop auth store init");
    assert_true(
        espclaw_app_scaffold_lua(temp_dir, "demo_app", "Demo App", "fs.read", "manual,telegram") == 0,
        "agent loop demo app scaffolded"
    );

    espclaw_auth_profile_default(&profile);
    profile.configured = true;
    snprintf(profile.base_url, sizeof(profile.base_url), "mock://tool-loop");
    snprintf(profile.access_token, sizeof(profile.access_token), "token_demo");
    snprintf(profile.account_id, sizeof(profile.account_id), "acc_demo");
    snprintf(profile.source, sizeof(profile.source), "unit_test");
    assert_true(espclaw_auth_store_save(&profile) == 0, "agent loop auth profile saved");

    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_agent_loop_run(temp_dir, "chat_tool_loop", "What apps are installed?", false, &result) == 0,
        "tool loop run succeeded"
    );
    assert_true(result.ok, "tool loop result ok");
    assert_true(result.used_tools, "tool loop used tools");
    assert_true(result.iterations == 2, "tool loop used two response rounds");
    assert_string_contains(result.final_text, "listed the installed apps", "tool loop final text");
    assert_true(
        espclaw_session_read_transcript(temp_dir, "chat_tool_loop", transcript, sizeof(transcript)) == 0,
        "tool loop transcript readable"
    );
    assert_string_contains(transcript, "What apps are installed?", "tool loop stored user message");
    assert_string_contains(transcript, "Requested tools: system.info, app.list", "tool loop stored tool summary");
    assert_string_contains(transcript, "\"role\":\"tool\"", "tool loop stored tool output");

    snprintf(profile.base_url, sizeof(profile.base_url), "mock://fs-read");
    assert_true(espclaw_auth_store_save(&profile) == 0, "fs read auth profile saved");
    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_agent_loop_run(temp_dir, "chat_fs_read", "Read the control file.", false, &result) == 0,
        "fs read loop succeeded"
    );
    assert_string_contains(result.final_text, "read the requested file", "fs read final text");
    assert_true(
        espclaw_session_read_transcript(temp_dir, "chat_fs_read", transcript, sizeof(transcript)) == 0,
        "fs read transcript readable"
    );
    assert_string_contains(transcript, "\"role\":\"assistant\"", "fs read transcript captured assistant output");
    assert_string_contains(transcript, "I read the requested file and summarized it.", "fs read final assistant text was recorded");

    adapter_state.calls = 0;
    espclaw_agent_set_http_adapter(tool_list_http_adapter, &adapter_state);
    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_agent_loop_run(temp_dir, "chat_tools_via_model", "List out all the available tools to you", false, &result) == 0,
        "tool listing via model succeeded"
    );
    espclaw_agent_set_http_adapter(NULL, NULL);
    assert_true(result.ok, "tool listing via model result ok");
    assert_true(result.used_tools, "tool listing via model used tool.list");
    assert_string_contains(result.final_text, "inspect files, apps, hardware", "tool listing final text returned");
    assert_true(
        espclaw_session_read_transcript(temp_dir, "chat_tools_via_model", transcript, sizeof(transcript)) == 0,
        "tool listing via model transcript readable"
    );
    assert_string_contains(transcript, "Requested tools: tool.list", "tool listing transcript captured tool request");
    assert_string_contains(transcript, "fs.read", "tool listing transcript captured tool output");

    adapter_state.calls = 0;
    espclaw_agent_set_http_adapter(confirmation_http_adapter, &adapter_state);
    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_agent_loop_run(temp_dir, "chat_confirm", "Run the demo app.", false, &result) == 0,
        "confirmation loop completed"
    );
    espclaw_agent_set_http_adapter(NULL, NULL);
    assert_true(adapter_state.calls == 2, "confirmation adapter saw two calls");
    assert_true(result.ok, "confirmation loop returned assistant response");
    assert_true(result.used_tools, "confirmation loop used tools");
    assert_string_contains(result.final_text, "Please confirm", "confirmation final text");
    assert_true(
        espclaw_session_read_transcript(temp_dir, "chat_confirm", transcript, sizeof(transcript)) == 0,
        "confirmation transcript readable"
    );
    assert_string_contains(transcript, "confirmation_required", "confirmation tool output captured");
}

int main(void)
{
    test_board_profiles();
    test_workspace_manifest();
    test_provider_and_channel_registry();
    test_tool_catalog();
    test_default_config_render();
    test_storage_backend_description();
    test_workspace_bootstrap_and_read();
    test_session_store();
    test_ota_state_machine();
    test_admin_ui_asset();
    test_admin_ops();
    test_admin_api_json();
    test_auth_store();
    test_provider_request_rendering();
    test_telegram_protocol();
    test_app_runtime_manifest_and_scaffold();
    test_app_runtime_hardware_bindings();
    test_app_runtime_vehicle_bindings();
    test_app_vm_and_control_loops();
    test_agent_loop();

    printf("espclaw core tests passed\n");
    return 0;
}
