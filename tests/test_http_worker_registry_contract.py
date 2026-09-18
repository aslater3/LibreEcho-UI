#!/usr/bin/env python3
import re
from pathlib import Path

source = (Path(__file__).resolve().parents[1] / "src" / "http_server.c").read_text()

for name, kind in (
    ("start_pcm_stream", "CHILD_WORKER_PCM_STREAM"),
    ("start_update_upload", "CHILD_WORKER_UPDATE"),
    ("start_update_fetch", "CHILD_WORKER_UPDATE"),
):
    match = re.search(
        rf"static int {name}\([^{{]+\)\{{(.*?)\nstatic ",
        source,
        re.DOTALL,
    )
    assert match, f"could not isolate {name}"
    body = match.group(1)
    assert f"child_worker_begin({kind}" in body, f"{name} does not reserve a slot"
    assert f"child_worker_register(slot,pid,{kind})" in body, f"{name} does not publish its PID"
    assert "sigprocmask(SIG_SETMASK,&previous,NULL)" in body, f"{name} does not restore SIGCHLD"

reaper = re.search(r"static void reap_child_workers\([^)]*\)\{(.*?)\n#ifdef ", source, re.DOTALL)
assert reaper and "waitpid(-1" not in reaper.group(1)
assert "LE_MAX_CHILD_WORKERS" in source
print("http outer-worker registration contract: all missing paths bounded and tracked: ok")
