#!/bin/zsh
set -euo pipefail
unsetopt BG_NICE

SIM_BIN="$1"
PORT="${2:-18081}"
if [[ "$PORT" == "0" ]]; then
  PORT=$(( 20000 + ( RANDOM % 20000 ) ))
fi
WORKSPACE="$(mktemp -d /tmp/espclaw-sim-api.XXXXXX)"
SIM_LOG="$WORKSPACE/simulator.log"
SIM_PID=""
HTTP_PID=""
HTTP_PORT=$(( PORT + 1 ))

cleanup() {
  if [[ -n "$HTTP_PID" ]]; then
    kill "$HTTP_PID" >/dev/null 2>&1 || true
    wait "$HTTP_PID" >/dev/null 2>&1 || true
  fi
  if [[ -n "$SIM_PID" ]]; then
    kill "$SIM_PID" >/dev/null 2>&1 || true
    sleep 0.1
    if kill -0 "$SIM_PID" >/dev/null 2>&1; then
      kill -9 "$SIM_PID" >/dev/null 2>&1 || true
    fi
    wait "$SIM_PID" >/dev/null 2>&1 || true
  fi
  rm -rf "$WORKSPACE"
}

trap cleanup EXIT

"$SIM_BIN" --workspace "$WORKSPACE" --port "$PORT" --profile esp32cam >"$SIM_LOG" 2>&1 &
SIM_PID="$!"

for _ in {1..50}; do
  if curl -sf "http://127.0.0.1:$PORT/api/status" >/tmp/espclaw-sim-status.json; then
    break
  fi
  sleep 0.1
done

STATUS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/status")"
[[ "$STATUS_JSON" == *"telegram"* ]]
[[ "$STATUS_JSON" == *'"board_profile":"esp32cam"'* ]]
[[ "$STATUS_JSON" == *'"storage_backend":"sdcard"'* ]]

BOARD_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/board")"
[[ "$BOARD_JSON" == *'"configured":true'* ]]
[[ "$BOARD_JSON" == *'"variant":"ai_thinker_esp32cam"'* ]]
[[ "$BOARD_JSON" == *'"cpu_cores":2'* ]]

BOARD_PRESETS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/board/presets")"
[[ "$BOARD_PRESETS_JSON" == *'"variant":"ai_thinker_esp32cam"'* ]]

BOARD_CONFIG_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/board/config")"
[[ "$BOARD_CONFIG_JSON" == *'"source":"workspace"'* ]]
[[ "$BOARD_CONFIG_JSON" == *'"raw_json":"{'* ]]

APPLY_BOARD_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/board/apply?variant_id=ai_thinker_esp32cam")"
[[ "$APPLY_BOARD_JSON" == *'"variant":"ai_thinker_esp32cam"'* ]]
[[ "$APPLY_BOARD_JSON" == *'"source":"workspace"'* ]]

BOARD_CONFIG_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/board/config")"
[[ "$BOARD_CONFIG_JSON" == *'ai_thinker_esp32cam'* ]]

SAVE_BOARD_JSON="$(curl -sf -X PUT "http://127.0.0.1:$PORT/api/board/config" \
  -H 'Content-Type: application/json' \
  --data '{\n  "variant": "ai_thinker_esp32cam",\n  "pins": {"buzzer": 9}\n}')"
[[ "$SAVE_BOARD_JSON" == *'"source":"workspace"'* ]]

BOARD_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/board")"
[[ "$BOARD_JSON" == *'"variant":"ai_thinker_esp32cam"'* ]]
[[ "$BOARD_JSON" == *'"name":"buzzer","pin":9'* ]]

NETWORK_STATUS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/network/status")"
[[ "$NETWORK_STATUS_JSON" == *'"wifi_ready":'* ]]
[[ "$NETWORK_STATUS_JSON" == *'"provisioning_active":'* ]]
[[ "$NETWORK_STATUS_JSON" == *'"ssid":'* ]]

NETWORK_PROVISIONING_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/network/provisioning")"
[[ "$NETWORK_PROVISIONING_JSON" == *'"active":'* ]]
[[ "$NETWORK_PROVISIONING_JSON" == *'"transport":'* ]]
[[ "$NETWORK_PROVISIONING_JSON" == *'"admin_url":'* ]]

NETWORK_SCAN_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/network/scan")"
[[ "$NETWORK_SCAN_JSON" == *'"ssid":"ESPClawLab"'* ]]
[[ "$NETWORK_SCAN_JSON" == *'"ssid":"Attila-5G"'* ]]

NETWORK_JOIN_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/network/join" \
  -H 'Content-Type: application/json' \
  --data '{"ssid":"Attila-5G","password":"secret123"}')"
[[ "$NETWORK_JOIN_JSON" == *'"wifi_ready":true'* ]]
[[ "$NETWORK_JOIN_JSON" == *'"ssid":"Attila-5G"'* ]]
[[ "$NETWORK_JOIN_JSON" == *'"onboarding_ssid":""'* ]]
[[ "$NETWORK_JOIN_JSON" == *'"message":"simulator connected"'* ]]

WORKSPACE_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/workspace/files")"
[[ "$WORKSPACE_JSON" == *"HEARTBEAT.md"* ]]
[[ "$WORKSPACE_JSON" == *"config/board.json"* ]]

printf '## Part 1\nhello\n## Part 2\nworld\n' >"$WORKSPACE/memory/context.md"

MONITOR_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/monitor")"
[[ "$MONITOR_JSON" == *'"available":true'* ]]
[[ "$MONITOR_JSON" == *'"cpu_cores":2'* ]]
[[ "$MONITOR_JSON" == *'"dual_core":true'* ]]
[[ "$MONITOR_JSON" == *'"workspace_total_bytes":'* ]]

TOOLS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/tools")"
grep -q '"name":"tool.list"' <<<"$TOOLS_JSON"
grep -q '"name":"behavior.register"' <<<"$TOOLS_JSON"
grep -q '"name":"component.install"' <<<"$TOOLS_JSON"
grep -q '"name":"component.install_from_file"' <<<"$TOOLS_JSON"
grep -q '"name":"component.install_from_blob"' <<<"$TOOLS_JSON"
grep -q '"name":"component.install_from_url"' <<<"$TOOLS_JSON"
grep -q '"name":"component.install_from_manifest"' <<<"$TOOLS_JSON"
grep -q '"name":"app.install_from_file"' <<<"$TOOLS_JSON"
grep -q '"name":"app.install_from_blob"' <<<"$TOOLS_JSON"
grep -q '"name":"app.install_from_url"' <<<"$TOOLS_JSON"
grep -q '"name":"context.chunks"' <<<"$TOOLS_JSON"
grep -q '"name":"context.load"' <<<"$TOOLS_JSON"
grep -q '"name":"context.search"' <<<"$TOOLS_JSON"
grep -q '"name":"context.select"' <<<"$TOOLS_JSON"
grep -q '"name":"context.summarize"' <<<"$TOOLS_JSON"
grep -q '"name":"app_patterns.list"' <<<"$TOOLS_JSON"

APP_PATTERNS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/app-patterns")"
[[ "$APP_PATTERNS_JSON" == *'"name":"shared_component_plus_apps"'* ]]

APP_PATTERNS_MD="$(curl -sf "http://127.0.0.1:$PORT/api/app-patterns.md")"
[[ "$APP_PATTERNS_MD" == *'# App Patterns'* ]]

COMPONENT_INSTALL_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/components/install" \
  -H 'Content-Type: application/json' \
  --data '{"component_id":"ms5611_driver","title":"MS5611 Driver","module":"sensors.ms5611","summary":"Shared MS5611 sensor driver","version":"0.1.0","source":"local M = {}; function M.sample() return {pressure_mbar=1007.2,temp_c=21.5} end; return M"}')"
[[ "$COMPONENT_INSTALL_JSON" == *'"ok":true'* ]]

COMPONENTS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/components")"
[[ "$COMPONENTS_JSON" == *'"id":"ms5611_driver"'* ]]
[[ "$COMPONENTS_JSON" == *'"module":"sensors.ms5611"'* ]]

COMPONENT_DETAIL_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/components/detail?component_id=ms5611_driver")"
[[ "$COMPONENT_DETAIL_JSON" == *'"id":"ms5611_driver"'* ]]
[[ "$COMPONENT_DETAIL_JSON" == *'pressure_mbar=1007.2'* ]]

APP_FROM_FILE_SOURCE=$'function handle(trigger, payload)\n  local sensor = require("sensors.ms5611")\n  local sample = sensor.sample()\n  return string.format("pressure=%.1f", sample.pressure_mbar)\nend\n'
curl -sf -X POST "http://127.0.0.1:$PORT/api/blobs/begin?blob_id=weather_app&target_path=memory/weather_app.lua&content_type=text%2Fx-lua" >/tmp/espclaw-sim-weather-blob-begin.json
curl -sf -X POST "http://127.0.0.1:$PORT/api/blobs/append?blob_id=weather_app" --data-binary "$APP_FROM_FILE_SOURCE" >/tmp/espclaw-sim-weather-blob-append.json
curl -sf -X POST "http://127.0.0.1:$PORT/api/blobs/commit?blob_id=weather_app" >/tmp/espclaw-sim-weather-blob-commit.json

APP_INSTALL_FROM_FILE_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/apps/install/from-file?app_id=weather_station&title=Weather%20Station&permissions=fs.read&triggers=manual&source_path=memory/weather_app.lua")"
[[ "$APP_INSTALL_FROM_FILE_JSON" == *'"ok":true'* ]]

RUN_FROM_FILE_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/apps/run?app_id=weather_station&trigger=manual" --data '')"
[[ "$RUN_FROM_FILE_JSON" == *'"ok":true'* ]]
[[ "$RUN_FROM_FILE_JSON" == *'pressure=1007.2'* ]]

COMPONENT_BLOB_SOURCE=$'local M = {}\nfunction M.sample() return {pressure_mbar=997.1,temp_c=16.0} end\nreturn M\n'
APP_BLOB_SOURCE=$'function handle(trigger, payload)\n  local sensor = require("sensors.ms5611_blob")\n  local sample = sensor.sample()\n  return string.format("pressure=%.1f", sample.pressure_mbar)\nend\n'
curl -sf -X POST "http://127.0.0.1:$PORT/api/blobs/begin?blob_id=ms_blob&target_path=memory/ms_blob.lua&content_type=text%2Fx-lua" >/tmp/espclaw-sim-ms-blob-begin.json
curl -sf -X POST "http://127.0.0.1:$PORT/api/blobs/append?blob_id=ms_blob" --data-binary "$COMPONENT_BLOB_SOURCE" >/tmp/espclaw-sim-ms-blob-append.json
curl -sf -X POST "http://127.0.0.1:$PORT/api/blobs/commit?blob_id=ms_blob" >/tmp/espclaw-sim-ms-blob-commit.json

COMPONENT_INSTALL_FROM_BLOB_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/components/install/from-blob?component_id=ms5611_blob_driver&title=MS5611%20Blob%20Driver&module=sensors.ms5611_blob&summary=Shared%20MS5611%20driver%20from%20blob&version=0.1.0&blob_id=ms_blob")"
[[ "$COMPONENT_INSTALL_FROM_BLOB_JSON" == *'"ok":true'* ]]

curl -sf -X POST "http://127.0.0.1:$PORT/api/blobs/begin?blob_id=weather_blob&target_path=memory/weather_blob.lua&content_type=text%2Fx-lua" >/tmp/espclaw-sim-weather-blob2-begin.json
curl -sf -X POST "http://127.0.0.1:$PORT/api/blobs/append?blob_id=weather_blob" --data-binary "$APP_BLOB_SOURCE" >/tmp/espclaw-sim-weather-blob2-append.json
curl -sf -X POST "http://127.0.0.1:$PORT/api/blobs/commit?blob_id=weather_blob" >/tmp/espclaw-sim-weather-blob2-commit.json

APP_INSTALL_FROM_BLOB_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/apps/install/from-blob?app_id=weather_station_blob&title=Weather%20Station%20Blob&permissions=fs.read&triggers=manual&blob_id=weather_blob")"
[[ "$APP_INSTALL_FROM_BLOB_JSON" == *'"ok":true'* ]]

RUN_FROM_BLOB_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/apps/run?app_id=weather_station_blob&trigger=manual" --data '')"
[[ "$RUN_FROM_BLOB_JSON" == *'"ok":true'* ]]
[[ "$RUN_FROM_BLOB_JSON" == *'pressure=997.1'* ]]

CONTEXT_CHUNKS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/context/chunks?path=memory/context.md&chunk_bytes=8")"
[[ "$CONTEXT_CHUNKS_JSON" == *'"chunk_count":4'* ]]

CONTEXT_LOAD_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/context/load?path=memory/context.md&chunk_index=1&chunk_bytes=8")"
[[ "$CONTEXT_LOAD_JSON" == *'"chunk_index":1'* ]]
[[ "$CONTEXT_LOAD_JSON" == *'hello'* ]]

CONTEXT_SEARCH_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/context/search?path=memory/context.md&query=world&chunk_bytes=8&limit=2")"
[[ "$CONTEXT_SEARCH_JSON" == *'"results":['* ]]
[[ "$CONTEXT_SEARCH_JSON" == *'world'* ]]

CONTEXT_SELECT_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/context/select?path=memory/context.md&query=Part&chunk_bytes=8&limit=2&output_bytes=64")"
[[ "$CONTEXT_SELECT_JSON" == *'"selected_text":'* ]]
[[ "$CONTEXT_SELECT_JSON" == *'chunk 0'* ]]

CONTEXT_SUMMARIZE_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/context/summarize?path=memory/context.md&query=hello&chunk_bytes=8&limit=2&summary_bytes=96")"
[[ "$CONTEXT_SUMMARIZE_JSON" == *'"summary":'* ]]
[[ "$CONTEXT_SUMMARIZE_JSON" == *'hello'* ]]

HTTP_ROOT="$WORKSPACE/http-src"
mkdir -p "$HTTP_ROOT"
printf '%s' $'local M = {}\nfunction M.sample() return {pressure_mbar=998.4,temp_c=18.0} end\nreturn M\n' >"$HTTP_ROOT/ms5611_url.lua"
printf '%s' $'function handle(trigger, payload)\n  local sensor = require("sensors.ms5611_url")\n  local sample = sensor.sample()\n  return string.format("pressure=%.1f", sample.pressure_mbar)\nend\n' >"$HTTP_ROOT/weather_url.lua"
cat >"$HTTP_ROOT/ms5611_manifest.json" <<EOF
{
  "id": "ms5611_manifest_driver",
  "title": "MS5611 Manifest Driver",
  "module": "sensors.ms5611_manifest",
  "summary": "Shared MS5611 driver from manifest",
  "version": "0.2.0",
  "source_url": "http://127.0.0.1:$HTTP_PORT/ms5611_url.lua",
  "docs_url": "http://127.0.0.1:$HTTP_PORT/ms5611_docs.md",
  "dependencies": []
}
EOF
printf '%s\n' '# MS5611 docs' >"$HTTP_ROOT/ms5611_docs.md"
python3 -m http.server "$HTTP_PORT" --bind 127.0.0.1 --directory "$HTTP_ROOT" >"$WORKSPACE/http-server.log" 2>&1 &
HTTP_PID="$!"
for _ in {1..50}; do
  if curl -sf "http://127.0.0.1:$HTTP_PORT/ms5611_url.lua" >/tmp/espclaw-sim-http-check.txt; then
    break
  fi
  sleep 0.1
done

COMPONENT_INSTALL_FROM_URL_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/components/install/from-url?component_id=ms5611_url_driver&title=MS5611%20URL%20Driver&module=sensors.ms5611_url&summary=Shared%20MS5611%20driver%20from%20URL&version=0.1.0&source_url=http%3A%2F%2F127.0.0.1%3A$HTTP_PORT%2Fms5611_url.lua")"
[[ "$COMPONENT_INSTALL_FROM_URL_JSON" == *'"ok":true'* ]]

COMPONENT_INSTALL_FROM_MANIFEST_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/components/install/from-manifest?manifest_url=http%3A%2F%2F127.0.0.1%3A$HTTP_PORT%2Fms5611_manifest.json")"
[[ "$COMPONENT_INSTALL_FROM_MANIFEST_JSON" == *'"ok":true'* ]]

COMPONENT_MANIFEST_DETAIL_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/components/detail?component_id=ms5611_manifest_driver")"
[[ "$COMPONENT_MANIFEST_DETAIL_JSON" == *'"manifest_url":"http://127.0.0.1:'* ]]
[[ "$COMPONENT_MANIFEST_DETAIL_JSON" == *'"source_url":"http://127.0.0.1:'* ]]
[[ "$COMPONENT_MANIFEST_DETAIL_JSON" == *'"docs_url":"http://127.0.0.1:'* ]]

APP_INSTALL_FROM_URL_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/apps/install/from-url?app_id=weather_station_url&title=Weather%20Station%20URL&permissions=fs.read&triggers=manual&source_url=http%3A%2F%2F127.0.0.1%3A$HTTP_PORT%2Fweather_url.lua")"
[[ "$APP_INSTALL_FROM_URL_JSON" == *'"ok":true'* ]]

RUN_FROM_URL_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/apps/run?app_id=weather_station_url&trigger=manual" --data '')"
[[ "$RUN_FROM_URL_JSON" == *'"ok":true'* ]]
[[ "$RUN_FROM_URL_JSON" == *'pressure=998.4'* ]]

HARDWARE_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/hardware")"
[[ "$HARDWARE_JSON" == *'"configured":true'* ]]
[[ "$HARDWARE_JSON" == *'"capabilities":['* ]]

AUTH_JSON="$(curl -sf -X PUT "http://127.0.0.1:$PORT/api/auth/codex" \
  -H 'Content-Type: application/json' \
  --data '{"provider_id":"openai_codex","model":"gpt-5.3-codex","base_url":"mock://tool-loop","account_id":"acc_sim","access_token":"token_sim","refresh_token":"refresh_sim","source":"sim_test"}')"
[[ "$AUTH_JSON" == *'"configured":true'* ]]
[[ "$AUTH_JSON" == *'"provider_id":"openai_codex"'* ]]

AUTH_STATUS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/auth/status")"
[[ "$AUTH_STATUS_JSON" == *'"configured":true'* ]]
[[ "$AUTH_STATUS_JSON" == *'"account_id":"acc_sim"'* ]]

AUTH_IMPORT_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/auth/import-json" \
  -H 'Content-Type: application/json' \
  --data '{"provider_id":"openai_codex","model":"gpt-5.3-codex","base_url":"mock://tool-loop","access_token":"imported_token","refresh_token":"imported_refresh","account_id":"acc_uploaded"}')"
[[ "$AUTH_IMPORT_JSON" == *'"configured":true'* ]]
[[ "$AUTH_IMPORT_JSON" == *'"account_id":"acc_uploaded"'* ]]

AUTH_STATUS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/auth/status")"
[[ "$AUTH_STATUS_JSON" == *'"configured":true'* ]]
[[ "$AUTH_STATUS_JSON" == *'"source":"imported_json"'* ]]
[[ "$AUTH_STATUS_JSON" == *'"account_id":"acc_uploaded"'* ]]

STATUS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/status")"
[[ "$STATUS_JSON" == *'"provider":"openai_codex"'* ]]

curl -sf -X POST "http://127.0.0.1:$PORT/api/apps/scaffold?app_id=loop_app" >/tmp/espclaw-sim-scaffold.json

APP_SOURCE=$'counter = counter or 0\nfunction handle(trigger, payload)\n  counter = counter + 1\n  return string.format("step=%d payload=%s", counter, payload)\nend\n'
curl -sf -X PUT "http://127.0.0.1:$PORT/api/apps/source?app_id=loop_app" --data-binary "$APP_SOURCE" >/tmp/espclaw-sim-source.json

DETAIL_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/apps/detail?app_id=loop_app")"
[[ "$DETAIL_JSON" == *'"id":"loop_app"'* ]]
[[ "$DETAIL_JSON" == *'counter = counter or 0'* ]]

RUN_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/apps/run?app_id=loop_app&trigger=manual" --data 'alpha')"
[[ "$RUN_JSON" == *'"ok":true'* ]]
[[ "$RUN_JSON" == *'step=1 payload=alpha'* ]]

CHAT_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/chat/run?session_id=sim_chat" --data 'Inspect the device and list the installed apps.')"
[[ "$CHAT_JSON" == *'"ok":true'* ]]
[[ "$CHAT_JSON" == *'"used_tools":true'* ]]
[[ "$CHAT_JSON" == *'listed the installed apps'* ]]

CHAT_SESSION_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/chat/session?session_id=sim_chat")"
[[ "$CHAT_SESSION_JSON" == *'"session_id":"sim_chat"'* ]]
[[ "$CHAT_SESSION_JSON" == *'Requested tools: system.info, app.list'* ]]

TOOLS_CHAT_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/chat/run?session_id=sim_tools" --data 'List out all the available tools to you')"
[[ "$TOOLS_CHAT_JSON" == *'"ok":true'* ]]
[[ "$TOOLS_CHAT_JSON" == *'"used_tools":true'* ]]
[[ "$TOOLS_CHAT_JSON" == *'inspect files, apps, hardware, networking, and OTA state'* ]]

TOOLS_CHAT_SESSION_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/chat/session?session_id=sim_tools")"
[[ "$TOOLS_CHAT_SESSION_JSON" == *'Requested tools: tool.list'* ]]
[[ "$TOOLS_CHAT_SESSION_JSON" == *'fs.read'* ]]

HELP_CHAT_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/chat/run?session_id=sim_help" --data '/help')"
[[ "$HELP_CHAT_JSON" == *'"ok":true'* ]]
[[ "$HELP_CHAT_JSON" == *'/wifi join <ssid> [password]'* ]]

TOOL_CHAT_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/chat/run?session_id=sim_tool_cmd" --data '/tool system.info {}')"
[[ "$TOOL_CHAT_JSON" == *'"ok":true'* ]]
[[ "$TOOL_CHAT_JSON" == *'workspace_ready'* ]]

LOOP_START_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/loops/start?loop_id=demo_loop&app_id=loop_app&trigger=manual&period_ms=10&iterations=3" --data 'tick')"
[[ "$LOOP_START_JSON" == *'"loop_id":"demo_loop"'* ]]

LOOPS_JSON=""
for _ in {1..60}; do
  LOOPS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/loops")"
  if [[ "$LOOPS_JSON" == *'"loop_id":"demo_loop"'* && "$LOOPS_JSON" == *'"completed":true'* ]]; then
    break
  fi
  sleep 0.05
done
[[ "$LOOPS_JSON" == *'"loop_id":"demo_loop"'* ]]
[[ "$LOOPS_JSON" == *'"iterations_completed":3'* ]]
[[ "$LOOPS_JSON" == *'step=3 payload=tick'* ]]

curl -sf -X POST "http://127.0.0.1:$PORT/api/loops/start?loop_id=stop_loop&app_id=loop_app&trigger=manual&period_ms=20&iterations=0" --data 'spin' >/tmp/espclaw-sim-loop-start.json
sleep 0.1
STOP_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/loops/stop?loop_id=stop_loop")"
[[ "$STOP_JSON" == *'"ok":true'* ]]

for _ in {1..60}; do
  LOOPS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/loops")"
  if [[ "$LOOPS_JSON" == *'"loop_id":"stop_loop"'* && "$LOOPS_JSON" == *'"stop_requested":true'* && "$LOOPS_JSON" == *'"completed":true'* ]]; then
    break
  fi
  sleep 0.05
done
[[ "$LOOPS_JSON" == *'"loop_id":"stop_loop"'* ]]
[[ "$LOOPS_JSON" == *'"stop_requested":true'* ]]

TASK_START_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/tasks/start?task_id=telemetry_task&app_id=loop_app&trigger=manual&period_ms=10&iterations=2" --data 'pulse')"
[[ "$TASK_START_JSON" == *'"task_id":"telemetry_task"'* ]]

TASKS_JSON=""
for _ in {1..60}; do
  TASKS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/tasks")"
  if [[ "$TASKS_JSON" == *'"task_id":"telemetry_task"'* && "$TASKS_JSON" == *'"completed":true'* ]]; then
    break
  fi
  sleep 0.05
done
[[ "$TASKS_JSON" == *'"task_id":"telemetry_task"'* ]]
[[ "$TASKS_JSON" == *'"schedule":"periodic"'* ]]
[[ "$TASKS_JSON" == *'"iterations_completed":2'* ]]

BEHAVIOR_REGISTER_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/behaviors/register?behavior_id=avoidance&app_id=loop_app&schedule=periodic&trigger=manual&period_ms=10&iterations=2&autostart=1" --data 'avoid')"
[[ "$BEHAVIOR_REGISTER_JSON" == *'"behavior_id":"avoidance"'* ]]
[[ "$BEHAVIOR_REGISTER_JSON" == *'"autostart":true'* ]]

BEHAVIORS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/behaviors")"
[[ "$BEHAVIORS_JSON" == *'"behavior_id":"avoidance"'* ]]
[[ "$BEHAVIORS_JSON" == *'"app_id":"loop_app"'* ]]

BEHAVIOR_START_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/behaviors/start?behavior_id=avoidance")"
[[ "$BEHAVIOR_START_JSON" == *'"behavior_id":"avoidance"'* ]]

for _ in {1..60}; do
  BEHAVIORS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/behaviors")"
  if [[ "$BEHAVIORS_JSON" == *'"behavior_id":"avoidance"'* && "$BEHAVIORS_JSON" == *'"completed":true'* ]]; then
    break
  fi
  sleep 0.05
done
[[ "$BEHAVIORS_JSON" == *'"behavior_id":"avoidance"'* ]]
[[ "$BEHAVIORS_JSON" == *'"iterations_completed":2'* ]]

BEHAVIOR_REMOVE_JSON="$(curl -sf -X DELETE "http://127.0.0.1:$PORT/api/behaviors?behavior_id=avoidance")"
[[ "$BEHAVIOR_REMOVE_JSON" != *'"behavior_id":"avoidance"'* ]]

EVENT_TASK_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/tasks/start?task_id=sensor_task&app_id=loop_app&schedule=event&trigger=manual&iterations=2")"
[[ "$EVENT_TASK_JSON" == *'"task_id":"sensor_task"'* ]]

curl -sf -X POST "http://127.0.0.1:$PORT/api/events/emit?name=manual" --data 'near' >/tmp/espclaw-sim-event-1.json
curl -sf -X POST "http://127.0.0.1:$PORT/api/events/emit?name=manual" --data 'far' >/tmp/espclaw-sim-event-2.json

for _ in {1..60}; do
  TASKS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/tasks")"
  if [[ "$TASKS_JSON" == *'"task_id":"sensor_task"'* && "$TASKS_JSON" == *'"completed":true'* ]]; then
    break
  fi
  sleep 0.05
done
[[ "$TASKS_JSON" == *'"task_id":"sensor_task"'* ]]
[[ "$TASKS_JSON" == *'"schedule":"event"'* ]]
[[ "$TASKS_JSON" == *'"events_received":2'* ]]

curl -sf -X POST "http://127.0.0.1:$PORT/api/tasks/start?task_id=stop_task&app_id=loop_app&trigger=manual&period_ms=20&iterations=0" --data 'spin' >/tmp/espclaw-sim-task-start.json
sleep 0.1
TASK_STOP_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/tasks/stop?task_id=stop_task")"
[[ "$TASK_STOP_JSON" == *'"ok":true'* ]]

APPS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/apps")"
[[ "$APPS_JSON" == *'"id":"loop_app"'* ]]

curl -sf -X DELETE "http://127.0.0.1:$PORT/api/apps?app_id=loop_app" >/tmp/espclaw-sim-delete.json
APPS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/apps")"
[[ "$APPS_JSON" != *'"id":"loop_app"'* ]]

CLEAR_AUTH_JSON="$(curl -sf -X DELETE "http://127.0.0.1:$PORT/api/auth/codex")"
[[ "$CLEAR_AUTH_JSON" == *'"ok":true'* ]]
