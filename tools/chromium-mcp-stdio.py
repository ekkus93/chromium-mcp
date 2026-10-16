#!/usr/bin/env python3
"""Launch Chromium MCP and bridge MCP stdio requests to its local HTTP API."""

from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path


def log(message: str) -> None:
  print(f"chromium-mcp: {message}", file=sys.stderr, flush=True)


def runtime_paths() -> tuple[Path, Path]:
  profile = Path(
    os.environ.get(
      "CHROMIUM_MCP_PROFILE_DIR",
      Path.home() / ".local/share/chromium-mcp-profile",
    )
  )
  runtime = Path(
    os.environ.get(
      "CHROMIUM_MCP_RUNTIME_DIR",
      profile / "Default/BrowserAgentRuntime",
    )
  )
  return (
    runtime / "browser-agent-mcp-endpoint",
    runtime / "browser-agent-mcp-token",
  )


def load_runtime(
  endpoint_file: Path, token_file: Path
) -> tuple[str, str] | None:
  try:
    endpoint = endpoint_file.read_text(encoding="utf-8").strip()
    token = token_file.read_text(encoding="utf-8").strip()
  except OSError:
    return None
  if not endpoint.startswith("http://127.0.0.1:") or not endpoint.endswith(
    "/mcp"
  ):
    return None
  if not token:
    return None
  return endpoint, token


def post(
  endpoint: str, token: str, message: dict, timeout: float = 600
) -> tuple[int, bytes]:
  request_message = dict(message)
  method = request_message.get("method")
  params = request_message.get("params")
  if isinstance(params, dict):
    params = dict(params)
    metadata = params.get("_meta")
    if isinstance(metadata, dict):
      metadata = dict(metadata)
      # Chromium's HTTP endpoint uses these fields only with matching
      # HTTP headers. The stdio bridge intentionally selects its
      # standard MCP compatibility path instead.
      for key in (
        "io.modelcontextprotocol/protocolVersion",
        "io.modelcontextprotocol/clientCapabilities",
        "io.modelcontextprotocol/clientInfo",
      ):
        metadata.pop(key, None)
      if metadata:
        params["_meta"] = metadata
      else:
        params.pop("_meta", None)
    request_message["params"] = params
  elif method and method != "notifications/initialized":
    request_message["params"] = {}

  body = json.dumps(request_message, separators=(",", ":")).encode("utf-8")
  request = urllib.request.Request(
    endpoint,
    data=body,
    headers={
      "Authorization": f"Bearer {token}",
      "Content-Type": "application/json",
      "Accept": "application/json, text/event-stream",
    },
    method="POST",
  )
  try:
    with urllib.request.urlopen(request, timeout=timeout) as response:
      return response.status, response.read()
  except urllib.error.HTTPError as error:
    return error.code, error.read()


def initialize_probe(endpoint: str, token: str) -> bool:
  probe = {
    "jsonrpc": "2.0",
    "id": "chromium-mcp-startup-probe",
    "method": "initialize",
    "params": {
      "protocolVersion": "2025-06-18",
      "capabilities": {},
      "clientInfo": {"name": "chromium-mcp-stdio", "version": "1"},
    },
  }
  try:
    status, body = post(endpoint, token, probe, timeout=1)
    return status == 200 and bool(json.loads(body).get("result"))
  except (OSError, ValueError, json.JSONDecodeError):
    return False


def start_browser(
  appdir: Path, profile: Path, log_path: Path
) -> subprocess.Popen:
  app_run = appdir / "AppRun"
  if not app_run.is_file():
    raise RuntimeError(f"Chromium AppImage is not prepared at {appdir}")
  if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
    raise RuntimeError(
      "No graphical display is available. Start Codex or Claude Code "
      "from your Ubuntu desktop session."
    )
  log_path.parent.mkdir(parents=True, exist_ok=True)
  log_file = log_path.open("ab", buffering=0)
  command = [
    str(app_run),
    f"--user-data-dir={profile}",
    "--enable-features=BrowserAgentService",
    "--enable-browser-agent-service",
    "--browser-agent-mcp-port=0",
  ]
  log(f"starting Chromium from {appdir}")
  return subprocess.Popen(
    command,
    stdin=subprocess.DEVNULL,
    stdout=log_file,
    stderr=log_file,
    start_new_session=True,
    close_fds=True,
  )


def stop_browser(process: subprocess.Popen) -> None:
  if process.poll() is not None:
    return
  try:
    os.killpg(process.pid, signal.SIGTERM)
    process.wait(timeout=10)
  except (ProcessLookupError, subprocess.TimeoutExpired):
    try:
      os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
      pass


def emit_error(request_id: object, code: int, message: str) -> None:
  response = {
    "jsonrpc": "2.0",
    "id": request_id,
    "error": {"code": code, "message": message},
  }
  print(json.dumps(response, separators=(",", ":")), flush=True)


def main() -> int:
  def request_shutdown(_signum: int, _frame: object) -> None:
    raise SystemExit(0)

  signal.signal(signal.SIGTERM, request_shutdown)
  signal.signal(signal.SIGINT, request_shutdown)

  parser = argparse.ArgumentParser()
  parser.add_argument("--appdir", required=True, type=Path)
  args = parser.parse_args()

  endpoint_file, token_file = runtime_paths()
  profile = Path(
    os.environ.get(
      "CHROMIUM_MCP_PROFILE_DIR",
      Path.home() / ".local/share/chromium-mcp-profile",
    )
  )
  state_dir = Path(
    os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")
  )
  log_path = state_dir / "chromium-mcp/chromium-mcp.log"

  browser = start_browser(args.appdir.resolve(), profile, log_path)
  owns_browser = True
  deadline = time.monotonic() + 60
  runtime: tuple[str, str] | None = None
  try:
    while time.monotonic() < deadline:
      runtime = load_runtime(endpoint_file, token_file)
      if runtime and initialize_probe(*runtime):
        # Chromium exits quickly when the profile is already open in
        # another process. In that case the existing process owns it.
        owns_browser = browser.poll() is None
        break
      if browser.poll() is not None and not runtime:
        time.sleep(0.25)
      else:
        time.sleep(0.25)
    if runtime is None or not initialize_probe(*runtime):
      raise RuntimeError(
        f"Chromium MCP did not start. Check the browser log at {log_path}"
      )

    endpoint, token = runtime
    log("Chromium MCP is ready")
    for line in sys.stdin:
      try:
        message = json.loads(line)
      except json.JSONDecodeError as error:
        emit_error(None, -32700, f"Invalid JSON: {error.msg}")
        continue
      if not isinstance(message, dict):
        emit_error(None, -32600, "Expected a JSON-RPC object")
        continue
      method = message.get("method")
      request_id = message.get("id")
      if method == "ping" and "id" in message:
        print(
          json.dumps(
            {"jsonrpc": "2.0", "id": request_id, "result": {}},
            separators=(",", ":"),
          ),
          flush=True,
        )
        continue
      if method == "subscriptions/listen":
        if "id" in message:
          emit_error(
            request_id,
            -32601,
            "Browser event streaming is not supported over this stdio bridge.",
          )
        continue
      try:
        status, body = post(endpoint, token, message)
        if "id" not in message:
          continue
        if not body:
          emit_error(
            request_id,
            -32603,
            f"Chromium MCP returned an empty response (HTTP {status})",
          )
          continue
        response = json.loads(body)
        print(json.dumps(response, separators=(",", ":")), flush=True)
      except (OSError, ValueError, json.JSONDecodeError) as error:
        if "id" in message:
          emit_error(
            request_id, -32603, f"Chromium MCP request failed: {error}"
          )
  except (RuntimeError, OSError) as error:
    log(str(error))
    return 1
  finally:
    if owns_browser:
      stop_browser(browser)
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
