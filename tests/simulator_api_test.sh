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

"$SIM_BIN" --workspace "$WORKSPACE" --port "$PORT" --profile esp32s3 >"$SIM_LOG" 2>&1 &
SIM_PID="$!"

for _ in {1..50}; do
  if curl -sf "http://127.0.0.1:$PORT/api/status" >/tmp/espclaw-sim-status.json; then
    break
  fi
  sleep 0.1
done

STATUS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/status")"
[[ "$STATUS_JSON" == *"telegram"* ]]

WORKSPACE_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/workspace/files")"
[[ "$WORKSPACE_JSON" == *"HEARTBEAT.md"* ]]

TOOLS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/tools")"
[[ "$TOOLS_JSON" == *'"name":"tool.list"'* ]]

AUTH_JSON="$(curl -sf -X PUT "http://127.0.0.1:$PORT/api/auth/codex" \
  -H 'Content-Type: application/json' \
  --data '{"provider_id":"openai_codex","model":"gpt-5.3-codex","base_url":"mock://tool-loop","account_id":"acc_sim","access_token":"token_sim","refresh_token":"refresh_sim","source":"sim_test"}')"
[[ "$AUTH_JSON" == *'"configured":true'* ]]
[[ "$AUTH_JSON" == *'"provider_id":"openai_codex"'* ]]

AUTH_STATUS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/auth/status")"
[[ "$AUTH_STATUS_JSON" == *'"configured":true'* ]]
[[ "$AUTH_STATUS_JSON" == *'"account_id":"acc_sim"'* ]]

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

APPS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/apps")"
[[ "$APPS_JSON" == *'"id":"loop_app"'* ]]

curl -sf -X DELETE "http://127.0.0.1:$PORT/api/apps?app_id=loop_app" >/tmp/espclaw-sim-delete.json
APPS_JSON="$(curl -sf "http://127.0.0.1:$PORT/api/apps")"
[[ "$APPS_JSON" != *'"id":"loop_app"'* ]]

CLEAR_AUTH_JSON="$(curl -sf -X DELETE "http://127.0.0.1:$PORT/api/auth/codex")"
[[ "$CLEAR_AUTH_JSON" == *'"ok":true'* ]]
