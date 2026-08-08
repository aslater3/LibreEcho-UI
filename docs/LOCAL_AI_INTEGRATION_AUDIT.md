# LibreEcho API Audit — Local LLM / STT / TTS Integration Gaps

**Target:** `http://192.168.0.125:8080` (LibreEcho-UI web daemon, `/api/v1/`)
**Source audited:** `/home/andy/workspace/LibreEcho-UI` (commit on `main`)
**Date:** 2026-07-27

---

## TL;DR

LibreEcho has a clean, well-factored voice pipeline, but **every local-AI
component is hardwired to a single fixed implementation with no remote
configuration surface**. To integrate a self-hosted stack (llama.cpp Gemma 4,
Wyoming Whisper, Piper) you need to add configuration for **five** things that
are currently compile-time or init-script constants:

| # | Capability | Current state | What's missing |
|---|-----------|---------------|----------------|
| 1 | **LLM provider** | Hardcoded to ChatGPT device-login OAuth | No custom/OpenAI-compatible endpoint, no base URL, no API-key auth, no provider selection |
| 2 | **STT engine** | Local sherpa-onnx, model path is a CLI flag | No API to select engine/model/endpoint; no remote (Wyoming) STT client |
| 3 | **TTS engine** | Local sherpa-onnx, 2 hardcoded voices | No API to select engine/model/endpoint; no remote (Wyoming) TTS client |
| 4 | **Pipeline routing** | Implicit: wake→local STT→agentd→local TTS | No mode switch (local vs Home Assistant vs custom); no way to point the pipeline at external services |
| 5 | **Wake word** | Configurable word + sensitivity only | Model selection is cosmetic ("Custom model" is a UI label with no backing) |

The good news: the **transport layer already supports arbitrary HTTP endpoints**
(`llm_http.c` builds a curl config from `request->url`), and there is already a
**provider abstraction** (`struct le_llm_provider`) and a **Wyoming daemon**
(`wyomingd.c`). The gaps are in *configuration* and *provider/engine selection*,
not in fundamental capability.

---

## Architecture as built

```
                ┌─────────────────────────────────────────────┐
                │  web daemon (api.c)  :8080  /api/v1/*        │
                │  - state-changing calls → adapter sockets    │
                └───────────────┬─────────────────────────────┘
                                │ unix sockets /run/libreecho/*.sock
   ┌────────┬────────┬──────────┼──────────┬─────────┬──────────┐
   │ micd   │ waked  │ sttd     │ agentd   │ ttsd    │ wyomingd │
   │ capture│ wake   │ sherpa   │ ChatGPT  │ sherpa  │ HA bridge│
   │ AEC    │ onnx   │ STT      │ OAuth    │ TTS     │ :10700   │
   └────────┴────────┴──────────┴──────────┴─────────┴──────────┘
        ▲         ▲         ▲          ▲          ▲
        └─────────┴── voice_pipeline.c ┴──────────┘
              wake event → stream STT → transcript → agentd
              agentd → LLM (ChatGPT) → streamed text → ttsd → speaker
```

Each daemon is a separate process talking over a unix socket. The web daemon
does **not** configure the AI daemons' engines — it only toggles the assistant
on/off and edits the prompt/model strings.

---

## Detailed findings

### 1. LLM provider — the biggest gap

**Files:** `src/adapter/llm_provider.{h,c}`, `llm_codex.c`, `llm_http.{h,c}`,
`llm_store.{h,c}`, `agentd.c`

The provider abstraction is real but has exactly **one** implementation:

```c
// llm_provider.c
const struct le_llm_provider *le_llm_provider_by_id(const char *id) {
    provider = le_llm_codex_provider();
    return provider && !strcmp(provider->id, id) ? provider : NULL;  // only "openai-codex"
}
```

`agentd.c` hardcodes the provider and rejects anything else:

```c
// agentd.c:837
state.provider = le_llm_provider_by_id("openai-codex");   // hardcoded
// agentd.c:718  (command_configure)
if (parsed > 0 && strcmp(value, "openai-codex"))
    return respond(fd, id, 0, "unsupported provider");      // rejects all others
```

The ChatGPT provider hardcodes its endpoints:

```c
// llm_codex.c
#define CODEX_AUTH_BASE     "https://auth.openai.com"
#define CODEX_RESPONSES_URL "https://chatgpt.com/backend-api/codex/responses"
```

And the whole auth model is **subscription device-login OAuth** — there is no
API-key path. `llm_store.c` only stores `access_token` / `refresh_token` /
`account_id` from an OAuth exchange. The API doc is explicit:

> "It never accepts or falls back to an OpenAI API key."

**What this means for a local LLM:** You cannot point LibreEcho at
`http://192.168.0.x:8000/v1` (your Gemma 4 server) through any supported path.
There is:
- ❌ no `base_url` / endpoint field in `agent_config`
- ❌ no API-key credential type
- ❌ no `openai-compatible` provider
- ❌ no provider selection (the field exists in the API/UI but is locked to one value)

**The silver lining:** `llm_http.c` already builds a generic curl request from
`request->url`, `content_type`, `authorization`, and `accept_sse`. A new
`le_llm_openai_compatible_provider()` that fills `request->url` from config and
parses standard `chat/completions` SSE deltas would slot into the existing
`stream_event` machinery with no transport changes.

**The UI already anticipates this** — `app.js` renders a "Local LLM" card:

> "A local language model can run without a subscription or metered API billing.
> Provider setup will appear here when a reviewed model is installed."

So the product intent is there; the backend just hasn't caught up.

---

### 2. STT engine — no remote/configurable path

**Files:** `src/adapter/sttd.c`, `stt_engine.h`, `stt_engine_sherpa.cpp`,
`voice_pipeline.c`, `init/libreecho-sttd.init`

STT is a local sherpa-onnx daemon. Its model path is a **CLI flag**, not API
config:

```c
// sttd.c
#define DEFAULT_STT_MODEL "/usr/local/share/libreecho/stt"
// ...
else if (!strcmp(argv[i], "--model-dir") && i + 1 < argc)
    model_dir = argv[++i];
```

The init script bakes the model into a read-only squashfs payload:

```sh
# init/libreecho-sttd.init
PAYLOAD=${PAYLOAD:-/data/libreecho/features/stt/payload.squashfs}
MODEL_ROOT=${MODEL_ROOT:-$RUNTIME_ROOT/usr/local/share/libreecho/stt}
```

`voice_pipeline.c` connects to STT over a **fixed unix socket**
(`/run/libreecho/stt.sock`) and expects a specific JSON event stream
(`"event":"transcript"`). There is:
- ❌ no API endpoint to choose STT engine or model
- ❌ no way to point STT at a remote Wyoming server (your Whisper on :10300)
- ❌ no engine abstraction at the pipeline level (it assumes the local socket protocol)

The `stt_engine.h` interface *is* abstract (`stt_engine_init(model_dir, threads)`),
so a `stt_engine_wyoming.c` that speaks Wyoming to an external server is feasible,
but nothing wires it to configuration.

---

### 3. TTS engine — two hardcoded voices, no remote path

**Files:** `src/adapter/ttsd.c`, `tts_engine.h`, `tts_engine_sherpa.cpp`,
`init/libreecho-ttsd.init`

TTS is local sherpa-onnx with a **fixed voice whitelist**:

```sh
# init/libreecho-ttsd.init
valid_voice() {
    case "$1" in
        northern-male|southern-female) return 0 ;;   # only two voices, ever
        *) return 1 ;;
    esac
}
```

The API exposes `tts_voice` (GET/PUT `/api/v1/audio`) but only those two values
are valid. There is:
- ❌ no API to select TTS engine or model
- ❌ no way to point TTS at a remote Wyoming Piper (your Piper on :10200)
- ❌ no voice catalog beyond the two compiled-in names

The `tts_engine.h` interface is abstract (`tts_engine_init(model_dir, voice)`),
so a `tts_engine_wyoming.c` is feasible but not wired to config.

---

### 4. Pipeline routing — no mode switch

**Files:** `src/adapter/agentd.c`, `voice_pipeline.c`, `wyomingd.c`,
`docs/HOME_ASSISTANT_VOICE.md`

There are effectively **three** possible pipelines, but no API to choose between
them:

1. **Local ChatGPT mode** (current default): wake → local STT → agentd → ChatGPT → local TTS
2. **Home Assistant satellite mode**: wake → Wyoming bridge (:10700) → HA does STT/LLM/TTS
3. **Custom local mode** (what you want): wake → external Whisper → external Gemma → external Piper

The Home Assistant integration is a **bit flag** (`integrations & 1u`) toggled via
`PUT /api/v1/integrations/home-assistant`, and the Wyoming daemon runs on a fixed
port. But:
- ❌ the Wyoming port is an init-script constant (`PORT=${PORT:-10700}`), not API-configurable
- ❌ there's no "use external STT/LLM/TTS endpoints" mode
- ❌ `agentd` and `wyomingd` are mutually exclusive by design but the choice isn't exposed as a clean pipeline-mode setting

The doc confirms the intent that modes are mutually exclusive but selected
outside the API:

> "Selecting Home Assistant mode immediately stops local STT, local assistant
> dispatch, and local TTS, and starts the Wyoming bridge."

---

### 5. Wake word — model selection is cosmetic

**Files:** `src/api.c` (`wake_json`/PUT handler), `web/js/app.js` (`wakePage`)

The API supports `wake_word` (string) and `sensitivity` (int). The UI offers a
dropdown including "Custom model", but the backend just stores the string — there
is no model loading behind it:

```c
// api.c PUT /api/v1/wake-word
if (json_get_string(q->body,"wake_word",s,sizeof(s))>0) rc=le_set_wake_word(c->backend,s);
if (!rc && json_get_int(q->body,"sensitivity",&v)>0) rc=le_set_wake_word_sensitivity(c->backend,v);
```

- ❌ no wake-word model upload/select
- ❌ "Custom model" in the UI has no backing implementation

(Lower priority than 1–4, since wake word works fine as-is.)

---

## What already exists to build on

| Asset | Location | Reuse for |
|-------|----------|-----------|
| Generic HTTP transport (curl config builder, SSE) | `llm_http.c` | OpenAI-compatible LLM provider |
| Provider abstraction + registry | `llm_provider.{h,c}` | Add `openai-compatible` provider |
| Streaming reply segmentation | `voice_reply.c`, `voice_playback.c` | Already works for any streamed text |
| Wyoming protocol implementation | `wyomingd.c`, `wyoming_protocol.{h,c}` | Client mode for external Whisper/Piper |
| Abstract STT/TTS engine interfaces | `stt_engine.h`, `tts_engine.h` | Add `_wyoming` engine backends |
| Atomic config persistence | `config_store.c`, `config_manager.c` | Store new endpoint settings |
| UI "Local LLM" placeholder card | `web/js/app.js` | Wire to real provider config |

---

## Recommended API additions

A minimal, backwards-compatible surface to enable a fully self-hosted pipeline:

### A. Extend `PUT /api/v1/assistant` (provider-neutral LLM config)

```json
{
  "enabled": true,
  "provider": "openai-compatible",          // NEW: was locked to "openai-codex"
  "model": "Gemma-4-12B",
  "prompt": "...",
  "llm": {                                   // NEW block
    "base_url": "http://192.168.0.10:8000/v1",
    "api_key": "***",                   // optional, stored 0600 like OAuth creds
    "auth_mode": "api-key" | "none" | "oauth"
  }
}
```

Backend: add `le_llm_openai_compatible_provider()`; relax the `strcmp(value,
"openai-codex")` guard in `command_configure`; store `base_url`/`api_key` in the
credentials store (mode 0600, excluded from exports — the pattern already exists).

### B. New `GET/PUT /api/v1/voice-pipeline` (engine + endpoint selection)

```json
{
  "mode": "local" | "home-assistant" | "custom",
  "stt": {
    "engine": "sherpa" | "wyoming",
    "wyoming_uri": "tcp://192.168.0.10:10300",
    "model": "whisper-small"
  },
  "tts": {
    "engine": "sherpa" | "wyoming",
    "wyoming_uri": "tcp://192.168.0.10:10200",
    "voice": "en_GB-alba-medium"
  },
  "wake_word": {
    "model": "alexa" | "custom",
    "sensitivity": 68
  }
}
```

This is the missing "pipeline routing" control. In `custom` mode the daemons
would use Wyoming clients to reach your external Whisper/Piper, and `agentd`
would use the OpenAI-compatible provider for the LLM.

### C. Make Wyoming port configurable

Promote `PORT` from `init/libreecho-wyomingd.init` into the config store and
expose it under the Home Assistant integration settings.

---

## Suggested implementation order

1. **OpenAI-compatible LLM provider** (highest value, smallest blast radius)
   — reuses `llm_http.c`, unblocks your Gemma 4 server immediately.
2. **Pipeline-mode config + `base_url` plumbing** — the routing glue.
3. **Wyoming STT/TTS client engines** — reuse `wyoming_protocol.c` in client mode.
4. **UI wiring** — the "Local LLM" card and a new pipeline settings panel.
5. **Wake-word model selection** — lowest priority, cosmetic today.

---

## Notes / caveats

- The device is **32-bit ARM, ~491 MB RAM** (per `HOME_ASSISTANT_VOICE.md`).
  Running Whisper/Piper *on the device* is impractical — the design already
  assumes they run elsewhere (HA or a server). Your Arc-equipped host is the
  right place for them; the device should be a *client*. This reinforces that
  the missing piece is **remote-endpoint configuration**, not on-device models.
- All AI features ship as **squashfs feature payloads** mounted read-only at
  runtime. New engine backends mean new payloads, but config (endpoints, keys)
  lives in `/data/libreecho/config/` which is writable — so configuration alone
  doesn't require a new image.
- Security posture is deliberately strict (no API keys by design, 0600 secrets,
  CSRF on all mutations, secrets excluded from exports/diagnostics). Any
  `api_key`/`base_url` addition should follow the existing `llm_store.c` 0600
  pattern and the export-redaction rules.
