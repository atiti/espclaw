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


if __name__ == "__main__":
    unittest.main()
