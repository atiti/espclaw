import unittest

from scripts.real_device_bench import (
    HELLO_MARKER,
    collect_requested_tools,
    normalize_base_url,
    parse_json_body,
    run_stages,
)


class FakeClient:
    def __init__(self, responses):
        self.responses = responses
        self.yolo_mode = True

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

    def chat_run_path(self, session_id):
        path = f"/api/chat/run?session_id={session_id}"
        if self.yolo_mode:
            path += "&yolo=1"
        return path

    def chat_run(self, session_id, prompt):
        from scripts.real_device_bench import require_json

        return require_json(self.post_text(self.chat_run_path(session_id), prompt), "/api/chat/run")

    def chat_session(self, session_id):
        from scripts.real_device_bench import require_json

        return require_json(self.get_json(f"/api/chat/session?session_id={session_id}"), "/api/chat/session")

    def operator_turn(self, surface, session_id, prompt):
        from scripts.real_device_bench import require_json

        path = f"/api/bench/operator-turn?surface={surface}&session_id={session_id}"
        if self.yolo_mode:
            path += "&yolo=1"
        return require_json(self.post_text(path, prompt), "/api/bench/operator-turn")


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

    def test_collect_requested_tools_deduplicates(self):
        transcript = (
            "{\"role\":\"assistant\",\"content\":\"Requested tools: tool.list, fs.read\"}\n"
            "{\"role\":\"assistant\",\"content\":\"Requested tools: fs.read, camera.capture\"}\n"
        )
        self.assertEqual(collect_requested_tools(transcript), ["tool.list", "fs.read", "camera.capture"])

    def test_run_stages_stops_on_first_failure(self):
        responses = {
            "/api/status": HttpResult(200, "", {"workspace_ready": True}),
            "/api/auth/status": HttpResult(200, "", {"configured": True}),
            ("POST", "/api/chat/run?session_id=bench_hello&yolo=1", f"Reply with exactly {HELLO_MARKER} and nothing else."): HttpResult(
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
            ("POST", "/api/chat/run?session_id=bench_hello&yolo=1", f"Reply with exactly {HELLO_MARKER} and nothing else."): HttpResult(
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

    def test_vision_stage_passes(self):
        responses = {
            ("POST", "/api/chat/run?session_id=bench_vision&yolo=1", "Use the camera tool to capture a fresh image, then describe the image briefly. Mention the capture path in your answer."): HttpResult(
                200,
                "",
                {
                    "ok": True,
                    "used_tools": True,
                    "final_text": "Captured image at media/capture_123.jpg. It appears evenly lit.",
                },
            ),
            "/api/chat/session?session_id=bench_vision": HttpResult(
                200,
                "",
                {
                    "session_id": "bench_vision",
                    "transcript": (
                        "{\"role\":\"assistant\",\"content\":\"Requested tools: camera.capture\"}\n"
                        "{\"role\":\"tool\",\"content\":\"{\\\"path\\\":\\\"media/capture_123.jpg\\\"}\"}\n"
                    ),
                },
            ),
        }

        result = run_stages(FakeClient(responses), "bench", ["vision"], continue_on_failure=False)
        self.assertTrue(result["stages"][0]["ok"])

    def test_operator_surfaces_stage_passes(self):
        responses = {
            (
                "POST",
                "/api/bench/operator-turn?surface=web&session_id=bench_surface_web&yolo=1",
                "List out all the available tools to you. Call tool.list exactly once, then briefly summarize the tool surface.",
            ): HttpResult(
                200,
                "",
                {
                    "ok": True,
                    "surface": "web",
                    "used_tools": True,
                    "iterations": 1,
                    "final_text": "Web surface inspected the tool catalog.",
                    "memory": {
                        "free_internal_before": 54000,
                        "largest_internal_before": 32000,
                        "free_internal_after": 53400,
                        "largest_internal_after": 31600,
                    },
                },
            ),
            (
                "POST",
                "/api/bench/operator-turn?surface=uart&session_id=bench_surface_uart&yolo=1",
                "List out all the available tools to you. Call tool.list exactly once, then briefly summarize the tool surface.",
            ): HttpResult(
                200,
                "",
                {
                    "ok": True,
                    "surface": "uart",
                    "used_tools": True,
                    "iterations": 1,
                    "final_text": "UART surface inspected the tool catalog.",
                    "memory": {
                        "free_internal_before": 53000,
                        "largest_internal_before": 30000,
                        "free_internal_after": 52400,
                        "largest_internal_after": 29500,
                    },
                },
            ),
            (
                "POST",
                "/api/bench/operator-turn?surface=telegram&session_id=bench_surface_telegram&yolo=1",
                "List out all the available tools to you. Call tool.list exactly once, then briefly summarize the tool surface.",
            ): HttpResult(
                200,
                "",
                {
                    "ok": True,
                    "surface": "telegram",
                    "used_tools": True,
                    "iterations": 1,
                    "final_text": "Telegram surface inspected the tool catalog.",
                    "memory": {
                        "free_internal_before": 52000,
                        "largest_internal_before": 28000,
                        "free_internal_after": 51500,
                        "largest_internal_after": 27600,
                    },
                },
            ),
            "/api/chat/session?session_id=bench_surface_web": HttpResult(
                200,
                "",
                {"transcript": "{\"role\":\"assistant\",\"content\":\"Requested tools: tool.list\"}\n"},
            ),
            "/api/chat/session?session_id=bench_surface_uart": HttpResult(
                200,
                "",
                {"transcript": "{\"role\":\"assistant\",\"content\":\"Requested tools: tool.list\"}\n"},
            ),
        }

        result = run_stages(FakeClient(responses), "bench", ["operator_surfaces"], continue_on_failure=False)
        self.assertTrue(result["stages"][0]["ok"])
        self.assertEqual(len(result["stages"][0]["details"]["surfaces"]), 3)

    def test_semantic_blink_task_stage_passes(self):
        responses = {
            (
                "POST",
                "/api/chat/run?session_id=bench_semantic_blink&yolo=1",
                "Create a Lua app named bench_blink_app that toggles GPIO 4 exactly 10 times, with 1000 ms on and 2000 ms off, then start a background task named bench_blink_task that runs it now. Preserve those exact timing values and pin assignment.",
            ): HttpResult(
                200,
                "",
                {"ok": True, "used_tools": True, "final_text": "Installed bench_blink_app and started bench_blink_task."},
            ),
            "/api/chat/session?session_id=bench_semantic_blink": HttpResult(
                200,
                "",
                {
                    "transcript": (
                        "{\"role\":\"assistant\",\"content\":\"Requested tools: app.install, task.start\"}\n"
                    )
                },
            ),
            "/api/apps/detail?app_id=bench_blink_app": HttpResult(
                200,
                "",
                {
                    "app": {
                        "id": "bench_blink_app",
                        "source": (
                            "function handle(trigger, payload)\n"
                            "  for i = 1, 10 do\n"
                            "    espclaw.gpio.write(4, 1)\n"
                            "    espclaw.sleep_ms(1000)\n"
                            "    espclaw.gpio.write(4, 0)\n"
                            "    espclaw.sleep_ms(2000)\n"
                            "  end\n"
                            "  return 'done'\n"
                            "end\n"
                        ),
                    }
                },
            ),
            "/api/tasks": HttpResult(
                200,
                "",
                {"tasks": [{"task_id": "bench_blink_task", "app_id": "bench_blink_app", "active": True}]},
            ),
            "/api/logs?bytes=2048": HttpResult(
                200,
                "",
                {"ok": True, "tail": "I (...) espclaw_agent: tool call source=model name=task.start"},
            ),
        }

        result = run_stages(FakeClient(responses), "bench", ["semantic_blink_task"], continue_on_failure=False)
        self.assertTrue(result["stages"][0]["ok"])
        self.assertEqual(result["stages"][0]["details"]["app_id"], "bench_blink_app")

    def test_tool_sweep_stage_collects_requested_tools(self):
        responses = {
            "/api/tools": HttpResult(
                200,
                "",
                {
                    "tools": [
                        {"name": "tool.list"},
                        {"name": "system.info"},
                        {"name": "hardware.list"},
                        {"name": "wifi.status"},
                        {"name": "wifi.scan"},
                        {"name": "ble.scan"},
                        {"name": "fs.list"},
                        {"name": "fs.write"},
                        {"name": "fs.read"},
                        {"name": "fs.delete"},
                        {"name": "app.list"},
                        {"name": "app.install"},
                        {"name": "app.run"},
                        {"name": "app.remove"},
                        {"name": "behavior.list"},
                        {"name": "behavior.register"},
                        {"name": "behavior.start"},
                        {"name": "behavior.stop"},
                        {"name": "behavior.remove"},
                        {"name": "task.list"},
                        {"name": "task.start"},
                        {"name": "task.stop"},
                        {"name": "event.emit"},
                        {"name": "event.watch_list"},
                        {"name": "event.watch_add"},
                        {"name": "event.watch_remove"},
                        {"name": "ota.check"},
                        {"name": "camera.capture"},
                        {"name": "gpio.read"},
                        {"name": "gpio.write"},
                        {"name": "pwm.write"},
                        {"name": "ppm.write"},
                        {"name": "adc.read"},
                        {"name": "i2c.scan"},
                        {"name": "i2c.read"},
                        {"name": "i2c.write"},
                        {"name": "temperature.read"},
                        {"name": "imu.read"},
                        {"name": "buzzer.play"},
                        {"name": "pid.compute"},
                        {"name": "control.mix"},
                        {"name": "spi.transfer"},
                        {"name": "uart.read"},
                        {"name": "uart.write"},
                    ]
                },
            ),
            "/api/hardware": HttpResult(
                200,
                "",
                {
                    "pins": [{"name": "flash_led", "pin": 4}],
                    "adc_channels": [{"unit": 1, "channel": 6}],
                    "i2c_buses": [{"port": 0}],
                    "uarts": [{"port": 0}],
                },
            ),
            "/api/chat/session?session_id=bench_tool_sweep_a": HttpResult(
                200,
                "",
                {"transcript": "{\"role\":\"assistant\",\"content\":\"Requested tools: tool.list, system.info, hardware.list, wifi.status, wifi.scan, ble.scan, fs.list, fs.write, fs.read, fs.delete, app.list, app.install, app.run, app.remove, behavior.list, behavior.register, behavior.start, behavior.stop, behavior.remove, task.list, task.start, task.stop, event.emit, event.watch_list, event.watch_add, event.watch_remove, ota.check\"}\n"},
            ),
            "/api/chat/session?session_id=bench_tool_sweep_b": HttpResult(
                200,
                "",
                {"transcript": "{\"role\":\"assistant\",\"content\":\"Requested tools: camera.capture, gpio.read, gpio.write, pwm.write, ppm.write, adc.read, i2c.scan, i2c.read, i2c.write, temperature.read, imu.read, buzzer.play, pid.compute, control.mix, spi.transfer, uart.read, uart.write\"}\n"},
            ),
        }

        class SweepClient(FakeClient):
            def post_text(self, path, body):
                if path == "/api/chat/run?session_id=bench_tool_sweep_a&yolo=1":
                    return HttpResult(200, "", {"ok": True, "final_text": "TOOL_SWEEP_CORE_DONE"})
                if path == "/api/chat/run?session_id=bench_tool_sweep_b&yolo=1":
                    return HttpResult(200, "", {"ok": True, "final_text": "TOOL_SWEEP_HARDWARE_DONE"})
                raise KeyError(path)

        result = run_stages(SweepClient(responses), "bench", ["tool_sweep"], continue_on_failure=False)
        self.assertTrue(result["stages"][0]["ok"])
        self.assertEqual(result["stages"][0]["details"]["missing_tools"], [])

    def test_large_lua_app_stage_records_largest_success(self):
        responses = {
            "/api/apps/detail?app_id=bench_large_1200": HttpResult(200, "", {"app": {"id": "bench_large_1200", "source": "a" * 1300}}),
            "/api/chat/session?session_id=bench_large_1200": HttpResult(200, "", {"transcript": "{\"role\":\"assistant\",\"content\":\"Requested tools: app.install\"}\n"}),
            ("POST", "/api/apps/run?app_id=bench_large_1200&trigger=manual", "payload"): HttpResult(200, "", {"result": "LARGE_APP_OK_1200"}),
            "/api/apps?app_id=bench_large_1200": HttpResult(200, "", {"ok": True}),
            "/api/apps/detail?app_id=bench_large_1800": HttpResult(200, "", {"app": {"id": "bench_large_1800", "source": "b" * 1900}}),
            "/api/chat/session?session_id=bench_large_1800": HttpResult(200, "", {"transcript": "{\"role\":\"assistant\",\"content\":\"Requested tools: app.install\"}\n"}),
            ("POST", "/api/apps/run?app_id=bench_large_1800&trigger=manual", "payload"): HttpResult(200, "", {"result": "LARGE_APP_OK_1800"}),
            "/api/apps?app_id=bench_large_1800": HttpResult(200, "", {"ok": True}),
            "/api/apps/detail?app_id=bench_large_2400": HttpResult(200, "", {"app": {"id": "bench_large_2400", "source": "c" * 1800}}),
            "/api/chat/session?session_id=bench_large_2400": HttpResult(200, "", {"transcript": "{\"role\":\"assistant\",\"content\":\"Requested tools: app.install\"}\n"}),
            ("POST", "/api/apps/run?app_id=bench_large_2400&trigger=manual", "payload"): HttpResult(200, "", {"result": "wrong"}),
        }

        class LargeClient(FakeClient):
            def post_text(self, path, body):
                if path == "/api/chat/run?session_id=bench_large_1200&yolo=1":
                    return HttpResult(200, "", {"ok": True, "final_text": ""})
                if path == "/api/chat/run?session_id=bench_large_1800&yolo=1":
                    return HttpResult(200, "", {"ok": True, "final_text": "INSTALLED"})
                if path == "/api/chat/run?session_id=bench_large_2400&yolo=1":
                    return HttpResult(200, "", {"ok": False, "final_text": "too large"})
                for key, value in self.responses.items():
                    if isinstance(key, tuple) and key[0] == "POST" and key[1] == path:
                        return value
                raise KeyError(path)

        result = run_stages(LargeClient(responses), "bench", ["large_lua_app"], continue_on_failure=False)
        self.assertTrue(result["stages"][0]["ok"])
        self.assertEqual(result["stages"][0]["details"]["largest_success_bytes"], 1900)


if __name__ == "__main__":
    unittest.main()
