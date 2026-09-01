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
assert "weather_provider:provider" in source
assert "home_location:location" in source
assert "haveLatitude!==haveLongitude" in source
assert "lat<-90||lat>90||lon<-180||lon>180" in source
assert "lat===originalLat&&lon===originalLon" in source
assert "The place changed but the coordinates did not" in source
assert "This image cannot clear old coordinates safely" in source
assert "/js/integrations-ui.js?rev=32" in index, "browser cache revision was not advanced"

print("home location panel and validation contract: ok")
