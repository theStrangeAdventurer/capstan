#!/usr/bin/env python3
"""Capture and diff the raw API request bodies of Capstan and OpenCode.

Purpose: verify whether Capstan and OpenCode send the same request to a
provider for a given model + reasoning setting. Used to investigate a
~6.8x streaming-speed gap on DeepSeek V4 Pro via OpenRouter.

The script runs a tiny local forwarding proxy, points each agent at it, runs
one short prompt, then prints a structural diff of the request JSON bodies.

Usage:
    OPENROUTER_API_KEY=... python3 benchmarks/polyglot/scripts/compare_requests.py

Outputs:
    - /tmp/capstan-vs-opencode-requests.jsonl   (raw captured request bodies)
    - a structural summary on stdout (field names, counts, key values)

The proxy forwards to https://openrouter.ai/api/v1 unchanged, so this also
works as a generic request inspector for any OpenAI-compatible endpoint.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import http.server
from pathlib import Path

PORT = 19121
UPSTREAM = os.environ.get("OPENROUTER_BASE_URL", "https://openrouter.ai/api/v1")
CAPTURE = "/tmp/capstan-vs-opencode-requests.jsonl"
MODEL = "deepseek/deepseek-v4-pro"
PROMPT = 'Say exactly "hello" and nothing else.'


class Proxy(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _forward(self, method: str) -> None:
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length else b""

        req = urllib.request.Request(
            UPSTREAM + self.path,
            data=body,
            method=method,
            headers={
                k: v
                for k, v in self.headers.items()
                if k.lower() not in ("host", "content-length", "accept-encoding")
            },
        )

        with open(CAPTURE, "a", encoding="utf-8") as f:
            f.write(
                json.dumps(
                    {
                        "agent": os.environ.get("PROXY_AGENT", "unknown"),
                        "path": self.path,
                        "request_body": body.decode("utf-8", "replace"),
                    },
                    ensure_ascii=False,
                )
                + "\n"
            )

        try:
            with urllib.request.urlopen(req) as up:
                self.send_response(up.status)
                for k, v in up.headers.items():
                    if k.lower() in ("transfer-encoding", "connection", "content-length"):
                        continue
                    self.send_header(k, v)
                self.end_headers()
                while chunk := up.read(16384):
                    self.wfile.write(chunk)
                    self.wfile.flush()
        except urllib.error.HTTPError as e:
            self.send_response(e.code)
            self.end_headers()
            self.wfile.write(e.read())

    def do_POST(self) -> None:
        self._forward("POST")

    def log_message(self, *args) -> None:
        pass


def run_proxy() -> threading.Thread:
    Path(CAPTURE).unlink(missing_ok=True)
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Proxy)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    return thread


def run_capstan(repo_root: str) -> None:
    prompt = Path("/tmp/capstan-req-prompt.txt")
    prompt.write_text(PROMPT, encoding="utf-8")
    # The built-in openrouter provider has no endpoint override in isolated
    # benchmark mode, so this runs against the live upstream and is captured
    # from the runtime log only when CAPSTAN_LOG_LEVEL=info is set. For a true
    # byte capture, temporarily add an endpoint override or run non-isolated.
    subprocess.run(
        [
            f"{repo_root}/build/capstan",
            "run", "--benchmark", "--no-wiki",
            "--provider", "openrouter", "--model", MODEL,
            "--profile", "implement", "--reasoning-effort", "medium",
            "--max-turns", "1",
            "--prompt-file", str(prompt),
            "--workdir", "/tmp", "--workspace", "/tmp",
            "--json",
        ],
        capture_output=True, text=True, timeout=180,
    )


def run_opencode() -> None:
    # baseURL override routes OpenCode through the local proxy.
    config = json.dumps({
        "provider": {
            "openrouter": {
                "options": {"baseURL": f"http://127.0.0.1:{PORT}/v1"},
                "models": {
                    MODEL: {
                        "variants": {"medium": {"reasoning": {"effort": "medium"}}},
                    },
                },
            },
        },
    }, separators=(",", ":"))
    env = os.environ.copy()
    env.update({
        "OPENCODE_CONFIG_DIR": "/tmp/capstan-req-opencode-cfg",
        "OPENCODE_CONFIG_CONTENT": config,
        "OPENCODE_DISABLE_PROJECT_CONFIG": "1",
        "OPENCODE_DISABLE_EXTERNAL_SKILLS": "1",
        "OPENCODE_DISABLE_CLAUDE_CODE": "1",
        "PROXY_AGENT": "opencode",
    })
    subprocess.run(
        [
            "opencode", "run", "--pure",
            "--model", f"openrouter/{MODEL}",
            "--agent", "build", "--format", "json",
            "--dir", "/tmp", "--dangerously-skip-permissions",
            "--variant", "medium",
        ],
        input=PROMPT, capture_output=True, text=True, timeout=180, env=env,
    )


def summarize() -> None:
    if not Path(CAPTURE).exists():
        print("No captures. The Capstan leg currently needs a non-isolated run or endpoint override.")
        return
    for line in Path(CAPTURE).read_text(encoding="utf-8").splitlines():
        d = json.loads(line)
        body = json.loads(d["request_body"])
        if not body.get("model", "").startswith("deepseek"):
            continue
        print(f"\n=== {d['agent']} request ===")
        print("top-level keys:", sorted(body.keys()))
        for k in ("reasoning", "usage", "stream_options", "max_tokens",
                  "temperature", "tool_choice"):
            if k in body:
                print(f"  {k}: {json.dumps(body[k], ensure_ascii=False)[:120]}")
        tools = body.get("tools") or []
        print(f"  tools: {len(tools)} -> {[t.get('function', {}).get('name') for t in tools]}")
        for m in body.get("messages", []):
            if m.get("role") == "system":
                c = m.get("content")
                n = sum(len(p.get("text", "")) for p in c) if isinstance(c, list) else len(c or "")
                print(f"  system prompt: {n} chars")


def main() -> int:
    repo_root = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
    run_proxy()
    time.sleep(0.3)
    print("capturing OpenCode...", flush=True)
    run_opencode()
    time.sleep(0.3)
    print("capturing Capstan (see note in source about endpoint)...", flush=True)
    run_capstan(repo_root)
    time.sleep(0.3)
    summarize()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
