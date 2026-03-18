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

cleanup() {
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
[[ "$BOARD_CONFIG_JSON" == *'auto'* ]]

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
[[ "$NETWORK_STATUS_JSON" == *'"wifi_ready":false'* ]]
[[ "$NETWORK_STATUS_JSON" == *'"provisioning_active":true'* ]]
[[ "$NETWORK_STATUS_JSON" == *'"provisioning_transport":"softap"'* ]]
[[ "$NETWORK_STATUS_JSON" == *'"onboarding_ssid":"ESPClaw-Sim"'* ]]
[[ "$NETWORK_STATUS_JSON" == *'"admin_url":"http://192.168.4.1/"'* ]]

NETWORK_PROVISIONING_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/network/provisioning")"
[[ "$NETWORK_PROVISIONING_JSON" == *'"active":true'* ]]
[[ "$NETWORK_PROVISIONING_JSON" == *'"transport":"softap"'* ]]
[[ "$NETWORK_PROVISIONING_JSON" == *'"service_name":"ESPClaw-Sim"'* ]]
[[ "$NETWORK_PROVISIONING_JSON" == *'"pop":""'* ]]
[[ "$NETWORK_PROVISIONING_JSON" == *'"admin_url":"http://192.168.4.1/"'* ]]

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

MONITOR_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/monitor")"
[[ "$MONITOR_JSON" == *'"available":true'* ]]
[[ "$MONITOR_JSON" == *'"cpu_cores":2'* ]]
[[ "$MONITOR_JSON" == *'"dual_core":true'* ]]
[[ "$MONITOR_JSON" == *'"workspace_total_bytes":'* ]]

TOOLS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/tools")"
[[ "$TOOLS_JSON" == *'"name":"tool.list"'* ]]
[[ "$TOOLS_JSON" == *'"name":"behavior.register"'* ]]
[[ "$TOOLS_JSON" == *'"name":"component.install"'* ]]
[[ "$TOOLS_JSON" == *'"name":"app_patterns.list"'* ]]

APP_PATTERNS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/app-patterns")"
[[ "$APP_PATTERNS_JSON" == *'"name":"shared_component_plus_apps"'* ]]

APP_PATTERNS_MD="$(curl -sf "http://127.0.0.1:$PORT/api/app-patterns.md")"
[[ "$APP_PATTERNS_MD" == *'# App Patterns'* ]]

COMPONENT_INSTALL_JSON="$(curl -sf -X POST "http://127.0.0.1:$PORT/api/components/install" \
  -H 'Content-Type: application/json' \
  --data '{"component_id":"ms5611_driver","title":"MS5611 Driver","module":"sensors.ms5611","summary":"Shared MS5611 sensor driver","version":"0.1.0","source":"local M = {}\\nfunction M.sample() return {pressure_mbar=1007.2,temp_c=21.5} end\\nreturn M\\n"}')"
[[ "$COMPONENT_INSTALL_JSON" == *'"ok":true'* ]]

COMPONENTS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/components")"
[[ "$COMPONENTS_JSON" == *'"id":"ms5611_driver"'* ]]
[[ "$COMPONENTS_JSON" == *'"module":"sensors.ms5611"'* ]]

COMPONENT_DETAIL_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/components/detail?component_id=ms5611_driver")"
[[ "$COMPONENT_DETAIL_JSON" == *'"id":"ms5611_driver"'* ]]
[[ "$COMPONENT_DETAIL_JSON" == *'pressure_mbar=1007.2'* ]]

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
