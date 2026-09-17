#!/usr/bin/env python3
"""Source contract for the Met Office (UKMO) weather provider (0.14 #259).

The provider is one enum value on the assistant config that must be accepted by
agentd, offered by the UI and documented in the same change; the fetch itself
stays on the keyless Open-Meteo endpoint. This is a source contract, not a
runtime test: it cannot prove what a live endpoint returns.
"""

import re
from pathlib import Path

app = Path("web/js/app.js").read_text(encoding="utf-8")
agentd = Path("src/adapter/agentd.c").read_text(encoding="utf-8")
api_doc = Path("docs/API.md").read_text(encoding="utf-8")
openapi = Path("web/openapi.json").read_text(encoding="utf-8")
index = Path("web/index.html").read_text(encoding="utf-8")

# The UI offers the source, and the id/label pair round-trips through wxId().
assert "['ukmo','Met Office UK']" in app, "the UKMO provider is not offered by the UI"
assert "['open-meteo','Open-Meteo'],['ukmo','Met Office UK']" in app, (
    "the provider list must keep the id/label pairing wxId() depends on")

# agentd accepts the value and says so when it does not.
assert 'strcmp(value, "ukmo")' in agentd, "agentd rejects the UKMO provider"
assert "weather_provider must be open-meteo, ukmo, met-no or off" in agentd, (
    "the rejection message does not name the accepted values")

# Only the UKMO provider asks for the Met Office models.
assert '!strcmp(state->config.weather_provider, "ukmo")' in agentd, (
    "the model parameter is not selected by provider")
assert '"&models=ukmo_seamless"' in agentd, "the UKMO model blend is not requested"
assert agentd.count("&models=") == 1, (
    "a model parameter is built in more than one place; only the UKMO branch may add one")

# Still keyless and still one transport: no credential, no second host.
assert "api.metoffice.gov.uk" not in agentd, "a keyed Met Office endpoint was introduced"
assert "datahub" not in agentd, "a keyed Met Office endpoint was introduced"
assert "https://api.open-meteo.com/v1/forecast" in agentd, (
    "the weather fetch no longer uses the keyless endpoint")

# Documented in the same change, per the repository API rules.
assert '`"ukmo"`' in api_doc, "docs/API.md does not document the new provider value"
assert "ukmo" in openapi, "web/openapi.json does not list the new provider value"

# The changed frontend asset is cache-busted for browsers already holding it.
match = re.search(r'/js/app\.js\?rev=(\d+)', index)
assert match and int(match.group(1)) >= 38, "app.js cache revision was not advanced"

print("Met Office (UKMO) weather provider contract: ok")
