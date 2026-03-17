import unittest

from scripts.real_device_bench import HELLO_MARKER, normalize_base_url, parse_json_body, run_stages


class FakeClient:
    def __init__(self, responses):
        self.responses = responses

    def get_json(self, path):
        return self.responses[path]

    def post_text(self, path, body):
        key = ("POST", path, body)
        return self.responses[key]

    def put_text(self, path, body):
        key = ("PUT", path, body)
        return self.responses[key]

    def delete_json(self, path):
        return self.responses[path]


class HttpResult:
    def __init__(self, status, body, json_body):
        self.status = status
        self.body = body
        self.json_body = json_body


class RealDeviceBenchTests(unittest.TestCase):
    def test_normalize_base_url_adds_scheme(self):
        self.assertEqual(normalize_base_url("192.168.1.10:8080"), "http://192.168.1.10:8080")

    def test_parse_json_body_rejects_non_object(self):
        self.assertIsNone(parse_json_body("[1,2,3]"))
        self.assertIsNone(parse_json_body("not-json"))

    def test_run_stages_stops_on_first_failure(self):
        responses = {
            "/api/status": HttpResult(200, "", {"workspace_ready": True}),
            "/api/auth/status": HttpResult(200, "", {"configured": True}),
            ("POST", "/api/chat/run?session_id=bench_hello", f"Reply with exactly {HELLO_MARKER} and nothing else."): HttpResult(
                200,
                "",
                {"ok": False, "final_text": "wrong"},
            ),
        }
        result = run_stages(FakeClient(responses), "bench", ["preflight", "hello"], continue_on_failure=False)
        self.assertFalse(result["ok"])
        self.assertEqual(len(result["stages"]), 2)
        self.assertFalse(result["stages"][1]["ok"])

    def test_run_stages_passes_hello(self):
        responses = {
            "/api/status": HttpResult(200, "", {"workspace_ready": True}),
            "/api/auth/status": HttpResult(200, "", {"configured": True}),
            ("POST", "/api/chat/run?session_id=bench_hello", f"Reply with exactly {HELLO_MARKER} and nothing else."): HttpResult(
                200,
                "",
                {"ok": True, "final_text": HELLO_MARKER},
            ),
        }
        result = run_stages(FakeClient(responses), "bench", ["preflight", "hello"], continue_on_failure=False)
        self.assertTrue(result["stages"][0]["ok"])
        self.assertTrue(result["stages"][1]["ok"])

    def test_inventory_stage_passes(self):
        responses = {
            "/api/status": HttpResult(200, "", {"workspace_ready": True}),
            "/api/auth/status": HttpResult(200, "", {"configured": True}),
            "/api/tools": HttpResult(
                200,
                "",
                {
                    "tools": [
                        {"name": "tool.list"},
                        {"name": "hardware.list"},
                        {"name": "app.install"},
                        {"name": "task.start"},
                        {"name": "event.emit"},
                    ]
                },
            ),
            "/api/hardware": HttpResult(200, "", {"pins": [{"name": "flash_led", "pin": 4}]}),
            "/api/workspace/files": HttpResult(
                200,
                "",
                {"files": [{"path": "HEARTBEAT.md", "exists": True}, {"path": "memory/MEMORY.md", "exists": True}]},
            ),
        }
        result = run_stages(FakeClient(responses), "bench", ["inventory"], continue_on_failure=False)
        self.assertTrue(result["stages"][0]["ok"])

    def test_task_event_runtime_stage_passes(self):
        responses = {
            (
                "POST",
                "/api/apps/scaffold?app_id=bench_event_app&permissions=fs.read%2Cfs.write&triggers=manual%2Csensor",
                "",
            ): HttpResult(200, "", {"ok": True}),
            (
                "PUT",
                "/api/apps/source?app_id=bench_event_app",
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
                "end\n",
            ): HttpResult(200, "", {"ok": True}),
            (
                "POST",
                "/api/tasks/start?task_id=bench_sensor_task&app_id=bench_event_app&schedule=event&trigger=sensor&iterations=0",
                "",
            ): HttpResult(200, "", {"tasks": [{"task_id": "bench_sensor_task"}]}),
            "/api/tasks": HttpResult(200, "", {"tasks": []}),
            ("POST", "/api/events/emit?name=sensor", "near"): HttpResult(200, "", {"ok": True}),
            ("POST", "/api/apps/run?app_id=bench_event_app&trigger=manual", ""): HttpResult(200, "", {"result": "near"}),
            ("POST", "/api/tasks/stop?task_id=bench_sensor_task", ""): HttpResult(200, "", {"ok": True}),
            "/api/apps?app_id=bench_event_app": HttpResult(200, "", {"ok": True}),
        }

        class TaskClient(FakeClient):
            def __init__(self, responses):
                super().__init__(responses)
                self.task_reads = 0

            def get_json(self, path):
                if path == "/api/tasks":
                    self.task_reads += 1
                    if self.task_reads == 1:
                        return HttpResult(200, "", {"tasks": [{"task_id": "bench_sensor_task"}]})
                    return HttpResult(
                        200,
                        "",
                        {
                            "tasks": [
                                {
                                    "task_id": "bench_sensor_task",
                                    "active": False,
                                    "completed": True,
                                    "stop_requested": True,
                                }
                            ]
                        },
                    )
                return self.responses[path]

        result = run_stages(TaskClient(responses), "bench", ["task_event_runtime"], continue_on_failure=False)
        self.assertTrue(result["stages"][0]["ok"])
        self.assertEqual(result["stages"][0]["details"]["contract"], "modern")

    def test_task_event_runtime_stage_falls_back_to_legacy_contract(self):
        responses = {
            (
                "POST",
                "/api/apps/scaffold?app_id=bench_event_app&permissions=fs.read%2Cfs.write&triggers=manual%2Csensor",
                "",
            ): HttpResult(200, "", {"ok": True}),
            (
                "PUT",
                "/api/apps/source?app_id=bench_event_app",
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
                "end\n",
            ): HttpResult(200, "", {"ok": True}),
            (
                "POST",
                "/api/tasks/start?task_id=bench_sensor_task&app_id=bench_event_app&schedule=event&trigger=manual&iterations=0",
                "",
            ): HttpResult(200, "", {"tasks": [{"task_id": "bench_sensor_task"}]}),
            (
                "POST",
                "/api/tasks/start?task_id=bench_sensor_task&app_id=bench_event_app&schedule=event&trigger=sensor&iterations=0",
                "",
            ): HttpResult(200, "", {"tasks": [{"task_id": "bench_sensor_task"}]}),
            ("POST", "/api/events/emit?name=sensor", "near"): HttpResult(200, "", {"ok": True}),
            ("POST", "/api/apps/run?app_id=bench_event_app&trigger=manual", ""): HttpResult(
                200, "", {"ok": False, "result": "lua runtime failed: main.lua:7: attempt to index a nil value (field 'fs')"}
            ),
            ("POST", "/api/tasks/stop?task_id=bench_sensor_task", ""): HttpResult(200, "", {"ok": True}),
            (
                "POST",
                "/api/apps/scaffold?app_id=bench_event_app",
                "",
            ): HttpResult(200, "", {"ok": True}),
            (
                "PUT",
                "/api/apps/source?app_id=bench_event_app",
                "function handle(trigger, payload)\n"
                "  espclaw.write_file('memory/bench_event.txt', payload)\n"
                "  return espclaw.read_file('memory/bench_event.txt')\n"
                "end\n",
            ): HttpResult(200, "", {"ok": True}),
            ("POST", "/api/events/emit?name=manual", "near"): HttpResult(200, "", {"ok": True}),
            ("POST", "/api/apps/run?app_id=bench_event_app&trigger=manual", "near"): HttpResult(
                200, "", {"ok": True, "result": "near"}
            ),
            "/api/apps?app_id=bench_event_app": HttpResult(200, "", {"ok": True}),
        }

        class LegacyTaskClient(FakeClient):
            def __init__(self, responses):
                super().__init__(responses)
                self.task_reads = 0

            def get_json(self, path):
                if path == "/api/tasks":
                    self.task_reads += 1
                    if self.task_reads in (1, 3):
                        return HttpResult(200, "", {"tasks": [{"task_id": "bench_sensor_task"}]})
                    if self.task_reads == 2:
                        return HttpResult(
                            200,
                            "",
                            {
                                "tasks": [
                                    {
                                        "task_id": "bench_sensor_task",
                                        "active": False,
                                        "completed": True,
                                        "stop_requested": True,
                                        "last_result": "app bench_event_app does not accept trigger sensor",
                                    }
                                ]
                            },
                        )
                    if self.task_reads == 4:
                        return HttpResult(
                            200,
                            "",
                            {
                                "tasks": [
                                    {
                                        "task_id": "bench_sensor_task",
                                        "active": False,
                                        "completed": True,
                                        "stop_requested": True,
                                        "last_result": "near",
                                    }
                                ]
                            },
                        )
                    return HttpResult(200, "", {"tasks": []})
                return self.responses[path]

        result = run_stages(LegacyTaskClient(responses), "bench", ["task_event_runtime"], continue_on_failure=False)
        self.assertTrue(result["stages"][0]["ok"])
        self.assertEqual(result["stages"][0]["details"]["contract"], "legacy")


if __name__ == "__main__":
    unittest.main()
