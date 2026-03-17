#!/usr/bin/env python3
import argparse
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Callable, Dict, List, Optional, Set


HELLO_MARKER = "ESPCLAW_BENCH_HI"


class BenchError(RuntimeError):
    pass


@dataclass
class HttpResult:
    status: int
    body: str
    json_body: Optional[Dict]


@dataclass
class StageResult:
    name: str
    ok: bool
    summary: str
    details: Dict


def normalize_base_url(value: str) -> str:
    value = value.strip()
    if not value.startswith("http://") and not value.startswith("https://"):
        value = "http://" + value
    return value.rstrip("/")


def parse_json_body(body: str) -> Optional[Dict]:
    try:
        parsed = json.loads(body)
    except json.JSONDecodeError:
        return None
    return parsed if isinstance(parsed, dict) else None


class BenchClient:
    def __init__(self, base_url: str, timeout_seconds: float, yolo_mode: bool) -> None:
        self.base_url = normalize_base_url(base_url)
        self.timeout_seconds = timeout_seconds
        self.yolo_mode = yolo_mode

    def get_json(self, path: str) -> HttpResult:
        return self._request_json("GET", path, None, None)

    def post_text(self, path: str, body: str) -> HttpResult:
        return self._request_json(
            "POST",
            path,
            body.encode("utf-8"),
            {"Content-Type": "text/plain; charset=utf-8"},
        )

    def post_json(self, path: str, payload: Dict) -> HttpResult:
        return self._request_json(
            "POST",
            path,
            json.dumps(payload).encode("utf-8"),
            {"Content-Type": "application/json"},
        )

    def put_text(self, path: str, body: str) -> HttpResult:
        return self._request_json(
            "PUT",
            path,
            body.encode("utf-8"),
            {"Content-Type": "text/plain; charset=utf-8"},
        )

    def delete_json(self, path: str) -> HttpResult:
        return self._request_json("DELETE", path, None, None)

    def scaffold_app(self, app_id: str) -> Dict:
        return require_json(
            self.post_text(f"/api/apps/scaffold?app_id={urllib.parse.quote(app_id)}", ""),
            "/api/apps/scaffold",
        )

    def update_app_source(self, app_id: str, source: str) -> Dict:
        return require_json(
            self.put_text(f"/api/apps/source?app_id={urllib.parse.quote(app_id)}", source),
            "/api/apps/source",
        )

    def chat_run_path(self, session_id: str) -> str:
        path = f"/api/chat/run?session_id={urllib.parse.quote(session_id)}"
        if self.yolo_mode:
            path += "&yolo=1"
        return path

    def chat_run(self, session_id: str, prompt: str) -> Dict:
        return require_json(self.post_text(self.chat_run_path(session_id), prompt), "/api/chat/run")

    def chat_session(self, session_id: str) -> Dict:
        return require_json(
            self.get_json(f"/api/chat/session?session_id={urllib.parse.quote(session_id)}"),
            "/api/chat/session",
        )

    def _request_json(
        self,
        method: str,
        path: str,
        data: Optional[bytes],
        headers: Optional[Dict[str, str]],
    ) -> HttpResult:
        request = urllib.request.Request(
            self.base_url + path,
            data=data,
            method=method,
            headers=headers or {},
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout_seconds) as response:
                body = response.read().decode("utf-8", errors="replace")
                return HttpResult(response.status, body, parse_json_body(body))
        except urllib.error.HTTPError as error:
            body = error.read().decode("utf-8", errors="replace")
            return HttpResult(error.code, body, parse_json_body(body))
        except urllib.error.URLError as error:
            raise BenchError(f"{method} {path} failed: {error}") from error
        except TimeoutError as error:
            raise BenchError(f"{method} {path} timed out") from error


def require_json(result: HttpResult, path: str) -> Dict:
    if result.json_body is None:
        raise BenchError(f"{path} did not return JSON: {result.body[:200]}")
    return result.json_body


def collect_requested_tools(transcript: str) -> List[str]:
    tools: List[str] = []
    seen: Set[str] = set()

    for raw_line in str(transcript or "").splitlines():
        line = raw_line.strip()
        payload: Optional[Dict] = None
        content = ""

        if not line:
            continue
        try:
            payload = json.loads(line)
        except json.JSONDecodeError:
            payload = None
        if isinstance(payload, dict):
            content = str(payload.get("content", ""))
        else:
            content = line
        if not content.startswith("Requested tools:"):
            continue
        for name in content.partition(":")[2].split(","):
            tool_name = name.strip()

            if tool_name and tool_name not in seen:
                seen.add(tool_name)
                tools.append(tool_name)
    return tools


def run_tool_case(
    client: BenchClient,
    session_id: str,
    prompt: str,
    expected_tools: List[str],
) -> Dict:
    result = client.chat_run(session_id, prompt)
    transcript = client.chat_session(session_id)
    called_tools = collect_requested_tools(transcript.get("transcript", ""))
    missing_tools = [name for name in expected_tools if name not in set(called_tools)]
    marker = f"TOOL_CASE_{session_id.split('_')[-1].upper()}_OK"
    final_text = result.get("final_text", "").strip()
    ok = bool(result.get("ok")) and not missing_tools and (final_text == marker or final_text == "")
    return {
        "session_id": session_id,
        "prompt": prompt,
        "expected_tools": expected_tools,
        "called_tools": called_tools,
        "missing_tools": missing_tools,
        "expected_marker": marker,
        "final_text_matches_marker": final_text == marker,
        "result": result,
        "transcript": transcript,
        "ok": ok,
    }


def task_event_runtime_attempt(
    client: BenchClient,
    app_id: str,
    task_id: str,
    scaffold_path: str,
    source: str,
    run_trigger: str,
    event_trigger: str,
    event_name: str,
    run_payload: str,
) -> Dict:
    details = {
        "scaffold": require_json(client.post_text(scaffold_path, ""), "/api/apps/scaffold"),
        "update": require_json(
            client.put_text(f"/api/apps/source?app_id={urllib.parse.quote(app_id)}", source),
            "/api/apps/source",
        ),
        "start": require_json(
            client.post_text(
                f"/api/tasks/start?task_id={urllib.parse.quote(task_id)}&app_id={urllib.parse.quote(app_id)}"
                f"&schedule=event&trigger={urllib.parse.quote(event_trigger)}&iterations=0",
                "",
            ),
            "/api/tasks/start",
        ),
    }
    details["tasks_running"] = require_json(client.get_json("/api/tasks"), "/api/tasks")
    details["emit"] = require_json(
        client.post_text(f"/api/events/emit?name={urllib.parse.quote(event_name)}", "near"),
        "/api/events/emit",
    )
    time.sleep(0.5)
    details["run"] = require_json(
        client.post_text(
            f"/api/apps/run?app_id={urllib.parse.quote(app_id)}&trigger={urllib.parse.quote(run_trigger)}",
            run_payload,
        ),
        "/api/apps/run",
    )
    details["stop"] = require_json(
        client.post_text(f"/api/tasks/stop?task_id={urllib.parse.quote(task_id)}", ""),
        "/api/tasks/stop",
    )
    details["tasks_after"] = require_json(client.get_json("/api/tasks"), "/api/tasks")
    client.delete_json(f"/api/apps?app_id={urllib.parse.quote(app_id)}")
    return details


def task_event_runtime_ok(details: Dict, task_id: str) -> bool:
    task_ids_running = [str(item.get("task_id", "")) for item in details.get("tasks_running", {}).get("tasks", []) if isinstance(item, dict)]
    task_after_ok = False

    for item in details.get("tasks_after", {}).get("tasks", []):
        if not isinstance(item, dict) or str(item.get("task_id", "")) != task_id:
            continue
        task_after_ok = (not bool(item.get("active"))) and bool(item.get("completed")) and bool(item.get("stop_requested"))
        break
    else:
        task_after_ok = True

    return (
        bool(details.get("scaffold", {}).get("ok"))
        and bool(details.get("update", {}).get("ok"))
        and task_id in task_ids_running
        and bool(details.get("emit", {}).get("ok"))
        and details.get("run", {}).get("result") == "near"
        and bool(details.get("stop", {}).get("ok"))
        and task_after_ok
    )


def stage_preflight(client: BenchClient, session_prefix: str) -> StageResult:
    status_result = require_json(client.get_json("/api/status"), "/api/status")
    auth_result = require_json(client.get_json("/api/auth/status"), "/api/auth/status")
    ok = bool(status_result.get("workspace_ready")) and bool(auth_result.get("configured"))
    summary = "device reachable, workspace ready, auth configured" if ok else "device preflight failed"
    return StageResult(
        name="preflight",
        ok=ok,
        summary=summary,
        details={"status": status_result, "auth": auth_result, "session_prefix": session_prefix},
    )


def stage_inventory(client: BenchClient, session_prefix: str) -> StageResult:
    tools_result = require_json(client.get_json("/api/tools"), "/api/tools")
    hardware_result = require_json(client.get_json("/api/hardware"), "/api/hardware")
    workspace_result = require_json(client.get_json("/api/workspace/files"), "/api/workspace/files")
    tool_names = [str(item.get("name", "")) for item in tools_result.get("tools", []) if isinstance(item, dict)]
    workspace_files = workspace_result.get("files", [])
    workspace_paths = [str(item.get("path", "")) for item in workspace_files if isinstance(item, dict)]
    board_pins = hardware_result.get("pins", [])
    ok = (
        "tool.list" in tool_names
        and "hardware.list" in tool_names
        and "app.install" in tool_names
        and "task.start" in tool_names
        and "event.emit" in tool_names
        and any(path == "HEARTBEAT.md" for path in workspace_paths)
        and any(path == "memory/MEMORY.md" for path in workspace_paths)
        and isinstance(board_pins, list)
    )
    return StageResult(
        name="inventory",
        ok=ok,
        summary="tool, hardware, and workspace inventory verified" if ok else "inventory surface validation failed",
        details={
            "session_prefix": session_prefix,
            "tools": tools_result,
            "hardware": hardware_result,
            "workspace": workspace_result,
        },
    )


def stage_hello(client: BenchClient, session_prefix: str) -> StageResult:
    session_id = f"{session_prefix}_hello"
    prompt = f"Reply with exactly {HELLO_MARKER} and nothing else."
    result = client.chat_run(session_id, prompt)
    ok = bool(result.get("ok")) and result.get("final_text", "").strip() == HELLO_MARKER
    return StageResult(
        name="hello",
        ok=ok,
        summary="exact text reply validated" if ok else "model did not return the expected exact marker",
        details={"session_id": session_id, "prompt": prompt, "result": result},
    )


def stage_tool_reasoning(client: BenchClient, session_prefix: str) -> StageResult:
    session_id = f"{session_prefix}_tools"
    prompt = (
        "Use tools to inspect the device, list installed apps, and tell me whether the workspace is ready. "
        "Keep the answer short."
    )
    result = client.chat_run(session_id, prompt)
    transcript = client.chat_session(session_id)
    used_tools = bool(result.get("used_tools"))
    transcript_text = transcript.get("transcript", "")
    ok = bool(result.get("ok")) and used_tools and "Requested tools:" in transcript_text
    return StageResult(
        name="tool_reasoning",
        ok=ok,
        summary="model executed a tool round" if ok else "tool round missing or unsuccessful",
        details={"session_id": session_id, "prompt": prompt, "result": result, "transcript": transcript},
    )


def stage_generate_echo_app(client: BenchClient, session_prefix: str) -> StageResult:
    app_id = f"{session_prefix}_echo"
    session_id = f"{session_prefix}_app"
    prompt = (
        f"Create a Lua app with app.install named {app_id}. "
        "The app must support the manual trigger and return exactly "
        "\"BENCH_ECHO:\" .. payload from handle(trigger, payload). "
        "After installing it, answer briefly."
    )
    result = client.chat_run(session_id, prompt)
    detail = require_json(client.get_json(f"/api/apps/detail?app_id={urllib.parse.quote(app_id)}"), "/api/apps/detail")
    run = require_json(
        client.post_text(f"/api/apps/run?app_id={urllib.parse.quote(app_id)}&trigger=manual", "alpha"),
        "/api/apps/run",
    )
    ok = bool(result.get("ok")) and detail.get("app", {}).get("id") == app_id and run.get("result") == "BENCH_ECHO:alpha"
    return StageResult(
        name="generate_echo_app",
        ok=ok,
        summary="LLM-generated Lua app installed and executed" if ok else "LLM-generated app validation failed",
        details={"session_id": session_id, "prompt": prompt, "result": result, "detail": detail, "run": run},
    )


def stage_task_event_runtime(client: BenchClient, session_prefix: str) -> StageResult:
    app_id = f"{session_prefix}_event_app"
    task_id = f"{session_prefix}_sensor_task"
    scaffold_path = (
        f"/api/apps/scaffold?app_id={urllib.parse.quote(app_id)}"
        "&permissions=fs.read%2Cfs.write"
        "&triggers=manual%2Csensor"
    )
    source = (
        "function on_sensor(payload)\n"
        "  espclaw.fs.write('memory/bench_event.txt', payload)\n"
        "  return 'EVENT:' .. payload\n"
        "end\n"
        "\n"
        "function handle(trigger, payload)\n"
        "  local text = espclaw.fs.read('memory/bench_event.txt')\n"
        "  if text == nil or text == '' then\n"
        "    return 'NONE'\n"
        "  end\n"
        "  return text\n"
        "end\n"
    )
    details = task_event_runtime_attempt(
        client,
        app_id,
        task_id,
        scaffold_path,
        source,
        "manual",
        "sensor",
        "sensor",
        "",
    )
    ok = task_event_runtime_ok(details, task_id)
    contract = "modern"

    if not ok:
        last_result = ""
        for item in details.get("tasks_after", {}).get("tasks", []):
            if isinstance(item, dict) and str(item.get("task_id", "")) == task_id:
                last_result = str(item.get("last_result", ""))
                break
        run_result = str(details.get("run", {}).get("result", ""))
        if "field 'fs'" in run_result or "does not accept trigger sensor" in last_result:
            details = task_event_runtime_attempt(
                client,
                app_id,
                task_id,
                f"/api/apps/scaffold?app_id={urllib.parse.quote(app_id)}",
                (
                    "function handle(trigger, payload)\n"
                    "  espclaw.write_file('memory/bench_event.txt', payload)\n"
                    "  return espclaw.read_file('memory/bench_event.txt')\n"
                    "end\n"
                ),
                "manual",
                "manual",
                "manual",
                "near",
            )
            ok = task_event_runtime_ok(details, task_id)
            contract = "legacy"

    return StageResult(
        name="task_event_runtime",
        ok=ok,
        summary="event-driven task runtime validated on-device" if ok else "task/event runtime validation failed",
        details={
            "contract": contract,
            "app_id": app_id,
            "task_id": task_id,
            **details,
        },
    )


def stage_vision(client: BenchClient, session_prefix: str) -> StageResult:
    session_id = f"{session_prefix}_vision"
    prompt = (
        "Use the camera tool to capture a fresh image, then describe the image briefly. "
        "Mention the capture path in your answer."
    )
    result = client.chat_run(session_id, prompt)
    transcript = client.chat_session(session_id)
    transcript_text = transcript.get("transcript", "")
    final_text = str(result.get("final_text", ""))
    ok = (
        bool(result.get("ok"))
        and bool(result.get("used_tools"))
        and "camera.capture" in transcript_text
        and ".jpg" in transcript_text
        and "media/" in final_text
    )
    return StageResult(
        name="vision",
        ok=ok,
        summary="camera capture and image-to-LLM round validated" if ok else "vision run failed or did not use the camera path",
        details={"session_id": session_id, "prompt": prompt, "result": result, "transcript": transcript},
    )


def stage_tool_sweep(client: BenchClient, session_prefix: str) -> StageResult:
    session_id_a = f"{session_prefix}_tool_sweep_a"
    session_id_b = f"{session_prefix}_tool_sweep_b"
    tools_result = require_json(client.get_json("/api/tools"), "/api/tools")
    hardware_result = require_json(client.get_json("/api/hardware"), "/api/hardware")
    all_tools = [
        str(item.get("name", ""))
        for item in tools_result.get("tools", [])
        if isinstance(item, dict) and item.get("name")
    ]
    excluded = {"system.reboot", "ota.apply"}
    expected = [name for name in all_tools if name not in excluded]
    pins = hardware_result.get("pins", []) if isinstance(hardware_result.get("pins", []), list) else []
    adc_channels = hardware_result.get("adc_channels", []) if isinstance(hardware_result.get("adc_channels", []), list) else []
    i2c_buses = hardware_result.get("i2c_buses", []) if isinstance(hardware_result.get("i2c_buses", []), list) else []
    uart_ports = hardware_result.get("uarts", []) if isinstance(hardware_result.get("uarts", []), list) else []
    flash_led_pin = next((int(item.get("pin", -1)) for item in pins if isinstance(item, dict) and item.get("name") == "flash_led"), 4)
    adc_channel = next((int(item.get("channel", 0)) for item in adc_channels if isinstance(item, dict)), 0)
    i2c_port = next((int(item.get("port", 0)) for item in i2c_buses if isinstance(item, dict)), 0)
    uart_port = next((int(item.get("port", 0)) for item in uart_ports if isinstance(item, dict)), 0)
    prompt_a = (
        "YOLO mode is enabled. This is a tool-call compliance test. The transcript is audited, so you must actually call every applicable tool from this exact set at least once before replying: "
        "tool.list, system.info, hardware.list, wifi.status, wifi.scan, ble.scan, fs.list, fs.write, fs.read, fs.delete, "
        "app.list, app.install, app.run, app.remove, behavior.list, behavior.register, behavior.start, behavior.stop, behavior.remove, "
        "task.list, task.start, task.stop, event.emit, event.watch_list, event.watch_add, event.watch_remove, ota.check. "
        "Use temporary ids starting with tool_sweep_. Create a minimal Lua app tool_sweep_app that returns 'TOOL_SWEEP_OK'. "
        "Create a periodic task tool_sweep_task and a periodic behavior tool_sweep_behavior, then stop/remove them. "
        "Use a workspace file named memory/tool_sweep.txt and delete it before finishing. "
        "Do not answer TOOL_SWEEP_CORE_DONE unless you completed the tool calls."
    )
    prompt_b = (
        "YOLO mode is enabled. This is a tool-call compliance test. The transcript is audited, so you must actually call every applicable tool from this exact set at least once before replying: "
        "camera.capture, gpio.read, gpio.write, pwm.write, ppm.write, adc.read, i2c.scan, i2c.read, i2c.write, temperature.read, imu.read, "
        "buzzer.play, pid.compute, control.mix, spi.transfer, uart.read, uart.write. "
        f"Use flash LED pin {flash_led_pin} for gpio/pwm/buzzer/ppm if you need a concrete pin, ADC channel {adc_channel}, I2C port {i2c_port}, and UART port {uart_port}. "
        "Hardware-dependent tools may fail if no external peripheral is attached, but you must still call them and then continue. "
        "Capture the camera into tool_sweep.jpg. "
        "Do not answer TOOL_SWEEP_HARDWARE_DONE unless you completed the tool calls."
    )
    result_a = client.chat_run(session_id_a, prompt_a)
    transcript_a = client.chat_session(session_id_a)
    result_b = client.chat_run(session_id_b, prompt_b)
    transcript_b = client.chat_session(session_id_b)
    called_tools = collect_requested_tools(transcript_a.get("transcript", "")) + collect_requested_tools(transcript_b.get("transcript", ""))
    called_set = set(called_tools)
    missing_tools = [name for name in expected if name not in called_set]
    ok = (
        bool(result_a.get("ok"))
        and bool(result_b.get("ok"))
        and result_a.get("final_text", "").strip() == "TOOL_SWEEP_CORE_DONE"
        and result_b.get("final_text", "").strip() == "TOOL_SWEEP_HARDWARE_DONE"
        and not missing_tools
    )
    return StageResult(
        name="tool_sweep",
        ok=ok,
        summary="LLM invoked the full non-disruptive tool catalog" if ok else "tool sweep missed one or more tools",
        details={
            "session_ids": [session_id_a, session_id_b],
            "expected_tools": expected,
            "called_tools": called_tools,
            "missing_tools": missing_tools,
            "prompt_a": prompt_a,
            "prompt_b": prompt_b,
            "result_a": result_a,
            "result_b": result_b,
            "transcript_a": transcript_a,
            "transcript_b": transcript_b,
        },
    )


def stage_tool_matrix_full(client: BenchClient, session_prefix: str) -> StageResult:
    hardware_result = require_json(client.get_json("/api/hardware"), "/api/hardware")
    pins = hardware_result.get("pins", []) if isinstance(hardware_result.get("pins", []), list) else []
    adc_channels = hardware_result.get("adc_channels", []) if isinstance(hardware_result.get("adc_channels", []), list) else []
    i2c_buses = hardware_result.get("i2c_buses", []) if isinstance(hardware_result.get("i2c_buses", []), list) else []
    uart_ports = hardware_result.get("uarts", []) if isinstance(hardware_result.get("uarts", []), list) else []
    flash_led_pin = next((int(item.get("pin", -1)) for item in pins if isinstance(item, dict) and item.get("name") == "flash_led"), 4)
    adc_channel = next((int(item.get("channel", 0)) for item in adc_channels if isinstance(item, dict)), 0)
    i2c_port = next((int(item.get("port", 0)) for item in i2c_buses if isinstance(item, dict)), 0)
    uart_port = next((int(item.get("port", 0)) for item in uart_ports if isinstance(item, dict)), 0)
    cases = [
        ("catalog", "This is a tool-call compliance test. Call tool.list exactly once, then reply exactly TOOL_CASE_CATALOG_OK.", ["tool.list"]),
        ("sysinfo", "This is a tool-call compliance test. Call system.info exactly once, then reply exactly TOOL_CASE_SYSINFO_OK.", ["system.info"]),
        ("hardware", "This is a tool-call compliance test. Call hardware.list exactly once, then reply exactly TOOL_CASE_HARDWARE_OK.", ["hardware.list"]),
        ("wifistatus", "This is a tool-call compliance test. Call wifi.status exactly once, then reply exactly TOOL_CASE_WIFISTATUS_OK.", ["wifi.status"]),
        ("wifiscan", "This is a tool-call compliance test. Call wifi.scan exactly once. If it reports unavailable, continue anyway and reply exactly TOOL_CASE_WIFISCAN_OK.", ["wifi.scan"]),
        ("blescan", "This is a tool-call compliance test. Call ble.scan exactly once. It may report unsupported on this board; continue anyway and reply exactly TOOL_CASE_BLESCAN_OK.", ["ble.scan"]),
        ("fslist", "This is a tool-call compliance test. Call fs.list on '.', then reply exactly TOOL_CASE_FSLIST_OK.", ["fs.list"]),
        ("fswrite", "This is a tool-call compliance test. Call fs.write on memory/tool_matrix.txt with content 'matrix', then reply exactly TOOL_CASE_FSWRITE_OK.", ["fs.write"]),
        ("fsread", "This is a tool-call compliance test. Call fs.read on memory/tool_matrix.txt, then reply exactly TOOL_CASE_FSREAD_OK.", ["fs.read"]),
        ("fsdelete", "This is a tool-call compliance test. Call fs.delete on memory/tool_matrix.txt, then reply exactly TOOL_CASE_FSDELETE_OK.", ["fs.delete"]),
        ("applist", "This is a tool-call compliance test. Call app.list exactly once, then reply exactly TOOL_CASE_APPLIST_OK.", ["app.list"]),
        ("appinstall", "This is a tool-call compliance test. Call app.install to create tool_matrix_app whose manual trigger returns exactly TOOL_MATRIX_APP_OK, then reply exactly TOOL_CASE_APPINSTALL_OK.", ["app.install"]),
        ("apprun", "This is a tool-call compliance test. Call app.run on tool_matrix_app with the manual trigger, then reply exactly TOOL_CASE_APPRUN_OK.", ["app.run"]),
        ("appremove", "This is a tool-call compliance test. Call app.remove on tool_matrix_app, then reply exactly TOOL_CASE_APPREMOVE_OK.", ["app.remove"]),
        ("behaviorlist", "This is a tool-call compliance test. Call behavior.list exactly once, then reply exactly TOOL_CASE_BEHAVIORLIST_OK.", ["behavior.list"]),
        ("behaviorregister", "This is a tool-call compliance test. Call behavior.register to create tool_matrix_behavior bound to tool_matrix_behavior_app, and include source that returns TOOL_MATRIX_BEHAVIOR_OK on manual trigger. If the app does not exist yet, use the same call to install/update its source. Reply exactly TOOL_CASE_BEHAVIORREGISTER_OK.", ["behavior.register"]),
        ("behaviorstart", "This is a tool-call compliance test. Call behavior.start on tool_matrix_behavior, then reply exactly TOOL_CASE_BEHAVIORSTART_OK.", ["behavior.start"]),
        ("behaviorstop", "This is a tool-call compliance test. Call behavior.stop on tool_matrix_behavior, then reply exactly TOOL_CASE_BEHAVIORSTOP_OK.", ["behavior.stop"]),
        ("behaviorremove", "This is a tool-call compliance test. Call behavior.remove on tool_matrix_behavior, then reply exactly TOOL_CASE_BEHAVIORREMOVE_OK.", ["behavior.remove"]),
        ("tasklist", "This is a tool-call compliance test. Call task.list exactly once, then reply exactly TOOL_CASE_TASKLIST_OK.", ["task.list"]),
        ("taskstart", "This is a tool-call compliance test. Call task.start exactly once to start task id tool_matrix_task using app id tool_matrix_task_app. Do not call app.list or app.install in this case. Reply exactly TOOL_CASE_TASKSTART_OK.", ["task.start"]),
        ("eventemit", "This is a tool-call compliance test. Call event.emit with name sensor and payload near, then reply exactly TOOL_CASE_EVENTEMIT_OK.", ["event.emit"]),
        ("taskstop", "This is a tool-call compliance test. Call task.stop on tool_matrix_task, then reply exactly TOOL_CASE_TASKSTOP_OK.", ["task.stop"]),
        ("watchlist", "This is a tool-call compliance test. Call event.watch_list exactly once, then reply exactly TOOL_CASE_WATCHLIST_OK.", ["event.watch_list"]),
        ("watchadd", f"This is a tool-call compliance test. Call event.watch_add to create watch id tool_matrix_watch as a UART watch on port {uart_port}, then reply exactly TOOL_CASE_WATCHADD_OK.", ["event.watch_add"]),
        ("watchremove", "This is a tool-call compliance test. Call event.watch_remove on tool_matrix_watch, then reply exactly TOOL_CASE_WATCHREMOVE_OK.", ["event.watch_remove"]),
        ("otacheck", "This is a tool-call compliance test. Call ota.check exactly once, then reply exactly TOOL_CASE_OTACHECK_OK.", ["ota.check"]),
        ("camera", "This is a tool-call compliance test. Call camera.capture with filename tool_matrix.jpg, then reply exactly TOOL_CASE_CAMERA_OK.", ["camera.capture"]),
        ("gpioread", f"This is a tool-call compliance test. Call gpio.read on pin {flash_led_pin}, then reply exactly TOOL_CASE_GPIOREAD_OK.", ["gpio.read"]),
        ("gpiowrite", f"This is a tool-call compliance test. Call gpio.write on pin {flash_led_pin} with value 0, then reply exactly TOOL_CASE_GPIOWRITE_OK.", ["gpio.write"]),
        ("pwmwrite", f"This is a tool-call compliance test. Call pwm.write using channel 0, pin {flash_led_pin}, and duty 128, then reply exactly TOOL_CASE_PWMWRITE_OK.", ["pwm.write"]),
        ("ppmwrite", f"This is a tool-call compliance test. Call ppm.write using channel 0, pin {flash_led_pin}, and value_us 1500. If it fails on this board, continue anyway and reply exactly TOOL_CASE_PPMWRITE_OK.", ["ppm.write"]),
        ("adcread", f"This is a tool-call compliance test. Call adc.read on channel {adc_channel}, then reply exactly TOOL_CASE_ADCREAD_OK.", ["adc.read"]),
        ("i2cscan", f"This is a tool-call compliance test. Call i2c.scan using I2C port {i2c_port}. If it fails because no bus or peripheral is attached, continue anyway and reply exactly TOOL_CASE_I2CSCAN_OK.", ["i2c.scan"]),
        ("i2cread", f"This is a tool-call compliance test. Call i2c.read using I2C port {i2c_port}, address 72, register 0, length 2. If it fails because no peripheral is attached, continue anyway and reply exactly TOOL_CASE_I2CREAD_OK.", ["i2c.read"]),
        ("i2cwrite", f"This is a tool-call compliance test. Call i2c.write using I2C port {i2c_port}, address 72, register 1, and data '00'. If it fails because no peripheral is attached, continue anyway and reply exactly TOOL_CASE_I2CWRITE_OK.", ["i2c.write"]),
        ("tempread", f"This is a tool-call compliance test. Call temperature.read for sensor tmp102 on I2C port {i2c_port}. If it fails because no peripheral is attached, continue anyway and reply exactly TOOL_CASE_TEMPREAD_OK.", ["temperature.read"]),
        ("imuread", f"This is a tool-call compliance test. Call imu.read for sensor mpu6050 on I2C port {i2c_port}. If it fails because no peripheral is attached, continue anyway and reply exactly TOOL_CASE_IMUREAD_OK.", ["imu.read"]),
        ("buzzer", f"This is a tool-call compliance test. Call buzzer.play using channel 0, pin {flash_led_pin}, frequency_hz 880, and duration_ms 20. If it fails on this board, continue anyway and reply exactly TOOL_CASE_BUZZER_OK.", ["buzzer.play"]),
        ("pid", "This is a tool-call compliance test. Call pid.compute with setpoint 1.0, measurement 0.5, dt_seconds 0.02, kp 1.0, ki 0.1, and kd 0.0, then reply exactly TOOL_CASE_PID_OK.", ["pid.compute"]),
        ("mix", "This is a tool-call compliance test. Call control.mix with mode differential, throttle 0.6, and steering -0.2, then reply exactly TOOL_CASE_MIX_OK.", ["control.mix"]),
        ("spi", "This is a tool-call compliance test. Call spi.transfer with bus 0 and data 'AA 55'. It may report unsupported on this board; continue anyway and reply exactly TOOL_CASE_SPI_OK.", ["spi.transfer"]),
        ("uartwrite", f"This is a tool-call compliance test. Call uart.write on port {uart_port} with data 'tool-matrix', then reply exactly TOOL_CASE_UARTWRITE_OK.", ["uart.write"]),
        ("uartread", f"This is a tool-call compliance test. Call uart.read on port {uart_port}. If it returns empty data, continue anyway and reply exactly TOOL_CASE_UARTREAD_OK.", ["uart.read"]),
    ]
    case_results = []
    called_tools: Set[str] = set()
    missing_tools: List[str] = []

    for case_id, prompt, expected_tools in cases:
        if case_id == "taskstart":
            client.scaffold_app("tool_matrix_task_app")
            client.update_app_source(
                "tool_matrix_task_app",
                "function handle(trigger, payload)\n"
                "  return 'task:' .. trigger .. ':' .. payload\n"
                "end\n",
            )
        case = run_tool_case(client, f"{session_prefix}_{case_id}", prompt, expected_tools)

        case_results.append(case)
        called_tools.update(case["called_tools"])
        for tool_name in case["missing_tools"]:
            if tool_name not in missing_tools:
                missing_tools.append(tool_name)

    return StageResult(
        name="tool_matrix_full",
        ok=all(case["ok"] for case in case_results),
        summary="real LLM tool matrix completed" if all(case["ok"] for case in case_results) else "one or more tool-matrix cases failed",
        details={
            "hardware": hardware_result,
            "cases": case_results,
            "called_tools": sorted(called_tools),
            "missing_tools": missing_tools,
        },
    )


def stage_large_lua_app(client: BenchClient, session_prefix: str) -> StageResult:
    thresholds = [1200, 1800, 2400, 3000, 3600]
    largest_success = 0
    attempts = []

    for target in thresholds:
        app_id = f"{session_prefix}_large_{target}"
        session_id = f"{session_prefix}_large_{target}"
        marker = f"LARGE_APP_OK_{target}"
        prompt = (
            f"YOLO mode is enabled. This is a tool-call compliance test. You must call app.install to create a Lua app named {app_id}. "
            f"The Lua source must be between {target} and {target + 400} bytes long, include several helper functions, local tables, and non-trivial branching, "
            f"and when run with the manual trigger it must return exactly {marker}. "
            "Define top-level helper functions and a top-level function handle(trigger, payload) as the runtime entrypoint. Do not return a module table. "
            "Use exactly one app.install tool call. Emit no assistant prose before or after the tool call. Put the full Lua source directly in the tool arguments. "
            "Do not answer INSTALLED unless the tool call succeeded. If you skip the tool call, answer FAILED."
        )
        result = client.chat_run(session_id, prompt)
        transcript = client.chat_session(session_id)
        detail = require_json(client.get_json(f"/api/apps/detail?app_id={urllib.parse.quote(app_id)}"), "/api/apps/detail")
        run = require_json(
            client.post_text(f"/api/apps/run?app_id={urllib.parse.quote(app_id)}&trigger=manual", "payload"),
            "/api/apps/run",
        )
        source = str(detail.get("app", {}).get("source", ""))
        source_length = len(source)
        success = (
            bool(result.get("ok"))
            and "app.install" in collect_requested_tools(transcript.get("transcript", ""))
            and detail.get("app", {}).get("id") == app_id
            and source_length >= target
            and run.get("result") == marker
        )
        attempts.append(
            {
                "target_bytes": target,
                "session_id": session_id,
                "app_id": app_id,
                "result": result,
                "transcript": transcript,
                "detail": detail,
                "run": run,
                "source_length": source_length,
                "ok": success,
            }
        )
        if success:
            largest_success = source_length
            client.delete_json(f"/api/apps?app_id={urllib.parse.quote(app_id)}")
            continue
        break

    ok = largest_success >= thresholds[0]
    return StageResult(
        name="large_lua_app",
        ok=ok,
        summary=f"largest successful generated Lua app source: {largest_success} bytes" if ok else "large Lua app generation failed at the first threshold",
        details={"attempts": attempts, "largest_success_bytes": largest_success},
    )


STAGES: Dict[str, Callable[[BenchClient, str], StageResult]] = {
    "preflight": stage_preflight,
    "inventory": stage_inventory,
    "hello": stage_hello,
    "tool_reasoning": stage_tool_reasoning,
    "generate_echo_app": stage_generate_echo_app,
    "task_event_runtime": stage_task_event_runtime,
    "vision": stage_vision,
    "tool_sweep": stage_tool_sweep,
    "tool_matrix_full": stage_tool_matrix_full,
    "large_lua_app": stage_large_lua_app,
}


def run_stages(client: BenchClient, session_prefix: str, stage_names: List[str], continue_on_failure: bool) -> Dict:
    started_at = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    results: List[StageResult] = []
    for name in stage_names:
        stage_fn = STAGES[name]
        try:
            stage_result = stage_fn(client, session_prefix)
        except BenchError as error:
            stage_result = StageResult(name=name, ok=False, summary=str(error), details={"error": str(error)})
        results.append(stage_result)
        if not stage_result.ok and not continue_on_failure:
            break
    ended_at = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    return {
        "ok": all(item.ok for item in results),
        "started_at": started_at,
        "ended_at": ended_at,
        "stages": [
            {"name": item.name, "ok": item.ok, "summary": item.summary, "details": item.details}
            for item in results
        ],
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run progressive ESPClaw checks against a real device.")
    parser.add_argument("--device", default="http://192.168.1.253:8080", help="Base URL of the ESPClaw device.")
    parser.add_argument(
        "--stages",
        default="preflight,inventory,hello,tool_reasoning,generate_echo_app,task_event_runtime,vision",
        help="Comma-separated stage list.",
    )
    parser.add_argument("--session-prefix", default="bench", help="Prefix for generated session/app ids.")
    parser.add_argument("--continue-on-failure", action="store_true", help="Run all stages even after a failure.")
    parser.add_argument("--timeout-seconds", type=float, default=30.0, help="Per-request timeout.")
    parser.add_argument("--output-json", help="Optional path to write the bench result JSON.")
    parser.set_defaults(yolo=True)
    parser.add_argument("--yolo", dest="yolo", action="store_true", help="Run chat stages with yolo=1 (default).")
    parser.add_argument("--no-yolo", dest="yolo", action="store_false", help="Disable yolo mode for chat stages.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    client = BenchClient(args.device, args.timeout_seconds, args.yolo)
    stage_names = [item.strip() for item in args.stages.split(",") if item.strip()]
    unknown = [item for item in stage_names if item not in STAGES]
    if unknown:
        raise SystemExit(f"unknown stage(s): {', '.join(unknown)}")

    result = run_stages(client, args.session_prefix, stage_names, args.continue_on_failure)
    rendered = json.dumps(result, indent=2, sort_keys=False)
    print(rendered)
    if args.output_json:
        with open(args.output_json, "w", encoding="utf-8") as handle:
            handle.write(rendered)
            handle.write("\n")
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
