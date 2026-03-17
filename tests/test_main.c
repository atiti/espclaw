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
#include "espclaw/behavior_runtime.h"
#include "espclaw/board_config.h"
#include "espclaw/board_profile.h"
#include "espclaw/channel.h"
#include "espclaw/config_render.h"
#include "espclaw/console_chat.h"
#include "espclaw/control_loop.h"
#include "espclaw/event_watch.h"
#include "espclaw/hardware.h"
#include "espclaw/lua_api_registry.h"
#include "espclaw/ota_manager.h"
#include "espclaw/ota_state.h"
#include "espclaw/provisioning.h"
#include "espclaw/provider.h"
#include "espclaw/provider_request.h"
#include "espclaw/runtime.h"
#include "espclaw/session_store.h"
#include "espclaw/storage.h"
#include "espclaw/task_policy.h"
#include "espclaw/task_runtime.h"
#include "espclaw/telegram_protocol.h"
#include "espclaw/tool_catalog.h"
#include "espclaw/web_tools.h"
#include "espclaw/workspace.h"

#ifndef ESPCLAW_SOURCE_DIR
#define ESPCLAW_SOURCE_DIR "."
#endif

#ifndef MBEDTLS_X509_BADCERT_NOT_TRUSTED
#define MBEDTLS_X509_BADCERT_NOT_TRUSTED 0x08
#define MBEDTLS_X509_BADCERT_FUTURE 0x0200
#define MBEDTLS_X509_BADCERT_BAD_MD 0x4000
#endif

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

static void assert_string_not_contains(const char *haystack, const char *needle, const char *message)
{
    assert_true(haystack != NULL, "haystack must not be null");
    assert_true(strstr(haystack, needle) == NULL, message);
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

static void read_text_file(const char *path, char *buffer, size_t buffer_size)
{
    FILE *file = fopen(path, "r");
    size_t read_len;

    assert_true(file != NULL, "fopen for read fixture failed");
    read_len = fread(buffer, 1, buffer_size - 1, file);
    assert_true(!ferror(file), "fread for read fixture failed");
    buffer[read_len] = '\0';
    fclose(file);
}

typedef struct {
    unsigned int calls;
} test_agent_adapter_state_t;

static espclaw_runtime_status_t s_console_status;
static bool s_console_reboot_requested;
static bool s_console_factory_reset_requested;
static espclaw_telegram_config_t s_console_telegram_config;

static const espclaw_runtime_status_t *console_status_adapter(void)
{
    return &s_console_status;
}

static int console_wifi_scan_adapter(espclaw_wifi_network_t *networks, size_t max_networks, size_t *count_out)
{
    if (networks == NULL || max_networks < 2 || count_out == NULL) {
        return -1;
    }
    memset(networks, 0, sizeof(*networks) * max_networks);
    snprintf(networks[0].ssid, sizeof(networks[0].ssid), "LabNet");
    networks[0].rssi = -41;
    networks[0].channel = 6;
    networks[0].secure = true;
    snprintf(networks[1].ssid, sizeof(networks[1].ssid), "OpenField");
    networks[1].rssi = -67;
    networks[1].channel = 11;
    networks[1].secure = false;
    *count_out = 2;
    return 0;
}

static int console_wifi_join_adapter(const char *ssid, const char *password, char *message, size_t message_size)
{
    assert_true(ssid != NULL, "console wifi join ssid provided");
    assert_true(strcmp(ssid, "LabNet") == 0, "console wifi join parsed ssid");
    assert_true(password != NULL, "console wifi join password provided");
    assert_true(strcmp(password, "secret pass") == 0, "console wifi join parsed password");
    snprintf(message, message_size, "connecting to %s", ssid);
    return 0;
}

static int console_factory_reset_adapter(char *message, size_t message_size)
{
    s_console_factory_reset_requested = true;
    snprintf(message, message_size, "Factory reset requested.");
    return 0;
}

static int console_telegram_get_config_adapter(espclaw_telegram_config_t *config)
{
    assert_true(config != NULL, "console telegram get config buffer provided");
    *config = s_console_telegram_config;
    return 0;
}

static int console_telegram_set_config_adapter(
    const espclaw_telegram_config_t *config,
    char *message,
    size_t message_size
)
{
    assert_true(config != NULL, "console telegram set config provided");
    s_console_telegram_config = *config;
    s_console_telegram_config.configured = s_console_telegram_config.bot_token[0] != '\0';
    snprintf(
        message,
        message_size,
        "Telegram config saved (%s, %lu s poll interval).",
        s_console_telegram_config.configured ? "configured" : "cleared",
        (unsigned long)s_console_telegram_config.poll_interval_seconds
    );
    return 0;
}

static void console_reboot_adapter(void)
{
    s_console_reboot_requested = true;
}

static int web_tool_http_adapter(const char *url, char *response, size_t response_size, void *user_data)
{
    (void)user_data;

    if (strstr(url, "/search?") != NULL) {
        assert_string_contains(url, "ms5611%20datasheet", "search url encodes query");
        snprintf(
            response,
            response_size,
            "{"
            "\"query\":\"ms5611 datasheet\","
            "\"provider\":\"llmproxy\","
            "\"results\":["
            "{\"position\":1,\"title\":\"MS5611 datasheet\",\"url\":\"https://example.com/ms5611.pdf\",\"snippet\":\"Pressure sensor PDF\",\"source\":\"vendor\"},"
            "{\"position\":2,\"title\":\"Altimeter notes\",\"url\":\"https://example.com/notes\",\"snippet\":\"Application note\",\"source\":\"docs\"}"
            "]"
            "}"
        );
        return 0;
    }
    if (strstr(url, "/scrape?") != NULL) {
        assert_string_contains(url, "https%3A%2F%2Fexample.com%2Fdoc.pdf", "fetch url encodes target");
        snprintf(
            response,
            response_size,
            "{"
            "\"url\":\"https://example.com/doc.pdf\","
            "\"title\":\"MS5611 PDF\","
            "\"excerpt\":\"Calibration constants and commands.\","
            "\"markdown\":\"# MS5611\\n\\nPROM read sequence and conversion commands.\""
            "}"
        );
        return 0;
    }

    return -1;
}

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
    char source[2048];
    size_t used = 0;
    int written = 0;

    (void)url;
    (void)profile;

    if (state != NULL) {
        state->calls++;
    }

    assert_string_contains(body, "\"store\":false", "agent loop sends store=false");
    assert_string_contains(body, "\"stream\":true", "agent loop sends stream=true for codex");
    assert_string_contains(body, "Tool Inventory Snapshot", "system prompt includes tool inventory snapshot");
    assert_string_contains(body, "hardware.list", "system prompt includes hardware tool");
    assert_string_contains(body, "behavior.register", "system prompt includes behavior tool");
    assert_string_contains(body, "task.start", "system prompt includes task tool");
    assert_string_contains(body, "Mutating tools require explicit confirmation", "read-only runs preserve confirmation policy");
    assert_string_contains(body, "tool-call compliance test", "system prompt includes compliance guidance");
    assert_string_contains(body, "explicitly tells you to run a tool by name", "system prompt includes explicit tool execution guidance");

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
    const char *source =
        "function on_sensor(payload)\\\\n"
        "  local banner = 'LLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLL"
        "LLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLL'\\\\n"
        "  return 'sensor:' .. payload\\\\n"
        "end\\\\n"
        "function handle(trigger, payload)\\\\n"
        "  return 'ready'\\\\n"
        "end\\\\n";
    int written = 0;

    (void)url;
    (void)profile;

    if (state != NULL) {
        state->calls++;
    }

    assert_string_contains(body, "\"store\":false", "tool list loop sends store=false");
    assert_string_contains(body, "\"stream\":true", "tool list loop sends stream=true for codex");
    assert_string_contains(body, "Tool Inventory Snapshot", "tool list prompt includes tool inventory snapshot");
    assert_string_contains(body, "hardware.list", "tool list prompt includes hardware tool");
    assert_string_contains(body, "lua_api.list", "tool list prompt includes lua api tool");
    assert_string_contains(body, "behavior.register", "tool list prompt includes behavior tool");
    assert_string_contains(body, "tool-call compliance test", "tool list prompt includes compliance guidance");
    assert_string_contains(body, "explicitly tells you to run a tool by name", "tool list prompt includes explicit tool execution guidance");
    assert_string_not_contains(body, "# Lua App Contract", "tool list prompt does not inject lua app snapshot for non-app runs");

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

static int app_install_http_adapter(
    const char *url,
    const espclaw_auth_profile_t *profile,
    const char *body,
    char *response,
    size_t response_size,
    void *user_data
)
{
    test_agent_adapter_state_t *state = (test_agent_adapter_state_t *)user_data;
    char source[4096];
    char banner[3400];
    int written = 0;

    (void)url;
    (void)profile;

    if (state != NULL) {
        state->calls++;
    }

    assert_string_contains(body, "Tool Inventory Snapshot", "app install prompt includes tool inventory snapshot");
    assert_string_contains(body, "app.install", "app install prompt includes app install tool");
    assert_string_contains(body, "event.emit", "app install prompt includes event tool");
    assert_string_contains(body, "lua_api.list", "app install prompt includes lua api tool");
    assert_string_contains(body, "# Lua App Contract", "app install prompt injects lua app contract");
    assert_string_contains(body, "espclaw.pwm.setup", "app install prompt includes lua api snapshot");
    assert_string_contains(body, "function handle(trigger, payload)", "app install prompt includes handler contract");

    if (state != NULL && state->calls > 1) {
        assert_string_contains(body, "\"type\":\"function_call_output\"", "app install follow-up includes tool output");
        snprintf(
            response,
            response_size,
            "{"
            "\"id\":\"resp_install_final\","
            "\"status\":\"completed\","
            "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"I installed the Lua app and it is ready to start as a task.\"}]}]"
            "}"
        );
        return 0;
    }

    memset(banner, 'L', sizeof(banner) - 1);
    banner[sizeof(banner) - 1] = '\0';
    written = snprintf(
        source,
        sizeof(source),
        "function on_sensor(payload)\\\\n"
        "  local banner = '%s'\\\\n"
        "  return 'sensor:' .. payload\\\\n"
        "end\\\\n"
        "function handle(trigger, payload)\\\\n"
        "  return 'ready'\\\\n"
        "end\\\\n",
        banner
    );
    assert_true(written > 3000, "app install source exceeds larger app threshold");
    written = snprintf(
        response,
        response_size,
        "{"
        "\"id\":\"resp_install_first\","
        "\"status\":\"completed\","
        "\"output\":["
        "{\"type\":\"function_call\",\"call_id\":\"call_install\",\"name\":\"app.install\",\"arguments\":\"{\\\"name\\\":\\\"Sensor Agent\\\",\\\"permissions\\\":\\\"task.control,uart.read,uart.write\\\",\\\"triggers\\\":\\\"manual,sensor\\\",\\\"lua\\\":\\\"%s\\\"}\",\"status\":\"completed\"}"
        "]"
        "}",
        source
    );
    assert_true(written > 0 && (size_t)written < response_size, "app install mock response serialized");
    return 0;
}

static int app_install_retry_http_adapter(
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

    if (state != NULL && state->calls == 1) {
        assert_string_contains(body, "# Lua App Contract", "app retry initial prompt includes lua contract");
        snprintf(
            response,
            response_size,
            "{"
            "\"id\":\"resp_retry_first\","
            "\"status\":\"completed\","
            "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"INSTALLED\"}]}]"
            "}"
        );
        return 0;
    }

    if (state != NULL && state->calls == 2) {
        assert_string_contains(body, "must not claim that an app was installed", "retry turn injects correction");
        snprintf(
            response,
            response_size,
            "{"
            "\"id\":\"resp_retry_second\","
            "\"status\":\"completed\","
            "\"output\":["
            "{\"type\":\"function_call\",\"call_id\":\"call_install_retry\",\"name\":\"app.install\",\"arguments\":\"{\\\"app_id\\\":\\\"retry_app\\\",\\\"title\\\":\\\"Retry App\\\",\\\"source\\\":\\\"function handle(trigger, payload)\\\\n  return 'retry-ok'\\\\nend\\\\n\\\",\\\"permissions_csv\\\":\\\"fs.read\\\",\\\"triggers_csv\\\":\\\"manual\\\"}\",\"status\":\"completed\"}"
            "]"
            "}"
        );
        return 0;
    }

    assert_string_contains(body, "\"type\":\"function_call_output\"", "retry follow-up includes tool output");
    snprintf(
        response,
        response_size,
        "{"
        "\"id\":\"resp_retry_final\","
        "\"status\":\"completed\","
        "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"INSTALLED\"}]}]"
        "}"
    );
    return 0;
}

static int app_install_wrong_tool_retry_http_adapter(
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

    if (state != NULL && state->calls == 1) {
        snprintf(
            response,
            response_size,
            "{"
            "\"id\":\"resp_wrong_tool_first\","
            "\"status\":\"completed\","
            "\"output\":["
            "{\"type\":\"function_call\",\"call_id\":\"call_hw\",\"name\":\"hardware.list\",\"arguments\":\"{}\",\"status\":\"completed\"}"
            "]"
            "}"
        );
        return 0;
    }

    if (state != NULL && state->calls == 2) {
        assert_string_contains(body, "must not claim that an app was installed", "wrong-tool retry injects correction");
        snprintf(
            response,
            response_size,
            "{"
            "\"id\":\"resp_wrong_tool_second\","
            "\"status\":\"completed\","
            "\"output\":["
            "{\"type\":\"function_call\",\"call_id\":\"call_install_after_detour\",\"name\":\"app.install\",\"arguments\":\"{\\\"app_id\\\":\\\"retry_after_detour\\\",\\\"title\\\":\\\"Retry After Detour\\\",\\\"source\\\":\\\"function handle(trigger, payload)\\\\n  return 'detour-ok'\\\\nend\\\\n\\\",\\\"permissions_csv\\\":\\\"fs.read\\\",\\\"triggers_csv\\\":\\\"manual\\\"}\",\"status\":\"completed\"}"
            "]"
            "}"
        );
        return 0;
    }

    assert_string_contains(body, "\"type\":\"function_call_output\"", "wrong-tool retry final follow-up includes tool output");
    snprintf(
        response,
        response_size,
        "{"
        "\"id\":\"resp_wrong_tool_final\","
        "\"status\":\"completed\","
        "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"INSTALLED\"}]}]"
        "}"
    );
    return 0;
}

static int web_search_fetch_retry_http_adapter(
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

    if (state != NULL && state->calls == 1) {
        snprintf(
            response,
            response_size,
            "{"
            "\"id\":\"resp_web_first\","
            "\"status\":\"completed\","
            "\"output\":["
            "{\"type\":\"function_call\",\"call_id\":\"call_web_search\",\"name\":\"web.search\",\"arguments\":\"{\\\"query\\\":\\\"ms5611 datasheet\\\"}\",\"status\":\"completed\"}"
            "]"
            "}"
        );
        return 0;
    }

    if (state != NULL && state->calls == 2) {
        assert_string_contains(body, "explicitly requires both web.search and web.fetch", "web retry injects correction");
        snprintf(
            response,
            response_size,
            "{"
            "\"id\":\"resp_web_second\","
            "\"status\":\"completed\","
            "\"output\":["
            "{\"type\":\"function_call\",\"call_id\":\"call_web_fetch\",\"name\":\"web.fetch\",\"arguments\":\"{\\\"url\\\":\\\"https://example.com/doc.pdf\\\"}\",\"status\":\"completed\"}"
            "]"
            "}"
        );
        return 0;
    }

    assert_string_contains(body, "\"type\":\"function_call_output\"", "web retry final follow-up includes tool output");
    snprintf(
        response,
        response_size,
        "{"
        "\"id\":\"resp_web_final\","
        "\"status\":\"completed\","
        "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"Used both web.search and web.fetch successfully.\"}]}]"
        "}"
    );
    return 0;
}

static void test_transport_error_formatting(void)
{
    char message[256];

    assert_true(
        espclaw_agent_format_transport_error(
            28674,
            -0x2700,
            MBEDTLS_X509_BADCERT_NOT_TRUSTED | MBEDTLS_X509_BADCERT_BAD_MD | MBEDTLS_X509_BADCERT_FUTURE,
            message,
            sizeof(message)) == 0,
        "transport error formatting succeeds"
    );
    assert_string_contains(message, "Provider transport failed:", "transport error prefix present");
    assert_string_contains(message, "28674", "transport error code present");
    assert_string_contains(message, "tls_code=", "tls code label present");
    assert_string_contains(message, "-9984", "tls code present");
    assert_string_contains(message, "NOT_TRUSTED", "tls not trusted flag present");
    assert_string_contains(message, "BAD_MD", "tls bad md flag present");
    assert_string_contains(message, "FUTURE", "tls future flag present");

    assert_true(
        espclaw_agent_format_transport_error(28674, 0, 0, message, sizeof(message)) == 0,
        "transport error formatting without tls succeeds"
    );
    assert_string_not_contains(message, "tls_code", "tls_code omitted when absent");
}

static void test_sse_completed_extraction_in_place(void)
{
    char buffer[2048];

    snprintf(
        buffer,
        sizeof(buffer),
        "event: response.created\n"
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_ignore\"}}\n\n"
        "event: response.completed\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_live\",\"status\":\"completed\",\"output\":[{\"id\":\"msg_live\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"ESPCLAW_BENCH_HI\"}]}]}}\n\n"
        "data: [DONE]\n\n"
    );

    assert_true(
        espclaw_agent_extract_sse_completed_response_json(buffer, buffer, sizeof(buffer)) == 0,
        "in-place sse extraction succeeded"
    );
    assert_string_not_contains(buffer, "event: response.created", "sse extraction removed earlier events");
    assert_string_contains(buffer, "\"type\":\"response.completed\"", "sse extraction preserved completed event");
    assert_string_contains(buffer, "\"id\":\"resp_live\"", "sse extraction preserved nested response id");
    assert_string_contains(buffer, "\"text\":\"ESPCLAW_BENCH_HI\"", "sse extraction preserved assistant text");
}

static void test_http_chunked_body_extraction(void)
{
    char buffer[4096];
    char error_text[256];
    const char *chunk_one = "event: response.completed\n";
    const char *chunk_two =
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_chunked\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"ESPCLAW_BENCH_HI\"}]}]}}\n\n";
    size_t buffer_len;

    snprintf(
        buffer,
        sizeof(buffer),
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%zx\r\n%s\r\n"
        "%zx\r\n%s\r\n"
        "0\r\n"
        "\r\n",
        strlen(chunk_one),
        chunk_one,
        strlen(chunk_two),
        chunk_two
    );
    buffer_len = strlen(buffer);
    memset(error_text, 0, sizeof(error_text));

    assert_true(
        espclaw_agent_extract_http_body_in_place(buffer, &buffer_len, error_text, sizeof(error_text)) == 0,
        "chunked body extraction succeeded"
    );
    assert_string_contains(buffer, "event: response.completed", "chunked body retained sse event");
    assert_string_contains(buffer, "\"id\":\"resp_chunked\"", "chunked body retained response id");
    assert_string_contains(buffer, "\"text\":\"ESPCLAW_BENCH_HI\"", "chunked body retained assistant text");
    assert_string_not_contains(buffer, "Transfer-Encoding", "chunked body removed http headers");
}

static void test_incremental_sse_stream_reduction(void)
{
    char payload[4096];
    char reduced[2048];
    char error_text[256];

    snprintf(
        payload,
        sizeof(payload),
        "HTTP/1.0 200 OK\r\n"
        "Connection: close\r\n"
        "\r\n"
        "event: response.created\n"
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_live\"}}\n\n"
        "event: response.output_text.done\n"
        "data: {\"type\":\"response.output_text.done\",\"text\":\"ESPCLAW_\"}\n\n"
        "event: response.output_text.done\n"
        "data: {\"type\":\"response.output_text.done\",\"text\":\"BENCH_HI\"}\n\n"
        "event: response.completed\n"
        "data: {\"id\":\"resp_live\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"ESPCLAW_BENCH_HI\"}]}]}\n\n"
    );
    memset(error_text, 0, sizeof(error_text));

    assert_true(
        espclaw_agent_reduce_sse_stream_to_response_json(payload, reduced, sizeof(reduced), error_text, sizeof(error_text)) == 0,
        "incremental sse reduction succeeded"
    );
    assert_string_contains(reduced, "\"id\":\"resp_live\"", "incremental sse reduction retained response id");
    assert_string_contains(reduced, "\"text\":\"ESPCLAW_BENCH_HI\"", "incremental sse reduction retained assistant text");
}

static void test_incremental_sse_stream_fallback_text(void)
{
    char payload[4096];
    char reduced[2048];
    char error_text[256];

    snprintf(
        payload,
        sizeof(payload),
        "HTTP/1.0 200 OK\r\n"
        "Connection: close\r\n"
        "\r\n"
        "event: response.created\n"
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_fallback\"}}\n\n"
        "event: response.output_text.done\n"
        "data: {\"type\":\"response.output_text.done\",\"text\":\"ESPCLAW_BENCH_HI\"}\n\n"
    );
    memset(error_text, 0, sizeof(error_text));

    assert_true(
        espclaw_agent_reduce_sse_stream_to_response_json(payload, reduced, sizeof(reduced), error_text, sizeof(error_text)) == 0,
        "incremental sse fallback reduction succeeded"
    );
    assert_string_contains(reduced, "\"id\":\"resp_fallback\"", "fallback reduction retained response id");
    assert_string_contains(reduced, "\"text\":\"ESPCLAW_BENCH_HI\"", "fallback reduction synthesized assistant text");
}

static void test_incremental_sse_stream_handles_long_completed_line(void)
{
    char long_text[4600];
    char *payload = NULL;
    char *reduced = NULL;
    char error_text[256];
    size_t index;

    for (index = 0; index < sizeof(long_text) - 1; ++index) {
        long_text[index] = (char)('a' + (index % 26));
    }
    long_text[sizeof(long_text) - 1] = '\0';

    payload = (char *)calloc(1, 8192);
    reduced = (char *)calloc(1, 6144);
    assert_true(payload != NULL && reduced != NULL, "long sse buffers allocated");

    snprintf(
        payload,
        8192,
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "\r\n"
        "event: response.completed\n"
        "data: {\"id\":\"resp_long\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"%s\"}]}]}\n\n",
        long_text
    );
    memset(error_text, 0, sizeof(error_text));

    assert_true(
        espclaw_agent_reduce_sse_stream_to_response_json(payload, reduced, 6144, error_text, sizeof(error_text)) == 0,
        "incremental sse reduction handles long completed line"
    );
    assert_string_contains(reduced, "\"id\":\"resp_long\"", "long completed line retained response id");
    assert_string_contains(reduced, "\"type\":\"output_text\"", "long completed line retained assistant content");
    assert_string_contains(reduced, "\"text\":\"abc", "long completed line retained long text");

    free(reduced);
    free(payload);
}

static void test_incremental_sse_stream_uses_output_text_done_when_completed_lacks_text(void)
{
    char payload[4096];
    char reduced[2048];
    char error_text[256];

    snprintf(
        payload,
        sizeof(payload),
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "\r\n"
        "event: response.created\n"
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_vision\"}}\n\n"
        "event: response.output_text.done\n"
        "data: {\"type\":\"response.output_text.done\",\"text\":\"A workbench with electronics parts.\"}\n\n"
        "event: response.completed\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_vision\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"refusal\",\"refusal\":\"\"}]}]}}\n\n"
    );
    memset(error_text, 0, sizeof(error_text));

    assert_true(
        espclaw_agent_reduce_sse_stream_to_response_json(payload, reduced, sizeof(reduced), error_text, sizeof(error_text)) == 0,
        "incremental sse reduction uses streamed output_text when completed lacks inline text"
    );
    assert_string_contains(reduced, "\"id\":\"resp_vision\"", "vision fallback retained response id");
    assert_string_contains(reduced, "\"type\":\"output_text\"", "vision fallback synthesized output text");
    assert_string_contains(reduced, "A workbench with electronics parts.", "vision fallback retained streamed output text");
}

static void test_incremental_sse_stream_uses_output_text_delta_when_completed_lacks_text(void)
{
    char payload[4096];
    char reduced[2048];
    char error_text[256];

    snprintf(
        payload,
        sizeof(payload),
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "\r\n"
        "event: response.created\n"
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_delta\"}}\n\n"
        "event: response.output_text.delta\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"ESPCLAW_\"}\n\n"
        "event: response.output_text.delta\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"DELTA\"}\n\n"
        "event: response.completed\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_delta\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"refusal\",\"refusal\":\"\"}]}]}}\n\n"
    );
    memset(error_text, 0, sizeof(error_text));

    assert_true(
        espclaw_agent_reduce_sse_stream_to_response_json(payload, reduced, sizeof(reduced), error_text, sizeof(error_text)) == 0,
        "incremental sse reduction uses streamed output_text deltas when completed lacks inline text"
    );
    assert_string_contains(reduced, "\"id\":\"resp_delta\"", "delta fallback retained response id");
    assert_string_contains(reduced, "\"type\":\"output_text\"", "delta fallback synthesized output text");
    assert_string_contains(reduced, "ESPCLAW_DELTA", "delta fallback retained streamed output text");
}

static void test_terminal_response_storage_handles_overlap(void)
{
    char buffer[256];

    snprintf(
        buffer,
        sizeof(buffer),
        "{\"id\":\"resp_overlap\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"ESPCLAW_BENCH_HI\"}]}]}"
    );

    assert_true(
        espclaw_agent_store_terminal_response_json(buffer, strlen(buffer), buffer, sizeof(buffer)) == 0,
        "terminal response storage handles overlap"
    );
    assert_string_contains(buffer, "\"id\":\"resp_overlap\"", "overlap storage preserved response id");
    assert_string_contains(buffer, "\"text\":\"ESPCLAW_BENCH_HI\"", "overlap storage preserved output text");
}

static void test_esp32cam_storage_attempts(void)
{
    espclaw_esp32cam_sd_attempt_t attempt;

    assert_true(espclaw_storage_esp32cam_attempt_count() == 2U, "esp32cam exposes sd retry attempts");
    assert_true(espclaw_storage_get_esp32cam_attempt(0, &attempt), "esp32cam sdspi attempt available");
    assert_true(strcmp(attempt.label, "sdspi") == 0, "esp32cam first attempt is sdspi");
    assert_true(attempt.mode == ESPCLAW_ESP32CAM_SD_MODE_SDSPI, "esp32cam first attempt uses sdspi");
    assert_true(attempt.clk_gpio == 14, "esp32cam sdspi clock pin matches board");
    assert_true(attempt.cmd_mosi_gpio == 15, "esp32cam sdspi mosi pin matches board");
    assert_true(attempt.d0_miso_gpio == 2, "esp32cam sdspi miso pin matches board");
    assert_true(attempt.d1_gpio == -1, "esp32cam sdspi leaves led/data1 pin unused");
    assert_true(attempt.d2_gpio == -1, "esp32cam sdspi leaves data2 pin unused");
    assert_true(attempt.d3_cs_gpio == 13, "esp32cam sdspi cs pin matches board");

    assert_true(espclaw_storage_get_esp32cam_attempt(1, &attempt), "esp32cam sdmmc attempt available");
    assert_true(strcmp(attempt.label, "sdmmc-1bit") == 0, "esp32cam second attempt is sdmmc");
    assert_true(attempt.mode == ESPCLAW_ESP32CAM_SD_MODE_SDMMC, "esp32cam second attempt uses sdmmc");
    assert_true(attempt.width == 1U, "esp32cam sdmmc attempt uses 1-bit mode");
    assert_true(attempt.clk_gpio == 14, "esp32cam sdmmc clock pin matches board");
    assert_true(attempt.cmd_mosi_gpio == 15, "esp32cam sdmmc command pin matches board");
    assert_true(attempt.d0_miso_gpio == 2, "esp32cam sdmmc data0 pin matches board");
    assert_true(attempt.d1_gpio == -1, "esp32cam sdmmc leaves led/data1 pin unused");
    assert_true(attempt.d2_gpio == -1, "esp32cam sdmmc leaves data2 pin unused");
    assert_true(attempt.d3_cs_gpio == -1, "esp32cam sdmmc leaves data3 pin unused");

    assert_true(!espclaw_storage_get_esp32cam_attempt(2, &attempt), "esp32cam has no third storage attempt");
}

static void test_board_boot_defaults_force_flash_led_low(void)
{
    char temp_dir[64];
    espclaw_board_profile_t cam = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32CAM);
    int level = 1;

    make_temp_dir(temp_dir, sizeof(temp_dir));
    espclaw_hw_sim_reset();
    assert_true(espclaw_board_configure_current(temp_dir, &cam) == 0, "board descriptor configured for boot defaults");
    assert_true(espclaw_hw_gpio_write(4, 1) == 0, "flash led preset high");
    assert_true(espclaw_hw_apply_board_boot_defaults() == 0, "board boot defaults applied");
    assert_true(espclaw_hw_gpio_read(4, &level) == 0, "flash led level readable");
    assert_true(level == 0, "flash led forced low by board boot defaults");
}

static int sse_wrapped_http_adapter(
    const char *url,
    const espclaw_auth_profile_t *profile,
    const char *body,
    char *response,
    size_t response_size,
    void *user_data
)
{
    (void)url;
    (void)profile;
    (void)body;
    (void)user_data;

    snprintf(
        response,
        response_size,
        "event: response.completed\n"
        "data: {"
        "\"type\":\"response.completed\","
        "\"response\":{"
        "\"id\":\"resp_sse_wrapped\","
        "\"status\":\"completed\","
        "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"ESPCLAW_BENCH_HI\"}]}]"
        "}"
        "}\n\n"
    );
    return 0;
}

static int app_install_arguments_first_http_adapter(
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
    (void)body;

    if (state != NULL) {
        state->calls++;
    }

    if (state != NULL && state->calls > 1) {
        snprintf(
            response,
            response_size,
            "{"
            "\"id\":\"resp_install_args_first_final\","
            "\"status\":\"completed\","
            "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"Installed the arguments-first app.\"}]}]"
            "}"
        );
        return 0;
    }

    snprintf(
        response,
        response_size,
        "{"
        "\"id\":\"resp_install_args_first\","
        "\"status\":\"completed\","
        "\"output\":["
        "{\"type\":\"function_call\",\"call_id\":\"call_install_args_first\",\"arguments\":\"{\\\"app_id\\\":\\\"args_first_app\\\",\\\"title\\\":\\\"Args First\\\",\\\"source\\\":\\\"function handle(trigger, payload)\\\\n  return \\'args-first:\\' .. payload\\\\nend\\\\n\\\",\\\"permissions\\\":\\\"fs.read\\\",\\\"triggers\\\":\\\"manual\\\"}\",\"name\":\"app.install\",\"status\":\"completed\"}"
        "]"
        "}"
    );
    return 0;
}

static int sse_stream_http_adapter(
    const char *url,
    const espclaw_auth_profile_t *profile,
    const char *body,
    char *response,
    size_t response_size,
    void *user_data
)
{
    (void)url;
    (void)profile;
    (void)body;
    (void)user_data;

    snprintf(
        response,
        response_size,
        "event: response.created\n"
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_sse_stream\",\"status\":\"in_progress\"}}\n\n"
        "event: response.output_text.done\n"
        "data: {\"type\":\"response.output_text.done\",\"text\":\"ESPCLAW_BENCH_HI\"}\n\n"
    );
    return 0;
}

static int long_output_text_http_adapter(
    const char *url,
    const espclaw_auth_profile_t *profile,
    const char *body,
    char *response,
    size_t response_size,
    void *user_data
)
{
    char long_text[2048];
    size_t index = 0;

    (void)url;
    (void)profile;
    (void)body;
    (void)user_data;

    for (index = 0; index < sizeof(long_text) - 1; ++index) {
        long_text[index] = (char)('a' + (index % 26));
    }
    long_text[sizeof(long_text) - 1] = '\0';

    snprintf(
        response,
        response_size,
        "{"
        "\"id\":\"resp_long_text\","
        "\"status\":\"completed\","
        "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"%s\"}]}]"
        "}",
        long_text
    );
    return 0;
}

static int behavior_register_http_adapter(
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

    assert_string_contains(body, "Tool Inventory Snapshot", "behavior register prompt includes tool inventory snapshot");
    assert_string_contains(body, "behavior.register", "behavior register prompt includes behavior tool");

    if (state != NULL && state->calls > 1) {
        assert_string_contains(body, "\"type\":\"function_call_output\"", "behavior register follow-up includes tool output");
        snprintf(
            response,
            response_size,
            "{"
            "\"id\":\"resp_behavior_final\","
            "\"status\":\"completed\","
            "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"I installed and saved the autonomous behavior.\"}]}]"
            "}"
        );
        return 0;
    }

    snprintf(
        response,
        response_size,
        "{"
        "\"id\":\"resp_behavior_first\","
        "\"status\":\"completed\","
        "\"output\":["
        "{\"type\":\"function_call\",\"call_id\":\"call_behavior\",\"name\":\"behavior.register\",\"arguments\":\"{\\\"behavior_id\\\":\\\"wall_watch\\\",\\\"title\\\":\\\"Wall Watch\\\",\\\"schedule\\\":\\\"event\\\",\\\"trigger\\\":\\\"sensor\\\",\\\"autostart\\\":true,\\\"source\\\":\\\"function on_sensor(payload)\\\\n  return 'watch:' .. payload\\\\nend\\\\nfunction handle(trigger, payload)\\\\n  return on_sensor(payload)\\\\nend\\\\n\\\"}\",\"status\":\"completed\"}"
        "]"
        "}"
    );
    return 0;
}

static int camera_capture_http_adapter(
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

    assert_string_contains(body, "Tool Inventory Snapshot", "camera capture prompt includes tool inventory");
    assert_string_contains(body, "camera.capture", "camera capture prompt includes camera tool");
    assert_string_contains(
        body,
        "trusted local operator surface",
        "mutation-enabled runs advertise operator-approved mutation policy"
    );

    if (state != NULL && state->calls > 1) {
        assert_string_contains(body, "\"type\":\"function_call_output\"", "camera follow-up includes tool output");
        assert_string_contains(body, "\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_image\"", "camera follow-up wraps image in a user message");
        assert_string_contains(body, "\"type\":\"input_image\"", "camera follow-up includes input_image content");
        assert_string_contains(body, "\"image_url\":\"data:image/jpeg;base64,", "camera follow-up includes jpeg data url on input_image");
        snprintf(
            response,
            response_size,
            "{"
            "\"id\":\"resp_camera_final\","
            "\"status\":\"completed\","
            "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"I inspected the captured camera frame and attached it to this run.\"}]}]"
            "}"
        );
        return 0;
    }

    snprintf(
        response,
        response_size,
        "{"
        "\"id\":\"resp_camera_first\","
        "\"status\":\"completed\","
        "\"output\":["
        "{\"type\":\"function_call\",\"call_id\":\"call_camera\",\"name\":\"camera.capture\",\"arguments\":\"{\\\"filename\\\":\\\"vision_test.jpg\\\"}\",\"status\":\"completed\"}"
        "]"
        "}"
    );
    return 0;
}

static int empty_completion_http_adapter(
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

    if (state != NULL && state->calls > 1) {
        assert_string_contains(body, "\"type\":\"function_call_output\"", "empty completion follow-up includes tool output");
        snprintf(
            response,
            response_size,
            "{"
            "\"id\":\"resp_empty_final\","
            "\"status\":\"completed\","
            "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"refusal\",\"refusal\":\"\"}]}]"
            "}"
        );
        return 0;
    }

    snprintf(
        response,
        response_size,
        "{"
        "\"id\":\"resp_empty_first\","
        "\"status\":\"completed\","
        "\"output\":["
        "{\"type\":\"function_call\",\"call_id\":\"call_tool_list\",\"name\":\"tool.list\",\"arguments\":\"{}\",\"status\":\"completed\"}"
        "]"
        "}"
    );
    return 0;
}

static int malformed_role_history_http_adapter(
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

    assert_string_not_contains(body, "\"role\":\"\"", "malformed empty history role omitted from request");

    snprintf(
        response,
        response_size,
        "{"
        "\"id\":\"resp_malformed_role_ok\","
        "\"status\":\"completed\","
        "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"History normalization worked.\"}]}]"
        "}"
    );
    return 0;
}

static int explicit_tool_retry_http_adapter(
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

    if (state != NULL && state->calls == 1U) {
        assert_string_contains(body, "yes do", "explicit tool retry first turn keeps user approval");
        snprintf(
            response,
            response_size,
            "{"
            "\"id\":\"resp_explicit_retry_1\","
            "\"status\":\"completed\","
            "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"I still need task.list.\"}]}]"
            "}"
        );
        return 0;
    }

    if (state != NULL && state->calls == 2U) {
        assert_string_contains(body, "explicitly told you to call these tools", "explicit tool retry corrective prompt included");
        assert_string_contains(body, "task.list", "explicit tool retry names missing tool");
        snprintf(
            response,
            response_size,
            "{"
            "\"id\":\"resp_explicit_retry_2\","
            "\"status\":\"completed\","
            "\"output\":["
            "{\"type\":\"function_call\",\"call_id\":\"call_task_list_retry\",\"name\":\"task_x2E_list\",\"arguments\":\"{}\",\"status\":\"completed\"}"
            "]"
            "}"
        );
        return 0;
    }

    assert_string_contains(body, "\"type\":\"function_call_output\"", "explicit tool retry follow-up includes tool output");
    snprintf(
        response,
        response_size,
        "{"
        "\"id\":\"resp_explicit_retry_final\","
        "\"status\":\"completed\","
        "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"Called task.list and confirmed the result.\"}]}]"
        "}"
    );
    return 0;
}

static int explicit_tool_alias_retry_http_adapter(
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

    if (state != NULL && state->calls == 1U) {
        assert_string_contains(body, "yes try that", "alias retry first turn keeps user approval");
        snprintf(
            response,
            response_size,
            "{"
            "\"id\":\"resp_explicit_alias_retry_1\","
            "\"status\":\"completed\","
            "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"I can emit an event and then check task.list next.\"}]}]"
            "}"
        );
        return 0;
    }

    if (state != NULL && state->calls == 2U) {
        assert_string_contains(body, "explicitly told you to call these tools", "alias retry corrective prompt included");
        assert_string_contains(body, "event.emit", "alias retry includes event emit");
        assert_string_contains(body, "task.list", "alias retry includes task list");
        snprintf(
            response,
            response_size,
            "{"
            "\"id\":\"resp_explicit_alias_retry_2\","
            "\"status\":\"completed\","
            "\"output\":["
            "{\"type\":\"function_call\",\"call_id\":\"call_event_emit_retry\",\"name\":\"event_x2E_emit\",\"arguments\":\"{\\\"name\\\":\\\"test.ping\\\",\\\"payload\\\":\\\"hello 1\\\"}\",\"status\":\"completed\"},"
            "{\"type\":\"function_call\",\"call_id\":\"call_task_list_retry_alias\",\"name\":\"task_x2E_list\",\"arguments\":\"{}\",\"status\":\"completed\"}"
            "]"
            "}"
        );
        return 0;
    }

    assert_string_contains(body, "\"type\":\"function_call_output\"", "alias retry follow-up includes tool output");
    assert_string_contains(body, "call_event_emit_retry", "alias retry follow-up contains event emit output");
    assert_string_contains(body, "call_task_list_retry_alias", "alias retry follow-up contains task list output");
    snprintf(
        response,
        response_size,
        "{"
        "\"id\":\"resp_explicit_alias_retry_final\","
        "\"status\":\"completed\","
        "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"Called event.emit and task.list and confirmed the result.\"}]}]"
        "}"
    );
    return 0;
}

static void test_board_profiles(void)
{
    espclaw_board_profile_t s3 = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32S3);
    espclaw_board_profile_t cam = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32CAM);

    assert_true(strcmp(s3.id, "esp32s3") == 0, "esp32s3 profile id");
    assert_true(strcmp(s3.provisioning, "ble") == 0, "esp32s3 uses BLE provisioning");
    assert_true(s3.default_storage_backend == ESPCLAW_STORAGE_BACKEND_SD_CARD, "esp32s3 defaults to sd storage");
    assert_true(s3.cpu_cores == 2, "esp32s3 is dual core");
    assert_true(s3.supports_concurrent_capture, "esp32s3 supports concurrent capture");
    assert_true(strcmp(s3.runtime_budget.memory_class, "full") == 0, "esp32s3 uses full runtime budget");
    assert_true(strcmp(cam.id, "esp32cam") == 0, "esp32cam profile id");
    assert_true(strcmp(cam.provisioning, "softap") == 0, "esp32cam uses SoftAP provisioning");
    assert_true(cam.default_storage_backend == ESPCLAW_STORAGE_BACKEND_SD_CARD, "esp32cam defaults to sd storage");
    assert_true(!cam.supports_ble_provisioning, "esp32cam does not expose BLE provisioning");
    assert_true(strcmp(cam.runtime_budget.memory_class, "balanced") == 0, "esp32cam uses balanced runtime budget");
    assert_true(cam.runtime_budget.agent_response_buffer_max == 131072U, "esp32cam uses a larger PSRAM-backed response buffer");
}

static void test_lua_api_registry(void)
{
    char json[16384];
    char markdown[16384];
    char prompt[16384];

    assert_true(espclaw_lua_api_count() > 10, "lua api registry has entries");
    assert_true(espclaw_render_lua_api_json(json, sizeof(json)) > 0, "lua api json rendered");
    assert_string_contains(json, "\"name\":\"espclaw.pwm.setup\"", "lua api json includes pwm setup");
    assert_string_contains(json, "\"signature\":\"espclaw.i2c.read_reg(port, address, reg, length)\"", "lua api json includes i2c read");
    assert_true(espclaw_render_lua_api_markdown(markdown, sizeof(markdown)) > 0, "lua api markdown rendered");
    assert_string_contains(markdown, "# Lua API Reference", "lua api markdown header");
    assert_string_contains(markdown, "`espclaw.time.sleep_ms(ms)`", "lua api markdown includes time sleep");
    assert_true(espclaw_render_lua_api_prompt_snapshot(prompt, sizeof(prompt)) > 0, "lua api prompt snapshot rendered");
    assert_string_contains(prompt, "# Lua App Contract", "lua api prompt includes rules header");
    assert_string_contains(prompt, "Do not assume external Lua modules like cjson", "lua api prompt includes no-cjson rule");
}

static void test_board_descriptor_and_task_policy(void)
{
    char temp_dir[128];
    char board_json[4096];
    espclaw_board_profile_t s3 = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32S3);
    espclaw_board_profile_t cam = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32CAM);
    const espclaw_board_descriptor_t *current = NULL;
    espclaw_board_i2c_bus_t i2c_bus;
    espclaw_board_uart_t uart;
    espclaw_board_adc_channel_t adc;
    espclaw_task_policy_t policy;
    int pin = -1;
    char presets_json[1024];
    char board_config_json[2048];
    char minimal_json[256];
    espclaw_board_descriptor_t preset;

    make_temp_dir(temp_dir, sizeof(temp_dir));
    assert_true(espclaw_workspace_bootstrap(temp_dir) == 0, "workspace bootstrap for board descriptor");
    assert_true(
        espclaw_workspace_write_file(
            temp_dir,
            "config/board.json",
            "{\n"
            "  \"variant\": \"ai_thinker_esp32cam\",\n"
            "  \"pins\": {\"buzzer\": 15},\n"
            "  \"i2c\": {\"default\": {\"port\": 0, \"sda\": 6, \"scl\": 7, \"frequency_hz\": 100000}},\n"
            "  \"uart\": {\"console\": {\"port\": 0, \"tx\": 21, \"rx\": 20, \"baud_rate\": 57600}},\n"
            "  \"adc\": {\"battery\": {\"unit\": 1, \"channel\": 3}}\n"
            "}\n"
        ) == 0,
        "board descriptor override written"
    );

    assert_true(espclaw_board_configure_current(temp_dir, &cam) == 0, "board descriptor configured");
    current = espclaw_board_current();
    assert_true(current != NULL, "current board descriptor available");
    assert_true(strcmp(current->variant_id, "ai_thinker_esp32cam") == 0, "board variant loaded from workspace");
    assert_true(strcmp(current->source, "workspace") == 0, "board source indicates workspace");
    assert_true(espclaw_board_resolve_pin_alias("buzzer", &pin) == 0, "buzzer alias resolved");
    assert_true(pin == 15, "board pin override applied");
    assert_true(espclaw_board_find_i2c_bus("default", &i2c_bus) == 0, "board i2c bus resolved");
    assert_true(i2c_bus.frequency_hz == 100000, "board i2c override applied");
    assert_true(espclaw_board_find_uart("console", &uart) == 0, "board uart resolved");
    assert_true(uart.baud_rate == 57600, "board uart override applied");
    assert_true(espclaw_board_find_adc_channel("battery", &adc) == 0, "board adc channel resolved");
    assert_true(adc.unit == 1 && adc.channel == 3, "board adc override applied");

    espclaw_task_policy_select(&s3);
    policy = espclaw_task_policy_current();
    assert_true(policy.cpu_cores == 2, "dual-core policy tracks cpu count");
    assert_true(policy.telegram_core == 0, "dual-core telegram task pinned to core 0");
    assert_true(policy.control_loop_core == 1, "dual-core control loop pinned to core 1");
    espclaw_task_policy_select(&cam);
    policy = espclaw_task_policy_current();
    assert_true(policy.cpu_cores == 2, "esp32cam policy tracks cpu count");

    assert_true(espclaw_render_board_json(current, board_json, sizeof(board_json)) > 0, "board json rendered");
    assert_string_contains(board_json, "\"variant\":\"ai_thinker_esp32cam\"", "board json variant rendered");
    assert_string_contains(board_json, "\"control_loop_core\":1", "board json includes task policy");

    assert_true(espclaw_board_preset_count(&cam) >= 1, "esp32cam preset count available");
    assert_true(espclaw_board_preset_at(&cam, 0, &preset) == 0, "esp32cam preset lookup succeeded");
    assert_true(strcmp(preset.variant_id, "ai_thinker_esp32cam") == 0, "preset variant matches builtin");
    assert_true(espclaw_board_render_minimal_config_json(&preset, minimal_json, sizeof(minimal_json)) > 0, "minimal board json rendered");
    assert_string_contains(minimal_json, "\"variant\": \"ai_thinker_esp32cam\"", "minimal board config variant");
    assert_true(espclaw_render_board_presets_json(&cam, presets_json, sizeof(presets_json)) > 0, "board presets json rendered");
    assert_string_contains(presets_json, "\"variant\":\"ai_thinker_esp32cam\"", "board preset listed");
    assert_true(espclaw_render_board_config_json(temp_dir, &cam, current, board_config_json, sizeof(board_config_json)) > 0, "board config json rendered");
    assert_string_contains(board_config_json, "\"source\":\"workspace\"", "board config source rendered");
    assert_string_contains(board_config_json, "\\\"variant\\\": \\\"ai_thinker_esp32cam\\\"", "board config raw json rendered");
}

static void test_workspace_manifest(void)
{
    const espclaw_workspace_file_t *memory_file = espclaw_find_workspace_file("memory/MEMORY.md");
    const espclaw_workspace_file_t *board_file = espclaw_find_workspace_file("config/board.json");

    assert_true(espclaw_workspace_file_count() == 6, "workspace bootstrap file count");
    assert_true(memory_file != NULL, "memory file exists");
    assert_true(board_file != NULL, "board config file exists");
    assert_string_contains(memory_file->default_content, "Long-term", "memory template content");
    assert_string_contains(board_file->default_content, "\"variant\": \"auto\"", "board config template content");
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
    assert_true(espclaw_tool_count() >= 25, "tool catalog size");
    assert_true(espclaw_find_tool("tool.list") != NULL, "tool list tool exists");
    assert_true(espclaw_find_tool("hardware.list") != NULL, "hardware list tool exists");
    assert_true(espclaw_find_tool("lua_api.list") != NULL, "lua api list tool exists");
    assert_true(espclaw_find_tool("behavior.register") != NULL, "behavior register tool exists");
    assert_true(espclaw_find_tool("task.start") != NULL, "task start tool exists");
    assert_true(espclaw_find_tool("event.emit") != NULL, "event emit tool exists");
    assert_true(espclaw_tool_requires_confirmation("fs.write"), "fs.write requires confirmation");
    assert_true(!espclaw_tool_requires_confirmation("wifi.scan"), "wifi.scan is read-only");
    assert_true(espclaw_find_tool("camera.capture") != NULL, "camera capture tool exists");
}

static void test_default_config_render(void)
{
    char buffer[4096];
    size_t written;
    espclaw_board_profile_t s3 = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32S3);
    espclaw_board_profile_t cam = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32CAM);

    written = espclaw_render_default_config(&s3, buffer, sizeof(buffer));

    assert_true(written > 0, "config render produced data");
    assert_string_contains(buffer, "\"board_profile\": \"esp32s3\"", "board profile rendered");
    assert_string_contains(buffer, "\"backend\": \"sdcard\"", "storage backend rendered");
    assert_string_contains(buffer, "\"providers\": [", "providers section rendered");
    assert_string_contains(buffer, "\"telegram\": {", "telegram section rendered");
    assert_string_contains(buffer, "\"admin_auth_required\": true", "security section rendered");

    written = espclaw_render_default_config(&cam, buffer, sizeof(buffer));
    assert_true(written > 0, "esp32cam config render produced data");
    assert_string_contains(buffer, "\"board_profile\": \"esp32cam\"", "esp32cam board profile rendered");
    assert_string_contains(buffer, "\"backend\": \"sdcard\"", "esp32cam storage backend rendered");
    assert_string_contains(buffer, "\"enabled\": true", "esp32cam camera enabled in config");
}

static void test_storage_backend_description(void)
{
    assert_true(strcmp(espclaw_storage_describe_workspace_root("/workspace"), "littlefs") == 0, "littlefs path detected");
    assert_true(strcmp(espclaw_storage_describe_workspace_root("/sdcard/workspace"), "sdcard") == 0, "sdcard path detected");
    assert_true(strcmp(espclaw_storage_describe_workspace_root("/tmp/espclaw"), "host") == 0, "host path detected");
}

static void test_system_monitor_snapshot_and_json(void)
{
    espclaw_board_profile_t cam = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32CAM);
    espclaw_system_monitor_snapshot_t snapshot;
    char json[1024];

    assert_true(espclaw_system_monitor_init(&cam) == 0, "system monitor init");
    assert_true(
        espclaw_system_monitor_snapshot(&cam, 1472U * 1024U, 128U * 1024U, &snapshot) == 0,
        "system monitor snapshot"
    );
    assert_true(snapshot.available, "system monitor available");
    assert_true(snapshot.cpu_cores == 2U, "system monitor tracks cpu core count");
    assert_true(snapshot.dual_core, "esp32cam monitor reports dual core");
    assert_true(snapshot.workspace_total_bytes == 1472U * 1024U, "monitor workspace total");
    assert_true(snapshot.workspace_used_bytes == 128U * 1024U, "monitor workspace used");
    assert_true(strcmp(snapshot.memory_class, "balanced") == 0, "monitor reports memory class");
    assert_true(snapshot.agent_history_slots == cam.runtime_budget.agent_history_max, "monitor exposes agent history slots");
    assert_true(snapshot.agent_request_buffer_bytes == cam.runtime_budget.agent_request_buffer_max, "monitor exposes request buffer");
    assert_true(
        espclaw_render_system_monitor_json(&snapshot, json, sizeof(json)) > 0,
        "system monitor json rendered"
    );
    assert_string_contains(json, "\"cpu_cores\":2", "system monitor json core count");
    assert_string_contains(json, "\"memory_class\":\"balanced\"", "system monitor json memory class");
    assert_string_contains(json, "\"workspace_used_bytes\":131072", "system monitor json workspace used");
    assert_string_contains(json, "\"agent_history_slots\":12", "system monitor json history slots");
}

static void test_camera_status_and_json(void)
{
    char temp_dir[128];
    char json[1024];
    espclaw_board_profile_t cam = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32CAM);
    espclaw_hw_camera_capture_t capture;
    espclaw_hw_camera_status_t status;

    make_temp_dir(temp_dir, sizeof(temp_dir));
    assert_true(espclaw_workspace_bootstrap(temp_dir) == 0, "workspace bootstrap for camera status");
    assert_true(espclaw_board_configure_current(temp_dir, &cam) == 0, "camera board configured");
    assert_true(
        espclaw_hw_camera_capture(temp_dir, "admin_capture.jpg", &capture) == 0,
        "host camera capture succeeded"
    );
    assert_true(capture.ok, "host camera capture marked ok");
    assert_true(
        espclaw_hw_camera_capture(temp_dir, "media/prefixed_capture.jpg", &capture) == 0,
        "host camera capture tolerates media-prefixed filename"
    );
    assert_true(strcmp(capture.relative_path, "media/prefixed_capture.jpg") == 0, "camera capture normalizes media prefix");
    assert_true(espclaw_hw_camera_status(&status) == 0, "camera status snapshot succeeded");
    assert_true(status.supported, "camera status reports supported");
    assert_true(status.last_capture_ok, "camera status reports last capture success");
    assert_true(strcmp(status.last_relative_path, "media/prefixed_capture.jpg") == 0, "camera status tracks last capture path");
    assert_true(
        espclaw_render_camera_status_json(&status, json, sizeof(json)) > 0,
        "camera status json rendered"
    );
    assert_string_contains(json, "\"supported\":true", "camera status json support flag");
    assert_string_contains(json, "\"last_capture_ok\":true", "camera status json success flag");
    assert_string_contains(json, "\"last_relative_path\":\"media/prefixed_capture.jpg\"", "camera status json capture path");
}

static void test_runtime_wifi_boot_deferral_policy(void)
{
    espclaw_board_profile_t s3 = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32S3);
    espclaw_board_profile_t cam = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32CAM);

    assert_true(!espclaw_runtime_should_defer_wifi_boot(&s3, false), "esp32s3 does not defer wifi boot");
    assert_true(!espclaw_runtime_should_defer_wifi_boot(&cam, true), "esp32cam keeps wifi boot when storage is ready");
    assert_true(espclaw_runtime_should_defer_wifi_boot(&cam, false), "esp32cam defers wifi boot when storage is unavailable");
    assert_true(
        !espclaw_runtime_should_force_softap_only_boot(&s3, false, false),
        "esp32s3 does not force softap-only boot"
    );
    assert_true(
        !espclaw_runtime_should_force_softap_only_boot(&cam, true, false),
        "esp32cam does not force softap-only boot when storage is ready"
    );
    assert_true(
        espclaw_runtime_should_force_softap_only_boot(&cam, false, false),
        "esp32cam forces softap-only boot when storage is unavailable and no Wi-Fi credentials exist"
    );
    assert_true(
        !espclaw_runtime_should_force_softap_only_boot(&cam, false, true),
        "esp32cam does not force softap-only boot when saved Wi-Fi credentials exist"
    );
}

static void test_storage_esp32cam_sdmmc_wiring_policy(void)
{
    espclaw_board_profile_t s3 = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32S3);
    espclaw_board_profile_t cam = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32CAM);

    assert_true(!espclaw_storage_use_esp32cam_sdmmc_wiring(&s3), "esp32s3 does not use esp32cam sdmmc wiring");
    assert_true(espclaw_storage_use_esp32cam_sdmmc_wiring(&cam), "esp32cam uses explicit slot-1 sdmmc wiring");
}

static void test_ota_manager_host_state_and_partition_layout(void)
{
    char csv_path[512];
    char csv[1024];
    char ota_json[512];
    char message[128];
    espclaw_ota_snapshot_t snapshot;

    espclaw_ota_manager_init();
    espclaw_ota_manager_snapshot(&snapshot);
    assert_true(snapshot.supported, "host ota manager reports support");
    assert_true(strcmp(snapshot.running_partition_label, "ota_0") == 0, "host ota running partition");
    assert_true(strcmp(snapshot.target_partition_label, "ota_1") == 0, "host ota target partition");

    assert_true(espclaw_ota_manager_begin(4096, message, sizeof(message)) == ESP_OK, "ota begin succeeds");
    assert_true(espclaw_ota_manager_write("abcd", 4, message, sizeof(message)) == ESP_OK, "ota write succeeds");
    assert_true(espclaw_ota_manager_finish(false, message, sizeof(message)) == ESP_OK, "ota finish succeeds");

    espclaw_ota_manager_snapshot(&snapshot);
    assert_true(snapshot.state.status == ESPCLAW_OTA_STATUS_PENDING_REBOOT, "ota enters pending reboot");
    assert_true(snapshot.written_bytes == 4, "ota written byte count tracked");
    assert_true(espclaw_render_ota_status_json(&snapshot, ota_json, sizeof(ota_json)) > 0, "ota status json rendered");
    assert_string_contains(ota_json, "\"status\":\"pending_reboot\"", "ota status json includes pending reboot");
    assert_string_contains(ota_json, "\"target_partition\":\"ota_1\"", "ota status json includes target partition");

    snprintf(csv_path, sizeof(csv_path), "%s/firmware/partitions_espclaw.csv", ESPCLAW_SOURCE_DIR);
    read_text_file(csv_path, csv, sizeof(csv));
    assert_string_contains(csv, "otadata", "partition table includes otadata");
    assert_string_contains(csv, "ota_0", "partition table includes ota_0");
    assert_string_contains(csv, "ota_1", "partition table includes ota_1");
    assert_string_contains(csv, "0x1E0000", "partition table uses enlarged symmetric ota slots");
    assert_string_contains(csv, "0x030000", "partition table reserves the reduced internal workspace");
}

static void test_esp32_sdkconfig_defaults_enable_psram_tls(void)
{
    char path[512];
    char contents[2048];

    snprintf(path, sizeof(path), "%s/firmware/sdkconfig.defaults.esp32", ESPCLAW_SOURCE_DIR);
    read_text_file(path, contents, sizeof(contents));
    assert_string_contains(contents, "CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y", "esp32 defaults enable PSRAM-backed mbedtls alloc");
    assert_string_contains(contents, "CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA=y", "esp32 defaults free mbedtls config data after setup");
}

static void test_camera_uses_reserved_ledc_channel(void)
{
    char path[512];
    char contents[65536];

    snprintf(path, sizeof(path), "%s/firmware/components/espclaw_core/src/hardware.c", ESPCLAW_SOURCE_DIR);
    read_text_file(path, contents, sizeof(contents));
    assert_string_contains(contents, "config.ledc_channel = LEDC_CHANNEL_1;", "camera xclk uses reserved ledc channel 1");
    assert_string_contains(contents, "config.ledc_timer = LEDC_TIMER_1;", "camera xclk uses reserved ledc timer 1");
}

static void test_task_runtime_uses_portmux_locking(void)
{
    char path[512];
    char contents[32768];

    snprintf(path, sizeof(path), "%s/firmware/components/espclaw_core/src/task_runtime.c", ESPCLAW_SOURCE_DIR);
    read_text_file(path, contents, sizeof(contents));
    assert_string_contains(contents, "portMUX_TYPE lock;", "task runtime uses a portMUX lock on device");
    assert_string_contains(contents, "portMUX_INITIALIZE(&slot->lock);", "task runtime initializes the portMUX");
    assert_string_contains(contents, "taskENTER_CRITICAL(&slot->lock);", "task runtime enters critical sections");
    assert_string_contains(contents, "taskEXIT_CRITICAL(&slot->lock);", "task runtime exits critical sections");
    assert_true(strstr(contents, "xSemaphoreCreateMutex") == NULL, "task runtime no longer uses queue-backed mutexes");
    assert_string_contains(contents, "#define ESPCLAW_TASK_RUNTIME_STACK_WORDS 16384", "task runtime uses a larger worker stack");
    assert_string_contains(contents, "ESPCLAW_TASK_RUNTIME_STACK_WORDS", "task runtime worker creation uses the configured stack macro");
}

static void test_runtime_skips_boot_automation_after_crash_reset(void)
{
    char path[512];
    char contents[65536];

    snprintf(path, sizeof(path), "%s/firmware/components/espclaw_core/src/runtime.c", ESPCLAW_SOURCE_DIR);
    read_text_file(path, contents, sizeof(contents));
    assert_string_contains(contents, "should_skip_boot_automation_for_reset_reason", "runtime defines crash-loop boot automation guard");
    assert_string_contains(contents, "ESP_RST_PANIC", "runtime treats panic resets as crash-loop candidates");
    assert_string_contains(contents, "ESP_RST_TASK_WDT", "runtime treats task watchdog resets as crash-loop candidates");
    assert_string_contains(contents, "ESP_RST_BROWNOUT", "runtime treats brownout resets as crash-loop candidates");
    assert_string_contains(contents, "Skipping boot apps after reset reason", "runtime logs when boot apps are skipped after a crash reset");
    assert_string_contains(contents, "Skipping autostart behaviors after reset reason", "runtime logs when autostart is skipped after a crash reset");
    assert_string_contains(contents, "Scheduled delayed behavior autostart", "runtime schedules delayed autostart on clean boots");
    assert_string_contains(contents, "delayed_behavior_autostart_task", "runtime defines delayed behavior autostart task");
    assert_string_contains(contents, "#define ESPCLAW_BEHAVIOR_AUTOSTART_DELAY_MS 8000", "runtime delays autostart behaviors after boot");
    assert_string_contains(contents, "Running delayed behavior autostart", "runtime logs when delayed autostart actually begins");
    assert_string_contains(contents, "reply = calloc(1, ESPCLAW_AGENT_TEXT_MAX + 64U);", "uart console allocates reply buffer off the task stack");
    assert_string_contains(contents, "result = calloc(1, sizeof(*result));", "uart console allocates the agent result off the task stack");
    assert_true(strstr(contents, "espclaw_agent_run_result_t result;") == NULL, "uart console no longer keeps the full agent result on the task stack");
}

static void test_behavior_runtime_caches_specs_for_embedded_autostart(void)
{
    char path[512];
    char contents[65536];

    snprintf(path, sizeof(path), "%s/firmware/components/espclaw_core/src/behavior_runtime.c", ESPCLAW_SOURCE_DIR);
    read_text_file(path, contents, sizeof(contents));
    assert_string_contains(contents, "ESPCLAW_BEHAVIOR_SPEC_KEY_PREFIX", "behavior runtime defines a dedicated spec key prefix");
    assert_string_contains(contents, "behavior_spec_store_json", "behavior runtime persists full behavior specs");
    assert_string_contains(contents, "behavior_spec_load_json", "behavior runtime loads behavior specs from the persisted store");
    assert_string_contains(contents, "behavior_spec_nvs_key", "behavior runtime derives compact NVS keys for behavior specs");
    assert_string_contains(contents, "behavior_spec_remove_json", "behavior runtime removes persisted behavior specs on delete");
}

static void test_app_runtime_caches_manifests_for_embedded_control_paths(void)
{
    char path[512];
    char contents[65536];

    snprintf(path, sizeof(path), "%s/firmware/components/espclaw_core/src/app_runtime.c", ESPCLAW_SOURCE_DIR);
    read_text_file(path, contents, sizeof(contents));
    assert_string_contains(contents, "ESPCLAW_APP_CACHE_NAMESPACE", "app runtime defines an embedded manifest cache namespace");
    assert_string_contains(contents, "app_manifest_cache_store_json", "app runtime persists manifest json for embedded use");
    assert_string_contains(contents, "app_manifest_cache_load_json", "app runtime loads manifests from the embedded cache first");
    assert_string_contains(contents, "(void)app_manifest_cache_store_json(app_id, manifest_json);", "app scaffold stores manifests in the embedded cache");
}

static void test_behavior_register_avoids_sd_manifest_validation(void)
{
    char path[512];
    char contents[65536];
    const char *register_fn = NULL;

    snprintf(path, sizeof(path), "%s/firmware/components/espclaw_core/src/behavior_runtime.c", ESPCLAW_SOURCE_DIR);
    read_text_file(path, contents, sizeof(contents));
    register_fn = strstr(contents, "int espclaw_behavior_register(");
    assert_true(register_fn != NULL, "behavior register implementation present");
    assert_true(
        register_fn != NULL && strstr(register_fn, "espclaw_app_load_manifest(") == NULL,
        "behavior register no longer opens app manifests from the workspace"
    );
}

static void test_session_append_avoids_workspace_bootstrap_on_embedded_console_paths(void)
{
    char path[512];
    char contents[16384];
    const char *append_fn = NULL;

    snprintf(path, sizeof(path), "%s/firmware/components/espclaw_core/src/session_store.c", ESPCLAW_SOURCE_DIR);
    read_text_file(path, contents, sizeof(contents));
    append_fn = strstr(contents, "int espclaw_session_append_message(");
    assert_true(append_fn != NULL, "session append implementation present");
    assert_true(
        append_fn != NULL && strstr(append_fn, "espclaw_workspace_bootstrap(") == NULL,
        "session append no longer re-runs workspace bootstrap on every message"
    );
}

static void test_console_chat_skips_embedded_transcript_writes(void)
{
    char path[512];
    char contents[32768];
    const char *append_fn = NULL;

    snprintf(path, sizeof(path), "%s/firmware/components/espclaw_core/src/console_chat.c", ESPCLAW_SOURCE_DIR);
    read_text_file(path, contents, sizeof(contents));
    append_fn = strstr(contents, "static void append_console_exchange(");
    assert_true(append_fn != NULL, "console exchange helper present");
    assert_string_contains(append_fn, "#ifdef ESP_PLATFORM", "console transcript helper has embedded guard");
    assert_string_contains(append_fn, "return;", "console transcript helper can no-op on embedded targets");
}

static void test_telegram_agent_turns_use_stateless_loop(void)
{
    char path[512];
    char contents[131072];

    snprintf(path, sizeof(path), "%s/firmware/components/espclaw_core/src/runtime.c", ESPCLAW_SOURCE_DIR);
    read_text_file(path, contents, sizeof(contents));
    assert_string_contains(contents, "espclaw_agent_loop_run_stateless(", "telegram polling uses the stateless agent loop");

    snprintf(path, sizeof(path), "%s/firmware/components/espclaw_core/src/agent_loop.c", ESPCLAW_SOURCE_DIR);
    read_text_file(path, contents, sizeof(contents));
    assert_string_contains(contents, "int espclaw_agent_loop_run_stateless(", "agent loop exposes a stateless entrypoint");
    assert_string_contains(contents, "persist_transcript,\n        false,", "stateless agent loop disables transcript persistence");
}

static void test_runtime_uses_larger_uart_console_stack(void)
{
    char path[512];
    char contents[65536];

    snprintf(path, sizeof(path), "%s/firmware/components/espclaw_core/src/runtime.c", ESPCLAW_SOURCE_DIR);
    read_text_file(path, contents, sizeof(contents));
    assert_string_contains(contents, "ESPCLAW_UART_CONSOLE_STACK_BYTES 32768", "runtime defines a larger UART console stack");
    assert_string_contains(contents, "\"espclaw_uart\",\n            ESPCLAW_UART_CONSOLE_STACK_BYTES,", "uart console task uses the dedicated larger stack");
}

static void test_embedded_console_and_agent_paths_heap_allocate_auth_profiles(void)
{
    char path[512];
    char contents[131072];

    snprintf(path, sizeof(path), "%s/firmware/components/espclaw_core/src/console_chat.c", ESPCLAW_SOURCE_DIR);
    read_text_file(path, contents, sizeof(contents));
    assert_string_contains(contents, "calloc(1, sizeof(*profile))", "console status path heap allocates auth profile on embedded");

    snprintf(path, sizeof(path), "%s/firmware/components/espclaw_core/src/agent_loop.c", ESPCLAW_SOURCE_DIR);
    read_text_file(path, contents, sizeof(contents));
    assert_string_contains(contents, "profile = (espclaw_auth_profile_t *)calloc(1, sizeof(*profile));", "agent loop heap allocates auth profile on embedded");
    assert_string_contains(contents, "free_embedded_auth_profile(profile);", "agent loop frees embedded auth profiles through helper");
}

static void test_uart_console_normalizes_newlines_for_serial_terminals(void)
{
    char path[512];
    char contents[65536];

    snprintf(path, sizeof(path), "%s/firmware/components/espclaw_core/src/runtime.c", ESPCLAW_SOURCE_DIR);
    read_text_file(path, contents, sizeof(contents));
    assert_string_contains(contents, "if (*cursor == '\\n' && (cursor == text || cursor[-1] != '\\r'))", "uart console writer detects bare newlines");
    assert_string_contains(contents, "espclaw_hw_uart_write(0, (const uint8_t *)\"\\r\\n\", 2, &written);", "uart console writer emits CRLF for bare newlines");
}

static void test_provisioning_descriptor(void)
{
    espclaw_provisioning_descriptor_t descriptor;
    char json[1024];

    assert_true(
        espclaw_provisioning_build_descriptor(
            true,
            "ble",
            "ESPClaw-123456",
            "",
            "espclaw-pass",
            "",
            &descriptor
        ) == 0,
        "ble provisioning descriptor built"
    );
    assert_true(descriptor.active, "ble provisioning active");
    assert_true(strcmp(descriptor.transport, "ble") == 0, "ble transport recorded");
    assert_string_contains(descriptor.qr_payload, "\"transport\":\"ble\"", "ble qr payload transport");
    assert_string_contains(descriptor.qr_payload, "\"name\":\"ESPClaw-123456\"", "ble qr payload service name");
    assert_string_contains(descriptor.qr_url, "https://espressif.github.io/esp-jumpstart/qrcode.html?data=", "ble qr helper url");
    assert_string_contains(descriptor.qr_url, "%7B%22ver%22%3A%22v1%22", "ble qr helper url encoded payload");

    assert_true(
        espclaw_provisioning_render_json(&descriptor, json, sizeof(json)) > 0,
        "provisioning json rendered"
    );
    assert_string_contains(json, "\"transport\":\"ble\"", "provisioning json transport");
    assert_string_contains(json, "\"pop\":\"espclaw-pass\"", "provisioning json pop");

    assert_true(
        espclaw_provisioning_build_descriptor(
            true,
            "softap",
            "ESPClaw-SoftAP",
            "",
            "",
            "http://192.168.4.1/",
            &descriptor
        ) == 0,
        "softap provisioning descriptor built"
    );
    assert_true(strcmp(descriptor.transport, "softap") == 0, "softap transport recorded");
    assert_true(descriptor.qr_payload[0] == '\0', "softap qr payload omitted");
    assert_true(strcmp(descriptor.admin_url, "http://192.168.4.1/") == 0, "softap admin url recorded");
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
    assert_string_contains(html, "<h2>Operator Chat</h2>", "chat-first heading present");
    assert_string_contains(html, "Quick Setup", "quick setup card present");
    assert_string_contains(html, "data-screen-target='chat'", "chat tab present");
    assert_string_contains(html, "data-screen-target='setup'", "setup tab present");
    assert_string_contains(html, "data-load-section='device'", "device module present");
    assert_string_contains(html, "data-load-section='apps'", "apps module present");
    assert_string_contains(html, "Custom board descriptor", "board json editor moved behind advanced details");
    assert_string_contains(html, "Send To Model", "chat send action present");
    assert_string_contains(html, "YOLO mode", "chat yolo toggle present");
    assert_string_contains(html, "Create", "app create action present");
    assert_string_contains(html, "/api/apps/scaffold", "app api wired into ui");
    assert_string_contains(html, "body: $('app-source').value", "save source posts editor contents");
    assert_string_contains(html, "body: $('app-payload').value", "run app posts payload input");
    assert_string_contains(html, "/api/auth/codex", "auth api wired into ui");
    assert_string_contains(html, "/api/auth/import-json", "auth json import api wired into ui");
    assert_string_contains(html, "Choose auth.json", "auth file picker action present");
    assert_string_contains(html, "Stored in secure device auth storage", "auth persistence note present");
    assert_string_contains(html, "/api/board/presets", "board presets api wired into ui");
    assert_string_contains(html, "/api/board/config", "board config api wired into ui");
    assert_string_contains(html, "/api/network/scan", "network scan api wired into ui");
    assert_string_contains(html, "/api/network/join", "network join api wired into ui");
    assert_string_contains(html, "/api/network/provisioning", "network provisioning api wired into ui");
    assert_string_contains(html, "provisioning-info", "provisioning panel present");
    assert_string_contains(html, "/api/monitor", "monitor api wired into ui");
    assert_string_contains(html, "/api/camera", "camera diagnostics api wired into ui");
    assert_string_contains(html, "/api/camera/capture", "camera capture test api wired into ui");
    assert_string_contains(html, "Camera Diagnostics", "camera diagnostics card present");
    assert_string_contains(html, "Test Capture", "camera test capture action present");
    assert_string_contains(html, "/api/tools", "tools api wired into ui");
    assert_string_contains(html, "/api/chat/run", "chat api wired into ui");
    assert_string_contains(html, "&yolo=1", "chat yolo query wiring present");
    assert_string_contains(html, "/api/behaviors", "behaviors api wired into ui");
    assert_string_contains(html, "/api/behaviors/register", "behavior register api wired into ui");
    assert_string_contains(html, "/api/tasks", "tasks api wired into ui");
    assert_string_contains(html, "/api/tasks/start", "task start api wired into ui");
    assert_string_contains(html, "/api/events/emit", "event emit api wired into ui");
    assert_string_contains(html, "Save Behavior", "behavior action label present");
    assert_string_contains(html, "Start Task", "task action label present");
    assert_string_contains(html, "Event-driven", "task schedule selector present");
    assert_string_contains(html, "setScreen('chat');", "chat screen opens by default");
    assert_string_contains(html, "details[data-load-section]", "details-driven lazy loading present");
    assert_true(strstr(html, "Loading board...") == NULL, "old raw loading placeholder removed");
    assert_true(strstr(html, "Loading provisioning...") == NULL, "old provisioning placeholder removed");
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
    char board_presets_json[1024];
    char board_config_json[2048];
    char hardware_json[3072];
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

    assert_true(
        espclaw_render_board_presets_json(&profile, board_presets_json, sizeof(board_presets_json)) > 0,
        "board presets json rendered for admin api"
    );
    assert_string_contains(board_presets_json, "\"variant\":\"generic_esp32s3\"", "s3 preset rendered");
    assert_true(
        espclaw_render_board_config_json(temp_dir, &profile, espclaw_board_current(), board_config_json, sizeof(board_config_json)) > 0,
        "board config json rendered for admin api"
    );
    assert_string_contains(board_config_json, "\"raw_json\":", "board config raw json included");
    assert_true(
        espclaw_render_hardware_json(espclaw_board_current(), hardware_json, sizeof(hardware_json)) > 0,
        "hardware json rendered"
    );
    assert_string_contains(hardware_json, "\"capabilities\":[", "hardware capabilities rendered");
    assert_string_contains(hardware_json, "\"pins\":[", "hardware pins rendered");

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
    char large_token[1801];
    char large_json[4096];
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
    profile.expires_at = 21474836480LL;

    assert_true(espclaw_auth_profile_is_ready(&profile), "codex auth profile is ready");
    assert_true(espclaw_auth_store_save(&profile) == 0, "auth profile saved");

    memset(&loaded, 0, sizeof(loaded));
    assert_true(espclaw_auth_store_load(&loaded) == 0, "auth profile loaded");
    assert_true(strcmp(loaded.access_token, "token_demo") == 0, "access token loaded");
    assert_true(strcmp(loaded.account_id, "acc_demo") == 0, "account id loaded");
    assert_true(strcmp(loaded.source, "unit_test") == 0, "source loaded");
    assert_true(loaded.expires_at == 21474836480LL, "64-bit expiry loaded without truncation");

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

    memset(&loaded, 0, sizeof(loaded));
    assert_true(
        espclaw_auth_store_import_json(
            "{"
            "\"access_token\":\"imported_access\","
            "\"refresh_token\":\"imported_refresh\","
            "\"account_id\":\"acc_imported\""
            "}",
            &loaded,
            message,
            sizeof(message)
        ) == 0,
        "raw auth json imported"
    );
    assert_true(strcmp(loaded.provider_id, "openai_codex") == 0, "raw auth import defaults to codex provider");
    assert_true(strcmp(loaded.model, "gpt-5.3-codex") == 0, "raw auth import keeps codex default model");
    assert_true(strcmp(loaded.account_id, "acc_imported") == 0, "raw auth import account id");
    assert_true(strcmp(loaded.source, "imported_json") == 0, "raw auth import source");
    assert_string_contains(message, "saved them to the device auth store", "raw auth import message");

    memset(&loaded, 0, sizeof(loaded));
    assert_true(espclaw_auth_store_load(&loaded) == 0, "raw auth import persisted");
    assert_true(strcmp(loaded.access_token, "imported_access") == 0, "raw auth import persisted token");

    memset(large_token, 'a', sizeof(large_token) - 1);
    large_token[sizeof(large_token) - 1] = '\0';
    snprintf(
        large_json,
        sizeof(large_json),
        "{"
        "\"access_token\":\"%s\","
        "\"refresh_token\":\"refresh_large\","
        "\"account_id\":\"acc_large\""
        "}",
        large_token
    );
    memset(&loaded, 0, sizeof(loaded));
    assert_true(
        espclaw_auth_store_import_json(large_json, &loaded, message, sizeof(message)) == 0,
        "large auth json imported"
    );
    assert_true(strcmp(loaded.access_token, large_token) == 0, "large auth import preserves token");
    assert_true(strcmp(loaded.account_id, "acc_large") == 0, "large auth import preserves account");

    memset(&loaded, 0, sizeof(loaded));
    assert_true(
        espclaw_auth_store_import_json(
            "{"
            "\"OPENAI_API_KEY\":null,"
            "\"tokens\":{"
            "\"id_token\":\"ignored\","
            "\"access_token\":\"nested_access\","
            "\"refresh_token\":\"nested_refresh\","
            "\"account_id\":\"nested_account\""
            "},"
            "\"last_refresh\":\"2026-03-13T00:04:16.658878Z\""
            "}",
            &loaded,
            message,
            sizeof(message)
        ) == 0,
        "nested codex auth json imported"
    );
    assert_true(strcmp(loaded.provider_id, "openai_codex") == 0, "nested auth import keeps codex provider default");
    assert_true(strcmp(loaded.model, "gpt-5.3-codex") == 0, "nested auth import keeps codex model default");
    assert_true(strcmp(loaded.base_url, "https://chatgpt.com/backend-api/codex") == 0, "nested auth import keeps codex base url default");
    assert_true(strcmp(loaded.access_token, "nested_access") == 0, "nested auth import captures access token");
    assert_true(strcmp(loaded.refresh_token, "nested_refresh") == 0, "nested auth import captures refresh token");
    assert_true(strcmp(loaded.account_id, "nested_account") == 0, "nested auth import captures account id");

    assert_true(
        espclaw_auth_store_import_json("{\"access_token\":\"missing_account\"}", &loaded, message, sizeof(message)) != 0,
        "invalid codex auth import rejected"
    );
    assert_string_contains(message, "missing required fields", "invalid auth import message");

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
    const char *large_id_update_json =
        "{\"ok\":true,\"result\":[{\"update_id\":9002,\"message\":{\"message_id\":13,"
        "\"from\":{\"id\":6568730383,\"is_bot\":false},\"chat\":{\"id\":6568730383,\"type\":\"private\"},"
        "\"date\":1710000001,\"text\":\"hi\"}}]}";
    char payload[1024];
    espclaw_telegram_update_t update;

    assert_true(espclaw_telegram_extract_update(update_json, &update), "telegram update extracted");
    assert_true(update.update_id == 9001, "telegram update id");
    assert_true(strcmp(update.chat_id, "222") == 0, "telegram chat id");
    assert_true(strcmp(update.from_id, "111") == 0, "telegram from id");
    assert_true(strcmp(update.text, "/status") == 0, "telegram text");

    assert_true(espclaw_telegram_extract_update(large_id_update_json, &update), "telegram large-id update extracted");
    assert_true(strcmp(update.chat_id, "6568730383") == 0, "telegram large chat id preserved");
    assert_true(strcmp(update.from_id, "6568730383") == 0, "telegram large from id preserved");
    assert_true(strcmp(update.text, "hi") == 0, "telegram large-id text preserved");

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
    assert_true(
        espclaw_app_scaffold_lua(
            temp_dir,
            "llm_installed_app",
            "LLM Installed App",
            "fs.read,fs.write,gpio.read,gpio.write,pwm.write,adc.read,i2c.read,i2c.write,uart.read,uart.write,camera.capture,temperature.read,imu.read,buzzer.play,ppm.write,task.control",
            "manual,boot,timer,uart,sensor"
        ) == 0,
        "large default app scaffolded"
    );
    assert_true(
        espclaw_app_scaffold_lua(temp_dir, "empty_defaults_app", "Empty Defaults App", "", "") == 0,
        "empty csv scaffold falls back to defaults"
    );
    assert_true(
        espclaw_app_scaffold_lua(temp_dir, "alias_app", "Alias App", "pwm,gpio,filesystem", "run,start") == 0,
        "alias scaffold normalizes model shorthand"
    );
    assert_true(espclaw_app_load_manifest(temp_dir, "hello_app", &manifest) == 0, "scaffold manifest load");
    assert_true(strcmp(manifest.entrypoint, "main.lua") == 0, "entrypoint loaded");
    assert_true(espclaw_app_collect_ids(temp_dir, ids, 4, &count) == 0, "apps collected");
    assert_true(count == 4, "four apps collected");
    assert_true(
        (strcmp(ids[0], "hello_app") == 0 || strcmp(ids[1], "hello_app") == 0 || strcmp(ids[2], "hello_app") == 0 || strcmp(ids[3], "hello_app") == 0) &&
            (strcmp(ids[0], "llm_installed_app") == 0 || strcmp(ids[1], "llm_installed_app") == 0 || strcmp(ids[2], "llm_installed_app") == 0 || strcmp(ids[3], "llm_installed_app") == 0) &&
            (strcmp(ids[0], "empty_defaults_app") == 0 || strcmp(ids[1], "empty_defaults_app") == 0 || strcmp(ids[2], "empty_defaults_app") == 0 || strcmp(ids[3], "empty_defaults_app") == 0) &&
            (strcmp(ids[0], "alias_app") == 0 || strcmp(ids[1], "alias_app") == 0 || strcmp(ids[2], "alias_app") == 0 || strcmp(ids[3], "alias_app") == 0),
        "app ids collected"
    );
    assert_true(espclaw_app_load_manifest(temp_dir, "alias_app", &manifest) == 0, "alias manifest load");
    assert_true(espclaw_app_has_permission(&manifest, "pwm.write"), "permission alias normalized to pwm.write");
    assert_true(espclaw_app_has_permission(&manifest, "gpio.write"), "permission alias normalized to gpio.write");
    assert_true(espclaw_app_has_permission(&manifest, "fs.read"), "permission alias normalized to fs.read");
    assert_true(espclaw_app_supports_trigger(&manifest, "manual"), "trigger alias normalized to manual");
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
    espclaw_board_profile_t cam = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32CAM);
    uint8_t uart_output[64];
    uint8_t i2c_registers[] = {0x12, 0x34};
    espclaw_hw_pwm_state_t pwm_state;
    size_t uart_output_length = 0;

    make_temp_dir(temp_dir, sizeof(temp_dir));
    assert_true(espclaw_workspace_bootstrap(temp_dir) == 0, "workspace bootstrap for vehicle app");
    assert_true(
        espclaw_workspace_write_file(
            temp_dir,
            "config/board.json",
            "{\n"
            "  \"variant\": \"ai_thinker_esp32cam\",\n"
            "  \"pins\": {\"buzzer\": 5, \"servo_main\": 13, \"ppm_out\": 14},\n"
            "  \"i2c\": {\"default\": {\"port\": 0, \"sda\": 21, \"scl\": 22, \"frequency_hz\": 400000}},\n"
            "  \"adc\": {\"battery\": {\"unit\": 1, \"channel\": 3}}\n"
            "}\n"
        ) == 0,
        "vehicle app board config written"
    );
    assert_true(
        espclaw_board_configure_current(temp_dir, &cam) == 0,
        "vehicle app board config loaded"
    );
    assert_true(
        espclaw_app_scaffold_lua(
            temp_dir,
            "hardware_app",
            "Hardware App",
            "gpio.read,gpio.write,pwm.write,adc.read,i2c.read,i2c.write,buzzer.play,uart.read,uart.write,task.control",
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
            "  local variant_direct = espclaw.board.variant()\n"
            "  local hardware = espclaw.hardware.list()\n"
            "  return string.format(\"adc=%d temp=%d pin=%d i2c=%d pwm=%d pid=%d uart_in=%s uart_written=%d variant=%s variant_direct=%s capabilities=%d\", adc, bytes[1] * 256 + bytes[2], pin, #devices, pwm.duty, math.floor(output), uart_in, uart_written, hardware.variant, variant_direct, #hardware.capabilities)\n"
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
    assert_string_contains(output, "variant=ai_thinker_esp32cam", "hardware list exposed board variant");
    assert_string_contains(output, "variant_direct=ai_thinker_esp32cam", "board variant helper exposed variant id string");
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
    espclaw_board_profile_t cam = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32CAM);
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
    assert_true(espclaw_workspace_bootstrap(temp_dir) == 0, "workspace bootstrap for vehicle app");
    assert_true(
        espclaw_workspace_write_file(
            temp_dir,
            "config/board.json",
            "{\n"
            "  \"variant\": \"ai_thinker_esp32cam\",\n"
            "  \"pins\": {\"servo_main\": 13, \"ppm_out\": 14},\n"
            "  \"i2c\": {\"default\": {\"port\": 0, \"sda\": 21, \"scl\": 22, \"frequency_hz\": 400000}},\n"
            "  \"adc\": {\"battery\": {\"unit\": 1, \"channel\": 3}}\n"
            "}\n"
        ) == 0,
        "vehicle app board config written"
    );
    assert_true(espclaw_board_configure_current(temp_dir, &cam) == 0, "vehicle app board configured");
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
            "  local variant = espclaw.board.variant()\n"
            "  local battery = espclaw.board.adc('battery')\n"
            "  assert(variant == 'ai_thinker_esp32cam')\n"
            "  assert(espclaw.i2c.begin_board('default'))\n"
            "  assert(espclaw.imu.mpu6050_begin(0, 0x68))\n"
            "  local sample = espclaw.imu.mpu6050_read(0, 0x68)\n"
            "  local roll, pitch = espclaw.imu.complementary_roll_pitch(sample, 0, 0, 0.98, 0.01)\n"
            "  local temp = espclaw.temperature.tmp102_c(0, 0x48)\n"
            "  local battery_mv = espclaw.adc.read_named_mv('battery')\n"
            "  local drive = espclaw.control.mix_differential(0.6, -0.2, -1, 1)\n"
            "  local quad = espclaw.control.mix_quad_x(0.5, 0.1, -0.05, 0.02, 0, 1)\n"
            "  assert(battery.unit == 1 and battery.channel == 3)\n"
            "  assert(espclaw.servo.attach(1, 'servo_main'))\n"
            "  local pulse = espclaw.servo.write_norm(1, drive.left, 1000, 2000)\n"
            "  assert(espclaw.ppm.begin(0, 'ppm_out', 22500, 300))\n"
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

static void test_task_runtime(void)
{
    char temp_dir[128];
    char output[256];
    espclaw_task_status_t tasks[ESPCLAW_TASK_RUNTIME_MAX];
    size_t count = 0;
    size_t index = 0;
    bool found = false;

    make_temp_dir(temp_dir, sizeof(temp_dir));
    assert_true(
        espclaw_app_scaffold_lua(temp_dir, "task_app", "Task App", "fs.read", "manual,sensor") == 0,
        "task app scaffolded"
    );
    assert_true(
        espclaw_app_update_source(
            temp_dir,
            "task_app",
            "counter = counter or 0\n"
            "event_counter = event_counter or 0\n"
            "function on_sensor(payload)\n"
            "  event_counter = event_counter + 1\n"
            "  return string.format(\"event=%d payload=%s\", event_counter, payload)\n"
            "end\n"
            "function handle(trigger, payload)\n"
            "  counter = counter + 1\n"
            "  return string.format(\"task=%d payload=%s\", counter, payload)\n"
            "end\n"
        ) == 0,
        "task app source updated"
    );

#ifdef ESPCLAW_HOST_LUA
    assert_true(
        espclaw_task_start("telemetry_task", temp_dir, "task_app", "manual", "ping", 5, 3, output, sizeof(output)) == 0,
        "task started"
    );
    for (index = 0; index < 50; ++index) {
        espclaw_hw_sleep_ms(5);
        count = espclaw_task_snapshot_all(tasks, ESPCLAW_TASK_RUNTIME_MAX);
        found = false;
        for (size_t task_index = 0; task_index < count; ++task_index) {
            if (strcmp(tasks[task_index].task_id, "telemetry_task") == 0) {
                found = tasks[task_index].completed;
                break;
            }
        }
        if (found) {
            break;
        }
    }
    assert_true(count > 0, "task snapshot returned data");
    found = false;
    for (size_t task_index = 0; task_index < count; ++task_index) {
        if (strcmp(tasks[task_index].task_id, "telemetry_task") == 0) {
            assert_true(strcmp(tasks[task_index].schedule, "periodic") == 0, "task schedule tracked");
            assert_true(tasks[task_index].completed, "task completed");
            assert_true(tasks[task_index].iterations_completed == 3, "task iteration count tracked");
            assert_string_contains(tasks[task_index].last_result, "task=3 payload=ping", "task reused persistent lua vm");
            found = true;
            break;
        }
    }
    assert_true(found, "telemetry task present in snapshot");

    assert_true(
        espclaw_task_start("stop_task", temp_dir, "task_app", "manual", "spin", 20, 0, output, sizeof(output)) == 0,
        "infinite task started"
    );
    espclaw_hw_sleep_ms(60);
    assert_true(espclaw_task_stop("stop_task", output, sizeof(output)) == 0, "task stop requested");
    for (index = 0; index < 50; ++index) {
        espclaw_hw_sleep_ms(5);
        count = espclaw_task_snapshot_all(tasks, ESPCLAW_TASK_RUNTIME_MAX);
        found = false;
        for (size_t task_index = 0; task_index < count; ++task_index) {
            if (strcmp(tasks[task_index].task_id, "stop_task") == 0) {
                found = tasks[task_index].completed;
                break;
            }
        }
        if (found) {
            break;
        }
    }
    found = false;
    for (size_t task_index = 0; task_index < count; ++task_index) {
        if (strcmp(tasks[task_index].task_id, "stop_task") == 0) {
            assert_true(tasks[task_index].stop_requested, "task stop tracked");
            found = true;
            break;
        }
    }
    assert_true(found, "stop task present in snapshot");

    assert_true(
        espclaw_task_start_with_schedule(
            "sensor_task",
            temp_dir,
            "task_app",
            "event",
            "sensor",
            "",
            0,
            2,
            output,
            sizeof(output)) == 0,
        "event task started"
    );
    assert_true(
        espclaw_task_emit_event("sensor", "first", output, sizeof(output)) == 0,
        "first event emitted"
    );
    assert_true(
        espclaw_task_emit_event("sensor", "second", output, sizeof(output)) == 0,
        "second event emitted"
    );
    for (index = 0; index < 50; ++index) {
        espclaw_hw_sleep_ms(5);
        count = espclaw_task_snapshot_all(tasks, ESPCLAW_TASK_RUNTIME_MAX);
        found = false;
        for (size_t task_index = 0; task_index < count; ++task_index) {
            if (strcmp(tasks[task_index].task_id, "sensor_task") == 0) {
                found = tasks[task_index].completed;
                if (found) {
                    assert_true(strcmp(tasks[task_index].schedule, "event") == 0, "event task schedule tracked");
                    assert_true(tasks[task_index].events_received == 2, "event task count tracked");
                    assert_string_contains(tasks[task_index].last_result, "event=2 payload=second", "event payload delivered");
                }
                break;
            }
        }
        if (found) {
            break;
        }
    }
    assert_true(found, "event task completed");
    espclaw_task_shutdown_all();
#else
    assert_true(true, "task runtime test is host-only");
#endif
}

static void test_event_watch_runtime(void)
{
    char temp_dir[128];
    char output[256];
    char watches_json[2048];
    espclaw_task_status_t tasks[ESPCLAW_TASK_RUNTIME_MAX];
    espclaw_board_profile_t cam_profile = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32CAM);
    size_t count = 0;
    size_t index;
    bool found_uart = false;
    bool found_sensor = false;

    make_temp_dir(temp_dir, sizeof(temp_dir));
    assert_true(espclaw_workspace_bootstrap(temp_dir) == 0, "workspace bootstrap for event watch runtime");
    espclaw_hw_sim_reset();
    espclaw_board_configure_current(temp_dir, &cam_profile);
    assert_true(espclaw_event_watch_runtime_start() == 0, "event watch runtime started");
    assert_true(
        espclaw_app_scaffold_lua(
            temp_dir,
            "watch_app",
            "Watch App",
            "task.control,uart.read,camera.capture,adc.read",
            "uart,sensor,manual") == 0,
        "watch app scaffolded"
    );
    assert_true(
        espclaw_app_update_source(
            temp_dir,
            "watch_app",
            "function on_uart(payload)\n"
            "  return 'uart:' .. payload\n"
            "end\n"
            "function on_sensor(payload)\n"
            "  return 'sensor:' .. payload\n"
            "end\n"
        ) == 0,
        "watch app source updated"
    );

#ifdef ESPCLAW_HOST_LUA
    assert_true(
        espclaw_task_start_with_schedule(
            "uart_task",
            temp_dir,
            "watch_app",
            "event",
            "uart",
            "",
            0,
            1,
            output,
            sizeof(output)) == 0,
        "uart task started"
    );
    assert_true(
        espclaw_task_start_with_schedule(
            "sensor_task_watch",
            temp_dir,
            "watch_app",
            "event",
            "sensor",
            "",
            0,
            1,
            output,
            sizeof(output)) == 0,
        "sensor task started"
    );
    assert_true(
        espclaw_event_watch_add_uart("uart_console", "uart", 0, output, sizeof(output)) == 0,
        "uart watch added"
    );
    assert_true(
        espclaw_event_watch_add_adc_threshold("battery_high", "sensor", 1, 3, 1000, 25, output, sizeof(output)) == 0,
        "adc watch added"
    );
    assert_true(espclaw_event_watch_render_json(watches_json, sizeof(watches_json)) == 0, "watch json rendered");
    assert_string_contains(watches_json, "\"watch_id\":\"uart_console\"", "uart watch listed");
    assert_string_contains(watches_json, "\"watch_id\":\"battery_high\"", "adc watch listed");

    espclaw_hw_sim_uart_feed_input(0, (const uint8_t *)"ping\n", 5);
    espclaw_hw_sim_set_adc_raw(1, 3, 500);
    espclaw_hw_sleep_ms(75);
    espclaw_hw_sim_set_adc_raw(1, 3, 1500);

    for (index = 0; index < 80; ++index) {
        size_t task_index;

        espclaw_hw_sleep_ms(10);
        count = espclaw_task_snapshot_all(tasks, ESPCLAW_TASK_RUNTIME_MAX);
        found_uart = false;
        found_sensor = false;
        for (task_index = 0; task_index < count; ++task_index) {
            if (strcmp(tasks[task_index].task_id, "uart_task") == 0 && tasks[task_index].completed) {
                assert_string_contains(tasks[task_index].last_result, "uart:ping", "uart watch payload delivered");
                found_uart = true;
            }
            if (strcmp(tasks[task_index].task_id, "sensor_task_watch") == 0 && tasks[task_index].completed) {
                assert_string_contains(tasks[task_index].last_result, "\"state\":\"above\"", "adc threshold payload delivered");
                found_sensor = true;
            }
        }
        if (found_uart && found_sensor) {
            break;
        }
    }

    assert_true(found_uart, "uart event watch triggered local task");
    assert_true(found_sensor, "adc event watch triggered local task");
    assert_true(espclaw_event_watch_remove("uart_console", output, sizeof(output)) == 0, "uart watch removed");
    assert_true(espclaw_event_watch_remove("battery_high", output, sizeof(output)) == 0, "adc watch removed");
    espclaw_task_shutdown_all();
#else
    assert_true(true, "event watch runtime test is host-only");
#endif
}

static void test_behavior_runtime(void)
{
    char temp_dir[128];
    char output[256];
    char json[2048];
    char behavior_index[256];
    char autostart_index[256];
    espclaw_behavior_spec_t spec;
    espclaw_behavior_status_t behaviors[ESPCLAW_TASK_RUNTIME_MAX];
    espclaw_task_status_t tasks[ESPCLAW_TASK_RUNTIME_MAX];
    size_t count = 0;
    size_t index = 0;
    bool found = false;

    make_temp_dir(temp_dir, sizeof(temp_dir));
    assert_true(espclaw_workspace_bootstrap(temp_dir) == 0, "workspace bootstrapped for behavior runtime");
    assert_true(
        espclaw_app_scaffold_lua(temp_dir, "behavior_app", "Behavior App", "task.control", "timer,manual") == 0,
        "behavior app scaffolded"
    );
    assert_true(
        espclaw_app_update_source(
            temp_dir,
            "behavior_app",
            "counter = counter or 0\n"
            "function on_timer(payload)\n"
            "  counter = counter + 1\n"
            "  return string.format(\"behavior=%d payload=%s\", counter, payload)\n"
            "end\n"
            "function handle(trigger, payload)\n"
            "  return on_timer(payload)\n"
            "end\n"
        ) == 0,
        "behavior app source updated"
    );

    memset(&spec, 0, sizeof(spec));
    snprintf(spec.behavior_id, sizeof(spec.behavior_id), "avoidance");
    snprintf(spec.title, sizeof(spec.title), "Obstacle Avoidance");
    snprintf(spec.app_id, sizeof(spec.app_id), "behavior_app");
    snprintf(spec.schedule, sizeof(spec.schedule), "periodic");
    snprintf(spec.trigger, sizeof(spec.trigger), "timer");
    snprintf(spec.payload, sizeof(spec.payload), "tick");
    spec.period_ms = 5;
    spec.max_iterations = 2;
    spec.autostart = true;

    assert_true(
        espclaw_behavior_register(temp_dir, &spec, output, sizeof(output)) == 0,
        "behavior registered"
    );
    assert_true(
        espclaw_behavior_load(temp_dir, "avoidance", &spec) == 0,
        "behavior loaded"
    );
    assert_true(spec.autostart, "behavior autostart persisted");
    assert_true(strcmp(spec.app_id, "behavior_app") == 0, "behavior app id persisted");

    assert_true(
        espclaw_behavior_render_json(temp_dir, json, sizeof(json)) == 0,
        "behavior json rendered"
    );
    assert_string_contains(json, "\"behavior_id\":\"avoidance\"", "behavior json includes id");
    assert_string_contains(json, "\"autostart\":true", "behavior json includes autostart");
    assert_true(
        espclaw_workspace_read_file(temp_dir, "config/behavior_index.txt", behavior_index, sizeof(behavior_index)) == 0,
        "behavior index persisted"
    );
    assert_true(strcmp(behavior_index, "avoidance") == 0, "behavior index contains saved behavior");
    assert_true(
        espclaw_workspace_read_file(temp_dir, "config/behavior_autostart.txt", autostart_index, sizeof(autostart_index)) == 0,
        "behavior autostart index persisted"
    );
    assert_true(strcmp(autostart_index, "avoidance") == 0, "behavior autostart index contains saved behavior");

#ifdef ESPCLAW_HOST_LUA
    assert_true(
        espclaw_behavior_start_autostart(temp_dir, output, sizeof(output)) == 0,
        "autostart behaviors started"
    );
    for (index = 0; index < 50; ++index) {
        espclaw_hw_sleep_ms(5);
        count = espclaw_behavior_snapshot_all(temp_dir, behaviors, ESPCLAW_TASK_RUNTIME_MAX);
        found = false;
        for (size_t behavior_index = 0; behavior_index < count; ++behavior_index) {
            if (strcmp(behaviors[behavior_index].spec.behavior_id, "avoidance") == 0) {
                found = behaviors[behavior_index].completed;
                if (found) {
                    assert_true(behaviors[behavior_index].iterations_completed == 2, "behavior iteration count tracked");
                    assert_string_contains(behaviors[behavior_index].last_result, "behavior=2 payload=tick", "behavior result tracked");
                }
                break;
            }
        }
        if (found) {
            break;
        }
    }
    assert_true(found, "autostart behavior completed");
    assert_true(
        espclaw_behavior_stop("avoidance", output, sizeof(output)) == 0 || strstr(output, "not found") != NULL,
        "behavior stop path callable"
    );
    count = espclaw_task_snapshot_all(tasks, ESPCLAW_TASK_RUNTIME_MAX);
    assert_true(count > 0, "behavior produced task snapshot");
    espclaw_task_shutdown_all();
#else
    assert_true(true, "behavior runtime test is host-only");
#endif

    assert_true(
        espclaw_behavior_remove(temp_dir, "avoidance", output, sizeof(output)) == 0,
        "behavior removed"
    );
    assert_true(
        espclaw_workspace_read_file(temp_dir, "config/behavior_index.txt", behavior_index, sizeof(behavior_index)) == 0,
        "behavior index still readable after remove"
    );
    assert_true(behavior_index[0] == '\0', "behavior index cleared after remove");
    assert_true(
        espclaw_workspace_read_file(temp_dir, "config/behavior_autostart.txt", autostart_index, sizeof(autostart_index)) == 0,
        "behavior autostart index still readable after remove"
    );
    assert_true(autostart_index[0] == '\0', "behavior autostart index cleared after remove");
    count = espclaw_behavior_snapshot_all(temp_dir, behaviors, ESPCLAW_TASK_RUNTIME_MAX);
    found = false;
    for (index = 0; index < count; ++index) {
        if (strcmp(behaviors[index].spec.behavior_id, "avoidance") == 0) {
            found = true;
            break;
        }
    }
    assert_true(!found, "removed behavior absent from snapshot");
}

static void test_app_runtime_event_handlers(void)
{
    char temp_dir[128];
    char output[256];

    make_temp_dir(temp_dir, sizeof(temp_dir));
    assert_true(
        espclaw_app_scaffold_lua(temp_dir, "event_app", "Event App", "task.control", "timer,sensor,manual") == 0,
        "event app scaffolded"
    );
    assert_true(
        espclaw_app_update_source(
            temp_dir,
            "event_app",
            "function on_timer(payload)\n"
            "  return 'timer:' .. payload\n"
            "end\n"
            "function on_event(trigger, payload)\n"
            "  return trigger .. ':' .. payload\n"
            "end\n"
        ) == 0,
        "event app source updated"
    );

#ifdef ESPCLAW_HOST_LUA
    assert_true(
        espclaw_app_run(temp_dir, "event_app", "timer", "tick", output, sizeof(output)) == 0,
        "named timer handler ran"
    );
    assert_string_contains(output, "timer:tick", "on_timer handler selected");
    assert_true(
        espclaw_app_run(temp_dir, "event_app", "sensor", "near", output, sizeof(output)) == 0,
        "generic event handler ran"
    );
    assert_string_contains(output, "sensor:near", "on_event handler selected");
#else
    assert_true(true, "event handler test is host-only");
#endif
}

static void test_app_runtime_module_entrypoints(void)
{
    char temp_dir[128];
    char output[256];

    make_temp_dir(temp_dir, sizeof(temp_dir));
    assert_true(
        espclaw_app_scaffold_lua(temp_dir, "module_entry_app", "Module Entry App", "fs.read", "manual,event") == 0,
        "module entry app scaffolded"
    );
    assert_true(
        espclaw_app_update_source(
            temp_dir,
            "module_entry_app",
            "local M = {}\n"
            "function M.manual(payload)\n"
            "  return 'module-manual:' .. payload\n"
            "end\n"
            "function M.handle(trigger, payload)\n"
            "  return 'module-handle:' .. trigger .. ':' .. payload\n"
            "end\n"
            "return M\n"
        ) == 0,
        "module entry app source updated"
    );

#ifdef ESPCLAW_HOST_LUA
    assert_true(
        espclaw_app_run(temp_dir, "module_entry_app", "manual", "payload", output, sizeof(output)) == 0,
        "module entry app manual trigger ran"
    );
    assert_string_contains(output, "module-manual:payload", "module manual handler selected");
    assert_true(
        espclaw_app_run(temp_dir, "module_entry_app", "event", "near", output, sizeof(output)) == 0,
        "module entry app generic handle ran"
    );
    assert_string_contains(output, "module-handle:event:near", "module handle fallback selected");
#else
    assert_true(true, "module entrypoint test is host-only");
#endif
}

static void test_lua_module_require_paths(void)
{
    char temp_dir[128];
    char path_buffer[256];
    char output[256];
    espclaw_app_vm_t *vm = NULL;

    make_temp_dir(temp_dir, sizeof(temp_dir));
    assert_true(espclaw_workspace_bootstrap(temp_dir) == 0, "workspace bootstrap for lua modules");
    snprintf(path_buffer, sizeof(path_buffer), "%s/lib", temp_dir);
    assert_true(mkdir(path_buffer, 0700) == 0, "workspace lib directory created");
    assert_true(
        snprintf(path_buffer, sizeof(path_buffer), "%s/lib/sensor_math.lua", temp_dir) < (int)sizeof(path_buffer),
        "workspace module path fits"
    );
    write_text_file(
        path_buffer,
        "local M = {}\n"
        "function M.scale(value)\n"
        "  return value * 2\n"
        "end\n"
        "return M\n"
    );

    assert_true(
        espclaw_app_scaffold_lua(temp_dir, "module_app", "Module App", "fs.read", "manual") == 0,
        "module app scaffolded"
    );
    snprintf(path_buffer, sizeof(path_buffer), "%s/apps/module_app/lib", temp_dir);
    assert_true(mkdir(path_buffer, 0700) == 0, "app lib directory created");
    assert_true(
        snprintf(path_buffer, sizeof(path_buffer), "%s/apps/module_app/lib/message_parts.lua", temp_dir) < (int)sizeof(path_buffer),
        "app module path fits"
    );
    write_text_file(
        path_buffer,
        "local M = {}\n"
        "function M.render(label, value)\n"
        "  return string.format(\"%s=%d\", label, value)\n"
        "end\n"
        "return M\n"
    );
    assert_true(
        espclaw_app_update_source(
            temp_dir,
            "module_app",
            "local sensor_math = require('sensor_math')\n"
            "local message_parts = require('message_parts')\n"
            "function handle(trigger, payload)\n"
            "  return message_parts.render(payload, sensor_math.scale(21))\n"
            "end\n"
        ) == 0,
        "module app source updated"
    );

#ifdef ESPCLAW_HOST_LUA
    assert_true(
        espclaw_app_vm_open(temp_dir, "module_app", &vm, output, sizeof(output)) == 0,
        "module app vm opened"
    );
    assert_true(
        espclaw_app_vm_step(vm, "manual", "scaled", output, sizeof(output)) == 0,
        "module app vm step succeeded"
    );
    assert_string_contains(output, "scaled=42", "module app required workspace and app-local modules");
    espclaw_app_vm_close(vm);
#else
    assert_true(true, "module require test is host-only");
#endif
}

static void test_app_runtime_fs_alias(void)
{
    char temp_dir[128];
    char output[256];

    make_temp_dir(temp_dir, sizeof(temp_dir));
    assert_true(
        espclaw_app_scaffold_lua(temp_dir, "fs_alias_app", "FS Alias App", "fs.read,fs.write", "manual") == 0,
        "fs alias app scaffolded"
    );
    assert_true(
        espclaw_app_update_source(
            temp_dir,
            "fs_alias_app",
            "function handle(trigger, payload)\n"
            "  espclaw.fs.write('memory/fs_alias.txt', payload)\n"
            "  return espclaw.fs.read('memory/fs_alias.txt')\n"
            "end\n"
        ) == 0,
        "fs alias app source updated"
    );

#ifdef ESPCLAW_HOST_LUA
    assert_true(
        espclaw_app_run(temp_dir, "fs_alias_app", "manual", "alias-ok", output, sizeof(output)) == 0,
        "fs alias app ran"
    );
    assert_string_contains(output, "alias-ok", "fs alias read/write succeeded");
#else
    assert_true(true, "fs alias test is host-only");
#endif
}

static void test_admin_scaffold_app_custom_triggers(void)
{
    char temp_dir[128];
    char manifest[1024];

    make_temp_dir(temp_dir, sizeof(temp_dir));
    assert_true(
        espclaw_admin_scaffold_app(
            temp_dir,
            "sensor_app",
            "Sensor App",
            "fs.read,fs.write",
            "manual,sensor"
        ) == 0,
        "custom scaffold app created"
    );
    assert_true(
        espclaw_workspace_read_file(temp_dir, "apps/sensor_app/app.json", manifest, sizeof(manifest)) == 0,
        "custom scaffold manifest readable"
    );
    assert_string_contains(manifest, "\"manual\"", "custom scaffold kept manual trigger");
    assert_string_contains(manifest, "\"sensor\"", "custom scaffold added sensor trigger");
    assert_string_contains(manifest, "\"fs.write\"", "custom scaffold kept permissions");
}

static void test_agent_loop(void)
{
    char temp_dir[128];
    char transcript[8192];
    espclaw_auth_profile_t profile;
    espclaw_agent_run_result_t result;
    test_agent_adapter_state_t adapter_state = {0};
    espclaw_board_profile_t camera_profile = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32CAM);

    make_temp_dir(temp_dir, sizeof(temp_dir));
    assert_true(espclaw_workspace_bootstrap(temp_dir) == 0, "workspace bootstrap for agent loop");
    espclaw_board_configure_current(temp_dir, &camera_profile);
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
        espclaw_agent_loop_run(temp_dir, "chat_tool_loop", "What apps are installed?", false, false, &result) == 0,
        "tool loop run succeeded"
    );
    assert_true(result.ok, "tool loop result ok");
    assert_true(result.used_tools, "tool loop used tools");
    assert_true(result.iterations >= 2, "tool loop used at least two response rounds");
    assert_string_contains(result.final_text, "listed the installed apps", "tool loop final text");
    assert_true(
        espclaw_session_read_transcript(temp_dir, "chat_tool_loop", transcript, sizeof(transcript)) == 0,
        "tool loop transcript readable"
    );
    assert_string_contains(transcript, "What apps are installed?", "tool loop stored user message");
    assert_string_contains(transcript, "Requested tools: system.info, app.list", "tool loop stored tool summary");
    assert_string_contains(transcript, "\"role\":\"tool\"", "tool loop stored tool output");

    adapter_state.calls = 0;
    espclaw_agent_set_http_adapter(sse_wrapped_http_adapter, &adapter_state);
    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_agent_loop_run(temp_dir, "chat_sse_wrapped", "Say hi.", false, false, &result) == 0,
        "sse wrapped loop succeeded"
    );
    espclaw_agent_set_http_adapter(NULL, NULL);
    assert_true(result.ok, "sse wrapped result ok");
    assert_true(!result.used_tools, "sse wrapped result did not use tools");
    assert_string_contains(result.response_id, "resp_sse_wrapped", "sse wrapped response id parsed");
    assert_true(strcmp(result.final_text, "ESPCLAW_BENCH_HI") == 0, "sse wrapped final text parsed");

    adapter_state.calls = 0;
    espclaw_agent_set_http_adapter(sse_stream_http_adapter, &adapter_state);
    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_agent_loop_run(temp_dir, "chat_sse_stream", "Say hi from sse stream.", false, false, &result) == 0,
        "sse stream fallback loop succeeded"
    );
    espclaw_agent_set_http_adapter(NULL, NULL);
    assert_true(result.ok, "sse stream fallback result ok");
    assert_true(!result.used_tools, "sse stream fallback result did not use tools");
    assert_string_contains(result.response_id, "resp_sse_stream", "sse stream fallback response id parsed");
    assert_true(strcmp(result.final_text, "ESPCLAW_BENCH_HI") == 0, "sse stream fallback final text parsed");

    adapter_state.calls = 0;
    espclaw_agent_set_http_adapter(long_output_text_http_adapter, &adapter_state);
    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_agent_loop_run(temp_dir, "chat_long_output_text", "Give me a long answer.", false, false, &result) == 0,
        "long output-text loop succeeded"
    );
    espclaw_agent_set_http_adapter(NULL, NULL);
    assert_true(result.ok, "long output-text result ok");
    assert_true(!result.used_tools, "long output-text result did not use tools");
    assert_string_contains(result.response_id, "resp_long_text", "long output-text response id parsed");
    assert_true(strlen(result.final_text) > 1500U, "long output-text final text preserved beyond the old tiny parser segment");

    snprintf(profile.base_url, sizeof(profile.base_url), "mock://fs-read");
    assert_true(espclaw_auth_store_save(&profile) == 0, "fs read auth profile saved");
    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_agent_loop_run(temp_dir, "chat_fs_read", "Read the control file.", false, false, &result) == 0,
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
        espclaw_agent_loop_run(temp_dir, "chat_tools_via_model", "List out all the available tools to you", false, false, &result) == 0,
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
        espclaw_agent_loop_run(temp_dir, "chat_confirm", "Run the demo app.", false, false, &result) == 0,
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

    adapter_state.calls = 0;
    espclaw_agent_set_http_adapter(app_install_http_adapter, &adapter_state);
    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_agent_loop_run(temp_dir, "chat_install", "Create a sensor agent Lua app and save it.", true, false, &result) == 0,
        "app install loop succeeded"
    );
    espclaw_agent_set_http_adapter(NULL, NULL);
    assert_true(result.ok, "app install result ok");
    assert_true(result.used_tools, "app install used tools");
    assert_string_contains(result.final_text, "installed the Lua app", "app install assistant reply returned");
    assert_true(espclaw_app_read_source(temp_dir, "sensor_agent", transcript, sizeof(transcript)) == 0, "installed app source readable");
    assert_string_contains(transcript, "function on_sensor(payload)", "installed app source persisted");

    adapter_state.calls = 0;
    espclaw_agent_set_http_adapter(app_install_arguments_first_http_adapter, &adapter_state);
    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_agent_loop_run(temp_dir, "chat_install_args_first", "Create another Lua app and save it.", true, true, &result) == 0,
        "arguments-first app install loop succeeded"
    );
    espclaw_agent_set_http_adapter(NULL, NULL);
    assert_true(result.ok, "arguments-first install result ok");
    assert_true(result.used_tools, "arguments-first install used tools");
    assert_true(
        espclaw_app_read_source(temp_dir, "args_first_app", transcript, sizeof(transcript)) == 0,
        "arguments-first app source readable"
    );
    assert_string_contains(transcript, "args-first:", "arguments-first app source persisted");

    adapter_state.calls = 0;
    espclaw_agent_set_http_adapter(app_install_retry_http_adapter, &adapter_state);
    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_agent_loop_run(
            temp_dir,
            "chat_install_retry",
            "Create an app called retry_app, install it, and reply INSTALLED.",
            true,
            true,
            &result) == 0,
        "app install retry loop succeeded"
    );
    espclaw_agent_set_http_adapter(NULL, NULL);
    assert_true(result.ok, "app install retry result ok");
    assert_true(result.used_tools, "app install retry used tools");
    assert_string_contains(result.final_text, "INSTALLED", "app install retry final text returned");
    assert_true(
        espclaw_app_read_source(temp_dir, "retry_app", transcript, sizeof(transcript)) == 0,
        "retry-installed app source readable"
    );
    assert_string_contains(transcript, "retry-ok", "retry-installed app source persisted");

    adapter_state.calls = 0;
    espclaw_agent_set_http_adapter(app_install_wrong_tool_retry_http_adapter, &adapter_state);
    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_agent_loop_run(
            temp_dir,
            "chat_install_retry_after_detour",
            "Create an app called retry_after_detour, install it, and reply INSTALLED.",
            true,
            true,
            &result) == 0,
        "app install retry after wrong-tool detour succeeded"
    );
    espclaw_agent_set_http_adapter(NULL, NULL);
    assert_true(result.ok, "app install retry after wrong-tool detour result ok");
    assert_true(result.used_tools, "app install retry after wrong-tool detour used tools");
    assert_true(
        espclaw_app_read_source(temp_dir, "retry_after_detour", transcript, sizeof(transcript)) == 0,
        "retry-after-detour app source readable"
    );
    assert_string_contains(transcript, "detour-ok", "retry-after-detour app source persisted");

    espclaw_web_set_http_adapter(web_tool_http_adapter, NULL);
    adapter_state.calls = 0;
    espclaw_agent_set_http_adapter(web_search_fetch_retry_http_adapter, &adapter_state);
    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_agent_loop_run(
            temp_dir,
            "chat_web_retry",
            "Use web search and fetch to inspect the ms5611 datasheet, then summarize it.",
            true,
            true,
            &result) == 0,
        "web search/fetch retry loop succeeded"
    );
    espclaw_agent_set_http_adapter(NULL, NULL);
    espclaw_web_set_http_adapter(NULL, NULL);
    assert_true(result.ok, "web search/fetch retry result ok");
    assert_true(result.used_tools, "web search/fetch retry used tools");
    assert_string_contains(result.final_text, "Used both web.search and web.fetch successfully.", "web retry final text returned");
    assert_true(adapter_state.calls == 3, "web retry adapter saw corrective third turn");

    adapter_state.calls = 0;
    espclaw_agent_set_http_adapter(behavior_register_http_adapter, &adapter_state);
    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_agent_loop_run(temp_dir, "chat_behavior", "Create an autonomous wall watcher behavior that reacts to sensor events.", true, false, &result) == 0,
        "behavior register loop succeeded"
    );
    espclaw_agent_set_http_adapter(NULL, NULL);
    assert_true(result.ok, "behavior register result ok");
    assert_true(result.used_tools, "behavior register used tools");
    assert_string_contains(result.final_text, "autonomous behavior", "behavior register assistant reply returned");
    assert_true(
        espclaw_behavior_render_json(temp_dir, transcript, sizeof(transcript)) == 0,
        "behavior json readable after model install"
    );
    assert_string_contains(transcript, "\"behavior_id\":\"wall_watch\"", "behavior definition persisted");
    assert_string_contains(transcript, "\"autostart\":true", "behavior autostart persisted");

    adapter_state.calls = 0;
    espclaw_agent_set_http_adapter(camera_capture_http_adapter, &adapter_state);
    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_agent_loop_run(temp_dir, "chat_camera", "Capture a camera image and inspect it.", true, false, &result) == 0,
        "camera capture loop succeeded"
    );
    espclaw_agent_set_http_adapter(NULL, NULL);
    assert_true(result.ok, "camera capture result ok");
    assert_true(result.used_tools, "camera capture used tools");
    assert_string_contains(result.final_text, "captured camera frame", "camera capture assistant reply returned");
    assert_true(
        espclaw_session_read_transcript(temp_dir, "chat_camera", transcript, sizeof(transcript)) == 0,
        "camera transcript readable"
    );
    assert_string_contains(transcript, "vision_test.jpg", "camera tool output persisted");

    adapter_state.calls = 0;
    espclaw_agent_set_http_adapter(empty_completion_http_adapter, &adapter_state);
    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_agent_loop_run(temp_dir, "chat_empty_completion", "List your tools and finish.", true, true, &result) == 0,
        "empty completion after tool round succeeded"
    );
    espclaw_agent_set_http_adapter(NULL, NULL);
    assert_true(result.ok, "empty completion result ok");
    assert_true(result.used_tools, "empty completion used tools");
    assert_true(result.final_text[0] == '\0', "empty completion preserves empty final text");
    assert_true(
        espclaw_session_read_transcript(temp_dir, "chat_empty_completion", transcript, sizeof(transcript)) == 0,
        "empty completion transcript readable"
    );
    assert_string_contains(transcript, "Requested tools: tool.list", "empty completion stored tool summary");
    assert_string_contains(transcript, "\"role\":\"assistant\",\"content\":\"\"", "empty completion stored blank assistant message");

    {
        char malformed_session_path[256];
        FILE *malformed_session = NULL;

        snprintf(malformed_session_path, sizeof(malformed_session_path), "%s/sessions/%s.jsonl", temp_dir, "chat_malformed_role");
        malformed_session = fopen(malformed_session_path, "w");
        assert_true(malformed_session != NULL, "malformed session file opened");
        fputs("{\"role\":\"assistant\",\"content\":\"Earlier reply\"}\n", malformed_session);
        fputs("{\"role\":\"\",\"content\":\"Broken row\"}\n", malformed_session);
        fputs("{\"role\":\"user\",\"content\":\"Current question\"}\n", malformed_session);
        fclose(malformed_session);
    }

    adapter_state.calls = 0;
    espclaw_agent_set_http_adapter(malformed_role_history_http_adapter, &adapter_state);
    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_agent_loop_run(temp_dir, "chat_malformed_role", "Please continue.", false, false, &result) == 0,
        "malformed history role loop succeeded"
    );
    espclaw_agent_set_http_adapter(NULL, NULL);
    assert_true(result.ok, "malformed history role result ok");
    assert_string_contains(result.final_text, "History normalization worked.", "malformed history role final text returned");

    {
        char explicit_retry_session_path[256];
        FILE *explicit_retry_session = NULL;

        snprintf(explicit_retry_session_path, sizeof(explicit_retry_session_path), "%s/sessions/%s.jsonl", temp_dir, "chat_explicit_tool_retry");
        explicit_retry_session = fopen(explicit_retry_session_path, "w");
        assert_true(explicit_retry_session != NULL, "explicit tool retry session file opened");
        fputs("{\"role\":\"assistant\",\"content\":\"Please run task.list and share the output.\"}\n", explicit_retry_session);
        fclose(explicit_retry_session);
    }

    adapter_state.calls = 0;
    espclaw_agent_set_http_adapter(explicit_tool_retry_http_adapter, &adapter_state);
    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_agent_loop_run(temp_dir, "chat_explicit_tool_retry", "yes do", true, true, &result) == 0,
        "explicit tool retry loop succeeded"
    );
    espclaw_agent_set_http_adapter(NULL, NULL);
    assert_true(result.ok, "explicit tool retry result ok");
    assert_true(result.used_tools, "explicit tool retry used tools");
    assert_true(adapter_state.calls == 3U, "explicit tool retry used corrective second turn");
    assert_string_contains(result.final_text, "Called task.list", "explicit tool retry final text returned");

    {
        char explicit_alias_retry_session_path[256];
        FILE *explicit_alias_retry_session = NULL;

        snprintf(
            explicit_alias_retry_session_path,
            sizeof(explicit_alias_retry_session_path),
            "%s/sessions/%s.jsonl",
            temp_dir,
            "chat_explicit_tool_alias_retry");
        explicit_alias_retry_session = fopen(explicit_alias_retry_session_path, "w");
        assert_true(explicit_alias_retry_session != NULL, "explicit alias retry session file opened");
        fputs(
            "{\"role\":\"assistant\",\"content\":\"Great. I can emit an event and then check task.list to confirm it.\"}\n",
            explicit_alias_retry_session);
        fclose(explicit_alias_retry_session);
    }

    adapter_state.calls = 0;
    espclaw_agent_set_http_adapter(explicit_tool_alias_retry_http_adapter, &adapter_state);
    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_agent_loop_run(temp_dir, "chat_explicit_tool_alias_retry", "yes try that", true, true, &result) == 0,
        "explicit alias retry loop succeeded"
    );
    espclaw_agent_set_http_adapter(NULL, NULL);
    assert_true(result.ok, "explicit alias retry result ok");
    assert_true(result.used_tools, "explicit alias retry used tools");
    assert_true(adapter_state.calls == 3U, "explicit alias retry used corrective second turn");
    assert_string_contains(result.final_text, "event.emit and task.list", "explicit alias retry final text returned");

    {
        espclaw_esp32cam_sd_attempt_t attempt = {0};

        assert_true(espclaw_storage_get_esp32cam_attempt(0, &attempt), "esp32cam first sd attempt available");
        assert_true(strcmp(attempt.label, "sdspi") == 0, "esp32cam prefers sdspi first");
    }
}

static void test_console_chat_and_web_tools(void)
{
    char temp_dir[128];
    char transcript[8192];
    char buffer[2048];
    char stored_path[128];
    espclaw_agent_run_result_t result;
    espclaw_console_runtime_adapter_t adapter = {
        .status = console_status_adapter,
        .wifi_scan = console_wifi_scan_adapter,
        .wifi_join = console_wifi_join_adapter,
        .telegram_get_config = console_telegram_get_config_adapter,
        .telegram_set_config = console_telegram_set_config_adapter,
        .factory_reset = console_factory_reset_adapter,
        .reboot = console_reboot_adapter,
    };

    make_temp_dir(temp_dir, sizeof(temp_dir));
    assert_true(espclaw_workspace_bootstrap(temp_dir) == 0, "workspace bootstrap for console chat");
    assert_true(espclaw_auth_store_init(temp_dir) == 0, "console chat auth store init");

    memset(&s_console_status, 0, sizeof(s_console_status));
    s_console_status.profile = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32CAM);
    s_console_status.storage_backend = ESPCLAW_STORAGE_BACKEND_SD_CARD;
    s_console_status.storage_ready = true;
    s_console_status.wifi_ready = true;
    snprintf(s_console_status.workspace_root, sizeof(s_console_status.workspace_root), "%s", temp_dir);
    snprintf(s_console_status.wifi_ssid, sizeof(s_console_status.wifi_ssid), "LabNet");
    s_console_reboot_requested = false;
    s_console_factory_reset_requested = false;
    memset(&s_console_telegram_config, 0, sizeof(s_console_telegram_config));
    s_console_telegram_config.enabled = true;
    s_console_telegram_config.poll_interval_seconds = 5;
    snprintf(s_console_telegram_config.token_hint, sizeof(s_console_telegram_config.token_hint), "unset");

    espclaw_console_set_runtime_adapter(&adapter);
    espclaw_web_set_http_adapter(web_tool_http_adapter, NULL);

    memset(&result, 0, sizeof(result));
    assert_true(espclaw_console_run(temp_dir, "console_help", "/help", true, false, &result) == 0, "console help succeeded");
    assert_true(result.ok, "console help result ok");
    assert_string_contains(result.final_text, "/tool <name> [json]", "console help lists tool command");

    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_console_run(temp_dir, "console_memory", "/memory", true, false, &result) == 0,
        "console memory succeeded"
    );
    assert_string_contains(result.final_text, "HEARTBEAT.md", "console memory explains heartbeat file");
    assert_string_contains(result.final_text, "fs.write", "console memory explains update path");

    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_console_run(temp_dir, "console_wifi_scan", "/wifi scan", true, false, &result) == 0,
        "console wifi scan succeeded"
    );
    assert_string_contains(result.final_text, "LabNet", "console wifi scan includes secure network");
    assert_string_contains(result.final_text, "OpenField", "console wifi scan includes open network");

    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_console_run(temp_dir, "console_wifi_join", "/wifi join \"LabNet\" \"secret pass\"", true, false, &result) == 0,
        "console wifi join succeeded"
    );
    assert_string_contains(result.final_text, "connecting to LabNet", "console wifi join response returned");

    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_console_run(temp_dir, "console_tg_status", "/telegram status", true, false, &result) == 0,
        "console telegram status succeeded"
    );
    assert_string_contains(result.final_text, "Telegram: enabled", "console telegram status shows enabled");
    assert_string_contains(result.final_text, "Token: unset", "console telegram status shows unset token");

    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_console_run(temp_dir, "console_tg_token", "/telegram token 123456:ABCDEF", true, false, &result) == 0,
        "console telegram token succeeded"
    );
    assert_string_contains(result.final_text, "Telegram config saved", "console telegram token response returned");
    assert_true(strcmp(s_console_telegram_config.bot_token, "123456:ABCDEF") == 0, "console telegram token stored");

    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_console_run(temp_dir, "console_tg_poll", "/telegram poll 11", true, false, &result) == 0,
        "console telegram poll succeeded"
    );
    assert_true(s_console_telegram_config.poll_interval_seconds == 11U, "console telegram poll interval updated");

    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_console_run(temp_dir, "console_tool_fs", "/tool fs.read {\"path\":\"AGENTS.md\"}", true, false, &result) == 0,
        "console tool fs.read succeeded"
    );
    assert_string_contains(result.final_text, "Agent Instructions", "console tool fs.read returned file contents");

    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_console_run(temp_dir, "console_tool_search", "/tool web.search {\"query\":\"ms5611 datasheet\"}", true, false, &result) == 0,
        "console tool web.search succeeded"
    );
    assert_string_contains(result.final_text, "MS5611 datasheet", "console tool web search returned condensed result");

    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_console_run(temp_dir, "console_tool_fetch", "/tool web.fetch {\"url\":\"https://example.com/doc.pdf\"}", true, false, &result) == 0,
        "console tool web.fetch succeeded"
    );
    assert_true(
        espclaw_admin_json_string_value(result.final_text, "stored_path", stored_path, sizeof(stored_path)),
        "web fetch returned stored path"
    );
    assert_true(espclaw_workspace_read_file(temp_dir, stored_path, buffer, sizeof(buffer)) == 0, "web fetch stored markdown readable");
    assert_string_contains(buffer, "PROM read sequence", "web fetch stored markdown content");

    read_text_file(
        ESPCLAW_SOURCE_DIR "/firmware/components/espclaw_core/src/web_tools.c",
        transcript,
        sizeof(transcript)
    );
    assert_string_contains(transcript, "esp_http_client_open(client, 0)", "web tools uses explicit http open");
    assert_string_contains(transcript, "esp_http_client_fetch_headers(client)", "web tools fetches headers before reads");
    assert_string_contains(transcript, "esp_http_client_close(client)", "web tools closes explicit http session");
    assert_string_contains(transcript, "config.crt_bundle_attach = esp_crt_bundle_attach", "web tools attaches cert bundle");

    read_text_file(
        ESPCLAW_SOURCE_DIR "/firmware/components/espclaw_core/src/runtime.c",
        transcript,
        sizeof(transcript)
    );
    assert_string_contains(transcript, "esp_log_level_set(\"wifi\", ESP_LOG_WARN)", "uart console suppresses noisy wifi info logs");
    assert_string_contains(transcript, "esp_log_level_set(\"esp_netif_handlers\", ESP_LOG_WARN)", "uart console suppresses noisy netif info logs");
    assert_string_contains(transcript, "uart_console_write_raw(\"\\r\\n\")", "uart console emits newline before running commands");
    assert_string_contains(transcript, "Telegram polling idle: empty bot token", "runtime reports idle telegram state");
    assert_string_contains(transcript, "espclaw_runtime_set_telegram_config", "runtime exposes telegram setter");
    assert_string_contains(transcript, "#define ESPCLAW_TELEGRAM_STACK_BYTES 16384", "telegram task stack budget is increased");
    assert_string_contains(transcript, "response = malloc(ESPCLAW_TELEGRAM_RESPONSE_BYTES);", "telegram response buffer moves off stack");
    assert_string_contains(transcript, "run_result = calloc(1, sizeof(*run_result));", "telegram agent result moves off stack");
    assert_string_contains(transcript, "\"espclaw_tg\",\n            ESPCLAW_TELEGRAM_STACK_BYTES,", "telegram task uses named stack budget");
    assert_string_contains(transcript, "config.crt_bundle_attach = esp_crt_bundle_attach;", "telegram send message attaches cert bundle");
    assert_string_contains(transcript, "client_config.crt_bundle_attach = esp_crt_bundle_attach;", "telegram get updates attaches cert bundle");
    assert_string_contains(transcript, "telegram_send_chat_action(config.bot_token, update.chat_id, \"typing\")", "telegram sends typing indicator before processing");
    assert_string_contains(transcript, "https://api.telegram.org/bot%s/sendPhoto", "telegram photo upload endpoint is present");
    assert_string_contains(transcript, "multipart/form-data; boundary=", "telegram photo upload uses multipart form data");
    assert_string_contains(transcript, "strcmp(update.text, \"/camera\") == 0 || strcmp(update.text, \"/photo\") == 0", "telegram supports direct camera commands");
    assert_string_contains(transcript, "telegram_send_chat_action(config.bot_token, update.chat_id, \"upload_photo\")", "telegram sends upload-photo indicator");
    assert_string_contains(transcript, "esp_http_client_open(client, 0) == ESP_OK", "telegram polling uses explicit http open");
    assert_string_contains(transcript, "esp_http_client_fetch_headers(client) >= 0", "telegram polling fetches headers before reads");

    read_text_file(
        ESPCLAW_SOURCE_DIR "/firmware/components/espclaw_core/src/admin_ui.c",
        transcript,
        sizeof(transcript)
    );
    assert_string_contains(transcript, "/api/telegram/config", "admin ui loads telegram config endpoint");
    assert_string_contains(transcript, "saveTelegramConfig", "admin ui exposes telegram save action");

    read_text_file(
        ESPCLAW_SOURCE_DIR "/firmware/components/espclaw_core/src/admin_server.c",
        transcript,
        sizeof(transcript)
    );
    assert_string_contains(transcript, "telegram_config_get_handler", "admin server exposes telegram config get handler");
    assert_string_contains(transcript, "telegram_config_post_handler", "admin server exposes telegram config post handler");

    read_text_file(
        ESPCLAW_SOURCE_DIR "/firmware/components/espclaw_core/src/agent_loop.c",
        transcript,
        sizeof(transcript)
    );
    assert_string_contains(transcript, "ESP_LOGD(", "agent loop uses debug logging");
    assert_string_contains(transcript, "agent alloc preflight free=%u largest=%u req=%u resp=%u hist=%u items=%u instr=%u", "agent alloc preflight message retained");
    assert_string_contains(transcript, "ESP_LOGD(TAG, \"raw Codex TLS handshake complete", "raw codex handshake log moved to debug");
    assert_string_contains(transcript, "ESP_LOGD(TAG, \"raw Codex headers parsed status=", "raw codex header log moved to debug");
    assert_string_contains(transcript, "log_tool_call_summary(&provider_response->tool_calls[tool_index], \"model\")", "model tool calls log summary");
    assert_string_contains(transcript, "log_tool_call_summary(&tool_call, \"manual\")", "manual tool calls log summary");

    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_console_run(temp_dir, "console_reboot", "/reboot", true, false, &result) == 0,
        "console reboot command succeeded"
    );
    assert_true(s_console_reboot_requested, "console reboot adapter invoked");

    memset(&result, 0, sizeof(result));
    assert_true(
        espclaw_console_run(temp_dir, "console_factory", "/factory-reset", true, false, &result) == 0,
        "console factory reset command succeeded"
    );
    assert_true(s_console_factory_reset_requested, "console factory reset adapter invoked");

    assert_true(
        espclaw_session_read_transcript(temp_dir, "console_tool_fetch", transcript, sizeof(transcript)) == 0,
        "console transcript readable"
    );
    assert_string_contains(transcript, "/tool web.fetch", "console transcript stored user slash command");
    assert_string_contains(transcript, "stored_path", "console transcript stored command response");

    espclaw_web_set_http_adapter(NULL, NULL);
    espclaw_console_set_runtime_adapter(NULL);
}

int main(void)
{
    test_board_profiles();
    test_lua_api_registry();
    test_board_descriptor_and_task_policy();
    test_workspace_manifest();
    test_provider_and_channel_registry();
    test_tool_catalog();
    test_default_config_render();
    test_storage_backend_description();
    test_storage_esp32cam_sdmmc_wiring_policy();
    test_ota_manager_host_state_and_partition_layout();
    test_esp32_sdkconfig_defaults_enable_psram_tls();
    test_camera_uses_reserved_ledc_channel();
    test_task_runtime_uses_portmux_locking();
    test_runtime_skips_boot_automation_after_crash_reset();
    test_behavior_runtime_caches_specs_for_embedded_autostart();
    test_app_runtime_caches_manifests_for_embedded_control_paths();
    test_behavior_register_avoids_sd_manifest_validation();
    test_session_append_avoids_workspace_bootstrap_on_embedded_console_paths();
    test_console_chat_skips_embedded_transcript_writes();
    test_telegram_agent_turns_use_stateless_loop();
    test_runtime_uses_larger_uart_console_stack();
    test_embedded_console_and_agent_paths_heap_allocate_auth_profiles();
    test_uart_console_normalizes_newlines_for_serial_terminals();
    test_system_monitor_snapshot_and_json();
    test_camera_status_and_json();
    test_runtime_wifi_boot_deferral_policy();
    test_provisioning_descriptor();
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
    test_app_runtime_event_handlers();
    test_app_runtime_module_entrypoints();
    test_app_runtime_vehicle_bindings();
    test_app_vm_and_control_loops();
    test_task_runtime();
    test_event_watch_runtime();
    test_behavior_runtime();
    test_lua_module_require_paths();
    test_app_runtime_fs_alias();
    test_admin_scaffold_app_custom_triggers();
    test_transport_error_formatting();
    test_sse_completed_extraction_in_place();
    test_http_chunked_body_extraction();
    test_incremental_sse_stream_reduction();
    test_incremental_sse_stream_fallback_text();
    test_incremental_sse_stream_handles_long_completed_line();
    test_incremental_sse_stream_uses_output_text_done_when_completed_lacks_text();
    test_incremental_sse_stream_uses_output_text_delta_when_completed_lacks_text();
    test_terminal_response_storage_handles_overlap();
    test_esp32cam_storage_attempts();
    test_board_boot_defaults_force_flash_led_low();
    test_agent_loop();
    test_console_chat_and_web_tools();

    printf("espclaw core tests passed\n");
    return 0;
}
