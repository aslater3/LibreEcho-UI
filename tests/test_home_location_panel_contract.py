#!/usr/bin/env python3
"""Regression contract for the released Home location settings surface."""

from pathlib import Path

source = Path("web/js/integrations-ui.js").read_text(encoding="utf-8")
index = Path("web/index.html").read_text(encoding="utf-8")

assert source.count("${weatherCard(a)}") == 1, "Integrations must render exactly one Home location card"
assert "function bindHomeLocation(a)" in source
assert "bindHomeLocation(a);" in source
for selector in ("#wx-provider", "#wx-location", "#wx-lat", "#wx-lon", "#save-wx"):
    assert selector in source, f"missing Home location binding: {selector}"
assert "weather_provider:wxId($('#wx-provider').value)" in source
assert "home_location:$('#wx-location').value.trim()" in source
assert "/js/integrations-ui.js?rev=29" in index, "browser cache revision was not advanced"

print("home location panel contract: ok")
