#!/usr/bin/env python3
"""Unit contracts for the live hardware harness."""

import unittest
from unittest.mock import Mock, patch

from tools.live_hardware_harness import (
    CAPTURE_QUERY,
    Harness,
    HttpResponse,
    classify_operation,
    redact_secrets,
    validate_envelope,
    parse_root_evidence,
)


class LiveHardwareHarnessContracts(unittest.TestCase):
    def test_capture_route_get_includes_required_query_parameters(self):
        harness = Harness("http://example.test", capture=True)
        harness.openapi = {"paths": {"/baby-monitor/stream": {"get": {}}}}
        harness.client = Mock()
        harness.client.request.return_value = HttpResponse(503, {}, b"", {})

        harness.api_route_checks()

        paths = [call.args[1] for call in harness.client.request.call_args_list]
        self.assertIn("/api/v1/baby-monitor/stream" + CAPTURE_QUERY, paths)

    def test_safety_checks_retry_429_and_space_probes(self):
        harness = Harness("http://example.test")
        responses = [HttpResponse(429, {}, b"", {}), HttpResponse(403, {}, b"", {})]
        responses.extend(HttpResponse(403, {}, b"", {}) for _ in range(2))
        harness.request = Mock(side_effect=responses)

        with patch("tools.live_hardware_harness.time.sleep") as sleep:
            harness.safety_checks()

        self.assertEqual(len(harness.results), 3)
        self.assertTrue(all(result.status == "pass" for result in harness.results))
        self.assertGreaterEqual(sleep.call_count, 3)

    def test_authenticated_discovery_replaces_anonymous_blocked_classification(self):
        harness = Harness("http://example.test")
        harness.config = {"data": {"authentication": "users"}}
        harness.csrf = "a" * 64
        harness.client = Mock()
        envelope = {"ok": True, "data": {}, "error": None}
        harness.client.request.side_effect = [
            HttpResponse(401, {}, b"", {"ok": False, "data": None, "error": {"code": "auth"}}),
            HttpResponse(200, {}, b"", {"ok": True, "data": {"token": "b" * 64}, "error": None}),
            HttpResponse(200, {}, b"", envelope),
            HttpResponse(200, {}, b"", envelope),
        ]

        with patch.dict("os.environ", {"LIBREECHO_USERNAME": "user", "LIBREECHO_PASSWORD": "password"}):
            harness.authentication_checks()

        self.assertIn("api-discovery", [result.name for result in harness.results])
        self.assertNotIn("blocked", [result.status for result in harness.results])

    def test_read_only_routes_are_safe_and_mutating_routes_are_gated(self):
        self.assertEqual(classify_operation("GET", "/api/v1/status"), "read")
        self.assertEqual(classify_operation("GET", "/api/v1/events"), "read")
        self.assertEqual(classify_operation("POST", "/api/v1/audio/test"), "mutating")
        self.assertEqual(classify_operation("POST", "/api/v1/system/reboot"), "destructive")

    def test_redaction_removes_credentials_from_nested_output(self):
        value = {"token": "abc", "password": "secret", "nested": {"api_key": "key"}}
        redacted = redact_secrets(value)
        self.assertEqual(redacted, {
            "token": "<redacted>",
            "password": "<redacted>",
            "nested": {"api_key": "<redacted>"},
        })

    def test_validate_envelope_rejects_malformed_json_shape(self):
        self.assertEqual(validate_envelope({"ok": True, "data": {}, "error": None}), None)
        self.assertIn("error", validate_envelope({"ok": True}))
        self.assertIn("ok", validate_envelope({"data": {}, "error": None}))

    def test_parse_root_evidence_extracts_key_markers(self):
        raw = """AUDIT_START\nuid=0 gid=0\nLinux LibreEcho 6.1.178 armv7l\npresent /dev/wmt\npresent /sys/class/net/wlan0\nMemTotal: 497584 kB\nAUDIT_END\n"""
        evidence = parse_root_evidence(raw)
        self.assertTrue(evidence["root"])
        self.assertEqual(evidence["kernel"], "6.1.178")
        self.assertTrue(evidence["wlan0"])
        self.assertTrue(evidence["wmt"])
        self.assertEqual(evidence["mem_total_kb"], 497584)


if __name__ == "__main__":
    unittest.main()
