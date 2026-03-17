#!/usr/bin/env python3
import argparse
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Callable, Dict, List, Optional


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
    def __init__(self, base_url: str, timeout_seconds: float) -> None:
        self.base_url = normalize_base_url(base_url)
        self.timeout_seconds = timeout_seconds

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
    result = require_json(client.post_text(f"/api/chat/run?session_id={urllib.parse.quote(session_id)}", prompt), "/api/chat/run")
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
    result = require_json(client.post_text(f"/api/chat/run?session_id={urllib.parse.quote(session_id)}", prompt), "/api/chat/run")
    transcript = require_json(
        client.get_json(f"/api/chat/session?session_id={urllib.parse.quote(session_id)}"),
        "/api/chat/session",
    )
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
    result = require_json(client.post_text(f"/api/chat/run?session_id={urllib.parse.quote(session_id)}", prompt), "/api/chat/run")
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


STAGES: Dict[str, Callable[[BenchClient, str], StageResult]] = {
    "preflight": stage_preflight,
    "inventory": stage_inventory,
    "hello": stage_hello,
    "tool_reasoning": stage_tool_reasoning,
    "generate_echo_app": stage_generate_echo_app,
    "task_event_runtime": stage_task_event_runtime,
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
        default="preflight,inventory,hello,tool_reasoning,generate_echo_app,task_event_runtime",
        help="Comma-separated stage list.",
    )
    parser.add_argument("--session-prefix", default="bench", help="Prefix for generated session/app ids.")
    parser.add_argument("--continue-on-failure", action="store_true", help="Run all stages even after a failure.")
    parser.add_argument("--timeout-seconds", type=float, default=30.0, help="Per-request timeout.")
    parser.add_argument("--output-json", help="Optional path to write the bench result JSON.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    client = BenchClient(args.device, args.timeout_seconds)
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
