#!/usr/bin/env python3
"""Regression contracts for the 0.13.10 setup and Wi-Fi UI fixes."""
from pathlib import Path
import json

ROOT = Path(__file__).parents[1]
SETUP_JS = (ROOT / "web/js/setup.js").read_text(encoding="utf-8")
SETUP_HTML = (ROOT / "web/setup.html").read_text(encoding="utf-8")
APP_JS = (ROOT / "web/js/app.js").read_text(encoding="utf-8")
BACKEND_H = (ROOT / "src/backend.h").read_text(encoding="utf-8")
NETWORKD = (ROOT / "src/adapter/networkd.c").read_text(encoding="utf-8")
BACKEND_LINUX = (ROOT / "src/backend_linux.c").read_text(encoding="utf-8")
API_C = (ROOT / "src/api.c").read_text(encoding="utf-8")
API_DOC = (ROOT / "docs/API.md").read_text(encoding="utf-8")
OPENAPI = json.loads((ROOT / "web/openapi.json").read_text(encoding="utf-8"))


def test_setup_completion_uses_device_http_port_for_ip_and_mdns_links():
    assert 'id="lan-link"' in SETUP_HTML
    assert 'id="mdns-link"' in SETUP_HTML
    assert "deviceIp" in SETUP_JS
    assert "window.location.port" not in SETUP_JS
    assert "http://${name}.local:8080/" in SETUP_JS
    assert "http://${deviceIp}:8080/" in SETUP_JS
    assert "device's HTTP port `8080`" in API_DOC
    assert "host-side ADB forwarding port" in API_DOC
    assert "AirPlay" in SETUP_HTML and "Avahi" in SETUP_HTML
    assert "mdns_available" in SETUP_JS


def test_scan_contract_preserves_radio_metadata_and_security_capabilities():
    for field in ("frequency_mhz", "channel", "band", "rssi_dbm", "capabilities"):
        assert field in BACKEND_H
        assert field in BACKEND_LINUX
        assert field in API_C
        assert field in API_DOC
    assert "wpa3-transition" in NETWORKD
    assert "wpa3-only" in NETWORKD
    assert "WPA3-SAE" in NETWORKD
    assert "wpa2_attempt" in NETWORKD


def test_ui_keeps_wpa3_networks_visible_and_offers_bounded_wpa2_attempt():
    assert "Try WPA2" in SETUP_JS
    assert "wpa2_attempt" in SETUP_JS
    assert "security_capabilities" in SETUP_JS or "capabilities" in SETUP_JS
    assert "actual" in SETUP_JS.lower() or "result" in SETUP_JS.lower()
    assert "Try WPA2" in APP_JS
    assert "wpa2_attempt" in APP_JS
    assert "wpa3-only" in APP_JS


def test_scan_api_schema_documents_metadata_and_security_outcomes():
    scan = OPENAPI["paths"]["/network/wifi/scan"]["get"]["responses"]["200"]
    schema_text = json.dumps(scan)
    for field in ("frequency_mhz", "channel", "band", "rssi_dbm", "capabilities", "wpa2_attempt"):
        assert field in schema_text
    assert "wpa3-transition" in API_DOC
    assert "wpa3-only" in API_DOC


if __name__ == "__main__":
    tests = [value for name, value in globals().items()
             if name.startswith("test_") and callable(value)]
    for test in tests:
        test()
    print(f"{len(tests)} setup/Wi-Fi UI contracts: ok")
